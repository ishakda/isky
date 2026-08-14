/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include "mock_backend.h"
#include "../util/buf.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

typedef struct {
    char **queue;
    int    head, count, cap;
    int    calls;
    char  *last_prompt;
} MockCtx;

static int mock_init(ModelBackend *self) { (void)self; return 0; }

static int mock_generate(ModelBackend *self, const K3GenerationRequest *req,
                         K3GenerationResult *res)
{
    MockCtx *m = (MockCtx *)self->ctx;
    memset(res, 0, sizeof *res);
    m->calls++;
    free(m->last_prompt);
    m->last_prompt = req->prompt ? strdup(req->prompt) : NULL;

    const char *text;
    char *owned = NULL;
    if (m->head < m->count) {
        text = m->queue[m->head++];
    } else {
        text = "{\"action\":\"final\",\"answer\":\"mock backend queue exhausted\"}";
    }
    owned = strdup(text);
    if (!owned) { res->error_code = 1; snprintf(res->error, sizeof res->error, "OOM"); return -1; }

    if (req->stream && req->on_token)
        req->on_token(owned, req->userdata);

    res->text = owned;
    res->tokens_generated = (int)(strlen(owned) / 4) + 1;
    res->generation_time = 0.001;
    res->stopped_by = 3;
    return 0;
}

static void mock_shutdown(ModelBackend *self)
{
    (void)self;
}

static int mock_count_tokens(ModelBackend *self, const char *text)
{
    (void)self;
    return text ? (int)(strlen(text) / 4) + 1 : 0;
}

ModelBackend *mock_backend_create(void)
{
    ModelBackend *b = (ModelBackend *)calloc(1, sizeof *b);
    MockCtx *m = (MockCtx *)calloc(1, sizeof *m);
    if (!b || !m) { free(b); free(m); return NULL; }
    m->cap = 16;
    m->queue = (char **)calloc((size_t)m->cap, sizeof(char *));
    if (!m->queue) { free(b); free(m); return NULL; }
    b->name = "mock";
    b->initialize = mock_init;
    b->generate = mock_generate;
    b->shutdown = mock_shutdown;
    b->count_tokens = mock_count_tokens;
    b->context_window = 32768;
    b->ctx = m;
    return b;
}

void mock_backend_destroy(ModelBackend *b)
{
    if (!b) return;
    MockCtx *m = (MockCtx *)b->ctx;
    if (m) {
        for (int i = 0; i < m->count; i++) free(m->queue[i]);
        free(m->queue);
        free(m->last_prompt);
        free(m);
    }
    free(b);
}

int mock_backend_push(ModelBackend *b, const char *response)
{
    MockCtx *m = (MockCtx *)b->ctx;
    if (m->count == m->cap) {
        int nc = m->cap * 2;
        char **nq = (char **)realloc(m->queue, (size_t)nc * sizeof(char *));
        if (!nq) return -1;
        m->queue = nq; m->cap = nc;
    }
    m->queue[m->count] = strdup(response);
    if (!m->queue[m->count]) return -1;
    m->count++;
    return 0;
}

int mock_backend_load_script(ModelBackend *b, const char *path)
{
    size_t len = 0;
    char *data = read_entire_file(path, 0, &len, NULL);
    if (!data) return -1;
    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!root) return -1;
    int rc = 0;
    if (cJSON_IsArray(root)) {
        cJSON *it;
        cJSON_ArrayForEach(it, root) {
            if (cJSON_IsString(it)) {
                if (mock_backend_push(b, it->valuestring) != 0) { rc = -1; break; }
            } else {
                /* allow objects: serialize them back to a JSON string */
                char *s = cJSON_PrintUnformatted(it);
                if (!s || mock_backend_push(b, s) != 0) rc = -1;
                free(s);
                if (rc) break;
            }
        }
    } else rc = -1;
    cJSON_Delete(root);
    return rc;
}

int mock_backend_calls(ModelBackend *b) { return ((MockCtx *)b->ctx)->calls; }

const char *mock_backend_last_prompt(ModelBackend *b)
{
    return ((MockCtx *)b->ctx)->last_prompt;
}
