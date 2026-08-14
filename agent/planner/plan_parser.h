/* SPDX-License-Identifier: Apache-2.0 */
/* plan_parser.h - parse and validate a plan (build prompt §7). A plan is:
 * { "goal": "...", "steps": [ {id, description, tool, expected_result, verification} ] }
 * Never trust raw model output: extract, repair, validate; malformed => reject. */
#ifndef AGENT_PLAN_PARSER_H
#define AGENT_PLAN_PARSER_H

#include "cJSON.h"

typedef struct {
    int   id;
    char *description;
    char *tool;             /* may be empty: a reasoning-only step */
    char *expected_result;
    char *verification;     /* e.g. "file_exists:hello.c" or "exit_zero" */
    int   done;
    int   ok;
} PlanStep;

typedef struct {
    char     *goal;
    PlanStep *steps;
    int       n_steps;
    char      error[256];
} Plan;

void plan_init(Plan *p);
void plan_free(Plan *p);

/* Parse from a model action's steps array (owned by caller) or from raw text.
 * Returns 1 on success, 0 on failure (p->error set). */
int  plan_from_steps_array(Plan *p, const cJSON *steps, const char *goal);
int  plan_parse_text(Plan *p, const char *model_text);

/* Serialize back to canonical JSON (caller frees). Used for persistence + prompts. */
char *plan_to_json(const Plan *p);
/* Parse a "kind:arg" verification spec into a VerifyCheck (arg points into spec). */
void  plan_parse_verification(const char *spec, const char **kind, const char **arg,
                              char *scratch, int scratchsz);

#endif
