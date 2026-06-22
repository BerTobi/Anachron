# ANACHRON

A from-scratch, native **C99 agentic coding harness** with **local, in-process
inference** — built to run on late-Windows-XP-era 32-bit hardware (Pentium M, SSE2,
2 GB RAM). One codebase compiles to **Windows XP 32-bit** *and* **antiX i686** binaries,
runs a small GGUF model (Qwen2.5-Coder-0.5B / Hammer-0.5B) entirely on the CPU with **no
network**, and drives a Claude-Code-style tool loop: read/write/edit files, run commands,
search & glob, with the model talking when it should and acting when it should.

> Why: a self-contained coding agent for a machine that can't reach a modern cloud — the
> whole loop (tool protocol, verify-on-write, context discipline, recovery) is the
> product; the 0.5B model is just the smallest thing that can drive it.

## What's interesting

- **Pure link-time backend swap** behind a 4-function `infer_*` interface: a stub (tests),
  in-process `llama.cpp` (on-metal), and an HTTP client to a remote `llama-server` (offload
  a big model to a desktop GPU; the thin client stays XP-safe).
- **Lazy GBNF grammar + lenient parser** so a tiny model can *converse* yet still emit
  exactly-shaped `<tool_call>` JSON when it acts.
- **Verify-on-write guardrail**: structural balance + `cc -fsyntax-only` for C; bad writes
  are reverted and the error fed back. Snapshots (`.anbak`) power `/undo`.
- **Context discipline**: a render-and-shrink loop keeps the prompt inside the window so
  long sessions never wedge; tool output is line/byte-capped; `read_file` pages large files.
- **Discovery & UX**: `search`/`glob` with a real `.gitignore` matcher, `@file` mentions,
  `AGENTS.md` auto-load, session save/resume, `/model` hot-swap, diff-on-edit, token usage.
- **XP-safe throughout**: no Win32 API newer than SP3, SSE2 only, warning-clean under
  `-Wall -Wextra`, sandbox-confined file access.

## Layout

```
core/        platform-independent engine (agent loop, prompt, tools-call parse,
             json, sandbox, verify, edit, diff, gitignore, obsfmt, strbuf)
tools/       the tool implementations (read/write/edit/list/run/search/glob)
platform/    the thin OS abstraction (platform_posix.c / platform_win32.c)
infer/       inference backends: infer_stub.c, infer_llama.cpp, infer_remote.c
grammars/    GBNF tool-call grammars
tests/       unit tests (make test) + e2e / verify-e2e scripts
examples/    a small demo workspace (strutil) for trying the agent
spike-phase0/ Phase-0 llama.cpp integration: patches/ + the XP mingw toolchain file
             (the vendored llama.cpp clone and the model weights are NOT in git)
```

## Build

Requires a C99 + C++ toolchain. The `llama`/`antix`/`xp` targets link a locally-built
`llama.cpp` (SSE2-only) and a GGUF model, neither of which is committed (see below).

```sh
make           # stub backend, no model — proves the loop (no GPU/model needed)
make test      # unit tests
make e2e       # scripted end-to-end; make verify-e2e for the write guardrail
make llama     # in-process inference binary (needs spike-phase0/ set up)
make win       # Windows XP cross-build (mingw)  -> anachron.exe
make remote    # remote/offload client            -> anachron-remote
```

### Getting a model + llama.cpp (not in the repo)

The model weights (multi-GB) and the `llama.cpp` clone are git-ignored. To run real
inference, place a GGUF under `spike-phase0/models/` (e.g.
`qwen2.5-coder-0.5b-instruct-q8_0.gguf` or a Hammer-0.5B function-calling fine-tune) and
build the SSE2-only `llama.cpp` static libs the Makefile links. See `PHASE0-FINDINGS.md`
and `DEPLOY.md` for the exact spike setup and cross-build details.

The **Hammer** function-calling models that `run.sh` expects are converted from the
official MadeAgents safetensors with this repo's own llama.cpp (so the GGUF matches the
linked `libllama`). For example:

```sh
pip install gguf sentencepiece
hf download MadeAgents/Hammer2.0-0.5b --local-dir /tmp/h
python3 spike-phase0/llama.cpp/convert_hf_to_gguf.py /tmp/h --outtype q8_0 \
    --outfile spike-phase0/models/hammer2.0-0.5b-q8_0.gguf
```

Use Hammer **2.0** (0.5B/1.5B) for agentic use — it emits ANACHRON's `<tool_call>`
format reliably. Hammer **2.1** converts fine but is specialized to its own tool schema
and won't emit our format, so it's not recommended here.

## Run

```sh
./run.sh                 # interactive REPL, 0.5B Qwen-Coder
./run.sh --hammer        # Hammer-0.5B function-calling fine-tune (best tool use)
./run.sh "write a function and save it to add.c"   # one-shot
```

In the REPL, `/help` lists commands (`/new` `/undo` `/save` `/sessions` `/model` …) and
`@path` attaches a file. Try the demo: copy `examples/strutil/` into the sandbox and ask
the agent to read, search, or edit it.

## Versioning

Semantic versioning (`MAJOR.MINOR.PATCH`), pre-1.0 until validated on real hardware. The
version lives in `core/version.h` and is printed by `anachron --version`. Releases are
tagged `vX.Y.Z` and recorded in [`CHANGELOG.md`](CHANGELOG.md) (Keep a Changelog format) —
to cut one: bump `core/version.h`, move the `Unreleased` notes into a dated entry, commit,
then `git tag -a vX.Y.Z`.

## Status & docs

Phases 0–4 (spike → core loop → in-process inference → antiX i686 → Windows XP build) are
done on the dev host; on-real-Pentium-M/XP-hardware validation is the remaining arc.
`HANDOFF.md` is the running state-of-the-project log; `CHANGELOG.md` is the release history;
`Instructions.md` is the operating brief and hard constraints; `DEPLOY.md` covers the
cross-builds.

## A note on the model ceiling

A 0.5B model reliably *chats and writes code* but is erratic at multi-step execution and
verbatim re-emission of large files. That's expected — the harness is built so a more
capable model over the remote backend can drive it without code changes. Treat the local
0.5B as "writes code, you run it"; use `@file` for reliable file viewing.
