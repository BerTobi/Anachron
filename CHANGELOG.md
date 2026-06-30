# Changelog

All notable changes to ANACHRON are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project follows
[Semantic Versioning](https://semver.org/). The compiled version is in `core/version.h`
and is printed by `anachron --version`.

## [Unreleased]

## [0.4.5] - 2026-06-30

### Added
- **Persisted prompt cache** — the slow first-turn prefill (the static system+few-shot
  prefix) is saved to disk via `llama_state_save_file` and reloaded on the next cold start
  via `llama_state_load_file`, so it's paid once instead of every run. On the dev host a
  cold turn went 78s → 2s on the next run; in the XP build under Wine, 251s → 8s. Default
  path `<model>.<size>.anchkv` (keyed to the model); `ANACHRON_PROMPT_CACHE=<path>` to
  relocate or `=0` to disable. A changed prompt degrades safely (the `n_keep` prefix match
  re-prefills only the divergent tail). This is the fix for the ~100-min cold start on a
  Pentium-M: build the cache once, then every later session starts in seconds.
- **Lean prompt mode** (`ANACHRON_LEAN=1`) — a terse system prompt + one demonstration,
  ~430 vs ~1190 prompt tokens (~2.75x), so the *first* (uncached) turn prefills ~2.75x
  faster on slow hardware. Keeps the essentials (one-tool-call form, the tool list,
  save-by-default, talk-vs-act); validated that the 0.5B still writes files correctly.
  Trade-off: fewer rules/examples, so it may be slightly less reliable on edits/recovery.
- Progress-bar console detection on Windows now falls back to `GetFileType` when msvcrt's
  `_isatty` reports false on a real console, and `ANACHRON_PROGRESS=1/0` force-overrides
  the load/prefill bars either way.

### Changed
- The stub and real Windows builds no longer share a filename: `make win` →
  `anachron-stub.exe` (no-model Win32-layer test, prints a notice), `make xp` →
  `dist/xp/anachron-xp.exe` (real llama backend). Prevents running the stub by mistake.

## [0.4.4] - 2026-06-30

### Fixed
- Windows XP / antiX (32-bit) couldn't load a model at all: the weights were memory-mapped,
  and a contiguous mmap of a multi-hundred-MB file fails in a 32-bit process's ~2 GB address
  space (`MapViewOfFile failed: Not enough memory`). The 32-bit builds now read the model
  into a heap buffer instead (mmap stays on for 64-bit); override with `ANACHRON_MMAP=0/1`.
  This is why there was no working XP build — the exe ran but no model would load.

### Added
- Prebuilt, static `anachron.exe` is attached to the release: download it plus a model and
  run on the XP box — no cross-compiler needed.
- Vendored Windows-XP llama.cpp artifacts under `prebuilt/xp/` (4 static libs + headers,
  ~10 MB) so `make xp` builds from a clean clone — the `spike-phase0/llama.cpp` checkout is
  an embedded, untracked repo, so a clone otherwise had nothing to link. New `make xp-vendor`
  refreshes them from a rebuilt `build-xp`.

### Notes
- **The XP build now actually runs** — validated under Wine (starts, parses args, loads a
  model, prefills); first confirmation it executes (previously objdump-only). Real
  Pentium-M / XP hardware is still the final check.
- **32-bit model guidance** (see DEPLOY.md): use the **q8 0.5B** — it's the model confirmed
  to generate and ~640 MB fits a clean 2 GB XP process (it failed only under Wine's
  fragmented address space). 1.5B (~1.6 GB) will not fit a 32-bit process. A smaller-than-q8
  quant must be built from f16/safetensors — requantizing *down* from the q8 GGUF (q4_0,
  q5_K_M) produces a model that loads but generates nothing (the tiny model degrades to
  immediate end-of-text).

## [0.4.3] - 2026-06-23

### Added
- Per-token decode bar for very slow hardware (in-process llama backend, POSIX terminal):
  when a single token's forward pass takes seconds (e.g. the 1.5B on a Pentium-M at
  ~0.2 tok/s), a small bar fills over that one decode — `[#######.......] ~3s` — so the
  gap between tokens shows live progress instead of dead air. A single decode is bounded
  work, so the percentage is honest; it's filled by a time estimate (EMA of recent decode
  times) and clamped below 100% until the token actually lands. Driven by ggml's
  `abort_callback` (measured firing ~485×/decode — ample), drawn with ANSI cursor
  save/restore so it never disturbs the streamed text. Auto-gated: only engages above
  ~1.5 s/token, so faster setups (and the 0.5B) never see it strobe. Tunable via
  `ANACHRON_PTOK_MIN_SEC`. The XP console (no ANSI) keeps the load + prefill bars only.
- Ctrl+C is now felt mid-decode: the same `abort_callback` aborts an in-progress forward
  pass, so an interrupt during a multi-second token stops within milliseconds instead of
  waiting out the whole decode. The aborted token's KV state is reconciled (the cache
  mirror is dropped so the next turn re-prefills cleanly) and partial output is kept.

### Fixed
- A Ctrl+C-aborted decode no longer prints llama's `failed to compute graph / failed to
  decode` error spew: that output is expected (we asked it to abort) and is suppressed
  while an interrupt is pending, so an interrupt reads as a clean stop, not a crash.
- Build: the antiX (`antix`) and Windows XP (`xp`) cross targets now depend on the
  project headers, so a `core/version.h` bump (or any header edit) rebuilds them
  instead of leaving a stale cross binary. The native targets already had this; the
  cross targets were missed — which is why a freshly-bumped antiX bundle could still
  report the previous version.

## [0.4.2] - 2026-06-23

### Added
- Cold-start progress bars (in-process llama backend, interactive terminal only): the
  two slow, previously-silent phases of a cold turn now show a real ASCII bar with a
  rough ETA — `loading model [####....] 43% ~1s` during model load, and
  `reading prompt [####....] 1024/1190 tokens ~13s` during the first prompt prefill.
  Both have a known size, so the bar and ETA are honest (the ETA extrapolates from the
  measured rate). Generation has no known length, so it just streams as before — no
  bogus percentage. The model-load bar uses llama.cpp's `progress_callback`; the
  prefill bar ticks the existing 32-token decode loop.

### Changed
- The backend now owns llama.cpp's load `progress_callback`, which replaces its default
  loader dots. On a non-interactive stderr (pipes, redirected output, the test harness)
  the bars draw nothing AND the dots stay suppressed, so redirected output is cleaner
  than before. The prefill bar only appears when there's a substantial prefix to
  process, so cache-reusing follow-up turns don't flash it.

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

[Unreleased]: https://github.com/BerTobi/Anachron/compare/v0.4.5...HEAD
[0.4.5]: https://github.com/BerTobi/Anachron/compare/v0.4.4...v0.4.5
[0.4.4]: https://github.com/BerTobi/Anachron/compare/v0.4.3...v0.4.4
[0.4.3]: https://github.com/BerTobi/Anachron/compare/v0.4.2...v0.4.3
[0.4.2]: https://github.com/BerTobi/Anachron/compare/v0.4.1...v0.4.2
[0.4.1]: https://github.com/BerTobi/Anachron/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/BerTobi/Anachron/compare/v0.3.1...v0.4.0
[0.3.1]: https://github.com/BerTobi/Anachron/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/BerTobi/Anachron/compare/v0.2.1...v0.3.0
[0.2.1]: https://github.com/BerTobi/Anachron/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/BerTobi/Anachron/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/BerTobi/Anachron/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/BerTobi/Anachron/releases/tag/v0.1.0
