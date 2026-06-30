# ANACHRON — Handoff Log

_Last updated: 2026-06-18. Read this first, then `Instructions.md` (the operating
brief) and `PHASE0-FINDINGS.md` (full Phase 0 results)._

## TL;DR — where the project is

- **Project:** ANACHRON — a native, no-network, local-inference agentic coding
  harness in C99 for late-XP-era 32-bit hardware (Pentium M / SSE2, 2 GB). One
  codebase → two binaries: **Windows XP 32-bit** + **antiX i686**. Model:
  Qwen2.5-Coder-0.5B-Instruct (GGUF). Full spec in `Instructions.md`.
- **Phase 0 (inference spike) is DONE and PASSED.** Route A (llama.cpp) is viable
  and chosen. Both target triples build and pass every static compatibility gate;
  the model emits coherent code SSE2-only.
- **Phase 1 (core loop + tools + platform + grammar, stub backend) DONE.** Full
  harness source exists, builds warning-clean, unit tests + e2e pass, Win32 path
  cross-compiles XP-safe. See "Phase 1 — status".
- **Phase 2 (real inference + conversational REPL) WORKING on the dev host.**
  `infer/infer_llama.cpp` wraps libllama+ggml (SSE2 spike build); `make llama` →
  `./anachron-llama`. The model loads, decodes grammar-constrained tool calls, and
  drives the loop. `main.c` is now an interactive **conversation** (persistent
  history across turns), not one-shot. See "Phase 2 — status".
- **Key finding:** the harness is solid; the **0.5B model is a weak agent** — it
  emits perfectly-formed tool calls (grammar guarantees it) but plans poorly and
  loops/degrades on multi-step tasks. This is the model, not the harness, and is
  exactly what the brief predicted. Levers: few-shot system prompt, single-step
  tasks, KV-cache continuation for speed. Next milestones: Phases 3–4 (real HW).
- **Nothing is committed to git** (this dir is not a git repo). The llama.cpp clone
  under `spike-phase0/` is its own shallow git repo with one local patch.

## Decisions locked in (so you don't re-litigate them)

- **Inference backend = Route A: llama.cpp**, wrapped behind the brief's `infer_*`
  interface. Do NOT build the from-scratch engine (Route B) — not needed.
- **Default quant = q8_0**, NOT q4_0. On SSE2-only, q8_0 is ~3.5× faster (q4_0's
  fast dequant needs SSSE3/AVX2). q8_0 (~530 MB) fits the 2 GB ceiling.
- **Link the harness against `libllama` + `ggml` only** (the `llama-simple`
  dependency set) — NOT the server/httplib/mtmd that `llama-cli` drags in.
- **Context cap ~2–4k**, `n_threads=1` on the M170 (single core).

## Dev-host environment (Ubuntu 22.04, x86_64)

This is a modern x86_64 box, NOT period hardware. It can prove SSE2-only code paths,
32-bit builds, XP static-link/imports, and coherence — but NOT true M170 wall-clock.

Installed / available:
- `gcc`/`g++` 11.4, `make`, `git`, `objdump`, `wget`, `curl`, `python3`+`pip3`.
- **cmake 4.3.2 + ninja** installed user-local via pip → live at `~/.local/bin`.
  **You must `export PATH="$HOME/.local/bin:$PATH"`** before cmake/ninja.
- **mingw XP cross-compiler:** `i686-w64-mingw32-gcc/g++` (GCC 10). Use the
  **`-posix`** variants (`i686-w64-mingw32-g++-posix`) — the default is win32-threads,
  whose libstdc++ lacks `std::thread`. winpthreads here is XP-safe (verified: no Vista
  imports; uses GetProcAddress fallback).
- **32-bit native (antiX):** `gcc-multilib`, `g++-multilib`, `libc6-dev-i386` (the
  user installed these mid-session via sudo).

NOT available: **`wine`** (so the XP `.exe` cannot be executed here), `ccache`, no
Pentium-M / antiX hardware.

## Directory map

```
Instructions.md                    operating brief (hard constraints — read fully)
PHASE0-FINDINGS.md                 full Phase 0 results + required XP patch list
HANDOFF.md                         this file
spike-phase0/
  toolchain-mingw-xp.cmake         CMake toolchain for the XP target
  patches/
    0001-anachron-xp-safe-sync.patch   XP-safe CRITICAL_SECTION+Event cond-var (see below)
  models/
    qwen2.5-coder-0.5b-instruct-q8_0.gguf   (645 MB) ← default
    qwen2.5-coder-0.5b-instruct-q4_0.gguf   (409 MB)
  llama.cpp/                       shallow clone @ 7b6c5a2 (own git repo; 1 local patch)
    build-sse2/                    native x86_64 strict SSE2-only (runnable here)
    build-antix-m32/               i686 -m32 SSE2-only (runnable here; llama-cli)
    build-xp/                      Windows XP static PE32 (bin/llama-simple.exe)
```

## The vendored llama.cpp patch (IMPORTANT)

`spike-phase0/llama.cpp/ggml/src/ggml-cpu/ggml-cpu.c` is modified in the working tree
(not committed). It replaces the Windows threadpool's Vista `SRWLOCK` +
`CONDITION_VARIABLE` with an XP-safe `CRITICAL_SECTION` + manual-reset-`Event`
broadcast condition variable (marked `// ANACHRON XP patch`). If the clone is reset or
re-cloned, reapply it:

```sh
cd spike-phase0/llama.cpp
git apply ../patches/0001-anachron-xp-safe-sync.patch
```

The other XP requirements are build flags only (no source change) — see below.

## Reproduce the builds

Always first: `export PATH="$HOME/.local/bin:$PATH"` and `cd spike-phase0/llama.cpp`.

**Native x86_64, strict SSE2-only (fast iteration / coherence checks):**
```sh
SSE2="-msse2 -mno-sse3 -mno-ssse3 -mno-sse4.1 -mno-sse4.2 -mno-avx -mno-bmi2"
cmake -B build-sse2 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGGML_NATIVE=OFF -DGGML_SSE42=OFF -DGGML_BMI2=OFF \
  -DGGML_AVX=OFF -DGGML_AVX2=OFF -DGGML_FMA=OFF -DGGML_F16C=OFF -DGGML_AVX512=OFF \
  -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=OFF \
  -DCMAKE_C_FLAGS="$SSE2" -DCMAKE_CXX_FLAGS="$SSE2"
cmake --build build-sse2 --target llama-cli -j8
```

**antiX i686 (-m32):** same as above but add `-m32` to the flags + linker, and
`-DGGML_OPENMP=OFF`, into `-B build-antix-m32` (set `CMAKE_EXE_LINKER_FLAGS=-m32`
and `CMAKE_SHARED_LINKER_FLAGS=-m32`).

**Windows XP (static PE32, subsystem 5.01):**
```sh
SSE2="-msse2 -mno-sse3 -mno-ssse3 -mno-sse4.1 -mno-sse4.2 -mno-avx -mno-bmi2 \
      -D_WIN32_WINNT=0x0501 -DWINVER=0x0501"
cmake -B build-xp -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw-xp.cmake -DBUILD_SHARED_LIBS=OFF \
  -DGGML_NATIVE=OFF -DGGML_SSE42=OFF -DGGML_BMI2=OFF \
  -DGGML_AVX=OFF -DGGML_AVX2=OFF -DGGML_FMA=OFF -DGGML_F16C=OFF -DGGML_AVX512=OFF \
  -DGGML_OPENMP=OFF -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=OFF \
  -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_SERVER=OFF \
  -DCMAKE_C_FLAGS="$SSE2" -DCMAKE_CXX_FLAGS="$SSE2"
cmake --build build-xp --target llama-simple -j8
```
Requires the source patch above + toolchain `CMAKE_SYSTEM_PROCESSOR i686` (already set
in `toolchain-mingw-xp.cmake`; `x86` would fall back to scalar `GGML_CPU_GENERIC`).

## Quick verification recipes

- **Run / coherence (native or i686):** `LD_LIBRARY_PATH=<build>/bin <build>/bin/llama-cli
  -m models/...q8_0.gguf -p "..." --single-turn -n 160 --temp 0 -t 1` (note: new
  llama-cli prints `[ Prompt: X t/s | Generation: Y t/s ]`, not "eval time").
- **SSE2-only check:** objdump `-d` the `libggml-cpu`/`-base` libs (or the XP `.exe`),
  extract mnemonics, confirm no SSE3/SSSE3/SSE4/AVX/BMI2. (`pinsrw`/`pextrw` are SSE2 —
  ignore them.)
- **XP safety:** `i686-w64-mingw32-objdump -p build-xp/bin/llama-simple.exe` →
  MajorSubsystemVersion 5 / Minor 1; DLL imports only KERNEL32/ADVAPI32/msvcrt; grep
  for `ConditionVariable|SRWLock|ThreadPowerThrottling|GetTickCount64` → none.

## Open gaps / risks for whoever continues

1. **No real-hardware runtime yet.** The XP `.exe` has never executed (no wine). The
   XP condition-variable shim's *multithread* correctness and on-M170 wall-clock are
   unverified. On the M170 use `n_threads=1` (single core) — then the cond-wait path
   isn't taken, side-stepping the shim risk for the primary target.
2. **Perf on the M170 will be far below dev-host numbers** (here: q8_0 single-thread
   ~6 t/s in 32-bit). Expect low single digits or less. Design UX around one small
   step per turn + token streaming.
3. The llama.cpp clone is shallow + locally patched, uncommitted. Consider vendoring
   it properly (submodule or committed snapshot) when the real repo is initialized.

## Phase 1 — status (DONE on dev host)

Built the platform-independent agentic core against a **scriptable stub backend** (no
real inference). Layout matches the brief:

```
core/       strbuf, json (minimal parser), sandbox (lexical path containment),
            toolcall (parse model text -> tool call), prompt (ChatML assembly +
            history + compaction), agent (the loop)
platform/   platform.h + platform_posix.c + platform_win32.c (file I/O, dir list,
            run_command w/ stdout+stderr capture in a cwd, monotonic clock)
infer/      infer.h (the brief's infer_* interface) + infer_stub.c
tools/      tools.c — the 5 sandboxed tools + dispatch
grammars/   toolcall.gbnf — per-tool GBNF (NOT wired yet; Phase 2 passes it to llama)
main.c      arg parsing + streaming console transcript (Claude-Code-style)
tests/      test_core.c (unit) + run-e2e.sh + demo-script.txt
Makefile    `make` / `make test` / `make e2e` / `make win` / `make clean`
```

Verified on the dev host:
- `make` — warning-clean (`-std=c99 -Wall -Wextra -msse2 -mno-sse3`).
- `make test` — json / toolcall / sandbox unit tests PASS.
- `make e2e` — full loop list_dir→write_file→read_file→run_command→final with real
  FS effects in a temp sandbox, streaming + transcript working.
- `make win` — mingw cross-compile of the Win32 platform path: **PE32 subsystem
  5.01**, imports only KERNEL32 + msvcrt, **zero Vista+ symbols**. (Compile/link
  check only — can't run, no wine. The full product also pulls ADVAPI32 + needs the
  Phase-0 threading patch once llama.cpp is linked in Phase 2.)

Design notes / decisions made this phase (revisitable in Phase 2):
- **Tool-call format = Qwen-native `<tool_call>{json}</tool_call>`** (Hermes-style).
  Aligns grammar + the model's training. Parser is lenient (tolerates prose around
  it, falls back to a bare `{...}`); a malformed turn triggers a **re-prompt**, not
  an abort — that recovery is the agentic backbone.
- **Sandbox is lexical** (string-only, in `/core`): rejects `..` escape, drive
  letters, `:` streams. Known gap: doesn't catch pre-existing symlinks pointing out
  (fine for Phase 1; add realpath containment when hardening).
- Stub backend: default emits one `final`; `ANACHRON_STUB_SCRIPT=<file>` (entries
  split on a line that is exactly `===`) drives multi-step flows for tests.
- The 5 tools are a `switch` in `tools_dispatch` — adding one (e.g. a surgical
  `edit`) is a case + a system-prompt line + a grammar alternative; the loop is
  untouched.

## Phase 2 — status (working on dev host)

- `infer/infer_llama.cpp` — the only C++ TU — wraps libllama+ggml behind the C
  `infer_*` interface (greedy/temp-0 decode, optional GBNF grammar, CPU-only,
  `ANACHRON_THREADS` env). **KV-cache reuse:** keeps a `cached` token vector,
  finds the longest shared prefix with the new prompt, `llama_memory_seq_rm`s the
  diverged tail, and decodes only the new suffix — so the big system+few-shot+
  history prefix is processed once, not every turn. Measured: a follow-up turn
  dropped from **33.5s -> 3.4s** (~10x); positions auto-continue from the cache
  via `llama_batch_get_one`.
- `make llama` builds `./anachron-llama`: C core compiled C99, the one cpp as C++,
  linked to `spike-phase0/llama.cpp/build-sse2/bin` libs (`-Wl,--no-as-needed` so
  the ggml-cpu backend self-registers; rpath set so no `LD_LIBRARY_PATH` needed).
- `main.c` is now an **interactive conversation** (REPL, persistent history across
  turns) when run with no task args; one-shot when a task is on argv.
- `grammars/toolcall.gbnf` is loaded by `main.c` and passed through. **GBNF gotcha
  fixed:** llama.cpp rejects a line-leading `|`; alternation must be trailing `|`
  with `( ... )` alternatives (see `c.gbnf` in the spike).
- **Talk-or-act fix (important, VERIFIED working):** an always-on grammar forced a
  tool call EVERY turn, so the model couldn't converse — it hallucinated tool calls
  on a greeting. Three coordinated changes fixed it:
  1. **Lazy grammar** (`llama_sampler_init_grammar_lazy_patterns`, trigger regex
     `[\s\S]*?(<tool_call>)`): free generation until `<tool_call>` appears, then
     the JSON is constrained.
  2. **Parser-based discriminator** in `core/agent.c`: ACT if the output parses as
     a tool call (wrapped OR bare JSON); re-prompt if a `<tool_call>` tag is present
     but botched; otherwise TALK (deliver via `on_message`, end the turn).
  3. **Few-shot priming** in `core/prompt.c` (`FEWSHOT`): 3 example turns teaching
     greet->plain text, task->tool call, result->summary. Biggest behavior lever.
  Verified 3-turn run: "are you going to help me code?" -> plain reply; "7 times 6?"
  -> "42"; "create todo.txt containing buy milk" -> write_file -> file created.
  Rough edges handled: (a) bare JSON without the `<tool_call>` wrapper still runs
  via the lenient parser; (b) **invented tools** — the 0.5B loved turning "write a
  function" into a fake `{"name":"add_numbers",...}` call. Fixed by a few-shot
  "write a function -> inline code" example + a system-prompt "never invent a tool"
  rule + an agent guard: bare JSON that starts with `{` and has `"name"` but is not
  a valid tool is re-prompted (not printed as a reply). Verified: "write a C
  function that adds two numbers" now returns the code inline.

  **Hit the 0.5B ceiling (do NOT keep prompt-whacking):** getting it to reliably
  emit `run_command` for a "compile/run X" request failed across 3 prompt variants.
  Failure modes seen: overwrote the source file via write_file (early), then (after
  few-shot+guard) NARRATES fake success ("Compiled reverse.c with no errors") WITHOUT
  emitting the tool call. The model prefers describing an action to taking it once a
  conversation grows. This is a capacity limit, not a harness bug. Real levers, not
  more prompt tuning: a larger local model (1.5B/3B Qwen-Coder if RAM allows; note
  the 2GB M170 ceiling), or the brief's future LAN-GPU offload (the `infer_*`
  interface is ready for it). Current few-shot leaves the working cases intact
  (greet->talk, write-function->inline code, save->write_file).

  **1.5B test (downloaded `qwen2.5-coder-1.5b-instruct-q8_0.gguf`, 1.76GB):** did
  NOT fix the run/compile gap. Chats correctly ("7x6"->"42") but for "compile/run
  this" it gave an EMPTY reply or HALLUCINATED program output - never emitted
  `run_command`, no binary produced. Also ~117s per COLD turn on the SSE2-only libs
  (no AVX, 3x params); follow-ups ~5s via KV reuse.

  **Diagnostic RESOLVED - it's the model, not the harness.** Ran the same compile
  request on the 0.5B with grammar ON vs `--no-grammar`: IDENTICAL behavior (both
  just echoed "Compile reverse.c by running: gcc..." as plain text; no `run_command`,
  no binary). The lazy grammar is dormant until `<tool_call>` appears, so it cannot
  suppress a tool call - confirmed empirically. Conclusion: small models (0.5B AND
  1.5B) reliably chat + write code, but will NOT reliably EXECUTE a command - they
  restate/narrate instead. Genuine capability ceiling. Real fix = a much more capable
  model via the LAN-GPU offload path (`infer_*` is ready); a 0.5B->1.5B bump does not
  help and costs ~5x speed on SSE2. Practical use today: let the model WRITE code,
  compile/run it yourself.

Run it:
```sh
make llama
./anachron-llama --model spike-phase0/models/qwen2.5-coder-0.5b-instruct-q8_0.gguf \
    --sandbox /tmp/work --max-iters 6
# then type tasks at the  you>  prompt; /quit to exit
```

Verified-good behavior: grammar-constrained tool calls, dispatch, streaming,
multi-turn memory. Verified-weak: the 0.5B model plans poorly (reads before
creating, repeats failed calls, forgets to call `final`). Harness is not the limit.

## Reliability hardening — Tier 1 (research-driven) DONE

Informed by a multi-harness research pass (see the harness-research artifact / the
field converges on "structure around a weak model, verify everything"). Three
cheap, pure-C, offline changes that target the exact failures observed above:

1. **Verify-on-write guardrail** (`core/verify.{c,h}` + `tools/tools.c`): after a
   `write_file`, run a portable **balance check** (braces/parens/brackets +
   string/comment aware) for code files, plus a **`cc -fsyntax-only` check** for C
   when a compiler is present (auto-detected in `main.c::detect_cc`). On failure
   the write is **reverted** (restore prior content, or `remove()` if new) and the
   exact error is fed back so the loop retries. Catches the dropped-semicolon and
   truncation classes; `--no-verify` disables. Degrades to balance-only with no
   compiler (XP). Missing-`#include` fatals and signal-killed/absent compilers are
   treated as "can't check -> skip", never a false reject (uses `-I. ./<path>`).
2. **Deterministic loop guards** (`core/agent.c`): repeated-identical-call detection
   (intervene after 3) kills the read-loop death-spiral; force-continue re-prompts a
   reply that narrates an action (\"I'll...\") but emits no tool call (bounded to 2,
   so genuine chat still ends the turn).
3. **Anti-narration prompt rules + richer tool descriptions** (`core/prompt.c`):
   lifted from Qwen Code / Cline / Codex - "use tools to act, text only to
   communicate", no "Sure/Okay", <=3 lines, dedicated-tool-over-shell, verify before
   `final`.

Tested: `make test` (unit `verify_balance`), `make verify-e2e` (deterministic stub:
bad write reverted, good write kept), + manual regression (missing-include accepted,
bad-semicolon rejected). **Adversarially reviewed** via a 4-dimension code-review
workflow: it found 5 real bugs (missing-include false-reject, signal-exit-code
false-reject, `-`-leading-filename flag injection, escaped-newline line miscount,
unchecked revert write) - ALL FIXED. All five build targets warning-clean.

## Function-calling fine-tune experiment (Hammer) - DONE, clearly positive

Tested `hammer2.0-0.5b-q8_0.gguf` (MadeAgents Hammer 2.0, a Qwen2.5-Coder-0.5B
function-calling fine-tune via "function masking"; downloaded to spike-phase0/models;
2.1 at 0.5/1.5B is only published as safetensors - would need converting). A pure
`--model` swap, no harness change. Result vs base Qwen2.5-Coder-0.5B on the exact
tasks the base FAILED:
- "save to add.c" -> base embedded a fake write_file() in a code block (no file);
  HAMMER emitted a real `<tool_call>` write_file -> add.c created. WIN.
- "compile it" -> base narrated fake success; HAMMER emitted a real run_command,
  ran gcc, saw the linker error, and correctly diagnosed "main is not defined". WIN.
- Greeting -> Hammer TALKED (did not spuriously tool-call; the feared FC-overcalling
  did not happen).
Compatibility was a non-issue: Hammer natively emits ANACHRON's `<tool_call>{json}`
format (it followed the in-context few-shot), so the lazy GBNF + lenient parser work
as-is. Speed ~ same as base 0.5B.
CAVEAT (honest): within a SINGLE multi-step instruction ("create X then run it") it
does the first step and stops - multi-step decomposition is still weak at 0.5B (matches
the research: small models drop on multi-turn). Across SEPARATE user turns it chains
fine. Good fit for the brief's "one small step per turn" UX.
ADOPTED: `./run.sh --hammer` selects it. Recommendation: Hammer as the default for
agentic/tool use; base Qwen-Coder is marginally better at pure code-writing.

## Plan/TODO scaffold (Tier-2) — BUILT, tested, GATED OFF by default (`--plan`)

Added an externalized `plan` tool (record steps -> re-inject each turn -> nudge
through them; Claude Code TodoWrite pattern) to fix the "does step 1 and stops" gap.
**It failed on every small local model tested** and is OFF by default:
- Hammer-0.5b: fixates - re-calls `plan` forever, never executes (loop).
- Qwen2.5-Coder-0.5b: records the plan, then echoes the steps as TEXT, never executes
  (even through 4 force-continue nudges).
- Qwen2.5-Coder-1.5b: empty output - the plan-heavy prompt derailed it entirely.
In every case it was STRICTLY WORSE than without it (Hammer at least did step 1
unscaffolded). Root cause = the documented small-model failure: "better at calling ->
worse at declining" + can't reliably transition plan->execute. Cross-model evidence,
not one data point.
DECISION (per user): keep the code, don't ship it on. Gated behind `--plan`
(cfg.plan_enabled): default build never offers the tool (uses `grammars/toolcall.gbnf`
+ plan-free prompt); `--plan` switches to `grammars/toolcall-plan.gbnf` + the plan
addendum/few-shot + the agent's TC_PLAN handler (plan-once + abandon-on-loop) +
plan-aware force-continue. Default behavior is unchanged/clean (verify-e2e + tests
pass; default banner shows the plan-free grammar).
WHY KEEP IT: it should work with a CAPABLE model. The intended path is the brief's
LAN/GPU offload (below) - a 7B+ on a desktop GPU streamed to the M170.

**Grammar mode-gating DONE (Tier-2):** once a plan is recorded, the agent swaps to a
plan-free grammar (`cfg.grammar_act`, main loads `toolcall.gbnf` as the act grammar in
`--plan` mode) so the model PHYSICALLY cannot emit `plan` again and must execute. This
fixed the infinite-replan loop. Re-tested on Hammer-0.5b: the loop is gone, but it then
executed steps OUT OF ORDER (ran `python3 hello.py` before writing it) and looped on the
failing command (the Tier-1 repeat-guard caught + bounded that). So mode-gating is a
real, correct improvement to the `--plan` path, but a 0.5b still can't SEQUENCE steps -
`--plan` remains off-by-default for small local models, now genuinely ready to retry on
a capable model (where gate + plan should work).

## Remote / GPU inference (future direction, user-requested)

User wants to run inference on a remote box (an RTX 2080 Ti) and have the M170 act as
a thin client - exactly the brief's "offload to a LAN GPU server; keep the inference
layer behind a clean interface." The `infer_*` interface (infer.h) is already the swap
point: add an `infer_remote.c` backend that ships the prompt to a server (the GPU box
runs llama.cpp/server with a 7B+ model) and streams tokens back, selected at link/run
time like the stub/llama backends. This unlocks the capable-model features that small
local models can't sustain (multi-step planning, the `--plan` scaffold). NOTE: the
brief says "do not build the network path now" for the on-metal iteration - so this is
a deliberate future phase, and any sockets/HTTP must still respect the XP/antiX targets
if the CLIENT runs on the M170 (or keep the client logic platform-clean behind platform.h).

## Context discipline (Tier-2) — DONE

The SWE-agent ACI lesson (shape observations to fit the tiny 2-4k window), done
HARNESS-side so it needs no model cooperation and can't backfire like `--plan`:
`core/obsfmt.c` (`obs_capped`, pure + unit-tested) line+byte-caps tool output
(200 lines / 8KB, line-aligned, with a "N more lines; M total" note). Wired into
`tools.c`: read_file capped, run_command capped + a "(command produced no output)"
sentinel, list_dir capped to 200 entries. No line numbers on read_file on purpose
(whole-file write_file -> numbered lines would risk the model copying "12| " into a
file). All builds warning-clean; `obsfmt` unit test + e2e + verify-e2e pass.

Tier 2 status: ALL DONE. Context discipline (below), grammar plan/act mode-gating
(above), and now: windowed read_file paging (`core/obsfmt.c::obs_window`; read_file
takes an optional `offset`, shows a "lines A-B of T" window + "offset=N to continue"
footer; whole-line soft byte cap so an over-long line is shown intact, never lost);
fuzzy `edit` tool (`core/edit.c::edit_apply`: exact-unique match then a
whitespace-tolerant line-based search/replace; verify-gated + snapshotted); and
snapshots (`commit_write` saves prior content to a sibling `<file>.anbak`, hidden from
list_dir, for operator recovery). All unit-tested (obsfmt/edit) + behaviorally tested.

## Remote / GPU-offload backend — DONE (`infer/infer_remote.c`, `make remote`)

The brief's LAN-offload path. A minimal raw-socket HTTP/1.1 client that POSTs the
prompt + GBNF grammar to a llama.cpp `server` `/completion` endpoint (which takes
`prompt` and `grammar` natively - a clean match for `infer_generate`) and returns the
generated text. Run a big model on the GPU box:
`llama-server -m big.gguf --host 0.0.0.0 --port 8080`, then on the client:
`ANACHRON_REMOTE=host:port ./anachron-remote --sandbox ./work` (--model is ignored;
default target 127.0.0.1:8080). This removes the real ceiling (the tiny local CPU
model) and is where `--plan` + multi-step actually become usable.
- **Auth:** set `ANACHRON_REMOTE_KEY=<key>` to send `Authorization: Bearer <key>`
  (matches `llama-server --api-key`); the key is CR/LF-sanitized to prevent header
  injection. Tested with/without against an auth-enforcing mock (401 -> graceful).
- **Access from outside the LAN:** plain HTTP, so run it over a network-layer VPN/SSH
  tunnel (Tailscale recommended -> connect to the tailnet IP, zero code change), NOT a
  raw public port (llama-server is unauthenticated by default). A public-HTTPS tunnel
  (ngrok/Cloudflare) would need TLS added to this client - not done.
- v1 is NON-STREAMING (one request -> one JSON response; whole reply delivered to
  on_token at once) and POSIX-only (`#ifndef _WIN32`); handles both Content-Length
  and chunked responses. Tested end-to-end against a Python mock /completion server
  (both encodings) - full agent loop runs over the wire.
- Follow-ups: token streaming (SSE), and an XP/Winsock client (the brief defers the
  network path out of the on-metal iteration, so the XP thin-client is a later phase).

Both Tier-2 and the remote backend were **adversarially code-reviewed** (workflow):
6 real bugs found + ALL FIXED - notably a heap over-read in the de-chunk path (a
malicious/huge chunk size overflowed `i + sz > len`; fixed to `sz > len - i`), an
obs_window mid-line byte-cap that lost/looped on over-long lines (fixed to whole-line
windowing), a read-loop that masked transport errors as EOF, and an inflated
list_dir "more entries" count. All 6 build targets warning-clean; full test suite green.

## Discovery & context features (Crush-spec batch) — DONE

After cross-referencing a Crush-inspired XP-agent spec (crush-inspired-xp-agent-spec.md;
see the crush-feature-gap artifact: ANACHRON was ahead on the engine/safety, behind on
discovery/UX/persistence), added the "discovery & context" batch — all harness-side so
they can't backfire on small models:
- **search + glob** (tools.c + `core/glob.c` unit-tested matcher): grep text across
  files / find files by wildcard. A budget+depth-capped recursive `walk()` with a
  lightweight ignore list (dotfiles/.anbak/node_modules/build/dist/target/obj/bin =
  a stand-in for .gitignore). New TC_SEARCH/TC_GLOB + grammar alternatives + a "use
  search/glob to find things instead of reading files" prompt rule. Fills the #1 gap
  (discovery was burning the tiny context).
- **@file mentions** (main.c expand_mentions): `@path` in a user line inlines that
  file (sandbox-resolved, size-capped); word-boundary check leaves emails alone.
- **AGENTS.md / CRUSH.md** auto-load (main.c load_project_context) into the system
  prompt (size-capped); `cfg.project_context` -> prompt_render.
- **Logging** (`--log PATH` / $ANACHRON_LOG): appends request/model/result/notice
  lines via `cfg.on_log`; default off. Aimed at debugging flaky tiny-model tool calls.
- Tools are now: read_file/write_file/edit/list_dir/run_command/search/glob/final
  (+ plan under --plan).

Adversarially reviewed (workflow): 4 real bugs found + FIXED - walk() recursion not
truly bounded (no depth cap, budget only charged on files, follows dir symlinks ->
added a depth cap + per-entry budget charge); search-in-subdir lost the path prefix
(results weren't sandbox-root-relative -> read_file couldn't round-trip them);
newline-in-pattern partial match (now requires the whole match within the line);
looks_binary only scanned 1KB (now whole buffer). All 6 targets warning-clean; tests +
e2e + verify-e2e green. Known follow-up: a symlinked dir is still followed (bounded by
the depth cap, just produces duplicate paths) — wants lstat/d_type symlink-skip in the
platform layer. Remaining Crush-batch items NOT done: full .gitignore parsing, session
persistence, config file, /model + /undo + more slash-commands, usage display, colour
diffs, OpenAI-compatible provider (all assessed in the crush-feature-gap artifact).

## Usability batch (slash commands, sessions, config) — DONE

The everyday-driving features. All harness-side (no model cooperation needed):
- **Slash commands** (`main.c handle_command`): `/help` `/new` `/clear` `/undo`
  `/save [name]` `/sessions` `/resume <name>` `/quit` `/exit`. Wired into the REPL
  (returns CMD_NOT_A_COMMAND / CMD_HANDLED / CMD_QUIT).
- **Session persistence** (`core/agent.c` agent_session_save/load): conversation
  history serialized as a JSON array of `{role,text(,elided)}` under
  `<sandbox>/.anachron-sessions/<name>.json`. `/save` writes (dir created lazily via
  the new `plat_mkdir`), `/sessions` lists, `/resume` reloads (replaces history).
- **`/undo`** (`cmd_undo`): reverts the last successful write/edit from its sibling
  `.anbak` snapshot. The session tracks `last_write` (set in agent.c after a
  successful TC_WRITE_FILE/TC_EDIT).
- **Config file** (`main.c load_config`): `agent.json` / `.anachron.json` in the CWD
  sets defaults (model, sandbox, grammar, log, ctx, max_iters, verify, plan,
  grammar_enabled); CLI flags override (config applied before the flag loop).
- **`plat_mkdir`** added to the platform layer (POSIX `mkdir`+`stat`; Win32
  `_mkdir`+`GetFileAttributesA`).
- New CLI inverse-toggles `--verify` / `--no-plan` so a config-set bool is always
  reversible from the command line.

Adversarially reviewed (workflow wbfnnf4nj: 11 confirmed findings → 9 distinct bugs,
ALL FIXED): (1, HIGH/data-loss) `/resume` didn't reset `last_write` → a following
`/undo` clobbered an unrelated file from the previous conversation; now load clears it.
(2) owned config strings leaked on the `-h/--help` early-exit. (3) Windows reserved
device names (con/nul/com1…) passed session-name sanitization → would bind to a device
on XP; now rejected. (4) config could flip verify/plan with no CLI way back → added
`--verify`/`--no-plan`. (5) `/resume` of a corrupt file said "no such session"; load now
returns -2 so the message says "corrupt". (6) `elided` compaction flag dropped across
save/load (benign re-elision); now persisted. (7) lossy name sanitization silently
collided distinct names (`a b` and `ab` → same file, silent overwrite); now names that
change under sanitization are rejected. (8) `/undo` mis-reported a failed write-back as
"newly created"; now distinguishes read-fail vs write-fail. (9) leading whitespace
bypassed command parsing and 31+char verbs mis-parsed; both handled. All 5 build targets
(anachron/test/remote/win/llama) warning-clean; unit + e2e + verify-e2e green; every fix
regression-tested through the stub binary.

## Power batch (gitignore, diff, symlink-skip, token usage, /model) — DONE

The last of the Crush backlog except the OpenAI provider (user: "not going to be using
API"). Five features:
- **lstat symlink-skip:** `plat_dirlist` gained `int *is_symlink` (posix lstat+S_ISLNK,
  win32 reparse-point); `walk()` skips ALL symlinks — kills the cycle the previous batch
  only bounded, and refuses to read outside the sandbox via a link. (Deliberate trade-off:
  in-sandbox file symlinks are now invisible to search/glob; read/write by explicit path
  still work — noted in the code.)
- **Real `.gitignore`:** new pure `core/gitignore.c` (parse → match, comments/negation/
  dir-only/anchored-vs-floating/last-match-wins, reuses glob_match). `walk()` loads
  `<root>/.gitignore` per discovery call and skips matches; the old static junk-list stays
  as the always-on safety skip. Leading spaces kept significant (git semantics).
- **Diff-on-edit:** new pure `core/diff.c` (LCS line diff, context=3, collapsed runs,
  size cap 1500 lines, optional ANSI colour). `commit_write` shows a diff of an
  overwritten EXISTING file via a new `tool_ctx.on_diff` callback (UI only — NOT in the
  model observation). Colour auto-on for an interactive POSIX TTY, forced off on Win32 and
  under `--no-color`/`"color":false`.
- **Token usage:** new `infer_last_usage()` across all three backends (llama exact, remote
  from tokens_evaluated/predicted, stub ~chars/4). agent.c accumulates per turn; main
  prints `(Ns - C ctx + G gen tokens)`.
- **`/model <path>`:** swaps the inference backend in place (re-init, free old, repoint
  both main's handle and the session's cfg.infer); conversation survives.
Also added `plat_isatty_stdout`, `plat_mkdir` (prior batch), `--no-color`/`--verify`/
`--no-plan` flags and the `color` config key.

Adversarially reviewed (workflow wr6nhf3zo: 10 confirmed → 8 distinct, all FIXED/addressed):
(MEDIUM) diff.c used raw malloc/realloc/calloc instead of the abort-on-OOM xmalloc/xrealloc
used everywhere else (NULL-deref + realloc leak on OOM) → switched. (LOW) a failed
infer_generate left stale token counts that agent.c re-added → agent.c now only accumulates
on a 0 return, and both real backends reset counts up-front. (LOW) gitignore trimmed leading
spaces (git keeps them significant) → removed + unit-tested. (LOW) docstring overpromised
negation (can't re-include under an excluded dir, same as git) → documented + regression
test. (LOW) symlink skip hides in-sandbox file symlinks → documented as intentional. (LOW)
remote dechunk hex size had no overflow guard (clamp already saved it) → added a cap. (LOW)
usage "prompt" label was last-iteration not a turn-sum like gen → relabeled "ctx". (LOW)
trailing-newline-only change shows no diff → confirmed by-design (file still written), left.
All 5 targets warning-clean; unit (incl new gitignore+diff tests) + e2e + verify-e2e green;
verified on the real llama backend (exact token counts) and the stub.

## Context-overflow wedge — FIXED

A real multi-turn session (write code → write more → "save it") bricked itself: the
prompt grew past the 2048-token window, the llama backend hard-errored ("prompt (N
tokens) exceeds context"), and every later turn produced nothing. Root causes: (1) the
old `history_compact` only elided tool results — it never touched the big @file-inline
user turns or code-filled assistant turns, so a code-heavy session couldn't be shrunk at
all; (2) the budget compared history chars to `ctx*3`, ignoring the ~1.5k-token fixed
system+few-shot+AGENTS.md overhead prepended every turn, and reserved no generation room.
Fixes: `core/prompt.c` `history_shrink` (escalating one-step shrink — oldest tool result
first, then truncate the oldest large non-pinned/non-tool-call message; keeps index 0 and
the last two turns); `core/agent.c` now renders then shrinks-and-re-renders until the
*actual* prompt fits `(ctx - ctx/4)*3` chars (proportional gen headroom, conservative
3 chars/token); default `--ctx` 2048→4096. Unit-tested (`test_history_shrink`) and
validated on the real Hammer model at the old tight ctx=2048 — a 3-turn growing session
(two code generations, then a write_file) completed with no overflow. Unstick a
wedged live session with `/new` or a larger `--ctx`/`$ANACHRON_CTX`.

## Sampler: repeat penalty + runaway-stop guard — DONE

A real session degenerated: asked to re-emit a big file it had printed earlier, the 0.5B
(greedy decode) fell into emitting one token forever (endless spaces) until the user hit
Ctrl-C. Fixes in `infer/infer_llama.cpp`: (1) a gentle repeat penalty in the sampler chain
`llama_sampler_init_penalties(64, 1.1, 0, 0)` — presence-only, mild, so it doesn't distort
code indentation or tool-call JSON; (2) a hard runaway-stop guard in the gen loop — if the
same token id repeats 40× in a row, stop cleanly instead of grinding to the context cap.
Validated on Hammer-0.5b: the tetris re-emit that spewed before now terminates finite (the
write was then correctly rejected by verify because the model's C was invalid — guardrail
working); greeting still talks (no over-call); an explicit `write_file` still emits and
passes the syntax check. Net: the spew is gone with no tool-use regression. (Root re-emit
problem — a 0.5B reproducing a big file verbatim — is the model ceiling; the remote backend
with a capable model is the real path.)

## REPL hotkeys & terminal hygiene — DONE

Ctrl+C used to kill the process (default SIGINT) and stray scroll/keystroke bytes leaked
into the prompt as garbage. Fixes:
- **Ctrl+C interrupts the current generation, not the process** — new `core/interrupt.c`
  (SIGINT handler sets a `volatile sig_atomic_t`; a 2nd press before it's cleared restores
  the default handler and re-raises, so a stuck process can still be force-killed).
  `infer_llama` polls `interrupt_pending()` per generated token AND between 32-token
  chunks of the prompt decode (the decode was one big call before, so an interrupt during
  a slow first-turn decode wasn't felt until it finished — now ~2-3s). The agent loop
  aborts the turn on interrupt without recording/parsing the partial output; main installs
  the handler, clears it around each turn, and prints `(interrupted)`. Validated on the
  real model: SIGINT mid-turn → `(interrupted)`, process survives, answers the next prompt,
  `/quit` exits 0.
- **Terminal-input hygiene** — `plat_flush_input` (tcflush / FlushConsoleInputBuffer)
  discards buffered input before each prompt so scroll/keystrokes during a generation don't
  become the next command; on startup (TTY, POSIX) the REPL emits the DECRST sequences to
  disable any leftover mouse-reporting mode a prior program left on, so the wheel scrolls
  the terminal's scrollback instead of injecting escape tokens. `plat_isatty_stdout` gates
  colour and the mouse reset.
All 5 targets warning-clean; unit + e2e + verify-e2e green.

## Next steps

- **Lift model success rate (cheap):** PARTIALLY DONE — the FEWSHOT in `core/prompt.c`
  now demonstrates `read_file` (was write_file + run_command only). Confirmed lever:
  Hammer-0.5b refused all read requests until a read_file example was added; now
  `read strutil.h` / `list the files` work, greeting still talks (no over-call). Tiny
  models still choke on out-of-distribution phrasings (e.g. "show me the feature tour in
  README.md" refuses; `read README.md` hallucinates) — the 0.5B ceiling. For reliably
  showing a file regardless of model, use the **`@file` mention** (inlined pre-inference).
  Still open: search/glob/edit have no few-shot demo either; consider adding if those
  prove flaky. Tune sampling if needed (currently greedy).
- **Speed:** DONE — KV-cache reuse implemented (see Phase 2 status; ~10x on
  follow-up turns). Remaining: record real tok/s on hardware; consider trimming the
  few-shot once the model is warmed, and `n_threads=1` tuning for the single-core M170.
- **Phase 3 — antiX i686: DONE (dev-host validated).** `make antix` ->
  `anachron-llama-antix`, a 32-bit i386 ELF PIE that links the i686 SSE2-only
  libllama/ggml from `build-antix-m32/bin` (compiles C with `gcc -m32`, the C++ TU
  with `g++ -m32`; same `--no-as-needed` trick + RUNPATH). Runs on the dev host
  (multilib) and answers coherently ("7x6"->"42", ~46s on 0.5B — 32-bit + SSE2 is
  slower than the x86_64 build, as Phase 0 predicted). **Bug fixed en route:**
  `core/agent.c` used `strstr` without `#include <string.h>` — harmless-by-luck on
  64-bit (pointer truncated to int) but the `-m32 -Wall` build flagged it; now
  included. Both native and antix builds are warning-clean.
  - **`make antix` now produces a RELOCATABLE bundle** in `dist/antix/` (binary +
    4 `.so`, `$ORIGIN` rpath via `--disable-new-dtags` so transitive lib->lib deps
    resolve too). Proven portable: copied to `/tmp/anachron-usb`, ran from `/tmp`
    with no `LD_LIBRARY_PATH` ("9 plus 10 equals 19"). Needs 32-bit libstdc++/libgcc
    on the target (present on antiX). Still UNVERIFIED on real Pentium-M hardware.
- **Phase 4 — Windows XP: DONE (build + Wine-validated; real-XP runtime unverified).**
  `make xp` -> `dist/xp/anachron-xp.exe`: real inference statically linked (mingw
  `g++-posix`, `build-xp` static `.a` libs in the llama-simple lib order + Win32
  system libs; the XP threadpool patch is baked into `ggml-cpu.a`). Single
  self-contained 8.7MB PE32: subsystem **5.01**, imports only KERNEL32/ADVAPI32/
  msvcrt, **zero Vista+ symbols**, SSE2-only. CANNOT run here (no wine). First-run
  risks to check on a real XP/M170 box: (1) CPU backend registers under static
  linking so the model loads (linked without `--whole-archive`, matching the
  spike's llama-simple); (2) the XP cond-var threadpool shim under multithread
  (side-stepped with single-core `n_threads=1`). Deployment steps in `DEPLOY.md`.
- **Phase 5:** polish (config, README, TUI). Remaining cross-cutting: real-HW
  validation of both targets; record tok/s on the M170.

Keep moving in small increments that compile/run on the dev host before cross-compiling.
