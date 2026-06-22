#include "json.h"
#include "strbuf.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    const char *err;
} jp;

static json_value *parse_value(jp *j);

static void skip_ws(jp *j) {
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')
        j->p++;
}

static json_value *jv_new(json_type t) {
    json_value *v = xmalloc(sizeof *v);
    memset(v, 0, sizeof *v);
    v->type = t;
    return v;
}

static void append_utf8(strbuf *sb, unsigned cp) {
    if (cp < 0x80) {
        sb_putc(sb, (char)cp);
    } else if (cp < 0x800) {
        sb_putc(sb, (char)(0xC0 | (cp >> 6)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_putc(sb, (char)(0xE0 | (cp >> 12)));
        sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_putc(sb, (char)(0xF0 | (cp >> 18)));
        sb_putc(sb, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

static int hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

/* Parse a JSON string literal (cursor must be at the opening quote) and return
 * the decoded, malloc'd contents. Sets j->err and returns NULL on failure. */
static char *parse_string_raw(jp *j) {
    j->p++; /* skip opening quote */
    strbuf sb;
    sb_init(&sb);
    while (*j->p && *j->p != '"') {
        char c = *j->p;
        if (c == '\\') {
            j->p++;
            char e = *j->p;
            switch (e) {
                case '"':  sb_putc(&sb, '"');  break;
                case '\\': sb_putc(&sb, '\\'); break;
                case '/':  sb_putc(&sb, '/');  break;
                case 'b':  sb_putc(&sb, '\b'); break;
                case 'f':  sb_putc(&sb, '\f'); break;
                case 'n':  sb_putc(&sb, '\n'); break;
                case 'r':  sb_putc(&sb, '\r'); break;
                case 't':  sb_putc(&sb, '\t'); break;
                case 'u': {
                    unsigned cp;
                    if (hex4(j->p + 1, &cp) != 0) {
                        j->err = "bad \\u escape";
                        sb_free(&sb);
                        return NULL;
                    }
                    j->p += 4;
                    /* combine UTF-16 surrogate pair if present */
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        j->p[1] == '\\' && j->p[2] == 'u') {
                        unsigned lo;
                        if (hex4(j->p + 3, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            j->p += 6;
                        }
                    }
                    append_utf8(&sb, cp);
                    break;
                }
                default:
                    j->err = "bad escape";
                    sb_free(&sb);
                    return NULL;
            }
            j->p++;
        } else {
            sb_putc(&sb, c);
            j->p++;
        }
    }
    if (*j->p != '"') {
        j->err = "unterminated string";
        sb_free(&sb);
        return NULL;
    }
    j->p++; /* skip closing quote */
    char *out = xstrndup(sb.data ? sb.data : "", sb.len);
    sb_free(&sb);
    return out;
}

static void obj_push(json_value *v, char *key, json_value *val) {
    v->items = xrealloc(v->items, (v->count + 1) * sizeof *v->items);
    v->keys  = xrealloc(v->keys,  (v->count + 1) * sizeof *v->keys);
    v->keys[v->count]  = key;
    v->items[v->count] = val;
    v->count++;
}

static void arr_push(json_value *v, json_value *val) {
    v->items = xrealloc(v->items, (v->count + 1) * sizeof *v->items);
    v->items[v->count++] = val;
}

static json_value *parse_object(jp *j) {
    json_value *v = jv_new(JSON_OBJECT);
    j->p++; /* '{' */
    skip_ws(j);
    if (*j->p == '}') { j->p++; return v; }
    for (;;) {
        skip_ws(j);
        if (*j->p != '"') { j->err = "expected object key"; json_free(v); return NULL; }
        char *key = parse_string_raw(j);
        if (!key) { json_free(v); return NULL; }
        skip_ws(j);
        if (*j->p != ':') { j->err = "expected ':'"; free(key); json_free(v); return NULL; }
        j->p++;
        json_value *val = parse_value(j);
        if (!val) { free(key); json_free(v); return NULL; }
        obj_push(v, key, val);
        skip_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; break; }
        j->err = "expected ',' or '}'";
        json_free(v);
        return NULL;
    }
    return v;
}

static json_value *parse_array(jp *j) {
    json_value *v = jv_new(JSON_ARRAY);
    j->p++; /* '[' */
    skip_ws(j);
    if (*j->p == ']') { j->p++; return v; }
    for (;;) {
        json_value *val = parse_value(j);
        if (!val) { json_free(v); return NULL; }
        arr_push(v, val);
        skip_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == ']') { j->p++; break; }
        j->err = "expected ',' or ']'";
        json_free(v);
        return NULL;
    }
    return v;
}

static json_value *parse_value(jp *j) {
    skip_ws(j);
    char c = *j->p;
    switch (c) {
        case '"': {
            char *s = parse_string_raw(j);
            if (!s) return NULL;
            json_value *v = jv_new(JSON_STRING);
            v->str = s;
            return v;
        }
        case '{': return parse_object(j);
        case '[': return parse_array(j);
        case 't':
            if (strncmp(j->p, "true", 4) == 0) { j->p += 4; json_value *v = jv_new(JSON_BOOL); v->boolean = 1; return v; }
            break;
        case 'f':
            if (strncmp(j->p, "false", 5) == 0) { j->p += 5; json_value *v = jv_new(JSON_BOOL); v->boolean = 0; return v; }
            break;
        case 'n':
            if (strncmp(j->p, "null", 4) == 0) { j->p += 4; return jv_new(JSON_NULL); }
            break;
        default:
            if (c == '-' || (c >= '0' && c <= '9')) {
                char *end;
                double d = strtod(j->p, &end);
                if (end == j->p) { j->err = "bad number"; return NULL; }
                j->p = end;
                json_value *v = jv_new(JSON_NUMBER);
                v->num = d;
                return v;
            }
            break;
    }
    j->err = "unexpected token";
    return NULL;
}

json_value *json_parse(const char *text, const char **err_out) {
    jp j = { text, NULL };
    json_value *v = parse_value(&j);
    if (err_out) *err_out = v ? NULL : (j.err ? j.err : "parse error");
    return v;
}

const json_value *json_obj_get(const json_value *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->count; i++)
        if (strcmp(obj->keys[i], key) == 0) return obj->items[i];
    return NULL;
}

const char *json_as_str(const json_value *v) {
    return (v && v->type == JSON_STRING) ? v->str : NULL;
}

void json_escape(strbuf *out, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  sb_append(out, "\\\""); break;
            case '\\': sb_append(out, "\\\\"); break;
            case '\n': sb_append(out, "\\n");  break;
            case '\r': sb_append(out, "\\r");  break;
            case '\t': sb_append(out, "\\t");  break;
            case '\b': sb_append(out, "\\b");  break;
            case '\f': sb_append(out, "\\f");  break;
            default:
                if (c < 0x20) sb_appendf(out, "\\u%04x", (unsigned)c);
                else          sb_putc(out, (char)c);
        }
    }
}

void json_free(json_value *v) {
    if (!v) return;
    free(v->str);
    for (size_t i = 0; i < v->count; i++) {
        if (v->keys) free(v->keys[i]);
        json_free(v->items[i]);
    }
    free(v->items);
    free(v->keys);
    free(v);
}
