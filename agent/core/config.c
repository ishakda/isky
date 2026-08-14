/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include "config.h"
#include "../util/buf.h"
#include "../util/jsonx.h"
#include "../util/log.h"

#include <stdlib.h>
#include <string.h>

static void cpy(char *dst, size_t n, const char *src)
{
    if (!src) { dst[0] = 0; return; }
    snprintf(dst, n, "%s", src);
}

void agent_config_defaults(AgentConfig *c)
{
    memset(c, 0, sizeof *c);
    cpy(c->backend, sizeof c->backend, "k3");
    cpy(c->model_dir, sizeof c->model_dir, "./model");
    c->trunk_gb = 0.0;
    c->cache_gb = 0.5;
    c->n_layers = 0;
    c->max_tokens = 1024;
    c->temperature = 0.2f;
    c->top_p = 0.95f;

    c->max_iterations = 30;
    c->max_retries = 3;
    c->max_reflections = 2;
    c->max_repair_attempts = 5;
    c->autonomy_level = 3;

    c->memory_enabled = 1;
    cpy(c->database, sizeof c->database, "agent.db");

    c->sandbox = 1;
    cpy(c->workspace, sizeof c->workspace, "./workspace");
    c->confirm_dangerous_tools = 1;
    c->approval = APPROVAL_RISKY;

    cpy(c->system_prompt_path, sizeof c->system_prompt_path,
        AGENT_PROMPT_DIR "/system.txt");

    cpy(c->log_level, sizeof c->log_level, "INFO");
    c->log_file[0] = 0;

    c->api_enabled = 0;
    c->api_port = 8080;
}

ApprovalMode agent_config_parse_approval(const char *s)
{
    if (!s) return APPROVAL_RISKY;
    if (!strcmp(s, "always")) return APPROVAL_ALWAYS;
    if (!strcmp(s, "never"))  return APPROVAL_NEVER;
    return APPROVAL_RISKY;
}

const char *agent_config_approval_name(ApprovalMode m)
{
    switch (m) {
    case APPROVAL_ALWAYS: return "always";
    case APPROVAL_NEVER:  return "never";
    default:              return "risky";
    }
}

int agent_config_load(AgentConfig *c, const char *path)
{
    agent_config_defaults(c);
    if (!path) return 0;
    size_t len = 0;
    char *data = read_entire_file(path, 1 << 20, &len, NULL);
    if (!data) {
        LOG_D("config", "no config file at %s; using defaults", path);
        return 0;
    }
    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!root) { LOG_E("config", "malformed JSON in %s", path); return -1; }

    cJSON *m = jx_obj(root, "model");
    if (m) {
        cpy(c->backend, sizeof c->backend, jx_str(m, "backend", c->backend));
        cpy(c->model_dir, sizeof c->model_dir, jx_str(m, "model_dir", c->model_dir));
        cpy(c->tok_dir, sizeof c->tok_dir, jx_str(m, "tok_dir", c->tok_dir));
        cpy(c->cfg_path, sizeof c->cfg_path, jx_str(m, "config_path", c->cfg_path));
        cpy(c->trunk_dir, sizeof c->trunk_dir, jx_str(m, "trunk_dir", c->trunk_dir));
        c->trunk_gb = jx_num(m, "trunk_gb", c->trunk_gb);
        c->cache_gb = jx_num(m, "cache_gb", c->cache_gb);
        c->n_layers = jx_int(m, "layers", c->n_layers);
        c->max_tokens = jx_int(m, "max_tokens", c->max_tokens);
        c->temperature = (float)jx_num(m, "temperature", c->temperature);
        c->top_p = (float)jx_num(m, "top_p", c->top_p);
    }
    cJSON *a = jx_obj(root, "agent");
    if (a) {
        c->max_iterations = jx_int(a, "max_iterations", c->max_iterations);
        c->max_retries = jx_int(a, "max_retries", c->max_retries);
        c->max_reflections = jx_int(a, "max_reflections", c->max_reflections);
        c->max_repair_attempts = jx_int(a, "max_repair_attempts", c->max_repair_attempts);
        c->autonomy_level = jx_int(a, "autonomy_level", c->autonomy_level);
    }
    cJSON *mem = jx_obj(root, "memory");
    if (mem) {
        c->memory_enabled = jx_bool(mem, "enabled", c->memory_enabled);
        cpy(c->database, sizeof c->database, jx_str(mem, "database", c->database));
    }
    cJSON *s = jx_obj(root, "security");
    if (s) {
        c->sandbox = jx_bool(s, "sandbox", c->sandbox);
        cpy(c->workspace, sizeof c->workspace, jx_str(s, "workspace", c->workspace));
        c->confirm_dangerous_tools =
            jx_bool(s, "confirm_dangerous_tools", c->confirm_dangerous_tools);
        c->approval = agent_config_parse_approval(
            jx_str(s, "approval", agent_config_approval_name(c->approval)));
    }
    cJSON *p = jx_obj(root, "prompts");
    if (p)
        cpy(c->system_prompt_path, sizeof c->system_prompt_path,
            jx_str(p, "system", c->system_prompt_path));
    cJSON *lg = jx_obj(root, "logging");
    if (lg) {
        cpy(c->log_level, sizeof c->log_level, jx_str(lg, "level", c->log_level));
        cpy(c->log_file, sizeof c->log_file, jx_str(lg, "file", c->log_file));
    }
    cJSON *api = jx_obj(root, "api");
    if (api) {
        c->api_enabled = jx_bool(api, "enabled", c->api_enabled);
        c->api_port = jx_int(api, "port", c->api_port);
    }
    cJSON_Delete(root);
    if (c->autonomy_level < 0) c->autonomy_level = 0;
    if (c->autonomy_level > 4) c->autonomy_level = 4;
    return 0;
}
