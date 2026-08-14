/* SPDX-License-Identifier: Apache-2.0 */
/* k3_engine.c - see k3_engine.h for why this exists and what gates it. */
#define _POSIX_C_SOURCE 200809L

#include "k3_engine.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "k3.h"
#include "k3_bind.h"
#include "k3_cache.h"
#include "k3_trunk.h"
#include "k3_tok.h"
#include "k3_cfg.h"

#include "../util/platform.h"

struct K3Engine {
    K3Cfg    cfg;
    int      fa[128];
    K3St     st;
    K3Cache  cache;
    K3Trunk  trunk;
    int      streamed;         /* trunk_dir given */
    K3LayerBind *lay;
    K3ModelBind  mb;
    int      n_bound;
    Tok      tok;
    int      have_tok;
    int      eos[8];
    int      n_eos;
    int      max_gen_default;
    int      verbose;
    unsigned rng;              /* xorshift state for sampling */
};

static void set_err(char *err, size_t errsz, const char *fmt, ...)
{
    if (!err || !errsz) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

/* ---- open / close ---------------------------------------------------------- */

int k3_engine_open(K3Engine **out, const K3EngineOpts *o, char *err, size_t errsz)
{
    *out = NULL;
    K3Engine *e = (K3Engine *)calloc(1, sizeof *e);
    if (!e) { set_err(err, errsz, "OOM"); return -1; }
    e->max_gen_default = o->max_gen_default > 0 ? o->max_gen_default : 512;
    e->verbose = o->verbose;
    e->rng = 0x9e3779b9u;

    /* config: prefer the checkpoint's own config.json */
    char guess[4096];
    const char *cfg_path = o->cfg_path;
    if (!cfg_path) {
        snprintf(guess, sizeof guess, "%s/config.json", o->model_dir);
        FILE *probe = fopen(guess, "rb");
        if (probe) { fclose(probe); cfg_path = guess; }
    }
    if (!cfg_path) {
        set_err(err, errsz, "no config.json under %s and none given", o->model_dir);
        free(e);
        return -1;
    }
    if (!k3_cfg_load_file(&e->cfg, e->fa, 128, cfg_path)) {
        set_err(err, errsz, "model config %s could not be read with confidence", cfg_path);
        free(e);
        return -1;
    }

    if (k3_st_open(&e->st, o->model_dir) != 0) {
        set_err(err, errsz, "cannot index safetensors under %s", o->model_dir);
        free(e);
        return -1;
    }

    const int NL = (o->n_layers > 0 && o->n_layers < e->cfg.n_layers)
                   ? o->n_layers : e->cfg.n_layers;
    e->lay = (K3LayerBind *)calloc((size_t)NL, sizeof(K3LayerBind));
    if (!e->lay) { set_err(err, errsz, "OOM"); goto fail_st; }

    if (o->trunk_dir) {
        if (k3_trunk_open(&e->trunk, o->trunk_dir, &e->cfg,
                          (int64_t)(o->trunk_gb * 1e9)) != 0) {
            set_err(err, errsz, "cannot open packed trunk %s", o->trunk_dir);
            goto fail_lay;
        }
        if (e->trunk.n_layers < NL) {
            set_err(err, errsz, "packed trunk has %d layers, need %d",
                    e->trunk.n_layers, NL);
            k3_trunk_close(&e->trunk);
            goto fail_lay;
        }
        e->streamed = 1;
        e->n_bound = NL;
    } else {
        for (int L = 0; L < NL; L++) {
            if (k3_bind_layer(&e->st, &e->cfg, L, &e->lay[L]) != 0) {
                set_err(err, errsz, "bind failed at layer %d", L);
                goto fail_bound;
            }
            e->n_bound = L + 1;
        }
    }

    if (k3_bind_model(&e->st, &e->cfg, 1, &e->mb) != 0) {
        set_err(err, errsz, "cannot bind embedding/lm_head");
        goto fail_bound;
    }

    {
        double cgb = o->cache_gb > 0 ? o->cache_gb : 0.5;
        if (k3_cache_init(&e->cache, &e->st, &e->cfg, (int64_t)(cgb * 1e9)) != 0) {
            set_err(err, errsz, "expert cache init failed (budget %.2f GB)", cgb);
            k3_bind_model_free(&e->mb);
            goto fail_bound;
        }
    }

    if (o->tok_dir) {
        k3_tok_load(&e->tok, o->tok_dir);   /* exits on malformed input by design */
        e->have_tok = 1;
        static const char *eos_names[] = { "[EOS]", "<|im_end|>", "<|endoftext|>", NULL };
        for (int i = 0; eos_names[i]; i++) {
            int id = tok_id_of(&e->tok, eos_names[i]);
            if (id >= 0 && e->n_eos < 8) e->eos[e->n_eos++] = id;
        }
    }

    *out = e;
    return 0;

fail_bound:
    if (e->streamed) k3_trunk_close(&e->trunk);
    for (int L = 0; L < e->n_bound; L++) k3_bind_free(&e->lay[L]);
fail_lay:
    free(e->lay);
fail_st:
    k3_st_close(&e->st);
    free(e);
    return -1;
}

void k3_engine_close(K3Engine *e)
{
    if (!e) return;
    k3_cache_free(&e->cache);
    k3_bind_model_free(&e->mb);
    if (e->streamed) k3_trunk_close(&e->trunk);
    else for (int L = 0; L < e->n_bound; L++) k3_bind_free(&e->lay[L]);
    free(e->lay);
    k3_st_close(&e->st);
    free(e);
}

/* ---- forward ----------------------------------------------------------------
 * Semantics mirror the CLI's forward() (src/cli/k3_run.c): embed rows, run
 * k3_decoder_layer_inc over every bound layer (with dense per-MLA KV slices),
 * apply the model-level attn-res aggregator, final RMSNorm, lm_head. Gated
 * against the CLI's tokens by test_k3_engine. */
typedef struct {
    float *h, *br, *ks, *sc, *lg;   /* hidden, block-res, kda state, scratch, logits */
    float *kvc, *ropec;
    int   *mla_slot;
    int    n_mla, kv_cap, cached;
    int    Tmax, maxb;
    size_t kper;
} GenBufs;

static int bufs_alloc(K3Engine *e, GenBufs *g, int Tmax)
{
    const K3Cfg *c = &e->cfg;
    memset(g, 0, sizeof *g);
    g->Tmax = Tmax;
    g->maxb = c->n_layers / c->attn_res_block + 2;
    const int P = c->kda_heads * c->kda_head_dim;
    g->kper = (size_t)P * c->kda_head_dim + (size_t)3 * P * (c->conv_k - 1);
    const int E = c->hidden;

    size_t sc_need = k3_layer_scratch(c, Tmax);
    size_t ic = k3_mla_scratch_cached(c, Tmax, Tmax, 1);
    if (ic > sc_need) sc_need = ic;
    const size_t need_scratch = (size_t)(g->maxb + 2) * (size_t)E;
    if (sc_need < need_scratch) sc_need = need_scratch;

    g->h  = (float *)malloc((size_t)Tmax * E * sizeof(float));
    g->br = (float *)malloc((size_t)Tmax * g->maxb * E * sizeof(float));
    g->ks = (float *)calloc(g->kper * (size_t)e->n_bound, sizeof(float));
    g->sc = (float *)malloc(sc_need * sizeof(float));
    g->lg = (float *)malloc((size_t)c->vocab * sizeof(float));
    g->mla_slot = (int *)malloc((size_t)e->n_bound * sizeof(int));
    if (!g->h || !g->br || !g->ks || !g->sc || !g->lg || !g->mla_slot) return -1;

    g->n_mla = 0;
    for (int L = 0; L < e->n_bound; L++)
        g->mla_slot[L] = k3_is_mla(c, L) ? g->n_mla++ : -1;
    g->kv_cap = Tmax;
    if (g->n_mla > 0) {
        const size_t kvper = (size_t)g->kv_cap * c->n_heads * (c->qk_nope + c->v_head);
        const size_t rpper = (size_t)g->kv_cap * c->qk_rope;
        g->kvc   = (float *)calloc(kvper * (size_t)g->n_mla, sizeof(float));
        g->ropec = (float *)calloc(rpper * (size_t)g->n_mla, sizeof(float));
        if (!g->kvc || !g->ropec) return -1;
    }
    g->cached = 0;
    return 0;
}

static void bufs_free(GenBufs *g)
{
    free(g->h); free(g->br); free(g->ks); free(g->sc); free(g->lg);
    free(g->kvc); free(g->ropec); free(g->mla_slot);
    memset(g, 0, sizeof *g);
}

static int engine_forward(K3Engine *e, GenBufs *g, const int *ids, int T)
{
    const K3Cfg *c = &e->cfg;
    const int E = c->hidden;

    for (int t = 0; t < T; t++)
        k3_embed_row(g->h + (size_t)t * E, e->mb.embed, e->mb.wdt, ids[t], E);

    memset(g->br, 0, (size_t)T * g->maxb * E * sizeof(float));
    int nb = 0;
    for (int L = 0; L < e->n_bound; L++) {
        if (e->streamed) {
            if (k3_trunk_bind(&e->trunk, c, L, &e->lay[L]) != 0) return -1;
            k3_trunk_prefetch(&e->trunk, L + 1);
        }
        if (e->lay[L].lay.moe) {
            e->lay[L].moe.src = &e->cache.src;
            e->lay[L].moe.layer = L;
        }
        if (g->kvc && g->mla_slot[L] >= 0) {
            const size_t kvper = (size_t)g->kv_cap * c->n_heads * (c->qk_nope + c->v_head);
            const size_t rpper = (size_t)g->kv_cap * c->qk_rope;
            const int mi = g->mla_slot[L];
            k3_decoder_layer_inc(g->h, g->br, &nb, &e->lay[L].lay, c, L, T,
                                 g->ks + g->kper * (size_t)L, g->sc,
                                 g->kvc + kvper * (size_t)mi,
                                 g->ropec + rpper * (size_t)mi,
                                 g->cached, g->kv_cap);
        } else {
            k3_decoder_layer_inc(g->h, g->br, &nb, &e->lay[L].lay, c, L, T,
                                 g->ks + g->kper * (size_t)L, g->sc,
                                 NULL, NULL, 0, 0);
        }
    }

    if (e->mb.out_res_norm && e->mb.out_res_proj) {
        float *fold = g->sc;
        float *src  = fold + E;
        for (int i = 0; i < E; i++)
            fold[i] = e->mb.out_res_norm[i] * e->mb.out_res_proj[i];
        for (int t = 0; t < T; t++) {
            for (int b = 0; b < nb; b++)
                memcpy(src + (size_t)b * E, g->br + ((size_t)t * g->maxb + b) * E,
                       (size_t)E * sizeof(float));
            memcpy(src + (size_t)nb * E, g->h + (size_t)t * E, (size_t)E * sizeof(float));
            k3_attn_res(g->h + (size_t)t * E, src, fold, nb + 1, E, c->rms_eps);
        }
    }

    float *nrm = g->sc;
    k3_rmsnorm(nrm, g->h + (size_t)(T - 1) * E, e->mb.norm, E, c->rms_eps);
    k3_mmw(g->lg, nrm, e->mb.lm_head, e->mb.wdt, E, c->vocab);
    return 0;
}

/* ---- sampling ---------------------------------------------------------------
 * Greedy when temperature <= 0. Otherwise softmax over logits/T restricted to
 * the top-p nucleus. Deterministic given the seed. */
static unsigned xs32(unsigned *s)
{
    unsigned x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

static int sample_logits(const float *lg, int vocab, float temp, float top_p,
                         unsigned *rng)
{
    if (temp <= 0.0f) {
        int best = 0;
        for (int i = 1; i < vocab; i++) if (lg[i] > lg[best]) best = i;
        return best;
    }
    /* top-k prefilter keeps the nucleus sort cheap: k=256 covers any practical
     * top_p mass on a peaked LM distribution. */
    enum { K = 256 };
    int   idx[K];
    float val[K];
    int n = 0;
    for (int i = 0; i < vocab; i++) {
        float v = lg[i];
        if (n < K) {
            int j = n++;
            while (j > 0 && val[j - 1] < v) {
                val[j] = val[j - 1]; idx[j] = idx[j - 1]; j--;
            }
            val[j] = v; idx[j] = i;
        } else if (v > val[K - 1]) {
            int j = K - 1;
            while (j > 0 && val[j - 1] < v) {
                val[j] = val[j - 1]; idx[j] = idx[j - 1]; j--;
            }
            val[j] = v; idx[j] = i;
        }
    }
    double p[K], sum = 0.0;
    for (int i = 0; i < n; i++) {
        p[i] = exp((double)(val[i] - val[0]) / (double)temp);
        sum += p[i];
    }
    int keep = n;
    if (top_p > 0.0f && top_p < 1.0f) {
        double acc = 0.0;
        for (int i = 0; i < n; i++) {
            acc += p[i] / sum;
            if (acc >= (double)top_p) { keep = i + 1; break; }
        }
    }
    double ksum = 0.0;
    for (int i = 0; i < keep; i++) ksum += p[i];
    double r = (double)(xs32(rng) & 0xffffff) / (double)0x1000000 * ksum;
    double acc = 0.0;
    for (int i = 0; i < keep; i++) {
        acc += p[i];
        if (r <= acc) return idx[i];
    }
    return idx[keep - 1];
}

/* ---- generation -------------------------------------------------------------- */

static int is_eos(const K3Engine *e, int id)
{
    for (int i = 0; i < e->n_eos; i++) if (e->eos[i] == id) return 1;
    return 0;
}

/* Find the earliest stop-string occurrence in text[from..]; returns byte offset
 * of the match start, or -1. `from` backs up by the longest stop len so a stop
 * split across two decode chunks is still found. */
static int find_stop(const char *text, size_t len, const char **stop, size_t scan_from)
{
    if (!stop) return -1;
    for (int i = 0; stop[i]; i++) {
        size_t sl = strlen(stop[i]);
        if (!sl || sl > len) continue;
        size_t start = scan_from > sl ? scan_from - sl : 0;
        const char *hit = strstr(text + start, stop[i]);
        if (hit) return (int)(hit - text);
    }
    return -1;
}

int k3_engine_generate_ids(K3Engine *e, const int *prompt, int np,
                           const K3SampleOpts *sopt, K3GenOut *out,
                           char *err, size_t errsz)
{
    memset(out, 0, sizeof *out);
    K3SampleOpts s;
    memset(&s, 0, sizeof s);
    if (sopt) s = *sopt;
    int gen = s.max_tokens > 0 ? s.max_tokens : e->max_gen_default;
    if (gen > K3_MAX_GEN) gen = K3_MAX_GEN;
    if (np <= 0)               { set_err(err, errsz, "empty prompt"); return -1; }
    if (np > K3_MAX_PROMPT)    { set_err(err, errsz, "prompt of %d ids exceeds %d",
                                         np, K3_MAX_PROMPT); return -1; }
    for (int i = 0; i < np; i++)
        if (prompt[i] < 0 || prompt[i] >= e->cfg.vocab) {
            set_err(err, errsz, "token id %d outside vocab %d", prompt[i], e->cfg.vocab);
            return -1;
        }

    unsigned rng = s.seed ? s.seed : (unsigned)time(NULL) ^ (unsigned)pf_getpid();

    GenBufs g;
    const int Tmax = np + gen + 1;
    if (bufs_alloc(e, &g, Tmax) != 0) {
        bufs_free(&g);
        set_err(err, errsz, "OOM allocating generation buffers (T=%d)", Tmax);
        return -1;
    }

    int *seq = (int *)malloc((size_t)(np + gen + 8) * sizeof(int));
    int *gen_ids = (int *)malloc((size_t)(gen + 8) * sizeof(int));
    if (!seq || !gen_ids) {
        free(seq); free(gen_ids); bufs_free(&g);
        set_err(err, errsz, "OOM");
        return -1;
    }
    memcpy(seq, prompt, (size_t)np * sizeof(int));
    int T = np, nout = 0, stopped_by = 0;

    /* streaming text state */
    char *text = NULL;
    size_t text_len = 0, text_cap = 0, emitted = 0;

    double t0 = mono_seconds();
    long drops0 = k3_expert_drops;

    while (nout < gen) {
        int frc;
        if (g.cached == 0) {   /* prefill: feed the whole prompt once */
            frc = engine_forward(e, &g, seq, T);
            if (frc == 0) g.cached = T;
        } else {
            frc = engine_forward(e, &g, seq + g.cached, T - g.cached);
            if (frc == 0) g.cached = T;
        }
        if (frc != 0) {
            free(text); free(seq); free(gen_ids); bufs_free(&g);
            set_err(err, errsz, "forward pass failed at step %d", nout);
            return -1;
        }
        int nxt = sample_logits(g.lg, e->cfg.vocab, s.temperature, s.top_p, &rng);
        if (is_eos(e, nxt)) { stopped_by = 3; break; }
        seq[T++] = nxt;
        gen_ids[nout++] = nxt;

        if (e->have_tok) {
            /* tiktoken decode is concatenative: re-decode all generated ids and
             * emit the delta. Exact, and O(n^2) is fine at n <= 4096. */
            size_t need = (size_t)nout * 8 + 16;
            if (need > text_cap) {
                text_cap = need * 2;
                char *nt = (char *)realloc(text, text_cap);
                if (!nt) { stopped_by = 0; break; }
                text = nt;
            }
            int m = tok_decode(&e->tok, gen_ids, nout, text, (int)text_cap - 1);
            if (m < 0) m = 0;
            text[m] = 0;
            size_t prev_len = text_len;
            text_len = (size_t)m;

            int cut = find_stop(text, text_len, s.stop, prev_len);
            if (cut >= 0) {
                text[cut] = 0;
                text_len = (size_t)cut;
                stopped_by = 1;
            }
            if (s.on_text && text_len > emitted) {
                char save = text[text_len];
                if (s.on_text(text + emitted, s.userdata)) {
                    text[text_len] = save;
                    stopped_by = 2;
                    emitted = text_len;
                    break;
                }
                text[text_len] = save;
                emitted = text_len;
            }
            if (stopped_by == 1) break;
        }
        if (T >= Tmax - 1) break;
    }

    out->seconds = mono_seconds() - t0;
    out->ids = gen_ids;
    out->n_ids = nout;
    out->stopped_by = stopped_by;
    if (e->have_tok) {
        if (!text) { text = (char *)malloc(1); if (text) text[0] = 0; }
        out->text = text;
    } else {
        free(text);
    }
    free(seq);

    if (k3_expert_drops != drops0) {
        set_err(err, errsz, "%ld routed expert load(s) failed; output is corrupt",
                k3_expert_drops - drops0);
        bufs_free(&g);
        return -1;
    }
    bufs_free(&g);
    return 0;
}

int k3_engine_generate(K3Engine *e, const char *prompt, const K3SampleOpts *s,
                       K3GenOut *out, char *err, size_t errsz)
{
    memset(out, 0, sizeof *out);
    if (!e->have_tok) {
        set_err(err, errsz, "no tokenizer loaded: text generation needs tok_dir");
        return -1;
    }
    int *ids = (int *)malloc((size_t)K3_MAX_PROMPT * sizeof(int));
    if (!ids) { set_err(err, errsz, "OOM"); return -1; }
    int np = tok_encode(&e->tok, prompt, (int)strlen(prompt), ids, K3_MAX_PROMPT);
    if (np <= 0) {
        free(ids);
        set_err(err, errsz, "prompt tokenized to %d ids", np);
        return -1;
    }
    int rc = k3_engine_generate_ids(e, ids, np, s, out, err, errsz);
    free(ids);
    return rc;
}

int k3_engine_count_tokens(K3Engine *e, const char *text)
{
    if (!text) return 0;
    if (!e->have_tok) return (int)(strlen(text) / 4) + 1;
    static int *scratch = NULL;
    if (!scratch) scratch = (int *)malloc((size_t)K3_MAX_PROMPT * sizeof(int));
    if (!scratch) return (int)(strlen(text) / 4) + 1;
    int n = tok_encode(&e->tok, text, (int)strlen(text), scratch, K3_MAX_PROMPT);
    return n > 0 ? n : (int)(strlen(text) / 4) + 1;
}

int k3_engine_context_window(const K3Engine *e)
{
    (void)e;
    return K3_MAX_PROMPT;
}

void k3_gen_out_free(K3GenOut *o)
{
    if (!o) return;
    free(o->text);
    free(o->ids);
    memset(o, 0, sizeof *o);
}
