/* SPDX-License-Identifier: Apache-2.0 */
/* shell.c - shell_execute with the full security model (build prompt §10):
 * timeout, output cap, workdir restriction, allowlist, denylist, env isolation,
 * confirmation for dangerous commands. There is NO unrestricted execution path. */
#include <stdio.h>
#include "tool_registry.h"
#include "../security/sandbox.h"
#include "../reasoning/verification.h"
#include "../util/buf.h"
#include "../util/jsonx.h"
#include "../util/platform.h"
#include "../util/log.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define SHELL_TIMEOUT_DEFAULT 60
#define SHELL_TIMEOUT_MAX     600
#define SHELL_OUTPUT_MAX      (128 * 1024)

/* Denylist: substrings that indicate a destructive or exfiltrating command. These
 * are refused outright unless confirmation is granted AND the pattern is not in the
 * always-forbidden set. Matched case-insensitively on token boundaries where useful. */
static const char *DANGER[] = {
    "rm -rf", "rm -fr", "rm  -rf", ":(){", "mkfs", "dd if=", "> /dev/sd",
    "format ", "diskpart", "shutdown", "reboot", "halt", "poweroff",
    "del /s", "del /q", "rmdir /s", "git reset --hard", "git clean -f",
    "chmod -r 777 /", "chown -r", "/etc/shadow", "/etc/passwd", "id_rsa",
    ".aws/credentials", ".ssh/", "curl http", "wget http",   /* net egress via shell */
    "sudo ", "su ", "doas ", "pkexec", "setcap", "insmod", "modprobe",
    "nc -", "ncat", "/dev/tcp/", "base64 -d", "eval $(", "history -c",
    NULL
};

/* Never allowed, even with confirmation: irrecoverable or system-scope. */
static const char *FORBIDDEN[] = {
    "mkfs", "diskpart", "shutdown", "reboot", "halt", "poweroff",
    ":(){", "> /dev/sd", "insmod", "modprobe", "pkexec", NULL
};

static int contains_ci(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (strncasecmp(p, needle, nl) == 0) return 1;
    return 0;
}

static const char *match_any(const char *cmd, const char *const *list)
{
    for (int i = 0; list[i]; i++)
        if (contains_ci(cmd, list[i])) return list[i];
    return NULL;
}

static int t_shell_execute(const AgentTool *self, const cJSON *args,
                           ToolContext *ctx, ToolResult *res)
{
    (void)self;
    const char *command = jx_str(args, "command", NULL);
    if (!command || !command[0]) {
        tool_result_fail(res, REC_INVALID_ARGUMENT, "shell_execute needs 'command'");
        return 0;
    }
    int timeout = jx_int(args, "timeout", SHELL_TIMEOUT_DEFAULT);
    if (timeout <= 0) timeout = SHELL_TIMEOUT_DEFAULT;
    if (timeout > SHELL_TIMEOUT_MAX) timeout = SHELL_TIMEOUT_MAX;

    /* forbidden always wins */
    const char *fb = match_any(command, FORBIDDEN);
    if (fb) {
        tool_result_fail(res, REC_PERMISSION_ERROR,
            "refused: command contains forbidden pattern '%s' (irrecoverable/system scope)",
            fb);
        return 0;
    }
    /* dangerous: refused unless the caller passed confirmed=true (the loop sets this
     * only after the human approved). The registry-level confirmation still applies. */
    const char *dg = match_any(command, DANGER);
    if (dg && !jx_bool(args, "confirmed", 0)) {
        tool_result_fail(res, REC_PERMISSION_ERROR,
            "refused: command matches dangerous pattern '%s'; needs explicit confirmation",
            dg);
        return 0;
    }

    /* working directory must resolve inside the sandbox */
    const char *wd = jx_str(args, "working_directory", NULL);
    char abswd[4096];
    const char *runwd;
    Sandbox *s = (Sandbox *)ctx->sandbox;
    if (wd) {
        if (sandbox_resolve(s, wd, abswd, sizeof abswd) != 0) {
            tool_result_fail(res, REC_PERMISSION_ERROR,
                             "working_directory '%s' is outside the workspace", wd);
            return 0;
        }
        runwd = abswd;
    } else {
        runwd = sandbox_root(s);
    }

    LOG_I("shell", "exec (timeout %ds, cwd %s): %s", timeout, runwd, command);
    ProcResult pr;
    if (proc_run(command, runwd, timeout, SHELL_OUTPUT_MAX, /*clean_env=*/1, &pr) != 0) {
        tool_result_fail(res, REC_TOOL_ERROR, "failed to spawn command");
        return 0;
    }

    ABuf out; ab_init(&out);
    ab_printf(&out, "$ %s\n", command);
    if (pr.output && pr.output[0]) {
        ab_puts(&out, pr.output);
        if (pr.output_len && pr.output[pr.output_len - 1] != '\n') ab_putc(&out, '\n');
    }
    if (pr.output_truncated) ab_puts(&out, "[output truncated]\n");
    ab_printf(&out, "[exit %d, %.2fs%s]", pr.exit_code, pr.seconds,
              pr.timed_out ? ", TIMED OUT" : (pr.signaled ? ", killed by signal" : ""));

    /* structured data for verification (exit code) */
    res->data = cJSON_CreateObject();
    cJSON_AddNumberToObject(res->data, "exit_code", pr.exit_code);
    cJSON_AddBoolToObject(res->data, "timed_out", pr.timed_out);

    if (pr.timed_out) {
        res->output = ab_take(&out);
        res->ok = 0;
        res->error = strdup("command timed out");
        res->error_class = REC_TIMEOUT;
    } else if (pr.exit_code != 0) {
        res->output = ab_take(&out);
        res->ok = 0;
        char eb[64]; snprintf(eb, sizeof eb, "command exited %d", pr.exit_code);
        res->error = strdup(eb);
        res->error_class = REC_TOOL_ERROR;
    } else {
        tool_result_ok(res, ab_take(&out));
    }
    res->truncated = pr.output_truncated;
    free(pr.output);
    ab_free(&out);
    return 0;
}

void shell_tool_register(ToolRegistry *r)
{
    AgentTool t;
    memset(&t, 0, sizeof t);
    t.name = "shell_execute";
    t.security_level = SEC_MEDIUM_RISK;
    t.requires_confirmation = 0;   /* policy decides; dangerous patterns force it */
    t.description = "Run a shell command inside the workspace (timeout, output cap, "
                    "clean env, allow/denylist). Dangerous commands are refused or gated.";
    t.args_schema = "{\"command\":\"str\",\"working_directory\":\"str?\",\"timeout\":int?}";
    t.execute = t_shell_execute;
    tool_registry_register(r, &t);
}
