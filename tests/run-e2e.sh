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

# Parallel fan-out: {"tasks": [...]} spawns one `anachron -p` PROCESS per task
# (no threads); every child's report comes back, scratch files are cleaned up.
PAR=$(mktemp -d)
OUT=$(ANACHRON_STUB_SCRIPT=tests/par-script.txt ./anachron --sandbox "$PAR" --yolo "fan out" 2>&1)
for i in 1 2 3; do
  echo "$OUT" | grep -q "=== Sub-agent $i of 3 ===" || { echo "FAIL: sub-agent $i missing"; exit 1; }
done
echo "$OUT" | grep -c 'child report done' | grep -q '^4$' || { echo "FAIL: expected 3 child reports + parent final"; exit 1; }
test "$(ls "$PAR" | grep -c anachron-par)" = "0" || { echo "FAIL: scratch files left behind"; exit 1; }
rm -rf "$PAR"
echo "ok: parallel sub-agent processes (3 spawned, all reported, scratch cleaned)"

# Session persistence: every turn auto-saves; --continue restores it in a new
# process, and the harness state stays hidden from the model's list_dir.
SESS=$(mktemp -d)
ANACHRON_STUB_SCRIPT=tests/demo-script.txt ./anachron --sandbox "$SESS" --yolo "create and verify hello.txt" >/dev/null 2>&1
test -s "$SESS/.anachron-sessions/last.json" || { echo "FAIL: no auto-saved session"; exit 1; }
OUT=$(ANACHRON_STUB_SCRIPT=tests/mixed-script.txt ./anachron -c --sandbox "$SESS" --yolo "follow-up" 2>&1)
echo "$OUT" | grep -q 'resumed previous conversation' || { echo "FAIL: --continue did not resume"; exit 1; }
python3 -c "
import json,sys
d=json.load(open('$SESS/.anachron-sessions/last.json'))
roles=[m['role'] for m in d]
assert roles.count('user') >= 2, roles          # both turns' tasks present
assert 'tool' in roles, roles                    # first turn's tool results survived
print('ok: --continue restores the auto-saved conversation across processes')"
rm -rf "$SESS"
echo
echo "E2E PASS"
