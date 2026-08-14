/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include "planner.h"
#include "../util/buf.h"
#include "../util/log.h"

#include <stdlib.h>
#include <string.h>

static char *build_prompt(const char *goal, const char *tools, const char *mem)
{
    ABuf b; ab_init(&b);
    ab_puts(&b,
        "You are the PLANNER of an autonomous agent. Decompose the objective into a "
        "short sequence of concrete, verifiable steps.\n\n"
        "Respond with ONE JSON object and nothing else:\n"
        "{\n"
        "  \"goal\": \"restated objective\",\n"
        "  \"steps\": [\n"
        "    {\"id\":1, \"description\":\"what to do\", \"tool\":\"tool_name_or_empty\", "
        "\"expected_result\":\"what success looks like\", "
        "\"verification\":\"file_exists:PATH | contains:TEXT | exit_zero | nonempty\"}\n"
        "  ]\n"
        "}\n\n"
        "Rules: keep it minimal (1-8 steps). Use only listed tools. A step needing no "
        "tool leaves \"tool\" empty. Prefer verifiable steps.\n\n");
    if (tools && tools[0]) { ab_puts(&b, "AVAILABLE TOOLS:\n"); ab_puts(&b, tools); ab_putc(&b, '\n'); }
    if (mem && mem[0])     { ab_puts(&b, "RELEVANT MEMORY:\n"); ab_puts(&b, mem); ab_putc(&b, '\n'); }
    ab_printf(&b, "OBJECTIVE: %s\n\nJSON plan:\n", goal);
    return ab_take(&b);
}

int planner_make_plan(Planner *pl, const char *goal, const char *mem, Plan *plan)
{
    plan_init(plan);
    char *prompt = build_prompt(goal, pl->tools_desc, mem);
    int attempts = pl->max_attempts > 0 ? pl->max_attempts : 3;

    for (int i = 0; i < attempts; i++) {
        K3GenerationRequest req;
        memset(&req, 0, sizeof req);
        req.prompt = prompt;
        req.max_tokens = pl->max_tokens > 0 ? pl->max_tokens : 1024;
        req.temperature = i == 0 ? pl->temperature : pl->temperature + 0.1f * i;
        req.top_p = 0.95f;
        K3GenerationResult res;
        if (pl->backend->generate(pl->backend, &req, &res) != 0) {
            LOG_W("planner", "model generation failed (attempt %d): %s", i + 1, res.error);
            model_result_free(&res);
            continue;
        }
        int ok = plan_parse_text(plan, res.text);
        model_result_free(&res);
        if (ok) {
            LOG_I("planner", "plan with %d step(s) (attempt %d)", plan->n_steps, i + 1);
            free(prompt);
            return 1;
        }
        LOG_W("planner", "plan parse failed (attempt %d): %s", i + 1, plan->error);
        plan_free(plan);
        plan_init(plan);
    }
    free(prompt);

    /* Simplified fallback: a single step that hands the whole goal to the loop,
     * which will then drive the model action-by-action. Never a malformed plan. */
    LOG_W("planner", "falling back to single-step direct-execution plan");
    plan->steps = (PlanStep *)calloc(1, sizeof(PlanStep));
    if (!plan->steps) { snprintf(plan->error, sizeof plan->error, "OOM"); return 0; }
    plan->steps[0].id = 1;
    plan->steps[0].description = strdup(goal);
    plan->steps[0].tool = strdup("");
    plan->n_steps = 1;
    plan->goal = strdup(goal);
    return 1;
}
