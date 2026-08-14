/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include "protocol.h"
#include "../util/jsonx.h"
#include "../util/log.h"

#include <stdlib.h>
#include <string.h>

const char *action_type_name(ActionType t)
{
    switch (t) {
    case ACT_TOOL:     return "tool";
    case ACT_FINAL:    return "final";
    case ACT_ASK_USER: return "ask_user";
    case ACT_PLAN:     return "plan";
    case ACT_REFLECT:  return "reflect";
    default:           return "invalid";
    }
}

void agent_action_init(AgentAction *a) { memset(a, 0, sizeof *a); }

void agent_action_free(AgentAction *a)
{
    if (!a) return;
    free(a->tool);
    if (a->arguments) cJSON_Delete(a->arguments);
    free(a->answer);
    free(a->question);
    free(a->message);
    free(a->expected_result);
    free(a->reason);
    if (a->steps) cJSON_Delete(a->steps);
    free(a->raw);
    memset(a, 0, sizeof *a);
}

static ActionType type_of(const char *s)
{
    if (!s) return ACT_INVALID;
    if (!strcmp(s, "tool"))     return ACT_TOOL;
    if (!strcmp(s, "final"))    return ACT_FINAL;
    if (!strcmp(s, "ask_user")) return ACT_ASK_USER;
    if (!strcmp(s, "plan"))     return ACT_PLAN;
    if (!strcmp(s, "reflect"))  return ACT_REFLECT;
    return ACT_INVALID;
}

static void fill_from_obj(AgentAction *a, cJSON *obj)
{
    const char *act = jx_str(obj, "action", NULL);
    a->type = type_of(act);
    a->reason = jx_strdup(obj, "reason");
    a->expected_result = jx_strdup(obj, "expected_result");

    switch (a->type) {
    case ACT_TOOL: {
        a->tool = jx_strdup(obj, "tool");
        /* accept "arguments" or "input" (build prompt uses both spellings) */
        cJSON *args = jx_obj(obj, "arguments");
        if (!args) args = jx_obj(obj, "input");
        if (args) a->arguments = cJSON_Duplicate(args, 1);
        else a->arguments = cJSON_CreateObject();
        break;
    }
    case ACT_FINAL:
        a->answer = jx_strdup(obj, "answer");
        if (!a->answer) a->answer = jx_strdup(obj, "message");
        break;
    case ACT_ASK_USER:
        a->question = jx_strdup(obj, "question");
        break;
    case ACT_REFLECT:
        a->message = jx_strdup(obj, "message");
        break;
    case ACT_PLAN: {
        cJSON *steps = jx_arr(obj, "steps");
        if (steps) a->steps = cJSON_Duplicate(steps, 1);
        break;
    }
    default:
        snprintf(a->error, sizeof a->error, "unknown or missing action '%s'",
                 act ? act : "(none)");
        break;
    }
}

ActionType agent_action_parse(const char *model_text, AgentAction *out)
{
    agent_action_init(out);
    if (!model_text) {
        snprintf(out->error, sizeof out->error, "empty model output");
        return ACT_INVALID;
    }
    const char *start = NULL, *end = NULL;
    cJSON *obj = jx_extract_object(model_text, &start, &end);
    if (!obj) {
        obj = jx_repair_object(model_text);
        if (obj) LOG_D("protocol", "action recovered via JSON repair");
    }
    if (!obj) {
        snprintf(out->error, sizeof out->error,
                 "no valid JSON action object found in model output");
        return ACT_INVALID;
    }
    if (start && end) {
        size_t n = (size_t)(end - start);
        out->raw = (char *)malloc(n + 1);
        if (out->raw) { memcpy(out->raw, start, n); out->raw[n] = 0; }
    } else {
        out->raw = cJSON_PrintUnformatted(obj);
    }
    fill_from_obj(out, obj);
    cJSON_Delete(obj);
    return out->type;
}

int agent_action_validate(AgentAction *a, const char *const *tool_names)
{
    if (a->type == ACT_INVALID) {
        if (!a->error[0]) snprintf(a->error, sizeof a->error, "invalid action");
        return 0;
    }
    if (a->type == ACT_TOOL) {
        if (!a->tool || !a->tool[0]) {
            snprintf(a->error, sizeof a->error, "tool action missing 'tool' name");
            return 0;
        }
        if (tool_names) {
            int found = 0;
            for (int i = 0; tool_names[i]; i++)
                if (!strcmp(tool_names[i], a->tool)) { found = 1; break; }
            if (!found) {
                snprintf(a->error, sizeof a->error, "unknown tool '%s'", a->tool);
                return 0;
            }
        }
        if (!a->arguments || !cJSON_IsObject(a->arguments)) {
            snprintf(a->error, sizeof a->error, "tool action arguments must be an object");
            return 0;
        }
        return 1;
    }
    if (a->type == ACT_FINAL) {
        if (!a->answer) {
            snprintf(a->error, sizeof a->error, "final action missing 'answer'");
            return 0;
        }
        return 1;
    }
    if (a->type == ACT_ASK_USER) {
        if (!a->question || !a->question[0]) {
            snprintf(a->error, sizeof a->error, "ask_user action missing 'question'");
            return 0;
        }
        return 1;
    }
    if (a->type == ACT_PLAN) {
        if (!a->steps || !cJSON_IsArray(a->steps) || cJSON_GetArraySize(a->steps) == 0) {
            snprintf(a->error, sizeof a->error, "plan action needs a non-empty 'steps' array");
            return 0;
        }
        return 1;
    }
    if (a->type == ACT_REFLECT) return 1;   /* message optional */
    return 0;
}
