/* SPDX-License-Identifier: Apache-2.0 */
/* agent_loop.c - the autonomous loop as an explicit state machine.
 *
 * Flow (build prompt §5): analyze -> (plan) -> for each action the model proposes:
 * select tool -> execute -> observe -> (reflect, bounded) -> verify -> recover on
 * failure -> next; then final verification -> final answer. The runtime, not the
 * model, decides whether an action is valid and permitted (§57). */
#include <stdio.h>
#include "agent_loop.h"
#include "../planner/planner.h"
#include "../planner/protocol.h"
#include "../planner/plan_parser.h"
#include "../context/context.h"
#include "../memory/memory.h"
#include "../reasoning/reflection.h"
#include "../reasoning/verification.h"
#include "../util/buf.h"
#include "../util/jsonx.h"
#include "../util/log.h"

#include <stdlib.h>
#include <string.h>

/* forward */
static const char *const *tool_name_list(Agent *a, const char ***owned);

typedef struct {
    Agent      *a;
    AgentTask  *task;
    Memory      mem;
    Plan        plan;
    ToolContext tctx;
    ReflectionBudget refl;
    char       *tools_desc;
    int         last_exit_code;
    char       *last_observation;
    int         step_attempts;    /* failures on the current step */
} LoopCtx;

static void emit(LoopCtx *L, EventType t, const char *data)
{
    event_bus_emit(L->a->events, t, L->task->id, L->task->current_step, data);
    if (L->a->db)
        db_log_trace(L->a->db, L->task->id, L->task->current_step,
                     event_type_name(t), data);
}

static void set_obs(LoopCtx *L, const char *obs)
{
    free(L->last_observation);
    L->last_observation = obs ? strdup(obs) : NULL;
    memory_add_observation(&L->mem, obs);
    emit(L, EV_OBSERVATION_CREATED, obs);
}

/* Ask the model for the next action given the current context. Returns 0 and fills
 * `act` on success; on model/parse failure returns -1 with act->error set. */
static int decide_action(LoopCtx *L, const char *extra_note, AgentAction *act)
{
    Agent *a = L->a;
    char *mem = memory_recall(&L->mem, L->task->goal, 6);
    char *obs = memory_recent_observations(&L->mem, 4000);

    char stepbuf[512]; stepbuf[0] = 0;
    if (L->task->current_step < L->plan.n_steps) {
        PlanStep *s = &L->plan.steps[L->task->current_step];
        snprintf(stepbuf, sizeof stepbuf, "Step %d/%d: %s%s%s",
                 L->task->current_step + 1, L->plan.n_steps,
                 s->description ? s->description : "",
                 s->tool && s->tool[0] ? " | suggested tool: " : "",
                 s->tool && s->tool[0] ? s->tool : "");
    }

    ContextParts cp;
    memset(&cp, 0, sizeof cp);
    cp.system_prompt = a->system_prompt;
    cp.goal = L->task->goal;
    cp.state = agent_state_name(L->task->state);
    cp.plan_json = L->task->plan;
    cp.tools_desc = L->tools_desc;
    cp.memory = mem;
    cp.observations = obs;
    cp.step_desc = stepbuf[0] ? stepbuf : NULL;
    cp.extra = extra_note;

    char *prompt = context_build(a->backend, &cp, a->backend->context_window);
    free(mem); free(obs);

    K3GenerationRequest req;
    memset(&req, 0, sizeof req);
    req.prompt = prompt;
    req.max_tokens = a->cfg.max_tokens;
    req.temperature = a->cfg.temperature;
    req.top_p = a->cfg.top_p;
    K3GenerationResult res;
    int rc = a->backend->generate(a->backend, &req, &res);
    free(prompt);
    if (rc != 0) {
        agent_action_init(act);
        snprintf(act->error, sizeof act->error, "model error: %.230s", res.error);
        act->type = ACT_INVALID;
        model_result_free(&res);
        return -1;
    }
    if (a->db) db_log_message(a->db, L->task->id, "assistant", res.text);

    /* two parse attempts: strict, then repair is already inside agent_action_parse */
    ActionType t = agent_action_parse(res.text, act);
    model_result_free(&res);

    const char **owned = NULL;
    const char *const *names = tool_name_list(a, &owned);
    int valid = agent_action_validate(act, names);
    free(owned);
    if (t == ACT_INVALID || !valid) {
        LOG_W("loop", "invalid action: %s", act->error);
        return -1;
    }
    return 0;
}

/* Execute one validated tool action; fills observation + verification inputs. */
static int run_tool(LoopCtx *L, AgentAction *act, ToolResult *res)
{
    emit(L, EV_TOOL_SELECTED, act->tool);
    /* If the current plan step requested confirmation via a dangerous shell command
     * and the policy is NEVER, mark arguments confirmed. Otherwise the registry gate
     * + tool-level denylist handle it. */
    tool_registry_execute(L->a->tools, act->tool, act->arguments, &L->tctx, res);

    char *argstr = cJSON_PrintUnformatted(act->arguments);
    if (L->a->db)
        db_log_tool_call(L->a->db, L->task->id, L->task->current_step, act->tool,
                         argstr, res->ok, recovery_class_name(res->error_class));
    free(argstr);

    /* capture exit code from structured data if present (shell) */
    if (res->data) {
        cJSON *ec = cJSON_GetObjectItemCaseSensitive(res->data, "exit_code");
        if (cJSON_IsNumber(ec)) L->last_exit_code = ec->valueint;
    }
    return res->ok ? 0 : -1;
}

/* Verify the current step using its plan verification spec, if any. */
static int verify_step(LoopCtx *L, char **reason)
{
    if (L->task->current_step >= L->plan.n_steps) return 1;
    PlanStep *s = &L->plan.steps[L->task->current_step];
    if (!s->verification || !s->verification[0]) return 1;
    emit(L, EV_VERIFICATION_STARTED, s->verification);
    char scratch[512];
    const char *kind, *arg;
    plan_parse_verification(s->verification, &kind, &arg, scratch, sizeof scratch);
    VerifyCheck vc = { kind, arg };
    int ok = verify_check(&vc, L->tctx.workspace, L->last_observation,
                          L->last_exit_code, reason);
    emit(L, ok ? EV_VERIFICATION_PASSED : EV_VERIFICATION_FAILED,
         ok ? s->verification : (reason && *reason ? *reason : "failed"));
    return ok;
}

/* Recovery: classify + choose strategy; returns the strategy and an optional note
 * to feed back to the model. */
static RecoveryStrategy do_recovery(LoopCtx *L, RecoveryClass cls, char **note)
{
    emit(L, EV_RECOVERY_STARTED, recovery_class_name(cls));
    L->step_attempts++;
    RecoveryStrategy strat = recovery_strategy_for2(cls, L->step_attempts,
                                                    L->a->cfg.max_retries,
                                                    L->a->cfg.max_repair_attempts);
    ABuf b; ab_init(&b);
    ab_printf(&b, "The previous action failed (%s). ", recovery_class_name(cls));
    if (L->last_observation) ab_printf(&b, "Observation: %.400s. ", L->last_observation);
    switch (strat) {
    case STRAT_RETRY:   ab_puts(&b, "Retry with a corrected approach."); break;
    case STRAT_REPLAN:  ab_puts(&b, "Choose a DIFFERENT action; do not repeat the failed one."); break;
    case STRAT_ASK_USER:ab_puts(&b, "If you cannot proceed safely, ask the user."); break;
    case STRAT_ABORT:   ab_puts(&b, "Recovery exhausted."); break;
    }
    *note = ab_take(&b);
    return strat;
}

/* The core iteration engine, shared by run and resume. */
static RunOutcome loop_body(LoopCtx *L, const RunOptions *opt, const char *initial_note)
{
    AgentTask *task = L->task;
    char *note = initial_note ? strdup(initial_note) : NULL;

    while (task->iteration < task->max_iterations) {
        task->iteration++;
        emit(L, EV_STEP_STARTED, NULL);
        reflection_reset(&L->refl);

        AgentAction act;
        if (decide_action(L, note, &act) != 0) {
            /* model/parse failure => recovery */
            free(note);
            RecoveryStrategy strat = do_recovery(L, REC_PARSER_ERROR, &note);
            agent_action_free(&act);
            if (strat == STRAT_ABORT) {
                agent_state_transition(&task->state, AGENT_FAILED);
                task_set_answer(task, "The model did not produce a valid action after retries.");
                free(note);
                return RUN_FAILED;
            }
            if (strat == STRAT_ASK_USER) {
                agent_state_transition(&task->state, AGENT_WAITING_USER);
                task_set_question(task, "I could not determine a valid next step. How should I proceed?");
                free(note);
                return RUN_NEEDS_USER;
            }
            continue;
        }
        free(note); note = NULL;

        if (act.type == ACT_FINAL) {
            /* final verification before accepting (build prompt §19) */
            char *reason = NULL;
            agent_state_transition(&task->state, AGENT_VERIFYING);
            int ok = verify_step(L, &reason);
            if (!ok && L->step_attempts < L->a->cfg.max_retries) {
                ABuf b; ab_init(&b);
                ab_printf(&b, "Verification failed before finalizing: %s. "
                          "Continue working; do not claim completion yet.",
                          reason ? reason : "unknown");
                note = ab_take(&b);
                free(reason);
                L->step_attempts++;
                agent_state_transition(&task->state, AGENT_EXECUTING);
                agent_action_free(&act);
                continue;
            }
            free(reason);
            task->success = 1;
            agent_state_transition(&task->state, AGENT_COMPLETED);
            task_set_answer(task, act.answer);
            emit(L, EV_TASK_COMPLETED, act.answer);
            agent_action_free(&act);
            return RUN_COMPLETED;
        }
        if (act.type == ACT_ASK_USER) {
            agent_state_transition(&task->state, AGENT_WAITING_USER);
            task_set_question(task, act.question);
            emit(L, EV_USER_INPUT_REQUIRED, act.question);
            agent_action_free(&act);
            return RUN_NEEDS_USER;
        }
        if (act.type == ACT_REFLECT) {
            if (reflection_allowed(&L->refl)) {
                ABuf b; ab_init(&b);
                ab_printf(&b, "Noted: %s. Now take a concrete action.",
                          act.message ? act.message : "");
                note = ab_take(&b);
            } else {
                note = strdup("Enough reflection. Take a concrete action now.");
            }
            agent_action_free(&act);
            continue;
        }
        if (act.type == ACT_PLAN) {
            /* model chose to (re)plan mid-run */
            Plan np; plan_init(&np);
            if (plan_from_steps_array(&np, act.steps, task->goal)) {
                plan_free(&L->plan);
                L->plan = np;
                char *pj = plan_to_json(&L->plan);
                task_set_plan(task, pj, L->plan.n_steps);
                memory_set_plan(&L->mem, pj);
                free(pj);
                emit(L, EV_PLAN_CREATED, task->plan);
                L->step_attempts = 0;
            } else {
                plan_free(&np);
            }
            agent_action_free(&act);
            agent_state_transition(&task->state, AGENT_EXECUTING);
            continue;
        }

        /* ACT_TOOL */
        if (opt && opt->dry_run) {
            char *args = cJSON_PrintUnformatted(act.arguments);
            ABuf b; ab_init(&b);
            ab_printf(&b, "[dry-run] would execute tool '%s' with %s", act.tool,
                      args ? args : "{}");
            set_obs(L, b.data);
            ab_free(&b);
            free(args);
            /* advance a step and keep going so the user sees the whole plan */
            if (task->current_step < L->plan.n_steps) task->current_step++;
            agent_action_free(&act);
            if (task->current_step >= L->plan.n_steps) {
                task_set_answer(task, "Dry run complete: listed the actions that would run.");
                agent_state_transition(&task->state, AGENT_COMPLETED);
                return RUN_COMPLETED;
            }
            continue;
        }

        agent_state_transition(&task->state, AGENT_EXECUTING);
        ToolResult res;
        run_tool(L, &act, &res);
        agent_state_transition(&task->state, AGENT_OBSERVING);
        set_obs(L, res.ok ? res.output : (res.error ? res.error : "tool failed"));

        if (!res.ok) {
            RecoveryClass cls = (RecoveryClass)res.error_class;
            if (cls == REC_NONE) cls = REC_TOOL_ERROR;
            agent_state_transition(&task->state, AGENT_RECOVERING);
            RecoveryStrategy strat = do_recovery(L, cls, &note);
            memory_record_episode(&L->mem, task->goal, act.tool,
                                  "failed", res.error);
            tool_result_free(&res);
            agent_action_free(&act);
            if (strat == STRAT_ABORT) {
                agent_state_transition(&task->state, AGENT_FAILED);
                task_set_answer(task, note ? note : "Task failed after recovery attempts.");
                emit(L, EV_TASK_FAILED, note);
                free(note);
                return RUN_FAILED;
            }
            if (strat == STRAT_ASK_USER) {
                agent_state_transition(&task->state, AGENT_WAITING_USER);
                task_set_question(task, "I hit an error I can't safely resolve. How should I proceed?");
                free(note);
                return RUN_NEEDS_USER;
            }
            continue;   /* retry / replan with note */
        }

        /* success: verify the step, advance */
        agent_state_transition(&task->state, AGENT_VERIFYING);
        char *reason = NULL;
        int vok = verify_step(L, &reason);
        tool_result_free(&res);
        agent_action_free(&act);
        if (!vok) {
            ABuf b; ab_init(&b);
            ab_printf(&b, "Step verification failed: %s. Adjust and try again.",
                      reason ? reason : "unknown");
            note = ab_take(&b);
            free(reason);
            L->step_attempts++;
            if (L->step_attempts > L->a->cfg.max_retries) {
                /* move on rather than loop forever; the final verification still guards */
                if (task->current_step < L->plan.n_steps) task->current_step++;
                L->step_attempts = 0;
            }
            agent_state_transition(&task->state, AGENT_EXECUTING);
            continue;
        }
        free(reason);

        /* step done */
        if (task->current_step < L->plan.n_steps) {
            L->plan.steps[task->current_step].done = 1;
            task->current_step++;
        }
        L->step_attempts = 0;
        if (L->a->db) db_save_task(L->a->db, task);
    }

    agent_state_transition(&task->state, AGENT_FAILED);
    task_set_answer(task, "Reached the maximum iteration budget without completing.");
    emit(L, EV_TASK_FAILED, "max iterations");
    free(note);
    return RUN_MAX_ITERS;
}

static RunOutcome loop_setup_and_run(Agent *a, AgentTask *task, const RunOptions *opt,
                                     const char *initial_note)
{
    LoopCtx L;
    memset(&L, 0, sizeof L);
    L.a = a;
    L.task = task;
    L.last_exit_code = 0;
    memory_init(&L.mem, a->db, task->id, 16);
    memory_set_goal(&L.mem, task->goal);
    reflection_init(&L.refl, a->cfg.max_reflections);
    agent_tool_context(a, &L.tctx, task->id);
    L.tools_desc = tool_registry_describe(a->tools, &L.tctx);

    emit(&L, EV_TASK_STARTED, task->goal);
    if (a->db) db_save_task(a->db, task);

    /* If the task has no plan yet, plan first (build prompt §5). */
    if (!task->plan) {
        agent_state_transition(&task->state, AGENT_ANALYZING);
        agent_state_transition(&task->state, AGENT_PLANNING);
        char *memc = memory_recall(&L.mem, task->goal, 6);
        Planner pl;
        memset(&pl, 0, sizeof pl);
        pl.backend = a->backend;
        pl.max_attempts = a->cfg.max_retries;
        pl.temperature = a->cfg.temperature;
        pl.max_tokens = a->cfg.max_tokens;
        pl.tools_desc = L.tools_desc;
        planner_make_plan(&pl, task->goal, memc, &L.plan);
        free(memc);
        char *pj = plan_to_json(&L.plan);
        task_set_plan(task, pj, L.plan.n_steps);
        memory_set_plan(&L.mem, pj);
        emit(&L, EV_PLAN_CREATED, pj);
        free(pj);
        if (a->db) db_save_task(a->db, task);
    } else {
        /* resuming: reload plan into L.plan */
        plan_parse_text(&L.plan, task->plan);
    }

    RunOutcome outcome;
    if (opt && opt->plan_only) {
        task_set_answer(task, task->plan ? task->plan : "(no plan)");
        agent_state_transition(&task->state, AGENT_COMPLETED);
        outcome = RUN_COMPLETED;
    } else {
        agent_state_transition(&task->state, AGENT_EXECUTING);
        outcome = loop_body(&L, opt, initial_note);
    }

    /* persist a compact procedure on success (build prompt §13 procedural memory) */
    if (outcome == RUN_COMPLETED && a->db && L.plan.n_steps > 0) {
        char *pj = plan_to_json(&L.plan);
        char pname[128];
        snprintf(pname, sizeof pname, "goal:%.100s", task->goal);
        memory_record_procedure(&L.mem, pname, pj);
        free(pj);
    }
    if (a->db) db_save_task(a->db, task);

    free(L.tools_desc);
    free(L.last_observation);
    plan_free(&L.plan);
    memory_free(&L.mem);
    return outcome;
}

RunOutcome agent_loop_run(Agent *a, AgentTask *task, const RunOptions *opt)
{
    return loop_setup_and_run(a, task, opt, NULL);
}

RunOutcome agent_loop_resume(Agent *a, AgentTask *task, const char *user_answer,
                             const RunOptions *opt)
{
    task->requires_user_input = 0;
    free(task->user_question); task->user_question = NULL;
    ABuf b; ab_init(&b);
    ab_printf(&b, "The user answered: %s. Continue the task using this.",
              user_answer ? user_answer : "(no answer)");
    if (a->db) db_log_message(a->db, task->id, "user", user_answer);
    /* re-enter execution */
    if (agent_state_is_terminal(task->state)) task->state = AGENT_EXECUTING;
    RunOutcome o = loop_setup_and_run(a, task, opt, b.data);
    ab_free(&b);
    return o;
}

/* Build a NULL-terminated array of tool names for validation. */
static const char *const *tool_name_list(Agent *a, const char ***owned)
{
    int n = tool_registry_count(a->tools);
    const char **arr = (const char **)malloc((size_t)(n + 1) * sizeof(char *));
    for (int i = 0; i < n; i++) arr[i] = tool_registry_at(a->tools, i)->name;
    arr[n] = NULL;
    *owned = arr;
    return arr;
}
