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
    "- DEFAULT TO SAVING when asked to create code: \"write/create/make/code/build a\n"
    "  program, script, function, game, or file\" means write_file it to a sensibly-named\n"
    "  file (e.g. tetris.c) in ONE step, then confirm in a line - do NOT print the code\n"
    "  first, and do NOT try to compile or run it before it exists. Reserve plain text\n"
    "  for \"show me\", \"explain\", \"what does this do\", or a snippet the user only reads.\n"
    "- ORDER MATTERS: a file must exist before you build or run it. To make-and-run\n"
    "  something, write_file FIRST, then run_command. NEVER run_command (gcc, ./prog, …)\n"
    "  on a file you have not created.\n"
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
 * instructions, so these exchanges teach the behaviours we want: (1) greet -> plain
 * text; (2) "show me X" -> read_file then a plain-text answer (read/explain does NOT
 * write); (3) "write a function" and (4) "code a program" -> write_file DIRECTLY to an
 * inferred filename (save-by-default, no print-then-resave). No standalone compile
 * example on purpose: the 0.5B copied `gcc -c X.c` and tried to build files it never
 * wrote. Rendered once between the system prompt and the real conversation. */
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
    "<tool_call>{\"name\": \"write_file\", \"arguments\": {\"path\": \"add.c\", \"content\": \"int add(int a, int b) {\\n    return a + b;\\n}\\n\"}}</tool_call>" IM_END
    IM_START "user\n<tool_response>\n"
    "Wrote 44 bytes to add.c (syntax OK)"
    "\n</tool_response>" IM_END
    IM_START "assistant\n"
    "Saved it to add.c." IM_END
    IM_START "user\n"
    "code a small C program that prints hello" IM_END
    IM_START "assistant\n"
    "<tool_call>{\"name\": \"write_file\", \"arguments\": {\"path\": \"hello.c\", \"content\": \"#include <stdio.h>\\n\\nint main(void) {\\n    printf(\\\"hello\\\\n\\\");\\n    return 0;\\n}\\n\"}}</tool_call>" IM_END
    IM_START "user\n<tool_response>\n"
    "Wrote 76 bytes to hello.c (syntax OK)"
    "\n</tool_response>" IM_END
    IM_START "assistant\n"
    "Saved it to hello.c." IM_END;

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

/* Lean prompt (ANACHRON_LEAN=1 / --lean): a terse system prompt + one demonstration,
 * ~1/5 the tokens of the full pair. The slow part of a cold turn is prefilling the
 * prompt, so on a Pentium-M this cuts first-turn latency ~5x. It keeps the essentials
 * (one-tool-call form, the tool list, save-by-default, talk-vs-act) but drops the
 * detailed rules/examples, so the 0.5B may be a bit less reliable on edits/recovery. */
static const char *LEAN_SYSTEM_PROMPT =
    "You are ANACHRON, a small local coding assistant. Be concise.\n"
    "TALK: to greet, answer, or explain, reply in plain text (no tool).\n"
    "ACT: emit exactly ONE tool call and nothing else, in this form:\n"
    "<tool_call>{\"name\": \"<tool>\", \"arguments\": { ... }}</tool_call>\n"
    "then you get the result and may act again or finish.\n"
    "Tools: read_file{path}, write_file{path,content}, edit{path,old,new}, list_dir{path}, "
    "run_command{cmd}, search{pattern}, glob{pattern}, final{message}. These are the ONLY tools.\n"
    "When asked to write/create/make/code a program, script, function, or file: write_file it "
    "to a sensible filename in ONE step, then confirm in a line - do not print the code first, "
    "and do not run a file before creating it. Use plain text for \"show me\"/\"explain\".\n"
    "Paths are relative to the working directory. When the task is done, call final.";

static const char *LEAN_FEWSHOT =
    IM_START "user\n" "hi" IM_END
    IM_START "assistant\n"
    "Hi! I can read, write, and run code in your working directory. What would you like to do?" IM_END
    IM_START "user\n" "write a C function that adds two integers" IM_END
    IM_START "assistant\n"
    "<tool_call>{\"name\": \"write_file\", \"arguments\": {\"path\": \"add.c\", \"content\": \"int add(int a, int b) {\\n    return a + b;\\n}\\n\"}}</tool_call>" IM_END
    IM_START "user\n<tool_response>\nWrote 36 bytes to add.c (syntax OK)\n</tool_response>" IM_END
    IM_START "assistant\n" "Saved it to add.c." IM_END;

void prompt_render(strbuf *out, history *h, int plan_enabled, const char *active_plan,
                   const char *project_context, int lean) {
    sb_clear(out);
    sb_append(out, IM_START "system\n");
    sb_append(out, lean ? LEAN_SYSTEM_PROMPT : SYSTEM_PROMPT);
    if (plan_enabled) sb_append(out, PLAN_ADDENDUM);
    if (project_context && *project_context) {
        sb_append(out, "\n\nProject notes (from AGENTS.md):\n");
        sb_append(out, project_context);
    }
    sb_append(out, IM_END);
    sb_append(out, lean ? LEAN_FEWSHOT : FEWSHOT);
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
