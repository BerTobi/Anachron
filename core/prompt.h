/* prompt — conversation state and prompt assembly.
 * Owns the message history and renders it into the model's native chat format.
 * This is model-specific (Qwen2.5 ChatML) but platform-independent, so it lives
 * in /core. The system prompt + tool schema are assembled here too. */
#ifndef ANACHRON_PROMPT_H
#define ANACHRON_PROMPT_H

#include "strbuf.h"
#include <stddef.h>

typedef enum {
    MSG_USER,         /* the human task, or a re-prompt nudge */
    MSG_ASSISTANT,    /* the model's raw output (the tool-call text) */
    MSG_TOOL_RESULT   /* an observation fed back to the model */
} msg_role;

typedef struct {
    msg_role role;
    char    *text;
    int      elided;  /* tool result already shrunk by compaction */
} message;

typedef struct {
    message *items;
    size_t   count;
    size_t   cap;
} history;

void history_init(history *h);
void history_free(history *h);
void history_push(history *h, msg_role role, const char *text);

/* Drop/shrink the oldest tool results until the rendered prompt is expected to
 * fit within `char_budget`. The original task and recent turns are preserved. */
/* Shrink the history by one step to reduce prompt size, escalating from cheap to
 * lossy: (1) elide the oldest not-yet-elided tool result; (2) else truncate the
 * oldest large message safe to cut (not the original task, the last turns, or an
 * assistant tool call). Returns 1 if something shrank, 0 if nothing is left.
 * The agent calls this in a loop until the rendered prompt fits the context. */
int history_shrink(history *h);

/* Render system prompt + tool schema + history into `out` as ChatML, ending with
 * an open assistant turn ready for generation. When `plan_enabled`, the optional
 * `plan` tool (+ its rule and few-shot) is offered; when `active_plan` is non-NULL,
 * a just-in-time plan reminder is injected right before the assistant turn. Both
 * are off in the default build. */
void prompt_render(strbuf *out, history *h, int plan_enabled, const char *active_plan,
                   const char *project_context);

#endif /* ANACHRON_PROMPT_H */
