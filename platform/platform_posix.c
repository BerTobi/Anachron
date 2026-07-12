/* platform_posix — POSIX implementation of platform.h (antiX / dev-host).
 * Guarded so it compiles to nothing on Windows, letting the Makefile pass both
 * platform sources without conflict. */
#ifndef _WIN32

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "platform.h"
#include "strbuf.h"
#include "interrupt.h"   /* Ctrl+C awareness in the blocking HTTP read loop */

#include <dirent.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
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

int plat_isatty_stdin(void) { return isatty(STDIN_FILENO) ? 1 : 0; }

int plat_isatty_stdout(void) {
    return isatty(STDOUT_FILENO) ? 1 : 0;
}

void plat_flush_input(void) {
    if (isatty(STDIN_FILENO)) tcflush(STDIN_FILENO, TCIFLUSH);
}

void plat_set_echo(int enable) {
    struct termios t;
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &t) != 0) return;
    if (enable) t.c_lflag |= ECHO;
    else        t.c_lflag &= ~((tcflag_t)ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

int plat_mkdir(const char *path) {
    if (mkdir(path, 0777) == 0) return 0;
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    return -1;
}

long plat_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_mtime;
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

int plat_self_path(char *buf, size_t n) {
    ssize_t r = readlink("/proc/self/exe", buf, n - 1);
    if (r <= 0) return -1;
    buf[r] = '\0';
    return 0;
}

int plat_move_file(const char *from, const char *to) {
    return rename(from, to) == 0 ? 0 : -1;   /* POSIX rename replaces atomically */
}

static int url_split(const char *url, char **host, char **port, char **path, int *https);
static int http_req_socket(const char *method,
                           const char *host, const char *port, const char *path,
                           const char *headers, const char *body, size_t body_len,
                           char **resp, size_t *resp_len, int *status,
                           char *errbuf, size_t errsz);
static int http_req_curl(const char *method, const char *url, const char *headers,
                         const char *body, size_t body_len,
                         char **resp, size_t *resp_len, int *status,
                         char *errbuf, size_t errsz);

int plat_http_get(const char *url, const char *headers,
                  char **resp, size_t *resp_len, int *status,
                  char *errbuf, size_t errsz) {
    if (errbuf && errsz) errbuf[0] = '\0';
    char *host = NULL, *port = NULL, *path = NULL;
    int https = 0;
    if (url_split(url, &host, &port, &path, &https) != 0) {
        if (errbuf) snprintf(errbuf, errsz, "bad URL: %s", url);
        return -1;
    }
    int rc;
    if (https) {
        rc = http_req_curl("GET", url, headers, NULL, 0,
                           resp, resp_len, status, errbuf, errsz);
    } else {
        rc = http_req_socket("GET", host, port, path, headers, NULL, 0,
                             resp, resp_len, status, errbuf, errsz);
    }
    free(host); free(port); free(path);
    return rc;
}

/* ---- plat_http_post ---------------------------------------------------------
 * http://  -> a minimal raw-socket HTTP/1.1 client (LAN llama-server: no TLS).
 * https:// -> delegated to the system `curl` (modern TLS without linking a TLS
 *             stack). Secrets ride in a 0600 temp config file, never on argv. */

/* Split "scheme://host[:port]/path" (path may be empty -> "/"). Returns 0/-1. */
static int url_split(const char *url, char **host, char **port, char **path, int *https) {
    const char *p;
    if (strncmp(url, "http://", 7) == 0) { *https = 0; p = url + 7; }
    else if (strncmp(url, "https://", 8) == 0) { *https = 1; p = url + 8; }
    else return -1;
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(hostend - p));
    if (colon) {
        *host = xstrndup(p, (size_t)(colon - p));
        *port = xstrndup(colon + 1, (size_t)(hostend - colon - 1));
    } else {
        *host = xstrndup(p, (size_t)(hostend - p));
        *port = xstrdup(*https ? "443" : "80");
    }
    *path = xstrdup(slash && *slash ? slash : "/");
    return 0;
}

static int http_dial(const char *host, const char *port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int send_all(int fd, const char *buf, size_t len) {
    while (len) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* De-chunk an HTTP/1.1 chunked body into `out`. */
static void dechunk(const char *body, size_t len, strbuf *out) {
    size_t i = 0;
    while (i < len) {
        size_t sz = 0;
        int any = 0;
        while (i < len && body[i] != '\r' && body[i] != '\n') {
            char ch = body[i++];
            int d;
            if (ch >= '0' && ch <= '9') d = ch - '0';
            else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
            else break;
            if (sz > len) sz = len;          /* cap before the multiply: no wrap */
            else sz = sz * 16 + (size_t)d;
            any = 1;
        }
        while (i < len && body[i] != '\n') i++;
        if (i < len) i++;
        if (!any || sz == 0) break;
        if (sz > len - i) sz = len - i;
        sb_append_n(out, body + i, sz);
        i += sz;
        while (i < len && (body[i] == '\r' || body[i] == '\n')) i++;
    }
}

/* The raw-socket path for http:// — reads until EOF, honours Ctrl+C between reads.
 * method is "GET" or "POST"; body may be NULL for GET. */
static int http_req_socket(const char *method,
                           const char *host, const char *port, const char *path,
                           const char *headers, const char *body, size_t body_len,
                           char **resp, size_t *resp_len, int *status,
                           char *errbuf, size_t errsz) {
    int fd = http_dial(host, port);
    if (fd < 0) {
        if (errbuf) snprintf(errbuf, errsz, "cannot connect to %s:%s", host, port);
        return -1;
    }
    strbuf req; sb_init(&req);
    sb_appendf(&req, "%s %s HTTP/1.1\r\nHost: %s:%s\r\nConnection: close\r\n",
               method, path, host, port);
    if (body)
        sb_appendf(&req, "Content-Type: application/json\r\nContent-Length: %zu\r\n",
                   body_len);
    if (headers) sb_append(&req, headers);
    sb_append(&req, "\r\n");
    if (body) sb_append_n(&req, body, body_len);
    int se = send_all(fd, sb_cstr(&req), req.len);
    sb_free(&req);
    if (se != 0) {
        close(fd);
        if (errbuf) snprintf(errbuf, errsz, "send failed to %s:%s", host, port);
        return -1;
    }

    strbuf acc; sb_init(&acc);
    char chunk[8192];
    for (;;) {
        /* poll so a Ctrl+C during a long server-side prefill aborts promptly */
        struct pollfd pf; pf.fd = fd; pf.events = POLLIN;
        int pr = poll(&pf, 1, 250);
        if (interrupt_pending()) {
            close(fd); sb_free(&acc);
            if (errbuf) snprintf(errbuf, errsz, "interrupted");
            return -1;
        }
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n > 0) { sb_append_n(&acc, chunk, (size_t)n); continue; }
        if (n == 0) break;
        if (errno == EINTR) continue;
        break;
    }
    close(fd);

    const char *raw = sb_cstr(&acc);
    const char *sep = strstr(raw, "\r\n\r\n");
    if (!sep) {
        sb_free(&acc);
        if (errbuf) snprintf(errbuf, errsz, "malformed HTTP response from %s:%s", host, port);
        return -1;
    }
    int st = 0;
    (void)sscanf(raw, "HTTP/%*s %d", &st);
    size_t hdr_len = (size_t)(sep - raw);
    const char *body_start = sep + 4;
    size_t blen = acc.len - hdr_len - 4;

    int chunked;
    {
        char *hdrs = xstrndup(raw, hdr_len);
        for (char *q = hdrs; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
        chunked = strstr(hdrs, "transfer-encoding: chunked") != NULL;
        free(hdrs);
    }
    strbuf dec; sb_init(&dec);
    if (chunked) dechunk(body_start, blen, &dec);
    else         sb_append_n(&dec, body_start, blen);
    sb_free(&acc);

    *resp = xmalloc(dec.len + 1);
    memcpy(*resp, sb_cstr(&dec), dec.len + 1);
    *resp_len = dec.len;
    *status = st;
    sb_free(&dec);
    return 0;
}

/* The curl path for https:// — headers via a private --config file so secrets
 * never appear on the command line; body via a temp file; status via -w. */
static int http_req_curl(const char *method, const char *url, const char *headers,
                         const char *body, size_t body_len,
                         char **resp, size_t *resp_len, int *status,
                         char *errbuf, size_t errsz) {
    char hdrf[] = "/tmp/anachron-hdr-XXXXXX";
    char bodyf[] = "/tmp/anachron-body-XXXXXX";
    char respf[] = "/tmp/anachron-resp-XXXXXX";
    int hfd = mkstemp(hdrf), bfd = mkstemp(bodyf), rfd = mkstemp(respf);
    if (hfd < 0 || bfd < 0 || rfd < 0) {
        if (hfd >= 0) { close(hfd); unlink(hdrf); }
        if (bfd >= 0) { close(bfd); unlink(bodyf); }
        if (rfd >= 0) { close(rfd); unlink(respf); }
        if (errbuf) snprintf(errbuf, errsz, "cannot create temp files");
        return -1;
    }
    close(rfd);
    /* curl --config format: one option per line; headers carry the secrets. */
    {
        strbuf cfg; sb_init(&cfg);
        if (body)
            sb_append(&cfg, "header = \"Content-Type: application/json\"\n");
        const char *p = headers ? headers : "";
        while (*p) {
            const char *nl = strstr(p, "\r\n");
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (len > 0) {
                sb_append(&cfg, "header = \"");
                sb_append_n(&cfg, p, len);
                sb_append(&cfg, "\"\n");
            }
            p = nl ? nl + 2 : p + len;
        }
        ssize_t wr = write(hfd, sb_cstr(&cfg), cfg.len);
        (void)wr;
        sb_free(&cfg);
        close(hfd);
    }
    {
        ssize_t wr = write(bfd, body, body_len);
        (void)wr;
        close(bfd);
    }

    strbuf cmd; sb_init(&cmd);
    if (body)
        sb_appendf(&cmd, "curl -sS --max-time 600 -X %s --config %s --data-binary @%s "
                         "-o %s -w '%%{http_code}' '%s'", method, hdrf, bodyf, respf, url);
    else
        sb_appendf(&cmd, "curl -sSL --max-time 120 -X %s --config %s "
                         "-o %s -w '%%{http_code}' '%s'", method, hdrf, respf, url);
    char *out = NULL; size_t olen = 0; int code = -1;
    int rr = plat_run_command(sb_cstr(&cmd), "/tmp", &out, &olen, &code);
    sb_free(&cmd);
    int st = out ? atoi(out) : 0;
    int ok = (rr == 0 && code == 0 && st > 0);
    if (ok) {
        char *buf = NULL; size_t blen = 0;
        if (plat_read_file(respf, &buf, &blen) == 0) {
            *resp = buf;
            *resp_len = blen;
            *status = st;
        } else {
            ok = 0;
            if (errbuf) snprintf(errbuf, errsz, "curl wrote no response file");
        }
    } else if (errbuf) {
        snprintf(errbuf, errsz, "curl failed (%s)",
                 out && *out ? out : "is curl installed?");
    }
    free(out);
    unlink(hdrf); unlink(bodyf); unlink(respf);
    return ok ? 0 : -1;
}

int plat_http_post(const char *url, const char *headers,
                   const char *body, size_t body_len,
                   char **resp, size_t *resp_len, int *status,
                   char *errbuf, size_t errsz) {
    if (errbuf && errsz) errbuf[0] = '\0';
    char *host = NULL, *port = NULL, *path = NULL;
    int https = 0;
    if (url_split(url, &host, &port, &path, &https) != 0) {
        if (errbuf) snprintf(errbuf, errsz, "bad URL: %s", url);
        return -1;
    }
    int rc;
    if (https) {
        rc = http_req_curl("POST", url, headers, body, body_len,
                           resp, resp_len, status, errbuf, errsz);
    } else {
        rc = http_req_socket("POST", host, port, path, headers, body, body_len,
                             resp, resp_len, status, errbuf, errsz);
    }
    free(host); free(port); free(path);
    return rc;
}

#endif /* !_WIN32 */
