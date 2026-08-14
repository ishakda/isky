/* SPDX-License-Identifier: Apache-2.0 */
#include "tool_registry.h"
#include "../security/permissions.h"
#include "../reasoning/verification.h"
#include "../util/buf.h"
#include "../util/log.h"
#include "../core/events.h"

#include <stdlib.h>
#include <string.h>

#define MAX_TOOLS 64

struct ToolRegistry {
    AgentTool tools[MAX_TOOLS];
    int       n;
};

/* built-in registration entry points (defined in the tool source files) */
void fs_tools_register(ToolRegistry *r);
void shell_tool_register(ToolRegistry *r);
void code_tools_register(ToolRegistry *r);
void git_tools_register(ToolRegistry *r);
void web_tools_register(ToolRegistry *r);

ToolRegistry *tool_registry_create(void)
{
    return (ToolRegistry *)calloc(1, sizeof(ToolRegistry));
}

void tool_registry_destroy(ToolRegistry *r) { free(r); }

int tool_registry_register(ToolRegistry *r, const AgentTool *tool)
{
    if (!r || r->n >= MAX_TOOLS || !tool || !tool->name) return -1;
    if (tool_registry_find(r, tool->name)) return -1;
    r->tools[r->n++] = *tool;
    return 0;
}

const AgentTool *tool_registry_find(ToolRegistry *r, const char *name)
{
    if (!r || !name) return NULL;
    for (int i = 0; i < r->n; i++)
        if (!strcmp(r->tools[i].name, name)) return &r->tools[i];
    return NULL;
}

int tool_registry_count(const ToolRegistry *r) { return r ? r->n : 0; }
const AgentTool *tool_registry_at(const ToolRegistry *r, int i)
{
    return (r && i >= 0 && i < r->n) ? &r->tools[i] : NULL;
}

int tool_registry_execute(ToolRegistry *r, const char *name, const cJSON *args,
                          ToolContext *ctx, ToolResult *res)
{
    tool_result_init(res);
    const AgentTool *t = tool_registry_find(r, name);
    if (!t) {
        tool_result_fail(res, REC_INVALID_ARGUMENT, "unknown tool '%s'", name ? name : "");
        return 0;
    }
    Permissions *perm = (Permissions *)(ctx ? ctx->permissions : NULL);
    if (perm && !permissions_is_enabled(perm, name)) {
        tool_result_fail(res, REC_PERMISSION_ERROR,
                         "tool '%s' is disabled by the permission policy", name);
        return 0;
    }

    /* confirmation gate */
    int need_conf = t->requires_confirmation;
    if (perm)
        need_conf = permissions_needs_confirmation(perm, name, t->security_level) ||
                    (t->requires_confirmation);
    if (need_conf) {
        if (!ctx || !ctx->confirm) {
            /* No confirmation channel available: fail closed for anything gated. */
            tool_result_fail(res, REC_PERMISSION_ERROR,
                "'%s' (%s) requires confirmation but no approver is attached; refusing",
                name, security_level_name(t->security_level));
            return 0;
        }
        char *summary = args ? cJSON_PrintUnformatted(args) : strdup("{}");
        int ok = ctx->confirm(name, t->security_level, summary ? summary : "{}",
                              ctx->confirm_ud);
        free(summary);
        if (!ok) {
            tool_result_fail(res, REC_PERMISSION_ERROR,
                             "user denied confirmation for '%s'", name);
            return 0;
        }
    }

    EventBus *bus = (EventBus *)(ctx ? ctx->events : NULL);
    if (bus) event_bus_emit(bus, EV_TOOL_STARTED, ctx->task_id, 0, name);
    int rc = t->execute(t, args, ctx, res);
    if (rc != 0 && res->ok) {
        /* dispatch error but result claims ok: normalize */
        tool_result_fail(res, REC_TOOL_ERROR, "tool dispatch error");
    }
    if (bus)
        event_bus_emit(bus, res->ok ? EV_TOOL_COMPLETED : EV_TOOL_FAILED,
                       ctx->task_id, 0, name);
    return 0;
}

char *tool_registry_describe(ToolRegistry *r, ToolContext *ctx)
{
    Permissions *perm = (Permissions *)(ctx ? ctx->permissions : NULL);
    ABuf b; ab_init(&b);
    for (int i = 0; i < r->n; i++) {
        const AgentTool *t = &r->tools[i];
        if (perm && !permissions_is_enabled(perm, t->name)) continue;
        ab_printf(&b, "- %s [%s]: %s\n", t->name,
                  security_level_name(t->security_level), t->description);
        if (t->args_schema && t->args_schema[0])
            ab_printf(&b, "    args: %s\n", t->args_schema);
    }
    return ab_take(&b);
}

void tool_registry_register_builtins(ToolRegistry *r)
{
    fs_tools_register(r);
    shell_tool_register(r);
    code_tools_register(r);
    git_tools_register(r);
    web_tools_register(r);
}
