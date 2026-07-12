/* version — the single source of truth for the ANACHRON release version.
 * Semantic versioning (https://semver.org): MAJOR.MINOR.PATCH. Pre-1.0 while the
 * harness is validated on the dev host but not yet on real Pentium-M / XP hardware.
 * On a release: bump this, add a CHANGELOG.md entry, and tag `vMAJOR.MINOR.PATCH`. */
#ifndef ANACHRON_VERSION_H
#define ANACHRON_VERSION_H

/* Overridable at build time (-DANACHRON_VERSION='"9.9.9"') so update-flow tests
 * can produce a binary that reports an arbitrary version. */
#ifndef ANACHRON_VERSION
#define ANACHRON_VERSION "0.8.5"
#endif

#endif /* ANACHRON_VERSION_H */
