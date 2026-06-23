#!/bin/sh
# Auto-repair of raw newlines inside C literals, end to end, via the stub backend.
# The scripted write puts a real newline INSIDE printf("hi<newline>") - the exact
# weak-model defect. The harness must escape it to \n, ACCEPT the write (not reject),
# and report the auto-escape. Passes with or without a C compiler present.
set -e

cd "$(dirname "$0")/.."
make anachron >/dev/null

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "sandbox: $TMP"
echo

OUT=$(ANACHRON_STUB_SCRIPT=tests/repair-script.txt ./anachron --sandbox "$TMP" "repair test" 2>&1 || true)
echo "$OUT" | grep -E 'write_file|result|auto-escaped|Wrote|REJECTED|final|==' || true

echo
echo "--- assertions ---"
echo "$OUT" | grep -q 'auto-escaped 1 raw newline' || { echo "FAIL: expected the auto-escape note"; exit 1; }
echo "ok: the raw newline inside the literal was auto-escaped"
if echo "$OUT" | grep -q 'REJECTED'; then echo "FAIL: write should NOT be rejected after repair"; exit 1; fi
echo "ok: write was accepted (not rejected)"
if [ ! -f "$TMP/hello.c" ]; then echo "FAIL: hello.c should exist"; exit 1; fi
grep -q 'hi\\n' "$TMP/hello.c" || { echo "FAIL: file literal not escaped to hi\\n"; exit 1; }
echo "ok: hello.c contains the escaped literal (hi\\n), no raw line break"
echo
echo "REPAIR-E2E PASS"
