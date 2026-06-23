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
#include <errno.h>
#ifndef _WIN32
#include <termios.h>   /* raw-mode line editing on a POSIX terminal */
#include <unistd.h>
#endif

#define DISPLAY_MAX_LINES 20

/* ANSI palette — emitted only when the UI has color enabled (interactive POSIX
 * terminal, not Windows, not --no-color). Kept muted and consistent. */
#define A_RESET  "\x1b[0m"
#define A_TITLE  "\x1b[1;36m"   /* bold cyan  — banner title, section headers */
#define A_PROMPT "\x1b[1;32m"   /* bold green — the you> prompt */
#define A_TOOL   "\x1b[36m"     /* cyan       — tool-call lines */
#define A_OK     "\x1b[32m"     /* green      — success */
#define A_ERR    "\x1b[31m"     /* red        — errors */
#define A_DIM    "\x1b[2m"      /* dim        — labels, secondary text */
#define A_NOTE   "\x1b[33m"     /* yellow     — notices */
#define A_FINAL  "\x1b[1;35m"   /* bold magenta — the final header */

typedef struct { FILE *out; FILE *log; int color; } ui;

/* Return `code` if the UI has color on, else "" — lets format strings stay simple. */
static const char *cc(const ui *u, const char *code) { return u->color ? code : ""; }

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
    const char *t = cc(u, A_TOOL), *r = cc(u, A_RESET);
    switch (c->kind) {
        case TC_READ_FILE:   fprintf(u->out, "\n%s> read_file(%s)%s\n", t, c->path, r); break;
        case TC_WRITE_FILE:  fprintf(u->out, "\n%s> write_file(%s, %zu bytes)%s\n",
                                     t, c->path, strlen(c->content ? c->content : ""), r); break;
        case TC_LIST_DIR:    fprintf(u->out, "\n%s> list_dir(%s)%s\n", t, c->path, r); break;
        case TC_RUN_COMMAND: fprintf(u->out, "\n%s> run_command(%s)%s\n", t, c->cmd, r); break;
        case TC_EDIT:        fprintf(u->out, "\n%s> edit(%s)%s\n", t, c->path, r); break;
        case TC_SEARCH:      fprintf(u->out, "\n%s> search(%s)%s\n", t, c->pattern ? c->pattern : "", r); break;
        case TC_GLOB:        fprintf(u->out, "\n%s> glob(%s)%s\n", t, c->pattern ? c->pattern : "", r); break;
        case TC_PLAN:        fprintf(u->out, "\n%s> plan:%s\n%s\n", t, r, c->plan ? c->plan : ""); break;
        default: break; /* final handled by ui_final */
    }
    fflush(u->out);
}

/* Print an observation indented, truncated to a sane number of lines so a long
 * file dump doesn't bury the transcript (the model still got the full text). */
static void ui_tool_result(const char *obs, int ok, void *ud) {
    ui *u = ud;
    if (ok) fprintf(u->out, "%s  result:%s\n", cc(u, A_DIM), cc(u, A_RESET));
    else    fprintf(u->out, "%s  result (error):%s\n", cc(u, A_ERR), cc(u, A_RESET));
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
    fprintf(u->out, "\n%s== final ==%s\n%s\n", cc(u, A_FINAL), cc(u, A_RESET), message);
    fflush(u->out);
}

static void ui_notice(const char *text, void *ud) {
    ui *u = ud;
    fprintf(u->out, "\n%s[notice] %s%s\n", cc(u, A_NOTE), text, cc(u, A_RESET));
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
static void print_turn_stats(const ui *u, double secs, const agent_session *s) {
    const char *d = cc(u, A_DIM), *r = cc(u, A_RESET);
    if (s->turn_prompt_tokens > 0 || s->turn_completion_tokens > 0)
        fprintf(u->out, "\n%s(%.2fs - %d ctx + %d gen tokens)%s\n",
                d, secs, s->turn_prompt_tokens, s->turn_completion_tokens, r);
    else
        fprintf(u->out, "\n%s(%.2fs)%s\n", d, secs, r);
}

/* ---- session stats (for /stats) ---------------------------------------- */
#define STAT_HIST 24   /* per-turn generated-token history kept for the graph */

typedef struct {
    int    turns;
    long   gen_tokens;        /* total generated across the session */
    long   ctx_tokens;        /* sum of per-turn final prompt sizes (context processed) */
    double seconds;           /* total wall-clock across turns */
    int    hist[STAT_HIST];   /* per-turn generated tokens, written as a ring */
    int    hist_n;            /* total turns recorded */
} session_stats;

static void stats_record(session_stats *st, const agent_session *s, double secs) {
    st->turns++;
    st->gen_tokens += s->turn_completion_tokens;
    st->ctx_tokens += s->turn_prompt_tokens;
    st->seconds    += secs;
    st->hist[st->hist_n % STAT_HIST] = s->turn_completion_tokens;
    st->hist_n++;
}

/* UTF-8 block characters for the sparkline (eighths, low -> high). */
static const char *const SPARK[8] = {
    "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
    "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"
};

static void stats_render(const ui *u, const session_stats *st) {
    const char *T = cc(u, A_TITLE), *D = cc(u, A_DIM), *G = cc(u, A_OK), *R = cc(u, A_RESET);
    fprintf(u->out, "\n%sSession stats%s\n", T, R);
    if (st->turns == 0) { fprintf(u->out, "  %sno turns yet%s\n", D, R); return; }

    fprintf(u->out, "  %sturns           %s %d\n", D, R, st->turns);
    fprintf(u->out, "  %sgenerated tokens%s %s%ld%s  (avg %ld/turn)\n",
            D, R, G, st->gen_tokens, R, st->gen_tokens / st->turns);
    fprintf(u->out, "  %scontext tokens  %s %ld  (processed)\n", D, R, st->ctx_tokens);
    fprintf(u->out, "  %swall time       %s %.1fs\n", D, R, st->seconds);
    if (st->seconds >= 0.05)   /* avoid a nonsense rate when timing is ~0 (e.g. the stub) */
        fprintf(u->out, "  %sthroughput      %s %s%.2f tok/s%s  (generated / wall-clock)\n",
                D, R, G, (double)st->gen_tokens / st->seconds, R);
    else
        fprintf(u->out, "  %sthroughput      %s n/a (too fast to measure)\n", D, R);

    int n = st->hist_n < STAT_HIST ? st->hist_n : STAT_HIST;
    int start = st->hist_n < STAT_HIST ? 0 : st->hist_n % STAT_HIST;
    int mx = 1;
    for (int k = 0; k < n; k++) { int v = st->hist[(start + k) % STAT_HIST]; if (v > mx) mx = v; }
    fprintf(u->out, "  %sgen tokens/turn %s ", D, R);
    if (u->color) {
        fputs(G, u->out);
        for (int k = 0; k < n; k++) {
            int v = st->hist[(start + k) % STAT_HIST];
            int lvl = v * 7 / mx; if (lvl < 0) lvl = 0; if (lvl > 7) lvl = 7;
            fputs(SPARK[lvl], u->out);
        }
        fputs(R, u->out);
        fprintf(u->out, "  (peak %d)\n", mx);
    } else {
        for (int k = 0; k < n; k++)
            fprintf(u->out, "%d ", st->hist[(start + k) % STAT_HIST]);
        fputc('\n', u->out);
    }
    fflush(u->out);
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
        "  --color/--no-color  force ANSI colour on/off (default: on for an interactive TTY)\n"
        "  --log PATH        append a debug log (prompts, raw model output, tool results)\n"
        "  -V, --version     print the version and exit\n"
        "Defaults may also be set in agent.json / .anachron.json in the current directory\n"
        "(keys: model, sandbox, grammar, log, ctx, max_iters, verify, plan, grammar_enabled,\n"
        "color); CLI flags override the config. With no task arguments, starts an interactive\n"
        "conversation; type /help for in-session commands (/new /undo /save /model /stats ...).\n",
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
/* Cooked-mode line read (piped input, non-terminals, and the Windows build). Strips
 * terminal escape sequences (e.g. arrow keys / mouse-wheel-as-arrows) so they don't
 * end up inside the command even when raw-mode editing isn't available. */
static int read_line_cooked(strbuf *task) {
    char line[4096];
    if (!fgets(line, sizeof line, stdin)) return 0;
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    sb_clear(task);
    for (size_t i = 0; i < n; ) {
        if (line[i] == 0x1b) {               /* ESC: skip a CSI/SS3 escape sequence */
            i++;
            if (i < n && (line[i] == '[' || line[i] == 'O')) {
                i++;
                while (i < n && !(line[i] >= 0x40 && line[i] <= 0x7e)) i++;
                if (i < n) i++;              /* consume the final byte */
            }
            continue;
        }
        if ((unsigned char)line[i] >= 0x20) sb_putc(task, line[i]);
        i++;
    }
    return 1;
}

#ifndef _WIN32
/* Reprint buf[pos..len] then move the cursor back so it sits at `pos` again. Used
 * after an insert/delete so the tail of the line is redrawn in place without needing
 * to know the prompt's column. `pad` erases one trailing char (after a delete). */
static void redraw_tail(const char *buf, size_t len, size_t pos, int pad) {
    fwrite(buf + pos, 1, len - pos, stdout);
    if (pad) fputc(' ', stdout);
    size_t back = (len - pos) + (pad ? 1 : 0);
    for (size_t i = 0; i < back; i++) fputc('\b', stdout);
    fflush(stdout);
}

/* Raw-mode line editor for an interactive POSIX terminal. Reads byte-by-byte with
 * echo off and implements in-line editing: printable chars insert at the cursor;
 * Left/Right move it; Home/End (and Ctrl+A/Ctrl+E) jump to the ends; Backspace and
 * Delete remove a char; Enter submits; Ctrl+C cancels the line; Ctrl+D on an empty
 * line is EOF. Arrow/escape sequences are parsed (not echoed as `^[[D` garbage).
 * Returns 1 = line ready, 0 = EOF/quit, -1 = not a TTY (caller falls back to cooked). */
static int read_line_raw(strbuf *task) {
    struct termios orig, raw;
    if (tcgetattr(STDIN_FILENO, &orig) != 0) return -1;
    raw = orig;
    raw.c_lflag &= ~((tcflag_t)(ICANON | ECHO | ISIG));  /* we handle echo + control */
    raw.c_iflag &= ~((tcflag_t)(IXON | ICRNL));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    char buf[4096];
    size_t len = 0, pos = 0;   /* content length and cursor index (0..len) */
    int ret = 1;
    for (;;) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1) { ret = len ? 1 : 0; break; }

        if (c == '\r' || c == '\n') { fputc('\n', stdout); fflush(stdout); break; }
        if (c == 3) { fputs("^C\n", stdout); fflush(stdout); len = pos = 0; break; } /* Ctrl+C */
        if (c == 4) { if (len == 0) { ret = 0; break; } continue; }                  /* Ctrl+D */
        if (c == 1) { while (pos > 0) { fputc('\b', stdout); pos--; } fflush(stdout); continue; } /* ^A home */
        if (c == 5) { while (pos < len) fputc(buf[pos++], stdout); fflush(stdout); continue; }    /* ^E end */
        if (c == 127 || c == 8) {            /* Backspace: delete left of cursor */
            if (pos > 0) {
                memmove(buf + pos - 1, buf + pos, len - pos);
                pos--; len--;
                fputc('\b', stdout);
                redraw_tail(buf, len, pos, 1);
            }
            continue;
        }
        if (c == 27) {                       /* escape sequence (arrows, Home/End, Del) */
            struct termios t = raw;
            t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 1;     /* short wait so a lone ESC won't block */
            tcsetattr(STDIN_FILENO, TCSANOW, &t);
            unsigned char a = 0, b = 0;
            if (read(STDIN_FILENO, &a, 1) == 1 && (a == '[' || a == 'O') &&
                read(STDIN_FILENO, &b, 1) == 1) {
                if (b == 'D') { if (pos > 0) { pos--; fputc('\b', stdout); fflush(stdout); } }       /* Left */
                else if (b == 'C') { if (pos < len) { fputc(buf[pos++], stdout); fflush(stdout); } } /* Right */
                else if (b == 'H') { while (pos > 0) { fputc('\b', stdout); pos--; } fflush(stdout); }/* Home */
                else if (b == 'F') { while (pos < len) fputc(buf[pos++], stdout); fflush(stdout); }   /* End */
                else if (b >= '0' && b <= '9') {     /* extended: ESC [ N ~ */
                    unsigned char d = 0;
                    while (read(STDIN_FILENO, &d, 1) == 1 && d != '~' &&
                           !(d >= 0x40 && d <= 0x7e)) { /* consume any params */ }
                    if (d == '~') {
                        if (b == '3') {              /* Delete: remove char at cursor */
                            if (pos < len) { memmove(buf + pos, buf + pos + 1, len - pos - 1);
                                             len--; redraw_tail(buf, len, pos, 1); }
                        } else if (b == '1' || b == '7') { while (pos > 0) { fputc('\b', stdout); pos--; } fflush(stdout); }
                        else if (b == '4' || b == '8') { while (pos < len) fputc(buf[pos++], stdout); fflush(stdout); }
                    }
                }
                /* Up/Down ('A'/'B') and anything else: ignored, not echoed */
            }
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
            continue;
        }
        if (c >= 0x20 && c < 0x7f) {          /* printable: insert at the cursor */
            if (len < sizeof buf - 1) {
                memmove(buf + pos + 1, buf + pos, len - pos);
                buf[pos] = (char)c;
                len++;
                fputc((int)c, stdout);
                pos++;
                redraw_tail(buf, len, pos, 0);
            }
            continue;
        }
        /* other control bytes are ignored */
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    sb_clear(task);
    sb_append_n(task, buf, len);
    return ret;
}
#endif /* !_WIN32 */

static int read_line(strbuf *task) {
#ifndef _WIN32
    if (isatty(STDIN_FILENO)) {
        int r = read_line_raw(task);
        if (r >= 0) return r;   /* -1 => not a TTY after all; fall back */
    }
#endif
    return read_line_cooked(task);
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
                                 infer_ctx **backend_slot,
                                 const session_stats *stats, const ui *u) {
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
            "  /stats           show session token + throughput stats\n"
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

    if (strcmp(verb, "/stats") == 0) {
        stats_render(u, stats);
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
    int color_force = 0;                 /* --color forces colour even when not a TTY (pipes) */

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
        } else if (strcmp(a, "--color") == 0 || strcmp(a, "--colour") == 0) {
            color_force = 1;
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
        } else if (strcmp(a, "--tty-diag") == 0) {
            /* Diagnose why the raw-mode line editor may not be engaging. */
#ifndef _WIN32
            fprintf(stdout, "isatty(stdin)=%d isatty(stdout)=%d\n",
                    isatty(STDIN_FILENO), isatty(STDOUT_FILENO));
            struct termios o, r, chk;
            int g = tcgetattr(STDIN_FILENO, &o);
            fprintf(stdout, "tcgetattr=%d (errno=%d)\n", g, g ? errno : 0);
            if (g == 0) {
                r = o;
                r.c_lflag &= ~((tcflag_t)(ICANON | ECHO | ISIG));
                r.c_cc[VMIN] = 1; r.c_cc[VTIME] = 0;
                int s = tcsetattr(STDIN_FILENO, TCSANOW, &r);
                int rc = errno;
                tcgetattr(STDIN_FILENO, &chk);
                fprintf(stdout, "tcsetattr(raw)=%d (errno=%d) -> after: ICANON=%d ECHO=%d ISIG=%d\n",
                        s, s ? rc : 0,
                        (chk.c_lflag & ICANON) != 0, (chk.c_lflag & ECHO) != 0,
                        (chk.c_lflag & ISIG) != 0);
                tcsetattr(STDIN_FILENO, TCSANOW, &o);  /* restore */
            }
#else
            fprintf(stdout, "tty-diag: not applicable on this build\n");
#endif
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

    /* Colour only on an interactive POSIX terminal (or when --color forces it, e.g.
     * piping to a colour-aware pager). The XP console does not interpret ANSI, so off. */
    int use_color = color_force || (want_color && plat_isatty_stdout());
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

    ui u = { stdout, logf, use_color };
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
    session_stats stats = {0};

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
        print_turn_stats(&u, plat_time_sec() - t0, &session);
        free(msg);
    } else {
        /* Interactive conversation. Clear any leftover mouse-reporting mode a prior
         * program may have left on, so the wheel scrolls the terminal's scrollback
         * instead of emitting escape sequences into the prompt (TTY + POSIX only). */
#ifndef _WIN32
        if (plat_isatty_stdout()) {
            /* Disable mouse reporting (1000/1002/1003/1006) and alternate-scroll (1007,
             * which turns the wheel into arrow-key escapes). NOT 1049 (leave alt-screen):
             * emitting it when not in the alt buffer makes some terminals restore a stale
             * cursor and draw over prior output. */
            fputs("\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?1007l", stdout);
            fflush(stdout);
        }
#endif
        fprintf(stdout,
            "%sANACHRON " ANACHRON_VERSION "%s - local agentic coding harness\n"
            "  %sbackend:%s %s\n"
            "  %ssandbox:%s %s\n"
            "  %sgrammar:%s %s\n"
            "  %sverify :%s %s\n"
            "  %scontext:%s %s\n"
            "  %sconfig :%s %s\n"
            "Type a task and press enter (use @path to attach a file). /help for commands.\n",
            cc(&u, A_TITLE), cc(&u, A_RESET),
            cc(&u, A_DIM), cc(&u, A_RESET), model ? model : "stub (no model)",
            cc(&u, A_DIM), cc(&u, A_RESET), sandbox,
            cc(&u, A_DIM), cc(&u, A_RESET), grammar ? grammar_path : "(none)",
            cc(&u, A_DIM), cc(&u, A_RESET),
            !verify_writes ? "off" : (verify_cc ? "on (balance + cc syntax check)" : "on (balance check)"),
            cc(&u, A_DIM), cc(&u, A_RESET), project_context ? "AGENTS.md loaded" : "(no AGENTS.md)",
            cc(&u, A_DIM), cc(&u, A_RESET), cfg_path ? cfg_path : "(none)");
        for (;;) {
            plat_flush_input();   /* drop scroll/keystroke bytes from the last turn */
            fprintf(stdout, "\n%syou>%s ", cc(&u, A_PROMPT), cc(&u, A_RESET));
            fflush(stdout);
            if (!read_line(&task)) { fprintf(stdout, "\n"); break; }
            if (task.len == 0) continue;
            cmd_result cr = handle_command(sb_cstr(&task), &session, sandbox, ctx, &backend, &stats, &u);
            if (cr == CMD_QUIT) break;
            if (cr == CMD_HANDLED) continue;
            char *msg = expand_mentions(sb_cstr(&task), sandbox);
            double t0 = plat_time_sec();
            interrupt_clear();
            plat_set_echo(0);   /* keys typed while generating won't echo as garbage;
                                   Ctrl+C still interrupts (signals stay on) */
            rc = agent_session_run_turn(&session, msg);
            plat_set_echo(1);
            double elapsed = plat_time_sec() - t0;
            stats_record(&stats, &session, elapsed);
            print_turn_stats(&u, elapsed, &session);
            if (interrupt_pending()) fprintf(stdout, "%s(interrupted)%s\n", cc(&u, A_NOTE), cc(&u, A_RESET));
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
