/* infer — inference behind a stable interface (brief §Inference Backend), now with
 * RUNTIME backend selection. One binary carries several backends; infer_init picks
 * by the shape of the model spec:
 *
 *   qwen2.5-...q8_0.gguf      local in-process llama.cpp (llama builds; stub otherwise)
 *   http://host:port          a llama.cpp `llama-server` on the LAN (/completion).
 *                             Plain HTTP - works from real XP, no TLS involved.
 *   anthropic:claude-opus-4-8 the Anthropic Messages API (needs ANACHRON_API_KEY)
 *   openai:gpt-...            any OpenAI-compatible /v1/chat/completions endpoint
 *                             (llama-server, LM Studio, Ollama, or api.openai.com)
 *
 * The agent core depends ONLY on these functions; backends live behind the
 * infer_backend vtable (infer_backend.h). */
#ifndef ANACHRON_INFER_H
#define ANACHRON_INFER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct infer_ctx infer_ctx;
struct history;   /* core/prompt.h; only chat-API backends look inside */

/* Open a backend for `model_spec` (see table above; NULL is allowed and handled by
 * the local backend - the stub runs, llama reports the missing model). Returns NULL
 * on failure. */
infer_ctx *infer_init(const char *model_spec, int n_ctx);

/* Generate a completion for `prompt`, optionally constrained by a GBNF `grammar`
 * (may be NULL; ignored by chat-API backends, honored by local AND remote llama).
 * Each decoded piece is delivered to `on_token` as it is produced so the UI can
 * stream on slow hardware. Returns 0 on success. */
int infer_generate(infer_ctx *c, const char *prompt, const char *grammar,
                   void (*on_token)(const char *piece, void *ud), void *ud);

/* Structured view of the conversation for chat-API backends, which need messages
 * rather than a rendered ChatML string. The agent calls this right before each
 * generate with the live history and the rendered system text; local/remote
 * backends ignore it. Pointers must stay valid through the following generate. */
void infer_set_chat(infer_ctx *c, const struct history *h, const char *system_text);

/* Token counts for the most recent infer_generate call (for usage display).
 * Best-effort: llama exact, servers report their own numbers, the stub
 * approximates. Both are 0 before the first generate. */
void infer_last_usage(const infer_ctx *c, int *prompt_tokens, int *completion_tokens);

void infer_free(infer_ctx *c);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ANACHRON_INFER_H */
