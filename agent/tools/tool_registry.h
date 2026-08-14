/* SPDX-License-Identifier: Apache-2.0 */
/* tool_registry.h - dynamic tool registry + validated execution (build prompt §8,§17).
 * A model NEVER executes a function. It emits an action; the registry validates the
 * request (existence, arg schema, permission, security level, confirmation) and only
 * then dispatches. */
#ifndef AGENT_TOOL_REGISTRY_H
#define AGENT_TOOL_REGISTRY_H

#include "tool.h"

typedef struct ToolRegistry ToolRegistry;

ToolRegistry *tool_registry_create(void);
void          tool_registry_destroy(ToolRegistry *r);

/* Register a tool (shallow-copies the descriptor; name must be stable/static or
 * owned elsewhere for the registry's lifetime). Returns 0, or -1 on dup/full. */
int          tool_registry_register(ToolRegistry *r, const AgentTool *tool);
const AgentTool *tool_registry_find(ToolRegistry *r, const char *name);
int          tool_registry_count(const ToolRegistry *r);
const AgentTool *tool_registry_at(const ToolRegistry *r, int i);

/* Validate + execute. Performs, in order: existence, permission/enablement,
 * security-level gating, confirmation (when required by policy), then execute().
 * Returns 0 when a decision was reached (inspect res->ok); res is always filled,
 * including on validation rejection (res->ok = 0 with an explanatory error). */
int tool_registry_execute(ToolRegistry *r, const char *name, const cJSON *args,
                          ToolContext *ctx, ToolResult *res);

/* Build the human-readable "AVAILABLE TOOLS" block for the model prompt. Caller
 * frees. Only lists tools currently enabled by the permission policy in ctx. */
char *tool_registry_describe(ToolRegistry *r, ToolContext *ctx);

/* Register the full built-in tool set (filesystem, shell, code, git, web). */
void tool_registry_register_builtins(ToolRegistry *r);

#endif
