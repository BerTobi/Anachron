/* infer_remote — the LAN/GPU-offload backend (the brief's "offload inference to a
 * LAN server"). A minimal raw-socket HTTP/1.1 client that POSTs the prompt + GBNF
 * grammar to a llama.cpp `server` /completion endpoint and returns the generated
 * text. The big model (e.g. a 7B+ on a desktop GPU) runs on the server; this
 * binary is a thin client, so a weak local CPU is no longer the ceiling.
 *
 * Server: run `llama-server -m big-model.gguf --host 0.0.0.0 --port 8080` on the
 * GPU box. Point the client at it with  ANACHRON_REMOTE=host:port  (default
 * 127.0.0.1:8080). The --model argument is ignored in this backend.
 *
 * Auth: if  ANACHRON_REMOTE_KEY  is set, an "Authorization: Bearer <key>" header is
 * sent, matching `llama-server --api-key <key>`. Plain HTTP, so use it over a VPN /
 * SSH tunnel (e.g. Tailscale) rather than a raw public port.
 *
 * v1 is NON-STREAMING (one request -> one JSON response): simpler and robust; the
 * whole reply is delivered to on_token at once. POSIX sockets only for now (the
 * dev host / antiX as client). An XP/Winsock client is a later step (the brief
 * defers the network path out of the on-metal iteration).
 *
 * /completion takes `prompt` and `grammar` directly, matching infer_generate. */
#ifndef _WIN32

#define _POSIX_C_SOURCE 200809L

#include "infer.h"
#include "strbuf.h"
#include "json.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct infer_ctx {
    char *host;
    char *port;
    char *api_key;  /* optional Bearer token (ANACHRON_REMOTE_KEY); NULL = no auth header */
    int   n_ctx;
    int   last_prompt_tokens;      /* from the server's tokens_evaluated */
    int   last_completion_tokens;  /* from the server's tokens_predicted */
};

infer_ctx *infer_init(const char *gguf_path, int n_ctx) {
    (void)gguf_path; /* the model lives on the server */
    struct infer_ctx *c = xmalloc(sizeof *c);
    c->n_ctx = n_ctx > 0 ? n_ctx : 2048;
    c->last_prompt_tokens = 0;
    c->last_completion_tokens = 0;

    const char *env = getenv("ANACHRON_REMOTE");
    const char *hostport = (env && *env) ? env : "127.0.0.1:8080";
    const char *colon = strrchr(hostport, ':');
    if (colon) {
        c->host = xstrndup(hostport, (size_t)(colon - hostport));
        c->port = xstrdup(colon + 1);
    } else {
        c->host = xstrdup(hostport);
        c->port = xstrdup("8080");
    }
    /* Optional Bearer token for an api-key-protected llama-server. Sanitize out any
     * CR/LF so the value can't inject extra request headers. */
    const char *key = getenv("ANACHRON_REMOTE_KEY");
    c->api_key = NULL;
    if (key && *key) {
        strbuf k; sb_init(&k);
        for (const char *p = key; *p; p++) if (*p != '\r' && *p != '\n') sb_putc(&k, *p);
        c->api_key = xstrdup(sb_cstr(&k));
        sb_free(&k);
    }
    fprintf(stderr, "infer_remote: target http://%s:%s/completion%s\n",
            c->host, c->port, c->api_key ? " (api key set)" : "");
    return c;
}

/* Connect a TCP socket to host:port. Returns fd or -1. */
static int dial(const char *host, const char *port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int send_all(int fd, const char *buf, size_t len) {
    while (len) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* De-chunk an HTTP/1.1 chunked body in place into `out`. */
static void dechunk(const char *body, size_t len, strbuf *out) {
    size_t i = 0;
    while (i < len) {
        /* parse hex chunk size up to CRLF */
        size_t sz = 0;
        int any = 0;
        while (i < len && body[i] != '\r' && body[i] != '\n') {
            char ch = body[i++];
            int d;
            if (ch >= '0' && ch <= '9') d = ch - '0';
            else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
            else break; /* ignore chunk extensions */
            /* A chunk can't exceed the body we hold; cap before the multiply so a
             * garbled/huge hex size line can't wrap sz around (the clamp below is a
             * second line of defense). */
            if (sz > len) { sz = len; }
            else sz = sz * 16 + (size_t)d;
            any = 1;
        }
        while (i < len && body[i] != '\n') i++; /* to end of size line */
        if (i < len) i++;                        /* past '\n' */
        if (!any || sz == 0) break;
        if (sz > len - i) sz = len - i; /* NB: `len - i` (i<=len held); `i + sz` could overflow */
        sb_append_n(out, body + i, sz);
        i += sz;
        while (i < len && (body[i] == '\r' || body[i] == '\n')) i++; /* trailing CRLF */
    }
}

int infer_generate(infer_ctx *c, const char *prompt, const char *grammar,
                   void (*on_token)(const char *piece, void *ud), void *ud) {
    /* Reset usage so an early failure never reports a prior call's counts. */
    c->last_prompt_tokens = 0;
    c->last_completion_tokens = 0;

    /* Build the JSON request body for llama.cpp /completion (non-streaming). */
    strbuf body; sb_init(&body);
    sb_append(&body, "{\"prompt\":\"");
    json_escape(&body, prompt);
    sb_appendf(&body, "\",\"stream\":false,\"cache_prompt\":true,\"temperature\":0,"
                      "\"n_predict\":%d", c->n_ctx);
    if (grammar && *grammar) {
        sb_append(&body, ",\"grammar\":\"");
        json_escape(&body, grammar);
        sb_append(&body, "\"");
    }
    sb_append(&body, "}");

    int fd = dial(c->host, c->port);
    if (fd < 0) {
        fprintf(stderr, "infer_remote: cannot connect to %s:%s\n", c->host, c->port);
        sb_free(&body);
        return -1;
    }

    strbuf req; sb_init(&req);
    sb_appendf(&req, "POST /completion HTTP/1.1\r\nHost: %s:%s\r\n"
                     "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n",
               c->host, c->port, body.len);
    if (c->api_key) sb_appendf(&req, "Authorization: Bearer %s\r\n", c->api_key);
    sb_append(&req, "\r\n");
    sb_append_n(&req, sb_cstr(&body), body.len);
    int srerr = send_all(fd, sb_cstr(&req), req.len);
    sb_free(&req);
    sb_free(&body);
    if (srerr) { close(fd); fprintf(stderr, "infer_remote: send failed\n"); return -1; }

    /* Read the whole response until the server closes (Connection: close). */
    strbuf resp; sb_init(&resp);
    char chunk[4096];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n > 0) { sb_append_n(&resp, chunk, (size_t)n); continue; }
        if (n == 0) break;                 /* clean EOF */
        if (errno == EINTR) continue;      /* interrupted - retry */
        fprintf(stderr, "infer_remote: read error mid-response\n");
        break;
    }
    close(fd);

    /* Split headers / body at the blank line. */
    const char *raw = sb_cstr(&resp);
    const char *sep = strstr(raw, "\r\n\r\n");
    if (!sep) { fprintf(stderr, "infer_remote: malformed HTTP response\n"); sb_free(&resp); return -1; }
    size_t hdr_len = (size_t)(sep - raw);
    const char *body_start = sep + 4;
    size_t body_len = resp.len - hdr_len - 4;

    /* De-chunk if needed: case-insensitive search for "chunked" in the headers. */
    strbuf dec; sb_init(&dec);
    int chunked;
    {
        char *hdrs = xstrndup(raw, hdr_len);
        for (char *p = hdrs; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
        chunked = strstr(hdrs, "transfer-encoding: chunked") != NULL;
        free(hdrs);
    }
    if (chunked) dechunk(body_start, body_len, &dec);
    else         sb_append_n(&dec, body_start, body_len);

    /* Parse the JSON response and hand "content" to the agent. */
    const char *err = NULL;
    json_value *jv = json_parse(sb_cstr(&dec), &err);
    int rc = 0;
    if (jv) {
        const char *content = json_as_str(json_obj_get(jv, "content"));
        if (content && on_token) on_token(content, ud);
        else if (!content) { fprintf(stderr, "infer_remote: response had no \"content\"\n"); rc = -1; }
        /* llama-server reports token counts; record them for the usage display. */
        const json_value *pe = json_obj_get(jv, "tokens_evaluated");
        const json_value *pp = json_obj_get(jv, "tokens_predicted");
        c->last_prompt_tokens = (pe && pe->type == JSON_NUMBER) ? (int)pe->num : 0;
        c->last_completion_tokens = (pp && pp->type == JSON_NUMBER) ? (int)pp->num : 0;
        json_free(jv);
    } else {
        fprintf(stderr, "infer_remote: could not parse response JSON (%s)\n", err ? err : "?");
        rc = -1;
    }
    sb_free(&dec);
    sb_free(&resp);
    return rc;
}

void infer_last_usage(const infer_ctx *c, int *prompt_tokens, int *completion_tokens) {
    if (prompt_tokens) *prompt_tokens = c ? c->last_prompt_tokens : 0;
    if (completion_tokens) *completion_tokens = c ? c->last_completion_tokens : 0;
}

void infer_free(infer_ctx *c) {
    if (!c) return;
    free(c->host);
    free(c->port);
    free(c->api_key);
    free(c);
}

#endif /* !_WIN32 */
