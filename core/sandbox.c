#include "sandbox.h"
#include "strbuf.h"

#include <string.h>

typedef struct { const char *s; size_t n; } seg;

static int is_sep(char c) { return c == '/' || c == '\\'; }

int sandbox_resolve(const char *root, const char *rel, char **out_abs) {
    /* A ':' anywhere means a Windows drive ("C:..") or NTFS alternate stream —
     * either way it can break out of the root, so reject outright. */
    if (strchr(rel, ':')) return -1;

    seg stack[256];
    size_t depth = 0;

    const char *p = rel;
    while (*p) {
        while (is_sep(*p)) p++;          /* collapse / skip leading separators */
        if (!*p) break;
        const char *start = p;
        while (*p && !is_sep(*p)) p++;
        size_t len = (size_t)(p - start);

        if (len == 1 && start[0] == '.') {
            continue;                    /* "." — no-op */
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (depth == 0) return -1;   /* "../" above the root — escape */
            depth--;
            continue;
        }
        if (depth >= sizeof(stack) / sizeof(stack[0])) return -1; /* absurdly deep */
        stack[depth].s = start;
        stack[depth].n = len;
        depth++;
    }

    strbuf out;
    sb_init(&out);
    sb_append(&out, root);
    for (size_t i = 0; i < depth; i++) {
        sb_putc(&out, '/');
        sb_append_n(&out, stack[i].s, stack[i].n);
    }
    *out_abs = xstrdup(sb_cstr(&out));
    sb_free(&out);
    return 0;
}
