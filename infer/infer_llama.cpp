/* infer_llama — the real backend (Phase 2). Wraps libllama + ggml (built
 * SSE2-only in spike-phase0/) behind the C `infer_*` interface. This is the only
 * C++ translation unit in the project; everything else stays C99. The agent core
 * is unchanged — swapping stub -> llama is a link-time choice (Makefile BACKEND).
 *
 * Generation strategy (Phase 2, first cut): each call clears the KV cache and
 * re-decodes the full prompt. Simple and correct. It is O(prompt) per agent
 * iteration, which is fine on the dev host; on the M170 a future optimization is
 * KV-cache continuation (feed only new tokens). Decoding is greedy (temp 0) for
 * deterministic, reliable tool calls, optionally masked by the GBNF grammar, with a
 * gentle repeat penalty and a hard runaway-repetition stop so a tiny model can't
 * loop on one token forever. */
#include "infer.h"
#include "interrupt.h"

#include "llama.h"
#include "ggml-backend.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define ANCH_ISATTY_ERR() _isatty(_fileno(stderr))
#define ANCH_ISATTY_OUT() _isatty(_fileno(stdout))
#else
#include <unistd.h>
#define ANCH_ISATTY_ERR() isatty(fileno(stderr))
#define ANCH_ISATTY_OUT() isatty(fileno(stdout))
#endif

extern "C" double plat_time_sec(void);   /* monotonic wall clock (platform_*.c) */

struct infer_ctx {
    llama_model              *model;
    llama_context            *ctx;
    const llama_vocab        *vocab;
    std::vector<llama_token>  cached;  /* tokens currently held in the KV cache (seq 0), in order */
    int                       last_prompt_tokens;
    int                       last_completion_tokens;
    /* intra-token decode instrumentation (drives the per-token progress bar) */
    double                    decode_t0;        /* wall-clock start of the current decode */
    long                      decode_fires;     /* abort_callback invocations in current decode */
    int                       probe;            /* ANACHRON_PROBE_DECODE: print per-decode fire stats */
    double                    last_decode_sec;  /* measured time of the previous decode (the T estimate) */
    double                    bar_last_draw;    /* throttle: wall-clock of the last bar redraw */
    int                       bar_active;       /* per-token bar engaged for the CURRENT decode (gated) */
    int                       out_tty;          /* stdout is an interactive terminal (set once) */
    double                    ptok_min_sec;     /* show the per-token bar only above this decode time */
};

static void quiet_log(enum ggml_log_level level, const char *text, void *ud) {
    (void)ud;
    /* During a user Ctrl+C we deliberately abort llama_decode, which makes ggml/llama
     * log "failed to compute graph / failed to decode" at ERROR level. That's expected,
     * not a fault - suppress it so an interrupt looks clean instead of like a crash. */
    if (interrupt_pending()) return;
    if (level >= GGML_LOG_LEVEL_ERROR) fputs(text, stderr);
}

/* --- cold-start progress bars (stderr, only when stderr is a terminal) -----------
 * The slow, silent part of a cold turn is model load + the first prompt prefill, and
 * both have a KNOWN size, so we can show a real bar (ASCII, XP-console safe) with a
 * rough ETA extrapolated from the steady rate. Generation has no known length, so it
 * just streams (no bar). When stderr is not a TTY (pipes, the test harness) we draw
 * nothing - and because we still own the load callback, that also suppresses llama's
 * own loader dots, keeping piped output clean. */
static bool stderr_is_tty(void) { return ANCH_ISATTY_ERR() != 0; }

static void draw_bar(const char *label, float frac, double eta, long cur, long total) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    const int W = 16;
    int fill = (int)(frac * W + 0.5f);
    char bar[W + 3];
    int k = 0;
    bar[k++] = '[';
    for (int i = 0; i < W; i++) bar[k++] = (i < fill) ? '#' : '.';
    bar[k++] = ']';
    bar[k] = '\0';
    char tail[48];
    if (total >= 0) snprintf(tail, sizeof tail, "%ld/%ld tokens", cur, total);
    else            snprintf(tail, sizeof tail, "%3d%%", (int)(frac * 100 + 0.5f));
    char etabuf[24];
    etabuf[0] = '\0';
    if (eta >= 0) snprintf(etabuf, sizeof etabuf, "  ~%.0fs", eta < 1.0 ? 1.0 : eta);
    fprintf(stderr, "\r%-14s %s %s%s   ", label, bar, tail, etabuf);
    fflush(stderr);
}

static void clear_bar(void) {
    fprintf(stderr, "\r%60s\r", "");
    fflush(stderr);
}

/* --- per-token decode bar (very slow hardware only) ------------------------------
 * On sub-~1-tok/s hardware a single token's forward pass is seconds of dead air. We
 * fill a small bar over that one decode (its work is bounded, so a % is honest),
 * driven by abort_callback ticks and a time estimate from the previous decode. It
 * lives just after the streamed text on the same terminal line, drawn/erased with
 * ANSI save-restore-cursor (DECSC/DECRC) + erase-to-EOL, so it never disturbs the
 * streamed output. ANSI is POSIX/antiX only - the XP console gets no per-token bar
 * (it has no cursor save/restore), which is fine: the load + prefill bars remain. */
#ifndef _WIN32
static void ptok_bar_begin(void) { fputs("\0337", stdout); fflush(stdout); }  /* DECSC: save cursor */
static void ptok_bar_end(void)   { fputs("\0338\033[K", stdout); fflush(stdout); } /* DECRC + erase bar */

static void ptok_bar_draw(double frac, double rem_sec) {
    if (frac < 0) frac = 0;
    if (frac > 0.99) frac = 0.99;   /* never show a full/"done" bar before the token lands */
    const int W = 14;
    int fill = (int)(frac * W + 0.5f);
    char bar[W + 1];
    for (int i = 0; i < W; i++) bar[i] = (i < fill) ? '#' : '.';
    bar[W] = '\0';
    if (rem_sec < 1.0) rem_sec = 1.0;
    /* DECRC back to end-of-text, erase any old bar, draw "  [####....] ~Ns" */
    printf("\0338\033[K  [%s] ~%.0fs", bar, rem_sec);
    fflush(stdout);
}
#else
static void ptok_bar_begin(void) {}
static void ptok_bar_end(void)   {}
static void ptok_bar_draw(double, double) {}
#endif

/* Engage the per-token bar for the next decode only when stdout is a terminal and the
 * previous decode was slow enough that a filling bar helps rather than strobes. */
static int ptok_bar_enabled(const infer_ctx *c) {
#ifdef _WIN32
    (void)c; return 0;
#else
    return c->out_tty && c->last_decode_sec > c->ptok_min_sec;
#endif
}

struct load_state { double t0; bool tty; };

static bool load_progress_cb(float progress, void *ud) {
    load_state *ls = (load_state *)ud;
    if (!ls || !ls->tty) return true;          /* not a tty: draw nothing, suppress dots */
    double now = plat_time_sec();
    if (ls->t0 < 0) ls->t0 = now;
    double el = now - ls->t0;
    double eta = (progress > 0.02f) ? el * (1.0 - progress) / progress : -1.0;
    draw_bar("loading model", progress, eta, -1, -1);
    if (progress >= 0.999f) clear_bar();
    return true;                               /* true => keep loading */
}

/* abort_callback: ggml invokes this periodically DURING llama_decode (it exists so a
 * long compute can be cancelled). We piggy-back on it to (a) measure decode granularity
 * [probe], and later to drive the per-token bar and make Ctrl+C responsive mid-decode.
 * Returning true aborts the compute; false continues. */
static bool decode_abort_cb(void *data) {
    infer_ctx *c = (infer_ctx *)data;
    if (!c) return false;
    c->decode_fires++;
    /* Ctrl+C felt mid-decode: abort this forward pass now instead of waiting up to a
     * full (multi-second, on slow hardware) token. Clean the bar first so the cursor
     * is restored, then signal the abort. */
    if (interrupt_pending()) {
        if (c->bar_active) { ptok_bar_end(); c->bar_active = 0; }
        return true;
    }
    if (c->bar_active) {
        double now = plat_time_sec();
        if (now - c->bar_last_draw >= 0.15) {        /* throttle redraws to ~150ms */
            c->bar_last_draw = now;
            double el = now - c->decode_t0;
            double frac = (c->last_decode_sec > 0) ? el / c->last_decode_sec : 0.0;
            ptok_bar_draw(frac, c->last_decode_sec - el);
        }
    }
    return false;   /* false => keep computing (Ctrl+C handling added in step 3) */
}

static int env_threads(void) {
    const char *e = getenv("ANACHRON_THREADS");
    int n = e ? atoi(e) : 0;
    if (n <= 0) n = 4; /* dev-host default; the M170 run should pass ANACHRON_THREADS=1 */
    return n;
}

extern "C" infer_ctx *infer_init(const char *gguf_path, int n_ctx) {
    if (!gguf_path) {
        fprintf(stderr, "infer_llama: no --model given\n");
        return nullptr;
    }
    llama_log_set(quiet_log, nullptr);
    ggml_backend_load_all();

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0; /* CPU only — there is no GPU on the target */

    /* Real model-load progress bar (the dominant cold-start cost). Owning the
     * callback also suppresses llama's default loader dots when piped. The callback
     * runs synchronously inside the load call, so a local load_state is fine. */
    load_state ls;
    ls.t0 = -1.0;
    ls.tty = stderr_is_tty();
    mparams.progress_callback = load_progress_cb;
    mparams.progress_callback_user_data = &ls;

    llama_model *model = llama_model_load_from_file(gguf_path, mparams);
    if (ls.tty) clear_bar();   /* clear the line even if the last tick was < 100% */
    if (!model) {
        fprintf(stderr, "infer_llama: failed to load model %s\n", gguf_path);
        return nullptr;
    }

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = (uint32_t)(n_ctx > 0 ? n_ctx : 2048);
    cparams.n_batch = cparams.n_ctx;
    cparams.n_threads = env_threads();
    cparams.n_threads_batch = cparams.n_threads;

    llama_context *ctx = llama_init_from_model(model, cparams);
    if (!ctx) {
        fprintf(stderr, "infer_llama: failed to create context\n");
        llama_model_free(model);
        return nullptr;
    }

    infer_ctx *c = new infer_ctx;
    c->model = model;
    c->ctx = ctx;
    c->vocab = llama_model_get_vocab(model);
    c->last_prompt_tokens = 0;
    c->last_completion_tokens = 0;
    c->decode_t0 = 0.0;
    c->decode_fires = 0;
    c->probe = (getenv("ANACHRON_PROBE_DECODE") != nullptr);
    c->last_decode_sec = 0.0;
    c->bar_last_draw = 0.0;
    c->bar_active = 0;
    c->out_tty = (ANCH_ISATTY_OUT() != 0);
    {   /* per-token bar engages above this decode time; default 1.5s (~< 0.66 tok/s) */
        const char *e = getenv("ANACHRON_PTOK_MIN_SEC");
        c->ptok_min_sec = (e && atof(e) > 0.0) ? atof(e) : 1.5;
    }
    llama_set_abort_callback(c->ctx, decode_abort_cb, c);
    return c;
}

extern "C" int infer_generate(infer_ctx *c, const char *prompt, const char *grammar,
                              void (*on_token)(const char *piece, void *ud), void *ud) {
    llama_memory_t mem = llama_get_memory(c->ctx);
    const int n_ctx = (int)llama_n_ctx(c->ctx);

    /* Reset usage up-front so a failed generate never reports a prior call's counts. */
    c->last_prompt_tokens = 0;
    c->last_completion_tokens = 0;

    /* Tokenize the full prompt (system + few-shot + whole conversation so far). */
    const int len = (int)strlen(prompt);
    int n_tok = -llama_tokenize(c->vocab, prompt, len, nullptr, 0, /*add_special*/ true, /*parse_special*/ true);
    if (n_tok <= 0) {
        fprintf(stderr, "infer_llama: tokenize failed\n");
        return -1;
    }
    std::vector<llama_token> toks(n_tok);
    if (llama_tokenize(c->vocab, prompt, len, toks.data(), n_tok, true, true) < 0) {
        fprintf(stderr, "infer_llama: tokenize failed (second pass)\n");
        return -1;
    }
    c->last_prompt_tokens = n_tok;

    /* --- KV-cache reuse (the speedup) ---
     * Find the longest prefix shared with what is already in the cache, drop the
     * diverged tail from the cache, and decode ONLY the new tail. The large,
     * unchanging system+few-shot+history prefix is decoded once and reused on
     * every later turn instead of being re-processed from scratch. */
    int n_keep = 0;
    while (n_keep < (int)c->cached.size() && n_keep < n_tok &&
           c->cached[n_keep] == toks[n_keep]) {
        n_keep++;
    }
    /* Must decode at least the final prompt token to get fresh logits to sample. */
    if (n_keep >= n_tok) n_keep = n_tok - 1;

    if (n_tok > n_ctx) {
        fprintf(stderr, "infer_llama: prompt (%d tokens) exceeds context (%d)\n", n_tok, n_ctx);
        return -1;
    }

    llama_memory_seq_rm(mem, 0, (llama_pos)n_keep, -1); /* drop cached positions >= n_keep */
    c->cached.resize((size_t)n_keep);

    /* Decode the new prefix tail [n_keep, n_tok) in chunks, checking for Ctrl+C
     * between them, so an interrupt during a long prompt decode (e.g. a big first
     * turn on slow hardware) takes effect promptly. Cached tokens are pushed per
     * chunk, so an aborted decode leaves c->cached consistent with the KV cache. */
    {
        const int CHUNK = 32;   /* small enough that Ctrl+C during a slow prompt
                                   decode is felt within a few seconds, big enough
                                   that per-batch overhead stays negligible */
        /* Prefill progress bar: only when there's a substantial prefix to process
         * (the slow first turn). Incremental turns reuse the KV cache (large n_keep,
         * few new tokens), so the bar stays out of the way. */
        const int n_new_total = n_tok - n_keep;
        const bool show_pf = stderr_is_tty() && n_new_total > 64;
        const double pf_t0 = show_pf ? plat_time_sec() : 0.0;
        for (int off = n_keep; off < n_tok; off += CHUNK) {
            if (interrupt_pending()) { if (show_pf) clear_bar(); return 0; }   /* aborted mid-prompt */
            int n_new = (n_tok - off < CHUNK) ? (n_tok - off) : CHUNK;
            llama_batch batch = llama_batch_get_one(toks.data() + off, n_new);
            if (llama_decode(c->ctx, batch) != 0) {
                if (show_pf) clear_bar();
                fprintf(stderr, "infer_llama: decode (prompt) failed\n");
                return -1;
            }
            for (int i = off; i < off + n_new; i++) c->cached.push_back(toks[i]);
            if (show_pf) {
                long done = (long)(off + n_new - n_keep);
                double frac = (double)done / (double)n_new_total;
                double el = plat_time_sec() - pf_t0;
                double eta = (frac > 0.02) ? el * (1.0 - frac) / frac : -1.0;
                draw_bar("reading prompt", (float)frac, eta, done, (long)n_new_total);
            }
        }
        if (show_pf) clear_bar();
    }

    /* Greedy + optional LAZY grammar. Lazy so the model can just talk: the grammar
     * stays dormant until it emits the "<tool_call>" trigger, then constrains the
     * JSON. A fresh sampler per call resets the trigger for this turn. */
    llama_sampler *smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (grammar && *grammar) {
        const char *patterns[] = { "[\\s\\S]*?(<tool_call>)" };
        llama_sampler *g = llama_sampler_init_grammar_lazy_patterns(
            c->vocab, grammar, "root", patterns, 1, nullptr, 0);
        if (g) llama_sampler_chain_add(smpl, g);
        else fprintf(stderr, "infer_llama: grammar rejected; decoding unconstrained\n");
    }
    /* A gentle repeat penalty (presence-only, 1.1 over the last 64 tokens) nudges the
     * model out of loops without distorting code indentation or tool-call JSON. The
     * hard backstop is the runaway-repetition guard in the loop below. Penalties run
     * before greedy so the argmax is taken over the adjusted logits. */
    llama_sampler_chain_add(smpl, llama_sampler_init_penalties(64, 1.1f, 0.0f, 0.0f));
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

    /* Generation: sample -> emit -> decode the sampled token back in. */
    int rc = 0;
    llama_token prev_id = -1;
    int same_run = 0;
    const int MAX_SAME_RUN = 40;  /* consecutive identical tokens => runaway */
    for (int gen = 0; gen < n_ctx; gen++) {
        if (interrupt_pending()) break;   /* Ctrl+C: stop generating, keep the session */
        llama_token id = llama_sampler_sample(smpl, c->ctx, -1);
        if (llama_vocab_is_eog(c->vocab, id)) break;

        /* Runaway-repetition guard: a tiny model with greedy decoding can fall into
         * emitting one token forever (e.g. spaces while re-typing a big file). Stop
         * cleanly instead of grinding all the way to the context cap. */
        if (id == prev_id) same_run++;
        else { same_run = 1; prev_id = id; }
        if (same_run >= MAX_SAME_RUN) {
            fprintf(stderr, "infer_llama: stopped a runaway repetition (same token x%d)\n",
                    same_run);
            break;
        }

        char buf[256];
        int np = llama_token_to_piece(c->vocab, id, buf, sizeof buf, 0, /*special*/ true);
        if (np < 0) { rc = -1; break; }
        if (on_token) on_token(std::string(buf, (size_t)np).c_str(), ud);
        c->last_completion_tokens++;

        c->cached.push_back(id);

        if (llama_memory_seq_pos_max(mem, 0) + 1 >= n_ctx) break; /* out of room */
        llama_token cur = id;
        llama_batch b = llama_batch_get_one(&cur, 1);
        /* The single-token forward pass: the ~5s gap on slow hardware. Measure how
         * often abort_callback fires inside it (step 1: validates the per-token bar). */
        c->decode_fires = 0;
        c->decode_t0 = plat_time_sec();
        c->bar_active = ptok_bar_enabled(c);
        c->bar_last_draw = 0.0;                  /* draw on the first throttled tick */
        if (c->bar_active) ptok_bar_begin();     /* save cursor at end of streamed text */
        int dec = llama_decode(c->ctx, b);
        double dsec = plat_time_sec() - c->decode_t0;
        if (c->bar_active) { ptok_bar_end(); c->bar_active = 0; }
        /* EMA estimate for the NEXT token's bar: robust to a one-off slow/fast decode
         * (e.g. host scheduling jitter) while still tracking a real rate change. */
        c->last_decode_sec = (c->last_decode_sec > 0.0)
            ? 0.6 * c->last_decode_sec + 0.4 * dsec
            : dsec;
        if (c->probe) {
            fprintf(stderr, "[probe] decode #%d: %ld fires in %.0f ms (%.1f ms/fire)\n",
                    c->last_completion_tokens, c->decode_fires, dsec * 1000.0,
                    c->decode_fires ? (dsec * 1000.0) / (double)c->decode_fires : 0.0);
        }
        if (dec != 0) {
            if (interrupt_pending()) {
                /* Aborted mid-decode by Ctrl+C: the just-emitted token is in c->cached
                 * but its KV entry may be partial, so the mirror and the KV cache can
                 * disagree. Drop the mirror; the next turn then re-prefills from scratch
                 * (one slow prompt) rather than reusing a possibly-corrupt prefix. */
                c->cached.clear();
                break;                        /* clean stop, keep the partial output */
            }
            rc = -1; break;                   /* genuine decode failure */
        }
    }

    llama_sampler_free(smpl);
    return rc;
}

extern "C" void infer_last_usage(const infer_ctx *c, int *prompt_tokens, int *completion_tokens) {
    if (prompt_tokens) *prompt_tokens = c ? c->last_prompt_tokens : 0;
    if (completion_tokens) *completion_tokens = c ? c->last_completion_tokens : 0;
}

extern "C" void infer_free(infer_ctx *c) {
    if (!c) return;
    llama_free(c->ctx);
    llama_model_free(c->model);
    delete c;
}
