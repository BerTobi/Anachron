#!/usr/bin/env python3
"""A fake inference server for the network-backend e2e tests. Speaks three wire
shapes on one port:

  POST /completion            llama.cpp llama-server (prompt+grammar -> content)
  POST /v1/chat/completions   OpenAI-compatible chat
  POST /v1/messages           Anthropic Messages API

Responses are scripted: SCRIPT entries are returned in order per endpoint (each
endpoint has its own cursor). Requests are appended to <logdir>/requests.log as
one JSON line each (method, path, headers of interest, body) so the test can
assert on what the client actually sent.

Usage: fake_server.py PORT LOGDIR
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

SCRIPT = [
    "<tool_call>{\"name\": \"write_file\", \"arguments\": {\"path\": \"net.txt\", "
    "\"content\": \"hello from the network backend\\n\"}}</tool_call>",
    "<tool_call>{\"name\": \"final\", \"arguments\": {\"message\": "
    "\"Wrote net.txt over the wire.\"}}</tool_call>",
]

# A task containing "bigwrite" gets this script instead: one big write_file
# (~14 KB of content, enough to overflow a 4096-token history budget but not
# the networked default) then final. Exercises the history-budget-follows-the-
# backend fix.
BIG_SCRIPT = [
    "<tool_call>" + json.dumps({
        "name": "write_file",
        "arguments": {"path": "big.txt",
                      "content": "a reasonably long line of filler text\n" * 370},
    }) + "</tool_call>",
    "<tool_call>{\"name\": \"final\", \"arguments\": {\"message\": "
    "\"Wrote big.txt over the wire.\"}}</tool_call>",
]

# A task containing "lookatscreen" drives the vision flow: a screenshot call,
# then final. The test asserts the SECOND request carries the image as an
# inline base64 content part.
LOOK_SCRIPT = [
    "<tool_call>{\"name\": \"screenshot\", \"arguments\": {}}</tool_call>",
    "<tool_call>{\"name\": \"final\", \"arguments\": {\"message\": "
    "\"I looked at the screen.\"}}</tool_call>",
]

PORT = int(sys.argv[1])
LOGDIR = sys.argv[2]
cursors = {}
flaky_429s = {"left": 2}   # a "flaky" task 429s this many times, then recovers

# A task containing "fetchpage" drives the fetch flow: the model GETs a page
# from this same server, then reports. The page has markup to strip and a
# distinctive sentence the test asserts survived extraction.
FETCH_SCRIPT = [
    "<tool_call>{\"name\": \"fetch\", \"arguments\": {\"url\": "
    f"\"http://127.0.0.1:{PORT}/page.html\"}}}}</tool_call>",
    "<tool_call>{\"name\": \"final\", \"arguments\": {\"message\": "
    "\"Fetched the page.\"}}</tool_call>",
]

# "readpdf" tasks: read_file on a PDF -> the harness must ATTACH the document
# to the follow-up request; then final.
PDF_SCRIPT = [
    "<tool_call>{\"name\": \"read_file\", \"arguments\": {\"path\": \"doc.pdf\"}}</tool_call>",
    "<tool_call>{\"name\": \"final\", \"arguments\": {\"message\": "
    "\"Read the PDF.\"}}</tool_call>",
]

# "searchweb" tasks: a websearch tool call (the test points ANACHRON_SEARCH_URL
# at this server), then final.
SEARCH_SCRIPT = [
    "<tool_call>{\"name\": \"websearch\", \"arguments\": "
    "{\"query\": \"xyzzy magic word\"}}</tool_call>",
    "<tool_call>{\"name\": \"final\", \"arguments\": {\"message\": "
    "\"Searched the web.\"}}</tool_call>",
]

PAGE_HTML = """<!doctype html>
<html><head><title>Fetch Test</title>
<style>body { color: red }</style>
<script>var hidden = "should not appear";</script>
</head><body>
<h1>Welcome</h1>
<p>The secret word is <b>xyzzy</b> &amp; nothing else matters.</p>
</body></html>"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        with open(f"{LOGDIR}/requests.log", "a") as f:
            f.write(json.dumps({"path": self.path, "method": "GET", "body": ""}) + "\n")
        if self.path.startswith("/page.html"):
            out = PAGE_HTML.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(out)))
            self.end_headers()
            self.wfile.write(out)
            return
        # model catalogs (OpenAI shape; Anthropic's /v1/models matches too).
        # fake-embedding-1 must be filtered out by the picker.
        if self.path in ("/v1/models", "/models",
                         "/v1beta/openai/models"):
            out = json.dumps({"data": [
                {"id": "fake-big"},
                {"id": "fake-lite"},
                {"id": "fake-embedding-1"},
            ]}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(out)))
            self.end_headers()
            self.wfile.write(out)
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n).decode("utf-8", "replace")
        if "flaky" in body and flaky_429s["left"] > 0:
            flaky_429s["left"] -= 1
            out = json.dumps({"error": {"message": "You exceeded your current quota."}}).encode()
            self.send_response(429)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(out)))
            self.end_headers()
            self.wfile.write(out)
            return
        if "bigwrite" in body:
            script, tag = BIG_SCRIPT, "big"
        elif "lookatscreen" in body:
            script, tag = LOOK_SCRIPT, "look"
        elif "fetchpage" in body:
            script, tag = FETCH_SCRIPT, "fetch"
        elif "readpdf" in body:
            script, tag = PDF_SCRIPT, "pdf"
        elif "searchweb" in body:
            script, tag = SEARCH_SCRIPT, "search"
        elif "flaky" in body:
            script, tag = SCRIPT, "flaky"   # own cursor: recovery after the 429s
        else:
            script, tag = SCRIPT, ""
        key = (self.path, tag)
        if tag and "tool_response" not in body:
            cursors[key] = 0   # first request of a fresh session: restart the script
        idx = cursors.get(key, 0)
        cursors[key] = idx + 1
        text = script[idx] if idx < len(script) else script[-1]

        with open(f"{LOGDIR}/requests.log", "a") as f:
            f.write(json.dumps({
                "path": self.path,
                "auth": self.headers.get("Authorization", ""),
                "x_api_key": self.headers.get("x-api-key", ""),
                "anthropic_version": self.headers.get("anthropic-version", ""),
                "content_type": self.headers.get("Content-Type", ""),
                "body": body,
            }) + "\n")

        if self.path == "/completion":
            resp = {"content": text, "tokens_evaluated": 111, "tokens_predicted": 22}
        elif self.path in ("/v1/chat/completions", "/v1beta/openai/chat/completions"):
            resp = {"choices": [{"message": {"role": "assistant", "content": text}}],
                    "usage": {"prompt_tokens": 333, "completion_tokens": 44}}
        elif self.path == "/v1/messages":
            resp = {"content": [{"type": "text", "text": text}],
                    "stop_reason": "end_turn",
                    "usage": {"input_tokens": 555, "output_tokens": 66}}
        else:
            self.send_response(404)
            self.end_headers()
            return
        out = json.dumps(resp).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)


HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
