/* gitignore — a pragmatic .gitignore matcher used by the discovery tools to skip
 * files the user has already declared uninteresting. Pure and platform-independent
 * (the caller reads the file and hands us the text), so it is unit-tested in
 * isolation. Supports the common subset: comments (#), blank lines, negation (!),
 * directory-only patterns (trailing '/'), anchored patterns (a leading or interior
 * '/' matches against the full repo-relative path) versus floating patterns (no
 * '/', matched against the basename at any depth), and '*'/'?' globbing. Last
 * matching pattern wins (so a later '!pat' can un-ignore) — but, exactly as in git,
 * a negated pattern cannot re-include a file whose parent directory is excluded,
 * because the tree walk prunes excluded directories before descending into them.
 * NOT supported: nested .gitignore files, '**' as distinct from '*', the rule that
 * '*' must not cross '/' ('*' here may cross separators — a minor over-match), and
 * backslash-escaped trailing spaces. */
#ifndef ANACHRON_GITIGNORE_H
#define ANACHRON_GITIGNORE_H

typedef struct gitignore gitignore;

/* Parse .gitignore text into a pattern set. Returns NULL if `text` is NULL or
 * contains no usable patterns. Free with gitignore_free. */
gitignore *gitignore_parse(const char *text);

/* 1 if `relpath` (repo-root-relative, forward slashes, no leading '/') is ignored
 * by the set, else 0. `is_dir` distinguishes directories so directory-only
 * patterns apply correctly. A NULL set ignores nothing. */
int  gitignore_match(const gitignore *gi, const char *relpath, int is_dir);

void gitignore_free(gitignore *gi);

#endif /* ANACHRON_GITIGNORE_H */
