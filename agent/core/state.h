/* SPDX-License-Identifier: Apache-2.0 */
/* state.h - the agent's explicit state machine (build prompt §5). */
#ifndef AGENT_STATE_H
#define AGENT_STATE_H

typedef enum {
    AGENT_IDLE = 0,
    AGENT_ANALYZING,
    AGENT_PLANNING,
    AGENT_EXECUTING,
    AGENT_OBSERVING,
    AGENT_VERIFYING,
    AGENT_RECOVERING,
    AGENT_COMPLETED,
    AGENT_FAILED,
    AGENT_WAITING_USER
} AgentState;

const char *agent_state_name(AgentState s);
int         agent_state_is_terminal(AgentState s);
/* Guarded transition: returns 1 if allowed and applies it, 0 if illegal
 * (logged). Keeps the machine honest and makes illegal jumps a test failure. */
int         agent_state_transition(AgentState *cur, AgentState next);

#endif
