/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include "plan_parser.h"
#include "../util/jsonx.h"

#include <stdlib.h>
#include <string.h>

void plan_init(Plan *p) { memset(p, 0, sizeof *p); }

void plan_free(Plan *p)
{
    if (!p) return;
    free(p->goal);
    for (int i = 0; i < p->n_steps; i++) {
        free(p->steps[i].description);
        free(p->steps[i].tool);
        free(p->steps[i].expected_result);
        free(p->steps[i].verification);
    }
    free(p->steps);
    memset(p, 0, sizeof *p);
}

int plan_from_steps_array(Plan *p, const cJSON *steps, const char *goal)
{
    plan_init(p);
    if (!steps || !cJSON_IsArray(steps)) {
        snprintf(p->error, sizeof p->error, "steps is not an array");
        return 0;
    }
    int n = cJSON_GetArraySize(steps);
    if (n <= 0) { snprintf(p->error, sizeof p->error, "empty plan"); return 0; }
    if (n > 64) n = 64;   /* bound plan size */
    p->steps = (PlanStep *)calloc((size_t)n, sizeof(PlanStep));
    if (!p->steps) { snprintf(p->error, sizeof p->error, "OOM"); return 0; }
    p->goal = goal ? strdup(goal) : NULL;

    int idx = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, steps) {
        if (idx >= n) break;
        if (!cJSON_IsObject(it)) continue;
        PlanStep *s = &p->steps[idx];
        s->id = jx_int(it, "id", idx + 1);
        s->description = jx_strdup(it, "description");
        if (!s->description) s->description = jx_strdup(it, "step");
        s->tool = jx_strdup(it, "tool");
        s->expected_result = jx_strdup(it, "expected_result");
        s->verification = jx_strdup(it, "verification");
        if (!s->description) {
            /* a step with no description is unusable; skip it */
            free(s->tool); free(s->expected_result); free(s->verification);
            memset(s, 0, sizeof *s);
            continue;
        }
        idx++;
    }
    p->n_steps = idx;
    if (idx == 0) { snprintf(p->error, sizeof p->error, "no usable steps in plan"); return 0; }
    return 1;
}

int plan_parse_text(Plan *p, const char *model_text)
{
    plan_init(p);
    cJSON *obj = jx_extract_object(model_text, NULL, NULL);
    if (!obj) obj = jx_repair_object(model_text);
    if (!obj) { snprintf(p->error, sizeof p->error, "no JSON plan found"); return 0; }
    const char *goal = jx_str(obj, "goal", NULL);
    cJSON *steps = jx_arr(obj, "steps");
    int rc = plan_from_steps_array(p, steps, goal);
    if (!rc) {
        /* fallback simpler format: a bare array of strings under "plan" or top-level */
        cJSON *arr = jx_arr(obj, "plan");
        if (arr && cJSON_IsArray(arr)) {
            int n = cJSON_GetArraySize(arr);
            if (n > 64) n = 64;
            plan_free(p);
            p->steps = (PlanStep *)calloc((size_t)n, sizeof(PlanStep));
            if (p->steps) {
                int idx = 0; cJSON *it;
                cJSON_ArrayForEach(it, arr) {
                    if (idx >= n) break;
                    if (cJSON_IsString(it)) {
                        p->steps[idx].id = idx + 1;
                        p->steps[idx].description = strdup(it->valuestring);
                        p->steps[idx].tool = strdup("");
                        idx++;
                    }
                }
                p->n_steps = idx;
                if (idx > 0) rc = 1;
            }
        }
    }
    cJSON_Delete(obj);
    return rc;
}

char *plan_to_json(const Plan *p)
{
    cJSON *root = cJSON_CreateObject();
    if (p->goal) cJSON_AddStringToObject(root, "goal", p->goal);
    cJSON *arr = cJSON_AddArrayToObject(root, "steps");
    for (int i = 0; i < p->n_steps; i++) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddNumberToObject(s, "id", p->steps[i].id);
        if (p->steps[i].description) cJSON_AddStringToObject(s, "description", p->steps[i].description);
        if (p->steps[i].tool) cJSON_AddStringToObject(s, "tool", p->steps[i].tool);
        if (p->steps[i].expected_result) cJSON_AddStringToObject(s, "expected_result", p->steps[i].expected_result);
        if (p->steps[i].verification) cJSON_AddStringToObject(s, "verification", p->steps[i].verification);
        cJSON_AddBoolToObject(s, "done", p->steps[i].done);
        cJSON_AddItemToArray(arr, s);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

void plan_parse_verification(const char *spec, const char **kind, const char **arg,
                             char *scratch, int scratchsz)
{
    *kind = NULL; *arg = NULL;
    if (!spec || !spec[0]) return;
    snprintf(scratch, (size_t)scratchsz, "%s", spec);
    char *colon = strchr(scratch, ':');
    if (colon) { *colon = 0; *arg = colon + 1; }
    *kind = scratch;
}
