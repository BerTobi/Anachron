#include "glob.h"

/* Iterative '*'/'?' wildcard match with backtracking on the last '*'. O(n*m) worst
 * case, no recursion. */
int glob_match(const char *pattern, const char *name) {
    const char *star = 0;   /* last '*' seen in the pattern */
    const char *ss = 0;     /* name position to resume from after that '*' */
    while (*name) {
        if (*pattern == '?' || *pattern == *name) {
            pattern++; name++;
        } else if (*pattern == '*') {
            star = pattern++;   /* remember, and try matching zero chars first */
            ss = name;
        } else if (star) {
            pattern = star + 1; /* backtrack: let the '*' absorb one more char */
            name = ++ss;
        } else {
            return 0;
        }
    }
    while (*pattern == '*') pattern++;
    return *pattern == 0;
}
