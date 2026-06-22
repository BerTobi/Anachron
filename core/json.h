/* json — minimal recursive-descent JSON parser, zero dependencies beyond strbuf.
 * Enough to read the tool-call envelope the model emits: objects, arrays,
 * strings (with full escape + \uXXXX decoding to UTF-8), numbers, bools, null.
 * Not a general-purpose serializer; there is no writer here on purpose. */
#ifndef ANACHRON_JSON_H
#define ANACHRON_JSON_H

#include <stddef.h>
#include "strbuf.h"

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type;

typedef struct json_value json_value;
struct json_value {
    json_type    type;
    char        *str;     /* JSON_STRING: decoded, nul-terminated */
    double       num;     /* JSON_NUMBER */
    int          boolean; /* JSON_BOOL */
    json_value **items;   /* JSON_ARRAY/JSON_OBJECT: element values */
    char       **keys;    /* JSON_OBJECT: member keys (NULL for arrays) */
    size_t       count;   /* element/member count */
};

/* Parse a single JSON value at the start of `text` (leading whitespace skipped;
 * trailing content after the value is ignored). Returns NULL on error and, if
 * `err_out` is non-NULL, points it at a static description. Free with json_free. */
json_value       *json_parse(const char *text, const char **err_out);
const json_value *json_obj_get(const json_value *obj, const char *key);
const char       *json_as_str(const json_value *v); /* NULL unless JSON_STRING */
void              json_free(json_value *v);

/* Append `s` to `out` with JSON string escaping (no surrounding quotes added).
 * Used to build request bodies for the remote backend. */
void              json_escape(strbuf *out, const char *s);

#endif /* ANACHRON_JSON_H */
