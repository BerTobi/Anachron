#!/bin/sh
# Recovery guard, end to end, via the stub backend. Sequence: a write is REJECTED
# (missing brace - a structural error auto-repair can't mask); the model then emits
# a plain-text FALSE claim ("I fixed it") with no tool call. The harness must NOT let
# that end the turn - it must nudge, after which the model emits the corrected write.
# Proof the guard worked: the corrected file ends up on disk (it would not exist if
# the false claim had ended the turn, since the rejected write was reverted).
set -e

cd "$(dirname "$0")/.."
make anachron >/dev/null

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "sandbox: $TMP"
echo

OUT=$(ANACHRON_STUB_SCRIPT=tests/recover-script.txt ./anachron --sandbox "$TMP" "recovery test" 2>&1 || true)
echo "$OUT" | grep -E 'write_file|result|REJECTED|Wrote|nudg|final|==' || true

echo
echo "--- assertions ---"
echo "$OUT" | grep -q 'REJECTED'        || { echo "FAIL: the first (broken) write should be rejected"; exit 1; }
echo "ok: broken write was rejected"
echo "$OUT" | grep -qi 'nudging to write' || { echo "FAIL: the false 'I fixed it' should trigger a recovery nudge"; exit 1; }
echo "ok: false completion claim was caught and nudged"
if [ ! -f "$TMP/bad.c" ]; then echo "FAIL: corrected bad.c should exist (turn did not end on the false claim)"; exit 1; fi
grep -q '}' "$TMP/bad.c"               || { echo "FAIL: bad.c should hold the corrected, balanced content"; exit 1; }
echo "ok: corrected file landed on disk after recovery"
echo
echo "RECOVER-E2E PASS"
