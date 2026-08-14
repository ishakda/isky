/* SPDX-License-Identifier: Apache-2.0 */
/* agent.h - the Agent object: owns the backend, registry, security, memory,
 * database, events, and config. One Agent runs many tasks in its lifetime. */
#ifndef AGENT_AGENT_H
#define AGENT_AGENT_H

#include "config.h"
#include "task.h"
#include "events.h"
#include "../model/model_interface.h"
#include "../tools/tool_registry.h"
#include "../security/permissions.h"
#include "../security/sandbox.h"
#include "../storage/database.h"

typedef struct Agent {
    AgentConfig    cfg;
    ModelBackend  *backend;      /* not owned; created by main/tests */
    ToolRegistry  *tools;
    Permissions   *perm;
    Sandbox       *sandbox;
    Database      *db;
    EventBus      *events;
    char          *system_prompt;
    /* confirmation hook (wired by the CLI/API) */
    int  (*confirm)(const char *tool, SecurityLevel lvl, const char *summary, void *ud);
    void  *confirm_ud;
} Agent;

/* Build an agent from config + an already-created backend. Loads permissions,
 * sandbox, database, system prompt, registers builtins. Returns NULL on failure. */
Agent *agent_create(const AgentConfig *cfg, ModelBackend *backend);
void   agent_destroy(Agent *a);

/* Populate a ToolContext pointing at this agent's security + events. */
void   agent_tool_context(Agent *a, ToolContext *ctx, const char *task_id);

/* Convenience: subscribe a handler to the event bus. */
int    agent_subscribe(Agent *a, EventHandler h, void *ud);

#endif
