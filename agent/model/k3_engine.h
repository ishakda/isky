/* SPDX-License-Identifier: Apache-2.0 */
/* k3_engine.h - a reusable generation session over the K3 inference library.
 *
 * WHY THIS EXISTS. The upstream CLI (src/cli/k3_run.c) contains the only complete
 * prompt->tokens->decode loop in the repository, as static functions inside main().
 * It loads the model, decodes once, and exits; it is greedy-only and prints rather
 * than returns. The agent needs the opposite shape: load ONCE, generate MANY times
 * within one process, with sampling, stop strings and a streaming callback, returning
 * text. This module builds that on the same public APIs the CLI uses (k3_st, k3_bind,
 * k3_trunk, k3_cache, k3_decoder_layer_inc, the tokenizer). k3_run.c is deliberately
 * NOT modified: its forward path is oracle-validated and stays byte-identical.
 *
 * The forward pass here mirrors the CLI's semantics exactly (embed rows ->
 * k3_decoder_layer_inc over all layers -> model-level attn-res -> final norm ->
 * lm_head) and test_k3_engine gates it against the CLI's output on the tiny
 * checkpoint, so a drift between the two paths is a test failure, not a mystery. */
#ifndef AGENT_K3_ENGINE_H
#define AGENT_K3_ENGINE_H

#include <stddef.h>

typedef struct {
    const char *model_dir;    /* checkpoint directory (safetensors shards + config.json) */
    const char *tok_dir;      /* tiktoken.model dir, or NULL for id-only sessions */
    const char *cfg_path;     /* explicit config.json, or NULL to use model_dir's */
    const char *trunk_dir;    /* packed trunk dir to stream, or NULL for resident */
    double      trunk_gb;     /* stream budget when trunk_dir is set */
    double      cache_gb;     /* expert cache budget (default 0.5 when 0) */
    int         n_layers;     /* 0 = all layers */
    int         max_gen_default;
    int         verbose;
} K3EngineOpts;

typedef struct K3Engine K3Engine;

typedef struct {
    float  temperature;   /* 0 = greedy */
    float  top_p;         /* 0 or 1 = disabled */
    int    max_tokens;
    unsigned seed;        /* 0 = derive from time */
    const char **stop;    /* NULL-terminated, or NULL */
    int  (*on_text)(const char *chunk, void *ud);   /* nonzero return cancels */
    void  *userdata;
} K3SampleOpts;

typedef struct {
    char  *text;          /* malloc'd; NULL on id-only sessions */
    int   *ids;           /* malloc'd generated ids */
    int    n_ids;
    double seconds;
    int    stopped_by;    /* 0 max_tokens, 1 stop string, 2 cancelled, 3 eos */
} K3GenOut;

/* Returns 0 on success; on failure writes a human-readable reason into err. */
int  k3_engine_open(K3Engine **out, const K3EngineOpts *opts, char *err, size_t errsz);
void k3_engine_close(K3Engine *e);

/* Text in, text out. Requires tok_dir. Caller frees out->text and out->ids. */
int  k3_engine_generate(K3Engine *e, const char *prompt, const K3SampleOpts *s,
                        K3GenOut *out, char *err, size_t errsz);

/* Ids in, ids out (greedy or sampled). The reproducible channel used by tests. */
int  k3_engine_generate_ids(K3Engine *e, const int *prompt, int np,
                            const K3SampleOpts *s, K3GenOut *out,
                            char *err, size_t errsz);

int  k3_engine_count_tokens(K3Engine *e, const char *text);
int  k3_engine_context_window(const K3Engine *e);
void k3_gen_out_free(K3GenOut *o);

#endif
