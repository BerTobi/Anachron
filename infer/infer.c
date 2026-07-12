/* infer — the backend router. Picks an implementation from the SHAPE of the model
 * spec at runtime (see infer.h), so one binary can run a local GGUF, a LAN
 * llama-server, or a hosted chat API, switchable with /model mid-session. */
#include "infer.h"
#include "infer_backend.h"
#include "strbuf.h"

#include <stdlib.h>
#include <string.h>

struct infer_ctx {
    infer_backend be;
};

static int starts_with(const char *s, const char *pfx) {
    return s && strncmp(s, pfx, strlen(pfx)) == 0;
}

infer_ctx *infer_init(const char *model_spec, int n_ctx) {
    infer_ctx *c = xmalloc(sizeof *c);
    memset(c, 0, sizeof *c);
    int rc;
    if (starts_with(model_spec, "http://") || starts_with(model_spec, "https://"))
        rc = remote_backend_open(model_spec, n_ctx, &c->be);
    else if (starts_with(model_spec, "anthropic:") || starts_with(model_spec, "openai:") ||
             starts_with(model_spec, "gemini:"))
        rc = api_backend_open(model_spec, n_ctx, &c->be);
    else
#ifdef ANACHRON_HAVE_LLAMA
        rc = llama_backend_open(model_spec, n_ctx, &c->be);
#else
        rc = stub_backend_open(model_spec, n_ctx, &c->be);
#endif
    if (rc != 0) {
        free(c);
        return NULL;
    }
    return c;
}

int infer_generate(infer_ctx *c, const char *prompt, const char *grammar,
                   void (*on_token)(const char *piece, void *ud), void *ud) {
    return c->be.generate(c->be.impl, prompt, grammar, on_token, ud);
}

void infer_set_chat(infer_ctx *c, const struct history *h, const char *system_text) {
    if (c->be.set_chat) c->be.set_chat(c->be.impl, h, system_text);
}

void infer_last_usage(const infer_ctx *c, int *prompt_tokens, int *completion_tokens) {
    if (!c) {
        if (prompt_tokens) *prompt_tokens = 0;
        if (completion_tokens) *completion_tokens = 0;
        return;
    }
    c->be.last_usage(c->be.impl, prompt_tokens, completion_tokens);
}

void infer_free(infer_ctx *c) {
    if (!c) return;
    c->be.free_impl(c->be.impl);
    free(c);
}
