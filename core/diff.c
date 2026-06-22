#include "diff.h"

#include <stdlib.h>
#include <string.h>

#define DIFF_MAX_LINES 1500   /* LCS table is O(n*m); cap keeps it ~9MB and fast */
#define DIFF_CONTEXT   3      /* unchanged lines kept around each change */

/* Split `text` into NUL-terminated lines. A single trailing newline does not
 * produce an empty final line. *count receives the line count. Caller frees the
 * returned array and each element. */
static char **split_lines(const char *text, size_t *count) {
    size_t n = 0, cap = 0;
    char **lines = NULL;
    const char *p = text ? text : "";
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            lines = (char **)xrealloc(lines, cap * sizeof *lines);
        }
        char *s = (char *)xmalloc(len + 1);
        memcpy(s, p, len);
        s[len] = '\0';
        lines[n++] = s;
        if (!nl) break;
        p = nl + 1;
        if (*p == '\0') break;   /* trailing newline: no empty final line */
    }
    *count = n;
    return lines;
}

static void free_lines(char **lines, size_t n) {
    for (size_t i = 0; i < n; i++) free(lines[i]);
    free(lines);
}

/* op types */
enum { OP_SAME = 0, OP_DEL = 1, OP_ADD = 2 };

static void emit_line(strbuf *out, int type, const char *text, int colour) {
    const char *pfx = type == OP_DEL ? "-" : type == OP_ADD ? "+" : " ";
    if (colour && type == OP_DEL) sb_append(out, "\x1b[31m");
    else if (colour && type == OP_ADD) sb_append(out, "\x1b[32m");
    sb_append(out, pfx);
    sb_append(out, " ");
    sb_append(out, text);
    if (colour && type != OP_SAME) sb_append(out, "\x1b[0m");
    sb_append(out, "\n");
}

int diff_unified(const char *old_text, const char *new_text, strbuf *out, int colour) {
    size_t n = 0, m = 0;
    char **a = split_lines(old_text, &n);
    char **b = split_lines(new_text, &m);

    /* Identical? (cheap exact compare before building the table) */
    if (n == m) {
        size_t i = 0;
        for (; i < n; i++) if (strcmp(a[i], b[i]) != 0) break;
        if (i == n) { free_lines(a, n); free_lines(b, m); return 0; }
    }

    if (n > DIFF_MAX_LINES || m > DIFF_MAX_LINES) {
        sb_appendf(out, "  (diff omitted: %zu -> %zu lines, too large to render)\n", n, m);
        free_lines(a, n);
        free_lines(b, m);
        return 1;
    }

    /* LCS length table: dp[i][j] = LCS(a[i..], b[j..]). */
    size_t stride = m + 1;
    int *dp = (int *)xmalloc((n + 1) * stride * sizeof *dp);
    memset(dp, 0, (n + 1) * stride * sizeof *dp);
    for (size_t i = n; i-- > 0;) {
        for (size_t j = m; j-- > 0;) {
            if (strcmp(a[i], b[j]) == 0)
                dp[i * stride + j] = dp[(i + 1) * stride + (j + 1)] + 1;
            else {
                int down = dp[(i + 1) * stride + j];
                int right = dp[i * stride + (j + 1)];
                dp[i * stride + j] = down >= right ? down : right;
            }
        }
    }

    /* Forward walk producing the op sequence. */
    size_t opcap = n + m, opn = 0;
    int   *optype = (int *)xmalloc(opcap * sizeof *optype);
    char **optext = (char **)xmalloc(opcap * sizeof *optext);
    size_t i = 0, j = 0;
    while (i < n && j < m) {
        if (strcmp(a[i], b[j]) == 0) {
            optype[opn] = OP_SAME; optext[opn++] = a[i]; i++; j++;
        } else if (dp[(i + 1) * stride + j] >= dp[i * stride + (j + 1)]) {
            optype[opn] = OP_DEL; optext[opn++] = a[i]; i++;
        } else {
            optype[opn] = OP_ADD; optext[opn++] = b[j]; j++;
        }
    }
    while (i < n) { optype[opn] = OP_DEL; optext[opn++] = a[i]; i++; }
    while (j < m) { optype[opn] = OP_ADD; optext[opn++] = b[j]; j++; }

    /* Mark lines to show: every change, plus DIFF_CONTEXT lines on each side. */
    size_t showsz = opn ? opn : 1;
    char *show = (char *)xmalloc(showsz);
    memset(show, 0, showsz);
    for (size_t k = 0; k < opn; k++) {
        if (optype[k] != OP_SAME) {
            size_t lo = k > DIFF_CONTEXT ? k - DIFF_CONTEXT : 0;
            size_t hi = k + DIFF_CONTEXT + 1 < opn ? k + DIFF_CONTEXT + 1 : opn;
            for (size_t t = lo; t < hi; t++) show[t] = 1;
        }
    }

    /* Render, collapsing hidden runs into a one-line marker. */
    size_t k = 0;
    while (k < opn) {
        if (show[k]) {
            emit_line(out, optype[k], optext[k], colour);
            k++;
        } else {
            size_t start = k;
            while (k < opn && !show[k]) k++;
            sb_appendf(out, "  ... (%zu unchanged line%s)\n",
                       k - start, (k - start) == 1 ? "" : "s");
        }
    }

    free(show);
    free(optype);
    free(optext);
    free(dp);
    free_lines(a, n);
    free_lines(b, m);
    return 1;
}
