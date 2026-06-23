#include "tools.h"
#include "sandbox.h"
#include "strbuf.h"
#include "obsfmt.h"
#include "edit.h"
#include "glob.h"
#include "gitignore.h"
#include "diff.h"
#include "platform.h"
#include "verify.h"

#include <ctype.h>
#include <stdio.h>   /* remove() for reverting a rejected new file */
#include <stdlib.h>
#include <string.h>

/* Cap how much tool output we feed back into the (tiny) context window: line- AND
 * byte-bounded, so neither a huge file nor a few absurdly long lines blow it. */
#define MAX_OBS_LINES 200
#define MAX_OBS_BYTES 8192

static char *dup_cstr(const char *s) { return xstrdup(s); }

/* Append `text` to `sb`, capped (line + byte aware) with a truncation note. */
static void append_capped(strbuf *sb, const char *text, size_t len) {
    (void)len; /* text is nul-terminated; obs_capped re-measures */
    char *c = obs_capped(text, MAX_OBS_LINES, MAX_OBS_BYTES);
    sb_append(sb, c);
    free(c);
}

static char *err_obs(const char *fmt, const char *arg, int *ok) {
    *ok = 0;
    strbuf sb; sb_init(&sb);
    sb_appendf(&sb, fmt, arg);
    char *r = dup_cstr(sb_cstr(&sb));
    sb_free(&sb);
    return r;
}

static char *do_read_file(const tool_ctx *ctx, const char *rel, long offset, int *ok) {
    char *abs = NULL;
    if (sandbox_resolve(ctx->sandbox_root, rel, &abs) != 0)
        return err_obs("ERROR: path \"%s\" escapes the working directory", rel, ok);
    char *buf = NULL; size_t len = 0;
    int rc = plat_read_file(abs, &buf, &len);
    free(abs);
    if (rc != 0) return err_obs("ERROR: could not read \"%s\"", rel, ok);
    /* Paged window: a big file shows the first MAX_OBS_LINES with a "offset=N to
     * continue" footer instead of blowing the context. */
    char *r = obs_window(buf, offset < 0 ? 0 : (size_t)offset, MAX_OBS_LINES, MAX_OBS_BYTES);
    free(buf);
    *ok = 1;
    return r;
}

/* Only shell out the path to the compiler if it is made of safe characters, so
 * a model-chosen filename can't inject shell syntax into the verify command. */
static int path_shell_safe(const char *p) {
    for (; *p; p++) {
        char c = *p;
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '/' || c == '-'))
            return 0;
    }
    return 1;
}

/* The verify-on-write guardrail. Returns a malloc'd rejection reason, or NULL if
 * the content passes (or verification doesn't apply). *checked reports whether a
 * real check ran (so the success message can say so). Two layers:
 *   1. balance check  — pure C, runs for any brace language, catches the common
 *      truncation/mismatch breakage a small model produces.
 *   2. compiler syntax check — for C files, when a compiler is available; catches
 *      hard syntax errors the balance check can't (e.g. a missing ';'). Warnings
 *      are suppressed (-w) so valid partial code (a lone function, missing
 *      includes) is NOT rejected. */
static char *verify_write(const tool_ctx *ctx, const char *rel, const char *content, int *checked) {
    *checked = 0;
    if (!ctx->verify_writes || !verify_is_codeish(rel)) return NULL;
    *checked = 1;

    char *b = verify_balance(content);
    if (b) return b;

    if (verify_is_c(rel) && ctx->verify_cc && path_shell_safe(rel)) {
        /* -I. so headers in the sandbox resolve; a leading "./" so a path beginning
         * with '-' is treated as a filename, not a compiler flag (gcc has no '--'
         * end-of-options marker). */
        strbuf cmd; sb_init(&cmd);
        sb_appendf(&cmd, "%s -fsyntax-only -w -I. ./%s", ctx->verify_cc, rel);
        char *out = NULL; size_t ol = 0; int code = -1;
        int rc = plat_run_command(sb_cstr(&cmd), ctx->sandbox_root, &out, &ol, &code);
        sb_free(&cmd);
        char *err = NULL;
        /* Reject ONLY on a normal exit with a small code (a real syntax diagnostic).
         * code<=0 or >=126 means the check couldn't run (compiler missing = 126/127,
         * or killed by a signal = 13x) -> skip, never reject a valid write. And a
         * "No such file" fatal is a missing include we can't resolve from a single
         * file -> we can't syntax-check it, so skip rather than false-reject the
         * incremental project code a model legitimately writes one file at a time. */
        int ran_clean = (rc == 0 && code > 0 && code < 126);
        int missing_include = (out && strstr(out, "No such file or directory") != NULL);
        if (ran_clean && !missing_include) {
            strbuf e; sb_init(&e);
            sb_append(&e, "compiler syntax check failed:\n");
            if (out) {
                size_t cap = ol > 600 ? 600 : ol;
                sb_append_n(&e, out, cap);
                if (ol > cap) sb_append(&e, "\n...(truncated)");
            }
            err = dup_cstr(sb_cstr(&e));
            sb_free(&e);
        }
        free(out);
        return err;
    }
    return NULL;
}

/* Save the prior content of an overwritten file to a sibling "<file>.anbak" so the
 * operator can recover a valid-but-wrong overwrite. Best-effort; hidden from list_dir. */
static void snapshot(const char *abs, const char *prior, size_t prior_len) {
    strbuf bk; sb_init(&bk);
    sb_appendf(&bk, "%s.anbak", abs);
    plat_write_file(sb_cstr(&bk), prior, prior_len);
    sb_free(&bk);
}

/* Write `content` to `rel`, gated by the verify-on-write guardrail. On rejection the
 * write is reverted and the error returned; on success the prior content is
 * snapshotted. `verb` is "Wrote" / "Edited" for the message. Shared by write_file
 * and edit. */
static char *commit_write(const tool_ctx *ctx, const char *rel, const char *content,
                          const char *verb, int *ok) {
    char *abs = NULL;
    if (sandbox_resolve(ctx->sandbox_root, rel, &abs) != 0)
        return err_obs("ERROR: path \"%s\" escapes the working directory", rel, ok);

    char *prior = NULL; size_t prior_len = 0;
    int existed = (plat_read_file(abs, &prior, &prior_len) == 0);

    size_t len = strlen(content);
    if (plat_write_file(abs, content, len) != 0) {
        free(prior); free(abs);
        return err_obs("ERROR: could not write \"%s\"", rel, ok);
    }

    int checked = 0;
    char *verr = verify_write(ctx, rel, content, &checked);
    if (verr) {
        const char *revert_note;
        if (existed)
            revert_note = (plat_write_file(abs, prior, prior_len) == 0)
                ? "reverted to previous content"
                : "WARNING: revert failed - the file may hold the rejected content";
        else
            revert_note = (remove(abs) == 0) ? "not created" : "WARNING: cleanup failed";
        free(prior); free(abs);
        *ok = 0;
        strbuf sb; sb_init(&sb);
        sb_appendf(&sb, "ERROR: %s to %s REJECTED - %s\nThe file was left unchanged (%s). "
                        "Fix the problem and try again.", verb, rel, verr, revert_note);
        free(verr);
        char *r = dup_cstr(sb_cstr(&sb));
        sb_free(&sb);
        return r;
    }

    if (existed) snapshot(abs, prior, prior_len);
    /* Show the user (not the model) what changed in an existing file. */
    if (existed && ctx->on_diff) {
        strbuf d; sb_init(&d);
        sb_appendf(&d, "%s %s:\n", verb, rel);
        if (diff_unified(prior, content, &d, ctx->diff_colour))
            ctx->on_diff(sb_cstr(&d), ctx->ud);
        sb_free(&d);
    }
    free(prior); free(abs);
    *ok = 1;
    strbuf sb; sb_init(&sb);
    sb_appendf(&sb, "%s %zu bytes to %s%s", verb, len, rel, checked ? " (syntax OK)" : "");
    char *r = dup_cstr(sb_cstr(&sb));
    sb_free(&sb);
    return r;
}

static char *do_write_file(const tool_ctx *ctx, const char *rel, const char *content, int *ok) {
    return commit_write(ctx, rel, content, "Wrote", ok);
}

static char *do_edit(const tool_ctx *ctx, const char *rel, const char *old, const char *neu, int *ok) {
    char *abs = NULL;
    if (sandbox_resolve(ctx->sandbox_root, rel, &abs) != 0)
        return err_obs("ERROR: path \"%s\" escapes the working directory", rel, ok);
    char *buf = NULL; size_t len = 0;
    int rc = plat_read_file(abs, &buf, &len);
    free(abs);
    if (rc != 0) return err_obs("ERROR: could not read \"%s\" to edit", rel, ok);

    const char *eerr = NULL;
    char *updated = edit_apply(buf, old, neu, &eerr);
    free(buf);
    if (!updated) {
        *ok = 0;
        strbuf sb; sb_init(&sb);
        sb_appendf(&sb, "ERROR: edit of %s failed - %s", rel, eerr ? eerr : "no change made");
        char *r = dup_cstr(sb_cstr(&sb));
        sb_free(&sb);
        return r;
    }
    char *r = commit_write(ctx, rel, updated, "Edited", ok);
    free(updated);
    return r;
}

static char *do_list_dir(const tool_ctx *ctx, const char *rel, int *ok) {
    char *abs = NULL;
    if (sandbox_resolve(ctx->sandbox_root, rel, &abs) != 0)
        return err_obs("ERROR: path \"%s\" escapes the working directory", rel, ok);
    plat_dirlist dl;
    int rc = plat_list_dir(abs, &dl);
    free(abs);
    if (rc != 0) return err_obs("ERROR: could not list \"%s\"", rel, ok);
    strbuf sb; sb_init(&sb);
    if (dl.count == 0) {
        sb_append(&sb, "(empty directory)");
    } else {
        size_t shown = 0;
        for (size_t i = 0; i < dl.count; i++) {
            const char *nm = dl.names[i];
            size_t nl = strlen(nm);
            if (nl >= 6 && strcmp(nm + nl - 6, ".anbak") == 0) continue; /* hide edit backups */
            if (shown >= MAX_OBS_LINES) {
                size_t rem = 0; /* count only the remaining VISIBLE entries */
                for (size_t j = i; j < dl.count; j++) {
                    const char *n2 = dl.names[j]; size_t l2 = strlen(n2);
                    if (!(l2 >= 6 && strcmp(n2 + l2 - 6, ".anbak") == 0)) rem++;
                }
                sb_appendf(&sb, "... (%zu more entr%s not shown)", rem, rem == 1 ? "y" : "ies");
                break;
            }
            sb_append(&sb, nm);
            if (dl.is_dir[i]) sb_putc(&sb, '/');
            sb_putc(&sb, '\n');
            shown++;
        }
    }
    plat_dirlist_free(&dl);
    *ok = 1;
    char *r = dup_cstr(sb_cstr(&sb));
    sb_free(&sb);
    return r;
}

/* If `cmd` runs a local executable `./NAME` whose C/C++ source (NAME.c / NAME.cpp) is
 * newer than the binary — or the binary doesn't exist yet — fill *src (the source
 * filename) and *bin (NAME) and return 1. This catches the "edited the source then ran
 * the stale binary" trap: the run silently uses the old build. */
static int run_uses_stale_build(const tool_ctx *ctx, const char *cmd,
                                strbuf *src, strbuf *bin) {
    const char *p = cmd, *last = NULL;
    while ((p = strstr(p, "./")) != NULL) { last = p + 2; p += 2; }  /* last ./NAME */
    if (!last) return 0;
    char name[256]; size_t n = 0;
    for (const char *q = last; *q && n < sizeof name - 1; q++) {
        char c = *q;
        if (c == ' ' || c == '\t' || c == '&' || c == '|' || c == ';' ||
            c == '>' || c == '<' || c == '\n' || c == '"' || c == '\'') break;
        name[n++] = c;
    }
    name[n] = '\0';
    if (n == 0 || strchr(name, '.')) return 0;   /* ./prog is a binary; ./x.c is not */

    char *babs = NULL, *cabs = NULL, *ccabs = NULL;
    if (sandbox_resolve(ctx->sandbox_root, name, &babs) != 0) return 0;
    strbuf cs;  sb_init(&cs);  sb_appendf(&cs,  "%s.c",   name);
    strbuf ccs; sb_init(&ccs); sb_appendf(&ccs, "%s.cpp", name);
    sandbox_resolve(ctx->sandbox_root, sb_cstr(&cs),  &cabs);
    sandbox_resolve(ctx->sandbox_root, sb_cstr(&ccs), &ccabs);
    long bm = plat_mtime(babs);
    long cm  = cabs  ? plat_mtime(cabs)  : -1;
    long ccm = ccabs ? plat_mtime(ccabs) : -1;
    int stale = 0;
    if (cm >= 0 && (bm < 0 || cm > bm)) { stale = 1; sb_clear(src); sb_append(src, sb_cstr(&cs)); }
    else if (ccm >= 0 && (bm < 0 || ccm > bm)) { stale = 1; sb_clear(src); sb_append(src, sb_cstr(&ccs)); }
    if (stale) { sb_clear(bin); sb_append(bin, name); }
    free(babs); free(cabs); free(ccabs);
    sb_free(&cs); sb_free(&ccs);
    return stale;
}

static char *do_run_command(const tool_ctx *ctx, const char *cmd, int *ok) {
    char *out = NULL; size_t len = 0; int code = -1;
    int rc = plat_run_command(cmd, ctx->sandbox_root, &out, &len, &code);
    if (rc != 0) {
        free(out);
        return err_obs("ERROR: could not launch command \"%s\"", cmd, ok);
    }
    /* Two common self-inflicted failures on small models, each with a targeted hint:
     * (a) running a binary whose source was edited (stale build), and (b) compiling/
     * running a file that was never written. A fresh in-loop hint steers a small model
     * far better than a system-prompt rule it has already ignored. */
    strbuf stale_src; sb_init(&stale_src);
    strbuf stale_bin; sb_init(&stale_bin);
    int stale = run_uses_stale_build(ctx, cmd, &stale_src, &stale_bin);
    int missing_file = (code != 0 && out &&
                        strstr(out, "No such file or directory") != NULL);

    strbuf sb; sb_init(&sb);
    sb_appendf(&sb, "exit code %d\n", code);
    if (!out || len == 0)
        sb_append(&sb, "(command produced no output)"); /* clearer than an empty result */
    else
        append_capped(&sb, out, len);
    if (stale) {
        const char *cxx = strstr(sb_cstr(&stale_src), ".cpp") ? "c++" : "cc";
        sb_appendf(&sb, "\nHINT: %s is newer than the ./%s binary (or it isn't built yet) "
                        "- this ran the OLD build, so your latest edits are NOT reflected. "
                        "Recompile first: `%s %s -o %s`, then run ./%s again.",
                   sb_cstr(&stale_src), sb_cstr(&stale_bin), cxx,
                   sb_cstr(&stale_src), sb_cstr(&stale_bin), sb_cstr(&stale_bin));
    } else if (missing_file) {
        sb_append(&sb, "\nHINT: a file in that command does not exist yet. Create it "
                       "with write_file FIRST, then run the command. Never compile or "
                       "run a file you have not written.");
    }
    free(out);
    sb_free(&stale_src);
    sb_free(&stale_bin);
    *ok = (code == 0);
    char *r = dup_cstr(sb_cstr(&sb));
    sb_free(&sb);
    return r;
}

/* ---- discovery tools: a recursive tree walk shared by search and glob ---- */
#define WALK_BUDGET 5000   /* cap files visited so a huge tree can't hang the walk */

/* Always-skip set, applied on top of any .gitignore: dot-entries (.git/.env
 * secrets, the .anachron-sessions store), edit backups, and common build dirs. */
static int ignored_entry(const char *n) {
    if (n[0] == '.') return 1;
    size_t l = strlen(n);
    if (l >= 6 && strcmp(n + l - 6, ".anbak") == 0) return 1;
    static const char *const junk[] = { "node_modules", "build", "dist", "target", "obj", "bin", NULL };
    for (int i = 0; junk[i]; i++) if (strcmp(n, junk[i]) == 0) return 1;
    return 0;
}

typedef void (*walk_fn)(const char *rel, const char *abs, void *ud);

#define WALK_MAX_DEPTH 64  /* hard recursion cap so a directory symlink cycle can't run away */

static void walk(const char *abs_dir, const char *rel_prefix, walk_fn fn, void *ud,
                 const gitignore *gi, int *budget, int depth) {
    if (*budget <= 0 || depth > WALK_MAX_DEPTH) return;
    plat_dirlist dl;
    if (plat_list_dir(abs_dir, &dl) != 0) return;
    for (size_t i = 0; i < dl.count && *budget > 0; i++) {
        const char *n = dl.names[i];
        if (ignored_entry(n)) continue;
        /* Never follow symlinks: avoids cycles and reading outside the sandbox.
         * This also hides in-sandbox file symlinks from search/glob (a deliberate
         * security-first trade-off); read_file/write_file by explicit path still work. */
        if (dl.is_symlink[i]) continue;
        (*budget)--; /* charge every entry (dirs too) so cycles are bounded by the budget */
        strbuf rl; sb_init(&rl);
        if (*rel_prefix) sb_appendf(&rl, "%s/%s", rel_prefix, n); else sb_append(&rl, n);
        if (gitignore_match(gi, sb_cstr(&rl), dl.is_dir[i])) { sb_free(&rl); continue; }
        strbuf ab; sb_init(&ab); sb_appendf(&ab, "%s/%s", abs_dir, n);
        if (dl.is_dir[i]) walk(sb_cstr(&ab), sb_cstr(&rl), fn, ud, gi, budget, depth + 1);
        else fn(sb_cstr(&rl), sb_cstr(&ab), ud);
        sb_free(&ab); sb_free(&rl);
    }
    plat_dirlist_free(&dl);
}

/* Read <root>/.gitignore (if any) into a pattern set. Caller frees with
 * gitignore_free. NULL when the file is absent/empty (matcher then ignores none). */
static gitignore *load_root_gitignore(const char *root) {
    strbuf p; sb_init(&p);
    sb_appendf(&p, "%s/.gitignore", root);
    char *buf = NULL; size_t len = 0;
    gitignore *gi = NULL;
    if (plat_read_file(sb_cstr(&p), &buf, &len) == 0) {
        gi = gitignore_parse(buf);
        free(buf);
    }
    sb_free(&p);
    return gi;
}

static const char *base_name(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}

typedef struct { const char *pat; strbuf *out; int count; } glob_state;
static void glob_visit(const char *rel, const char *abs, void *ud) {
    (void)abs;
    glob_state *g = ud;
    if (g->count >= MAX_OBS_LINES) return;
    if (glob_match(g->pat, base_name(rel))) {
        sb_append(g->out, rel); sb_putc(g->out, '\n'); g->count++;
    }
}

static char *do_glob(const tool_ctx *ctx, const char *pattern, int *ok) {
    strbuf res; sb_init(&res);
    glob_state g = { pattern, &res, 0 };
    int budget = WALK_BUDGET;
    gitignore *gi = load_root_gitignore(ctx->sandbox_root);
    walk(ctx->sandbox_root, "", glob_visit, &g, gi, &budget, 0);
    gitignore_free(gi);
    char *r;
    if (g.count == 0) r = dup_cstr("(no files match)");
    else {
        if (g.count >= MAX_OBS_LINES) sb_append(&res, "... (more matches not shown; refine the pattern)");
        r = dup_cstr(sb_cstr(&res));
    }
    sb_free(&res);
    *ok = 1;
    return r;
}

typedef struct { const char *pat; strbuf *out; int hits; } search_state;
static int looks_binary(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) if (buf[i] == '\0') return 1; /* any NUL => treat as binary */
    return 0;
}
static void search_visit(const char *rel, const char *abs, void *ud) {
    search_state *s = ud;
    if (s->hits >= MAX_OBS_LINES) return;
    char *buf = NULL; size_t len = 0;
    if (plat_read_file(abs, &buf, &len) != 0) return;
    size_t plen = strlen(s->pat);
    if (!looks_binary(buf, len)) {
        size_t lineno = 1, ls = 0;
        for (size_t i = 0; i <= len && s->hits < MAX_OBS_LINES; i++) {
            if (i == len || buf[i] == '\n') {
                if (i > ls) {
                    const char *hit = strstr(buf + ls, s->pat);   /* buf is nul-terminated */
                    if (hit && hit + plen <= buf + i) {            /* WHOLE match lands in this line */
                        size_t llen = i - ls; if (llen > 200) llen = 200;
                        sb_appendf(s->out, "%s:%zu: ", rel, lineno);
                        sb_append_n(s->out, buf + ls, llen);
                        sb_putc(s->out, '\n');
                        s->hits++;
                    }
                }
                lineno++; ls = i + 1;
            }
        }
    }
    free(buf);
}

static char *do_search(const tool_ctx *ctx, const char *pattern, const char *rel, int *ok) {
    const char *sub = (rel && *rel) ? rel : ".";
    char *abs = NULL;
    if (sandbox_resolve(ctx->sandbox_root, sub, &abs) != 0)
        return err_obs("ERROR: path \"%s\" escapes the working directory", sub, ok);
    if (!pattern || !*pattern) { free(abs); return err_obs("ERROR: empty search pattern%s", "", ok); }
    /* Report matches relative to the SANDBOX ROOT (not the subdir) so paths round-trip
     * with read_file. Normalize the subdir into the walk's rel_prefix. */
    char *pfx = xstrdup((rel && *rel && strcmp(sub, ".") != 0) ? sub : "");
    { size_t l = strlen(pfx); while (l && (pfx[l-1] == '/' || pfx[l-1] == '\\')) pfx[--l] = '\0'; }
    const char *pfx_use = pfx;
    if (pfx_use[0] == '.' && (pfx_use[1] == '/' || pfx_use[1] == '\\')) pfx_use += 2;
    strbuf res; sb_init(&res);
    search_state s = { pattern, &res, 0 };
    int budget = WALK_BUDGET;
    gitignore *gi = load_root_gitignore(ctx->sandbox_root);
    walk(abs, pfx_use, search_visit, &s, gi, &budget, 0);
    gitignore_free(gi);
    free(abs); free(pfx);
    char *r;
    if (s.hits == 0) r = dup_cstr("(no matches)");
    else {
        if (s.hits >= MAX_OBS_LINES) sb_append(&res, "... (more matches not shown; narrow the search)");
        r = dup_cstr(sb_cstr(&res));
    }
    sb_free(&res);
    *ok = 1;
    return r;
}

char *tools_dispatch(const tool_ctx *ctx, const tool_call *call, int *ok) {
    *ok = 0;
    switch (call->kind) {
        case TC_READ_FILE:   return do_read_file(ctx, call->path, call->offset, ok);
        case TC_WRITE_FILE:  return do_write_file(ctx, call->path, call->content, ok);
        case TC_LIST_DIR:    return do_list_dir(ctx, call->path, ok);
        case TC_RUN_COMMAND: return do_run_command(ctx, call->cmd, ok);
        case TC_EDIT:        return do_edit(ctx, call->path, call->find, call->content, ok);
        case TC_SEARCH:      return do_search(ctx, call->pattern, call->path, ok);
        case TC_GLOB:        return do_glob(ctx, call->pattern, ok);
        default:             return dup_cstr("ERROR: tool not dispatchable");
    }
}
