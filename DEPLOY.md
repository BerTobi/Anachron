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

```sh
make xp             # -> dist/xp/anachron.exe  (single static PE32, subsystem 5.01)
```

Copy `dist/xp/anachron.exe` + a model file to the XP box. In `cmd.exe`:

```
anachron.exe --model model.gguf --sandbox work
```

- Fully **static** — no DLLs to ship (imports only KERNEL32/ADVAPI32/msvcrt, all XP).
- Subsystem 5.01, SSE2-only, no Vista+ APIs; the XP-safe threadpool patch is baked
  into the linked `ggml-cpu.a`.
- On the single-core M170 use `--max-iters` modestly; threading defaults are fine
  (single core → the cond-var shim path isn't exercised).
- **UNVERIFIED at runtime:** no wine / no XP box on the dev host, so the .exe has
  never executed. Open risks to check on first real run: (1) the CPU backend
  registers under static linking (model loads), (2) the XP threadpool shim behaves.

## Rebuilding the model-backed binaries from scratch

The per-target llama/ggml libs come from the Phase-0 spike builds
(`spike-phase0/llama.cpp/build-antix-m32` and `build-xp`). If those are wiped, rebuild
them per the recipes in `HANDOFF.md` before `make antix` / `make xp`.
