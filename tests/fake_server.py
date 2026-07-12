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

PORT = int(sys.argv[1])
LOGDIR = sys.argv[2]
cursors = {}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n).decode("utf-8", "replace")
        idx = cursors.get(self.path, 0)
        cursors[self.path] = idx + 1
        text = SCRIPT[idx] if idx < len(SCRIPT) else SCRIPT[-1]

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
        elif self.path == "/v1/chat/completions":
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
