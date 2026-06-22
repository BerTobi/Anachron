#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "anachron: out of memory\n"); abort(); }
    return p;
}

void *xrealloc(void *q, size_t n) {
    void *p = realloc(q, n ? n : 1);
    if (!p) { fprintf(stderr, "anachron: out of memory\n"); abort(); }
    return p;
}

char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void sb_init(strbuf *sb) {
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_free(strbuf *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

/* Ensure room for `need` content bytes plus the trailing nul. */
static void sb_reserve(strbuf *sb, size_t need) {
    if (need + 1 <= sb->cap) return;
    size_t nc = sb->cap ? sb->cap : 32;
    while (nc < need + 1) nc *= 2;
    sb->data = xrealloc(sb->data, nc);
    sb->cap = nc;
}

void sb_clear(strbuf *sb) {
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

void sb_putc(strbuf *sb, char c) {
    sb_reserve(sb, sb->len + 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

void sb_append_n(strbuf *sb, const char *s, size_t n) {
    if (n == 0) return;
    sb_reserve(sb, sb->len + n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_append(strbuf *sb, const char *s) {
    sb_append_n(sb, s, strlen(s));
}

void sb_appendf(strbuf *sb, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) { va_end(ap2); return; }
    sb_reserve(sb, sb->len + (size_t)need);
    vsnprintf(sb->data + sb->len, (size_t)need + 1, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)need;
}

const char *sb_cstr(const strbuf *sb) {
    return sb->data ? sb->data : "";
}
