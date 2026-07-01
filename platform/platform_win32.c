/* platform_win32 — Windows XP implementation of platform.h.
 * Strictly XP-safe: FindFirstFile/FindNextFile, GetTickCount (NOT
 * GetTickCount64), _popen/_pclose, _chdir/_getcwd — all present since XP.
 * _WIN32_WINNT is pinned to 0x0501 by the build flags. Guarded so it compiles
 * to nothing off Windows. */
#ifdef _WIN32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif

#include "platform.h"
#include "strbuf.h"

#include <windows.h>
#include <direct.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int plat_read_file(const char *path, char **out_buf, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return -1; }
    rewind(f);
    char *buf = xmalloc((size_t)n + 1);
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    *out_buf = buf;
    *out_len = rd;
    return 0;
}

int plat_write_file(const char *path, const char *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t wr = fwrite(buf, 1, len, f);
    return (fclose(f) == 0 && wr == len) ? 0 : -1;
}

int plat_list_dir(const char *path, plat_dirlist *out) {
    strbuf pat; sb_init(&pat);
    sb_appendf(&pat, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(sb_cstr(&pat), &fd);
    sb_free(&pat);
    if (h == INVALID_HANDLE_VALUE) return -1;

    out->names = NULL;
    out->is_dir = NULL;
    out->is_symlink = NULL;
    out->count = 0;
    size_t cap = 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        if (out->count == cap) {
            cap = cap ? cap * 2 : 16;
            out->names = xrealloc(out->names, cap * sizeof *out->names);
            out->is_dir = xrealloc(out->is_dir, cap * sizeof *out->is_dir);
            out->is_symlink = xrealloc(out->is_symlink, cap * sizeof *out->is_symlink);
        }
        out->names[out->count] = xstrdup(fd.cFileName);
        out->is_dir[out->count] =
            (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        /* Junctions / symlinks surface as reparse points; the walk won't follow them. */
        out->is_symlink[out->count] =
            (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0;
        out->count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

void plat_dirlist_free(plat_dirlist *dl) {
    for (size_t i = 0; i < dl->count; i++) free(dl->names[i]);
    free(dl->names);
    free(dl->is_dir);
    free(dl->is_symlink);
    dl->names = NULL;
    dl->is_dir = NULL;
    dl->is_symlink = NULL;
    dl->count = 0;
}

int plat_isatty_stdout(void) {
    if (_isatty(_fileno(stdout))) return 1;
    /* msvcrt _isatty can under-report a real console; ask Win32 directly. */
    return GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_CHAR ? 1 : 0;
}

int plat_isatty_stdin(void) {
    if (_isatty(_fileno(stdin))) return 1;
    return GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_CHAR ? 1 : 0;
}

void plat_flush_input(void) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE) FlushConsoleInputBuffer(h);
}

void plat_set_echo(int enable) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) return;
    if (enable) mode |= ENABLE_ECHO_INPUT;
    else        mode &= ~(DWORD)ENABLE_ECHO_INPUT;
    SetConsoleMode(h, mode);
}

int plat_mkdir(const char *path) {
    if (_mkdir(path) == 0) return 0;
    DWORD attr = GetFileAttributesA(path);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        return 0;
    return -1;
}

long plat_mtime(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return -1;
    /* FILETIME is 100ns ticks since 1601; convert to unix seconds for comparison. */
    unsigned long long t = ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32)
                         | fad.ftLastWriteTime.dwLowDateTime;
    return (long)(t / 10000000ULL - 11644473600ULL);
}

int plat_run_command(const char *cmd, const char *cwd,
                     char **out, size_t *out_len, int *exit_code) {
    char saved[MAX_PATH];
    if (!_getcwd(saved, sizeof saved)) return -1;
    if (cwd && _chdir(cwd) != 0) return -1;

    strbuf full; sb_init(&full);
    sb_appendf(&full, "%s 2>&1", cmd);
    FILE *p = _popen(sb_cstr(&full), "r");
    sb_free(&full);
    if (!p) { _chdir(saved); return -1; }

    strbuf cap; sb_init(&cap);
    char buf[4096];
    size_t rd;
    while ((rd = fread(buf, 1, sizeof buf, p)) > 0)
        sb_append_n(&cap, buf, rd);
    int status = _pclose(p);

    _chdir(saved);

    *out = xstrdup(sb_cstr(&cap));
    *out_len = cap.len;
    sb_free(&cap);
    *exit_code = status;
    return 0;
}

double plat_time_sec(void) {
    /* GetTickCount is XP-safe (ms since boot, wraps ~49 days — fine for our
     * per-turn timing). GetTickCount64 is Vista+ and deliberately avoided. */
    return (double)GetTickCount() / 1000.0;
}

#endif /* _WIN32 */
