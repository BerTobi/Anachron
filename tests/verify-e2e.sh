#!/bin/sh
# Verify-on-write guardrail, end to end, via the deterministic stub backend:
# a structurally broken file must be REJECTED and reverted (never created); a
# valid file must be kept. Uses only the balance check, so it passes with or
# without a C compiler present.
set -e

cd "$(dirname "$0")/.."
make anachron >/dev/null

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "sandbox: $TMP"
echo

ANACHRON_STUB_SCRIPT=tests/verify-script.txt ./anachron --sandbox "$TMP" "verify guardrail test" 2>&1 \
    | grep -E 'write_file|result|REJECTED|Wrote|final|==' || true

echo
echo "--- assertions ---"
if [ -f "$TMP/broken.c" ]; then echo "FAIL: broken.c should have been reverted"; exit 1; fi
echo "ok: broken.c was rejected and reverted (not created)"
if [ ! -f "$TMP/good.c" ]; then echo "FAIL: good.c should have been written"; exit 1; fi
echo "ok: good.c was written"
echo
echo "VERIFY-E2E PASS"
