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

# The agent tool: a sub-agent runs the task in a fresh context; the parent gets
# only its final report, and the child's file effects are real.
OUT=$(ANACHRON_STUB_SCRIPT=tests/agent-script.txt ./anachron --sandbox "$TMP" --yolo "delegate" 2>&1)
echo "$OUT" | grep -q 'Sub-agent report:' || { echo "FAIL: no sub-agent report"; exit 1; }
echo "$OUT" | grep -q 'Delegated and done' || { echo "FAIL: parent did not conclude"; exit 1; }
grep -q 'the subagent was here' "$TMP/notes.txt" || { echo "FAIL: child write missing"; exit 1; }
echo "ok: agent tool (isolated child context, real effects, report fed back)"

# ...and a child that tries to spawn its own sub-agent is refused (depth cap).
OUT=$(ANACHRON_STUB_SCRIPT=tests/agent-deep-script.txt ./anachron --sandbox "$TMP" --yolo "delegate deep" 2>&1)
echo "$OUT" | grep -q 'sub-agents cannot spawn sub-agents' || { echo "FAIL: depth cap not enforced"; exit 1; }
echo "$OUT" | grep -q 'Depth respected' || { echo "FAIL: parent did not recover from depth refusal"; exit 1; }
echo "ok: sub-agent depth capped at one level"
echo
echo "E2E PASS"
