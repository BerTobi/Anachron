/* Unit tests for the platform-independent pieces that are easy to get subtly
 * wrong: the JSON parser, the tool-call parser, and sandbox containment.
 * Built and run by `make test`. assert()-based; relies on asserts being live
 * (no -DNDEBUG in CFLAGS). */
#include "json.h"
#include "toolcall.h"
#include "sandbox.h"
#include "strbuf.h"
#include "verify.h"
#include "obsfmt.h"
#include "edit.h"
#include "glob.h"
#include "gitignore.h"
#include "diff.h"
#include "prompt.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_json(void) {
    const char *err = NULL;

    json_value *v = json_parse(
        "{\"a\":\"hi\\nthere\",\"n\":42,\"b\":true,\"o\":{\"p\":\"q\"},\"arr\":[1,2,3]}", &err);
    assert(v && v->type == JSON_OBJECT);
    assert(strcmp(json_as_str(json_obj_get(v, "a")), "hi\nthere") == 0);
    const json_value *o = json_obj_get(v, "o");
    assert(o && o->type == JSON_OBJECT);
    assert(strcmp(json_as_str(json_obj_get(o, "p")), "q") == 0);
    const json_value *arr = json_obj_get(v, "arr");
    assert(arr && arr->type == JSON_ARRAY && arr->count == 3);
    json_free(v);

    /* é ('é') must decode to UTF-8 0xC3 0xA9. */
    v = json_parse("\"\\u00e9\"", &err);
    assert(v && v->type == JSON_STRING);
    assert((unsigned char)v->str[0] == 0xC3 &&
           (unsigned char)v->str[1] == 0xA9 &&
           v->str[2] == '\0');
    json_free(v);

    /* malformed input returns NULL, not a crash */
    v = json_parse("{bad", &err);
    assert(v == NULL && err != NULL);

    printf("  json: ok\n");
}

static void test_toolcall(void) {
    tool_call tc;

    /* prose + tags + trailing text; multiline escaped content */
    int rc = toolcall_parse(
        "sure!\n<tool_call>{\"name\":\"write_file\",\"arguments\":"
        "{\"path\":\"a/b.c\",\"content\":\"line1\\nline2\\n\"}}</tool_call> trailing", &tc);
    assert(rc == 0 && tc.kind == TC_WRITE_FILE);
    assert(strcmp(tc.path, "a/b.c") == 0);
    assert(strcmp(tc.content, "line1\nline2\n") == 0);
    toolcall_free(&tc);

    /* no tool call at all -> recoverable failure */
    rc = toolcall_parse("just some text", &tc);
    assert(rc != 0 && tc.kind == TC_NONE && tc.error != NULL);
    toolcall_free(&tc);

    /* missing required argument -> failure */
    rc = toolcall_parse("<tool_call>{\"name\":\"read_file\",\"arguments\":{}}</tool_call>", &tc);
    assert(rc != 0 && tc.kind == TC_NONE);
    toolcall_free(&tc);

    /* final */
    rc = toolcall_parse("<tool_call>{\"name\":\"final\",\"arguments\":{\"message\":\"done\"}}</tool_call>", &tc);
    assert(rc == 0 && tc.kind == TC_FINAL && strcmp(tc.message, "done") == 0);
    toolcall_free(&tc);

    /* plan (parsed whenever present; the agent only OFFERS it under --plan) */
    rc = toolcall_parse("<tool_call>{\"name\":\"plan\",\"arguments\":{\"steps\":\"1. write x\\n2. run x\"}}</tool_call>", &tc);
    assert(rc == 0 && tc.kind == TC_PLAN && strcmp(tc.plan, "1. write x\n2. run x") == 0);
    toolcall_free(&tc);

    /* bare JSON object (no tags) still works as a fallback */
    rc = toolcall_parse("{\"name\":\"list_dir\",\"arguments\":{\"path\":\".\"}}", &tc);
    assert(rc == 0 && tc.kind == TC_LIST_DIR && strcmp(tc.path, ".") == 0);
    toolcall_free(&tc);

    printf("  toolcall: ok\n");
}

static void test_sandbox(void) {
    char *abs = NULL;

    assert(sandbox_resolve("/root", "a/b.c", &abs) == 0);
    assert(strcmp(abs, "/root/a/b.c") == 0);
    free(abs);

    assert(sandbox_resolve("/root", "a/../b", &abs) == 0);
    assert(strcmp(abs, "/root/b") == 0);
    free(abs);

    assert(sandbox_resolve("/root", ".", &abs) == 0);
    assert(strcmp(abs, "/root") == 0);
    free(abs);

    /* leading separator is treated as relative, never as an absolute escape */
    assert(sandbox_resolve("/root", "/etc/passwd", &abs) == 0);
    assert(strcmp(abs, "/root/etc/passwd") == 0);
    free(abs);

    /* escapes are rejected */
    assert(sandbox_resolve("/root", "../x", &abs) == -1);
    assert(sandbox_resolve("/root", "a/../../x", &abs) == -1);
    /* drive / stream specifier rejected */
    assert(sandbox_resolve("/root", "C:\\windows", &abs) == -1);

    printf("  sandbox: ok\n");
}

static void test_verify(void) {
    char *e;

    /* structurally sound C -> NULL */
    e = verify_balance("int add(int a, int b) {\n    return a + b;\n}\n");
    assert(e == NULL);

    /* missing closing brace -> rejected */
    e = verify_balance("int f(void) {\n    return 0;\n");
    assert(e != NULL); free(e);

    /* stray closing brace -> rejected */
    e = verify_balance("}\n");
    assert(e != NULL); free(e);

    /* unterminated string -> rejected */
    e = verify_balance("char *s = \"oops;\n");
    assert(e != NULL); free(e);

    /* braces/parens inside strings and comments must NOT count */
    e = verify_balance("char *s = \"}}})\"; // {{{ ((( \n int x = 0;\n");
    assert(e == NULL);
    e = verify_balance("/* } { ( */ int y = ([{}]);\n");
    assert(e == NULL);

    /* extension classifiers */
    assert(verify_is_c("foo.c") && verify_is_c("a/b.h") && !verify_is_c("readme.md"));
    assert(verify_is_codeish("x.js") && verify_is_codeish("p.json") && !verify_is_codeish("notes.txt"));

    printf("  verify: ok\n");
}

static void test_repair(void) {
    int n; char *r;

    /* raw newline inside a string literal -> escaped to \n; result is sound C */
    r = verify_repair_literals("printf(\"Too low!\n\");\n", &n);
    assert(r != NULL && n == 1);
    assert(strstr(r, "\"Too low!\\n\"") != NULL);   /* literal closes on its own line */
    assert(verify_balance(r) == NULL);              /* and the structure is valid */
    free(r);

    /* clean code with no in-literal newlines -> NULL (nothing to repair) */
    r = verify_repair_literals("int main(void) {\n    return 0;\n}\n", &n);
    assert(r == NULL && n == 0);

    /* an already-escaped \n must be left alone (no false positive) */
    r = verify_repair_literals("puts(\"hi\\n\");\n", &n);
    assert(r == NULL && n == 0);

    /* char literal holding a raw newline -> escaped */
    r = verify_repair_literals("char c = '\n';\n", &n);
    assert(r != NULL && n == 1 && strstr(r, "'\\n'") != NULL);
    free(r);

    /* a real newline in a // comment is not inside a literal -> untouched */
    r = verify_repair_literals("// hi\nint x;\n", &n);
    assert(r == NULL && n == 0);

    /* several newlines in one string -> each escaped */
    r = verify_repair_literals("\"a\nb\nc\"", &n);
    assert(r != NULL && n == 2 && strcmp(r, "\"a\\nb\\nc\"") == 0);
    free(r);

    printf("  repair: ok\n");
}

static void test_obsfmt(void) {
    char *r;

    /* short text passes through unchanged */
    r = obs_capped("a\nb\nc\n", 200, 8192);
    assert(strcmp(r, "a\nb\nc\n") == 0);
    free(r);

    /* line cap: 5 lines, max 3 -> first 3 + a note mentioning 2 more / 5 total */
    r = obs_capped("1\n2\n3\n4\n5\n", 3, 8192);
    assert(strncmp(r, "1\n2\n3\n", 6) == 0);
    assert(strstr(r, "2 more") != NULL && strstr(r, "5 total") != NULL);
    free(r);

    /* byte cap kicks in before the line cap */
    r = obs_capped("xxxxxxxxxx\nyyyy\n", 200, 4);
    assert(strstr(r, "more line") != NULL || strstr(r, "truncated") != NULL);
    free(r);

    /* empty stays empty (no spurious note) */
    r = obs_capped("", 200, 8192);
    assert(r[0] == '\0');
    free(r);

    /* windowed paging: 5 lines, window of 2 from offset 0 -> shows 1-2, footer to offset 2 */
    r = obs_window("a\nb\nc\nd\ne\n", 0, 2, 8192);
    assert(strstr(r, "lines 1-2 of 5") && strstr(r, "a\nb\n"));
    assert(strstr(r, "offset=2"));
    free(r);
    /* second page from offset 2 -> lines 3-4, still more */
    r = obs_window("a\nb\nc\nd\ne\n", 2, 2, 8192);
    assert(strstr(r, "lines 3-4 of 5") && strstr(r, "c\nd\n") && strstr(r, "offset=4"));
    free(r);
    /* final page -> no footer (nothing more) */
    r = obs_window("a\nb\nc\nd\ne\n", 4, 2, 8192);
    assert(strstr(r, "lines 5-5 of 5") && !strstr(r, "offset="));
    free(r);
    /* offset past end is reported, not a crash */
    r = obs_window("a\nb\n", 9, 2, 8192);
    assert(strstr(r, "past end"));
    free(r);
    /* last line without trailing newline still counted */
    r = obs_window("a\nb\nc", 0, 10, 8192);
    assert(strstr(r, "lines 1-3 of 3"));
    free(r);
    /* an over-long line is shown WHOLE (soft byte cap), not truncated or lost */
    r = obs_window("0123456789\nnext\n", 0, 10, 4);
    assert(strstr(r, "0123456789"));            /* full first line present */
    assert(strstr(r, "lines 1-1 of 2") && strstr(r, "offset=1")); /* advance past it, no loss */
    free(r);

    printf("  obsfmt: ok\n");
}

static void test_edit(void) {
    const char *err;
    char *r;

    /* exact unique replace */
    r = edit_apply("int a;\nint b;\nint c;\n", "int b;", "int B;", &err);
    assert(r && strcmp(r, "int a;\nint B;\nint c;\n") == 0);
    free(r);

    /* not found -> NULL + error */
    r = edit_apply("hello\n", "nope", "x", &err);
    assert(r == NULL && err != NULL);

    /* ambiguous exact match -> refused */
    r = edit_apply("x\nx\n", "x", "y", &err);
    assert(r == NULL && err != NULL);

    /* whitespace-tolerant line match: old lacks the file's indentation */
    r = edit_apply("    foo();\nbar();\n", "foo();", "baz();", &err);
    assert(r && strstr(r, "baz();") && strstr(r, "bar();"));
    assert(strstr(r, "foo();") == NULL);
    free(r);

    /* multi-line fuzzy block (trailing whitespace drift) */
    r = edit_apply("if (x) {\n    do_it();\n}\n", "if (x) {  \n    do_it();\n}", "if (y) {\n    go();\n}", &err);
    assert(r && strstr(r, "if (y)") && strstr(r, "go();"));
    free(r);

    printf("  edit: ok\n");
}

static void test_glob(void) {
    assert(glob_match("*.c", "main.c"));
    assert(glob_match("*.c", "a.c"));
    assert(!glob_match("*.c", "main.h"));
    assert(glob_match("test_*.c", "test_core.c"));
    assert(!glob_match("test_*.c", "core.c"));
    assert(glob_match("*", "anything"));
    assert(glob_match("a?c", "abc") && !glob_match("a?c", "ac"));
    assert(glob_match("*x*y*", "axby") && !glob_match("*x*y*", "ayx"));
    assert(glob_match("foo", "foo") && !glob_match("foo", "foobar"));
    assert(glob_match("", "") && !glob_match("", "x"));
    printf("  glob: ok\n");
}

static void test_gitignore(void) {
    /* NULL/empty set ignores nothing. */
    assert(gitignore_parse(NULL) == NULL);
    assert(gitignore_parse("\n# just a comment\n   \n") == NULL);
    assert(gitignore_match(NULL, "anything", 0) == 0);

    gitignore *gi = gitignore_parse(
        "# comment\n"
        "*.log\n"            /* floating: matches basename at any depth */
        "build/\n"           /* directory-only */
        "/root.txt\n"        /* anchored to root */
        "!keep.log\n"        /* negation un-ignores */
        "docs/*.md\n");      /* anchored path pattern */
    assert(gi != NULL);

    assert(gitignore_match(gi, "a.log", 0));
    assert(gitignore_match(gi, "sub/dir/b.log", 0));   /* floating matches at depth */
    assert(!gitignore_match(gi, "keep.log", 0));       /* negated */
    assert(gitignore_match(gi, "build", 1));           /* dir-only matches a dir */
    assert(!gitignore_match(gi, "build", 0));          /* ...but not a file named build */
    assert(gitignore_match(gi, "root.txt", 0));        /* anchored at root */
    assert(!gitignore_match(gi, "sub/root.txt", 0));   /* not at depth */
    assert(gitignore_match(gi, "docs/readme.md", 0));  /* anchored path */
    assert(!gitignore_match(gi, "src/main.c", 0));     /* unmatched */
    gitignore_free(gi);

    /* Leading spaces are significant (git semantics), not trimmed. */
    gitignore *g2 = gitignore_parse(" foo\n");
    assert(g2 && !gitignore_match(g2, "foo", 0));   /* " foo" must not match "foo" */
    gitignore_free(g2);

    /* Negation matcher is last-match-wins even under an excluded dir; the walk
     * (not the matcher) is what prevents re-inclusion by pruning the dir. */
    gitignore *g3 = gitignore_parse("logs/\n!logs/keep.txt\n");
    assert(g3 && gitignore_match(g3, "logs", 1));          /* dir excluded */
    assert(!gitignore_match(g3, "logs/keep.txt", 0));      /* matcher un-ignores it */
    gitignore_free(g3);
    printf("  gitignore: ok\n");
}

static void test_diff(void) {
    strbuf d; sb_init(&d);

    /* Identical inputs: no diff, returns 0, nothing appended. */
    assert(diff_unified("a\nb\nc\n", "a\nb\nc\n", &d, 0) == 0);
    assert(d.len == 0);

    /* A single changed line shows a '-' and a '+'. */
    sb_clear(&d);
    assert(diff_unified("a\nb\nc\n", "a\nX\nc\n", &d, 0) == 1);
    assert(strstr(sb_cstr(&d), "- b\n") != NULL);
    assert(strstr(sb_cstr(&d), "+ X\n") != NULL);
    assert(strstr(sb_cstr(&d), "  a\n") != NULL);   /* context kept */

    /* Pure addition. */
    sb_clear(&d);
    assert(diff_unified("a\n", "a\nb\n", &d, 0) == 1);
    assert(strstr(sb_cstr(&d), "+ b\n") != NULL);

    /* Colour mode wraps changed lines in ANSI codes. */
    sb_clear(&d);
    diff_unified("a\n", "b\n", &d, 1);
    assert(strstr(sb_cstr(&d), "\x1b[31m") != NULL);   /* red for removal */
    assert(strstr(sb_cstr(&d), "\x1b[32m") != NULL);   /* green for addition */

    sb_free(&d);
    printf("  diff: ok\n");
}

static char *big(char c, size_t n) {
    char *s = malloc(n + 1);
    memset(s, c, n);
    s[n] = '\0';
    return s;
}

static void test_history_shrink(void) {
    /* Build a history: task, a tool result, a huge assistant message (code), then
     * two recent turns. Shrinking must (1) elide the tool result first, then (2)
     * truncate the big old assistant message, while never touching index 0 or the
     * last two turns. */
    history h;
    history_init(&h);
    char *code = big('x', 2000);
    char *recent = big('y', 2000);
    history_push(&h, MSG_USER, "original task");        /* 0: pinned */
    history_push(&h, MSG_TOOL_RESULT, "lots of tool output here ...");  /* 1 */
    history_push(&h, MSG_ASSISTANT, code);              /* 2: big, old, truncatable */
    history_push(&h, MSG_USER, "newer question");       /* 3: recent */
    history_push(&h, MSG_ASSISTANT, recent);            /* 4: recent, must survive */
    free(code); free(recent);

    /* (1) first shrink elides the oldest tool result. */
    assert(history_shrink(&h) == 1);
    assert(h.items[1].elided && strstr(h.items[1].text, "elided"));

    /* (2) next shrink truncates the big old assistant message (index 2). */
    size_t before = strlen(h.items[2].text);
    assert(history_shrink(&h) == 1);
    assert(strlen(h.items[2].text) < before);
    assert(strstr(h.items[2].text, "elided"));

    /* Pinned task and the two most recent turns are never cut. */
    assert(strcmp(h.items[0].text, "original task") == 0);
    assert(strlen(h.items[4].text) == 2000);   /* recent assistant untouched */

    /* Eventually there is nothing left to shrink. */
    int guard = 0;
    while (history_shrink(&h) && guard < 100) guard++;
    assert(history_shrink(&h) == 0);
    history_free(&h);
    printf("  history_shrink: ok\n");
}

int main(void) {
    test_json();
    test_toolcall();
    test_sandbox();
    test_verify();
    test_repair();
    test_obsfmt();
    test_edit();
    test_glob();
    test_gitignore();
    test_diff();
    test_history_shrink();
    printf("ALL TESTS PASSED\n");
    return 0;
}
