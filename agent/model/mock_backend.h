/* SPDX-License-Identifier: Apache-2.0 */
/* mock_backend.h - deterministic scripted backend for testing the agent stack.
 *
 * Two modes:
 *   1. Scripted: mock_backend_push() queues responses returned in FIFO order.
 *   2. Script file: a JSON array of strings loaded from disk (used by CLI
 *      --backend mock --mock-script FILE for reproducible end-to-end demos).
 * When the queue is empty the backend returns a final-answer JSON action, so a
 * runaway loop terminates deterministically in tests. */
#ifndef AGENT_MOCK_BACKEND_H
#define AGENT_MOCK_BACKEND_H

#include "model_interface.h"

ModelBackend *mock_backend_create(void);
void          mock_backend_destroy(ModelBackend *b);
int           mock_backend_push(ModelBackend *b, const char *response);
int           mock_backend_load_script(ModelBackend *b, const char *path);
/* number of generate() calls served so far */
int           mock_backend_calls(ModelBackend *b);
/* last prompt received (for asserting context construction); valid until next call */
const char   *mock_backend_last_prompt(ModelBackend *b);

#endif
