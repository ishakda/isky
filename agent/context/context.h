/* SPDX-License-Identifier: Apache-2.0 */
/* context.h - the context builder (build prompt §15,§16). Assembles the model
 * prompt from: system instructions + role + goal + current state + plan +
 * available tools + relevant memory + recent observations + required output
 * format. Budgets tokens and compresses when over budget. */
#ifndef AGENT_CONTEXT_H
#define AGENT_CONTEXT_H

#include "../model/model_interface.h"

typedef struct {
    const char *system_prompt;    /* from agent/prompts/system.txt */
    const char *role;             /* agent profile role line, or NULL */
    const char *goal;
    const char *state;            /* AgentState name */
    const char *plan_json;
    const char *tools_desc;
    const char *memory;           /* RELEVANT MEMORY block */
    const char *observations;     /* recent observations */
    const char *step_desc;        /* current step description, or NULL */
    const char *extra;            /* recovery hint / reflection, or NULL */
} ContextParts;

/* Build the full prompt. `budget_tokens` is the max prompt size; when the naive
 * assembly exceeds it, observations and memory are truncated (compressed) first,
 * preserving the system prompt, goal, plan and output-format contract. Caller
 * frees. `backend` provides token counting. */
char *context_build(ModelBackend *backend, const ContextParts *p, int budget_tokens);

/* The mandatory REQUIRED OUTPUT FORMAT block (the strict action protocol). */
const char *context_output_format(void);

#endif
