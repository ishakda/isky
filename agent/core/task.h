/* SPDX-License-Identifier: Apache-2.0 */
/* task.h - every user request becomes a task (build prompt §6). */
#ifndef AGENT_TASK_H
#define AGENT_TASK_H

#include <time.h>
#include "state.h"

typedef struct {
    char       id[64];
    char      *goal;
    AgentState state;

    char      *plan;              /* serialized JSON plan, or NULL */
    int        current_step;
    int        total_steps;

    int        max_iterations;
    int        iteration;

    int        success;
    int        requires_user_input;
    char      *user_question;     /* set when requires_user_input */
    char      *final_answer;      /* set on completion */

    time_t     created_at;
    time_t     updated_at;
} AgentTask;

AgentTask *task_create(const char *goal, int max_iterations);
void       task_free(AgentTask *t);
void       task_touch(AgentTask *t);
void       task_set_plan(AgentTask *t, const char *plan_json, int total_steps);
void       task_set_answer(AgentTask *t, const char *answer);
void       task_set_question(AgentTask *t, const char *question);

#endif
