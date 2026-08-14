/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include "k3_backend.h"
#include "../util/log.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    K3EngineOpts opts;
    /* deep copies of the option strings so the caller's argv may vanish */
    char *model_dir, *tok_dir, *cfg_path, *trunk_dir;
    K3Engine *engine;
    float default_temp, default_top_p;
} K3BackendCtx;

static int k3b_init(ModelBackend *self)
{
    K3BackendCtx *c = (K3BackendCtx *)self->ctx;
    if (c->engine) return 0;
    char err[512];
    LOG_I("k3", "loading model from %s%s", c->opts.model_dir,
          c->opts.trunk_dir ? " (streamed trunk)" : "");
    if (k3_engine_open(&c->engine, &c->opts, err, sizeof err) != 0) {
        LOG_E("k3", "engine open failed: %s", err);
        return -1;
    }
    self->context_window = k3_engine_context_window(c->engine);
    LOG_I("k3", "model ready");
    return 0;
}

static int k3b_generate(ModelBackend *self, const K3GenerationRequest *req,
                        K3GenerationResult *res)
{
    K3BackendCtx *c = (K3BackendCtx *)self->ctx;
    memset(res, 0, sizeof *res);
    if (!c->engine && k3b_init(self) != 0) {
        res->error_code = 1;
        snprintf(res->error, sizeof res->error, "model failed to initialize");
        return -1;
    }
    K3SampleOpts s;
    memset(&s, 0, sizeof s);
    s.temperature = req->temperature;
    s.top_p = req->top_p;
    s.max_tokens = req->max_tokens;
    s.stop = req->stop;
    if (req->stream && req->on_token) {
        s.on_text = req->on_token;
        s.userdata = req->userdata;
    }
    K3GenOut out;
    char err[512];
    if (k3_engine_generate(c->engine, req->prompt, &s, &out, err, sizeof err) != 0) {
        res->error_code = 1;
        snprintf(res->error, sizeof res->error, "%s", err);
        return -1;
    }
    res->text = out.text ? out.text : strdup("");
    res->tokens_generated = out.n_ids;
    res->generation_time = out.seconds;
    res->stopped_by = out.stopped_by;
    free(out.ids);
    return 0;
}

static void k3b_shutdown(ModelBackend *self)
{
    K3BackendCtx *c = (K3BackendCtx *)self->ctx;
    if (c->engine) { k3_engine_close(c->engine); c->engine = NULL; }
}

static int k3b_count(ModelBackend *self, const char *text)
{
    K3BackendCtx *c = (K3BackendCtx *)self->ctx;
    if (c->engine) return k3_engine_count_tokens(c->engine, text);
    return text ? (int)(strlen(text) / 4) + 1 : 0;
}

ModelBackend *k3_backend_create(const K3EngineOpts *opts)
{
    ModelBackend *b = (ModelBackend *)calloc(1, sizeof *b);
    K3BackendCtx *c = (K3BackendCtx *)calloc(1, sizeof *c);
    if (!b || !c) { free(b); free(c); return NULL; }
    c->opts = *opts;
    if (opts->model_dir) { c->model_dir = strdup(opts->model_dir); c->opts.model_dir = c->model_dir; }
    if (opts->tok_dir)   { c->tok_dir   = strdup(opts->tok_dir);   c->opts.tok_dir   = c->tok_dir; }
    if (opts->cfg_path)  { c->cfg_path  = strdup(opts->cfg_path);  c->opts.cfg_path  = c->cfg_path; }
    if (opts->trunk_dir) { c->trunk_dir = strdup(opts->trunk_dir); c->opts.trunk_dir = c->trunk_dir; }
    b->name = "k3";
    b->initialize = k3b_init;
    b->generate = k3b_generate;
    b->shutdown = k3b_shutdown;
    b->count_tokens = k3b_count;
    b->context_window = 32768;
    b->ctx = c;
    return b;
}

void k3_backend_destroy(ModelBackend *b)
{
    if (!b) return;
    K3BackendCtx *c = (K3BackendCtx *)b->ctx;
    if (c) {
        k3b_shutdown(b);
        free(c->model_dir); free(c->tok_dir); free(c->cfg_path); free(c->trunk_dir);
        free(c);
    }
    free(b);
}

void model_result_free(K3GenerationResult *res)
{
    if (!res) return;
    free(res->text);
    res->text = NULL;
}
