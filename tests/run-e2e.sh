#!/bin/sh
# End-to-end smoke test: drive the full agent loop with the scripted stub backend
# and assert the tool calls produced real filesystem effects in a temp sandbox.
set -e
export ANACHRON_NO_CONFIG=1   # hermetic: never read a developer agent.json

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

# A reply that mixes prose with the closing <tool_call> JSON (frontier-model
# style) must show the prose and hide the protocol tag.
OUT=$(ANACHRON_STUB_SCRIPT=tests/mixed-script.txt ./anachron --sandbox "$TMP" --yolo "mixed reply" 2>&1)
echo "$OUT" | grep -q 'finished the requested work' || { echo "FAIL: mixed-reply prose missing"; exit 1; }
echo "$OUT" | grep -q 'tool_call' && { echo "FAIL: raw tool_call JSON leaked into the transcript"; exit 1; }
echo "ok: mixed prose+tool_call reply hides the protocol tag"
echo
echo "E2E PASS"
