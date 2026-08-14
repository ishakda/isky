/* SPDX-License-Identifier: Apache-2.0 */
/* main.c - k3-agent CLI (build prompt §25,§26,§41,§47,§48,§49).
 *
 * Interactive:      k3-agent
 * One-shot:         k3-agent --task "..."
 * From file:        k3-agent --task-file task.txt
 * Resume:           k3-agent --resume TASK_ID
 * Plan only:        k3-agent --plan "..."
 * Dry run:          k3-agent --dry-run "..."
 * Review a project: k3-agent review ./path
 * API server:       k3-agent --serve [--port N]
 *
 * Backend defaults to config; --backend mock (+ optional --mock-script FILE) runs the
 * whole stack deterministically with no model. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/agent.h"
#include "core/agent_loop.h"
#include "core/config.h"
#include "model/mock_backend.h"
#include "model/k3_backend.h"
#include "api/server.h"
#include "util/log.h"
#include "util/buf.h"

static LogLevel parse_level(const char *s)
{
    if (!s) return LOG_INFO;
    if (!strcasecmp(s, "TRACE")) return LOG_TRACE;
    if (!strcasecmp(s, "DEBUG")) return LOG_DEBUG;
    if (!strcasecmp(s, "INFO"))  return LOG_INFO;
    if (!strcasecmp(s, "WARN"))  return LOG_WARN;
    if (!strcasecmp(s, "ERROR")) return LOG_ERROR;
    return LOG_INFO;
}

/* ---- event printing (streaming trace to the console) ---- */
static void on_event(const AgentEvent *ev, void *ud)
{
    (void)ud;
    switch (ev->type) {
    case EV_PLAN_CREATED:
        printf("\n\033[36mAgent > Plan created.\033[0m\n"); break;
    case EV_STEP_STARTED:
        printf("\033[90m  · step %d\033[0m\n", ev->step + 1); break;
    case EV_TOOL_SELECTED:
        printf("\033[33m  → tool: %s\033[0m\n", ev->data ? ev->data : ""); break;
    case EV_TOOL_FAILED:
        printf("\033[31m  ✗ tool failed: %s\033[0m\n", ev->data ? ev->data : ""); break;
    case EV_VERIFICATION_PASSED:
        printf("\033[32m  ✓ verified: %s\033[0m\n", ev->data ? ev->data : ""); break;
    case EV_VERIFICATION_FAILED:
        printf("\033[31m  ✗ verification failed: %s\033[0m\n", ev->data ? ev->data : ""); break;
    case EV_RECOVERY_STARTED:
        printf("\033[35m  ↻ recovery: %s\033[0m\n", ev->data ? ev->data : ""); break;
    case EV_USER_INPUT_REQUIRED:
        printf("\033[36m  ? %s\033[0m\n", ev->data ? ev->data : ""); break;
    default: break;
    }
    fflush(stdout);
}

/* ---- confirmation hook ---- */
static int confirm_hook(const char *tool, SecurityLevel lvl, const char *summary, void *ud)
{
    ApprovalMode mode = *(ApprovalMode *)ud;
    if (mode == APPROVAL_NEVER) return 1;
    printf("\n\033[1;33mCONFIRM\033[0m %s [%s]: %s\nAllow? [y/N] ",
           tool, security_level_name(lvl), summary);
    fflush(stdout);
    char line[16];
    if (!fgets(line, sizeof line, stdin)) return 0;
    return line[0] == 'y' || line[0] == 'Y';
}

static ModelBackend *make_backend(const AgentConfig *cfg, const char *mock_script)
{
    if (!strcmp(cfg->backend, "mock")) {
        ModelBackend *b = mock_backend_create();
        if (b && mock_script) {
            if (mock_backend_load_script(b, mock_script) != 0)
                LOG_W("main", "could not load mock script %s", mock_script);
        }
        return b;
    }
    K3EngineOpts o;
    memset(&o, 0, sizeof o);
    o.model_dir = cfg->model_dir;
    o.tok_dir = cfg->tok_dir[0] ? cfg->tok_dir : NULL;
    o.cfg_path = cfg->cfg_path[0] ? cfg->cfg_path : NULL;
    o.trunk_dir = cfg->trunk_dir[0] ? cfg->trunk_dir : NULL;
    o.trunk_gb = cfg->trunk_gb;
    o.cache_gb = cfg->cache_gb;
    o.n_layers = cfg->n_layers;
    o.max_gen_default = cfg->max_tokens;
    return k3_backend_create(&o);
}

static void print_outcome(AgentTask *t, RunOutcome o)
{
    printf("\n");
    if (o == RUN_COMPLETED)
        printf("\033[1;32mAgent >\033[0m %s\n", t->final_answer ? t->final_answer : "(done)");
    else if (o == RUN_NEEDS_USER)
        printf("\033[1;36mAgent needs input >\033[0m %s\n", t->user_question ? t->user_question : "");
    else
        printf("\033[1;31mAgent failed >\033[0m %s\n", t->final_answer ? t->final_answer : "(failed)");
    printf("  (task %s, state %s, %d iterations)\n",
           t->id, agent_state_name(t->state), t->iteration);
}

static void run_one(Agent *a, const char *goal, RunOptions *opt)
{
    AgentTask *t = task_create(goal, a->cfg.max_iterations);
    RunOutcome o = agent_loop_run(a, t, opt);
    print_outcome(t, o);
    /* interactive resume loop for WAITING_USER */
    while (o == RUN_NEEDS_USER) {
        printf("You (answer) > "); fflush(stdout);
        char line[4096];
        if (!fgets(line, sizeof line, stdin)) break;
        line[strcspn(line, "\n")] = 0;
        o = agent_loop_resume(a, t, line, opt);
        print_outcome(t, o);
    }
    task_free(t);
}

static void usage(FILE *f)
{
    fprintf(f,
    "isky - autonomous agent on the Kimi K3 engine\n\n"
    "  isky [--config F] [--backend k3|mock] [--mock-script F]\n"
    "           [--task \"goal\" | --task-file F | --resume ID | review DIR]\n"
    "           [--plan \"goal\"] [--dry-run \"goal\"]\n"
    "           [--approval always|risky|never] [--autonomy 0..4]\n"
    "           [--workspace DIR] [--serve [--port N]] [--log LEVEL]\n\n"
    "  No task args => interactive REPL. Slash commands: /help /status /tasks\n"
    "  /memory /tools /plan /permissions /model /config /reset /exit\n");
}

static void repl(Agent *a, RunOptions *opt)
{
    printf("\nisky v1.0 — autonomous agent on Kimi K3  (backend=%s, autonomy=%d, approval=%s)\n"
           "Type a goal, or /help. /exit to quit.\n\n",
           a->backend->name, a->cfg.autonomy_level,
           agent_config_approval_name(a->cfg.approval));
    char line[8192];
    for (;;) {
        printf("\033[1mYou >\033[0m "); fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        line[strcspn(line, "\n")] = 0;
        if (!line[0]) continue;
        if (line[0] == '/') {
            if (!strcmp(line, "/exit") || !strcmp(line, "/quit")) break;
            else if (!strcmp(line, "/help"))
                printf("Commands: /status /tasks /memory /tools /plan /permissions "
                       "/model /config /reset /exit\n");
            else if (!strcmp(line, "/tools")) {
                ToolContext ctx; agent_tool_context(a, &ctx, NULL);
                char *d = tool_registry_describe(a->tools, &ctx);
                printf("%s", d); free(d);
            }
            else if (!strcmp(line, "/tasks")) {
                if (a->db) {
                    cJSON *arr = db_list_tasks(a->db, 20);
                    char *js = cJSON_Print(arr);
                    printf("%s\n", js); free(js); cJSON_Delete(arr);
                } else printf("(memory/database disabled)\n");
            }
            else if (!strcmp(line, "/memory")) {
                if (a->db) {
                    cJSON *arr = db_search_memories(a->db, NULL, NULL, 20);
                    char *js = cJSON_Print(arr);
                    printf("%s\n", js); free(js); cJSON_Delete(arr);
                } else printf("(memory disabled)\n");
            }
            else if (!strcmp(line, "/permissions"))
                printf("approval=%s autonomy=%d\n",
                       agent_config_approval_name(a->cfg.approval), a->cfg.autonomy_level);
            else if (!strcmp(line, "/model"))
                printf("backend=%s ctx=%d max_tokens=%d temp=%.2f\n",
                       a->backend->name, a->backend->context_window,
                       a->cfg.max_tokens, a->cfg.temperature);
            else if (!strcmp(line, "/config"))
                printf("workspace=%s db=%s sandbox=%d\n",
                       a->cfg.workspace, a->cfg.database, a->cfg.sandbox);
            else if (!strcmp(line, "/status") || !strcmp(line, "/plan") ||
                     !strcmp(line, "/reset"))
                printf("(no active task in REPL; each goal runs to completion)\n");
            else printf("unknown command '%s' (/help)\n", line);
            continue;
        }
        run_one(a, line, opt);
    }
}

int main(int argc, char **argv)
{
    const char *config_path = "config/agent.json";
    const char *task = NULL, *task_file = NULL, *resume_id = NULL;
    const char *review_dir = NULL, *mock_script = NULL;
    const char *backend_override = NULL, *approval_override = NULL;
    const char *workspace_override = NULL, *log_override = NULL;
    int plan_only = 0, dry_run = 0, serve = 0, port_override = 0, autonomy_override = -1;

    for (int i = 1; i < argc; i++) {
        const char *v = argv[i];
        if (!strcmp(v, "--help") || !strcmp(v, "-h")) { usage(stdout); return 0; }
        else if (!strcmp(v, "--config") && i + 1 < argc) config_path = argv[++i];
        else if (!strcmp(v, "--task") && i + 1 < argc) task = argv[++i];
        else if (!strcmp(v, "--task-file") && i + 1 < argc) task_file = argv[++i];
        else if (!strcmp(v, "--resume") && i + 1 < argc) resume_id = argv[++i];
        else if (!strcmp(v, "--plan") && i + 1 < argc) { task = argv[++i]; plan_only = 1; }
        else if (!strcmp(v, "--dry-run") && i + 1 < argc) { task = argv[++i]; dry_run = 1; }
        else if (!strcmp(v, "review") && i + 1 < argc) review_dir = argv[++i];
        else if (!strcmp(v, "--backend") && i + 1 < argc) backend_override = argv[++i];
        else if (!strcmp(v, "--mock-script") && i + 1 < argc) mock_script = argv[++i];
        else if (!strcmp(v, "--approval") && i + 1 < argc) approval_override = argv[++i];
        else if (!strcmp(v, "--autonomy") && i + 1 < argc) autonomy_override = atoi(argv[++i]);
        else if (!strcmp(v, "--workspace") && i + 1 < argc) workspace_override = argv[++i];
        else if (!strcmp(v, "--serve")) serve = 1;
        else if (!strcmp(v, "--port") && i + 1 < argc) port_override = atoi(argv[++i]);
        else if (!strcmp(v, "--log") && i + 1 < argc) log_override = argv[++i];
        else { fprintf(stderr, "unknown option %s\n\n", v); usage(stderr); return 2; }
    }

    AgentConfig cfg;
    if (agent_config_load(&cfg, config_path) != 0) {
        fprintf(stderr, "failed to load config %s\n", config_path);
        return 2;
    }
    if (backend_override)  snprintf(cfg.backend, sizeof cfg.backend, "%s", backend_override);
    if (approval_override) cfg.approval = agent_config_parse_approval(approval_override);
    if (autonomy_override >= 0) cfg.autonomy_level = autonomy_override;
    if (workspace_override) snprintf(cfg.workspace, sizeof cfg.workspace, "%s", workspace_override);
    if (log_override) snprintf(cfg.log_level, sizeof cfg.log_level, "%s", log_override);

    log_set_level(parse_level(cfg.log_level));
    if (cfg.log_file[0]) log_set_file(cfg.log_file);

    ModelBackend *backend = make_backend(&cfg, mock_script);
    if (!backend) { fprintf(stderr, "cannot create backend '%s'\n", cfg.backend); return 1; }
    if (backend->initialize && backend->initialize(backend) != 0) {
        fprintf(stderr, "backend '%s' failed to initialize\n", cfg.backend);
        return 1;
    }

    Agent *a = agent_create(&cfg, backend);
    if (!a) { fprintf(stderr, "agent creation failed\n"); return 1; }
    static ApprovalMode approval_for_hook;
    approval_for_hook = cfg.approval;
    a->confirm = confirm_hook;
    a->confirm_ud = &approval_for_hook;
    agent_subscribe(a, on_event, NULL);

    RunOptions opt; memset(&opt, 0, sizeof opt);
    opt.plan_only = plan_only;
    opt.dry_run = dry_run;

    int rc = 0;
    if (serve) {
        ApiServer *s = api_server_create(a, port_override ? port_override : cfg.api_port);
        if (!s) { fprintf(stderr, "cannot start API server\n"); rc = 1; }
        else { api_server_run(s); api_server_destroy(s); }
    } else if (resume_id) {
        if (!a->db) { fprintf(stderr, "resume needs the database enabled\n"); rc = 1; }
        else {
            AgentTask t; memset(&t, 0, sizeof t);
            if (db_load_task(a->db, resume_id, &t) == 1) {
                printf("resuming %s (state %s)\n", t.id, agent_state_name(t.state));
                RunOutcome o = agent_loop_resume(a, &t, "(resumed)", &opt);
                print_outcome(&t, o);
                free(t.goal); free(t.plan); free(t.user_question); free(t.final_answer);
            } else { fprintf(stderr, "task %s not found\n", resume_id); rc = 1; }
        }
    } else if (review_dir) {
        char goal[2048];
        snprintf(goal, sizeof goal,
            "Review the project at %s. Inspect architecture, look for bugs, security "
            "issues, performance and maintainability concerns, and report structured "
            "findings graded CRITICAL/HIGH/MEDIUM/LOW/INFO.", review_dir);
        run_one(a, goal, &opt);
    } else if (task) {
        run_one(a, task, &opt);
    } else if (task_file) {
        size_t len = 0;
        char *g = read_entire_file(task_file, 1 << 20, &len, NULL);
        if (!g) { fprintf(stderr, "cannot read task file %s\n", task_file); rc = 1; }
        else { run_one(a, g, &opt); free(g); }
    } else {
        repl(a, &opt);
    }

    agent_destroy(a);
    if (!strcmp(cfg.backend, "mock")) mock_backend_destroy(backend);
    else k3_backend_destroy(backend);
    return rc;
}
