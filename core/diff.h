/* diff — a small line-based unified-style diff used for show-on-edit. Pure and
 * platform-independent, so it is unit-tested in isolation. diff_unified appends a
 * human-readable diff of old_text -> new_text to `out`: removed lines prefixed
 * '-', added '+', context ' ', with runs of unchanged lines collapsed. When
 * `colour` is non-zero, removed/added lines are wrapped in ANSI red/green (callers
 * pass 0 for non-terminals and on Windows). The LCS table is O(n*m), so inputs
 * past an internal line cap are skipped with a short note instead.
 * Returns 1 if a diff was written, 0 if the two texts are identical. */
#ifndef ANACHRON_DIFF_H
#define ANACHRON_DIFF_H

#include "strbuf.h"

int diff_unified(const char *old_text, const char *new_text, strbuf *out, int colour);

#endif /* ANACHRON_DIFF_H */
