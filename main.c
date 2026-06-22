/* main — entry point: arg parsing, backend wire-up, and the console front-end.
 *
 * Two ways to run:
 *   - Interactive (no task on the command line): a conversational REPL. You type,
 *     the agent runs its tool loop and replies, the conversation persists, repeat.
 *     This is the Claude-Code-style experience.
 *   - One-shot (task given as arguments): run a single turn and exit. Handy for
 *     scripting and the e2e test.
 *
 * The renderer lives here (not in /core) and turns the agent's event callbacks
 * into a streamed transcript. Plain ASCII only — the XP console ignores ANSI. */
#include "agent.h"
#include "infer.h"
#include "toolcall.h"
#include "platform.h"
#include "strbuf.h"
#include "sandbox.h"
#include "obsfmt.h"
#include "json.h"
#include "interrupt.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISPLAY_MAX_LINES 20

typedef struct { FILE *out; FILE *log; } ui;

/* Debug log sink: append one line to the log file when --log/ANACHRON_LOG is set. */
static void ui_log(const char *text, void *ud) {
    ui *u = ud;
    if (!u->log) return;
    fprintf(u->log, "%s\n", text);
    fflush(u->log);
}

static void ui_token(const char *piece, void *ud) {
    ui *u = ud;
    fputs(piece, u->out);
    fflush(u->out);
}

/* A plain conversational reply. The text already streamed via ui_token, so just
 * close the line. */
static void ui_message(const char *text, void *ud) {
    ui *u = ud;
    (void)text;
    fputc('\n', u->out);
    fflush(u->out);
}

static void ui_tool_call(const tool_call *c, void *ud) {
    ui *u = ud;
    switch (c->kind) {
        case TC_READ_FILE:   fprintf(u->out, "\n> read_file(%s)\n", c->path); break;
        case TC_WRITE_FILE:  fprintf(u->out, "\n> write_file(%s, %zu bytes)\n",
                                     c->path, strlen(c->content ? c->content : "")); break;
        case TC_LIST_DIR:    fprintf(u->out, "\n> list_dir(%s)\n", c->path); break;
        case TC_RUN_COMMAND: fprintf(u->out, "\n> run_command(%s)\n", c->cmd); break;
        case TC_EDIT:        fprintf(u->out, "\n> edit(%s)\n", c->path); break;
        case TC_SEARCH:      fprintf(u->out, "\n> search(%s)\n", c->pattern ? c->pattern : ""); break;
        case TC_GLOB:        fprintf(u->out, "\n> glob(%s)\n", c->pattern ? c->pattern : ""); break;
        case TC_PLAN:        fprintf(u->out, "\n> plan:\n%s\n", c->plan ? c->plan : ""); break;
        default: break; /* final handled by ui_final */
    }
    fflush(u->out);
}

/* Print an observation indented, truncated to a sane number of lines so a long
 * file dump doesn't bury the transcript (the model still got the full text). */
static void ui_tool_result(const char *obs, int ok, void *ud) {
    ui *u = ud;
    fprintf(u->out, "  result%s:\n", ok ? "" : " (error)");
    const char *p = obs;
    int line = 0;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (line >= DISPLAY_MAX_LINES) {
            int remaining = 1;
            for (const char *q = p; (q = strchr(q, '\n')) != NULL; q++) remaining++;
            fprintf(u->out, "    ... (%d more line%s)\n", remaining, remaining == 1 ? "" : "s");
            break;
        }
        fprintf(u->out, "    %.*s\n", (int)len, p);
        line++;
        if (!nl) break;
        p = nl + 1;
    }
    fflush(u->out);
}

static void ui_final(const char *message, void *ud) {
    ui *u = ud;
    fprintf(u->out, "\n== final ==\n%s\n", message);
    fflush(u->out);
}

static void ui_notice(const char *text, void *ud) {
    ui *u = ud;
    fprintf(u->out, "\n[notice] %s\n", text);
    fflush(u->out);
}

/* Diff shown to the user when an existing file is edited (not fed to the model). */
static void ui_diff(const char *diff, void *ud) {
    ui *u = ud;
    fputc('\n', u->out);
    fputs(diff, u->out);
    fflush(u->out);
}

/* Per-turn footer: wall-clock plus the backend's token counts when available.
 * "ctx" is the final step's prompt size (not a turn sum); "gen" is summed over the
 * turn's tool-loop iterations, so the two are deliberately on different bases. */
static void print_turn_stats(double secs, const agent_session *s) {
    if (s->turn_prompt_tokens > 0 || s->turn_completion_tokens > 0)
        fprintf(stdout, "\n(%.2fs - %d ctx + %d gen tokens)\n",
                secs, s->turn_prompt_tokens, s->turn_completion_tokens);
    else
        fprintf(stdout, "\n(%.2fs)\n", secs);
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [options] [task...]\n"
        "  --model PATH      GGUF model (required for the llama backend; ignored by the stub)\n"
        "  --sandbox DIR     working directory tools are confined to (default \".\")\n"
        "  --max-iters N     tool-loop iteration cap per turn (default 8)\n"
        "  --ctx N           model context window in tokens (default 4096)\n"
        "  --grammar PATH    GBNF grammar to constrain decoding (default chosen by --plan)\n"
        "  --no-grammar      disable grammar constraint\n"
        "  --no-verify       disable the verify-on-write guardrail (revert of bad writes)\n"
        "  --verify          force-enable verify (overrides \"verify\": false in config)\n"
        "  --plan            offer the experimental `plan` tool (off by default; small\n"
        "                    local models fixate on it - intended for a capable backend)\n"
        "  --no-plan         force-disable the plan tool (overrides \"plan\": true in config)\n"
        "  --no-color        do not ANSI-colour the diff shown on edits\n"
        "  --log PATH        append a debug log (prompts, raw model output, tool results)\n"
        "  -V, --version     print the version and exit\n"
        "Defaults may also be set in agent.json / .anachron.json in the current directory\n"
        "(keys: model, sandbox, grammar, log, ctx, max_iters, verify, plan, grammar_enabled,\n"
        "color); CLI flags override the config. With no task arguments, starts an interactive\n"
        "conversation; type /help for in-session commands (/new /undo /save /model ...).\n",
        prog);
}

/* Load a grammar file into a malloc'd string, or return NULL (best effort). */
static char *load_grammar(const char *path) {
    char *buf = NULL; size_t len = 0;
    if (plat_read_file(path, &buf, &len) != 0) return NULL;
    return buf;
}

/* Load the project context file (AGENTS.md, else CRUSH.md) from the sandbox into a
 * malloc'd, size-capped string for the system prompt. NULL if neither exists. */
static char *load_project_context(const char *sandbox) {
    static const char *const names[] = { "AGENTS.md", "CRUSH.md", NULL };
    for (int i = 0; names[i]; i++) {
        char *abs = NULL;
        if (sandbox_resolve(sandbox, names[i], &abs) != 0) continue;
        char *buf = NULL; size_t len = 0;
        int rc = plat_read_file(abs, &buf, &len);
        free(abs);
        if (rc != 0) continue;
        char *capped = obs_capped(buf, 400, 12000); /* don't let it blow the ctx */
        free(buf);
        return capped;
    }
    return NULL;
}

/* Expand "@path" mentions in a user line: leave the line intact and append the
 * (size-capped) contents of each mentioned file that exists in the sandbox. A '@'
 * only counts at a word boundary, so emails etc. are left alone. Returns malloc'd. */
static char *expand_mentions(const char *line, const char *sandbox) {
    strbuf out; sb_init(&out);
    sb_append(&out, line);
    for (const char *p = line; *p; ) {
        int boundary = (p == line) || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n';
        if (*p == '@' && boundary && p[1] && p[1] != ' ' && p[1] != '\t') {
            const char *s = p + 1, *e = s;
            while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
            char *rel = xstrndup(s, (size_t)(e - s));
            char *abs = NULL;
            if (sandbox_resolve(sandbox, rel, &abs) == 0) {
                char *buf = NULL; size_t len = 0;
                if (plat_read_file(abs, &buf, &len) == 0) {
                    char *capped = obs_capped(buf, 300, 12000);
                    sb_appendf(&out, "\n\n[file %s]\n%s", rel, capped);
                    free(capped); free(buf);
                }
                free(abs);
            }
            free(rel);
            p = e;
        } else {
            p++;
        }
    }
    char *r = xstrdup(sb_cstr(&out));
    sb_free(&out);
    return r;
}

/* Probe for a C compiler so the verify-on-write guardrail can do a syntax check.
 * Returns a static name ("cc"/"gcc"/"clang") or NULL if none responds. */
static const char *detect_cc(void) {
    static const char *const cands[] = { "cc", "gcc", "clang", NULL };
    for (int i = 0; cands[i]; i++) {
        strbuf c; sb_init(&c);
        sb_appendf(&c, "%s --version", cands[i]);
        char *out = NULL; size_t ol = 0; int code = -1;
        int rc = plat_run_command(sb_cstr(&c), ".", &out, &ol, &code);
        sb_free(&c);
        free(out);
        if (rc == 0 && code == 0) return cands[i];
    }
    return NULL;
}

/* Read one line from stdin into `task`. Returns 0 on EOF. */
static int read_line(strbuf *task) {
    char line[4096];
    if (!fgets(line, sizeof line, stdin)) return 0;
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    sb_clear(task);
    sb_append(task, line);
    return 1;
}

/* ---- interactive slash commands -------------------------------------------
 * Sessions are flat JSON files under <sandbox>/.anachron-sessions/. The dir is
 * created lazily on the first /save. */

typedef enum { CMD_NOT_A_COMMAND, CMD_HANDLED, CMD_QUIT } cmd_result;

/* Resolve the sessions directory to an absolute path (does not create it). */
static int sessions_dir(const char *sandbox, char **out_abs) {
    return sandbox_resolve(sandbox, ".anachron-sessions", out_abs);
}

/* Case-insensitive match against the MS-DOS reserved device names (CON, PRN,
 * AUX, NUL, COM1-9, LPT1-9). Win32 resolves these regardless of directory or
 * extension, so a "con.json" session file would bind to the console device. */
static int is_reserved_dos_name(const char *base, size_t blen) {
    char lo[5];
    if (blen != 3 && blen != 4) return 0;
    for (size_t k = 0; k < blen; k++) {
        char c = base[k];
        lo[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lo[blen] = '\0';
    if (blen == 3)
        return strcmp(lo, "con") == 0 || strcmp(lo, "prn") == 0 ||
               strcmp(lo, "aux") == 0 || strcmp(lo, "nul") == 0;
    return (strncmp(lo, "com", 3) == 0 || strncmp(lo, "lpt", 3) == 0) &&
           lo[3] >= '1' && lo[3] <= '9';
}

/* Build <dir>/<name>.json from a filename-safe `name`. Returns 0 on success, -1
 * if the name is empty, contains characters outside [A-Za-z0-9._-] (rejected
 * rather than silently stripped so distinct names can't collide), or is a
 * Windows reserved device name. */
static int session_path(const char *dir, const char *name, char **out) {
    strbuf safe;
    sb_init(&safe);
    int lossy = 0;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
            sb_putc(&safe, c);
        else
            lossy = 1;
    }
    if (safe.len == 0 || lossy) { sb_free(&safe); return -1; }
    {
        const char *sc = sb_cstr(&safe);
        size_t blen = 0;
        while (sc[blen] && sc[blen] != '.') blen++;   /* base before first dot */
        if (is_reserved_dos_name(sc, blen)) { sb_free(&safe); return -1; }
    }
    strbuf path;
    sb_init(&path);
    sb_appendf(&path, "%s/%s.json", dir, sb_cstr(&safe));
    *out = xstrdup(sb_cstr(&path));
    sb_free(&safe);
    sb_free(&path);
    return 0;
}

/* /undo: restore the last written/edited file from its sibling .anbak snapshot. */
static void cmd_undo(agent_session *s, const char *sandbox) {
    if (!s->last_write) {
        fprintf(stdout, "nothing to undo\n");
        return;
    }
    char *abs = NULL;
    if (sandbox_resolve(sandbox, s->last_write, &abs) != 0) {
        fprintf(stdout, "cannot resolve %s\n", s->last_write);
        return;
    }
    strbuf bak;
    sb_init(&bak);
    sb_appendf(&bak, "%s.anbak", abs);
    char *prior = NULL;
    size_t plen = 0;
    if (plat_read_file(sb_cstr(&bak), &prior, &plen) != 0) {
        fprintf(stdout, "no snapshot to restore for %s "
                        "(it may have been newly created)\n", s->last_write);
    } else if (plat_write_file(abs, prior, plen) != 0) {
        fprintf(stdout, "found a snapshot for %s but could not restore it "
                        "(write failed); backup kept at %s.anbak\n",
                s->last_write, s->last_write);
    } else {
        fprintf(stdout, "reverted %s to its pre-write contents\n", s->last_write);
    }
    free(prior);
    sb_free(&bak);
    free(abs);
}

/* Handle a line beginning with '/'. Returns CMD_NOT_A_COMMAND if the line is not
 * a recognized command and should be sent to the model verbatim. `backend_slot`
 * and `ctx_tokens` let /model swap the inference backend in place. */
static cmd_result handle_command(const char *line, agent_session *s,
                                 const char *sandbox, int ctx_tokens,
                                 infer_ctx **backend_slot) {
    while (*line == ' ' || *line == '\t') line++;   /* tolerate leading blanks */
    if (line[0] != '/') return CMD_NOT_A_COMMAND;

    /* Split into verb and a single trailing argument (rest of the line). */
    char verb[32];
    size_t i = 0;
    while (line[i] && line[i] != ' ' && i < sizeof verb - 1) {
        verb[i] = line[i];
        i++;
    }
    verb[i] = '\0';
    /* If the token is longer than verb[], skip its tail so the remainder isn't
     * mis-carved into `arg`; an over-length token matches no command anyway. */
    while (line[i] && line[i] != ' ') i++;
    while (line[i] == ' ') i++;
    const char *arg = line + i;   /* "" if no argument */

    if (strcmp(verb, "/quit") == 0 || strcmp(verb, "/exit") == 0)
        return CMD_QUIT;

    if (strcmp(verb, "/help") == 0) {
        fprintf(stdout,
            "commands:\n"
            "  /help            show this help\n"
            "  /new, /clear     start a fresh conversation (clears history)\n"
            "  /undo            revert the last write/edit from its snapshot\n"
            "  /save [name]     save this conversation (default name: last)\n"
            "  /sessions        list saved conversations\n"
            "  /resume <name>   load a saved conversation\n"
            "  /model <path>    load a different GGUF model (keeps the conversation)\n"
            "  /quit, /exit     leave\n"
            "Anything else is sent to the model; use @path to attach a file.\n");
        return CMD_HANDLED;
    }

    if (strcmp(verb, "/new") == 0 || strcmp(verb, "/clear") == 0) {
        agent_session_clear(s);
        fprintf(stdout, "(history cleared)\n");
        return CMD_HANDLED;
    }

    if (strcmp(verb, "/model") == 0) {
        if (!arg[0]) {
            fprintf(stdout, "usage: /model <path-to-gguf>\n");
            return CMD_HANDLED;
        }
        infer_ctx *nb = infer_init(arg, ctx_tokens);
        if (!nb) {
            fprintf(stdout, "could not load model %s (keeping the current one)\n", arg);
            return CMD_HANDLED;
        }
        infer_free(*backend_slot);
        *backend_slot = nb;
        s->cfg.infer = nb;          /* run_turn reads the backend from the session's cfg */
        fprintf(stdout, "switched to model %s\n", arg);
        return CMD_HANDLED;
    }

    if (strcmp(verb, "/undo") == 0) {
        cmd_undo(s, sandbox);
        return CMD_HANDLED;
    }

    if (strcmp(verb, "/save") == 0) {
        char *dir = NULL;
        if (sessions_dir(sandbox, &dir) != 0) {
            fprintf(stdout, "cannot resolve sessions directory\n");
            return CMD_HANDLED;
        }
        plat_mkdir(dir);
        char *path = NULL;
        if (session_path(dir, arg[0] ? arg : "last", &path) != 0) {
            fprintf(stdout, "invalid session name\n");
            free(dir);
            return CMD_HANDLED;
        }
        if (agent_session_save(s, path) == 0)
            fprintf(stdout, "saved (%zu messages)\n", s->h.count);
        else
            fprintf(stdout, "save failed (%s)\n", path);
        free(path);
        free(dir);
        return CMD_HANDLED;
    }

    if (strcmp(verb, "/sessions") == 0) {
        char *dir = NULL;
        if (sessions_dir(sandbox, &dir) != 0) {
            fprintf(stdout, "cannot resolve sessions directory\n");
            return CMD_HANDLED;
        }
        plat_dirlist dl;
        if (plat_list_dir(dir, &dl) != 0 || dl.count == 0) {
            fprintf(stdout, "no saved sessions\n");
        } else {
            fprintf(stdout, "saved sessions:\n");
            for (size_t k = 0; k < dl.count; k++) {
                const char *nm = dl.names[k];
                size_t l = strlen(nm);
                if (!dl.is_dir[k] && l > 5 && strcmp(nm + l - 5, ".json") == 0)
                    fprintf(stdout, "  %.*s\n", (int)(l - 5), nm);
            }
            plat_dirlist_free(&dl);
        }
        free(dir);
        return CMD_HANDLED;
    }

    if (strcmp(verb, "/resume") == 0) {
        if (!arg[0]) {
            fprintf(stdout, "usage: /resume <name>\n");
            return CMD_HANDLED;
        }
        char *dir = NULL;
        if (sessions_dir(sandbox, &dir) != 0) {
            fprintf(stdout, "cannot resolve sessions directory\n");
            return CMD_HANDLED;
        }
        char *path = NULL;
        if (session_path(dir, arg, &path) != 0) {
            fprintf(stdout, "invalid session name\n");
            free(dir);
            return CMD_HANDLED;
        }
        int lr = agent_session_load(s, path);
        if (lr == 0)
            fprintf(stdout, "resumed '%s' (%zu messages)\n", arg, s->h.count);
        else if (lr == -2)
            fprintf(stdout, "session '%s' is corrupt or not valid JSON\n", arg);
        else
            fprintf(stdout, "no such session '%s'\n", arg);
        free(path);
        free(dir);
        return CMD_HANDLED;
    }

    fprintf(stdout, "unknown command %s (try /help)\n", verb);
    return CMD_HANDLED;
}

/* Find and parse a config file in the current directory. Returns a JSON object
 * (caller frees with json_free) and sets *out_path to the file used, or NULL if
 * none is present/valid. Keys present in it become defaults that CLI flags override. */
static json_value *load_config(const char **out_path) {
    static const char *const cands[] = { "agent.json", ".anachron.json", NULL };
    for (int i = 0; cands[i]; i++) {
        char *buf = NULL;
        size_t len = 0;
        if (plat_read_file(cands[i], &buf, &len) != 0) continue;
        const char *err = NULL;
        json_value *v = json_parse(buf, &err);
        free(buf);
        if (v && v->type == JSON_OBJECT) { *out_path = cands[i]; return v; }
        json_free(v);
    }
    return NULL;
}

/* Read a boolean key; returns `dflt` if absent or not a bool. */
static int cfg_bool(const json_value *o, const char *key, int dflt) {
    const json_value *v = json_obj_get(o, key);
    return (v && v->type == JSON_BOOL) ? v->boolean : dflt;
}

/* Read a numeric key; returns `dflt` if absent or not a number. */
static int cfg_int(const json_value *o, const char *key, int dflt) {
    const json_value *v = json_obj_get(o, key);
    return (v && v->type == JSON_NUMBER) ? (int)v->num : dflt;
}

int main(int argc, char **argv) {
    const char *model = NULL;
    const char *sandbox = ".";
    const char *grammar_path = NULL;     /* default chosen below based on --plan */
    int use_grammar = 1;
    int verify_writes = 1;
    int plan_enabled = 0;                /* off by default; see --plan */
    const char *log_path = NULL;         /* --log PATH or $ANACHRON_LOG; NULL = no log */
    int max_iters = 8;
    int ctx = 4096;  /* the system prompt + few-shot overhead is ~1.5k tokens; 2048
                        left too little room and long sessions overflowed */
    int want_color = 1;                  /* gated by TTY + platform below; --no-color forces off */

    /* Config file (agent.json / .anachron.json) sets defaults; CLI flags below
     * override them. String values are xstrdup'd so they outlive the parsed JSON. */
    const char *cfg_path = NULL;
    json_value *conf = load_config(&cfg_path);
    char *owned_model = NULL, *owned_sandbox = NULL,
         *owned_grammar = NULL, *owned_log = NULL;
    if (conf) {
        const char *s;
        if ((s = json_as_str(json_obj_get(conf, "model"))))
            model = owned_model = xstrdup(s);
        if ((s = json_as_str(json_obj_get(conf, "sandbox"))))
            sandbox = owned_sandbox = xstrdup(s);
        if ((s = json_as_str(json_obj_get(conf, "grammar"))))
            grammar_path = owned_grammar = xstrdup(s);
        if ((s = json_as_str(json_obj_get(conf, "log"))))
            log_path = owned_log = xstrdup(s);
        max_iters    = cfg_int(conf, "max_iters", max_iters);
        ctx          = cfg_int(conf, "ctx", ctx);
        verify_writes = cfg_bool(conf, "verify", verify_writes);
        plan_enabled  = cfg_bool(conf, "plan", plan_enabled);
        use_grammar   = cfg_bool(conf, "grammar_enabled", use_grammar);
        want_color    = cfg_bool(conf, "color", want_color);
        json_free(conf);
    }

    strbuf task; sb_init(&task);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--model") == 0 && i + 1 < argc) {
            model = argv[++i];
        } else if (strcmp(a, "--sandbox") == 0 && i + 1 < argc) {
            sandbox = argv[++i];
        } else if (strcmp(a, "--max-iters") == 0 && i + 1 < argc) {
            max_iters = atoi(argv[++i]);
        } else if (strcmp(a, "--ctx") == 0 && i + 1 < argc) {
            ctx = atoi(argv[++i]);
        } else if (strcmp(a, "--grammar") == 0 && i + 1 < argc) {
            grammar_path = argv[++i];
        } else if (strcmp(a, "--no-grammar") == 0) {
            use_grammar = 0;
        } else if (strcmp(a, "--no-verify") == 0) {
            verify_writes = 0;
        } else if (strcmp(a, "--verify") == 0) {
            verify_writes = 1;                /* re-enable when config set it off */
        } else if (strcmp(a, "--plan") == 0) {
            plan_enabled = 1;
        } else if (strcmp(a, "--no-plan") == 0) {
            plan_enabled = 0;                 /* re-disable when config set it on */
        } else if (strcmp(a, "--no-color") == 0 || strcmp(a, "--no-colour") == 0) {
            want_color = 0;
        } else if (strcmp(a, "--log") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(argv[0]);
            sb_free(&task);
            free(owned_model); free(owned_sandbox);
            free(owned_grammar); free(owned_log);
            return 0;
        } else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            fprintf(stdout, "anachron %s\n", ANACHRON_VERSION);
            sb_free(&task);
            free(owned_model); free(owned_sandbox);
            free(owned_grammar); free(owned_log);
            return 0;
        } else {
            if (task.len) sb_putc(&task, ' ');
            sb_append(&task, a);
        }
    }

    if (!grammar_path)
        grammar_path = plan_enabled ? "grammars/toolcall-plan.gbnf" : "grammars/toolcall.gbnf";

    char *grammar = NULL, *grammar_act = NULL;
    if (use_grammar) {
        grammar = load_grammar(grammar_path);
        if (!grammar)
            fprintf(stderr, "anachron: note: no grammar loaded from %s (continuing unconstrained)\n",
                    grammar_path);
        /* In --plan mode, also load the plan-free grammar used once a plan is
         * recorded, so the model is mode-gated into executing rather than re-planning. */
        if (plan_enabled) grammar_act = load_grammar("grammars/toolcall.gbnf");
    }

    /* The verify-on-write guardrail uses a C compiler for a syntax check when one
     * is available; without it, the structural balance check still runs. */
    const char *verify_cc = verify_writes ? detect_cc() : NULL;

    /* Colour the edit diffs only on an interactive POSIX terminal: the XP console
     * does not interpret ANSI escapes, so force it off there. */
    int use_color = want_color && plat_isatty_stdout();
#ifdef _WIN32
    use_color = 0;
#endif

    char *project_context = load_project_context(sandbox);

    if (!log_path) { const char *e = getenv("ANACHRON_LOG"); if (e && *e) log_path = e; }
    FILE *logf = log_path ? fopen(log_path, "a") : NULL;
    if (log_path && !logf)
        fprintf(stderr, "anachron: note: could not open log file %s\n", log_path);

    infer_ctx *backend = infer_init(model, ctx);
    if (!backend) {
        fprintf(stderr, "anachron: failed to initialize inference backend\n");
        free(grammar); free(grammar_act); free(project_context);
        free(owned_model); free(owned_sandbox); free(owned_grammar); free(owned_log);
        if (logf) fclose(logf);
        sb_free(&task);
        return 1;
    }

    ui u = { stdout, logf };
    agent_config cfg = {0};
    cfg.infer = backend;
    cfg.grammar = grammar;          /* stub ignores it; llama backend honors it */
    cfg.grammar_act = grammar_act;  /* plan-free grammar used after a plan (--plan only) */
    cfg.sandbox_root = sandbox;
    cfg.max_iters = max_iters;
    cfg.ctx_tokens = ctx;
    cfg.verify_writes = verify_writes;
    cfg.verify_cc = verify_cc;
    cfg.plan_enabled = plan_enabled;
    cfg.project_context = project_context;
    cfg.diff_colour = use_color;
    cfg.on_diff = ui_diff;
    cfg.on_log = logf ? ui_log : NULL;
    cfg.ud = &u;
    cfg.on_iter_start = NULL;       /* no step-counter noise; tool lines show progress */
    cfg.on_token = ui_token;
    cfg.on_tool_call = ui_tool_call;
    cfg.on_tool_result = ui_tool_result;
    cfg.on_message = ui_message;
    cfg.on_final = ui_final;
    cfg.on_notice = ui_notice;

    agent_session session;
    agent_session_init(&session, &cfg);

    /* Ctrl+C now interrupts the current generation instead of killing the process
     * (a second press still force-quits). */
    interrupt_install();

    int rc = 0;
    if (task.len > 0) {
        /* One-shot mode. */
        char *msg = expand_mentions(sb_cstr(&task), sandbox);
        double t0 = plat_time_sec();
        interrupt_clear();
        rc = agent_session_run_turn(&session, msg);
        print_turn_stats(plat_time_sec() - t0, &session);
        free(msg);
    } else {
        /* Interactive conversation. Clear any leftover mouse-reporting mode a prior
         * program may have left on, so the wheel scrolls the terminal's scrollback
         * instead of emitting escape sequences into the prompt (TTY + POSIX only). */
#ifndef _WIN32
        if (plat_isatty_stdout()) {
            fputs("\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?1049l", stdout);
            fflush(stdout);
        }
#endif
        fprintf(stdout,
            "ANACHRON " ANACHRON_VERSION " - local agentic coding harness\n"
            "  backend: %s\n"
            "  sandbox: %s\n"
            "  grammar: %s\n"
            "  verify : %s\n"
            "  context: %s\n"
            "  config : %s\n"
            "Type a task and press enter (use @path to attach a file). /help for commands.\n",
            model ? model : "stub (no model)", sandbox,
            grammar ? grammar_path : "(none)",
            !verify_writes ? "off" : (verify_cc ? "on (balance + cc syntax check)" : "on (balance check)"),
            project_context ? "AGENTS.md loaded" : "(no AGENTS.md)",
            cfg_path ? cfg_path : "(none)");
        for (;;) {
            plat_flush_input();   /* drop scroll/keystroke bytes from the last turn */
            fprintf(stdout, "\nyou> ");
            fflush(stdout);
            if (!read_line(&task)) { fprintf(stdout, "\n"); break; }
            if (task.len == 0) continue;
            cmd_result cr = handle_command(sb_cstr(&task), &session, sandbox, ctx, &backend);
            if (cr == CMD_QUIT) break;
            if (cr == CMD_HANDLED) continue;
            char *msg = expand_mentions(sb_cstr(&task), sandbox);
            double t0 = plat_time_sec();
            interrupt_clear();
            rc = agent_session_run_turn(&session, msg);
            print_turn_stats(plat_time_sec() - t0, &session);
            if (interrupt_pending()) fprintf(stdout, "(interrupted)\n");
            interrupt_clear();
            free(msg);
        }
    }

    agent_session_free(&session);
    infer_free(backend);
    free(grammar);
    free(grammar_act);
    free(project_context);
    free(owned_model); free(owned_sandbox); free(owned_grammar); free(owned_log);
    if (logf) fclose(logf);
    sb_free(&task);
    return rc;
}
