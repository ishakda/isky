/* SPDX-License-Identifier: Apache-2.0 */
#include "task.h"
#include "../util/platform.h"

#include <stdlib.h>
#include <string.h>

AgentTask *task_create(const char *goal, int max_iterations)
{
    AgentTask *t = (AgentTask *)calloc(1, sizeof *t);
    if (!t) return NULL;
    gen_id(t->id, sizeof t->id, "task");
    t->goal = goal ? strdup(goal) : strdup("");
    t->state = AGENT_IDLE;
    t->max_iterations = max_iterations > 0 ? max_iterations : 30;
    t->current_step = 0;
    t->total_steps = 0;
    t->created_at = t->updated_at = time(NULL);
    return t;
}

void task_free(AgentTask *t)
{
    if (!t) return;
    free(t->goal);
    free(t->plan);
    free(t->user_question);
    free(t->final_answer);
    free(t);
}

void task_touch(AgentTask *t) { t->updated_at = time(NULL); }

void task_set_plan(AgentTask *t, const char *plan_json, int total_steps)
{
    free(t->plan);
    t->plan = plan_json ? strdup(plan_json) : NULL;
    t->total_steps = total_steps;
    t->current_step = 0;
    task_touch(t);
}

void task_set_answer(AgentTask *t, const char *answer)
{
    free(t->final_answer);
    t->final_answer = answer ? strdup(answer) : NULL;
    task_touch(t);
}

void task_set_question(AgentTask *t, const char *question)
{
    free(t->user_question);
    t->user_question = question ? strdup(question) : NULL;
    t->requires_user_input = question != NULL;
    task_touch(t);
}
