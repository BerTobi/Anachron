#!/bin/sh
# End-to-end smoke test: drive the full agent loop with the scripted stub backend
# and assert the tool calls produced real filesystem effects in a temp sandbox.
set -e

cd "$(dirname "$0")/.."
make anachron >/dev/null

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "sandbox: $TMP"
echo

ANACHRON_STUB_SCRIPT=tests/demo-script.txt ./anachron --sandbox "$TMP" "create and verify hello.txt"

echo
echo "--- assertions ---"
test -f "$TMP/hello.txt"            && echo "ok: hello.txt was created"
grep -q "hello from anachron" "$TMP/hello.txt" && echo "ok: content matches"
echo
echo "E2E PASS"
