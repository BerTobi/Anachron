/* infer_remote — the LAN/GPU-offload backend (the brief's "offload inference to a
 * LAN server"). POSTs the prompt + GBNF grammar to a llama.cpp `llama-server`
 * /completion endpoint and streams back the generated text. The big model (e.g. a
 * 7B+ on a desktop GPU) runs on the server; this side is a thin client, so a weak
 * local CPU is no longer the ceiling — including the Pentium-M: the transport is
 * plain HTTP over the LAN, which real XP handles fine (no TLS involved).
 *
 * Server: `llama-server -m big-model.gguf --host 0.0.0.0 --port 8080` on the GPU
 * box; point ANACHRON at it with  --model http://gpu-box:8080  (a full path in the
 * URL overrides the default /completion endpoint).
 *
 * Auth: if ANACHRON_REMOTE_KEY is set, "Authorization: Bearer <key>" is sent,
 * matching `llama-server --api-key`. Plain HTTP: use a trusted LAN / tunnel.
 *
 * Non-streaming (one request -> one JSON response): simple and robust; the whole
 * reply is delivered to on_token at once. Grammar still applies — llama-server
 * accepts GBNF — so a small remote model stays as tool-reliable as a local one. */
#include "infer_backend.h"
#include "platform.h"
#include "strbuf.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct remote_impl {
    char *url;      /* full endpoint URL (…/completion by default) */
    char *api_key;  /* optional Bearer token; NULL = no auth header */
    int   n_ctx;
    int   last_prompt_tokens;      /* from the server's tokens_evaluated */
    int   last_completion_tokens;  /* from the server's tokens_predicted */
};

static int remote_generate(void *impl, const char *prompt, const char *grammar,
                           void (*on_token)(const char *piece, void *ud), void *ud) {
    struct remote_impl *c = impl;
    c->last_prompt_tokens = 0;
    c->last_completion_tokens = 0;

    /* Request body for llama.cpp /completion (non-streaming). cache_prompt keeps
     * the server's KV warm across turns — the same speedup as the local backend. */
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

    strbuf hdrs; sb_init(&hdrs);
    if (c->api_key) sb_appendf(&hdrs, "Authorization: Bearer %s\r\n", c->api_key);

    char err[160];
    char *resp = NULL; size_t rlen = 0; int status = 0;
    int hr = plat_http_post(c->url, hdrs.len ? sb_cstr(&hdrs) : NULL,
                            sb_cstr(&body), body.len,
                            &resp, &rlen, &status, err, sizeof err);
    sb_free(&hdrs);
    sb_free(&body);
    if (hr != 0) {
        fprintf(stderr, "infer_remote: %s\n", err);
        return -1;
    }
    if (status != 200) {
        fprintf(stderr, "infer_remote: server answered HTTP %d%s%.200s\n",
                status, rlen ? ": " : "", rlen ? resp : "");
        free(resp);
        return -1;
    }

    const char *jerr = NULL;
    json_value *jv = json_parse(resp, &jerr);
    int rc = 0;
    if (jv) {
        const char *content = json_as_str(json_obj_get(jv, "content"));
        if (content && on_token) on_token(content, ud);
        else if (!content) {
            fprintf(stderr, "infer_remote: response had no \"content\"\n");
            rc = -1;
        }
        const json_value *pe = json_obj_get(jv, "tokens_evaluated");
        const json_value *pp = json_obj_get(jv, "tokens_predicted");
        c->last_prompt_tokens = (pe && pe->type == JSON_NUMBER) ? (int)pe->num : 0;
        c->last_completion_tokens = (pp && pp->type == JSON_NUMBER) ? (int)pp->num : 0;
        json_free(jv);
    } else {
        fprintf(stderr, "infer_remote: could not parse response JSON (%s)\n",
                jerr ? jerr : "?");
        rc = -1;
    }
    free(resp);
    return rc;
}

static void remote_last_usage(const void *impl, int *prompt_tokens, int *completion_tokens) {
    const struct remote_impl *c = impl;
    if (prompt_tokens) *prompt_tokens = c ? c->last_prompt_tokens : 0;
    if (completion_tokens) *completion_tokens = c ? c->last_completion_tokens : 0;
}

static void remote_free(void *impl) {
    struct remote_impl *c = impl;
    if (!c) return;
    free(c->url);
    free(c->api_key);
    free(c);
}

int remote_backend_open(const char *spec, int n_ctx, infer_backend *out) {
    struct remote_impl *c = xmalloc(sizeof *c);
    c->n_ctx = n_ctx > 0 ? n_ctx : 2048;
    c->last_prompt_tokens = 0;
    c->last_completion_tokens = 0;

    /* "http://host:port" gets the default /completion endpoint appended; a spec
     * that already carries a path (beyond a bare trailing "/") is used as-is. */
    size_t len = strlen(spec);
    while (len > 0 && spec[len - 1] == '/') len--;   /* drop trailing slashes */
    const char *scheme_end = strstr(spec, "://");
    const char *slash = scheme_end ? strchr(scheme_end + 3, '/') : NULL;
    strbuf u; sb_init(&u);
    sb_append_n(&u, spec, len);
    if (!slash || (size_t)(slash - spec) >= len) sb_append(&u, "/completion");
    c->url = xstrdup(sb_cstr(&u));
    sb_free(&u);

    /* Optional Bearer token; CR/LF stripped so it can't inject headers. */
    const char *key = getenv("ANACHRON_REMOTE_KEY");
    c->api_key = NULL;
    if (key && *key) {
        strbuf k; sb_init(&k);
        for (const char *p = key; *p; p++)
            if (*p != '\r' && *p != '\n') sb_putc(&k, *p);
        c->api_key = xstrdup(sb_cstr(&k));
        sb_free(&k);
    }
    fprintf(stderr, "infer_remote: target %s%s\n",
            c->url, c->api_key ? " (api key set)" : "");

    out->impl = c;
    out->generate = remote_generate;
    out->set_chat = NULL;
    out->last_usage = remote_last_usage;
    out->free_impl = remote_free;
    return 0;
}
