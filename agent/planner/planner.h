/* SPDX-License-Identifier: Apache-2.0 */
/* planner.h - turns a goal into a validated Plan (build prompt §7). Robust to
 * unreliable base-model output: extract -> repair -> regenerate -> simplified
 * fallback. Never returns a plan built from malformed tool instructions. */
#ifndef AGENT_PLANNER_H
#define AGENT_PLANNER_H

#include "plan_parser.h"
#include "../model/model_interface.h"

typedef struct {
    ModelBackend *backend;
    int           max_attempts;    /* generation retries on parse failure */
    float         temperature;
    int           max_tokens;
    const char   *tools_desc;      /* AVAILABLE TOOLS block for the prompt */
} Planner;

/* Produce a plan for `goal`. Returns 1 on success (fills plan), 0 on failure
 * (plan->error set). On total model failure, synthesises a single-step fallback
 * plan so the loop can still attempt direct execution. */
int planner_make_plan(Planner *pl, const char *goal, const char *memory_context,
                      Plan *plan);

#endif
