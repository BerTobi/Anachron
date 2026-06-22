# Phase 0 — Inference Spike Findings (Route A: llama.cpp)

**Date:** 2026-06-18
**Goal:** De-risk the riskiest component first. Answer the brief's headline
question: *can llama.cpp build and run SSE2-only (no SSE3/SSSE3/SSE4/AVX/FMA/F16C),
and does Qwen2.5-Coder-0.5B emit coherent tokens?*

**Verdict: ✅ Route A is viable — proceed. Do not fall back to Route B (from-scratch engine).**

---

## What was tested (and where)

All work done on the dev host (x86_64 Ubuntu 22.04, gcc 11.4). The dev host can
prove the *SSE2-only code path* and *coherence*; it cannot prove *Pentium-M
wall-clock* (no period hardware here). Everything under `spike-phase0/`.

- llama.cpp `7b6c5a2` (b1), cmake 4.3.2 (installed user-local via pip — no sudo).
- Model: `Qwen2.5-Coder-0.5B-Instruct-GGUF`, both `q8_0` (645 MB) and `q4_0` (409 MB).

## Result 1 — Current llama.cpp's default x86 baseline is *above* SSE2

With `GGML_NATIVE=OFF` and AVX/AVX2/FMA/F16C all OFF, the configure still selects
`-msse4.2 -mbmi2` (`GGML_SSE42`, `GGML_BMI2`). So a naive "AVX off" build is **not**
SSE2-safe and would emit illegal instructions on a Pentium M. There is no
`GGML_SSE2`/`GGML_SSE3` toggle — SSE4.2 is the lowest selectable tier, and you drop
to SSE2 by turning **SSE42 + BMI2 off**.

## Result 2 — Strict SSE2-only build COMPILES and LINKS cleanly ✅

Configured with:

```
-DGGML_NATIVE=OFF -DGGML_SSE42=OFF -DGGML_BMI2=OFF
-DGGML_AVX=OFF -DGGML_AVX2=OFF -DGGML_FMA=OFF -DGGML_F16C=OFF -DGGML_AVX512=OFF
-DCMAKE_C_FLAGS / CXX_FLAGS = "-msse2 -mno-sse3 -mno-ssse3 -mno-sse4.1 -mno-sse4.2 -mno-avx -mno-bmi2"
```

`llama-cli` built with **no errors** → current llama.cpp has **no unguarded SSE3+
intrinsics** that break an SSE2 compile. (The SSE3/SSSE3 uses in ggml-cpu are all
`#if`-guarded and compile out; `vec.cpp` has real `__SSE2__` paths.) No need for an
older commit.

## Result 3 — objdump confirms the binary is genuinely SSE2-only ✅

Disassembled `libggml-cpu/-base/-.so`, `libllama.so`, `libllama-common.so`:

- **Zero** AVX (`v`-prefixed vector) instructions.
- **Zero** true >SSE2 instructions (SSE3/SSSE3/SSE4.1/SSE4.2/BMI2/AVX512).
  - The only initial hit, `pinsrw`, is an **SSE2** instruction (the byte/dword/qword
    inserts `pinsrb/d/q` are SSE4.1; the *word* form is SSE2) — false positive.
- SSE2 SIMD **is** actually present (`movdqa`, `paddd`, `pmullw`, `pshufd`, `mulps`,
  `cvtdq2ps`, …) — i.e. it's a real SSE2 vector path, not a pure-scalar fallback.

## Result 4 — Coherence: PASS ✅

Strict SSE2-only build, q8_0, temp 0, prompt "write `int is_prime(int n)`":

```c
int is_prime(int n) {
    if (n <= 1) return 0;
    if (n <= 3) return 1;
    if (n % 2 == 0 || n % 3 == 0) return 0;
    for (int i = 5; i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) return 0;
    }
    return 1;
}
```

Correct edge cases + 6k±1 optimization. Genuinely coherent code, not gibberish.

## Result 5 — Performance + a quant surprise (dev-host x86_64, SSE2-only build)

| quant | threads | prompt t/s | generation t/s |
|-------|---------|-----------:|---------------:|
| q8_0  | 8       | 24.7       | **11.0**       |
| q8_0  | 1       | 11.1       | **6.7**        |
| q4_0  | 8       | 9.9        | 6.0            |
| q4_0  | 1       | 2.4        | 1.9            |

**On SSE2-only, q8_0 is ~3.5× faster than q4_0.** q4_0's fast dequant depends on
SSSE3 `pshufb` / AVX2; without them it takes a slow path, while q8_0's int8→int16
`pmaddwd` vectorizes fine under plain SSE2. This is an ISA-path property, so the
*ordering* should hold on the M170.

➡️ **Quant recommendation for Route A SSE2: use q8_0**, not q4_0 — opposite of the
usual "smaller = faster" assumption. q8_0 (~530 MB weights) still fits the 2 GB
ceiling (< 1.5 GB working set with a 2–4k KV cache).

---

## Phase 0 exit criteria — status

The brief's gate: *"coherent tokens on **both** targets, and tok/s recorded for each."*

| Criterion | Status |
|---|---|
| SSE2-only build feasible | ✅ proven (compile + objdump) |
| Coherent tokens | ✅ proven |
| tok/s recorded (dev host, SSE2-only) | ✅ done (table above) |
| antiX i686 target build | ✅ built (`ELF 32-bit i386`), **runs + coherent**, SSE2-only verified |
| Windows XP (mingw) target build | ✅ built static PE32, **subsystem 5.01**, SSE2-only, XP-safe imports (requires the XP patch below) |
| **True Pentium-M / antiX-hardware runtime** | ❗ open — no period hardware on dev host (XP `.exe` not executed: no wine) |

The dev host now settles everything except true period-hardware wall-clock: SSE2-only
is real & coherent, and **both target triples build and pass every static
compatibility gate**. What remains is running on an actual M170 / antiX box.

## Result 6 — Both target builds produced & verified

Toolchains used (installed on the dev host): `g++-mingw-w64-i686` (XP cross),
`g++-multilib` + `gcc-multilib` + `libc6-dev-i386` (antiX i686 native).

**antiX / i686** — `cmake` with `-m32 -msse2 -mno-sse3 …`, `GGML_OPENMP=OFF`,
built `llama-cli`. Binary is `ELF 32-bit LSB, Intel 80386`. objdump: SSE2-only
(no >SSE2/AVX). Coherence: identical correct `is_prime`. Clean tok/s (q8_0):

| i686 (-m32) | t=1 | t=8 |
|---|---|---|
| generation | 6.2 t/s | 6.9 t/s |

(Single-thread 32-bit ≈ 64-bit SSE2 — 6.2 vs 6.7 — so the 32-bit ABI penalty is
small on the 1-core path that matters for the M170; 32-bit just scales worse across
threads, which the single-core M170 can't use anyway.)

**Windows XP** — static cross-build of `llama-simple` (the minimal libllama+ggml
link path), `BUILD_SHARED_LIBS=OFF`. Verified on the produced `.exe`:

| Check | Result |
|---|---|
| Subsystem version | **5.01** (Major 5 / Minor 1) = Windows XP |
| DLL imports | only `KERNEL32`, `ADVAPI32`, `msvcrt` — all XP-safe |
| Vista+ symbols | none (no ConditionVariable/SRWLock/ThreadPowerThrottling/GetTickCount64) |
| Static | fully static — no libstdc++/libgcc/winpthread DLL deps |
| SSE2-only | no AVX/SSE3+; SSE2 SIMD present |

Cannot execute it here (no wine / no XP machine) → runtime correctness, including the
XP condition-variable shim under multithreading, awaits M170/XP validation. (Note the
M170 is single-core → intended to run `n_threads=1`, where the cond-wait path is never
taken.)

## Required llama.cpp modifications for the XP target (carry as vendored patches)

1. **Build defines:** `-D_WIN32_WINNT=0x0501 -DWINVER=0x0501` — pins the API ceiling
   to XP. This alone compiles out llama.cpp's Win8 `SetThreadInformation(...,
   ThreadPowerThrottling, ...)` call (`ggml-cpu.c`, already guarded by
   `#if _WIN32_WINNT >= 0x0602`; mingw otherwise defaults `_WIN32_WINNT` to a modern OS).
2. **Toolchain `CMAKE_SYSTEM_PROCESSOR i686`** (not `x86`) so `ggml_get_system_arch()`
   matches `^(x86_64|i686|AMD64|amd64)$` and selects the **x86 SSE2 kernels** instead of
   the generic scalar fallback (`-DGGML_CPU_GENERIC`).
3. **CMake feature flags:** `GGML_NATIVE=OFF GGML_SSE42=OFF GGML_BMI2=OFF` (+ all AVX
   off). Even with AVX off, the default x86 tier is `-msse4.2 -mbmi2`; these two must be
   explicitly off for SSE2-only.
4. **Source patch — `ggml/src/ggml-cpu/ggml-cpu.c` threading defs:** upstream uses
   `SRWLOCK` + `CONDITION_VARIABLE` (Vista+) for the Windows threadpool mutex/cond. The
   patch in this spike replaces them with an XP-safe `CRITICAL_SECTION` + manual-reset
   `Event` broadcast condition variable. (Marked `// ANACHRON XP patch` in the source.)

## Decision

- **Proceed with Route A (llama.cpp).** The `infer_*` interface (Phase 1) wraps
  `libllama` + `libggml` built with the SSE2-only flag set above.
- **Default quant: q8_0** for the SSE2 target.
- Link the harness against **just `libllama` + `ggml`** (the `llama-simple` example's
  dependency set) — *not* server/httplib/mtmd, which the current `llama-cli` drags in.
- For XP: build **static** (`BUILD_SHARED_LIBS=OFF`, `-static`) + subsystem 5.01.

## To reproduce / extend

```sh
# strict SSE2-only native build (done):
cd spike-phase0/llama.cpp
cmake -B build-sse2 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGGML_NATIVE=OFF -DGGML_SSE42=OFF -DGGML_BMI2=OFF \
  -DGGML_AVX=OFF -DGGML_AVX2=OFF -DGGML_FMA=OFF -DGGML_F16C=OFF -DGGML_AVX512=OFF \
  -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=OFF \
  -DCMAKE_C_FLAGS="-msse2 -mno-sse3 -mno-ssse3 -mno-sse4.1 -mno-sse4.2 -mno-avx -mno-bmi2" \
  -DCMAKE_CXX_FLAGS="-msse2 -mno-sse3 -mno-ssse3 -mno-sse4.1 -mno-sse4.2 -mno-avx -mno-bmi2"
cmake --build build-sse2 --target llama-cli -j8

# to extend to the real targets, install first (needs sudo):
#   sudo apt install g++-mingw-w64-i686 gcc-multilib libc6-dev-i386
```
