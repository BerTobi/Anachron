/* sandbox — lexical containment of tool paths inside a working directory.
 * Platform-independent string logic only (no filesystem calls), so it lives in
 * /core. It resolves a model-supplied relative path against the sandbox root and
 * refuses anything that would escape: "..", drive letters, alternate streams.
 *
 * Limitation (documented, revisit when hardening): this is purely lexical, so a
 * pre-existing symlink inside the sandbox pointing outward is NOT caught. Good
 * enough for Phase 1 dev-host testing; Phase 3+ can add realpath containment. */
#ifndef ANACHRON_SANDBOX_H
#define ANACHRON_SANDBOX_H

/* Resolve `rel` (treated as relative, even if it starts with a separator)
 * against `root`. On success returns 0 and stores a malloc'd absolute-ish path
 * in *out_abs (caller frees). Returns -1 if the path escapes the sandbox or
 * contains a drive/stream specifier; *out_abs is left untouched. */
int sandbox_resolve(const char *root, const char *rel, char **out_abs);

#endif /* ANACHRON_SANDBOX_H */
