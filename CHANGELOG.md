# Changelog

All notable changes to ANACHRON are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project follows
[Semantic Versioning](https://semver.org/). The compiled version is in `core/version.h`
and is printed by `anachron --version`.

## [Unreleased]

### Fixed
- Interactive prompt no longer echoes mouse-wheel / arrow-key escape sequences as
  `^[[A`/`^[[B` garbage or folds them into the command. On a POSIX terminal the REPL now
  reads input in raw mode and consumes/ignores escape sequences (with Backspace, Enter,
  Ctrl+C-cancels-line, Ctrl+D-EOF); piped input and the Windows build keep the cooked
  path. Also disables alternate-scroll (`?1007`) on startup so the wheel scrolls
  scrollback during generation.

## [0.1.0] - 2026-06-22

First tagged release. A from-scratch, native C99 agentic coding harness with local,
in-process inference, built for late-Windows-XP-era 32-bit hardware (Pentium M, SSE2,
2 GB RAM) and validated on the dev host. On-real-hardware (Pentium-M / XP) validation
is the remaining arc before 1.0.

### Added
- **Agentic core loop**: a Claude-Code-style read/act loop driven by a small local
  model, with talk-vs-act discrimination so it converses when asked and emits tool calls
  when it acts. Lazy GBNF grammar + a lenient parser keep tool-call JSON well-formed
  without preventing plain-text replies.
- **Tools**: `read_file` (with paging), `write_file`, `edit` (fuzzy match), `list_dir`,
  `run_command`, `search` (grep) and `glob` (with a real `.gitignore` matcher), `final`.
- **Inference backends** behind a 4-function `infer_*` interface (link-time swap):
  a stub (tests), in-process `llama.cpp` (CPU, SSE2-only, with KV-cache reuse), and an
  HTTP client to a remote `llama-server` for offloading a big model to a desktop GPU.
- **Reliability**: verify-on-write guardrail (structural balance + `cc -fsyntax-only`,
  bad writes reverted), deterministic loop guards (repeat-call / narration nudges),
  context-bounding compaction so long sessions never overflow the window, and a sampler
  repeat penalty + runaway-repetition stop so a tiny model can't loop on one token.
- **Discovery & context**: `@file` mentions (inlined before inference), `AGENTS.md` /
  `CRUSH.md` auto-load into the system prompt, and capped tool output.
- **Sessions & UX**: slash commands (`/help`, `/new`, `/clear`, `/undo`, `/save`,
  `/sessions`, `/resume`, `/model`, `/quit`), session persistence as JSON, snapshot-based
  `/undo` (`.anbak`), in-place `/model` hot-swap, diff-on-edit (with optional colour),
  per-turn token usage, and a config file (`agent.json` / `.anachron.json`).
- **REPL hotkeys**: Ctrl+C interrupts the current generation and returns to the prompt
  (a second press still force-quits); terminal-input hygiene flushes stray scroll /
  keystroke bytes and disables leftover mouse-reporting so the wheel scrolls scrollback.
- **Cross-builds**: antiX i686 (`make antix`) and Windows XP PE32 (`make win` / the `xp`
  target), both reusing the SSE2-only Phase-0 `llama.cpp` libs.
- **Safety**: sandbox-confined file access (no `..`/drive-letter/`:` escape), an XP SP3
  Win32 API ceiling, SSE2-only, warning-clean under `-Wall -Wextra`.
- Unit tests (`make test`), scripted end-to-end (`make e2e`, `make verify-e2e`),
  `--version`, and project docs (README, HANDOFF, DEPLOY, Instructions, PHASE0-FINDINGS).

[Unreleased]: https://github.com/BerTobi/Anachron/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/BerTobi/Anachron/releases/tag/v0.1.0
