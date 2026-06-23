#include "agent.h"
#include "prompt.h"
#include "tools.h"
#include "strbuf.h"
#include "json.h"
#include "platform.h"
#include "interrupt.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>  /* strstr — without this it is implicitly declared and the
                        returned pointer is truncated to int on 64-bit (latent bug) */

/* Threaded through infer_generate's token callback: accumulate the full model
 * output while also forwarding each piece to the UI for live streaming. */
typedef struct {
    strbuf             *acc;
    const agent_config *cfg;
} stream_state;

static void on_token_trampoline(const char *piece, void *ud) {
    stream_state *st = ud;
    sb_append(st->acc, piece);
    if (st->cfg->on_token) st->cfg->on_token(piece, st->cfg->ud);
}

#define NOTICE(cfg, msg) do { \
    if ((cfg)->on_notice) (cfg)->on_notice((msg), (cfg)->ud); \
    log_kv((cfg), "notice", (msg)); \
} while (0)

/* Emit one labelled line to the optional debug log (off unless cfg->on_log is set). */
static void log_kv(const agent_config *cfg, const char *label, const char *text) {
    if (!cfg->on_log) return;
    strbuf s; sb_init(&s);
    sb_appendf(&s, "[%s] %s", label, text ? text : "");
    cfg->on_log(sb_cstr(&s), cfg->ud);
    sb_free(&s);
}

/* A stable signature for a tool call, used to detect repeated identical actions
 * (the death-spiral failure mode of weak models). */
static void tc_signature(const tool_call *c, strbuf *out) {
    sb_appendf(out, "%d|", (int)c->kind);
    if (c->path) sb_append(out, c->path);
    if (c->cmd)  sb_append(out, c->cmd);
    if (c->content) {
        unsigned long h = 5381;
        for (const char *p = c->content; *p; p++) h = ((h << 5) + h) + (unsigned char)*p;
        sb_appendf(out, "#%lu", h);
    }
}

/* Heuristic: does this reply DESCRIBE an action without taking it? Lets us turn
 * "I'll write the file..." (with no tool call) into a re-prompt instead of letting
 * the turn end with nothing done. Case-insensitive substring scan. */
static int has_action_intent(const char *s) {
    static const char *const phrases[] = {
        "i will", "i'll ", "let me", "let's", "i am going to", "i'm going to",
        "i need to", "now i", "next, i", "first, i", "going to create",
        "going to write", "going to run", "here is the plan", "here's the plan", NULL
    };
    for (int i = 0; phrases[i]; i++) {
        const char *needle = phrases[i];
        for (const char *p = s; *p; p++) {
            const char *a = p, *b = needle;
            while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
            if (!*b) return 1;
        }
    }
    return 0;
}

/* Re-prompt used by the recovery guard: a write/edit didn't land (it was rejected,
 * or it changed nothing) and the model is trying to end the turn anyway - typically
 * by narrating a false "I fixed it". Push it to actually emit the corrected write. */
static const char RECOVER_MSG[] =
    "Your last write did not take effect (it was rejected, or it changed nothing), so "
    "the requested change is NOT saved. Do not claim it is done - emit a write_file "
    "tool call NOW with the corrected, COMPLETE file content. Use \\n inside C strings, "
    "never a real line break.";

void agent_session_init(agent_session *s, const agent_config *cfg) {
    s->cfg = *cfg;
    history_init(&s->h);
    s->last_write = NULL;
    s->turn_prompt_tokens = 0;
    s->turn_completion_tokens = 0;
}

void agent_session_free(agent_session *s) {
    history_free(&s->h);
    free(s->last_write);
    s->last_write = NULL;
}

void agent_session_clear(agent_session *s) {
    history_free(&s->h);
    history_init(&s->h);
    free(s->last_write);
    s->last_write = NULL;
}

static const char *role_name(msg_role r) {
    switch (r) {
        case MSG_USER:        return "user";
        case MSG_ASSISTANT:   return "assistant";
        case MSG_TOOL_RESULT: return "tool";
    }
    return "user";
}

int agent_session_save(const agent_session *s, const char *path) {
    strbuf j;
    sb_init(&j);
    sb_append(&j, "[");
    for (size_t i = 0; i < s->h.count; i++) {
        const message *m = &s->h.items[i];
        if (i) sb_append(&j, ",");
        sb_append(&j, "{\"role\":\"");
        sb_append(&j, role_name(m->role));
        sb_append(&j, "\",\"text\":\"");
        json_escape(&j, m->text ? m->text : "");
        /* Persist the compaction flag too, so a reloaded already-elided tool
         * result isn't needlessly re-elided on the next compaction pass. */
        sb_append(&j, m->elided ? "\",\"elided\":1}" : "\"}");
    }
    sb_append(&j, "]");
    int rc = plat_write_file(path, sb_cstr(&j), j.len);
    sb_free(&j);
    return rc;
}

/* Returns 0 on success, -1 if the file cannot be read, -2 if it is not valid
 * JSON / not a JSON array (so callers can tell "no such session" from "corrupt"). */
int agent_session_load(agent_session *s, const char *path) {
    char *buf = NULL;
    size_t len = 0;
    if (plat_read_file(path, &buf, &len) != 0) return -1;

    const char *err = NULL;
    json_value *v = json_parse(buf, &err);
    free(buf);
    if (!v || v->type != JSON_ARRAY) { json_free(v); return -2; }

    history_free(&s->h);
    history_init(&s->h);
    /* A resumed conversation has no in-process snapshot, so /undo must not target
     * whatever the previous conversation last wrote (mirrors agent_session_clear). */
    free(s->last_write);
    s->last_write = NULL;
    for (size_t i = 0; i < v->count; i++) {
        const json_value *o = v->items[i];
        const char *role = json_as_str(json_obj_get(o, "role"));
        const char *text = json_as_str(json_obj_get(o, "text"));
        if (!role || !text) continue;
        msg_role r = strcmp(role, "assistant") == 0 ? MSG_ASSISTANT
                   : strcmp(role, "tool") == 0      ? MSG_TOOL_RESULT
                                                    : MSG_USER;
        history_push(&s->h, r, text);
        const json_value *el = json_obj_get(o, "elided");
        if (el && el->type == JSON_NUMBER && el->num != 0 && s->h.count > 0)
            s->h.items[s->h.count - 1].elided = 1;
    }
    json_free(v);
    return 0;
}

int agent_session_run_turn(agent_session *s, const char *user_msg) {
    const agent_config *cfg = &s->cfg;
    history *h = &s->h;
    history_push(h, MSG_USER, user_msg);

    tool_ctx tctx;
    tctx.sandbox_root  = cfg->sandbox_root;
    tctx.verify_writes = cfg->verify_writes;
    tctx.verify_cc     = cfg->verify_cc;
    tctx.diff_colour   = cfg->diff_colour;
    tctx.on_diff       = cfg->on_diff;
    tctx.ud            = cfg->ud;
    /* Prompt size budget that keeps the RENDERED prompt (system + few-shot +
     * AGENTS.md + history) inside the context window with room left to generate.
     * Measured in chars at a deliberately low ~3 chars/token so we over-estimate
     * tokens and shrink early rather than overflow. */
    int ctxt = cfg->ctx_tokens > 0 ? cfg->ctx_tokens : 4096;
    /* Reserve 1/4 of the window for generation and budget the rest at 3 chars/token.
     * Dense code/JSON runs ~2.85 chars/token, so reserving a *proportional* slice
     * keeps the rendered prompt below the window for any ctx (a fixed reserve could
     * be outgrown at very large ctx). */
    size_t max_prompt_chars = (size_t)(ctxt - ctxt / 4) * 3;

    s->turn_prompt_tokens = 0;
    s->turn_completion_tokens = 0;

    int finished = 0;
    int force_nudges = 0;          /* times we've nudged a stalled reply forward */
    int pending_unsaved = 0;       /* last write/edit didn't land (rejected or no-op), not yet re-saved */
    int repeat = 0;                /* consecutive identical tool calls */
    int replan_count = 0;          /* redundant plan re-calls (loop guard; --plan only) */
    char *active_plan = NULL;      /* the model's checklist for a multi-step task (--plan only) */
    strbuf prompt;   sb_init(&prompt);
    strbuf last_sig; sb_init(&last_sig);

    for (int iter = 0; iter < cfg->max_iters; iter++) {
        if (cfg->on_iter_start) cfg->on_iter_start(iter, cfg->ud);

        /* Render, and if the prompt would overflow the context window, shrink the
         * history one step and re-render — repeat until it fits or nothing is left
         * to drop. This is what keeps a long session from wedging the backend. */
        for (;;) {
            prompt_render(&prompt, h, cfg->plan_enabled, active_plan, cfg->project_context);
            if (prompt.len <= max_prompt_chars) break;
            if (!history_shrink(h)) break;
        }
        log_kv(cfg, "request", sb_cstr(&prompt));

        /* Mode-gate: once a plan is recorded, switch to the plan-free grammar so the
         * model physically cannot emit another `plan` and must execute a step. */
        const char *grammar = (active_plan && cfg->grammar_act) ? cfg->grammar_act
                                                                : cfg->grammar;
        strbuf acc; sb_init(&acc);
        stream_state st = { &acc, cfg };
        int grc = infer_generate(cfg->infer, sb_cstr(&prompt), grammar,
                                 on_token_trampoline, &st);
        if (grc == 0) {   /* skip usage on a failed generate: counts may be stale */
            int pt = 0, ct = 0;
            infer_last_usage(cfg->infer, &pt, &ct);
            s->turn_prompt_tokens = pt;        /* last iteration's prompt size */
            s->turn_completion_tokens += ct;   /* generated tokens summed over the turn */
        }

        /* Ctrl+C during generation: abort the turn without recording or parsing the
         * partial output, and return to the prompt. */
        if (interrupt_pending()) { sb_free(&acc); break; }

        const char *out = sb_cstr(&acc);
        history_push(h, MSG_ASSISTANT, out);
        log_kv(cfg, "model", out);

        /* Decide TALK vs ACT by whether the output parses as a valid tool call
         * (toolcall_parse accepts the <tool_call> wrapper or a bare JSON object).
         *   - parses        -> ACT: dispatch the tool and keep looping.
         *   - tag but botched-> the model meant to act; re-prompt to recover.
         *   - neither        -> TALK: it's a conversational reply; end the turn.
         * This is what lets it hold a conversation instead of flailing with tools
         * on a simple greeting, while still recovering from a malformed call. */
        /* Does the output look like it is TRYING to be a tool call? Either the
         * <tool_call> tag, or a bare JSON object (starts with '{' and has a
         * "name") — e.g. an invented tool like {"name":"add_numbers",...}. We
         * must not print such JSON to the user as if it were a reply. */
        const char *t = out;
        while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r') t++;
        int has_tag = strstr(out, "<tool_call>") != NULL;
        int looks_like_call = has_tag || (*t == '{' && strstr(out, "\"name\"") != NULL);

        tool_call call;
        int parsed = toolcall_parse(out, &call);

        if (parsed != 0) {
            if (looks_like_call) {
                /* Tried to call a tool but it was invalid (bad JSON or, commonly,
                 * an invented tool name). Nudge it toward plain text or a real tool. */
                NOTICE(cfg, call.error ? call.error : "invalid tool call");
                history_push(h, MSG_TOOL_RESULT,
                    "ERROR: that is not a valid tool call. The only tools are read_file, "
                    "write_file, list_dir, run_command, final. To show code or an answer, "
                    "reply in plain text with NO JSON. To save code to a file, use write_file.");
                toolcall_free(&call);
                sb_free(&acc);
                continue;
            }
            /* Force-continue: a plain reply that narrates an untaken action, or (with
             * --plan) stops with an active plan unfinished, is nudged forward rather
             * than ending the turn. Bounded (higher with a plan) so a model that is
             * genuinely just talking still gets to finish. */
            int plan_open = (active_plan != NULL);
            int nudge_cap = plan_open ? 4 : (pending_unsaved ? 3 : 2);
            if ((plan_open || pending_unsaved || has_action_intent(out)) && force_nudges < nudge_cap) {
                force_nudges++;
                const char *why, *msg;
                if (pending_unsaved) {        /* a write didn't land but the model is wrapping up */
                    why = "claimed completion while the file is unsaved; nudging to write it";
                    msg = RECOVER_MSG;
                } else if (plan_open) {
                    why = "stopped mid-plan; nudging to continue";
                    msg = "You have a plan in progress and have not finished it. Take the NEXT "
                          "step now (emit the tool call), or call final only if every step is done.";
                } else {
                    why = "described an action without taking it; nudging to act";
                    msg = "You described an action but did not take it. If you intend to act, "
                          "emit the tool call NOW - do not describe it. If you were only "
                          "answering, ignore this.";
                }
                NOTICE(cfg, why);
                history_push(h, MSG_USER, msg);
                toolcall_free(&call);
                sb_free(&acc);
                continue;
            }
            /* Plain conversational reply. */
            if (cfg->on_message) cfg->on_message(out, cfg->ud);
            toolcall_free(&call);
            sb_free(&acc);
            finished = 1;
            break;
        }
        sb_free(&acc);

        if (call.kind == TC_FINAL) {
            if (cfg->on_tool_call) cfg->on_tool_call(&call, cfg->ud);
            if (cfg->on_final) cfg->on_final(call.message ? call.message : "", cfg->ud);
            toolcall_free(&call);
            finished = 1;
            break;
        }

        /* plan scaffold (--plan only): record the checklist ONCE (in `active_plan`,
         * re-injected each turn). Small local models fixate on re-calling plan, so
         * refuse repeats and abandon after a couple so the loop can't spin. */
        if (cfg->plan_enabled && call.kind == TC_PLAN) {
            if (cfg->on_tool_call) cfg->on_tool_call(&call, cfg->ud);
            if (active_plan) {
                replan_count++;
                NOTICE(cfg, "re-planned instead of executing; redirecting to a step");
                if (replan_count >= 2) {
                    free(active_plan); active_plan = NULL;
                    history_push(h, MSG_TOOL_RESULT,
                        "STOP planning. Execute the first step NOW: emit a write_file or "
                        "run_command tool call. Do not call plan again.");
                } else {
                    history_push(h, MSG_TOOL_RESULT,
                        "You already have a plan. Do NOT call plan again - execute the "
                        "first unfinished step now with write_file or run_command.");
                }
                toolcall_free(&call);
                continue;
            }
            active_plan = xstrdup(call.plan ? call.plan : "");
            force_nudges = 0;
            strbuf po; sb_init(&po);
            sb_appendf(&po, "Plan recorded. Now immediately execute step 1 with a "
                            "write_file/run_command tool call - do NOT call plan again.\n%s",
                       active_plan);
            if (cfg->on_tool_result) cfg->on_tool_result(sb_cstr(&po), 1, cfg->ud);
            history_push(h, MSG_TOOL_RESULT, sb_cstr(&po));
            sb_free(&po);
            toolcall_free(&call);
            continue;
        }

        /* Deterministic loop guard: stop the model re-issuing the same failing
         * call forever (the read_file death-spiral). After 3 identical calls,
         * intervene instead of executing again. */
        strbuf sig; sb_init(&sig); tc_signature(&call, &sig);
        int is_repeat = (last_sig.len > 0 && strcmp(sb_cstr(&sig), sb_cstr(&last_sig)) == 0);
        repeat = is_repeat ? repeat + 1 : 1;
        sb_clear(&last_sig); sb_append(&last_sig, sb_cstr(&sig));
        sb_free(&sig);
        if (repeat >= 3) {
            NOTICE(cfg, "repeated the same tool call; intervening");
            history_push(h, MSG_TOOL_RESULT,
                "ERROR: you have issued this exact tool call repeatedly with the same "
                "result. Do NOT repeat it - try a different tool or arguments, or call final.");
            toolcall_free(&call);
            continue;
        }

        if (cfg->on_tool_call) cfg->on_tool_call(&call, cfg->ud);

        int ok = 0;
        char *obs = tools_dispatch(&tctx, &call, &ok);
        if (cfg->on_tool_result) cfg->on_tool_result(obs, ok, cfg->ud);
        log_kv(cfg, "result", obs);
        history_push(h, MSG_TOOL_RESULT, obs);
        /* Remember the last successful mutation so /undo knows what to revert. */
        if (ok && call.path &&
            (call.kind == TC_WRITE_FILE || call.kind == TC_EDIT)) {
            free(s->last_write);
            s->last_write = xstrdup(call.path);
        }
        /* Recovery-guard signal: a write/edit that returned ok==0 (rejected by the
         * verify-on-write check, or a no-op) leaves the requested change unsaved. The
         * turn-end guard uses this to stop a false "I fixed it" from ending the turn;
         * a successful write/edit clears it. */
        if (call.kind == TC_WRITE_FILE || call.kind == TC_EDIT)
            pending_unsaved = !ok;
        free(obs);
        toolcall_free(&call);
    }

    if (!finished && !interrupt_pending())
        NOTICE(cfg, "reached iteration cap without calling final");

    free(active_plan);
    sb_free(&prompt);
    sb_free(&last_sig);
    return finished ? 0 : 1;
}

int agent_run(const agent_config *cfg, const char *user_task) {
    agent_session s;
    agent_session_init(&s, cfg);
    int rc = agent_session_run_turn(&s, user_task);
    agent_session_free(&s);
    return rc;
}
