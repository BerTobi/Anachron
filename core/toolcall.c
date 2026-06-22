#include "toolcall.h"
#include "json.h"
#include "strbuf.h"

#include <stdlib.h>
#include <string.h>

const char *toolcall_kind_name(tc_kind k) {
    switch (k) {
        case TC_READ_FILE:   return "read_file";
        case TC_WRITE_FILE:  return "write_file";
        case TC_LIST_DIR:    return "list_dir";
        case TC_RUN_COMMAND: return "run_command";
        case TC_EDIT:        return "edit";
        case TC_SEARCH:      return "search";
        case TC_GLOB:        return "glob";
        case TC_PLAN:        return "plan";
        case TC_FINAL:       return "final";
        default:             return "none";
    }
}

void toolcall_free(tool_call *tc) {
    if (!tc) return;
    free(tc->path);
    free(tc->content);
    free(tc->find);
    free(tc->pattern);
    free(tc->cmd);
    free(tc->message);
    free(tc->plan);
    free(tc->error);
    memset(tc, 0, sizeof *tc);
}

static int fail(tool_call *out, const char *msg) {
    out->kind = TC_NONE;
    out->error = xstrdup(msg);
    return -1;
}

/* Pull a required string argument out of the arguments object. */
static char *req_str(const json_value *args, const char *key) {
    const char *s = json_as_str(json_obj_get(args, key));
    return s ? xstrdup(s) : NULL;
}

int toolcall_parse(const char *text, tool_call *out) {
    memset(out, 0, sizeof *out);
    if (!text) return fail(out, "no model output");

    /* Prefer the explicit <tool_call> ... </tool_call> envelope. Fall back to
     * the first bare '{' so a model that forgets the tags still works. */
    const char *json_start = NULL;
    const char *open = strstr(text, "<tool_call>");
    if (open) {
        json_start = open + strlen("<tool_call>");
    } else {
        json_start = strchr(text, '{');
    }
    if (!json_start) return fail(out, "no <tool_call> block or JSON object found");

    const char *err = NULL;
    json_value *root = json_parse(json_start, &err);
    if (!root) return fail(out, err ? err : "tool-call JSON did not parse");

    const char *name = json_as_str(json_obj_get(root, "name"));
    const json_value *args = json_obj_get(root, "arguments");
    if (!name) { json_free(root); return fail(out, "tool call missing string \"name\""); }
    if (!args || args->type != JSON_OBJECT) {
        json_free(root);
        return fail(out, "tool call missing \"arguments\" object");
    }

    int ok = 0;
    if (strcmp(name, "read_file") == 0) {
        out->kind = TC_READ_FILE;
        out->path = req_str(args, "path");
        ok = out->path != NULL;
        const json_value *off = json_obj_get(args, "offset");  /* optional */
        if (off && off->type == JSON_NUMBER) {
            out->offset = (long)off->num;
            if (out->offset < 0) out->offset = 0;
            out->has_offset = 1;
        }
    } else if (strcmp(name, "write_file") == 0) {
        out->kind = TC_WRITE_FILE;
        out->path = req_str(args, "path");
        out->content = req_str(args, "content");
        ok = out->path && out->content;
    } else if (strcmp(name, "list_dir") == 0) {
        out->kind = TC_LIST_DIR;
        out->path = req_str(args, "path");
        ok = out->path != NULL;
    } else if (strcmp(name, "run_command") == 0) {
        out->kind = TC_RUN_COMMAND;
        out->cmd = req_str(args, "cmd");
        ok = out->cmd != NULL;
    } else if (strcmp(name, "edit") == 0) {
        out->kind = TC_EDIT;
        out->path = req_str(args, "path");
        out->find = req_str(args, "old");
        out->content = req_str(args, "new");
        ok = out->path && out->find && out->content;
    } else if (strcmp(name, "search") == 0) {
        out->kind = TC_SEARCH;
        out->pattern = req_str(args, "pattern");
        out->path = req_str(args, "path");   /* optional subdir; NULL -> whole sandbox */
        ok = out->pattern != NULL;
    } else if (strcmp(name, "glob") == 0) {
        out->kind = TC_GLOB;
        out->pattern = req_str(args, "pattern");
        ok = out->pattern != NULL;
    } else if (strcmp(name, "plan") == 0) {
        out->kind = TC_PLAN;
        out->plan = req_str(args, "steps");
        ok = out->plan != NULL;
    } else if (strcmp(name, "final") == 0) {
        out->kind = TC_FINAL;
        out->message = req_str(args, "message");
        ok = out->message != NULL;
    } else {
        json_free(root);
        return fail(out, "unknown tool name");
    }

    json_free(root);
    if (!ok) {
        /* Reset kind so the caller treats it as a re-prompt, but keep a useful note. */
        tc_kind attempted = out->kind;
        toolcall_free(out);
        strbuf m; sb_init(&m);
        sb_appendf(&m, "tool \"%s\" is missing a required argument", toolcall_kind_name(attempted));
        int rc = fail(out, sb_cstr(&m));
        sb_free(&m);
        return rc;
    }
    return 0;
}
