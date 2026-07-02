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
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   /* SetConsoleTextAttribute: colour on real XP (no ANSI/VT there) */
#endif

#define DISPLAY_MAX_LINES 10   /* tool results are capped in the transcript (model gets all) */

/* Semantic colour roles. One table maps each role to a 16-colour ANSI index (0-15,
 * -1 = terminal default); both backends derive from it — SGR escapes on a POSIX/antiX
 * terminal, SetConsoleTextAttribute on the Windows XP console (which has NO ANSI/VT).
 * This is what finally gives colour on real XP, where raw escapes render as garbage. */
typedef enum {
    CR_DEFAULT = 0, CR_ACCENT, CR_TITLE, CR_PROMPT, CR_TOOL, CR_OK, CR_ADD,
    CR_ERR, CR_REMOVE, CR_WARN, CR_MUTED, CR_FINAL, CR_USER, CR_COUNT
} ui_role;

/* ANSI index per role: 0-7 normal, 8-15 bright (8 = grey). -1 inherits the terminal. */
static const int ROLE_ANSI[CR_COUNT] = {
    /*CR_DEFAULT*/ -1, /*CR_ACCENT*/ 11, /*CR_TITLE*/ 14, /*CR_PROMPT*/ 10,
    /*CR_TOOL*/ 6, /*CR_OK*/ 10, /*CR_ADD*/ 10, /*CR_ERR*/ 9, /*CR_REMOVE*/ 9,
    /*CR_WARN*/ 3, /*CR_MUTED*/ 8, /*CR_FINAL*/ 13, /*CR_USER*/ 12,
};

/* Streaming-render states (see ui_token). The raw model token stream is turned into
 * a readable view live: plain replies pass through indented; a write_file/edit tool
 * call's `content`/`new` value is shown with real newlines and indentation while the
 * surrounding JSON wrapper is suppressed. */
enum { SR_DECIDE, SR_PLAIN, SR_TOOL, SR_VALWAIT, SR_CONTENT, SR_AFTER };

typedef struct {
    FILE *out; FILE *log;
    int  color;          /* colour enabled at all */
    int  unicode;        /* terminal renders box/block glyphs (POSIX yes, XP raster no) */
    int  win;            /* using the Win32 Console-API colour backend */
    void *hcon;          /* Win32 console handle (HANDLE), NULL otherwise */
    unsigned short attr0;/* Win32 original text attributes, restored on ui_reset/exit */
    int  interactive;    /* a human is at the keyboard (REPL + tty stdin) — gate applies */
    int  yolo;           /* --yolo / ANACHRON_YOLO: skip the permission gate */
    int  sr;             /* stream state */
    char sr_acc[64];     /* DECIDE: accumulate until plain-vs-tool is clear */
    int  sr_acc_n;
    char sr_win[12];     /* TOOL: rolling window to spot the `"content"`/`"new"` key */
    int  sr_win_n;
    int  sr_esc;         /* CONTENT: previous char was a backslash */
    int  sr_bol;         /* at beginning of a line (drives indentation) */
    int  sr_began;       /* emitted anything this generation (drives the leading blank line) */
    int  sr_flush;       /* emitted whitespace: flush at the end of this piece (word pacing) */
    int  turn_labeled;   /* printed the once-per-turn "anachron" gutter label */
    char model_name[96]; /* model display name (basename, no .gguf) for the status band */
    int  ctx_total;      /* context window size, for the band's ctx % */
} ui;

#define SR_INDENT "  "

#ifdef _WIN32
/* Restore the console colour even when we never return to main() — a second Ctrl+C
 * force-quits (interrupt.c re-raises SIGINT to the default handler) and closing the
 * window skips normal cleanup, and a Win32 console attribute is a persistent, process-
 * global side effect. This handler runs for Ctrl+C / close / logoff / shutdown; it
 * restores the original attributes and returns FALSE so default handling still proceeds. */
static HANDLE g_hcon_restore = NULL;
static WORD   g_attr0_restore = 0;
static BOOL WINAPI ui_ctrl_handler(DWORD type) {
    (void)type;
    if (g_hcon_restore) SetConsoleTextAttribute(g_hcon_restore, g_attr0_restore);
    return FALSE;
}
#endif

/* Detect the console + capture its original attributes (Win32); decide unicode
 * capability. Called once at startup after u.color is decided. */
static void ui_console_init(ui *u) {
#ifdef _WIN32
    u->unicode = 0;                     /* XP default raster font can't render block/box glyphs */
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &csbi)) {
        u->hcon = (void *)h;
        u->attr0 = csbi.wAttributes;    /* the sentinel for CR_DEFAULT + restore-on-exit */
        u->win = 1;
        g_hcon_restore = h; g_attr0_restore = csbi.wAttributes;
        SetConsoleCtrlHandler(ui_ctrl_handler, TRUE);   /* restore colour on force-quit/close */
    } else {
        u->win = 0; u->color = 0;        /* redirected to a file/pipe: no console colour */
    }
#else
    u->unicode = 1;                     /* a POSIX terminal renders the block glyphs */
    u->win = 0;
#endif
}

/* Select a colour role for subsequent output. POSIX: emit an SGR escape. Win32: set
 * the console text attribute (flushing buffered text first so it keeps its old colour). */
static void ui_style(ui *u, ui_role role) {
    if (!u->color) return;
    int idx = ROLE_ANSI[role];
#ifdef _WIN32
    if (u->win) {
        WORD a = u->attr0;
        if (idx >= 0) {
            /* ANSI colour order -> Win32 FOREGROUND bits (R and B are swapped). */
            static const WORD fg[8] = {
                0, FOREGROUND_RED, FOREGROUND_GREEN, FOREGROUND_RED | FOREGROUND_GREEN,
                FOREGROUND_BLUE, FOREGROUND_RED | FOREGROUND_BLUE,
                FOREGROUND_GREEN | FOREGROUND_BLUE,
                FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE };
            a = (WORD)(fg[idx & 7] | (idx >= 8 ? FOREGROUND_INTENSITY : 0) | (u->attr0 & 0xF0));
        }
        fflush(u->out);
        SetConsoleTextAttribute((HANDLE)u->hcon, a);
    }
#else
    if (idx < 0) { fputs("\x1b[0m", u->out); return; }
    fprintf(u->out, "\x1b[%dm", idx < 8 ? 30 + idx : 90 + (idx - 8));
#endif
}

static void ui_reset(ui *u) {
    if (!u->color) return;
#ifdef _WIN32
    if (u->win) { fflush(u->out); SetConsoleTextAttribute((HANDLE)u->hcon, u->attr0); }
#else
    fputs("\x1b[0m", u->out);
#endif
}

/* Restore the console to how we found it (call on exit; also safe mid-run). */
static void ui_console_restore(ui *u) { ui_reset(u); }

/* Convenience: print `text` in one role then reset. */
static void ui_span(ui *u, ui_role role, const char *text) {
    ui_style(u, role); fputs(text, u->out); ui_reset(u);
}

/* Banner row: "  label value" with the label muted. */
static void banner_row(ui *u, const char *label, const char *value) {
    fputs("  ", u->out);
    ui_span(u, CR_MUTED, label);
    fprintf(u->out, " %s\n", value);
}

/* The once-per-turn gutter label: every reply block opens with an amber "anachron"
 * line, the counterpart of the blue "you>" prompt, so the transcript always says who
 * is speaking. Printed lazily before the turn's first visible output (text or tool). */
static void turn_label(ui *u) {
    fputc('\n', u->out);
    ui_style(u, CR_ACCENT);
    fputs("anachron", u->out);
    ui_reset(u);
    fputc('\n', u->out);
    u->turn_labeled = 1;
}

/* Open a block within the turn: the gutter label the first time, then just a
 * blank-line separator between blocks. Flushed so the label/separator shows at
 * once even though streamed text is paced to word boundaries. */
static void block_start(ui *u) {
    if (!u->turn_labeled) turn_label(u);
    else fputc('\n', u->out);
    fflush(u->out);
}

/* Permission gate: pause before a file write/edit or a shell command and ask the user.
 * The pending action's details were just printed by ui_tool_call. Returns 1 (allow) /
 * 0 (decline). Auto-allows when not interactive (one-shot / piped: the invocation is
 * the consent, and the sandbox is the boundary) or with --yolo. Default is NO — a bare
 * Enter, EOF (Ctrl-D), or any non-'y' answer declines; input is flushed first so a
 * stray keystroke typed during generation can't auto-approve. */
static int ui_confirm(const tool_call *c, void *ud) {
    ui *u = ud;
    if (u->yolo || !u->interactive) return 1;
    ui_style(u, CR_WARN);
    fputs(c->kind == TC_RUN_COMMAND ? "  run this command? [y/N] "
        : c->kind == TC_WRITE_FILE  ? "  write this file? [y/N] "
        :                             "  apply this edit? [y/N] ", u->out);
    ui_reset(u);
    fflush(u->out);

    plat_flush_input();     /* drop type-ahead so a stray Enter can't auto-approve */
    plat_set_echo(1);       /* show the answer as it is typed (echo is off during a turn) */
    char buf[16];
    char *r = fgets(buf, sizeof buf, stdin);
    plat_set_echo(0);
    int yes = r && (buf[0] == 'y' || buf[0] == 'Y');
    if (!yes) { ui_style(u, CR_MUTED); fputs("  declined.\n", u->out); ui_reset(u); }
    return yes;
}

/* Emit one already-decoded output char, indenting at the start of each line and
 * printing a leading blank line before the model's first output of the turn. */
static void sr_emit(ui *u, char c) {
    if (!u->sr_began) { block_start(u); u->sr_began = 1; }
    if (u->sr_bol && c != '\n') { fputs(SR_INDENT, u->out); u->sr_bol = 0; }
    fputc(c, u->out);
    u->sr_bol = (c == '\n');
    if (c == ' ' || c == '\n' || c == '\t') u->sr_flush = 1;
}

static int win_ends(const char *w, int n, const char *needle) {
    int nl = (int)strlen(needle);
    return n >= nl && memcmp(w + n - nl, needle, (size_t)nl) == 0;
}

/* Reset the streaming renderer at the start of each model generation. */
static void ui_stream_reset(ui *u) {
    u->sr = SR_DECIDE; u->sr_acc_n = 0; u->sr_win_n = 0;
    u->sr_esc = 0; u->sr_bol = 1; u->sr_began = 0; u->sr_flush = 0;
}

static void ui_iter_start(int iter, void *ud) { (void)iter; ui_stream_reset((ui *)ud); }

/* Debug log sink: append one line to the log file when --log/ANACHRON_LOG is set. */
static void ui_log(const char *text, void *ud) {
    ui *u = ud;
    if (!u->log) return;
    fprintf(u->log, "%s\n", text);
    fflush(u->log);
}

static void ui_token(const char *piece, void *ud) {
    ui *u = ud;
    for (const char *p = piece; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (u->sr) {
        case SR_DECIDE:
            if (u->sr_acc_n < (int)sizeof u->sr_acc - 1) u->sr_acc[u->sr_acc_n++] = (char)c;
            u->sr_acc[u->sr_acc_n] = '\0';
            if (strstr(u->sr_acc, "<tool_call>") || strstr(u->sr_acc, "\"name\"")) {
                /* a tool call — seed the key-matcher window from the tail we buffered */
                int W = (int)sizeof u->sr_win;
                int start = u->sr_acc_n > W ? u->sr_acc_n - W : 0;
                u->sr_win_n = u->sr_acc_n - start;
                memcpy(u->sr_win, u->sr_acc + start, (size_t)u->sr_win_n);
                u->sr = SR_TOOL;
            } else if (u->sr_acc_n >= (int)sizeof u->sr_acc - 1) {
                u->sr = SR_PLAIN;                 /* no tool marker in 63 chars — plain text */
                for (int i = 0; i < u->sr_acc_n; i++) sr_emit(u, u->sr_acc[i]);
            }
            break;
        case SR_PLAIN:
            sr_emit(u, (char)c);
            break;
        case SR_TOOL: {
            int W = (int)sizeof u->sr_win;
            if (u->sr_win_n < W) u->sr_win[u->sr_win_n++] = (char)c;
            else { memmove(u->sr_win, u->sr_win + 1, (size_t)(W - 1)); u->sr_win[W - 1] = (char)c; }
            if (win_ends(u->sr_win, u->sr_win_n, "\"content\""))
                u->sr = SR_VALWAIT;   /* render write_file's code; edits show via the diff */
            break;
        }
        case SR_VALWAIT:
            if (c == '"') u->sr = SR_CONTENT;     /* opening quote of the value */
            break;
        case SR_CONTENT:
            if (u->sr_esc) {
                char e = (c == 'n') ? '\n' : (c == 't') ? '\t' : (c == 'r') ? '\r' : (char)c;
                sr_emit(u, e);
                u->sr_esc = 0;
            } else if (c == '\\') { u->sr_esc = 1; }
            else if (c == '"') { u->sr = SR_AFTER; }   /* end of the value */
            else sr_emit(u, (char)c);
            break;
        case SR_AFTER:
            break;                                 /* suppress the JSON tail */
        }
    }
    /* Pace the stream to word boundaries: flush only when this piece emitted
     * whitespace. Words appear whole, and the XP console gets far fewer writes.
     * Anything still buffered lands with the next boundary or the block renderers'
     * own flushes (block_start / ui_message / ui_tool_call ... all flush). */
    if (u->sr_flush) { u->sr_flush = 0; fflush(u->out); }
}

/* A plain conversational reply: the text streamed via ui_token. Flush a short reply
 * that never left the DECIDE buffer, then close the line. */
static void ui_message(const char *text, void *ud) {
    ui *u = ud;
    (void)text;
    if (u->sr == SR_DECIDE)
        for (int i = 0; i < u->sr_acc_n; i++) sr_emit(u, u->sr_acc[i]);
    fputc('\n', u->out);
    fflush(u->out);
}

static void ui_tool_call(const tool_call *c, void *ud) {
    ui *u = ud;
    if (c->kind == TC_FINAL) return;   /* rendered by ui_final; don't emit a leading blank line */
    block_start(u);
    ui_style(u, CR_TOOL);
    switch (c->kind) {
        case TC_READ_FILE:   fprintf(u->out, "> read_file(%s)", c->path); break;
        case TC_WRITE_FILE:  fprintf(u->out, "> write_file(%s, %zu bytes)",
                                     c->path, strlen(c->content ? c->content : "")); break;
        case TC_LIST_DIR:    fprintf(u->out, "> list_dir(%s)", c->path); break;
        case TC_RUN_COMMAND: fprintf(u->out, "> run_command(%s)", c->cmd); break;
        case TC_EDIT:        fprintf(u->out, "> edit(%s)", c->path); break;
        case TC_SEARCH:      fprintf(u->out, "> search(%s)", c->pattern ? c->pattern : ""); break;
        case TC_GLOB:        fprintf(u->out, "> glob(%s)", c->pattern ? c->pattern : ""); break;
        case TC_PLAN:        fputs("> plan:", u->out); ui_reset(u);
                             fprintf(u->out, "\n%s\n", c->plan ? c->plan : "");
                             fflush(u->out); return;
        default:             ui_reset(u); fflush(u->out); return; /* final handled by ui_final */
    }
    ui_reset(u);
    fputc('\n', u->out);
    fflush(u->out);
}

/* Print an observation indented, truncated to a sane number of lines so a long
 * file dump doesn't bury the transcript (the model still got the full text). */
static void ui_tool_result(const char *obs, int ok, void *ud) {
    ui *u = ud;
    if (ok) { ui_style(u, CR_MUTED); fputs("  result:", u->out); }
    else    { ui_style(u, CR_ERR);   fputs("  result (error):", u->out); }
    ui_reset(u);
    fputc('\n', u->out);
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

/* The turn's answer. With the gutter label there is no "== final ==" banner any
 * more: the reply is simply the last block of the anachron turn, indented like
 * streamed text, with the status band closing the turn right after. */
static void ui_final(const char *message, void *ud) {
    ui *u = ud;
    block_start(u);
    const char *p = message;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        fprintf(u->out, SR_INDENT "%.*s\n", (int)len, p);
        if (!nl) break;
        p = nl + 1;
    }
    fflush(u->out);
}

static void ui_notice(const char *text, void *ud) {
    ui *u = ud;
    fputc('\n', u->out);
    ui_style(u, CR_WARN); fprintf(u->out, "[notice] %s", text); ui_reset(u);
    fputc('\n', u->out);
    fflush(u->out);
}

/* Diff shown to the user when an existing file is edited (not fed to the model). The
 * diff text is plain (no embedded escapes); we colour it per-line here so it works on
 * both backends: added lines green, removed red, hunk/headers muted. */
static void ui_diff(const char *diff, void *ud) {
    ui *u = ud;
    /* Count changed lines first, so the "Edited X:" header can carry a +N -M stat. */
    int add = 0, del = 0;
    for (const char *q = diff; *q; ) {
        if      (q[0] == '+') add++;
        else if (q[0] == '-') del++;
        const char *nl = strchr(q, '\n');
        if (!nl) break;
        q = nl + 1;
    }
    fputc('\n', u->out);
    const char *p = diff;
    int first = 1;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        ui_role role = CR_DEFAULT;
        if      (p[0] == '+') role = CR_ADD;
        else if (p[0] == '-') role = CR_REMOVE;
        else if (p[0] != ' ') role = CR_MUTED; /* the "Edited X:" header; body lines start +/-/space */
        ui_style(u, role);
        fprintf(u->out, "%.*s", (int)len, p);
        ui_reset(u);
        if (first && (add || del)) {           /* "Edited x.c:" -> "Edited x.c: +3 -1" */
            fputc(' ', u->out);
            ui_style(u, CR_ADD);    fprintf(u->out, "+%d", add); ui_reset(u);
            fputc(' ', u->out);
            ui_style(u, CR_REMOVE); fprintf(u->out, "-%d", del); ui_reset(u);
        }
        first = 0;
        fputc('\n', u->out);
        if (!nl) break;
        p = nl + 1;
    }
    fflush(u->out);
}

/* Format a duration: "12.4s" under a minute, "12m34s" above (M170 turns run long). */
static void fmt_dur(char *buf, size_t n, double secs) {
    if (secs < 60.0) snprintf(buf, n, "%.1fs", secs);
    else snprintf(buf, n, "%dm%02ds", (int)(secs / 60.0), (int)secs % 60);
}

/* End-of-turn status band: one muted line with the model, how full the context
 * window is, tokens generated, and wall time. The ctx figure is the final prompt of
 * the turn as a share of the window; it turns amber at 80% and earns a hint at 90%,
 * because a 4096-token window fills sooner than it feels. Middle-dot separators on
 * a terminal that renders them, ASCII pipes on the XP console. */
static void print_turn_stats(ui *u, double secs, const agent_session *s) {
    int fancy = u->color && u->unicode;
    const char *sep  = fancy ? " \xc2\xb7 " : " | ";          /* " · " */
    const char *dash = fancy ? "\xe2\x94\x80\xe2\x94\x80" : "--"; /* "──" */
    char dur[32];
    fmt_dur(dur, sizeof dur, secs);
    int pct = -1;
    if (u->ctx_total > 0 && s->turn_prompt_tokens > 0) {
        long p = (long)s->turn_prompt_tokens * 100 / u->ctx_total;
        pct = p > 100 ? 100 : (int)p;
    }
    fputc('\n', u->out);
    ui_style(u, CR_MUTED);
    fprintf(u->out, "%s %s", dash, u->model_name[0] ? u->model_name : "no model");
    if (pct >= 0) {
        fprintf(u->out, "%sctx ", sep);
        if (pct >= 80) ui_style(u, CR_WARN);
        fprintf(u->out, "%d%%", pct);
        if (pct >= 80) ui_style(u, CR_MUTED);
    }
    if (s->turn_completion_tokens > 0)
        fprintf(u->out, "%s%d tok", sep, s->turn_completion_tokens);
    fprintf(u->out, "%s%s", sep, dur);
    ui_reset(u);
    fputc('\n', u->out);
    if (pct >= 90) {
        ui_style(u, CR_WARN);
        fputs("   context nearly full - /new starts a fresh conversation\n", u->out);
        ui_reset(u);
    }
    fflush(u->out);
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

/* Print "  label            " in muted, leaving the cursor for the value. */
static void stat_label(ui *u, const char *label) {
    ui_style(u, CR_MUTED);
    fprintf(u->out, "  %-16s", label);
    ui_reset(u);
    fputc(' ', u->out);
}

static void stats_render(ui *u, const session_stats *st) {
    fputc('\n', u->out);
    ui_span(u, CR_TITLE, "Session stats"); fputc('\n', u->out);
    if (st->turns == 0) { ui_style(u, CR_MUTED); fputs("  no turns yet\n", u->out); ui_reset(u); return; }

    stat_label(u, "turns");            fprintf(u->out, "%d\n", st->turns);
    stat_label(u, "generated tokens");
    ui_style(u, CR_OK); fprintf(u->out, "%ld", st->gen_tokens); ui_reset(u);
    fprintf(u->out, "  (avg %ld/turn)\n", st->gen_tokens / st->turns);
    stat_label(u, "context tokens");   fprintf(u->out, "%ld  (processed)\n", st->ctx_tokens);
    stat_label(u, "wall time");        fprintf(u->out, "%.1fs\n", st->seconds);
    stat_label(u, "throughput");
    if (st->seconds >= 0.05) {   /* avoid a nonsense rate when timing is ~0 (e.g. the stub) */
        ui_style(u, CR_OK); fprintf(u->out, "%.2f tok/s", (double)st->gen_tokens / st->seconds); ui_reset(u);
        fputs("  (generated / wall-clock)\n", u->out);
    } else {
        fputs("n/a (too fast to measure)\n", u->out);
    }

    int n = st->hist_n < STAT_HIST ? st->hist_n : STAT_HIST;
    int start = st->hist_n < STAT_HIST ? 0 : st->hist_n % STAT_HIST;
    int mx = 1;
    for (int k = 0; k < n; k++) { int v = st->hist[(start + k) % STAT_HIST]; if (v > mx) mx = v; }
    stat_label(u, "gen tokens/turn");
    if (u->color && u->unicode) {   /* block glyphs only where the terminal renders them */
        ui_style(u, CR_OK);
        for (int k = 0; k < n; k++) {
            int v = st->hist[(start + k) % STAT_HIST];
            int lvl = v * 7 / mx; if (lvl < 0) lvl = 0; if (lvl > 7) lvl = 7;
            fputs(SPARK[lvl], u->out);
        }
        ui_reset(u);
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
        "  --color/--no-color  force colour on/off (default: on for an interactive TTY)\n"
        "  --yolo              skip the y/N permission gate before file writes / commands\n"
        "  --lean              terse system prompt: ~2.7x faster first turn on slow hardware\n"
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

/* '!' shell escape: run the rest of the line as a shell command in the sandbox,
 * no model involved. The user typed it, so the command is its own consent — the
 * [y/N] gate is for model-initiated commands. Output renders like a tool result
 * (indented, capped), plus the exit code when it is non-zero. */
static void run_shell_escape(ui *u, const char *cmd, const char *sandbox) {
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (!*cmd) {
        fprintf(u->out, "usage: !<command>   (runs in the sandbox, without the model)\n");
        return;
    }
    char *out = NULL; size_t olen = 0; int code = -1;
    if (plat_run_command(cmd, sandbox, &out, &olen, &code) != 0) {
        ui_style(u, CR_ERR); fputs("  could not run the command\n", u->out); ui_reset(u);
        free(out);
        return;
    }
    ui_tool_result((out && *out) ? out : "(no output)", code == 0, u);
    if (code != 0) {
        ui_style(u, CR_MUTED); fprintf(u->out, "  exit code %d\n", code); ui_reset(u);
    }
    free(out);
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

/* Command history for Up/Down recall. Session-scoped, oldest..newest. */
#define HIST_MAX 128
static char  *g_hist[HIST_MAX];
static int    g_hist_count = 0;

static void hist_push(const char *line) {
    if (!line || !*line) return;                          /* skip blanks */
    if (g_hist_count > 0 && strcmp(g_hist[g_hist_count - 1], line) == 0) return; /* dedup */
    if (g_hist_count == HIST_MAX) {                       /* ring: drop oldest */
        free(g_hist[0]);
        memmove(g_hist, g_hist + 1, (HIST_MAX - 1) * sizeof *g_hist);
        g_hist_count--;
    }
    g_hist[g_hist_count++] = xstrdup(line);
}

/* Move the cursor to the start of the input (just after the prompt) and erase the
 * old input to end of line, so a recalled history entry can be drawn cleanly. */
static void clear_input_line(size_t pos) {
    for (size_t i = 0; i < pos; i++) fputc('\b', stdout);
    fputs("\x1b[K", stdout);   /* erase to end of line */
}

/* Raw-mode line editor for an interactive POSIX terminal. Reads byte-by-byte with
 * echo off and implements in-line editing: printable chars insert at the cursor;
 * Left/Right move it; Home/End (and Ctrl+A/Ctrl+E) jump to the ends; Up/Down recall
 * command history; Backspace and Delete remove a char; Enter submits; Ctrl+C cancels
 * the line; Ctrl+D on an empty line is EOF. Escape sequences are parsed, not echoed.
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
    int browse = g_hist_count; /* history index; == count means "not browsing" */
    char saved[4096];          /* in-progress line, stashed when browsing up */
    size_t saved_len = 0;
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
                else if (b == 'A') {                 /* Up: recall older history */
                    if (browse > 0) {
                        if (browse == g_hist_count) { memcpy(saved, buf, len); saved_len = len; }
                        browse--;
                        const char *h = g_hist[browse];
                        size_t hl = strlen(h); if (hl >= sizeof buf) hl = sizeof buf - 1;
                        clear_input_line(pos);
                        memcpy(buf, h, hl); len = pos = hl;
                        fwrite(buf, 1, len, stdout); fflush(stdout);
                    }
                }
                else if (b == 'B') {                 /* Down: newer history / in-progress line */
                    if (browse < g_hist_count) {
                        browse++;
                        const char *src = (browse == g_hist_count) ? saved : g_hist[browse];
                        size_t sl = (browse == g_hist_count) ? saved_len : strlen(src);
                        if (sl >= sizeof buf) sl = sizeof buf - 1;
                        clear_input_line(pos);
                        memcpy(buf, src, sl); len = pos = sl;
                        fwrite(buf, 1, len, stdout); fflush(stdout);
                    }
                }
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
                /* anything else: ignored, not echoed */
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
    if (ret == 1 && len > 0) hist_push(sb_cstr(task));   /* remember submitted lines */
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

/* Strip a trailing newline / carriage return from a fgets'd line, in place. */
static void chomp(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

static int cmp_cstr(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Does the name end in ".gguf" (case-insensitive)? Our practical "is it a model" test. */
static int ends_gguf(const char *nm) {
    size_t L = strlen(nm);
    if (L < 5) return 0;
    const char *e = nm + L - 5;
    return e[0] == '.' && (e[1] | 32) == 'g' && (e[2] | 32) == 'g'
                       && (e[3] | 32) == 'u' && (e[4] | 32) == 'f';
}

/* Where to look for models: a ./models folder if it exists, else the current folder. */
static const char *model_dir(void) {
    plat_dirlist dl;
    if (plat_list_dir("models", &dl) == 0) { plat_dirlist_free(&dl); return "models"; }
    return ".";
}

/* Collect *.gguf paths from `dir` (prefixed with "dir/" unless dir is "."). Returns the
 * count; *out is a malloc'd array of malloc'd path strings (caller frees each + the array). */
static int list_gguf(const char *dir, char ***out) {
    plat_dirlist dl;
    *out = NULL;
    if (plat_list_dir(dir, &dl) != 0) return 0;
    char **v = NULL; int n = 0;
    for (size_t i = 0; i < dl.count; i++) {
        if (dl.is_dir[i] || !ends_gguf(dl.names[i])) continue;
        char path[1024];
        int r = (strcmp(dir, ".") == 0)
                ? snprintf(path, sizeof path, "%s", dl.names[i])
                : snprintf(path, sizeof path, "%s/%s", dir, dl.names[i]);
        if (r < 0 || (size_t)r >= sizeof path) continue;   /* skip a name too long to hold */
        char **nv = realloc(v, (size_t)(n + 1) * sizeof *v);
        if (!nv) break;
        v = nv; v[n++] = xstrdup(path);
    }
    plat_dirlist_free(&dl);
    if (n > 1) qsort(v, (size_t)n, sizeof *v, cmp_cstr);   /* alphabetical: stable numbering */
    *out = v;
    return n;
}

/* List the models found nearby and let the user pick one by number (or type a path).
 * Returns a malloc'd path, or NULL if the user entered nothing / EOF. Reads a plain line
 * (callers use this only where stdin is in cooked mode). */
static char *pick_model(void) {
    const char *dir = model_dir();
    const char *where = strcmp(dir, ".") == 0 ? "this folder" : dir;
    char **v; int n = list_gguf(dir, &v);
    if (n > 0) {
        fprintf(stdout, "Models found in %s:\n", where);
        for (int i = 0; i < n; i++) fprintf(stdout, "  %d) %s\n", i + 1, v[i]);
        fputs("Choose a number, or type a .gguf path: ", stdout);
    } else {
        fprintf(stdout, "No .gguf models found in %s. Type a model path: ", where);
    }
    fflush(stdout);
    char buf[512]; char *pick = NULL;
    if (fgets(buf, sizeof buf, stdin)) {
        chomp(buf);
        if (buf[0]) {
            char *end; long k = strtol(buf, &end, 10);
            pick = (n > 0 && *end == '\0' && k >= 1 && k <= n) ? xstrdup(v[k - 1]) : xstrdup(buf);
        }
    }
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
    return pick;
}

/* Remember the model's display name for the status band: the path's basename with
 * a ".gguf" extension dropped. NULL means the stub backend. */
static void ui_set_model(ui *u, const char *path) {
    const char *b = path ? path : "stub";
    for (const char *p = b; *p; p++)
        if (*p == '/' || *p == '\\') b = p + 1;
    size_t n = strlen(b);
    if (ends_gguf(b)) n -= 5;
    if (n >= sizeof u->model_name) n = sizeof u->model_name - 1;
    memcpy(u->model_name, b, n);
    u->model_name[n] = '\0';
}

/* One /help row: the command in the tool colour, the description plain. */
static void help_row(ui *u, const char *cmd, const char *desc) {
    fputs("  ", u->out);
    ui_style(u, CR_TOOL);
    fprintf(u->out, "%-15s", cmd);
    ui_reset(u);
    fprintf(u->out, "  %s\n", desc);
}

/* Handle a line beginning with '/'. Returns CMD_NOT_A_COMMAND if the line is not
 * a recognized command and should be sent to the model verbatim. `backend_slot`
 * and `ctx_tokens` let /model swap the inference backend in place. */
static cmd_result handle_command(const char *line, agent_session *s,
                                 const char *sandbox, int ctx_tokens,
                                 infer_ctx **backend_slot,
                                 const session_stats *stats, ui *u) {
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
        fputc('\n', u->out);
        ui_span(u, CR_TITLE, "conversation"); fputc('\n', u->out);
        help_row(u, "/new, /clear",  "start a fresh conversation (clears history)");
        help_row(u, "/undo",         "revert the last write/edit from its snapshot");
        help_row(u, "/save [name]",  "save this conversation (default name: last)");
        help_row(u, "/sessions",     "list saved conversations");
        help_row(u, "/resume <name>","load a saved conversation");
        ui_span(u, CR_TITLE, "session"); fputc('\n', u->out);
        help_row(u, "/model [path]", "switch models (no path: pick from a list)");
        help_row(u, "/stats",        "session token + throughput stats");
        help_row(u, "!<command>",    "run a shell command in the sandbox (no model)");
        help_row(u, "/help",         "show this help");
        help_row(u, "/quit, /exit",  "leave");
        ui_style(u, CR_MUTED);
        fputs("Anything else is sent to the model. @path attaches a file; a line ending\n"
              "in \\ continues on the next line; Ctrl+C interrupts a turn; on a [y/N]\n"
              "question, Enter means No.\n", u->out);
        ui_reset(u);
        return CMD_HANDLED;
    }

    if (strcmp(verb, "/new") == 0 || strcmp(verb, "/clear") == 0) {
        agent_session_clear(s);
        fprintf(stdout, "(history cleared)\n");
        return CMD_HANDLED;
    }

    if (strcmp(verb, "/model") == 0) {
        /* With a path, switch to it; with no arg, list the models nearby and pick one. */
        char *chosen = arg[0] ? xstrdup(arg) : pick_model();
        if (!chosen) return CMD_HANDLED;   /* nothing chosen */
        infer_ctx *nb = infer_init(chosen, ctx_tokens);
        if (!nb) {
            fprintf(stdout, "could not load model %s (keeping the current one)\n", chosen);
            free(chosen);
            return CMD_HANDLED;
        }
        infer_free(*backend_slot);
        *backend_slot = nb;
        s->cfg.infer = nb;          /* run_turn reads the backend from the session's cfg */
        ui_set_model(u, chosen);    /* keep the status band's name in step */
        fprintf(stdout, "switched to model %s\n", chosen);
        free(chosen);
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

/* True only for an explicit truthy env value (1 / true / yes / on, any case); every other
 * value — 0/false/no/off/empty/unset — is false, so a falsy spelling can't flip a gate on. */
static int env_truthy(const char *name) {
    const char *v = getenv(name);
    if (!v || !*v) return 0;
    char c = v[0];
    if (c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y') return 1;
    if ((c == 'o' || c == 'O') && (v[1] == 'n' || v[1] == 'N') && v[2] == '\0') return 1;
    return 0;
}

/* Read a boolean key; returns `dflt` if absent or not a bool. */
static int cfg_bool(const json_value *o, const char *key, int dflt) {
    const json_value *v = json_obj_get(o, key);
    return (v && v->type == JSON_BOOL) ? v->boolean : dflt;
}

/* Write a string as a JSON value body, escaping backslashes and quotes — so a Windows
 * path like C:\models\x.gguf lands in agent.json as valid JSON. */
static void json_put_escaped(FILE *f, const char *s) {
    for (; *s; s++) {
        if (*s == '\\' || *s == '"') fputc('\\', f);
        fputc(*s, f);
    }
}

/* First-run setup: when launched with no model (e.g. double-clicked), ask for the model,
 * sandbox and lean setting right here, and offer to save them to agent.json so the next
 * launch skips this. Returns 1 with the model and sandbox out-params (malloc'd) set, or
 * 0 to cancel. Reads plain lines (the raw-mode editor isn't active yet). */
static int run_setup(char **model_out, char **sandbox_out, int *lean_out) {
    char buf[512];
    fputs("\nANACHRON " ANACHRON_VERSION " - setup\n"
          "No model configured. Answer a few questions to start (Ctrl+C to quit).\n\n", stdout);

    char *model = NULL;
    for (;;) {
        char *p = pick_model();                       /* lists nearby .gguf, or asks for a path */
        if (!p) return 0;                             /* nothing entered / EOF -> cancel */
        FILE *t = fopen(p, "rb");
        if (!t) { fprintf(stdout, "  Can't open \"%s\" - try again.\n", p); free(p); continue; }
        fclose(t);
        model = p;
        break;
    }

    fputs("Working folder the agent may read/write [work]: ", stdout); fflush(stdout);
    buf[0] = '\0';
    if (fgets(buf, sizeof buf, stdin)) chomp(buf);
    char *sandbox = xstrdup(buf[0] ? buf : "work");
    plat_mkdir(sandbox);

    fputs("Faster first turn (lean prompt, slightly terser)? [y/N]: ", stdout); fflush(stdout);
    int lean = 0;
    if (fgets(buf, sizeof buf, stdin)) lean = (buf[0] == 'y' || buf[0] == 'Y');

    fputs("Save these to agent.json so setup is skipped next time? [Y/n]: ", stdout); fflush(stdout);
    if (fgets(buf, sizeof buf, stdin) && buf[0] != 'n' && buf[0] != 'N') {
        FILE *f = fopen("agent.json", "w");
        if (f) {
            fputs("{\n  \"model\": \"", f);   json_put_escaped(f, model);
            fputs("\",\n  \"sandbox\": \"", f); json_put_escaped(f, sandbox);
            fprintf(f, "\",\n  \"lean\": %s\n}\n", lean ? "true" : "false");
            fclose(f);
            fputs("  Saved agent.json.\n", stdout);
        } else {
            fputs("  (Could not write agent.json; continuing anyway.)\n", stdout);
        }
    }
    fputc('\n', stdout);
    *model_out = model; *sandbox_out = sandbox; *lean_out = lean;
    return 1;
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
    int yolo = env_truthy("ANACHRON_YOLO");   /* skip the permission gate (only 1/true/yes/on) */
    int lean = env_truthy("ANACHRON_LEAN");   /* terse prompt for a faster first turn */

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
        yolo          = cfg_bool(conf, "yolo", yolo);
        lean          = cfg_bool(conf, "lean", lean);
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
        } else if (strcmp(a, "--yolo") == 0 || strcmp(a, "--no-confirm") == 0) {
            yolo = 1;
        } else if (strcmp(a, "--lean") == 0) {
            lean = 1;
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

    /* Colour on an interactive terminal (or when --color forces it). This now works on
     * BOTH targets: ANSI escapes on a POSIX terminal, the Win32 Console API on the XP
     * console (which has no ANSI). ui_console_init() below confirms a real console and
     * turns colour back off when output is redirected to a file/pipe. */
    /* First-run setup when launched with no model (e.g. double-clicked on XP): ask for the
     * model / sandbox / lean here, then continue. Must run before load_project_context()
     * (which reads AGENTS.md from the sandbox) and before infer_init(). */
    if (!model && task.len == 0 && plat_isatty_stdin()) {
        char *sm = NULL, *ss = NULL;
        if (run_setup(&sm, &ss, &lean)) {
            free(owned_model);   owned_model   = sm; model   = sm;
            free(owned_sandbox); owned_sandbox = ss; sandbox = ss;
        }
    }

    int use_color = color_force || (want_color && plat_isatty_stdout());

    char *project_context = load_project_context(sandbox);

    if (!log_path) { const char *e = getenv("ANACHRON_LOG"); if (e && *e) log_path = e; }
    FILE *logf = log_path ? fopen(log_path, "a") : NULL;
    if (log_path && !logf)
        fprintf(stderr, "anachron: note: could not open log file %s\n", log_path);

    infer_ctx *backend = infer_init(model, ctx);
    if (!backend) {
        if (!model)
            fprintf(stderr, "anachron: no model. Pass --model PATH, or just launch it to set one up.\n");
        else
            fprintf(stderr, "anachron: failed to load the model (%s). Check it's a valid GGUF that fits.\n", model);
        /* Keep a double-clicked console window open long enough to read the error. */
        if (plat_isatty_stdin()) { fputs("\nPress Enter to exit.\n", stderr); (void)getchar(); }
        free(grammar); free(grammar_act); free(project_context);
        free(owned_model); free(owned_sandbox); free(owned_grammar); free(owned_log);
        if (logf) fclose(logf);
        sb_free(&task);
        return 1;
    }

    ui u = {0};
    u.out = stdout; u.log = logf; u.color = use_color;
    ui_console_init(&u);   /* capture console attrs (Win32); set unicode capability */
    u.yolo = yolo;
    ui_set_model(&u, model);   /* status-band display name ("stub" when no model) */
    u.ctx_total = ctx;
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
    cfg.lean = lean;   /* terse prompt (--lean / ANACHRON_LEAN); faster first-turn prefill */
    cfg.diff_colour = 0;   /* diff text stays plain; ui_diff() colours it per-line (both backends) */
    cfg.on_diff = ui_diff;
    cfg.on_log = logf ? ui_log : NULL;
    cfg.ud = &u;
    cfg.on_iter_start = ui_iter_start;   /* resets the streaming renderer each generation */
    cfg.on_token = ui_token;
    cfg.on_tool_call = ui_tool_call;
    cfg.confirm_tool = ui_confirm;   /* permission gate (interactive only; --yolo bypasses) */
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
        u.turn_labeled = 0;
        rc = agent_session_run_turn(&session, msg);
        print_turn_stats(&u, plat_time_sec() - t0, &session);
        free(msg);
    } else {
        u.interactive = plat_isatty_stdin();   /* enable the permission gate when a human is present */
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
        ui_style(&u, CR_TITLE);
        fputs("ANACHRON " ANACHRON_VERSION, stdout);
        ui_reset(&u);
        fputs(" - local agentic coding harness\n", stdout);
        banner_row(&u, "backend:", model ? model : "stub (no model)");
        banner_row(&u, "sandbox:", sandbox);
        banner_row(&u, "grammar:", grammar ? grammar_path : "(none)");
        banner_row(&u, "verify :", !verify_writes ? "off"
                   : (verify_cc ? "on (balance + cc syntax check)" : "on (balance check)"));
        banner_row(&u, "context:", project_context ? "AGENTS.md loaded" : "(no AGENTS.md)");
        banner_row(&u, "config :", cfg_path ? cfg_path : "(none)");
        fputs("Type a task and press enter (use @path to attach a file). /help for commands.\n", stdout);
        for (;;) {
            plat_flush_input();   /* drop scroll/keystroke bytes from the last turn */
            fputc('\n', stdout);
            /* Mode-as-colour: the prompt goes amber under --yolo as a standing reminder
             * that writes and commands will NOT ask for confirmation. */
            ui_style(&u, u.yolo ? CR_WARN : CR_USER); fputs("you>", stdout); ui_reset(&u);
            fputc(' ', stdout);
            fflush(stdout);
            if (!read_line(&task)) { fprintf(stdout, "\n"); break; }
            /* A trailing '\' continues the input on the next line (multiline task).
             * The backslash becomes a newline in the message. */
            while (task.len > 0 && sb_cstr(&task)[task.len - 1] == '\\') {
                ui_style(&u, CR_MUTED); fputs("...>", stdout); ui_reset(&u);
                fputc(' ', stdout);
                fflush(stdout);
                strbuf more; sb_init(&more);
                int r = read_line(&more);
                strbuf joined; sb_init(&joined);
                sb_append_n(&joined, sb_cstr(&task), task.len - 1);
                sb_putc(&joined, '\n');
                sb_append(&joined, sb_cstr(&more));
                sb_clear(&task);
                sb_append(&task, sb_cstr(&joined));
                sb_free(&joined);
                sb_free(&more);
                if (!r) break;   /* EOF mid-continuation: send what we have */
            }
            if (task.len == 0) continue;
            if (sb_cstr(&task)[0] == '!') {   /* shell escape: run it directly, no model */
                run_shell_escape(&u, sb_cstr(&task) + 1, sandbox);
                continue;
            }
            cmd_result cr = handle_command(sb_cstr(&task), &session, sandbox, ctx, &backend, &stats, &u);
            if (cr == CMD_QUIT) break;
            if (cr == CMD_HANDLED) continue;
            char *msg = expand_mentions(sb_cstr(&task), sandbox);
            double t0 = plat_time_sec();
            interrupt_clear();
            plat_set_echo(0);   /* keys typed while generating won't echo as garbage;
                                   Ctrl+C still interrupts (signals stay on) */
            u.turn_labeled = 0;
            rc = agent_session_run_turn(&session, msg);
            plat_set_echo(1);
            double elapsed = plat_time_sec() - t0;
            stats_record(&stats, &session, elapsed);
            print_turn_stats(&u, elapsed, &session);
            if (interrupt_pending()) { ui_style(&u, CR_WARN); fputs("(interrupted)", stdout); ui_reset(&u); fputc('\n', stdout); }
            interrupt_clear();
            free(msg);
        }
    }

    ui_console_restore(&u);   /* leave the console the colour we found it (Win32) */
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
