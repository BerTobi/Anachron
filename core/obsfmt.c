#include "obsfmt.h"
#include "strbuf.h"

#include <string.h>

char *obs_capped(const char *text, size_t max_lines, size_t max_bytes) {
    size_t len = strlen(text);

    /* total line count (count the final unterminated line too) */
    size_t total = 0;
    for (size_t i = 0; i < len; i++) if (text[i] == '\n') total++;
    if (len && text[len - 1] != '\n') total++;

    /* cutoff = first of: max_lines complete lines, or max_bytes bytes */
    size_t off = 0, lines = 0;
    while (off < len && lines < max_lines && off < max_bytes) {
        if (text[off] == '\n') lines++;
        off++;
    }

    strbuf sb;
    sb_init(&sb);
    sb_append_n(&sb, text, off);
    if (off < len) {
        size_t more = total > lines ? total - lines : 0;
        if (more > 0)
            sb_appendf(&sb, "\n... (%zu more line%s not shown; %zu total)",
                       more, more == 1 ? "" : "s", total);
        else
            sb_appendf(&sb, "\n... (output truncated; %zu lines total)", total);
    }
    char *r = xstrdup(sb_cstr(&sb));
    sb_free(&sb);
    return r;
}

char *obs_window(const char *text, size_t offset, size_t count, size_t max_bytes) {
    size_t len = strlen(text);
    size_t total = 0;
    for (size_t i = 0; i < len; i++) if (text[i] == '\n') total++;
    if (len && text[len - 1] != '\n') total++;

    strbuf sb;
    sb_init(&sb);
    if (total == 0) { sb_append(&sb, "(empty file)"); goto done; }
    if (offset >= total) {
        sb_appendf(&sb, "(offset %zu is past end of file; %zu lines total)", offset, total);
        goto done;
    }

    /* skip `offset` lines */
    size_t start = 0, skipped = 0;
    while (start < len && skipped < offset) { if (text[start] == '\n') skipped++; start++; }

    /* Emit WHOLE lines only: up to `count` lines, stopping before a line that would
     * push the window past `max_bytes` — but always include at least one line, so a
     * single over-long line is shown intact (a soft byte cap) and paging can't lose
     * its tail or spin on a partial line. */
    size_t q = start, shown = 0;
    while (q < len && shown < count) {
        size_t le = q;
        while (le < len && text[le] != '\n') le++;
        size_t lend = (le < len) ? le + 1 : le;     /* include the newline */
        if (shown > 0 && (lend - start) > max_bytes) break;
        q = lend;
        shown++;
        if (le >= len) break;                        /* final line without a newline */
    }
    size_t next = offset + shown;

    sb_appendf(&sb, "(lines %zu-%zu of %zu)\n", offset + 1, offset + shown, total);
    sb_append_n(&sb, text + start, q - start);
    if (next < total)
        sb_appendf(&sb, "\n(... %zu more lines; read_file offset=%zu to continue)",
                   total - next, next);
done:
    {
        char *r = xstrdup(sb_cstr(&sb));
        sb_free(&sb);
        return r;
    }
}
