/* toolcall — turn raw model text into a structured tool call.
 * The model is asked to emit exactly one  <tool_call>{json}</tool_call>  block
 * (Qwen/Hermes native format). This parser is deliberately lenient: it locates
 * the block (or a bare top-level JSON object as a fallback) and validates the
 * name + required arguments. If it can't, the agent loop re-prompts rather than
 * crashing — that recovery is what keeps the loop feeling agentic at 0.5B. */
#ifndef ANACHRON_TOOLCALL_H
#define ANACHRON_TOOLCALL_H

#include <stddef.h>   /* size_t (the parallel tasks array) */

#define AGENT_MAX_PAR 8   /* parallel sub-agent fan-out cap (processes, not threads) */

typedef enum {
    TC_NONE = 0,     /* nothing parseable — caller should re-prompt */
    TC_READ_FILE,
    TC_WRITE_FILE,
    TC_LIST_DIR,
    TC_RUN_COMMAND,
    TC_EDIT,
    TC_SEARCH,       /* grep text across files */
    TC_GLOB,         /* find files by name pattern */
    TC_SCREENSHOT,   /* capture the screen to a PNG (vision backends see it) */
    TC_FETCH,        /* GET a URL and return its text (HTML stripped) */
    TC_WEBSEARCH,    /* web search (DuckDuckGo HTML) -> result text */
    TC_AGENT,        /* run a sub-agent on a task in a fresh context (depth 1) */
    TC_ASK,          /* ask the human a question mid-task (interactive only) */
    TC_PLAN,         /* only offered when the plan scaffold is enabled (--plan) */
    TC_FINAL,
    TC_KIND_COUNT    /* keep last: sizes the per-tool permission table */
} tc_kind;

typedef struct {
    tc_kind kind;
    char   *path;     /* read_file / write_file / list_dir / edit; search: optional subdir */
    char   *content;  /* write_file; edit: the replacement ("new") text */
    char   *find;     /* edit: the search ("old") text */
    char   *pattern;  /* search: text to grep; glob: filename wildcard */
    char   *cmd;      /* run_command */
    char   *url;      /* fetch */
    char   *query;    /* websearch */
    char   *question; /* ask */
    char   *task;     /* agent: the sub-task description (single form) */
    char  **tasks;    /* agent: parallel form — up to AGENT_MAX_PAR sub-tasks */
    size_t  ntasks;
    char   *message;  /* final */
    char   *plan;     /* plan (steps, newline-separated) */
    long    offset;   /* read_file: line offset for paging (0 if absent) */
    int     has_offset;
    char   *error;    /* when kind == TC_NONE: why parsing failed (static or malloc'd via dup) */
} tool_call;

/* Returns 0 if a valid call was parsed (out->kind != TC_NONE), -1 otherwise
 * (out->kind == TC_NONE and out->error set). Always initializes *out.
 * Free with toolcall_free regardless of return value. */
int  toolcall_parse(const char *model_text, tool_call *out);
void toolcall_free(tool_call *tc);

const char *toolcall_kind_name(tc_kind k);

#endif /* ANACHRON_TOOLCALL_H */
