/* SPDX-License-Identifier: Apache-2.0 */
/* config.h - runtime configuration (build prompt §30). Loaded from config/agent.json;
 * every field has a safe default so a missing/partial file still yields a usable agent.
 * Nothing here is hardcoded at a call site. */
#ifndef AGENT_CONFIG_H
#define AGENT_CONFIG_H

typedef enum { APPROVAL_ALWAYS = 0, APPROVAL_RISKY, APPROVAL_NEVER } ApprovalMode;

typedef struct {
    /* model */
    char  backend[32];        /* "k3" | "mock" */
    char  model_dir[1024];
    char  tok_dir[1024];
    char  cfg_path[1024];
    char  trunk_dir[1024];
    double trunk_gb;
    double cache_gb;
    int    n_layers;
    int    max_tokens;        /* per model call */
    float  temperature;
    float  top_p;

    /* agent loop */
    int    max_iterations;
    int    max_retries;
    int    max_reflections;
    int    max_repair_attempts;   /* coding loop */
    int    autonomy_level;        /* 0..4 */

    /* memory */
    int    memory_enabled;
    char   database[1024];

    /* security */
    int    sandbox;
    char   workspace[1024];
    int    confirm_dangerous_tools;
    ApprovalMode approval;

    /* prompts */
    char   system_prompt_path[1024];

    /* logging */
    char   log_level[16];
    char   log_file[1024];

    /* api */
    int    api_enabled;
    int    api_port;
} AgentConfig;

void agent_config_defaults(AgentConfig *c);
/* Load and overlay from a JSON file. Missing file => defaults + return 0 (not an
 * error). Malformed file => return -1. */
int  agent_config_load(AgentConfig *c, const char *path);
ApprovalMode agent_config_parse_approval(const char *s);
const char  *agent_config_approval_name(ApprovalMode m);

#endif
