/* infer_backend — the internal vtable each inference backend implements. Only
 * infer.c (the router) and the backend implementations include this; the agent
 * core sees just infer.h. */
#ifndef ANACHRON_INFER_BACKEND_H
#define ANACHRON_INFER_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

struct history;

typedef struct infer_backend {
    void *impl;
    int  (*generate)(void *impl, const char *prompt, const char *grammar,
                     void (*on_token)(const char *piece, void *ud), void *ud);
    /* nullable: only chat-API backends consume the structured conversation */
    void (*set_chat)(void *impl, const struct history *h, const char *system_text);
    void (*last_usage)(const void *impl, int *prompt_tokens, int *completion_tokens);
    void (*free_impl)(void *impl);
} infer_backend;

/* Constructors: fill *out and return 0, or return non-zero on failure (after
 * printing the reason to stderr). */
int stub_backend_open  (const char *spec, int n_ctx, infer_backend *out);
int remote_backend_open(const char *spec, int n_ctx, infer_backend *out);
int api_backend_open   (const char *spec, int n_ctx, infer_backend *out);
#ifdef ANACHRON_HAVE_LLAMA
int llama_backend_open (const char *spec, int n_ctx, infer_backend *out);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ANACHRON_INFER_BACKEND_H */
