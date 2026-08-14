/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include "context.h"
#include "../util/buf.h"
#include "../util/log.h"

#include <stdlib.h>
#include <string.h>

const char *context_output_format(void)
{
    return
    "REQUIRED OUTPUT FORMAT\n"
    "Respond with EXACTLY ONE JSON object, no prose around it, one of:\n"
    "  {\"action\":\"tool\",\"tool\":\"NAME\",\"arguments\":{...},\"expected_result\":\"...\"}\n"
    "  {\"action\":\"final\",\"answer\":\"...\"}\n"
    "  {\"action\":\"ask_user\",\"question\":\"...\"}\n"
    "  {\"action\":\"plan\",\"steps\":[...]}\n"
    "  {\"action\":\"reflect\",\"message\":\"...\"}\n"
    "Never invent tool results. Choose a tool only from AVAILABLE TOOLS.\n";
}

static void section(ABuf *b, const char *title, const char *body)
{
    if (!body || !body[0]) return;
    ab_printf(b, "\n=== %s ===\n%s\n", title, body);
}

/* Truncate a section body to a char budget, keeping the tail (most recent). */
static char *tail_trim(const char *s, int max_chars)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    if (max_chars <= 0 || (int)n <= max_chars) return strdup(s);
    const char *start = s + (n - (size_t)max_chars);
    const char *nl = strchr(start, '\n');
    if (nl) start = nl + 1;
    ABuf b; ab_init(&b);
    ab_puts(&b, "[... earlier context compressed ...]\n");
    ab_puts(&b, start);
    return ab_take(&b);
}

char *context_build(ModelBackend *backend, const ContextParts *p, int budget_tokens)
{
    /* First assembly at full size. */
    char *mem = p->memory ? strdup(p->memory) : NULL;
    char *obs = p->observations ? strdup(p->observations) : NULL;

    for (int pass = 0; pass < 3; pass++) {
        ABuf b; ab_init(&b);
        if (p->system_prompt) ab_puts(&b, p->system_prompt);
        section(&b, "ROLE", p->role);
        section(&b, "TASK", p->goal);
        section(&b, "CURRENT STATE", p->state);
        section(&b, "CURRENT STEP", p->step_desc);
        section(&b, "PLAN", p->plan_json);
        section(&b, "AVAILABLE TOOLS", p->tools_desc);
        section(&b, "MEMORY", mem);
        section(&b, "OBSERVATIONS", obs);
        section(&b, "NOTE", p->extra);
        ab_printf(&b, "\n%s\nYour JSON response:\n", context_output_format());

        int tokens = backend->count_tokens ? backend->count_tokens(backend, b.data)
                                            : (int)(b.len / 4);
        if (budget_tokens <= 0 || tokens <= budget_tokens || pass == 2) {
            free(mem); free(obs);
            if (tokens > budget_tokens && budget_tokens > 0)
                LOG_W("context", "prompt %d tokens exceeds budget %d after compression",
                      tokens, budget_tokens);
            return ab_take(&b);
        }
        /* over budget: compress the two elastic sections and retry */
        int over = tokens - budget_tokens;
        int cut_chars = over * 4 + 512;
        LOG_D("context", "prompt %d > budget %d; compressing (~%d chars)",
              tokens, budget_tokens, cut_chars);
        ab_free(&b);
        if (obs) {
            char *t = tail_trim(obs, (int)strlen(obs) - cut_chars);
            free(obs); obs = t;
            cut_chars -= (int)strlen(obs);
        }
        if (cut_chars > 0 && mem) {
            char *t = tail_trim(mem, (int)strlen(mem) - cut_chars);
            free(mem); mem = t;
        }
    }
    free(mem); free(obs);
    return strdup(p->system_prompt ? p->system_prompt : "");
}
