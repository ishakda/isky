/* SPDX-License-Identifier: Apache-2.0 */
/* events.h - internal event bus (build prompt §28). CLI and API subscribe. */
#ifndef AGENT_EVENTS_H
#define AGENT_EVENTS_H

typedef enum {
    EV_TASK_CREATED = 0,
    EV_TASK_STARTED,
    EV_PLAN_CREATED,
    EV_STEP_STARTED,
    EV_TOOL_SELECTED,
    EV_TOOL_STARTED,
    EV_TOOL_COMPLETED,
    EV_TOOL_FAILED,
    EV_OBSERVATION_CREATED,
    EV_REFLECTION_STARTED,
    EV_VERIFICATION_STARTED,
    EV_VERIFICATION_PASSED,
    EV_VERIFICATION_FAILED,
    EV_RECOVERY_STARTED,
    EV_TASK_COMPLETED,
    EV_TASK_FAILED,
    EV_USER_INPUT_REQUIRED,
    EV_MODEL_OUTPUT,        /* streamed model text chunk (data = chunk) */
    EV_LOG                  /* free-form status line */
} EventType;

typedef struct {
    EventType   type;
    const char *task_id;
    const char *data;       /* type-specific payload, may be NULL */
    int         step;
    double      ts;
} AgentEvent;

typedef void (*EventHandler)(const AgentEvent *ev, void *userdata);

typedef struct EventBus EventBus;

EventBus *event_bus_create(void);
void      event_bus_destroy(EventBus *b);
int       event_bus_subscribe(EventBus *b, EventHandler h, void *userdata);
void      event_bus_emit(EventBus *b, EventType type, const char *task_id,
                         int step, const char *data);
const char *event_type_name(EventType t);

#endif
