/* SPDX-License-Identifier: Apache-2.0 */
/* model_interface.h - the ONE boundary between the agent runtime and any model.
 *
 * The agent never includes k3 headers. It sees a ModelBackend vtable and the two
 * request/result structs below. k3_backend.c adapts the K3 engine to this
 * interface; mock_backend.c implements it with scripted responses for tests;
 * a remote-API backend can be added later without touching the agent. */
#ifndef AGENT_MODEL_INTERFACE_H
#define AGENT_MODEL_INTERFACE_H

#include <stddef.h>

typedef struct {
    const char *prompt;
    int         max_tokens;
    float       temperature;    /* 0 = greedy */
    float       top_p;          /* 1.0 = disabled */
    int         stream;         /* invoke on_token per decoded chunk */
    const char **stop;          /* NULL-terminated array of stop strings, or NULL */
    /* Streaming callback: chunk is a NUL-terminated UTF-8 piece. Return non-zero
     * to cancel generation early (treated as a stop, not an error). */
    int  (*on_token)(const char *chunk, void *userdata);
    void *userdata;
} K3GenerationRequest;

typedef struct {
    char  *text;               /* malloc'd, caller frees via model_result_free */
    int    tokens_generated;
    double generation_time;    /* seconds */
    int    error_code;         /* 0 ok; nonzero = MODEL_ERROR class */
    char   error[256];
    int    stopped_by;         /* 0 max_tokens, 1 stop string, 2 cancelled, 3 eos */
} K3GenerationResult;

typedef struct ModelBackend {
    const char *name;
    /* Returns 0 on success. */
    int  (*initialize)(struct ModelBackend *self);
    int  (*generate)(struct ModelBackend *self, const K3GenerationRequest *req,
                     K3GenerationResult *res);
    void (*shutdown)(struct ModelBackend *self);
    /* Rough token count for context budgeting. Never fails; falls back to
     * bytes/4 heuristics when no tokenizer is loaded. */
    int  (*count_tokens)(struct ModelBackend *self, const char *text);
    int   context_window;      /* max prompt tokens the backend accepts */
    void *ctx;                 /* backend-private state */
} ModelBackend;

void model_result_free(K3GenerationResult *res);

#endif
