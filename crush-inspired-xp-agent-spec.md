# Minimal Agentic Coding CLI for Windows XP — Feature Spec

A Crush-inspired agent to hand to Claude Code, built from scratch.

**Target environment**
- Windows XP SP3, single-core Pentium M, ≤2 GB RAM
- .NET Framework 4.0 console app (last .NET that runs on XP SP3)
- Talks to an OpenAI-compatible endpoint — Ollama/LM Studio on a LAN desktop (RTX 2080 Ti), over **plain HTTP** (no TLS needed)
- Goal: capture the Crush features that actually matter, in priority order, so Claude Code can build it in stages

> If you go with C instead of C#, the feature list still stands — only the HTTP (WinHTTP/WinINet) and JSON (e.g. cJSON) implementation details change. The .NET notes at the bottom assume C#.

---

## P0 — Core (without these it isn't an agent)

1. **Provider connection.** Call an OpenAI-compatible `/v1/chat/completions`. Configurable `base_url`, `model`, `api_key`, `context_window`, `max_tokens`. Plain HTTP is fine.
2. **Conversation loop & history.** Accumulate the message list, resend the full context each turn, keep looping until the model returns no further tool calls.
3. **Tool-call protocol — with a fallback.** Support native OpenAI function-calling (`tools` / `tool_calls`) **and** a prompt-based text protocol fallback for models that don't do native tool calls reliably (many local coder models are flaky here). A config flag or auto-detect to switch between them. *This is the single highest-risk piece — see notes.*
4. **Built-in tools** (this is what makes it agentic):
   - `read_file`
   - `write_file` (create / overwrite)
   - `edit_file` (find-and-replace or unified-diff patch), showing the diff
   - `list_dir` / `glob`
   - `search` (grep across files)
   - `run_command` (run via `cmd.exe`, capture stdout/stderr/exit code, enforce a timeout)
5. **System prompt + project context file.** Auto-load an `AGENTS.md` / `CRUSH.md`-style file from the project root into the system prompt.

## P1 — Makes it genuinely usable day to day

6. **Permission gate.** Confirm (y/n) before `run_command` and before any file write/edit. Config allowlist to auto-approve known-safe commands. (Crush's tool-permission idea, simplified.)
7. **Session persistence.** Save each session as a JSON file on disk; list and resume them; multiple sessions per project. Skip SQLite — flat files are plenty.
8. **Config file, hierarchical.** Project `./agent.json` overrides global `%APPDATA%\agent\config.json`. Keep it crush.json-shaped so it feels familiar.
9. **Ignore rules.** Respect `.gitignore` plus a `.crushignore`-style file so reads and searches skip build junk and secrets.
10. **Streaming output.** Read the response stream incrementally (Ollama `stream: true`) so tokens appear as they arrive; spinner while waiting on first byte.
11. **Colored diffs & output.** Green/red diff lines, dimmed tool output — via the Console color API, **not** ANSI escapes (XP's console won't interpret them).
12. **Logging.** Write requests, raw tool-call payloads, and errors to `./logs`. Invaluable for debugging flaky local-model tool calls.

## P2 — Nice-to-have / cheap Crush parity

13. **Mid-session model switch.** A `/model` command to swap models without losing context.
14. **@-file mentions.** Type `@path/to/file` to inject a file into the prompt.
15. **Slash commands.** `/new`, `/sessions`, `/clear`, `/help`, and `/undo` (revert the last file change — keep a backup before each edit).
16. **Usage display.** Token counts per turn (free locally, but handy for tuning context).
17. **HTTP MCP client (stretch).** Support MCP servers over plain HTTP / JSON-RPC to add external tools. Skip the stdio and SSE transports.

## Won't-do (drop these, and why)

- **LSP integration** — language servers are RAM-hungry separate processes; you can't spare it on 2 GB. Lean on `search` + the model instead.
- **Rich TUI** (Bubble Tea-style panels, mouse, animations) — a clean line-based console is enough and far simpler on XP.
- **Vision / image input** — no vision model fits your GPU budget anyway.
- **Multi-provider auth** (Anthropic SSO, Copilot login, model auto-discovery) — one configured endpoint only.
- **TLS/HTTPS hardening** — you're hitting a LAN endpoint over plain HTTP, so no schannel/cipher headaches. Keep it HTTP.

---

## Suggested build order

`1 → 2 → 3 → 4` gives you a working agent. Then `6, 7` for safety and persistence, then `5, 8, 9`, then `10, 11, 12` for polish, then dip into P2 as you like.

## XP / .NET 4.0 implementation notes (give these to Claude Code up front)

- **`HttpClient` is .NET 4.5+.** On 4.0, use `HttpWebRequest` / `HttpWebResponse` for both normal and streaming reads.
- **No `System.Text.Json` in 4.0.** Bundle an older `Newtonsoft.Json` that ships a `net40` build (Json.NET did through the 12.x line), or fall back to `DataContractJsonSerializer`.
- **Console colors** via `Console.ForegroundColor` / `BackgroundColor`. Do **not** emit ANSI escape codes.
- **Shell execution** via `System.Diagnostics.Process` → `cmd.exe /c "..."`, with stdout/stderr redirected and a timeout.
- **Plain HTTP** to the LAN box means no certificate or cipher problems — leave it HTTP.
- **Test tool-calling against your real model early.** Reliability of tool calls is the #1 risk to the whole project. If native function-calling is shaky, the P0 text-protocol fallback (item 3) is what rescues it.
