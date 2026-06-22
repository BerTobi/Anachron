# ANACHRON — Phase 1 build (stub backend, dev-host testable).
#
# Targets:
#   make            native dev-host binary  -> ./anachron        (POSIX platform)
#   make test       build + run core unit tests
#   make e2e        full agent-loop run over a scripted stub (real FS effects)
#   make win        cross-compile the Win32 path (mingw) -> ./anachron.exe
#                   COMPILE/LINK CHECK ONLY — cannot run here (no wine).
#   make clean
#
# Phase 2 will add a `llama` backend (BACKEND=llama) linking libllama + ggml from
# spike-phase0/. For now only the stub backend exists.

CC      ?= cc
WINCC   ?= i686-w64-mingw32-gcc

INCLUDES = -Icore -Iplatform -Iinfer -Itools
WARN     = -Wall -Wextra
# Match the target CPU baseline even on the dev host: SSE2 only.
SSE2     = -msse2 -mno-sse3
CFLAGS  ?= -std=c99 -O2 $(WARN) $(SSE2) $(INCLUDES)

CORE   = core/strbuf.c core/json.c core/sandbox.c core/toolcall.c core/verify.c core/obsfmt.c core/edit.c core/glob.c core/gitignore.c core/diff.c core/prompt.c core/agent.c
TOOLS  = tools/tools.c
INFER  = infer/infer_stub.c
MAIN   = main.c

NATIVE_SRC = $(CORE) $(TOOLS) $(INFER) platform/platform_posix.c $(MAIN)
WIN_SRC    = $(CORE) $(TOOLS) $(INFER) platform/platform_win32.c $(MAIN)
TEST_SRC   = tests/test_core.c core/strbuf.c core/json.c core/sandbox.c core/toolcall.c core/verify.c core/obsfmt.c core/edit.c core/glob.c core/gitignore.c core/diff.c core/prompt.c

# --- Phase 2 real backend: link libllama + ggml (built SSE2-only in the spike) ---
LLAMA_DIR    = spike-phase0/llama.cpp
LLAMA_INC    = -I$(LLAMA_DIR)/include -I$(LLAMA_DIR)/ggml/include
LLAMA_LIBDIR = $(LLAMA_DIR)/build-sse2/bin
# --no-as-needed forces the ggml-cpu backend's registration constructor to run.
LLAMA_LINK   = -L$(LLAMA_LIBDIR) -Wl,-rpath,$(abspath $(LLAMA_LIBDIR)) \
               -Wl,--no-as-needed -lllama -lggml -lggml-base -lggml-cpu -Wl,--as-needed \
               -lpthread -lm -ldl
LL_CSRC      = $(CORE) $(TOOLS) platform/platform_posix.c $(MAIN)

# --- Phase 3: antiX i686 (-m32) real-inference build. Links the 32-bit SSE2-only
# libs from build-antix-m32 (built in Phase 0). The dev host can run the result. ---
LLAMA_LIBDIR_ANTIX = $(LLAMA_DIR)/build-antix-m32/bin
ANTIX_DIST         = dist/antix
ANTIX_RUNLIBS      = libllama.so.0 libggml.so.0 libggml-base.so.0 libggml-cpu.so.0
# -L finds the libs at LINK time; rpath '$ORIGIN' makes the RUNTIME loader look
# next to the binary (so the bundle is relocatable). --disable-new-dtags emits the
# old DT_RPATH, which (unlike DT_RUNPATH) also covers transitive lib->lib deps.
LLAMA_LINK_ANTIX   = -L$(LLAMA_LIBDIR_ANTIX) -Wl,-rpath,'$$ORIGIN' -Wl,--disable-new-dtags \
                     -Wl,--no-as-needed -lllama -lggml -lggml-base -lggml-cpu -Wl,--as-needed \
                     -lpthread -lm -ldl

# --- Phase 4: Windows XP (-posix mingw) real-inference build. Links the STATIC
# build-xp libs (the XP threading patch is already baked into ggml-cpu.a). Produces
# a single self-contained static PE32, subsystem 5.01. Recipe mirrors the spike's
# llama-simple link (lib order + Win32 system libs matter). ---
XPCC      = i686-w64-mingw32-gcc-posix
XPCXX     = i686-w64-mingw32-g++-posix
XP_DIST   = dist/xp
XP_LIBDIR = $(LLAMA_DIR)/build-xp
XP_STATIC = $(XP_LIBDIR)/src/libllama.a $(XP_LIBDIR)/ggml/src/ggml.a \
            $(XP_LIBDIR)/ggml/src/ggml-cpu.a $(XP_LIBDIR)/ggml/src/ggml-base.a
XP_SYSLIBS = -lkernel32 -luser32 -lgdi32 -lwinspool -lshell32 -lole32 -loleaut32 \
             -luuid -lcomdlg32 -ladvapi32
XP_EXTRA  = -mno-ssse3 -mno-sse4.1 -mno-sse4.2 -mno-avx -D_WIN32_WINNT=0x0501 -DWINVER=0x0501
XP_LDFLAGS = -static -static-libgcc -static-libstdc++ -mconsole \
             -Wl,--major-subsystem-version=5 -Wl,--minor-subsystem-version=1
LL_CSRC_WIN = $(CORE) $(TOOLS) platform/platform_win32.c $(MAIN)

REMOTE_SRC = $(CORE) $(TOOLS) infer/infer_remote.c platform/platform_posix.c $(MAIN)

.PHONY: all test e2e verify-e2e win llama antix xp remote clean

all: anachron

anachron: $(NATIVE_SRC)
	$(CC) $(CFLAGS) $(NATIVE_SRC) -o $@

anachron-test: $(TEST_SRC)
	$(CC) $(CFLAGS) $(TEST_SRC) -o $@

test: anachron-test
	./anachron-test

e2e: anachron
	sh tests/run-e2e.sh

# Verify-on-write guardrail end-to-end (stub backend, deterministic): a bad write
# must be rejected and reverted; a good write kept.
verify-e2e: anachron
	sh tests/verify-e2e.sh

# XP-safe cross build: subsystem 5.01, static, API ceiling pinned to XP SP3.
win: anachron.exe
anachron.exe: $(WIN_SRC)
	$(WINCC) $(CFLAGS) -mno-ssse3 -mno-sse4.1 -mno-sse4.2 -mno-avx \
	    -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 \
	    -static -mconsole \
	    -Wl,--major-subsystem-version=5 -Wl,--minor-subsystem-version=1 \
	    $(WIN_SRC) -o $@

# Real-inference build: C core compiled as C99, the one C++ TU as C++, linked
# against the SSE2-only libllama/ggml from the Phase-0 spike. Run with e.g.:
#   ./anachron-llama --model spike-phase0/models/qwen2.5-coder-0.5b-instruct-q8_0.gguf
llama: anachron-llama
anachron-llama: $(LL_CSRC) infer/infer_llama.cpp
	@mkdir -p build-obj
	@for f in $(LL_CSRC); do \
	    echo "  CC  $$f"; \
	    $(CC) $(CFLAGS) -c $$f -o build-obj/`basename $$f .c`.o || exit 1; \
	done
	@echo "  CXX infer/infer_llama.cpp"
	@$(CXX) -std=c++17 -O2 $(INCLUDES) $(LLAMA_INC) -c infer/infer_llama.cpp -o build-obj/infer_llama.o
	@echo "  LD  anachron-llama"
	@$(CXX) build-obj/*.o $(LLAMA_LINK) -o anachron-llama

# antiX i686 cross build (-m32) as a RELOCATABLE bundle in dist/antix/: the binary
# plus its runtime .so's, found via an $ORIGIN rpath. Copy the whole dist/antix/
# folder to an antiX box (with a GGUF model) and run it in place.
antix: $(ANTIX_DIST)/anachron-llama-antix
$(ANTIX_DIST)/anachron-llama-antix: $(LL_CSRC) infer/infer_llama.cpp
	@mkdir -p build-obj-antix $(ANTIX_DIST)
	@for f in $(LL_CSRC); do \
	    echo "  CC32  $$f"; \
	    $(CC) -m32 $(CFLAGS) -c $$f -o build-obj-antix/`basename $$f .c`.o || exit 1; \
	done
	@echo "  CXX32 infer/infer_llama.cpp"
	@$(CXX) -m32 $(SSE2) -std=c++17 -O2 $(INCLUDES) $(LLAMA_INC) -c infer/infer_llama.cpp -o build-obj-antix/infer_llama.o
	@echo "  LD32  $(ANTIX_DIST)/anachron-llama-antix (\$$ORIGIN rpath)"
	@$(CXX) -m32 build-obj-antix/*.o $(LLAMA_LINK_ANTIX) -o $(ANTIX_DIST)/anachron-llama-antix
	@echo "  CP    runtime libs -> $(ANTIX_DIST)/"
	@for so in $(ANTIX_RUNLIBS); do cp -L $(LLAMA_LIBDIR_ANTIX)/$$so $(ANTIX_DIST)/$$so; done
	@echo "Bundle ready: $(ANTIX_DIST)/ (binary + $(words $(ANTIX_RUNLIBS)) libs). Needs a GGUF model + 32-bit libstdc++/libgcc on the target."

# Windows XP real-inference build: static PE32, subsystem 5.01, single self-contained
# .exe (no DLLs). Compiles the C core with mingw gcc-posix and the C++ TU with
# mingw g++-posix, links the static build-xp libs. Cannot run here (no wine) - verify
# with: i686-w64-mingw32-objdump -p dist/xp/anachron.exe
xp: $(XP_DIST)/anachron.exe
$(XP_DIST)/anachron.exe: $(LL_CSRC_WIN) infer/infer_llama.cpp
	@mkdir -p build-obj-xp $(XP_DIST)
	@for f in $(LL_CSRC_WIN); do \
	    echo "  XPCC  $$f"; \
	    $(XPCC) $(CFLAGS) $(XP_EXTRA) -c $$f -o build-obj-xp/`basename $$f .c`.o || exit 1; \
	done
	@echo "  XPCXX infer/infer_llama.cpp"
	@$(XPCXX) $(SSE2) $(XP_EXTRA) -std=c++17 -O2 $(INCLUDES) $(LLAMA_INC) -c infer/infer_llama.cpp -o build-obj-xp/infer_llama.o
	@echo "  XPLD  $(XP_DIST)/anachron.exe (static PE32, subsystem 5.01)"
	@$(XPCXX) build-obj-xp/*.o $(XP_STATIC) $(XP_SYSLIBS) $(XP_LDFLAGS) -o $(XP_DIST)/anachron.exe
	@echo "Built $(XP_DIST)/anachron.exe - copy it + a GGUF model to the XP box."

# Remote/GPU-offload build: thin HTTP client, NO local model/llama libs. Point it at
# a llama.cpp server with ANACHRON_REMOTE=host:port. Pure C, links like the stub build.
remote: anachron-remote
anachron-remote: $(REMOTE_SRC)
	$(CC) $(CFLAGS) $(REMOTE_SRC) -o $@

clean:
	rm -f anachron anachron-test anachron.exe anachron-llama anachron-remote
	rm -rf build-obj build-obj-antix build-obj-xp dist
