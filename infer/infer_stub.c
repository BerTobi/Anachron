/* infer_stub — a fake backend so the entire agent loop is testable on the dev
 * host with no model and no llama.cpp. It does NOT run a network or a model.
 *
 * Two modes:
 *   - Default: emit one `final` tool call. Proves the wiring end to end.
 *   - Scripted: if $ANACHRON_STUB_SCRIPT names a file, each generate() call
 *     returns the next entry from it. Entries are separated by a line that is
 *     exactly "===". This lets a test drive list_dir -> write -> read -> run ->
 *     final and assert real filesystem effects.
 *
 * Output is streamed to on_token in small chunks to exercise the streaming path. */
#include "infer_backend.h"
#include "strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct stub_impl {
    char **entries;   /* scripted model outputs, in order */
    size_t count;
    size_t idx;       /* next entry to emit */
    int last_prompt_tokens;      /* ~chars/4 approximation for the usage display */
    int last_completion_tokens;
};

static const char *DEFAULT_FINAL =
    "<tool_call>{\"name\": \"final\", \"arguments\": {\"message\": "
    "\"Stub backend active: no model is loaded. Set ANACHRON_STUB_SCRIPT to a "
    "script file to exercise the agent loop, or build with the llama backend.\"}}"
    "</tool_call>";

/* Split file contents on lines that are exactly "===" into a list of entries. */
static void load_script(struct stub_impl *c, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);
    char *buf = xmalloc((size_t)n + 1);
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = '\0';
    fclose(f);

    strbuf cur; sb_init(&cur);
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        /* tolerate a trailing \r on the delimiter line */
        int is_delim = (llen == 3 && strncmp(line, "===", 3) == 0) ||
                       (llen == 4 && strncmp(line, "===\r", 4) == 0);
        if (is_delim) {
            c->entries = xrealloc(c->entries, (c->count + 1) * sizeof *c->entries);
            c->entries[c->count++] = xstrdup(sb_cstr(&cur));
            sb_clear(&cur);
        } else {
            sb_append_n(&cur, line, llen);
            sb_putc(&cur, '\n');
        }
        line = nl ? nl + 1 : NULL;
    }
    if (cur.len > 0) {
        c->entries = xrealloc(c->entries, (c->count + 1) * sizeof *c->entries);
        c->entries[c->count++] = xstrdup(sb_cstr(&cur));
    }
    sb_free(&cur);
    free(buf);
}

static int stub_generate(void *impl, const char *prompt, const char *grammar,
                         void (*on_token)(const char *piece, void *ud), void *ud) {
    struct stub_impl *c = impl;
    (void)grammar;
    const char *out = DEFAULT_FINAL;
    if (c->entries && c->idx < c->count) out = c->entries[c->idx++];

    /* No real tokenizer here; approximate ~4 chars/token for the usage display. */
    c->last_prompt_tokens = (int)(strlen(prompt) / 4);
    c->last_completion_tokens = (int)(strlen(out) / 4);

    /* Stream in ~12-byte chunks so the UI's token path is exercised. */
    size_t len = strlen(out);
    char chunk[16];
    for (size_t i = 0; i < len; i += 12) {
        size_t k = (len - i < 12) ? (len - i) : 12;
        memcpy(chunk, out + i, k);
        chunk[k] = '\0';
        if (on_token) on_token(chunk, ud);
    }
    return 0;
}

static void stub_last_usage(const void *impl, int *prompt_tokens, int *completion_tokens) {
    const struct stub_impl *c = impl;
    if (prompt_tokens) *prompt_tokens = c ? c->last_prompt_tokens : 0;
    if (completion_tokens) *completion_tokens = c ? c->last_completion_tokens : 0;
}

static void stub_free(void *impl) {
    struct stub_impl *c = impl;
    if (!c) return;
    for (size_t i = 0; i < c->count; i++) free(c->entries[i]);
    free(c->entries);
    free(c);
}

int stub_backend_open(const char *spec, int n_ctx, infer_backend *out) {
    (void)spec;
    (void)n_ctx;
    struct stub_impl *c = xmalloc(sizeof *c);
    c->entries = NULL;
    c->count = 0;
    c->idx = 0;
    c->last_prompt_tokens = 0;
    c->last_completion_tokens = 0;
    const char *script = getenv("ANACHRON_STUB_SCRIPT");
    if (script && *script) load_script(c, script);
    out->impl = c;
    out->generate = stub_generate;
    out->set_chat = NULL;
    out->last_usage = stub_last_usage;
    out->free_impl = stub_free;
    return 0;
}
