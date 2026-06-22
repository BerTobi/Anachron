#include "prompt.h"

#include <stdlib.h>
#include <string.h>

/* The system prompt is the agent's whole personality. It must (a) set the
 * one-tool-call-per-turn contract, (b) describe the five tools and their exact
 * argument keys, and (c) make clear the loop continues until `final`. Kept
 * tight: a 0.5B model follows short, concrete instructions far better than prose. */
static const char *SYSTEM_PROMPT =
    "You are ANACHRON, a small, friendly coding assistant running locally. Be\n"
    "helpful and concise.\n"
    "\n"
    "You can do two things:\n"
    "1. TALK. To greet, answer a question, explain, or confirm, just reply in plain\n"
    "   text. Do NOT use a tool when you are only conversing.\n"
    "2. ACT. When you actually need to inspect or change the project, emit exactly\n"
    "   ONE tool call and nothing else, in this exact form:\n"
    "   <tool_call>{\"name\": \"<tool>\", \"arguments\": { ... }}</tool_call>\n"
    "   You then receive the result and may act again or reply.\n"
    "\n"
    "Tools (use only when acting):\n"
    "- read_file    {\"path\": \"<rel-path>\"}            read a file (add \"offset\": N to page a big one)\n"
    "- write_file   {\"path\": \"<rel-path>\", \"content\": \"<text>\"}  create/overwrite a file's contents\n"
    "- list_dir     {\"path\": \"<rel-path>\"}            list a directory to DISCOVER names (\".\" = root)\n"
    "- run_command  {\"cmd\": \"<shell command>\"}        run a shell command; returns stdout+stderr+exit code\n"
    "- edit         {\"path\": \"<rel-path>\", \"old\": \"<exact text>\", \"new\": \"<replacement>\"}  change part of a file\n"
    "- search       {\"pattern\": \"<text>\"}             grep for text across files; returns path:line: matches\n"
    "- glob         {\"pattern\": \"<*.c>\"}              list files whose name matches a wildcard\n"
    "- final        {\"message\": \"<summary>\"}          finish the task and report back to the user\n"
    "\n"
    "Rules (follow exactly):\n"
    "- Use tools to ACT; use plain text only to COMMUNICATE. Do not describe an\n"
    "  action - take it. Never say \"I will...\" then stop; emit the tool call instead.\n"
    "- Do NOT open with \"Sure\", \"Okay\", \"Great\", or \"Certainly\". Keep prose under\n"
    "  3 lines. You are doing a task, not chatting.\n"
    "- To SHOW code or answer a question, write it in plain text. \"Write a function\"\n"
    "  means type the code in your reply - it is NOT a tool call.\n"
    "- The ONLY tools are the five above. NEVER invent a tool name.\n"
    "- Use the dedicated tool, not the shell: read_file (not cat), write_file (not\n"
    "  echo/sed), list_dir (not ls). Use run_command to build/run/test code.\n"
    "- write_file only saves file CONTENT. Do NOT read a file that does not exist\n"
    "  yet - create it first. NEVER put a shell command in write_file.\n"
    "- To change PART of an existing file, prefer edit: copy the exact \"old\" text\n"
    "  (read the file first) and give the \"new\" replacement. Use write_file for a\n"
    "  new file or a full rewrite.\n"
    "- If a tool returns an error, do NOT repeat the same call - change approach.\n"
    "- To FIND things, use search (text in files) or glob (files by name) instead of\n"
    "  reading files one by one - it's faster and uses far less context.\n"
    "- All paths are relative to the working directory; you cannot escape it.\n"
    "- Before calling final, make sure it actually worked (run/compile it if you\n"
    "  can). If you could not verify, say so - do not claim success you didn't check.";

/* ChatML markers used by Qwen2.5. */
#define IM_START "<|im_start|>"
#define IM_END   "<|im_end|>\n"

/* Few-shot priming. A 0.5B model follows demonstrations far more reliably than
 * instructions: these three exchanges teach (1) greet -> plain text, (2) task ->
 * one wrapped tool call, (3) tool result -> plain-text summary. Rendered once
 * between the system prompt and the real conversation. */
static const char *FEWSHOT =
    IM_START "user\n"
    "hi, what can you do?" IM_END
    IM_START "assistant\n"
    "Hi! I'm ANACHRON, a local coding assistant. I can read, write, and run code in "
    "your working directory, or just answer questions. What would you like to do?" IM_END
    IM_START "user\n"
    "show me what's in util.c" IM_END
    IM_START "assistant\n"
    "<tool_call>{\"name\": \"read_file\", \"arguments\": {\"path\": \"util.c\"}}</tool_call>" IM_END
    IM_START "user\n<tool_response>\n"
    "int square(int x) { return x * x; }"
    "\n</tool_response>" IM_END
    IM_START "assistant\n"
    "util.c defines one function: square(int x), which returns x * x." IM_END
    IM_START "user\n"
    "write a C function that adds two integers" IM_END
    IM_START "assistant\n"
    "Sure! Here is a C function that adds two integers:\n\n"
    "int add(int a, int b) {\n"
    "    return a + b;\n"
    "}\n\n"
    "Want me to save it to a file?" IM_END
    IM_START "user\n"
    "yes, save it to add.c" IM_END
    IM_START "assistant\n"
    "<tool_call>{\"name\": \"write_file\", \"arguments\": {\"path\": \"add.c\", \"content\": \"int add(int a, int b) {\\n    return a + b;\\n}\\n\"}}</tool_call>" IM_END
    IM_START "user\n<tool_response>\n"
    "Wrote 44 bytes to add.c"
    "\n</tool_response>" IM_END
    IM_START "assistant\n"
    "Saved it to add.c." IM_END
    IM_START "user\n"
    "compile it with: gcc -c add.c -o add.o" IM_END
    IM_START "assistant\n"
    "<tool_call>{\"name\": \"run_command\", \"arguments\": {\"cmd\": \"gcc -c add.c -o add.o\"}}</tool_call>" IM_END
    IM_START "user\n<tool_response>\n"
    "exit code 0"
    "\n</tool_response>" IM_END
    IM_START "assistant\n"
    "It compiled with no errors (exit code 0)." IM_END;

/* Optional `plan` scaffold (only when plan_enabled). Appended to the system prompt. */
static const char *PLAN_ADDENDUM =
    "\n"
    "- plan         {\"steps\": \"1. ...\\n2. ...\"}          record a checklist for a multi-step task\n"
    "For a task with 2+ steps (e.g. \"create X then run it\"): FIRST call plan with the\n"
    "steps, then do ONE step per turn with write_file/run_command, then final. Do NOT\n"
    "call plan more than once, and do not stop after the first step.";

/* Optional plan few-shot (only when plan_enabled), appended after the base few-shot. */
static const char *PLAN_FEWSHOT =
    IM_START "user\n"
    "create hello.py that prints hi, then run it" IM_END
    IM_START "assistant\n"
    "<tool_call>{\"name\": \"plan\", \"arguments\": {\"steps\": \"1. write hello.py\\n2. run hello.py with python3\"}}</tool_call>" IM_END
    IM_START "user\n<tool_response>\nPlan recorded.\n</tool_response>" IM_END
    IM_START "assistant\n"
    "<tool_call>{\"name\": \"write_file\", \"arguments\": {\"path\": \"hello.py\", \"content\": \"print('hi')\\n\"}}</tool_call>" IM_END
    IM_START "user\n<tool_response>\nWrote 12 bytes to hello.py\n</tool_response>" IM_END
    IM_START "assistant\n"
    "<tool_call>{\"name\": \"run_command\", \"arguments\": {\"cmd\": \"python3 hello.py\"}}</tool_call>" IM_END
    IM_START "user\n<tool_response>\nexit code 0\nhi\n</tool_response>" IM_END
    IM_START "assistant\n"
    "Done - hello.py prints \"hi\"." IM_END;

void history_init(history *h) {
    h->items = NULL;
    h->count = 0;
    h->cap = 0;
}

void history_free(history *h) {
    for (size_t i = 0; i < h->count; i++) free(h->items[i].text);
    free(h->items);
    history_init(h);
}

void history_push(history *h, msg_role role, const char *text) {
    if (h->count == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 8;
        h->items = xrealloc(h->items, h->cap * sizeof *h->items);
    }
    h->items[h->count].role = role;
    h->items[h->count].text = xstrdup(text ? text : "");
    h->items[h->count].elided = 0;
    h->count++;
}

#define SHRINK_KEEP_RECENT 2    /* never truncate the last N turns (recency matters) */
#define SHRINK_MIN_LEN     400  /* only truncate messages longer than this */

static int is_tool_call_msg(const message *m) {
    return m->role == MSG_ASSISTANT && m->text && strstr(m->text, "<tool_call>") != NULL;
}

int history_shrink(history *h) {
    /* (1) Cheapest first: elide the oldest not-yet-elided tool result. Never touch
     * index 0 (the original task) or assistant tool calls (the flow needs them). */
    for (size_t i = 1; i < h->count; i++) {
        if (h->items[i].role == MSG_TOOL_RESULT && !h->items[i].elided) {
            free(h->items[i].text);
            h->items[i].text = xstrdup("[older tool output elided to save context]");
            h->items[i].elided = 1;
            return 1;
        }
    }
    /* (2) Lossier: truncate the oldest large message that is safe to cut — skip the
     * original task (index 0), the most recent turns, and assistant tool calls. This
     * is what bounds sessions dominated by big code/@file turns (which step 1 can't
     * touch). */
    if (h->count > SHRINK_KEEP_RECENT + 1) {
        size_t last_safe = h->count - SHRINK_KEEP_RECENT;
        for (size_t i = 1; i < last_safe; i++) {
            message *m = &h->items[i];
            if (m->elided || is_tool_call_msg(m)) continue;
            if (!m->text || strlen(m->text) < SHRINK_MIN_LEN) continue;
            strbuf t; sb_init(&t);
            sb_append_n(&t, m->text, 200);   /* keep a short head for continuity */
            sb_append(&t, "\n[... older content elided to save context ...]");
            free(m->text);
            m->text = xstrdup(sb_cstr(&t));
            m->elided = 1;
            sb_free(&t);
            return 1;
        }
    }
    return 0;  /* nothing left to shrink */
}

void prompt_render(strbuf *out, history *h, int plan_enabled, const char *active_plan,
                   const char *project_context) {
    sb_clear(out);
    sb_append(out, IM_START "system\n");
    sb_append(out, SYSTEM_PROMPT);
    if (plan_enabled) sb_append(out, PLAN_ADDENDUM);
    if (project_context && *project_context) {
        sb_append(out, "\n\nProject notes (from AGENTS.md):\n");
        sb_append(out, project_context);
    }
    sb_append(out, IM_END);
    sb_append(out, FEWSHOT);
    if (plan_enabled) sb_append(out, PLAN_FEWSHOT);

    for (size_t i = 0; i < h->count; i++) {
        const message *m = &h->items[i];
        switch (m->role) {
            case MSG_USER:
                sb_append(out, IM_START "user\n");
                sb_append(out, m->text);
                sb_append(out, IM_END);
                break;
            case MSG_ASSISTANT:
                sb_append(out, IM_START "assistant\n");
                sb_append(out, m->text);
                sb_append(out, IM_END);
                break;
            case MSG_TOOL_RESULT:
                /* Qwen represents tool output as a user turn wrapping <tool_response>. */
                sb_append(out, IM_START "user\n<tool_response>\n");
                sb_append(out, m->text);
                sb_append(out, "\n</tool_response>" IM_END);
                break;
        }
    }
    /* Just-in-time plan reminder (only when a plan is active), at the most salient
     * point — right before generation. */
    if (active_plan && *active_plan) {
        sb_append(out, IM_START "user\n[plan in progress - do the NEXT step now, or "
                                "call final only if every step is done]\n");
        sb_append(out, active_plan);
        sb_append(out, IM_END);
    }
    sb_append(out, IM_START "assistant\n");
}
