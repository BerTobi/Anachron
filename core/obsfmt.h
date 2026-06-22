/* obsfmt — shape tool observations to fit a tiny context window (the SWE-agent
 * "agent-computer interface" lesson: bound and clarify what the model sees). Pure
 * string logic, platform-independent, unit-tested. No line numbers on purpose:
 * ANACHRON edits via whole-file write_file, so numbered lines would risk the model
 * copying "12| " prefixes back into a file. */
#ifndef ANACHRON_OBSFMT_H
#define ANACHRON_OBSFMT_H

#include <stddef.h>

/* Return a malloc'd copy of `text` capped to at most `max_lines` lines AND
 * `max_bytes` bytes (whichever limit is hit first), line-aligned. If anything was
 * cut, a "... (N more lines not shown; M total)" note is appended. Caller frees. */
char *obs_capped(const char *text, size_t max_lines, size_t max_bytes);

/* A paged view of `text`: skip `offset` lines, then show up to `count` lines (and
 * <= max_bytes). Wrapped with a "lines A-B of T" header and, if more follows, a
 * "... read_file offset=N to continue" footer. Lets a model page a large file.
 * Returns malloc'd; caller frees. */
char *obs_window(const char *text, size_t offset, size_t count, size_t max_bytes);

#endif /* ANACHRON_OBSFMT_H */
