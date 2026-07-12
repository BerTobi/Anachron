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
#include <wininet.h>   /* plat_http_get for /update; wininet.dll ships with every XP */
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

int plat_self_path(char *buf, size_t n) {
    DWORD r = GetModuleFileNameA(NULL, buf, (DWORD)n);
    return (r > 0 && r < (DWORD)n) ? 0 : -1;
}

int plat_move_file(const char *from, const char *to) {
    /* MoveFileEx may rename the RUNNING exe (its file object stays open under the
     * new name) — that is exactly the self-update pivot. Replaces `to` if present. */
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

/* HTTP(S) GET through WinINet (XP-safe: all of these exist since IE3). Whether a
 * modern https host is REACHABLE depends on the OS's Schannel patch level — stock
 * XP SP3 tops out at TLS 1.0, so github.com refuses it; POSReady-patched systems
 * can succeed. Callers must treat failure as normal and fall back. Returns 0 on
 * ANY HTTP response (*status carries the code, even 4xx). */
int plat_http_get(const char *url, const char *headers,
                  char **resp, size_t *resp_len, int *status,
                  char *errbuf, size_t errsz) {
    if (errbuf && errsz) errbuf[0] = '\0';
    HINTERNET net = InternetOpenA("anachron",
                                  INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!net) {
        if (errbuf) snprintf(errbuf, errsz, "InternetOpen failed (err %lu)", GetLastError());
        return -1;
    }
    HINTERNET req = InternetOpenUrlA(net, url, headers,
                                     headers ? (DWORD)strlen(headers) : 0,
                                     INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                                     INTERNET_FLAG_NO_UI, 0);
    if (!req) {
        if (errbuf) snprintf(errbuf, errsz, "could not connect (err %lu - on stock XP "
                             "this is usually the TLS ceiling)", GetLastError());
        InternetCloseHandle(net);
        return -1;
    }
    DWORD st = 0, slen = sizeof st;
    if (!HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                        &st, &slen, NULL)) {
        if (errbuf) snprintf(errbuf, errsz, "no HTTP status (err %lu)", GetLastError());
        InternetCloseHandle(req);
        InternetCloseHandle(net);
        return -1;
    }
    *status = (int)st;
    strbuf acc; sb_init(&acc);
    char chunk[8192];
    DWORD got = 0;
    while (InternetReadFile(req, chunk, sizeof chunk, &got) && got > 0)
        sb_append_n(&acc, chunk, (size_t)got);
    InternetCloseHandle(req);
    InternetCloseHandle(net);
    *resp = xmalloc(acc.len + 1);
    memcpy(*resp, sb_cstr(&acc), acc.len + 1);
    *resp_len = acc.len;
    sb_free(&acc);
    return 0;
}

/* Split "scheme://host[:port]/path" for InternetConnect/HttpOpenRequest. */
static int win_url_split(const char *url, char **host, INTERNET_PORT *port,
                         char **path, int *https) {
    const char *p;
    if (strncmp(url, "http://", 7) == 0) { *https = 0; p = url + 7; }
    else if (strncmp(url, "https://", 8) == 0) { *https = 1; p = url + 8; }
    else return -1;
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    const char *colon = NULL;
    for (const char *q = p; q < hostend; q++) if (*q == ':') { colon = q; break; }
    if (colon) {
        *host = xstrndup(p, (size_t)(colon - p));
        *port = (INTERNET_PORT)atoi(colon + 1);
    } else {
        *host = xstrndup(p, (size_t)(hostend - p));
        *port = *https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    }
    *path = xstrdup(slash && *slash ? slash : "/");
    return 0;
}

int plat_http_post(const char *url, const char *headers,
                   const char *body, size_t body_len,
                   char **resp, size_t *resp_len, int *status,
                   char *errbuf, size_t errsz) {
    if (errbuf && errsz) errbuf[0] = '\0';
    char *host = NULL, *path = NULL;
    INTERNET_PORT port = 0;
    int https = 0;
    if (win_url_split(url, &host, &port, &path, &https) != 0) {
        if (errbuf) snprintf(errbuf, errsz, "bad URL: %s", url);
        return -1;
    }

    int rc = -1;
    HINTERNET net = NULL, conn = NULL, req = NULL;
    net = InternetOpenA("anachron", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!net) {
        if (errbuf) snprintf(errbuf, errsz, "InternetOpen failed (err %lu)", GetLastError());
        goto done;
    }
    /* A remote server may spend minutes prefilling a big prompt before the first
     * response byte — keep the receive window generous. */
    {
        DWORD t_conn = 15000, t_io = 600000;
        InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT, &t_conn, sizeof t_conn);
        InternetSetOptionA(net, INTERNET_OPTION_SEND_TIMEOUT, &t_io, sizeof t_io);
        InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &t_io, sizeof t_io);
    }
    conn = InternetConnectA(net, host, port, NULL, NULL,
                            INTERNET_SERVICE_HTTP, 0, 0);
    if (!conn) {
        if (errbuf) snprintf(errbuf, errsz, "cannot connect to %s:%u (err %lu)",
                             host, (unsigned)port, GetLastError());
        goto done;
    }
    req = HttpOpenRequestA(conn, "POST", path, NULL, NULL, NULL,
                           INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                           INTERNET_FLAG_NO_UI | INTERNET_FLAG_KEEP_CONNECTION |
                           (https ? INTERNET_FLAG_SECURE : 0), 0);
    if (!req) {
        if (errbuf) snprintf(errbuf, errsz, "HttpOpenRequest failed (err %lu)", GetLastError());
        goto done;
    }
    {
        strbuf h; sb_init(&h);
        sb_append(&h, "Content-Type: application/json\r\n");
        if (headers) sb_append(&h, headers);
        BOOL ok = HttpSendRequestA(req, sb_cstr(&h), (DWORD)h.len,
                                   (void *)body, (DWORD)body_len);
        sb_free(&h);
        if (!ok) {
            if (errbuf) snprintf(errbuf, errsz, "request failed (err %lu%s)",
                                 GetLastError(),
                                 https ? " - on stock XP this is usually the TLS ceiling" : "");
            goto done;
        }
    }
    {
        DWORD st = 0, slen = sizeof st;
        if (!HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                            &st, &slen, NULL)) {
            if (errbuf) snprintf(errbuf, errsz, "no HTTP status (err %lu)", GetLastError());
            goto done;
        }
        *status = (int)st;
    }
    {
        strbuf acc; sb_init(&acc);
        char chunk[8192];
        DWORD got = 0;
        while (InternetReadFile(req, chunk, sizeof chunk, &got) && got > 0)
            sb_append_n(&acc, chunk, (size_t)got);
        *resp = xmalloc(acc.len + 1);
        memcpy(*resp, sb_cstr(&acc), acc.len + 1);
        *resp_len = acc.len;
        sb_free(&acc);
    }
    rc = 0;
done:
    if (req) InternetCloseHandle(req);
    if (conn) InternetCloseHandle(conn);
    if (net) InternetCloseHandle(net);
    free(host);
    free(path);
    return rc;
}

#endif /* _WIN32 */

/* Screen capture: classic GDI BitBlt into a 24-bit DIB section — every call
 * here exists on Windows XP SP3 (and NT4, for that matter). CAPTUREBLT pulls
 * layered windows in too. The DIB is requested top-down (negative height) so
 * rows come out in PNG order; GDI hands back BGR with 4-byte-aligned rows,
 * repacked to tight RGB. Screens wider than 1400px are halved (2x2 average)
 * until they fit, keeping the base64 payload well under vision-API limits. */
#include "png.h"

int plat_screenshot(const char *path, char *errbuf, size_t errsz) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw <= 0 || sh <= 0) {
        snprintf(errbuf, errsz, "screenshot: no screen metrics");
        return -1;
    }
    HDC sdc = GetDC(NULL);
    HDC mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sw;
    bi.bmiHeader.biHeight = -sh;            /* top-down rows */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbm || !bits) {
        if (hbm) DeleteObject(hbm);
        DeleteDC(mdc); ReleaseDC(NULL, sdc);
        snprintf(errbuf, errsz, "screenshot: CreateDIBSection failed");
        return -1;
    }
    HGDIOBJ old = SelectObject(mdc, hbm);
    BOOL ok = BitBlt(mdc, 0, 0, sw, sh, sdc, 0, 0, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    SelectObject(mdc, old);
    DeleteDC(mdc); ReleaseDC(NULL, sdc);
    if (!ok) {
        DeleteObject(hbm);
        snprintf(errbuf, errsz, "screenshot: BitBlt failed");
        return -1;
    }

    size_t stride = ((size_t)sw * 3 + 3) & ~(size_t)3;   /* DIB rows are dword-aligned */
    unsigned char *rgb = malloc((size_t)sw * sh * 3);
    if (!rgb) {
        DeleteObject(hbm);
        snprintf(errbuf, errsz, "screenshot: out of memory");
        return -1;
    }
    for (int y = 0; y < sh; y++) {
        const unsigned char *src = (const unsigned char *)bits + (size_t)y * stride;
        unsigned char *dst = rgb + (size_t)y * sw * 3;
        for (int x = 0; x < sw; x++) {          /* BGR -> RGB */
            dst[x*3]     = src[x*3 + 2];
            dst[x*3 + 1] = src[x*3 + 1];
            dst[x*3 + 2] = src[x*3];
        }
    }
    DeleteObject(hbm);

    while (sw > 1400) {                          /* halve with a 2x2 box average */
        int nw = sw / 2, nh = sh / 2;
        for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) {
            for (int ch = 0; ch < 3; ch++) {
                int a = rgb[((size_t)(2*y)   * sw + 2*x)   * 3 + ch];
                int b = rgb[((size_t)(2*y)   * sw + 2*x+1) * 3 + ch];
                int c = rgb[((size_t)(2*y+1) * sw + 2*x)   * 3 + ch];
                int d = rgb[((size_t)(2*y+1) * sw + 2*x+1) * 3 + ch];
                rgb[((size_t)y * nw + x) * 3 + ch] = (unsigned char)((a+b+c+d) / 4);
            }
        }
        sw = nw; sh = nh;
    }

    int rc = png_write_rgb(path, sw, sh, rgb, errbuf, errsz);
    free(rgb);
    return rc;
}

/* Process-level parallelism for sub-agent fan-out: cmd.exe /c parses the
 * redirections in the command string, CreateProcess runs detached, and the
 * handle is the process HANDLE. All XP-safe. */
int plat_spawn(const char *cmd, const char *cwd, void **handle) {
    strbuf full; sb_init(&full);
    sb_appendf(&full, "cmd.exe /c %s", cmd);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    char *line = xstrdup(sb_cstr(&full));   /* CreateProcess may scribble on it */
    sb_free(&full);
    BOOL ok = CreateProcessA(NULL, line, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW, NULL,
                             (cwd && *cwd) ? cwd : NULL, &si, &pi);
    free(line);
    if (!ok) return -1;
    CloseHandle(pi.hThread);
    *handle = (void *)pi.hProcess;
    return 0;
}

int plat_wait_all(void **handles, size_t n, int *exit_codes) {
    for (size_t i = 0; i < n; i++) {
        HANDLE h = (HANDLE)handles[i];
        exit_codes[i] = -1;
        if (!h) continue;
        if (WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0) {
            DWORD code = 0;
            if (GetExitCodeProcess(h, &code)) exit_codes[i] = (int)code;
        }
        CloseHandle(h);
    }
    return 0;
}
