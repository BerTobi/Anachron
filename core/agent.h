/* agent — the platform-independent agentic loop (brief §Agent Loop).
 *   assemble prompt -> generate -> parse tool call -> dispatch -> feed back
 * repeated up to max_iters, with token streaming and malformed-call recovery.
 * The UI is decoupled via the event callbacks below, so /core stays free of any
 * console/OS knowledge — main.c renders the Claude-Code-style transcript. */
#ifndef ANACHRON_AGENT_H
#define ANACHRON_AGENT_H

#include "infer.h"
#include "toolcall.h"
#include "prompt.h"

typedef struct {
    infer_ctx  *infer;         /* backend (stub or llama) */
    const char *grammar;       /* GBNF to constrain decoding; NULL for the stub */
    const char *grammar_act;   /* GBNF used once a plan is active: same minus the `plan`
                                  tool, so the model is FORCED to execute (can't re-plan).
                                  NULL -> always use `grammar`. */
    const char *sandbox_root;  /* working directory for all tools */
    int         max_iters;     /* hard cap on loop turns */
    int         ctx_tokens;    /* model context window; drives compaction budget */
    int         verify_writes; /* 1 = verify-on-write guardrail (revert bad writes) */
    const char *verify_cc;     /* C compiler for the syntax check, or NULL */
    int         plan_enabled;  /* 1 = offer the `plan` scaffold (off by default; see --plan).
                                  Small local models fixate/loop on it; intended for a future
                                  capable model via remote/GPU inference. */
    const char *project_context; /* contents of an AGENTS.md/CRUSH.md, injected into the
                                    system prompt; NULL if none. */
    int         diff_colour;     /* 1 = ANSI-colour the diff shown on edits */
    void      (*on_diff)(const char *diff, void *ud); /* nullable: diff shown on edit */
    void      (*on_log)(const char *text, void *ud);  /* nullable debug log sink */

    /* UI hooks (all nullable). Called in loop order so the front-end can render
     * progress on slow hardware without /core knowing what a terminal is. */
    void *ud;
    void (*on_iter_start)(int iter, void *ud);
    void (*on_token)(const char *piece, void *ud);          /* streamed model output */
    void (*on_tool_call)(const tool_call *call, void *ud);
    /* nullable permission gate: called before a mutating/executing tool (write_file,
     * edit, run_command) is dispatched. Return nonzero to allow, 0 to decline. */
    int  (*confirm_tool)(const tool_call *call, void *ud);
    void (*on_tool_result)(const char *obs, int ok, void *ud);
    void (*on_message)(const char *text, void *ud);         /* plain conversational reply; turn ends */
    void (*on_final)(const char *message, void *ud);        /* explicit `final` tool; turn ends */
    void (*on_notice)(const char *text, void *ud);          /* e.g. re-prompt / cap hit */
} agent_config;

/* A conversation. The history persists across user turns, so the agent
 * remembers earlier messages, tool calls, and results — this is what makes it a
 * conversation rather than a series of one-shot tasks. */
typedef struct {
    agent_config cfg;
    history      h;
    char        *last_write;  /* sandbox-relative path of the most recent write/edit,
                                 NULL if none — drives /undo. Owned by the session. */
    int  turn_prompt_tokens;      /* prompt tokens of the last generate in the turn */
    int  turn_completion_tokens;  /* generated tokens summed over the turn */
} agent_session;

void agent_session_init(agent_session *s, const agent_config *cfg);
void agent_session_free(agent_session *s);

/* Reset the conversation history to empty (keeps cfg). Used by /new and /clear. */
void agent_session_clear(agent_session *s);

/* Persist / restore the conversation history as a JSON array of {role,text}.
 * agent_session_save returns 0 on success, non-zero on failure.
 * agent_session_load replaces history and returns 0 on success, -1 if the file
 * cannot be read (no such session), -2 if it is not valid JSON / not an array. */
int  agent_session_save(const agent_session *s, const char *path);
int  agent_session_load(agent_session *s, const char *path);

/* Append `user_msg` and run the tool loop until `final` or the iteration cap.
 * Returns 0 if it ended via `final`, non-zero if it hit the cap. The agent's
 * reply is delivered through cfg.on_final; history is retained for the next turn. */
int  agent_session_run_turn(agent_session *s, const char *user_msg);

/* Convenience: a single-turn run in a throwaway session (used by tests/e2e). */
int  agent_run(const agent_config *cfg, const char *user_task);

#endif /* ANACHRON_AGENT_H */
