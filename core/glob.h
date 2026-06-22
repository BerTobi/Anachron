/* glob — a tiny wildcard matcher for the `glob` discovery tool. Pure, platform-
 * independent, unit-tested. Matches a single name (a basename) against a pattern
 * using '*' (any run of chars) and '?' (one char). Path separators are not special
 * — the tool applies this to basenames while it recurses the tree itself. */
#ifndef ANACHRON_GLOB_H
#define ANACHRON_GLOB_H

int glob_match(const char *pattern, const char *name);

#endif /* ANACHRON_GLOB_H */
