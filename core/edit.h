/* edit — apply a search/replace to file content for the `edit` tool. Pure string
 * logic, platform-independent, unit-tested. Tries an exact unique match first, then
 * a whitespace-tolerant line-based match (so indentation / trailing-space drift in
 * the model's `old` text still lands). Intended mainly for capable models (a small
 * local model should prefer whole-file write_file); see HANDOFF.md. */
#ifndef ANACHRON_EDIT_H
#define ANACHRON_EDIT_H

/* Replace the single occurrence of `old` in `content` with `neu`. Returns the new
 * content (malloc'd) on success. On failure returns NULL and sets *err to a static
 * reason (not found / matches more than once / empty search). Caller frees. */
char *edit_apply(const char *content, const char *old, const char *neu, const char **err);

#endif /* ANACHRON_EDIT_H */
