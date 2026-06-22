/* platform_posix — POSIX implementation of platform.h (antiX / dev-host).
 * Guarded so it compiles to nothing on Windows, letting the Makefile pass both
 * platform sources without conflict. */
#ifndef _WIN32

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "platform.h"
#include "strbuf.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
    int rc = (fclose(f) == 0 && wr == len) ? 0 : -1;
    return rc;
}

int plat_list_dir(const char *path, plat_dirlist *out) {
    DIR *d = opendir(path);
    if (!d) return -1;
    out->names = NULL;
    out->is_dir = NULL;
    out->is_symlink = NULL;
    out->count = 0;
    size_t cap = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (out->count == cap) {
            cap = cap ? cap * 2 : 16;
            out->names = xrealloc(out->names, cap * sizeof *out->names);
            out->is_dir = xrealloc(out->is_dir, cap * sizeof *out->is_dir);
            out->is_symlink = xrealloc(out->is_symlink, cap * sizeof *out->is_symlink);
        }
        out->names[out->count] = xstrdup(ent->d_name);
        /* stat (follows links) for is_dir; lstat (does not) to flag symlinks so the
         * tree walk can refuse to follow them and avoid cycles / sandbox escape. */
        strbuf full; sb_init(&full);
        sb_appendf(&full, "%s/%s", path, ent->d_name);
        struct stat stbuf, lbuf;
        out->is_dir[out->count] =
            (stat(sb_cstr(&full), &stbuf) == 0 && S_ISDIR(stbuf.st_mode)) ? 1 : 0;
        out->is_symlink[out->count] =
            (lstat(sb_cstr(&full), &lbuf) == 0 && S_ISLNK(lbuf.st_mode)) ? 1 : 0;
        sb_free(&full);
        out->count++;
    }
    closedir(d);
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
    return isatty(STDOUT_FILENO) ? 1 : 0;
}

int plat_mkdir(const char *path) {
    if (mkdir(path, 0777) == 0) return 0;
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    return -1;
}

int plat_run_command(const char *cmd, const char *cwd,
                     char **out, size_t *out_len, int *exit_code) {
    char saved[4096];
    if (!getcwd(saved, sizeof saved)) return -1;
    if (cwd && chdir(cwd) != 0) return -1;

    /* 2>&1 folds stderr into the captured stream. */
    strbuf full; sb_init(&full);
    sb_appendf(&full, "%s 2>&1", cmd);
    FILE *p = popen(sb_cstr(&full), "r");
    sb_free(&full);
    if (!p) { if (chdir(saved) != 0) { /* best effort */ } return -1; }

    strbuf cap; sb_init(&cap);
    char buf[4096];
    size_t rd;
    while ((rd = fread(buf, 1, sizeof buf, p)) > 0)
        sb_append_n(&cap, buf, rd);
    int status = pclose(p);

    if (chdir(saved) != 0) { /* best effort restore */ }

    *out = xstrdup(sb_cstr(&cap));
    *out_len = cap.len;
    sb_free(&cap);

    if (status == -1) *exit_code = -1;
    else if (WIFEXITED(status)) *exit_code = WEXITSTATUS(status);
    else *exit_code = 128;
    return 0;
}

double plat_time_sec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    return (double)time(NULL);
}

#endif /* !_WIN32 */
