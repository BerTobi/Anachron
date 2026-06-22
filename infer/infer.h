/* infer — the inference backend behind a stable interface (brief §Inference
 * Backend). Phase 1 ships infer_stub.c; Phase 2 adds infer_llama.cpp wrapping
 * libllama + ggml. The agent core depends ONLY on these three functions, so the
 * backend (and a future LAN-offload backend) is a pure link-time swap. */
#ifndef ANACHRON_INFER_H
#define ANACHRON_INFER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct infer_ctx infer_ctx;

/* Load a model and prepare a context with the given token window. For the stub
 * backend `gguf_path` may be NULL. Returns NULL on failure. */
infer_ctx *infer_init(const char *gguf_path, int n_ctx);

/* Generate a completion for `prompt`, optionally constrained by a GBNF `grammar`
 * (may be NULL). Each decoded piece is delivered to `on_token` as it is produced
 * so the UI can stream on slow hardware. Returns 0 on success. */
int infer_generate(infer_ctx *c, const char *prompt, const char *grammar,
                   void (*on_token)(const char *piece, void *ud), void *ud);

/* Token counts for the most recent infer_generate call (for usage display).
 * Either out-pointer may be NULL. Counts are best-effort: the stub approximates,
 * the remote backend reports the server's numbers, the llama backend is exact.
 * Both are 0 before the first generate. */
void infer_last_usage(const infer_ctx *c, int *prompt_tokens, int *completion_tokens);

void infer_free(infer_ctx *c);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ANACHRON_INFER_H */
