/* SPDX-License-Identifier: Apache-2.0 */
/* server.h - optional local HTTP API (build prompt §27). Minimal, single-threaded,
 * loopback-only. Endpoints: POST /v1/tasks, GET /v1/tasks/:id, cancel, resume,
 * events (snapshot), GET /v1/tools, GET /v1/memory. */
#ifndef AGENT_SERVER_H
#define AGENT_SERVER_H

#include "../core/agent.h"

typedef struct ApiServer ApiServer;

/* Binds 127.0.0.1:port. Returns NULL on failure. */
ApiServer *api_server_create(Agent *a, int port);
void       api_server_destroy(ApiServer *s);
/* Serve forever (blocking). Returns on fatal error. */
int        api_server_run(ApiServer *s);

#endif
