/* SPDX-License-Identifier: Apache-2.0 */
/* git.c - read-mostly git integration (build prompt §44). status/diff/log/branch
 * are SAFE reads; commit is LOW_RISK and gated by confirmation. Never pushes. */
#include <stdio.h>
#include "tool_registry.h"
#include "../security/sandbox.h"
#include "../reasoning/verification.h"
#include "../util/buf.h"
#include "../util/jsonx.h"
#include "../util/platform.h"

#include <stdlib.h>
#include <string.h>

static int run_git(ToolContext *ctx, const char *gitargs, ToolResult *res, int is_write)
{
    Sandbox *s = (Sandbox *)ctx->sandbox;
    char cmd[8192];
    /* -C confines git to the workspace; refuse shell metacharacters in args */
    if (strpbrk(gitargs, ";|&`$><\n")) {
        tool_result_fail(res, REC_INVALID_ARGUMENT, "illegal characters in git arguments");
        return 0;
    }
    snprintf(cmd, sizeof cmd, "git -C '%s' %s 2>&1", sandbox_root(s), gitargs);
    ProcResult pr;
    if (proc_run(cmd, sandbox_root(s), 60, 128 * 1024, 1, &pr) != 0) {
        tool_result_fail(res, REC_TOOL_ERROR, "cannot run git");
        return 0;
    }
    ABuf out; ab_init(&out);
    if (pr.output) ab_puts(&out, pr.output);
    ab_printf(&out, "\n[git exit %d]", pr.exit_code);
    if (pr.exit_code != 0) {
        res->output = ab_take(&out);
        res->ok = 0;
        res->error = strdup("git command failed");
        res->error_class = REC_TOOL_ERROR;
    } else {
        tool_result_ok(res, ab_take(&out));
    }
    (void)is_write;
    free(pr.output);
    ab_free(&out);
    return 0;
}

static int t_git_status(const AgentTool *s, const cJSON *a, ToolContext *c, ToolResult *r)
{ (void)s;(void)a; return run_git(c, "status --short --branch", r, 0); }

static int t_git_diff(const AgentTool *s, const cJSON *a, ToolContext *c, ToolResult *r)
{
    (void)s;
    const char *path = jx_str(a, "path", NULL);
    char args[4096];
    if (path && !strpbrk(path, ";|&`$><\n'"))
        snprintf(args, sizeof args, "diff -- '%s'", path);
    else
        snprintf(args, sizeof args, "diff");
    return run_git(c, args, r, 0);
}

static int t_git_log(const AgentTool *s, const cJSON *a, ToolContext *c, ToolResult *r)
{
    (void)s;
    int n = jx_int(a, "count", 10);
    if (n <= 0 || n > 100) n = 10;
    char args[64];
    snprintf(args, sizeof args, "log --oneline -n %d", n);
    return run_git(c, args, r, 0);
}

static int t_git_branch(const AgentTool *s, const cJSON *a, ToolContext *c, ToolResult *r)
{ (void)s;(void)a; return run_git(c, "branch -a", r, 0); }

static int t_git_commit(const AgentTool *s, const cJSON *a, ToolContext *c, ToolResult *r)
{
    (void)s;
    const char *msg = jx_str(a, "message", NULL);
    if (!msg || !msg[0]) {
        tool_result_fail(r, REC_INVALID_ARGUMENT, "git_commit needs 'message'");
        return 0;
    }
    if (strpbrk(msg, "'`$\n")) {
        tool_result_fail(r, REC_INVALID_ARGUMENT, "commit message has illegal characters");
        return 0;
    }
    int add_all = jx_bool(a, "add_all", 1);
    if (add_all) {
        ToolResult tmp; tool_result_init(&tmp);
        run_git(c, "add -A", &tmp, 1);
        int ok = tmp.ok;
        tool_result_free(&tmp);
        if (!ok) { tool_result_fail(r, REC_TOOL_ERROR, "git add failed"); return 0; }
    }
    char args[8192];
    snprintf(args, sizeof args, "commit -m '%s'", msg);
    return run_git(c, args, r, 1);
}

void git_tools_register(ToolRegistry *reg)
{
    AgentTool t;
    struct { const char *n; const char *d; SecurityLevel lvl; int conf;
             int (*fn)(const AgentTool*, const cJSON*, ToolContext*, ToolResult*);
             const char *schema; } defs[] = {
        {"git_status", "Show working-tree status.", SEC_SAFE, 0, t_git_status, "{}"},
        {"git_diff",   "Show unstaged diff (optionally for one path).", SEC_SAFE, 0, t_git_diff, "{\"path\":\"str?\"}"},
        {"git_log",    "Show recent commit log.", SEC_SAFE, 0, t_git_log, "{\"count\":int?}"},
        {"git_branch", "List branches.", SEC_SAFE, 0, t_git_branch, "{}"},
        {"git_commit", "Stage all and commit (never pushes).", SEC_LOW_RISK, 1, t_git_commit,
                       "{\"message\":\"str\",\"add_all\":bool?}"},
    };
    for (size_t i = 0; i < sizeof defs / sizeof defs[0]; i++) {
        memset(&t, 0, sizeof t);
        t.name = defs[i].n; t.description = defs[i].d; t.security_level = defs[i].lvl;
        t.requires_confirmation = defs[i].conf; t.execute = defs[i].fn;
        t.args_schema = defs[i].schema;
        tool_registry_register(reg, &t);
    }
}
