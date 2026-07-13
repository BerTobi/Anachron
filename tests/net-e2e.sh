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
# of the 131072 budget -> "ctx 0%"
echo "$OUT" | grep -q 'ctx 0%' || { echo "FAIL(openai): band lost the ctx tracker"; echo "$OUT" | tail -3; exit 1; }
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
echo "$OUT" | grep -q 'context is filling up' && { echo "FAIL(budget): compacted under a 128k budget"; exit 1; }
echo "ok: /model switch to an API raises the history budget"

# 6b) ...but an explicit --ctx is respected across the switch (control: the same
#     big turn under a forced 4096 budget must compact)
OUT=$(printf '/model\nopenai:test-model\nbigwrite\n/quit\n' | ANACHRON_API_URL="http://127.0.0.1:$PORT" \
      ./anachron --ctx 4096 --sandbox "$TMP/sb" 2>&1)
echo "$OUT" | grep -q 'context is filling up' || { echo "FAIL(budget): explicit --ctx 4096 not respected"; exit 1; }
echo "ok: explicit --ctx survives the switch (compaction control)"

# 7) vision: a screenshot tool call attaches the PNG to the follow-up request
#    as an inline base64 image part, on both wire shapes. The fake-screenshot
#    hook substitutes a canned PNG (nothing is captured on CI).
python3 - "$TMP/shot.png" <<'EOF'
import struct, sys, zlib
w, h = 4, 3
raw = b''.join(b'\x00' + bytes([255,0,0]*w) for _ in range(h))
def chunk(t, d): return struct.pack('>I',len(d)) + t + d + struct.pack('>I', zlib.crc32(t+d)&0xffffffff)
png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB',w,h,8,2,0,0,0))
       + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
open(sys.argv[1],'wb').write(png)
EOF
OUT=$(ANACHRON_API_URL="http://127.0.0.1:$PORT" ANACHRON_FAKE_SCREENSHOT="$TMP/shot.png" \
      ./anachron --model "openai:test-model" --sandbox "$TMP/sb" --yolo "lookatscreen" 2>&1)
echo "$OUT" | grep -q 'I looked at the screen' || { echo "FAIL(vision): $OUT"; exit 1; }
test -s "$TMP/sb/screenshot.png" || { echo "FAIL(vision): screenshot not saved in sandbox"; exit 1; }
OUT=$(ANACHRON_API_URL="http://127.0.0.1:$PORT" ANACHRON_API_KEY="sk-test-123" ANACHRON_FAKE_SCREENSHOT="$TMP/shot.png" \
      ./anachron --model "anthropic:test-model" --sandbox "$TMP/sb" --yolo "lookatscreen" 2>&1)
echo "$OUT" | grep -q 'I looked at the screen' || { echo "FAIL(vision/anthropic): $OUT"; exit 1; }
echo "ok: screenshot flow (both wire shapes; image lands in the sandbox)"

# 8) fetch: the model GETs a page; the follow-up request must carry the page's
#    TEXT (entities decoded) with the markup and script stripped. Also exercises
#    -p print mode: only the final answer lands on stdout.
OUT=$(ANACHRON_API_URL="http://127.0.0.1:$PORT" \
      ./anachron -p --model "openai:test-model" --sandbox "$TMP/sb" --yolo "fetchpage" 2>/dev/null)
test "$OUT" = "Fetched the page." || { echo "FAIL(fetch/-p): got '$OUT'"; exit 1; }
grep -q 'secret word is xyzzy & nothing else' "$TMP/requests.log" || { echo "FAIL(fetch): text not extracted"; exit 1; }
grep -q 'should not appear' "$TMP/requests.log" && { echo "FAIL(fetch): script text leaked"; exit 1; }
grep -q '<h1>' "$TMP/requests.log" && { echo "FAIL(fetch): markup leaked"; exit 1; }
echo "ok: fetch strips HTML; -p prints only the answer"

# 9) transient failures are retried: the server 429s twice, then recovers; the
#    turn must succeed anyway, with retry notes on stderr.
rm -f "$TMP/sb/net.txt"
OUT=$(ANACHRON_API_URL="http://127.0.0.1:$PORT" ANACHRON_API_RETRY_MS=50 \
      ./anachron --model "openai:test-model" --sandbox "$TMP/sb" --yolo "flaky task: write net.txt" 2>&1)
echo "$OUT" | grep -q 'Wrote net.txt over the wire' || { echo "FAIL(retry): turn died on 429s"; echo "$OUT" | tail -4; exit 1; }
test "$(echo "$OUT" | grep -c 'HTTP 429 - retrying')" = "2" || { echo "FAIL(retry): expected 2 retry notes"; exit 1; }
grep -q 'hello from the network backend' "$TMP/sb/net.txt" || { echo "FAIL(retry): no file"; exit 1; }
echo "ok: 429s retried with backoff (turn survived, 2 retry notes)"

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

# vision wire contract: the request after the screenshot carries the image
import base64
def png_from(msgs, get):
    for m in msgs:
        if isinstance(m.get("content"), list):
            for part in m["content"]:
                d = get(part)
                if d: return base64.b64decode(d)
    return None
vis_oai = [r for r in oai if "lookatscreen" in r["body"] and "image_url" in r["body"]]
assert vis_oai, "openai: no request carried an image part"
png = png_from(json.loads(vis_oai[-1]["body"])["messages"],
               lambda p: p.get("image_url", {}).get("url", "").split("base64,")[-1]
                         if p.get("type") == "image_url" else None)
assert png and png[:8] == b'\x89PNG\r\n\x1a\n', "openai: image part is not the PNG"
vis_anth = [r for r in anth if "lookatscreen" in r["body"] and '"type":"image"' in r["body"]]
assert vis_anth, "anthropic: no request carried an image block"
b3 = json.loads(vis_anth[-1]["body"])
png = png_from(b3["messages"],
               lambda p: p.get("source", {}).get("data") if p.get("type") == "image" else None)
assert png and png[:8] == b'\x89PNG\r\n\x1a\n', "anthropic: image block is not the PNG"
src = [p for m in b3["messages"] if isinstance(m.get("content"), list)
       for p in m["content"] if p.get("type") == "image"][0]["source"]
assert src["type"] == "base64" and src["media_type"] == "image/png"
print("ok: wire contracts (headers, grammar routing, messages, image parts)")
EOF

echo "NET-E2E PASS"
