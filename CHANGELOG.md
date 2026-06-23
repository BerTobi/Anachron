# Changelog

All notable changes to ANACHRON are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project follows
[Semantic Versioning](https://semver.org/). The compiled version is in `core/version.h`
and is printed by `anachron --version`.

## [Unreleased]

## [0.4.1] - 2026-06-23

### Added
- No-progress guard: a `write_file`/`edit` whose content is byte-identical to what's
  already on disk is reported as `NO CHANGE` (with the change marked unsuccessful)
  instead of a successful "Wrote N bytes", so the loop can't mistake a re-saved stub
  for progress. This catches the weak-model trap of "told it's wrong → re-saves the
  same bytes → claims done". Cross-turn by construction (compares disk state), where
  the in-turn repeat guard resets each turn. Covered by `make noop-e2e`.
- Auto-repair of raw newlines inside C string/char literals: the most common
  weak-model defect (emitting a real line break where `\n` was meant, e.g.
  `printf("Too low!⏎")`) is now escaped to `\n` just before the syntax check, and the
  write is accepted with a note. Provably safe — valid code has no raw newlines inside
  a literal, so the repair only ever rescues a broken write. New `verify_repair_literals`
  reuses the `verify_balance` literal scanner; covered by unit tests + `make repair-e2e`.
- Recovery guard: when a write/edit didn't land (rejected by verify-on-write, or a
  no-op) and the model then tries to end the turn on plain text — typically a false
  "I fixed it, it works now" with no tool call — the loop re-prompts it to emit the
  corrected write rather than ending with nothing saved. Covered by `make recover-e2e`.

### Notes
- These three address a failure observed with both Hammer and Qwen-Coder: the model
  genuinely attempts real code (Qwen wrote a full number-guessing game) but lost the
  work to a `\n`-in-literal syntax error and then falsely claimed it had fixed it.
  Auto-repair fixes the defect; the recovery guard catches whatever still slips through.

## [0.4.0] - 2026-06-23

### Added
- Live-formatted streaming: a `write_file` tool call's code now streams as real,
  indented source (newlines and escapes decoded) instead of escaped-JSON on one line,
  so it looks the way it will land in the file. The `<tool_call>` / JSON wrapper is
  suppressed, and `final` messages / edit diffs aren't double-printed.
- Indentation + spacing for the model's output: replies and streamed code are indented
  under a leading blank line for a cleaner, more readable transcript.

## [0.3.1] - 2026-06-23

### Added
- Arrow-key line editing at the prompt (interactive POSIX terminal): Left/Right move the
  cursor, Home/End (and Ctrl+A/Ctrl+E) jump to the ends, Backspace/Delete remove a
  character, and printable keys insert at the cursor — so you can fix a typo mid-prompt
  instead of retyping. (Previously arrow keys were merely swallowed.)
- Up/Down command history at the prompt: recall and edit previously submitted lines
  (session-scoped, de-duplicated).

### Fixed
- Keys pressed while the model is generating no longer echo as `^[[…` escape garbage:
  terminal echo is suppressed during generation and any type-ahead is flushed before the
  next prompt. Ctrl+C still interrupts (signal handling stays on).
- `run_command` now warns when you run a stale build: if a command runs `./NAME` and its
  source (`NAME.c`/`NAME.cpp`) is newer than the binary — or the binary doesn't exist —
  the observation hints to recompile first (with the exact `cc NAME.c -o NAME` command).
  This catches the "edited the source, ran the old binary, got unchanged output" trap.
  New `plat_mtime` platform primitive backs the check.

## [0.3.0] - 2026-06-22

### Added
- Colour theme for the interactive REPL: the banner, `you>` prompt, tool-call lines,
  results/errors, notices, and the `final` header are now coloured (muted ANSI). Auto-on
  for an interactive terminal, off on Windows; `--color`/`--no-color` force it either way.
- `/stats` command: session token + throughput stats — turns, total/avg generated tokens,
  context tokens processed, wall time, tokens/sec, and a per-turn sparkline graph of
  generated tokens (Unicode blocks with colour, plain numbers without).
- Hammer 2.0 1.5B model (`--hammer-big`): the 2.0 family's reliable `<tool_call>` with
  better code than the 0.5B; slower (a cold turn is minutes on CPU).
- Hammer 2.1 1.5B model (`--hammer21-big`): works — it emits the call JSON wrapped in a
  ``` fence, which the lenient parser accepts — but it's the slowest option (~11 min cold
  turn on the dev host).

### Notes
- Hammer model fit for ANACHRON's tool format: **2.0 0.5B/1.5B** and **2.1 1.5B** all emit
  a usable tool call; **2.1 0.5B does not** (it prints raw code), so it was dropped.
  `--hammer` stays Hammer 2.0 0.5B (fast); use `--hammer-big` (2.0 1.5B) or `--hammer21-big`
  (2.1 1.5B) for a larger, slower model.

## [0.2.1] - 2026-06-22

### Fixed
- "code/build a program/game" now writes the file instead of trying to compile a file
  that doesn't exist yet. The small model was copying the few-shot's standalone
  `gcc -c add.c` compile example; that example was removed and replaced with a
  "code a program -> write_file" demonstration, and "code"/"build"/"game" were added to
  the save-by-default triggers.
- When a `run_command` fails with "No such file or directory", the observation now
  appends a hint telling the model to write_file the file first — recovers the common
  "tried to compile a file it never wrote" case (e.g. after copying a stale `gcc`
  command from the conversation) instead of looping on the same failed build.

## [0.2.0] - 2026-06-22

### Changed
- Creating code now saves to a file by default. "write/create/make a program, script,
  function, or file" writes it (to a sensibly-inferred filename) in one step and
  confirms, instead of printing the code and waiting to be told to save — which avoids
  the unreliable re-emit round-trip on small models. "show me", "explain", and questions
  still answer in plain text. Implemented via the system-prompt rule + few-shot.

### Fixed
- Banner no longer draws over previous terminal output: stopped emitting `ESC[?1049l`
  (leave-alternate-screen) on startup, which made some terminals restore a stale cursor.
- The cooked-mode prompt now also strips terminal escape sequences from the line, so
  mouse-wheel/arrow bytes can't corrupt the command even when raw-mode editing is
  unavailable (e.g. inside some multiplexers).

### Added
- `--tty-diag` flag: reports whether the terminal grants raw mode (for debugging the
  interactive line editor).

## [0.1.1] - 2026-06-22

### Fixed
- Interactive prompt no longer echoes mouse-wheel / arrow-key escape sequences as
  `^[[A`/`^[[B` garbage or folds them into the command. On a POSIX terminal the REPL now
  reads input in raw mode and consumes/ignores escape sequences (with Backspace, Enter,
  Ctrl+C-cancels-line, Ctrl+D-EOF); piped input and the Windows build keep the cooked
  path. Also disables alternate-scroll (`?1007`) on startup so the wheel scrolls
  scrollback during generation.
- Build: targets now depend on the project headers, so a `core/version.h` bump (or any
  header edit) triggers a rebuild instead of leaving a stale binary on incremental builds.

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

[Unreleased]: https://github.com/BerTobi/Anachron/compare/v0.4.1...HEAD
[0.4.1]: https://github.com/BerTobi/Anachron/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/BerTobi/Anachron/compare/v0.3.1...v0.4.0
[0.3.1]: https://github.com/BerTobi/Anachron/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/BerTobi/Anachron/compare/v0.2.1...v0.3.0
[0.2.1]: https://github.com/BerTobi/Anachron/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/BerTobi/Anachron/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/BerTobi/Anachron/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/BerTobi/Anachron/releases/tag/v0.1.0
