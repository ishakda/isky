/* SPDX-License-Identifier: Apache-2.0 */
#include "reflection.h"
#include "../util/buf.h"

#include <stdlib.h>

void reflection_init(ReflectionBudget *r, int max_per_step)
{
    r->max = max_per_step > 0 ? max_per_step : 2;
    r->used = 0;
}

void reflection_reset(ReflectionBudget *r) { r->used = 0; }

int reflection_allowed(ReflectionBudget *r)
{
    if (r->used >= r->max) return 0;
    r->used++;
    return 1;
}

char *reflection_prompt(const char *action_summary, const char *observation,
                        const char *expected)
{
    ABuf b; ab_init(&b);
    ab_puts(&b, "Reflect briefly on the last action, then decide the next action.\n");
    if (action_summary) ab_printf(&b, "Action: %s\n", action_summary);
    if (expected)       ab_printf(&b, "Expected: %s\n", expected);
    if (observation)    ab_printf(&b, "Observed: %s\n", observation);
    ab_puts(&b, "Did it achieve the expected result? What should happen next?\n");
    return ab_take(&b);
}
