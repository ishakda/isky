/* SPDX-License-Identifier: Apache-2.0 */
#include "state.h"
#include "../util/log.h"

const char *agent_state_name(AgentState s)
{
    switch (s) {
    case AGENT_IDLE:         return "IDLE";
    case AGENT_ANALYZING:    return "ANALYZING";
    case AGENT_PLANNING:     return "PLANNING";
    case AGENT_EXECUTING:    return "EXECUTING";
    case AGENT_OBSERVING:    return "OBSERVING";
    case AGENT_VERIFYING:    return "VERIFYING";
    case AGENT_RECOVERING:   return "RECOVERING";
    case AGENT_COMPLETED:    return "COMPLETED";
    case AGENT_FAILED:       return "FAILED";
    case AGENT_WAITING_USER: return "WAITING_USER";
    }
    return "?";
}

int agent_state_is_terminal(AgentState s)
{
    return s == AGENT_COMPLETED || s == AGENT_FAILED;
}

/* Adjacency: which transitions the loop is allowed to make. This is a safety net
 * (a bug that jumps COMPLETED->EXECUTING is caught), deliberately permissive
 * enough for the real control flow. */
static int allowed(AgentState a, AgentState b)
{
    if (a == b) return 1;                      /* self-loop (re-entering a state) */
    if (agent_state_is_terminal(a)) return 0;  /* terminal is sticky */
    switch (a) {
    case AGENT_IDLE:
        return b == AGENT_ANALYZING || b == AGENT_FAILED;
    case AGENT_ANALYZING:
        return b == AGENT_PLANNING || b == AGENT_EXECUTING ||
               b == AGENT_WAITING_USER || b == AGENT_COMPLETED || b == AGENT_FAILED;
    case AGENT_PLANNING:
        return b == AGENT_EXECUTING || b == AGENT_WAITING_USER ||
               b == AGENT_FAILED || b == AGENT_COMPLETED;
    case AGENT_EXECUTING:
        /* VERIFYING/COMPLETED are reachable directly: a `final` action proposed
         * from EXECUTING goes straight to final verification without an
         * intervening tool OBSERVING step. */
        return b == AGENT_OBSERVING || b == AGENT_WAITING_USER ||
               b == AGENT_VERIFYING || b == AGENT_COMPLETED || b == AGENT_FAILED;
    case AGENT_OBSERVING:
        return b == AGENT_EXECUTING || b == AGENT_VERIFYING ||
               b == AGENT_RECOVERING || b == AGENT_PLANNING ||
               b == AGENT_COMPLETED || b == AGENT_FAILED;
    case AGENT_VERIFYING:
        return b == AGENT_COMPLETED || b == AGENT_PLANNING ||
               b == AGENT_EXECUTING || b == AGENT_RECOVERING || b == AGENT_FAILED;
    case AGENT_RECOVERING:
        return b == AGENT_EXECUTING || b == AGENT_PLANNING ||
               b == AGENT_WAITING_USER || b == AGENT_FAILED;
    case AGENT_WAITING_USER:
        return b == AGENT_ANALYZING || b == AGENT_PLANNING ||
               b == AGENT_EXECUTING || b == AGENT_FAILED;
    default:
        return 0;
    }
}

int agent_state_transition(AgentState *cur, AgentState next)
{
    if (!allowed(*cur, next)) {
        LOG_W("state", "illegal transition %s -> %s (ignored)",
              agent_state_name(*cur), agent_state_name(next));
        return 0;
    }
    if (*cur != next)
        LOG_D("state", "%s -> %s", agent_state_name(*cur), agent_state_name(next));
    *cur = next;
    return 1;
}
