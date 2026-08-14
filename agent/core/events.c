/* SPDX-License-Identifier: Apache-2.0 */
#include "events.h"
#include "../util/platform.h"

#include <stdlib.h>
#include <string.h>

#define MAX_SUBS 16

struct EventBus {
    EventHandler handlers[MAX_SUBS];
    void        *userdata[MAX_SUBS];
    int          n;
};

EventBus *event_bus_create(void)
{
    return (EventBus *)calloc(1, sizeof(EventBus));
}

void event_bus_destroy(EventBus *b) { free(b); }

int event_bus_subscribe(EventBus *b, EventHandler h, void *userdata)
{
    if (!b || b->n >= MAX_SUBS) return -1;
    b->handlers[b->n] = h;
    b->userdata[b->n] = userdata;
    b->n++;
    return 0;
}

void event_bus_emit(EventBus *b, EventType type, const char *task_id,
                    int step, const char *data)
{
    if (!b) return;
    AgentEvent ev;
    ev.type = type;
    ev.task_id = task_id;
    ev.data = data;
    ev.step = step;
    ev.ts = mono_seconds();
    for (int i = 0; i < b->n; i++)
        b->handlers[i](&ev, b->userdata[i]);
}

const char *event_type_name(EventType t)
{
    switch (t) {
    case EV_TASK_CREATED:          return "TASK_CREATED";
    case EV_TASK_STARTED:          return "TASK_STARTED";
    case EV_PLAN_CREATED:          return "PLAN_CREATED";
    case EV_STEP_STARTED:          return "STEP_STARTED";
    case EV_TOOL_SELECTED:         return "TOOL_SELECTED";
    case EV_TOOL_STARTED:          return "TOOL_STARTED";
    case EV_TOOL_COMPLETED:        return "TOOL_COMPLETED";
    case EV_TOOL_FAILED:           return "TOOL_FAILED";
    case EV_OBSERVATION_CREATED:   return "OBSERVATION_CREATED";
    case EV_REFLECTION_STARTED:    return "REFLECTION_STARTED";
    case EV_VERIFICATION_STARTED:  return "VERIFICATION_STARTED";
    case EV_VERIFICATION_PASSED:   return "VERIFICATION_PASSED";
    case EV_VERIFICATION_FAILED:   return "VERIFICATION_FAILED";
    case EV_RECOVERY_STARTED:      return "RECOVERY_STARTED";
    case EV_TASK_COMPLETED:        return "TASK_COMPLETED";
    case EV_TASK_FAILED:           return "TASK_FAILED";
    case EV_USER_INPUT_REQUIRED:   return "USER_INPUT_REQUIRED";
    case EV_MODEL_OUTPUT:          return "MODEL_OUTPUT";
    case EV_LOG:                   return "LOG";
    }
    return "?";
}
