# ANACHRON — Native Agentic Coding Harness for Legacy 32-bit Machines

> Working codename, rename freely. This file is the operating brief for Claude Code on this project. Read it fully before writing any code, and treat the **Hard Constraints** section as non-negotiable.

## Mission

Build a minimal, self-contained agentic coding assistant that runs **natively on the metal** of late-XP-era 32-bit hardware, with **local model inference** and **no network**. One shared C codebase produces two binaries:

- **Windows XP (32-bit)** — primary reference target is a Dell XPS M170 (Pentium M, SSE2, 2 GB RAM).
- **antiX Linux (32-bit / i686)** — runs on the same class of hardware.

The model is small on purpose: **Qwen2.5-Coder-0.5B**, quantized, loaded from a GGUF file. The agent reads/edits files and runs commands on the local machine, in a loop, the same way Claude Code does — except the weights live on the same machine as the harness.

This is the "small local" first iteration. A future iteration may offload inference to a LAN GPU server; **keep the inference layer behind a clean interface so that swap is cheap, but do not build the network path now.**

## Hard Constraints (non-negotiable)

These are the guardrails. If any task would require violating one, **stop and flag it** rather than working around it.

1. **32-bit x86 only.** No 64-bit assumptions. Pointers are 4 bytes. Keep model + working set under ~1.5 GB (2 GB machine minus OS). 0.5B at Q4 is ~350 MB of weights, so 32-bit file offsets and a single mmap are fine.
2. **Windows XP target ceiling:** PE subsystem version **5.01**, no Win32 APIs newer than XP SP3 (no condition variables, no `GetTickCount64`, no Vista+ calls). CPU baseline is **SSE2 only** — the Pentium M has no SSE3/SSSE3/AVX. Any SIMD must compile and run with `-msse2 -mno-sse3`.
3. **antiX target baseline:** i686 + glibc, SSE2 baseline (assume no AVX).
4. **Language: C (C99).** Avoid C11 threads/atomics and anything a period-appropriate or i686-targeting compiler won't support. If a C++ dependency is introduced (e.g. llama.cpp), it must build under the XP-compatible toolset and stay within these same constraints.
5. **No modern runtime at runtime.** No Node, Python, .NET, or interpreter. The shipped artifact is a native binary plus the GGUF file. Build-time tooling on a modern dev host is fine.
6. **No network.** All inference is local and in-process (or a local child process). Do not add HTTP/sockets in this iteration.
7. **One codebase.** Platform differences are isolated behind a thin abstraction layer; core logic is platform-independent and testable on the dev host.

## Target Hardware Reference

| | Dell XPS M170 (XP) | antiX box |
|---|---|---|
| CPU | Pentium M, 1 core, ~2.13 GHz | i686, SSE2 baseline |
| SIMD | **SSE2 only** (no SSE3) | SSE2 baseline |
| RAM | 2 GB (hard ceiling) | assume ~2 GB |
| OS | Windows XP 32-bit | antiX 32-bit |

Design every memory and performance decision against the M170. If it runs there, it runs anywhere in scope.

## Repository Layout

```
/core        Platform-independent agent: loop, message state, prompt assembly,
             tool-call grammar + parser, tool dispatch.
/platform    platform.h + platform_win32.c + platform_posix.c
             (spawn process & capture stdout/stderr, file read/write/list, timing).
/infer       Inference behind a stable interface (see below). Pluggable backend.
/tools       Tool implementations: read_file, write_file, list_dir, run_command.
/models      The GGUF model file (not committed — document how to fetch).
/build       Makefile/CMake + cross-compile notes for each target.
main.c       Entry point: minimal console UI, arg parsing, wire-up.
```

## Inference Backend — Phase 0 Decision (highest risk)

This is the riskiest component and must be de-risked **first**, before any agent work. Expose it behind a stable interface:

```c
typedef struct infer_ctx infer_ctx;
infer_ctx *infer_init(const char *gguf_path, int n_ctx);
/* Generate constrained by an optional grammar; emit tokens via callback.
   Returns when an end condition is hit. */
int infer_generate(infer_ctx *c, const char *prompt,
                   const char *grammar /* nullable */,
                   void (*on_token)(const char *piece, void *ud), void *ud);
void infer_free(infer_ctx *c);
```

**Route A (try first): llama.cpp as the backend.** It already supports the Qwen2.5 architecture and its tokenizer, and GGUF loading. Build it for each target with AVX/AVX2/FMA/F16C **off** (`-DGGML_AVX=OFF -DGGML_AVX2=OFF -DGGML_FMA=OFF -DGGML_F16C=OFF`) and verify it runs SSE2-only on the M170. **Risk:** current llama.cpp may assume SSE3+ in places. If a current build won't run SSE2-only, try an older commit, then fall back to Route B.

**Route B (fallback / "living history" path): a from-scratch C engine** in the llama2.c spirit, implementing the Qwen2.5 dense transformer (RMSNorm, RoPE, grouped-query attention, SwiGLU MLP) with Q4_0/Q8_0 dequant and **hand-written SSE2 int8/int16 matmul kernels** (`<emmintrin.h>`, `_mm_madd_epi16`). More work and you must also parse GGUF and implement the Qwen BPE tokenizer (large ~151 k vocab — non-trivial), but it is guaranteed to build on XP and gives full control.

**Phase 0 exit criteria:** Qwen2.5-Coder-0.5B produces coherent tokens from a fixed prompt on *both* targets, and tokens/sec is recorded for each. Do not proceed until this passes.

## Model Notes (Qwen2.5-Coder-0.5B)

- Standard **dense** transformer (RMSNorm, RoPE, GQA, SwiGLU) — deliberately chosen over newer hybrid-architecture small models because a vanilla transformer is what a from-scratch engine and SSE2 kernels can actually run.
- Large vocabulary (~151 k). Embedding and output matrices are a big fraction of the weights at this size; the BPE tokenizer with its merges table is the most tedious part of any custom engine. This is the strongest argument for Route A.
- Quantization: prefer **Q4_0** or **Q8_0** for the simplest dequant path (especially for custom SSE2 kernels). Q4_K_M only if the backend handles it.
- Context: native is large, but **cap to ~2–4 k tokens** to bound KV-cache memory and keep wall-clock per turn sane. Short context is a feature here.
- Capability expectation: at 0.5B this is a **constrained single-step assistant**, not a reliable autonomous agent. Design the UX around "write this small function / explain this error / propose one edit," not long unattended plans.

## Tool-Call Protocol (grammar-constrained)

A 0.5B model will not reliably emit clean structured tool calls on its own. **Constrain decoding to a grammar** so every tool call is parseable by construction (llama.cpp GBNF on Route A; a hand-written constrained sampler on Route B). Keep the schema tiny:

- `read_file(path)`
- `write_file(path, content)`
- `list_dir(path)`
- `run_command(cmd)`  ← capture stdout+stderr, return exit code
- `final(message)`     ← ends the loop

Use the smallest format the model handles well under the grammar (strict JSON, or a line-based form if that parses more reliably at this size). Every tool runs inside a configurable working-directory sandbox; reject paths that escape it.

## Agent Loop

```
assemble(system_prompt + tool_schema + history)
for i in 0..MAX_ITERS:
    text = infer_generate(prompt, tool_grammar)
    call = parse_tool_call(text)
    if call.kind == final: print(call.message); break
    obs  = dispatch(call)            # via platform layer, sandboxed
    history.append(call, obs)
    if context_too_long: compact_oldest(history)
```

Cap `MAX_ITERS` low (e.g. 8). Stream tokens to the console as they generate so the user sees progress on slow hardware.

## Build & Toolchain

**antiX (do this target first — it's easier):**
```
gcc -m32 -msse2 -O2 -std=c99 ...    # native i686 toolchain
```

**Windows XP (cross-compile from a modern Linux dev host):**
```
i686-w64-mingw32-gcc -msse2 -mno-sse3 -O2 -std=c99 \
    -Wl,--major-subsystem-version=5 -Wl,--minor-subsystem-version=1 ...
# Prefer static linking. Avoid any API newer than XP.
```
(MSVC `v141_xp` toolset is an acceptable alternative for an MSVC build.)

Keep the build a single Makefile (or CMake) with a target switch. No exotic build deps.

## Milestones

- **Phase 0 — Inference spike.** Model emits coherent tokens on both targets; record tok/s. *(Gate.)*
- **Phase 1 — Core loop, stub backend.** Build the agent loop, tool dispatch, platform layer, and grammar/parser against a fake "echo" backend, fully testable on the dev host.
- **Phase 2 — Wire real inference.** Connect the chosen backend behind `infer_*`; enable grammar-constrained tool calls.
- **Phase 3 — antiX build & validation.** End-to-end on 32-bit antiX.
- **Phase 4 — Windows XP build & validation.** End-to-end on the M170 (the hard target).
- **Phase 5 — Polish.** Minimal TUI, config (model path, sandbox dir, iter cap), error handling, README with build steps for both targets.

## Out of Scope (this iteration)

- Remote/LAN inference (keep the interface swap-ready, but don't build it).
- Models larger than ~1B, anything multimodal, anything needing AVX or >2 GB.
- Long autonomous multi-step runs; rich diff UI; package installers.

## How To Work (operating principles)

- Move in **small, verifiable increments**; prefer something that compiles and runs on the dev host before cross-compiling.
- **Never** introduce a modern dependency, a 64-bit assumption, or an API newer than XP without flagging it against the Hard Constraints first.
- When a design choice has real tradeoffs (engine route, quant format, tokenizer strategy), **explain the options and your recommendation** rather than silently picking — surface the decision so it can be steered.
- Keep the platform-independent core genuinely independent; if you reach for an OS call in `/core`, that's a smell — push it behind `platform.h`.
- Test inference output for coherence, not just "it ran."
