/* platform — the thin OS abstraction. /core and /tools call only through here;
 * platform_posix.c and platform_win32.c provide the two implementations. Keep
 * this surface small: file I/O, directory listing, child-process capture, and a
 * monotonic clock. Everything XP-safe (no Win32 API newer than XP SP3). */
#ifndef ANACHRON_PLATFORM_H
#define ANACHRON_PLATFORM_H

#include <stddef.h>

/* All int-returning functions: 0 on success, non-zero on failure.
 * Output buffers are malloc'd and owned by the caller. */

/* Read an entire file. *out_buf is nul-terminated (and *out_len excludes it). */
int  plat_read_file(const char *path, char **out_buf, size_t *out_len);
int  plat_write_file(const char *path, const char *buf, size_t len);

typedef struct {
    char  **names;       /* entry names, no path prefix */
    int    *is_dir;      /* 1 if the entry is a directory, else 0 */
    int    *is_symlink;  /* 1 if the entry is a symlink/reparse point, else 0 */
    size_t  count;
} plat_dirlist;

int  plat_list_dir(const char *path, plat_dirlist *out);
void plat_dirlist_free(plat_dirlist *dl);

/* Create a directory. Returns 0 on success OR if it already exists, non-zero else. */
int  plat_mkdir(const char *path);

/* Last-modified time of `path` in seconds since the epoch, or -1 if it does not
 * exist. Only used for relative comparisons (is a source newer than its binary?). */
long plat_mtime(const char *path);

/* 1 if standard output is an interactive terminal, else 0 (drives colour output). */
int  plat_isatty_stdout(void);

/* 1 if standard input is an interactive terminal, else 0 (drives the permission gate:
 * a y/N prompt only makes sense when a human is at the keyboard). */
int  plat_isatty_stdin(void);

/* Discard any buffered, unread terminal input (best effort). Called before each
 * prompt so stray bytes typed/scrolled during a long generation don't get read as
 * the next command. No-op / harmless on a non-terminal. */
void plat_flush_input(void);

/* Turn terminal echo on/off (best effort), leaving signal generation intact. Used to
 * silence keys typed while the model is generating, so they don't spew escape bytes;
 * combined with plat_flush_input before the next prompt. No-op on a non-terminal. */
void plat_set_echo(int enable);

/* Run `cmd` through the system shell with `cwd` as the working directory,
 * capturing stdout+stderr combined into *out (malloc'd, nul-terminated).
 * *exit_code receives the child's exit status. Returns 0 if the command was
 * launched at all (whatever its exit code), non-zero if it could not run. */
int  plat_run_command(const char *cmd, const char *cwd,
                      char **out, size_t *out_len, int *exit_code);

/* Monotonic-ish seconds for timing. XP-safe (GetTickCount, not GetTickCount64). */
double plat_time_sec(void);

/* Absolute path of the running executable (for the self-updater). */
int  plat_self_path(char *buf, size_t n);

/* Move/rename `from` to `to`, replacing an existing `to`. On Windows this works
 * even when `from` is the RUNNING executable (a running exe may be renamed but
 * not overwritten) — the pivot of the self-update swap. */
int  plat_move_file(const char *from, const char *to);

/* HTTP(S) GET, same contract and transports as plat_http_post below: returns 0
 * whenever an HTTP response was obtained (*status carries the code, even 4xx),
 * non-zero when none could be had. `headers` adds "Name: value\r\n" lines (may
 * be NULL). TLS reach follows the platform: WinINet per the OS patch level on
 * Windows (stock XP SP3 tops out below TLS 1.2), the system curl on POSIX. */
int  plat_http_get(const char *url, const char *headers,
                   char **resp, size_t *resp_len, int *status,
                   char *errbuf, size_t errsz);

/* HTTP(S) POST with a JSON body (Content-Type: application/json is always sent;
 * `headers` adds extra "Name: value\r\n" lines, may be NULL). Returns 0 whenever an
 * HTTP response was obtained — *status carries the code and *resp the body, even
 * for 4xx/5xx (API error bodies explain themselves) — and non-zero when no response
 * could be had (connect/TLS failure), with the reason in errbuf.
 *   Windows: WinINet, http and https alike (TLS reach depends on the OS - a LAN
 *            http:// llama-server works on stock XP; api.anthropic.com needs a
 *            TLS-1.2-capable Windows).
 *   POSIX:   raw socket for http:// (interrupt-aware); https:// is delegated to
 *            the system `curl` (secrets passed via a private temp file, not argv). */
int  plat_http_post(const char *url, const char *headers,
                    const char *body, size_t body_len,
                    char **resp, size_t *resp_len, int *status,
                    char *errbuf, size_t errsz);

#endif /* ANACHRON_PLATFORM_H */
