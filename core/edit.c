#include "edit.h"
#include "strbuf.h"

#include <stdlib.h>
#include <string.h>

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* Compare two byte ranges ignoring leading/trailing whitespace (not internal). */
static int line_eq_stripped(const char *a, size_t alen, const char *b, size_t blen) {
    while (alen && is_ws(a[0]))        { a++; alen--; }
    while (alen && is_ws(a[alen - 1])) alen--;
    while (blen && is_ws(b[0]))        { b++; blen--; }
    while (blen && is_ws(b[blen - 1])) blen--;
    return alen == blen && memcmp(a, b, alen) == 0;
}

/* Split into line spans [ls[i], le[i]) where le excludes the newline. A trailing
 * empty line after a final '\n' is included; callers trim it where needed. */
static size_t split_lines(const char *t, size_t tlen, size_t **ls_out, size_t **le_out) {
    size_t cap = 16, L = 0;
    size_t *ls = xmalloc(cap * sizeof *ls), *le = xmalloc(cap * sizeof *le);
    size_t s = 0;
    for (;;) {
        size_t e = s;
        while (e < tlen && t[e] != '\n') e++;
        if (L == cap) { cap *= 2; ls = xrealloc(ls, cap * sizeof *ls); le = xrealloc(le, cap * sizeof *le); }
        ls[L] = s; le[L] = e; L++;
        if (e >= tlen) break;
        s = e + 1;
    }
    *ls_out = ls; *le_out = le;
    return L;
}

char *edit_apply(const char *content, const char *old, const char *neu, const char **err) {
    *err = NULL;
    if (!old || !*old) { *err = "empty search text"; return NULL; }
    size_t olen = strlen(old);

    /* 1. exact match — must be unique */
    const char *p = strstr(content, old);
    if (p) {
        if (strstr(p + 1, old)) {
            *err = "search text matches more than once; add more surrounding context";
            return NULL;
        }
        strbuf sb; sb_init(&sb);
        sb_append_n(&sb, content, (size_t)(p - content));
        sb_append(&sb, neu);
        sb_append(&sb, p + olen);
        char *r = xstrdup(sb_cstr(&sb));
        sb_free(&sb);
        return r;
    }

    /* 2. line-based, whitespace-tolerant (per-line leading/trailing ws ignored) */
    size_t clen = strlen(content);
    size_t *cls, *cle, *ols, *ole;
    size_t cL = split_lines(content, clen, &cls, &cle);
    size_t oL = split_lines(old, olen, &ols, &ole);
    while (oL > 1 && ole[oL - 1] == ols[oL - 1]) oL--; /* drop a trailing empty old line */

    int found = -1, count = 0;
    if (oL > 0 && oL <= cL) {
        for (size_t a = 0; a + oL <= cL; a++) {
            int m = 1;
            for (size_t k = 0; k < oL; k++) {
                if (!line_eq_stripped(content + cls[a + k], cle[a + k] - cls[a + k],
                                      old + ols[k], ole[k] - ols[k])) { m = 0; break; }
            }
            if (m) { count++; if (found < 0) found = (int)a; }
        }
    }

    char *result = NULL;
    if (count == 0) {
        *err = "search text not found in file";
    } else if (count > 1) {
        *err = "search text matches more than once; add more surrounding context";
    } else {
        size_t a = (size_t)found;
        size_t b0 = cls[a];
        size_t b1 = (a + oL < cL) ? cls[a + oL] : clen; /* start of line after the block, or EOF */
        strbuf sb; sb_init(&sb);
        sb_append_n(&sb, content, b0);
        sb_append(&sb, neu);
        size_t nlen = strlen(neu);
        if (b1 > b0 && content[b1 - 1] == '\n' && (nlen == 0 || neu[nlen - 1] != '\n'))
            sb_putc(&sb, '\n'); /* keep the following line on its own line */
        sb_append(&sb, content + b1);
        result = xstrdup(sb_cstr(&sb));
        sb_free(&sb);
    }
    free(cls); free(cle); free(ols); free(ole);
    return result;
}
