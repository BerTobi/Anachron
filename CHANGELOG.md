# Changelog

All notable changes to ANACHRON are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project follows
[Semantic Versioning](https://semver.org/). The compiled version is in `core/version.h`
and is printed by `anachron --version`.

## [Unreleased]

## [0.11.0] - 2026-07-12

### Added
- **`agent` tool: sub-agents with fresh context.** The model can delegate a
  self-contained task to a sub-agent that has the same tools and sandbox but a
  separate, empty conversation — the parent receives ONLY the final report.
  This is the context-isolation trick from clide (the Claude Code ancestor
  that fanned out models to answer questions about whole folders): "summarize
  every file in src/" no longer floods the parent's history with file
  contents. The child's tool calls still render in the transcript and still
  hit the permission gate; streaming and chatter are suppressed; nesting is
  capped at one level (a sub-agent that tries to spawn another is refused);
  the child's generated tokens count toward the turn's band total. Offered on
  hosted-API backends.

## [0.10.0] - 2026-07-12

### Added
- **`-p` / `--print`: Unix filter mode.** The task comes from the arguments,
  piped stdin, or both (`git diff | anachron -p "review this"`); only the final
  answer is printed to stdout, so the output pipes cleanly into the next
  command. Diagnostics stay on stderr. This is the "Unix-style composable
  interface" from the original Claude CLI feature list.
- **`fetch` tool: the model can read the web.** `fetch {"url": ...}` GETs a
  page or API over the platform HTTP stack (WinINet on Windows/XP, curl for
  https on POSIX), strips HTML to readable text (scripts/styles/comments
  dropped, entities decoded), and caps the result at 24 KB. Gated
  (`fetch this URL? [y/N]`) since a fetch can carry data off the machine.
  Offered on hosted-API backends beside `screenshot`; XP TLS caveats apply as
  usual (plain-http and LAN URLs always work).

## [0.9.0] - 2026-07-12

### Added
- **The model can see the screen.** A new `screenshot` tool — inspired by the
  original Claude CLI, where "see the screen" sat beside read/write/bash from
  day one — captures the whole screen to a PNG in the working directory and
  attaches the image to the tool result. On vision-capable backends
  (`anthropic:`/`openai:`/`gemini:`) the model actually looks at it: "what's on
  my screen?", "check the GUI you just launched", "read the error in the other
  window". Details:
  - **Windows/XP**: classic GDI `BitBlt` into a DIB (every call XP-SP3-safe;
    new imports GDI32/USER32 ship on every XP), downscaled by halves to
    ≤1400px wide, written by an in-house dependency-free PNG encoder (stored
    deflate — format-valid everywhere, zero libraries).
  - **POSIX**: delegates to the first available capture tool
    (`import`/`scrot`/`gnome-screenshot`/`spectacle`/`grim`).
  - The tool is **gated** (`capture the screen? [y/N]`) like writes and
    commands, and only *offered* in the system prompt when the backend can see
    images — a text-only local model is never told about it.
  - Only the newest screenshot stays inlined in the conversation (older ones
    would re-upload megabytes of base64 on every call); compaction drops
    attachments with their messages.
  - `ANACHRON_FAKE_SCREENSHOT=<file>` substitutes a canned image — the vision
    path is fully testable headless (net-e2e round-trips the PNG bytes through
    both wire shapes), and it's how the feature was validated live: Gemini
    described a synthetic four-quadrant test image pixel-perfectly.

## [0.8.5] - 2026-07-12

### Changed
- **The networked history-budget default is now 128k tokens (was 32k).** Hosted
  models have room to spare (Gemini: 1M), and 32k compacted long sessions far
  earlier than necessary. 128k still bounds what a turn re-uploads per tool
  step; `--ctx` (or `"ctx"` in the config) overrides in either direction and
  applies across `/model` switches. Local models keep the 4096 default.

## [0.8.4] - 2026-07-12

### Fixed
- **The `ctx N%` tracker is back on the status band for API/remote sessions.**
  v0.7.0 hid it on networked backends because the server's window is unknown —
  which removed the tracker exactly where turns cost money. It now tracks the
  thing that actually matters operationally: how full the harness's HISTORY
  BUDGET is (the point where compaction starts — 32k tokens on hosted/remote
  backends, the real window locally), using the exact prompt token counts the
  APIs report. Same amber-at-80% / hint-at-90% behavior everywhere. On a LAN
  llama-server with prompt caching the reported count covers only the uncached
  tail, so the % can read low between turns there.

## [0.8.3] - 2026-07-12

### Fixed
- **`/model` switching to an API model no longer compacts history it doesn't
  need to.** The relaxed 32k-token history budget that hosted/remote backends
  get at launch now also applies when you *switch* to one mid-session (and the
  real local window is restored when you switch back). Previously a session
  started on a local model kept the 4096-token budget after switching, so one
  decent-sized file write triggered spurious "context is filling up" compaction
  on backends with room to spare. An explicit `--ctx` is still respected.
- **Flattened tool calls are accepted.** Some API models (Gemini flash,
  persistently) emit `{"name": "write_file", "path": ..., "content": ...}`
  with the arguments hoisted beside `"name"` instead of nested under
  `"arguments"`. The intent is unambiguous, so the parser now takes the
  top-level keys as the arguments (and accepts the common `"parameters"`
  alias) instead of erroring three times and derailing the turn.
- **The `<tool_call>` JSON no longer leaks into plain-text replies.** When a
  model closes a prose reply with a tool call (frontier-model style: summary
  paragraph, then `final`), the streamed transcript now shows the prose and
  suppresses the protocol tag onward; text that genuinely ends mid-"<tool" is
  still shown.

## [0.8.2] - 2026-07-12

### Fixed
- **`/model` lists the API catalog even while a local model is running.** The
  provider to ask is now inferred in order: the current backend if it's an API
  spec → the config file's model (your key is saved there) → a configured
  `ANACHRON_API_URL` (assumed OpenAI-compatible, keyless LAN servers included).
  Previously the catalog only appeared when the session was already on an API
  backend — exactly the moment you least needed the list.
- The picker also scans the folder the RUNNING `.gguf` came from ("Models
  beside the current one"), so launching via a script that keeps models in its
  own stash still lists the local alternatives.

## [0.8.1] - 2026-07-12

### Added
- **`/model` lists your API provider's catalog.** When the current backend is an
  API spec (`gemini:`/`anthropic:`/`openai:`), the picker queries the provider's
  models endpoint with your key and lists the results — numbered and pickable —
  alongside local `.gguf` files. Non-chat modalities (embeddings, tts, image,
  audio, music, robotics, …) are filtered out. Honors `ANACHRON_API_URL`, so a
  LAN llama-server / LM Studio catalog lists too.
- `plat_http_get` now works on POSIX (raw socket for http, system curl for
  https) and takes headers + returns the HTTP status on both platforms.

### Changed
- `/update`'s GitHub check is now explicitly Windows-only (the release assets
  are Windows binaries; POSIX builds update from source) — previously it was
  only accidental that POSIX skipped it.

## [0.8.0] - 2026-07-12

Where the agent works, made deliberate: per-conversation scratch folders, a
project-folder mode like other coding agents, and config that follows you.

### Added
- **`"sandbox": "auto"`** — every conversation gets its own fresh
  `anachron-sessions/<timestamp>/` folder, and `/new` rotates to a clean one
  (dropping the old folder's `AGENTS.md` project notes with it). No more one
  session's files haunting the next.
- **`--here`** — the sandbox is the folder you're standing in, overriding any
  configured sandbox: `cd my-project && anachron --here` works like other
  coding agents. (`--sandbox .` was always the built-in default; `--here` makes
  it explicit and config-proof.)
- **Global config fallback** — when the current directory has no
  `agent.json`/`.anachron.json`, `~/.anachron.json` (or `%USERPROFILE%`) is
  read, so your model and API key follow you into any project folder. A
  project-local config still wins.
- **`make install`** — installs the llama build as `anachron` on your PATH
  (`PREFIX=$HOME/.local` by default).
- **`ANACHRON_NO_CONFIG=1`** — hermetic mode: ignore every config file. All e2e
  suites now set it, after the suites were caught silently running against a
  developer's configured live API (and passing!).

### Changed
- `run.sh` defaults to `--sandbox auto` (override with `ANACHRON_SANDBOX`).
- First-run setup explains the `auto` / `.` / fixed-folder sandbox choices.

## [0.7.1] - 2026-07-03

### Added
- **`gemini:<model>` backend** — the Google Gemini API through its OpenAI-compatible
  layer (`.../v1beta/openai/chat/completions`). A **free** key from
  aistudio.google.com/apikey works; use the `-latest` aliases
  (`gemini:gemini-flash-latest`) — the pro models carry little or no free-tier
  quota, and the older dated models are closed to new projects. Validated live:
  a full agent turn (write fizzbuzz.c → gcc compile → run → verify → final) on
  `gemini-flash-latest` through the unchanged `<tool_call>` protocol and gate.

### Fixed
- API endpoint resolution: a base URL that already carries a path (Google's
  compat layer, reverse proxies) now gets only the endpoint leaf appended
  (`/chat/completions`), a bare host gets the canonical `/v1/...` path, and a
  base that already IS the full endpoint is used as-is.
- API error bodies wrapped in a JSON array (Google's style) now surface their
  message instead of raw JSON.

## [0.7.0] - 2026-07-03

The local 0.5B stops being the ceiling: one binary now carries three backend
families, selected at runtime by the shape of the model spec — switchable
mid-session with `/model`.

### Added
- **Remote inference** — `--model http://gpu-box:8080` points ANACHRON at a
  llama.cpp `llama-server` on the LAN (`/completion`; a full path in the URL
  overrides the endpoint). The big model runs on the big machine; this side is a
  thin client, so the Pentium-M is no longer the ceiling. Plain HTTP — **works on
  real XP** with no TLS involved (WinINet on Windows, raw sockets on POSIX, with
  Ctrl+C honoured mid-request). GBNF grammar still applies (llama-server accepts
  it), `cache_prompt` keeps the server's KV warm across turns, and the server's
  token counts flow into the status band. `--api-key` servers: `ANACHRON_REMOTE_KEY`
  or `"remote_key"` in agent.json.
- **Hosted chat APIs** — `--model anthropic:claude-opus-4-8` (Anthropic Messages
  API: `x-api-key` + `anthropic-version: 2023-06-01`, required `max_tokens`, no
  sampling params — current models reject them) and `--model openai:MODEL` (any
  OpenAI-compatible `/v1/chat/completions`: llama-server, LM Studio, Ollama, or
  api.openai.com). Key via `ANACHRON_API_KEY` / `"api_key"`; base URL via
  `ANACHRON_API_URL` / `"api_url"` — point it at a LAN server and no key is needed.
  The APIs receive the structured conversation (tool results as `<tool_response>`
  user turns) plus ANACHRON's system prompt, so the existing `<tool_call>` text
  protocol, agent loop, permission gate, and transcript work unchanged. Anthropic
  refusals (`stop_reason: "refusal"`) surface as a readable notice. HTTPS transport:
  WinINet on Windows (TLS per OS — the LAN option is the XP-reliable path), the
  system `curl` on POSIX (secrets via a private temp file, never argv).
- **Runtime backend dispatch** — the infer layer is now a router over per-backend
  vtables (`infer/infer.c` + `infer_backend.h`); every build ships the network
  backends alongside its local one (stub or in-process llama). The separate
  `make remote` binary is gone — remote is built in everywhere.
- `tests/net-e2e.sh` (`make net-e2e`): the same scripted write-file exchange
  through a fake server speaking all three wire shapes, with wire-contract
  assertions (auth headers, grammar routed only to llama-server, message
  structure, usage plumbing). The XP build is additionally validated under Wine
  against the same server.

### Changed
- Setup and `/model` accept the new spec forms; the status band shows `host:port`
  for servers and the model id for APIs, and hides the ctx% (the server's window
  isn't ours to measure). Networked backends default the history budget to a
  roomier `--ctx 32768` unless `--ctx` is set explicitly.

## [0.6.0] - 2026-07-03

Phase 4 of the UI-polish plan (session awareness) — and a self-updater, so a new
release no longer means re-downloading by hand.

### Added
- **`/update` — ANACHRON updates itself.** Two sources, newest wins:
  1. **GitHub releases**, over the platform's own HTTP stack (WinINet — ships with
     every XP). Whether github.com is *reachable* depends on the OS: stock XP SP3
     tops out at TLS 1.0 so it fails there (and says so plainly); POSReady-patched
     systems and newer Windows work.
  2. **An `updates\` folder** next to the exe — the fully-offline flow: download the
     newest `anachron-<ver>-winxp.exe` release asset on any machine, drop it in,
     run `/update`.
  The candidate is validated by actually running it (`--version` must report a newer
  version than the current one — the filename is only a hint), then installed with
  the classic Windows swap: the *running* exe is renamed aside (legal), the new one
  copied into its place, and `anachron-old.exe` is cleaned up on the next start. A
  failed install restores the original. POSIX builds print the from-source line
  (`git pull && make llama`). Settings, models, and the work folder are untouched.
- **`/files`** — what changed this session: per-file `+added -removed`, with
  `created` and write-count annotations. Fed by a new `on_file_change` accounting
  callback from the tool layer (rough line counts when a diff is too large to render).
- **Compaction is no longer silent.** When history has to be compacted to fit the
  context window, a notice says so — before this, the agent just seemed to forget
  (and the next prefill was mysteriously slower).

### Notes
- P4's "markdown → colour for assistant text" was evaluated and skipped: the 0.5B's
  replies are 1-3 plain lines — there is nothing to colour. This completes the
  four-phase polish plan.
- `ANACHRON_VERSION` can be overridden at build time (`-DANACHRON_VERSION='"9.9.9"'`)
  for update-flow testing.
- New Windows import: `wininet.dll` (an IE component, present on every XP).

## [0.5.5] - 2026-07-02

The "caching is failing" fix: a v0.5.1 regression made every turn after the first
re-read the whole prompt — worst exactly where it hurts most, the M170.

### Fixed
- **28 prompt tokens were silently skipped in every large prefill** (v0.5.1–v0.5.4).
  The smooth prefill bar's 4-token calibration batch decoded 4 tokens but the loop
  still advanced by the full 32-token chunk, so tokens 5–32 of the new text were never
  decoded: the model literally never saw them (on a cold start that is a slice of the
  system prompt), and the KV-cache mirror was left shifted, so the next turn's prefix
  match died at ~token 4 and **re-read the entire prompt** — the "reading prompt" bar
  on every turn, minutes long on a Pentium-M. One line: advance by what was decoded.
  Old `.anchkv` prompt-cache files self-heal (partial match, then re-saved clean).
- **Turn-to-turn reuse is now exact.** The backend remembers the text its KV cache
  covers (previous prompt + emitted output); when the next prompt is a literal
  continuation — the normal case in a conversation — it tokenizes only the appended
  tail instead of re-tokenizing everything and comparing token IDs (sampled tokens
  don't always re-tokenize identically, especially grammar-constrained JSON). An
  in-session turn now prefills just the new user message: ~20-40 tokens, seconds on
  the M170, no bar. Falls back to the old longest-prefix match after history
  compaction or for a cache loaded from disk.
- Generated tokens are committed to the cache mirror only after their forward pass
  lands, so an out-of-room stop or a mid-decode Ctrl+C can no longer leave the mirror
  claiming state the KV doesn't hold.

### Changed
- **The history-compaction budget is measured, not guessed.** The shrink threshold
  used a fixed 3 chars/token estimate — ~25% pessimistic on real prompts — so long
  sessions started rewriting (and cache-invalidating) old history well before the
  context window required it. The session now measures its real chars/token after
  every generate (with a 5% safety margin) and budgets against that.
- `history_shrink` no longer elides the tool result the model *just* received (the
  most recent messages are protected, as they already were for truncation); as a last
  resort against overflowing the window it still can.
- `ANACHRON_PROBE_DECODE=1` now also prints per-generate prefill reuse
  (`prompt=N reused=K new=M`) — this is what pinpointed the regression.

## [0.5.4] - 2026-07-02

Phase 3 of the UI-polish plan: interaction — the "is it frozen?" fix and the input
affordances every harness has.

### Added
- **A live "thinking" indicator on both consoles.** Once the current token's forward
  pass has run past ~1.5s (`ANACHRON_PTOK_MIN_SEC`), a small line is appended after the
  streamed text and updated ~4×/s: `[####......] 47s 0.2t/s` — this token's progress,
  the turn's elapsed generation time, and the rate. The ticking numbers are the
  "not hung" signal on sub-1-tok/s hardware. It replaces the POSIX-only per-token bar
  and — via `GetConsoleScreenBufferInfo`/`SetConsoleCursorPosition` cursor save/restore —
  now works on the **real XP console** too, which previously had no decode indicator at
  all. Erased before every token prints; fast hardware never sees it.
- **`!command` shell escape.** A line starting with `!` runs directly as a shell command
  in the sandbox — no model, no `[y/N]` gate (you typed it; that is the consent). Output
  renders like a tool result, with the exit code shown when non-zero.
- **Multiline input.** A line ending in `\` continues on a muted `...>` prompt; the
  backslash becomes a newline in the message.
- **Mode-as-colour prompt.** The `you>` prompt turns amber under `--yolo` as a standing
  reminder that writes and commands will not ask for confirmation.

### Changed
- Streamed text is paced to **word boundaries**: output flushes on whitespace instead of
  per token piece, so words appear whole and the XP console does far fewer writes.
  (Display-only — the token pipeline is untouched.)
- Tool results shown in the transcript are capped at **10 lines** (was 20); the model
  still receives the full text.

## [0.5.3] - 2026-07-01

Phase 2 of the UI-polish plan: the transcript structure that says "real harness" —
who is speaking, what changed, and how full the context is, at a glance.

### Added
- **End-of-turn status band.** The `(Ns - C ctx + G gen tokens)` footer is now a one-line
  muted band: `── model · ctx N% · G tok · time` (ASCII `--`/`|` on the XP console). The
  ctx figure — the turn's final prompt as a share of the context window — turns amber at
  80% and adds a "context nearly full — /new starts a fresh conversation" hint at 90%,
  so a filling 4096-token window is visible before it overflows. Durations over a minute
  read `12m34s`. The model name follows `/model` swaps.
- **Role-gutter message blocks.** Every reply block now opens with an amber `anachron`
  gutter label (printed once per turn, before the first visible output — text or tool
  call), the counterpart of the `you>` prompt, which is now blue. The transcript always
  says who is speaking. With the label in place the `== final ==` banner is gone: the
  final reply is simply the turn's last block, indented like streamed text, with the
  status band closing the turn.
- **Per-edit `+N -M` diff stat.** The `Edited file:` header of every shown diff now
  carries the added/removed line counts, coloured green/red: `Edited main.c: +3 -1`.
- **A real `/help`.** Two-column layout — commands in the tool colour, descriptions
  plain — grouped into sections, with the tips (`@path`, Ctrl+C, Enter-means-No on
  `[y/N]`) in muted text below.

## [0.5.2] - 2026-07-01

### Added
- **Double-click to run.** Launching with no arguments (e.g. double-clicking the exe on
  XP) now starts an interactive first-run **setup**: it lists the `.gguf` models it finds
  (in a `./models` folder if present, else the current folder), lets you pick one by
  number (or type a path), asks for the working folder and the lean setting, and offers to
  save them to `agent.json` so the next launch skips setup. A double-clicked window no
  longer flashes shut on a bad/missing model — it pauses on the error so you can read it.
- **`/model` with no argument lists the available models** and lets you pick one by number
  to hot-swap (giving a path still works). Searched in `./models` if present, else the
  current folder; sorted alphabetically so the numbering is stable.
- **`--lean` flag** (same as `ANACHRON_LEAN=1`): terse system prompt for a ~2.7x faster
  first turn. `lean` is now a real CLI/config option threaded through `agent_config`
  (no longer read via `getenv` inside core), and `ANACHRON_LEAN` parses as a strict boolean
  (only `1`/`true`/`yes`/`on` enable it).

## [0.5.1] - 2026-07-01

### Fixed
- The prefill ("reading prompt") progress bar now appears immediately and advances
  smoothly on slow hardware. It was redrawn only *between* 32-token decode batches — on a
  Pentium-M a batch is ~2-3 minutes, so the bar took minutes to appear and then jumped
  once per batch. It's now driven by the decode abort-callback (which fires hundreds of
  times per batch), interpolating progress by a per-token time estimate: a 0% frame shows
  at once, a tiny 4-token warm-up batch calibrates the rate, then the bar moves ~every
  150 ms regardless of batch size (measured ~1074 distinct positions across a 1190-token
  prefill, vs ~37 before). The 32-token batch size — and prefill throughput — is unchanged.
- Ctrl+C during prefill is now felt mid-batch (via the same callback) and handled as a
  clean interrupt, instead of only being checked at each 32-token batch boundary.

## [0.5.0] - 2026-07-01

Phase 1 of the UI-polish plan: make it read like a real agent harness, not a toy —
starting on the target where it looked worst.

### Added
- **Colour on Windows XP, for the first time.** A semantic colour layer (`ui_style` /
  `ui_reset`) with two backends: ANSI SGR on a POSIX/antiX terminal, and the **Win32
  Console API** (`SetConsoleTextAttribute`) on the XP console — which does not interpret
  ANSI/VT at all. One hand-tuned 16-colour role theme (assistant = amber, you = blue,
  added = green, removed = red, muted = grey, warn = yellow, …) drives both, derived from
  a single role→ANSI-index table. The blanket `_WIN32` colour-off is gone; colour is
  gated on a real console (`GetConsoleScreenBufferInfo`) and the original attributes are
  restored on exit — including a `SetConsoleCtrlHandler` so a Ctrl+C force-quit or a
  window close never leaves the console tinted.
- **Permission gate.** An interactive `[y/N]` confirmation before `write_file`, `edit`,
  and `run_command`. Default is **No** — a bare Enter, EOF, or any non-`y` answer declines,
  and pending input is flushed first so a stray keystroke can't auto-approve. Non-interactive
  / one-shot runs auto-allow (the invocation is the consent; the sandbox is the boundary).
  Bypass with `--yolo` / `ANACHRON_YOLO=1`. A decline is fed back to the model so it takes
  another approach. Closes the safety gap of a coding agent that previously ran shell
  commands and wrote files with no confirmation at all.
- **Per-line coloured diffs on both backends** (added green / removed red / header muted),
  replacing the ANSI-in-string colouring that could never render on XP.

### Fixed
- The `/stats` sparkline falls back to numbers where the terminal can't render Unicode
  block glyphs (the XP raster font) instead of printing mojibake — gated on a unicode
  capability flag set per platform.
- Windows console detection (for both colour and prompt interactivity) falls back to
  `GetFileType` when msvcrt's `_isatty` under-reports a real console.
- `ANACHRON_YOLO` now parses as a real boolean: only `1`/`true`/`yes`/`on` bypass the
  gate; `0`/`false`/`no`/`off`/empty keep it on (previously any non-empty value bypassed).

### Notes
- Validated: correct SGR on native; the permission gate exercised interactively under a
  pseudo-tty (allow / default-No / `--yolo` / `ANACHRON_YOLO=false`-still-gates); all six
  suites green; every target — including 32-bit antiX and the XP cross-build — warning-clean.
  The **Win32 colour rendering itself is compile- and logic-validated**; final visual
  confirmation is on real XP hardware, since Wine can't show Console-API colour.

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

[Unreleased]: https://github.com/BerTobi/Anachron/compare/v0.8.2...HEAD
[0.8.2]: https://github.com/BerTobi/Anachron/compare/v0.8.1...v0.8.2
[0.8.1]: https://github.com/BerTobi/Anachron/compare/v0.8.0...v0.8.1
[0.8.0]: https://github.com/BerTobi/Anachron/compare/v0.7.1...v0.8.0
[0.7.1]: https://github.com/BerTobi/Anachron/compare/v0.7.0...v0.7.1
[0.7.0]: https://github.com/BerTobi/Anachron/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/BerTobi/Anachron/compare/v0.5.5...v0.6.0
[0.5.5]: https://github.com/BerTobi/Anachron/compare/v0.5.4...v0.5.5
[0.5.4]: https://github.com/BerTobi/Anachron/compare/v0.5.3...v0.5.4
[0.5.3]: https://github.com/BerTobi/Anachron/compare/v0.5.2...v0.5.3
[0.5.2]: https://github.com/BerTobi/Anachron/compare/v0.5.1...v0.5.2
[0.5.1]: https://github.com/BerTobi/Anachron/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/BerTobi/Anachron/compare/v0.4.5...v0.5.0
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
