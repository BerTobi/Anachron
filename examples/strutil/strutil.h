#ifndef STRUTIL_H
#define STRUTIL_H

#include <stddef.h>

/* Return a newly-allocated uppercase copy of s (caller frees). */
char *str_upper(const char *s);

/* Return a newly-allocated copy of s with leading/trailing spaces removed. */
char *str_trim(const char *s);

/* Count occurrences of byte c in s. */
size_t str_count(const char *s, char c);

#endif /* STRUTIL_H */
