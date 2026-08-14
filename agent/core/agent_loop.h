/* SPDX-License-Identifier: Apache-2.0 */
/* agent_loop.h - the autonomous loop / state machine (build prompt §5). */
#ifndef AGENT_LOOP_H
#define AGENT_LOOP_H

#include "agent.h"
#include "task.h"

typedef enum {
    RUN_COMPLETED = 0,   /* task->final_answer set */
    RUN_FAILED,
    RUN_NEEDS_USER,      /* task->user_question set; call agent_loop_resume with answer */
    RUN_MAX_ITERS
} RunOutcome;

typedef struct {
    int plan_only;       /* build the plan, do not execute (build prompt §47) */
    int dry_run;         /* show planned actions/tools, execute nothing (§48) */
} RunOptions;

/* Run the task to a terminal state (or until it needs the user / hits max iters).
 * The task must already have its goal set; state is driven internally. */
RunOutcome agent_loop_run(Agent *a, AgentTask *task, const RunOptions *opt);

/* Resume a task that stopped in WAITING_USER, feeding the user's answer. */
RunOutcome agent_loop_resume(Agent *a, AgentTask *task, const char *user_answer,
                             const RunOptions *opt);

#endif
