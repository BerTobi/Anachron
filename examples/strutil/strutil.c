#include "strutil.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

char *str_upper(const char *s) {
    /* TODO: handle a NULL input gracefully (a good first task for the agent). */
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    for (size_t i = 0; i < n; i++) out[i] = (char)toupper((unsigned char)s[i]);
    out[n] = '\0';
    return out;
}

char *str_trim(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    char *out = malloc(n + 1);
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

size_t str_count(const char *s, char c) {
    size_t k = 0;
    for (; *s; s++) if (*s == c) k++;
    return k;
}
