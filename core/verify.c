#include "verify.h"
#include "strbuf.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static char *dupf(const char *fmt, int n) {
    strbuf s;
    sb_init(&s);
    sb_appendf(&s, fmt, n);
    char *r = xstrdup(sb_cstr(&s));
    sb_free(&s);
    return r;
}

/* Return the file's extension (including the leading '.'), or NULL. Resets at
 * path separators so "dir.x/file" without an extension returns NULL. */
static const char *ext_of(const char *path) {
    const char *dot = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') dot = NULL;
        else if (*p == '.') dot = p;
    }
    return dot;
}

static int ext_in(const char *path, const char *const *exts) {
    const char *e = ext_of(path);
    if (!e) return 0;
    for (size_t i = 0; exts[i]; i++) {
        const char *a = e, *b = exts[i];
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*a && !*b) return 1;
    }
    return 0;
}

int verify_is_c(const char *path) {
    static const char *const c[] = { ".c", ".h", ".cpp", ".cc", ".cxx", ".hpp", ".hh", ".hxx", NULL };
    return ext_in(path, c);
}

int verify_is_codeish(const char *path) {
    static const char *const code[] = {
        ".c", ".h", ".cpp", ".cc", ".cxx", ".hpp", ".hh", ".hxx",
        ".js", ".ts", ".jsx", ".tsx", ".json", ".java", ".go", ".rs", ".css", NULL
    };
    return ext_in(path, code);
}

char *verify_balance(const char *code) {
    int paren = 0, brace = 0, brack = 0, line = 1;
    enum { NORMAL, STR, CH, LINE_C, BLOCK_C } st = NORMAL;

    for (const char *p = code; *p; p++) {
        char c = *p;
        if (c == '\n') line++;
        switch (st) {
            case NORMAL:
                if (c == '"')                         st = STR;
                else if (c == '\'')                   st = CH;
                else if (c == '/' && p[1] == '/')   { st = LINE_C; p++; }
                else if (c == '/' && p[1] == '*')   { st = BLOCK_C; p++; }
                else if (c == '(')                    paren++;
                else if (c == ')' && --paren < 0)     return dupf("unmatched ')' near line %d", line);
                else if (c == '{')                    brace++;
                else if (c == '}' && --brace < 0)     return dupf("unmatched '}' near line %d", line);
                else if (c == '[')                    brack++;
                else if (c == ']' && --brack < 0)     return dupf("unmatched ']' near line %d", line);
                break;
            case STR:
                if (c == '\\' && p[1]) { if (p[1] == '\n') line++; p++; }
                else if (c == '"')     st = NORMAL;
                break;
            case CH:
                if (c == '\\' && p[1]) { if (p[1] == '\n') line++; p++; }
                else if (c == '\'')    st = NORMAL;
                break;
            case LINE_C:
                if (c == '\n') st = NORMAL;
                break;
            case BLOCK_C:
                if (c == '*' && p[1] == '/') { st = NORMAL; p++; }
                break;
        }
    }

    if (st == STR)     return xstrdup("unterminated string literal (missing closing quote)");
    if (st == BLOCK_C) return xstrdup("unterminated block comment (missing closing */)");
    if (brace)         return dupf("unbalanced braces: %d unclosed '{'", brace);
    if (paren)         return dupf("unbalanced parentheses: %d unclosed '('", paren);
    if (brack)         return dupf("unbalanced brackets: %d unclosed '['", brack);
    return NULL;
}
