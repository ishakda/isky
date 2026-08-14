/* SPDX-License-Identifier: Apache-2.0 */
/* tool.h - the generic tool interface (build prompt §8) and the security levels
 * every tool carries (§21). The model requests a tool; the executor validates the
 * request (schema, existence, permission, security level, sandbox) and only then
 * runs it. The model never calls a C function directly. */
#ifndef AGENT_TOOL_H
#define AGENT_TOOL_H

#include "cJSON.h"

typedef enum {
    SEC_SAFE = 0,     /* read-only, no side effects           */
    SEC_LOW_RISK,     /* creates/edits inside the sandbox     */
    SEC_MEDIUM_RISK,  /* runs commands                        */
    SEC_HIGH_RISK,    /* deletes, destructive fs ops          */
    SEC_CRITICAL      /* system configuration                 */
} SecurityLevel;

const char *security_level_name(SecurityLevel l);

/* Result of a tool execution. `output` is human/model-readable text; `data` is an
 * optional structured payload the tool owns (freed with the result). */
typedef struct {
    int    ok;              /* 1 success, 0 failure */
    char  *output;          /* malloc'd, NUL-terminated; never NULL after fill */
    int    truncated;
    char  *error;           /* malloc'd on failure, else NULL */
    int    error_class;     /* one of RecoveryClass (see reasoning/verification.h) */
    cJSON *data;            /* optional structured result, owned */
} ToolResult;

void tool_result_init(ToolResult *r);
void tool_result_free(ToolResult *r);
void tool_result_ok(ToolResult *r, char *output_owned);
void tool_result_okf(ToolResult *r, const char *fmt, ...);
void tool_result_fail(ToolResult *r, int error_class, const char *fmt, ...);

/* Context handed to every tool: the sandbox root and approval hook. Defined fully
 * here to avoid a cycle; security/permissions owns the policy behind approve(). */
struct ToolRegistry;
typedef struct ToolContext {
    const char *workspace;        /* sandbox root (absolute) */
    int         sandbox_enabled;
    void       *sandbox;          /* Sandbox* (opaque here) */
    void       *permissions;      /* Permissions* */
    /* Confirmation hook: return 1 to allow, 0 to deny. May be NULL (deny risky). */
    int  (*confirm)(const char *tool, SecurityLevel lvl, const char *summary, void *ud);
    void  *confirm_ud;
    void  *events;                /* EventBus*, optional */
    const char *task_id;
} ToolContext;

typedef struct AgentTool {
    const char   *name;
    const char   *description;
    const char   *args_schema;    /* human-readable arg doc, shown to the model */
    SecurityLevel security_level;
    int           requires_confirmation;
    /* Execute with parsed JSON arguments. Returns 0 when the tool ran (inspect
     * result->ok for success/failure); nonzero only for internal dispatch errors. */
    int  (*execute)(const struct AgentTool *self, const cJSON *args,
                    ToolContext *ctx, ToolResult *res);
    void *state;                  /* tool-private */
} AgentTool;

#endif
