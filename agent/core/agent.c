/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include "agent.h"
#include "../util/buf.h"
#include "../util/log.h"

#include <stdlib.h>
#include <string.h>

Agent *agent_create(const AgentConfig *cfg, ModelBackend *backend)
{
    Agent *a = (Agent *)calloc(1, sizeof *a);
    if (!a) return NULL;
    a->cfg = *cfg;
    a->backend = backend;

    a->events = event_bus_create();
    a->sandbox = sandbox_create(cfg->workspace, cfg->sandbox);
    a->perm = permissions_create(cfg->approval, cfg->autonomy_level);
    if (!a->events || !a->sandbox || !a->perm) { agent_destroy(a); return NULL; }

    /* optional permissions file next to the config workspace */
    {
        char pp[1100];
        snprintf(pp, sizeof pp, "config/permissions.json");
        permissions_load(a->perm, pp);
    }

    if (cfg->memory_enabled) {
        a->db = db_open(cfg->database);
        if (!a->db) LOG_W("agent", "database disabled: cannot open %s", cfg->database);
    }

    a->tools = tool_registry_create();
    if (!a->tools) { agent_destroy(a); return NULL; }
    tool_registry_register_builtins(a->tools);

    /* system prompt */
    size_t len = 0;
    a->system_prompt = read_entire_file(cfg->system_prompt_path, 1 << 16, &len, NULL);
    if (!a->system_prompt) {
        LOG_W("agent", "system prompt not found at %s; using built-in default",
              cfg->system_prompt_path);
        a->system_prompt = strdup(
            "You are an autonomous software and knowledge agent. Complete the user's "
            "objective using tools; never claim an action happened unless a tool did it; "
            "verify results; ask the user only when essential.");
    }

    LOG_I("agent", "ready: backend=%s tools=%d autonomy=%d approval=%s sandbox=%s",
          backend ? backend->name : "none", tool_registry_count(a->tools),
          cfg->autonomy_level, agent_config_approval_name(cfg->approval),
          cfg->sandbox ? "on" : "off");
    return a;
}

void agent_destroy(Agent *a)
{
    if (!a) return;
    if (a->tools) tool_registry_destroy(a->tools);
    if (a->perm) permissions_destroy(a->perm);
    if (a->sandbox) sandbox_destroy(a->sandbox);
    if (a->db) db_close(a->db);
    if (a->events) event_bus_destroy(a->events);
    free(a->system_prompt);
    free(a);
}

void agent_tool_context(Agent *a, ToolContext *ctx, const char *task_id)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->workspace = sandbox_root(a->sandbox);
    ctx->sandbox_enabled = a->cfg.sandbox;
    ctx->sandbox = a->sandbox;
    ctx->permissions = a->perm;
    ctx->confirm = a->confirm;
    ctx->confirm_ud = a->confirm_ud;
    ctx->events = a->events;
    ctx->task_id = task_id;
}

int agent_subscribe(Agent *a, EventHandler h, void *ud)
{
    return event_bus_subscribe(a->events, h, ud);
}
