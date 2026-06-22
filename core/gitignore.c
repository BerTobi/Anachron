#include "gitignore.h"
#include "glob.h"
#include "strbuf.h"

#include <stdlib.h>
#include <string.h>

/* One parsed pattern. `glob` is the cleaned pattern (leading '!'/'/' and trailing
 * '/' stripped). `negated` un-ignores a prior match. `dir_only` applies only to
 * directories. `anchored` patterns match the whole relpath; floating patterns
 * (no interior '/') match any path component's basename. */
typedef struct {
    char *glob;
    int   negated;
    int   dir_only;
    int   anchored;
} gi_pat;

struct gitignore {
    gi_pat *pats;
    size_t  count;
    size_t  cap;
};

static void push_pat(gitignore *gi, char *glob, int negated, int dir_only, int anchored) {
    if (gi->count == gi->cap) {
        gi->cap = gi->cap ? gi->cap * 2 : 16;
        gi->pats = xrealloc(gi->pats, gi->cap * sizeof *gi->pats);
    }
    gi->pats[gi->count].glob = glob;
    gi->pats[gi->count].negated = negated;
    gi->pats[gi->count].dir_only = dir_only;
    gi->pats[gi->count].anchored = anchored;
    gi->count++;
}

/* Parse a single line (already split, no trailing newline) into the set. */
static void parse_line(gitignore *gi, const char *line, size_t len) {
    /* Trim trailing spaces and a CR (a backslash-escaped trailing space is not
     * supported; vanishingly rare). Leading spaces are NOT trimmed — git treats
     * them as significant. */
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\r')) len--;
    if (len == 0 || line[0] == '#') return;   /* blank or comment */

    int negated = 0;
    if (line[0] == '!') { negated = 1; line++; len--; }
    if (len == 0) return;

    int dir_only = 0;
    if (line[len - 1] == '/') { dir_only = 1; len--; }
    if (len == 0) return;

    /* Anchored if it begins with '/' or contains a '/' anywhere before the end. */
    int anchored = 0;
    if (line[0] == '/') { anchored = 1; line++; len--; }
    else {
        for (size_t i = 0; i < len; i++)
            if (line[i] == '/') { anchored = 1; break; }
    }
    if (len == 0) return;

    char *g = (char *)xmalloc(len + 1);
    memcpy(g, line, len);
    g[len] = '\0';
    push_pat(gi, g, negated, dir_only, anchored);
}

gitignore *gitignore_parse(const char *text) {
    if (!text) return NULL;
    gitignore *gi = (gitignore *)xmalloc(sizeof *gi);
    gi->pats = NULL;
    gi->count = 0;
    gi->cap = 0;

    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        parse_line(gi, p, len);
        if (!nl) break;
        p = nl + 1;
    }
    if (gi->count == 0) { free(gi->pats); free(gi); return NULL; }
    return gi;
}

static const char *base_of(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/') b = q + 1;
    return b;
}

static int pat_matches(const gi_pat *pt, const char *relpath, int is_dir) {
    if (pt->dir_only && !is_dir) return 0;
    if (pt->anchored)
        return glob_match(pt->glob, relpath);
    /* Floating: match the basename of the entry (gitignore matches such a pattern
     * at any depth; because the walk tests each entry by its own relpath, matching
     * the entry basename is sufficient). */
    return glob_match(pt->glob, base_of(relpath));
}

int gitignore_match(const gitignore *gi, const char *relpath, int is_dir) {
    if (!gi || !relpath) return 0;
    int ignored = 0;
    for (size_t i = 0; i < gi->count; i++) {
        if (pat_matches(&gi->pats[i], relpath, is_dir))
            ignored = !gi->pats[i].negated;   /* last match wins */
    }
    return ignored;
}

void gitignore_free(gitignore *gi) {
    if (!gi) return;
    for (size_t i = 0; i < gi->count; i++) free(gi->pats[i].glob);
    free(gi->pats);
    free(gi);
}
