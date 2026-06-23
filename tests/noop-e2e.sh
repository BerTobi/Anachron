#!/bin/sh
# No-progress guard, end to end, via the deterministic stub backend: writing a
# file once succeeds; writing byte-identical content again must be flagged as a
# no-op ("NO CHANGE") rather than reported as a successful write, so the loop
# can't mistake a re-saved stub for progress. The file must survive intact.
set -e

cd "$(dirname "$0")/.."
make anachron >/dev/null

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "sandbox: $TMP"
echo

OUT=$(ANACHRON_STUB_SCRIPT=tests/noop-script.txt ./anachron --sandbox "$TMP" "no-op guard test" 2>&1 || true)
echo "$OUT" | grep -E 'write_file|result|NO CHANGE|Wrote|final|==' || true

echo
echo "--- assertions ---"
# First write reports a real write; second reports NO CHANGE.
echo "$OUT" | grep -q 'Wrote .* bytes to prog.c'    || { echo "FAIL: first write should report bytes written"; exit 1; }
echo "ok: first write reported as a real write"
echo "$OUT" | grep -q 'NO CHANGE: prog.c'           || { echo "FAIL: identical re-write should be flagged NO CHANGE"; exit 1; }
echo "ok: identical re-write flagged as NO CHANGE"
# The file must still exist with the original content.
if [ ! -f "$TMP/prog.c" ]; then echo "FAIL: prog.c should still exist"; exit 1; fi
grep -q 'return 0;' "$TMP/prog.c"                   || { echo "FAIL: prog.c content was clobbered"; exit 1; }
echo "ok: prog.c intact after the no-op"
echo
echo "NOOP-E2E PASS"
