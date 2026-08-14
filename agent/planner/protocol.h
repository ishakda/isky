/* SPDX-License-Identifier: Apache-2.0 */
/* protocol.h - the strict agent action protocol (build prompt §16,§17,§38).
 * The model must emit exactly one of these actions as a JSON object. This module
 * extracts it from raw model text, repairs near-JSON, and validates it. Malformed
 * actions are NEVER executed. */
#ifndef AGENT_PROTOCOL_H
#define AGENT_PROTOCOL_H

#include "cJSON.h"

typedef enum {
    ACT_INVALID = 0,
    ACT_TOOL,
    ACT_FINAL,
    ACT_ASK_USER,
    ACT_PLAN,
    ACT_REFLECT
} ActionType;

typedef struct {
    ActionType type;
    char      *tool;            /* ACT_TOOL */
    cJSON     *arguments;       /* ACT_TOOL: owned object (never NULL when TOOL) */
    char      *answer;          /* ACT_FINAL */
    char      *question;        /* ACT_ASK_USER */
    char      *message;         /* ACT_REFLECT */
    char      *expected_result; /* ACT_TOOL, optional */
    char      *reason;          /* optional, any */
    cJSON     *steps;           /* ACT_PLAN: owned array */
    char      *raw;             /* the JSON object text actually parsed */
    char       error[256];      /* why parsing/validation failed (ACT_INVALID) */
} AgentAction;

void agent_action_init(AgentAction *a);
void agent_action_free(AgentAction *a);

/* Parse model output into an action. Tries: strict extract -> repair -> fail.
 * Returns a->type; on failure a->type==ACT_INVALID and a->error is set. */
ActionType agent_action_parse(const char *model_text, AgentAction *out);

/* Validate a parsed action against the registry of tool names (NULL-terminated
 * array). Checks action validity, tool existence, and required fields. Returns 1
 * if valid, 0 otherwise (fills out->error). */
int agent_action_validate(AgentAction *a, const char *const *tool_names);

const char *action_type_name(ActionType t);

#endif
