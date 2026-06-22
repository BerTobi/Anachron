/* verify — structural verification of file content before a write is committed.
 * Platform-independent, pure string logic (lives in /core, unit-tested directly).
 * This is the cheap, always-available half of the verify-on-write guardrail; the
 * tools layer adds an optional compiler syntax check on top for C files. */
#ifndef ANACHRON_VERIFY_H
#define ANACHRON_VERIFY_H

/* Scan brace-language source for the first structural problem: unbalanced
 * parentheses, braces or brackets, or an unterminated string or block comment.
 * String literals, char literals, line comments and block comments are skipped
 * so their contents do not count toward the balance. Returns a malloc'd
 * description of the problem, or NULL if the content is structurally sound. */
char *verify_balance(const char *code);

/* Extension classifiers (case-insensitive, based on the path's suffix). */
int verify_is_codeish(const char *path); /* a brace language we can balance-check */
int verify_is_c(const char *path);       /* C/C++ — eligible for a compiler syntax check */

#endif /* ANACHRON_VERIFY_H */
