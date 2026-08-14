/* SPDX-License-Identifier: Apache-2.0 */
/* reflection.h - bounded reflection (build prompt §18). After an important action
 * the agent may ask "did this achieve the expected result? what next?" — but the
 * count is capped so there is no endless self-reflection loop. */
#ifndef AGENT_REFLECTION_H
#define AGENT_REFLECTION_H

typedef struct {
    int used;
    int max;
} ReflectionBudget;

void reflection_init(ReflectionBudget *r, int max_per_step);
void reflection_reset(ReflectionBudget *r);   /* call at each new step */
int  reflection_allowed(ReflectionBudget *r);  /* consume one; 0 when exhausted */

/* Compose a short reflection question about the last action/observation.
 * Caller frees. */
char *reflection_prompt(const char *action_summary, const char *observation,
                        const char *expected);

#endif
