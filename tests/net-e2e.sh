#!/bin/sh
# Network-backend end-to-end: the SAME scripted exchange (write net.txt -> final)
# through all three network backends against tests/fake_server.py:
#   1. --model http://127.0.0.1:P            (llama-server /completion + grammar)
#   2. --model openai:test    + ANACHRON_API_URL   (/v1/chat/completions)
#   3. --model anthropic:test + ANACHRON_API_URL   (/v1/messages, x-api-key headers)
# Asserts the real filesystem effect, the usage counts reaching the band, and the
# wire details (auth headers, grammar only on /completion, messages structure).
set -e
export ANACHRON_NO_CONFIG=1   # hermetic: never read a developer agent.json
cd "$(dirname "$0")/.."

TMP=$(mktemp -d)
PORT=$((20000 + $$ % 20000))
python3 tests/fake_server.py "$PORT" "$TMP" &
SRV=$!
trap 'kill $SRV 2>/dev/null; rm -rf "$TMP"' EXIT
sleep 0.5

# 1) remote llama-server
mkdir -p "$TMP/sb"
OUT=$(./anachron --model "http://127.0.0.1:$PORT" --sandbox "$TMP/sb" --yolo "write net.txt" 2>&1)
echo "$OUT" | grep -q 'Wrote net.txt over the wire' || { echo "FAIL(remote): $OUT"; exit 1; }
grep -q 'hello from the network backend' "$TMP/sb/net.txt" || { echo "FAIL(remote): no file"; exit 1; }
# two generates of 22 tokens each are summed over the turn
echo "$OUT" | grep -q '44 tok' || { echo "FAIL(remote): server usage counts not shown"; exit 1; }
echo "ok: remote llama-server backend (usage flows to the band)"

# 2) openai-compatible
rm -f "$TMP/sb/net.txt"
OUT=$(ANACHRON_API_URL="http://127.0.0.1:$PORT" \
      ./anachron --model "openai:test-model" --sandbox "$TMP/sb" --yolo "write net.txt" 2>&1)
echo "$OUT" | grep -q 'Wrote net.txt over the wire' || { echo "FAIL(openai): $OUT"; exit 1; }
grep -q 'hello from the network backend' "$TMP/sb/net.txt" || { echo "FAIL(openai): no file"; exit 1; }
# the band tracks the history budget on API backends too: 333 prompt tokens
# of the 32768 budget -> "ctx 1%"
echo "$OUT" | grep -q 'ctx 1%' || { echo "FAIL(openai): band lost the ctx tracker"; echo "$OUT" | tail -3; exit 1; }
echo "ok: openai-compatible backend (ctx tracker on the band)"

# 3) anthropic
rm -f "$TMP/sb/net.txt"
OUT=$(ANACHRON_API_URL="http://127.0.0.1:$PORT" ANACHRON_API_KEY="sk-test-123" \
      ./anachron --model "anthropic:test-model" --sandbox "$TMP/sb" --yolo "write net.txt" 2>&1)
echo "$OUT" | grep -q 'Wrote net.txt over the wire' || { echo "FAIL(anthropic): $OUT"; exit 1; }
grep -q 'hello from the network backend' "$TMP/sb/net.txt" || { echo "FAIL(anthropic): no file"; exit 1; }
echo "ok: anthropic backend"

# 4) gemini alias: a base URL that carries a path must get only the endpoint
#    leaf appended (Google's /v1beta/openai compat layer shape)
rm -f "$TMP/sb/net.txt"
OUT=$(ANACHRON_API_URL="http://127.0.0.1:$PORT/v1beta/openai" ANACHRON_API_KEY="g-test" \
      ./anachron --model "gemini:gemini-2.5-pro" --sandbox "$TMP/sb" --yolo "write net.txt" 2>&1)
echo "$OUT" | grep -q 'Wrote net.txt over the wire' || { echo "FAIL(gemini): $OUT"; exit 1; }
grep -q 'hello from the network backend' "$TMP/sb/net.txt" || { echo "FAIL(gemini): no file"; exit 1; }
echo "ok: gemini alias (path-carrying base URL)"

# 5) /model lists the provider's catalog (filtered to chat models)
OUT=$(printf '/model\n\n/quit\n' | ANACHRON_API_URL="http://127.0.0.1:$PORT" ANACHRON_API_KEY="g-test" \
      ./anachron --model "gemini:gemini-x" --sandbox "$TMP/sb" 2>&1)
echo "$OUT" | grep -q 'gemini:fake-big'  || { echo "FAIL(/model): catalog not listed"; echo "$OUT" | tail -6; exit 1; }
echo "$OUT" | grep -q 'gemini:fake-lite' || { echo "FAIL(/model): catalog incomplete"; exit 1; }
echo "$OUT" | grep -q 'fake-embedding' && { echo "FAIL(/model): embedding model not filtered"; exit 1; }
echo "ok: /model lists the API catalog (embeddings filtered)"

# 5b) ...even when the CURRENT backend is local: with a base URL configured the
#     picker falls back to the openai-compat catalog
OUT=$(printf '/model\n\n/quit\n' | ANACHRON_API_URL="http://127.0.0.1:$PORT" \
      ./anachron --sandbox "$TMP/sb" 2>&1)
echo "$OUT" | grep -q 'openai:fake-big' || { echo "FAIL(/model fallback): catalog not listed from local backend"; exit 1; }
echo "ok: /model catalog fallback while a local backend runs"

# 6) the history budget follows the backend on a /model switch: starting on the
#    LOCAL backend (4096-token budget) and switching to an API model must not
#    compact history that the hosted model has plenty of room for
OUT=$(printf '/model\nopenai:test-model\nbigwrite\n/quit\n' | ANACHRON_API_URL="http://127.0.0.1:$PORT" \
      ./anachron --sandbox "$TMP/sb" 2>&1)
echo "$OUT" | grep -q 'Wrote big.txt over the wire' || { echo "FAIL(budget): big turn broke"; echo "$OUT" | tail -4; exit 1; }
echo "$OUT" | grep -q 'context is filling up' && { echo "FAIL(budget): compacted under a 32k budget"; exit 1; }
echo "ok: /model switch to an API raises the history budget"

# 6b) ...but an explicit --ctx is respected across the switch (control: the same
#     big turn under a forced 4096 budget must compact)
OUT=$(printf '/model\nopenai:test-model\nbigwrite\n/quit\n' | ANACHRON_API_URL="http://127.0.0.1:$PORT" \
      ./anachron --ctx 4096 --sandbox "$TMP/sb" 2>&1)
echo "$OUT" | grep -q 'context is filling up' || { echo "FAIL(budget): explicit --ctx 4096 not respected"; exit 1; }
echo "ok: explicit --ctx survives the switch (compaction control)"

# Wire assertions from the server's request log
python3 - "$TMP/requests.log" <<'EOF'
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
comp = [r for r in reqs if r["path"] == "/completion"]
oai  = [r for r in reqs if r["path"] == "/v1/chat/completions"]
anth = [r for r in reqs if r["path"] == "/v1/messages"]
assert comp and oai and anth, "an endpoint was never called"
# llama-server got the prompt + grammar; APIs never get a grammar
assert '"grammar"' in comp[0]["body"], "remote: grammar missing"
assert all('"grammar"' not in r["body"] for r in oai + anth), "API got a grammar"
# anthropic wire contract
a = anth[0]
assert a["x_api_key"] == "sk-test-123", "anthropic: x-api-key missing"
assert a["anthropic_version"] == "2023-06-01", "anthropic: version header missing"
b = json.loads(a["body"])
assert "max_tokens" in b and "system" in b and b["messages"][0]["role"] == "user"
assert "temperature" not in b, "anthropic: temperature must not be sent"
# openai wire contract
b = json.loads(oai[0]["body"])
assert b["messages"][0]["role"] == "system", "openai: system message missing"
assert b.get("temperature") == 0, "openai: temperature 0 expected"
# the tool result came back as a wrapped user turn on the second call
b2 = json.loads(anth[1]["body"])
joined = json.dumps(b2["messages"])
assert "tool_response" in joined, "anthropic: tool result not in follow-up"
print("ok: wire contracts (headers, grammar routing, message structure)")
EOF

echo "NET-E2E PASS"
