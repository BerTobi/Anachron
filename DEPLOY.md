# ANACHRON — deploying to the target machines

Two cross-builds produce relocatable bundles under `dist/`. Both need a GGUF model
file (not bundled — it's hundreds of MB). Default/ recommended model:
`qwen2.5-coder-0.5b-instruct-q8_0.gguf` (q8_0 is the SSE2-fast quant; see PHASE0-FINDINGS).

## antiX (32-bit Linux, i686)

```sh
make antix          # -> dist/antix/  (binary + 4 .so, found via an $ORIGIN rpath)
```

`dist/antix/` is relocatable — copy the whole folder anywhere (USB stick, the antiX
box) and run it in place:

```sh
./anachron-llama-antix --model /path/to/model.gguf --sandbox ./work
```

- Libraries resolve via `$ORIGIN` (next to the binary), so no `LD_LIBRARY_PATH` needed.
- Requires **32-bit `libstdc++.so.6` / `libgcc_s.so.1` / libc** on the target — present
  on a stock antiX install (it's a 32-bit distro). They are intentionally not bundled.
- On the single-core Pentium-M, run with `ANACHRON_THREADS=1` (export before launch).
- Verified: builds + runs coherently on the x86_64 dev host via multilib. NOT yet run
  on real Pentium-M hardware.

## Windows XP (32-bit)

A prebuilt, static **`anachron-<ver>-winxp.exe`** is attached to each GitHub release —
download it plus a model and run; no toolchain needed. This is the **real** model build.
(Do NOT use `make win` / `anachron-stub.exe` — that's a no-model stub for testing the
Win32 layer.) To build the real exe yourself from a clean clone:

```sh
make xp             # -> dist/xp/anachron-xp.exe  (single static PE32, subsystem 5.01)
```

`make xp` links the vendored XP llama.cpp artifacts in `prebuilt/xp/`, so it builds from
a clean clone without the (untracked, embedded) `spike-phase0/llama.cpp` checkout. After
rebuilding llama.cpp for XP, refresh them with `make xp-vendor` and commit `prebuilt/xp/`.

Copy `dist/xp/anachron-xp.exe` (or the downloaded release exe) + a model to the XP box.
In `cmd.exe`:

```
anachron-xp.exe --model model.gguf --sandbox work
```

- Fully **static** — no DLLs to ship (imports only KERNEL32/ADVAPI32/msvcrt, all XP).
- Subsystem 5.01, SSE2-only, no Vista+ APIs.
- **Validated under Wine**: the .exe runs, parses args, loads a model and prefills.
  This is the first confirmation it executes (previously objdump-only). Real
  Pentium-M / XP hardware remains the final check.

### Memory: the 32-bit ceiling (read before picking a model)

A 32-bit process has ~2 GB of user address space, and the weights want one large
contiguous buffer — so model choice is constrained:

- **Use the q8 0.5B** (~640 MB). It is the model confirmed to *generate*. On a clean
  2 GB XP boot it should fit; under Wine it failed to allocate only because Wine's
  graphics libraries fragment the address space.
- If the model fails to load with an allocation error, try `ANACHRON_MMAP=1`
  (on 32-bit the default is mmap **off** — read into a heap buffer; the override flips
  loading to memory-mapping, which may succeed where the buffer alloc didn't).
- **1.5B models (~1.6 GB) will not fit** a 32-bit process — don't try them on XP.
- A *smaller-than-q8* quant must be produced from the **f16 / safetensors** weights.
  Requantizing *down from the q8 GGUF* (q4_0, q5_K_M) yields a model that loads but
  generates nothing (the 0.5B degrades to immediate end-of-text), so don't requant q8.

## Rebuilding the model-backed binaries from scratch

The per-target llama/ggml libs come from the Phase-0 spike builds
(`spike-phase0/llama.cpp/build-antix-m32` and `build-xp`). If those are wiped, rebuild
them per the recipes in `HANDOFF.md`. The XP libs are also vendored under `prebuilt/xp/`
(see `make xp-vendor`), so `make xp` does not need `build-xp` present.
