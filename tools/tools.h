/* tools — the five tool implementations and the dispatch entry point.
 * Each tool is sandboxed to a working directory and returns a human/model
 * readable observation string. Adding a tool is a matter of a new case here plus
 * a line in the system prompt + grammar — the loop itself never changes. */
#ifndef ANACHRON_TOOLS_H
#define ANACHRON_TOOLS_H

#include "toolcall.h"

typedef struct {
    const char *sandbox_root;  /* working directory all tools are confined to */
    int         verify_writes; /* 1 = run the verify-on-write guardrail */
    const char *verify_cc;     /* C compiler for the syntax check (e.g. "cc"); NULL = balance-only */
    /* Optional: when an existing file is overwritten/edited, a diff of the change
     * is rendered (with ANSI colour iff diff_colour) and handed to on_diff for the
     * UI — it is NOT fed back to the model. on_diff NULL disables the display. */
    int         diff_colour;
    void      (*on_diff)(const char *diff, void *ud);
    void       *ud;
} tool_ctx;

/* Execute `call` and return a malloc'd observation string (caller frees).
 * *ok is set to 1 on success, 0 on a tool-level error (the observation still
 * describes what went wrong so the model can react). TC_FINAL is handled by the
 * agent loop, not here. */
char *tools_dispatch(const tool_ctx *ctx, const tool_call *call, int *ok);

#endif /* ANACHRON_TOOLS_H */
