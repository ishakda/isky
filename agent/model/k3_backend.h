/* SPDX-License-Identifier: Apache-2.0 */
/* k3_backend.h - adapts the K3 engine session to the agent's ModelBackend vtable. */
#ifndef AGENT_K3_BACKEND_H
#define AGENT_K3_BACKEND_H

#include "model_interface.h"
#include "k3_engine.h"

/* Creates (but does not load) a backend; the heavy load happens in initialize(),
 * so a CLI can print its banner before the minutes-long bind starts. */
ModelBackend *k3_backend_create(const K3EngineOpts *opts);
void          k3_backend_destroy(ModelBackend *b);

#endif
