/* infer_api — hosted chat-API backends: the Anthropic Messages API and any
 * OpenAI-compatible /v1/chat/completions endpoint (llama-server, LM Studio,
 * Ollama, api.openai.com). Selected by the model spec:
 *
 *   anthropic:claude-opus-4-8    -> POST {base}/v1/messages
 *   openai:gpt-...               -> POST {base}/v1/chat/completions
 *
 * Config: ANACHRON_API_KEY (or agent.json "api_key") for the key;
 * ANACHRON_API_URL (or "api_url") overrides the base URL — point it at a LAN
 * server for openai:<model> without any key. Transport is plat_http_post, so TLS
 * reach follows the platform (curl on POSIX, WinINet on Windows).
 *
 * These are CHAT APIs: they take structured messages, not our rendered ChatML
 * string. The agent hands us the live history + system text via set_chat before
 * each generate; we build the JSON from that. The tool protocol is unchanged —
 * the system prompt teaches <tool_call>{...}</tool_call>, which frontier models
 * follow directly, so the whole agent loop, gate, and transcript work as-is.
 * GBNF grammar does not apply here and is ignored.
 *
 * Anthropic wire notes (api version 2023-06-01): x-api-key + anthropic-version
 * headers; max_tokens required; current models REJECT sampling params, so none
 * are sent. OpenAI-compatible servers accept temperature; 0 keeps local models
 * deterministic. */
#include "infer_backend.h"
#include "platform.h"
#include "prompt.h"
#include "strbuf.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum api_kind { API_ANTHROPIC, API_OPENAI };

struct api_impl {
    enum api_kind kind;
    char *model;       /* the part after the prefix */
    char *url;         /* full endpoint URL */
    char *api_key;     /* NULL = no auth header (LAN servers) */
    int   max_tokens;  /* per-response output cap */
    const history *h;             /* set_chat: valid through the next generate */
    const char    *system_text;
    int last_prompt_tokens;
    int last_completion_tokens;
};

static void api_set_chat(void *impl, const struct history *h, const char *system_text) {
    struct api_impl *c = impl;
    c->h = h;
    c->system_text = system_text;
}

/* Append one {"role":R,"content":C} object (C JSON-escaped). */
static void put_msg(strbuf *b, int *first, const char *role, const char *content,
                    const char *wrap_open, const char *wrap_close) {
    if (!*first) sb_append(b, ",");
    *first = 0;
    sb_appendf(b, "{\"role\":\"%s\",\"content\":\"", role);
    if (wrap_open) json_escape(b, wrap_open);
    json_escape(b, content ? content : "");
    if (wrap_close) json_escape(b, wrap_close);
    sb_append(b, "\"}");
}

/* History -> messages array. Tool results become user turns wrapped in
 * <tool_response> tags, mirroring the ChatML rendering the local models see. */
static void put_history(strbuf *b, const history *h) {
    int first = 1;
    for (size_t i = 0; i < h->count; i++) {
        const message *m = &h->items[i];
        switch (m->role) {
            case MSG_USER:
                put_msg(b, &first, "user", m->text, NULL, NULL);
                break;
            case MSG_ASSISTANT:
                put_msg(b, &first, "assistant", m->text, NULL, NULL);
                break;
            case MSG_TOOL_RESULT:
                put_msg(b, &first, "user", m->text,
                        "<tool_response>\n", "\n</tool_response>");
                break;
        }
    }
}

/* Surface an API error body readably: {"error":{"message":...}} on both APIs;
 * Google's compat layer wraps the same shape in a one-element array. */
static void print_api_error(const char *who, int status, const char *resp) {
    const char *msg = NULL;
    json_value *jv = resp ? json_parse(resp, NULL) : NULL;
    if (jv) {
        const json_value *root = jv;
        if (root->type == JSON_ARRAY && root->count > 0) root = root->items[0];
        const json_value *e = json_obj_get(root, "error");
        if (e) msg = json_as_str(json_obj_get(e, "message"));
    }
    fprintf(stderr, "%s: HTTP %d%s%.300s\n", who, status,
            msg ? ": " : (resp && *resp ? ": " : ""),
            msg ? msg : (resp ? resp : ""));
    if (status == 401) fprintf(stderr, "%s: is ANACHRON_API_KEY set and valid?\n", who);
    json_free(jv);
}

static int api_generate(void *impl, const char *prompt, const char *grammar,
                        void (*on_token)(const char *piece, void *ud), void *ud) {
    struct api_impl *c = impl;
    (void)prompt;    /* chat APIs take the structured history instead */
    (void)grammar;   /* GBNF does not apply to hosted APIs */
    c->last_prompt_tokens = 0;
    c->last_completion_tokens = 0;
    if (!c->h) {
        fprintf(stderr, "infer_api: no conversation provided (set_chat missing)\n");
        return -1;
    }

    strbuf body; sb_init(&body);
    if (c->kind == API_ANTHROPIC) {
        sb_appendf(&body, "{\"model\":\"%s\",\"max_tokens\":%d,\"system\":\"",
                   c->model, c->max_tokens);
        json_escape(&body, c->system_text ? c->system_text : "");
        sb_append(&body, "\",\"messages\":[");
        put_history(&body, c->h);
        sb_append(&body, "]}");
    } else {
        sb_appendf(&body, "{\"model\":\"%s\",\"max_tokens\":%d,\"temperature\":0,"
                          "\"messages\":[{\"role\":\"system\",\"content\":\"",
                   c->model, c->max_tokens);
        json_escape(&body, c->system_text ? c->system_text : "");
        sb_append(&body, "\"}");
        if (c->h->count > 0) {
            sb_append(&body, ",");
            put_history(&body, c->h);
        }
        sb_append(&body, "]}");
    }

    strbuf hdrs; sb_init(&hdrs);
    if (c->kind == API_ANTHROPIC) {
        sb_appendf(&hdrs, "x-api-key: %s\r\nanthropic-version: 2023-06-01\r\n",
                   c->api_key ? c->api_key : "");
    } else if (c->api_key) {
        sb_appendf(&hdrs, "Authorization: Bearer %s\r\n", c->api_key);
    }

    char err[160];
    char *resp = NULL; size_t rlen = 0; int status = 0;
    int hr = plat_http_post(c->url, hdrs.len ? sb_cstr(&hdrs) : NULL,
                            sb_cstr(&body), body.len,
                            &resp, &rlen, &status, err, sizeof err);
    sb_free(&hdrs);
    sb_free(&body);
    if (hr != 0) {
        fprintf(stderr, "infer_api: %s\n", err);
        return -1;
    }
    if (status != 200) {
        print_api_error("infer_api", status, resp);
        free(resp);
        return -1;
    }

    const char *jerr = NULL;
    json_value *jv = json_parse(resp, &jerr);
    int rc = 0;
    if (!jv) {
        fprintf(stderr, "infer_api: could not parse response JSON (%s)\n",
                jerr ? jerr : "?");
        free(resp);
        return -1;
    }

    if (c->kind == API_ANTHROPIC) {
        /* content is an array of blocks; concatenate the text ones. A refusal
         * (stop_reason "refusal") can arrive with no text at all — say so
         * instead of handing the loop an empty reply. */
        const json_value *content = json_obj_get(jv, "content");
        int emitted = 0;
        if (content && content->type == JSON_ARRAY) {
            for (size_t i = 0; i < content->count; i++) {
                const json_value *blk = content->items[i];
                const char *t = json_as_str(json_obj_get(blk, "type"));
                if (t && strcmp(t, "text") == 0) {
                    const char *text = json_as_str(json_obj_get(blk, "text"));
                    if (text && *text) {
                        if (on_token) on_token(text, ud);
                        emitted = 1;
                    }
                }
            }
        }
        if (!emitted) {
            const char *sr = json_as_str(json_obj_get(jv, "stop_reason"));
            if (on_token)
                on_token(sr && strcmp(sr, "refusal") == 0
                         ? "(the API declined this request)"
                         : "(the API returned no text)", ud);
        }
        const json_value *usage = json_obj_get(jv, "usage");
        if (usage) {
            const json_value *in = json_obj_get(usage, "input_tokens");
            const json_value *outt = json_obj_get(usage, "output_tokens");
            c->last_prompt_tokens = (in && in->type == JSON_NUMBER) ? (int)in->num : 0;
            c->last_completion_tokens = (outt && outt->type == JSON_NUMBER) ? (int)outt->num : 0;
        }
    } else {
        const json_value *choices = json_obj_get(jv, "choices");
        const char *text = NULL;
        if (choices && choices->type == JSON_ARRAY && choices->count > 0) {
            const json_value *m = json_obj_get(choices->items[0], "message");
            if (m) text = json_as_str(json_obj_get(m, "content"));
        }
        if (text && *text) {
            if (on_token) on_token(text, ud);
        } else {
            if (on_token) on_token("(the API returned no text)", ud);
        }
        const json_value *usage = json_obj_get(jv, "usage");
        if (usage) {
            const json_value *pt = json_obj_get(usage, "prompt_tokens");
            const json_value *ct = json_obj_get(usage, "completion_tokens");
            c->last_prompt_tokens = (pt && pt->type == JSON_NUMBER) ? (int)pt->num : 0;
            c->last_completion_tokens = (ct && ct->type == JSON_NUMBER) ? (int)ct->num : 0;
        }
    }
    json_free(jv);
    free(resp);
    return rc;
}

static void api_last_usage(const void *impl, int *prompt_tokens, int *completion_tokens) {
    const struct api_impl *c = impl;
    if (prompt_tokens) *prompt_tokens = c ? c->last_prompt_tokens : 0;
    if (completion_tokens) *completion_tokens = c ? c->last_completion_tokens : 0;
}

static void api_free(void *impl) {
    struct api_impl *c = impl;
    if (!c) return;
    free(c->model);
    free(c->url);
    free(c->api_key);
    free(c);
}

int api_backend_open(const char *spec, int n_ctx, infer_backend *out) {
    (void)n_ctx;   /* hosted contexts dwarf ours; history is budgeted by the agent */
    enum api_kind kind;
    const char *model;
    const char *label;
    const char *default_base;
    if (strncmp(spec, "anthropic:", 10) == 0) {
        kind = API_ANTHROPIC; model = spec + 10; label = "anthropic";
        default_base = "https://api.anthropic.com";
    } else if (strncmp(spec, "openai:", 7) == 0) {
        kind = API_OPENAI; model = spec + 7; label = "openai";
        default_base = "https://api.openai.com";
    } else if (strncmp(spec, "gemini:", 7) == 0) {
        /* Google's Gemini API speaks the OpenAI chat shape on its compat layer;
         * the base already carries a path, so only the leaf gets appended below. */
        kind = API_OPENAI; model = spec + 7; label = "gemini";
        default_base = "https://generativelanguage.googleapis.com/v1beta/openai";
    } else return -1;
    if (!*model) {
        fprintf(stderr, "infer_api: no model named (e.g. %s:%s)\n", label,
                kind == API_ANTHROPIC ? "claude-opus-4-8" : "gemini-2.5-pro");
        return -1;
    }

    struct api_impl *c = xmalloc(sizeof *c);
    memset(c, 0, sizeof *c);
    c->kind = kind;
    c->model = xstrdup(model);

    const char *base = getenv("ANACHRON_API_URL");
    if (!base || !*base) base = default_base;
    size_t blen = strlen(base);
    while (blen > 0 && base[blen - 1] == '/') blen--;
    /* Endpoint resolution: a bare scheme://host[:port] gets the API's canonical
     * /v1/... path; a base that already carries a path (Google's compat layer,
     * reverse proxies) gets only the endpoint leaf; a base that already IS the
     * full endpoint is used as-is. */
    const char *leaf      = (kind == API_ANTHROPIC) ? "/messages" : "/chat/completions";
    const char *leaf_full = (kind == API_ANTHROPIC) ? "/v1/messages" : "/v1/chat/completions";
    size_t llen = strlen(leaf);
    const char *scheme_end = strstr(base, "://");
    const char *first_slash = scheme_end ? strchr(scheme_end + 3, '/') : NULL;
    int has_path = first_slash && (size_t)(first_slash - base) < blen;
    strbuf u; sb_init(&u);
    sb_append_n(&u, base, blen);
    if (blen >= llen && strncmp(base + blen - llen, leaf, llen) == 0)
        ;                                   /* already the full endpoint */
    else if (has_path)
        sb_append(&u, leaf);
    else
        sb_append(&u, leaf_full);
    c->url = xstrdup(sb_cstr(&u));
    sb_free(&u);

    const char *key = getenv("ANACHRON_API_KEY");
    if (key && *key) {
        strbuf k; sb_init(&k);
        for (const char *p = key; *p; p++)
            if (*p != '\r' && *p != '\n') sb_putc(&k, *p);
        c->api_key = xstrdup(sb_cstr(&k));
        sb_free(&k);
    }
    if (!c->api_key && strncmp(spec, "gemini:", 7) == 0) {
        fprintf(stderr, "infer_api: gemini needs ANACHRON_API_KEY "
                        "(a free key from aistudio.google.com/apikey)\n");
        free(c->model); free(c->url); free(c);
        return -1;
    }
    if (kind == API_ANTHROPIC && !c->api_key) {
        fprintf(stderr, "infer_api: anthropic needs ANACHRON_API_KEY "
                        "(or \"api_key\" in agent.json)\n");
        free(c->model); free(c->url); free(c);
        return -1;
    }

    {   /* per-response output cap; generous default, overridable */
        const char *e = getenv("ANACHRON_API_MAX_TOKENS");
        c->max_tokens = (e && atoi(e) > 0) ? atoi(e) : 8192;
    }

    fprintf(stderr, "infer_api: %s -> %s (model %s)\n", label, c->url, c->model);

    out->impl = c;
    out->generate = api_generate;
    out->set_chat = api_set_chat;
    out->last_usage = api_last_usage;
    out->free_impl = api_free;
    return 0;
}
