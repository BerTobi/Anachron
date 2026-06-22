/* strbuf — growable, always-nul-terminated byte buffer, plus tiny alloc helpers.
 * Platform-independent. C99. The whole codebase leans on this for building
 * prompts, observations, and decoded strings without manual realloc bookkeeping. */
#ifndef ANACHRON_STRBUF_H
#define ANACHRON_STRBUF_H

#include <stddef.h>

/* malloc/realloc that abort() on OOM — this is a single-user CLI, not a server,
 * so unwinding an allocation failure buys nothing. strdup variants included
 * because strdup itself is POSIX, not C99. */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

typedef struct {
    char  *data;   /* always nul-terminated when len > 0; may be NULL when empty */
    size_t len;    /* bytes of content, excluding the trailing nul */
    size_t cap;    /* allocated bytes, including room for the nul */
} strbuf;

void        sb_init(strbuf *sb);
void        sb_free(strbuf *sb);
void        sb_clear(strbuf *sb);
void        sb_putc(strbuf *sb, char c);
void        sb_append(strbuf *sb, const char *s);
void        sb_append_n(strbuf *sb, const char *s, size_t n);
void        sb_appendf(strbuf *sb, const char *fmt, ...);
const char *sb_cstr(const strbuf *sb); /* never NULL — "" when empty */

#endif /* ANACHRON_STRBUF_H */
