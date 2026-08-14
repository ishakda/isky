/* SPDX-License-Identifier: Apache-2.0 */
#include "permissions.h"
#include "../util/buf.h"
#include "../util/jsonx.h"
#include "../util/log.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char name[64];
    int  enabled;
    int  require_confirmation;
    int  has_override;
} ToolPerm;

struct Permissions {
    ToolPerm     tools[64];
    int          n;
    ApprovalMode approval;
    int          autonomy;
};

Permissions *permissions_create(ApprovalMode approval, int autonomy_level)
{
    Permissions *p = (Permissions *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->approval = approval;
    p->autonomy = autonomy_level;
    return p;
}

void permissions_destroy(Permissions *p) { free(p); }

static ToolPerm *find(Permissions *p, const char *tool)
{
    for (int i = 0; i < p->n; i++)
        if (!strcmp(p->tools[i].name, tool)) return &p->tools[i];
    return NULL;
}

void permissions_set(Permissions *p, const char *tool, int enabled, int require_conf)
{
    ToolPerm *tp = find(p, tool);
    if (!tp && p->n < (int)(sizeof p->tools / sizeof p->tools[0])) {
        tp = &p->tools[p->n++];
        snprintf(tp->name, sizeof tp->name, "%s", tool);
    }
    if (!tp) return;
    tp->enabled = enabled;
    tp->require_confirmation = require_conf;
    tp->has_override = 1;
}

int permissions_load(Permissions *p, const char *path)
{
    if (!path) return 0;
    size_t len = 0;
    char *data = read_entire_file(path, 1 << 20, &len, NULL);
    if (!data) { LOG_D("perm", "no permissions file at %s", path); return 0; }
    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!root) { LOG_E("perm", "malformed permissions JSON %s", path); return -1; }

    /* Optional top-level "approval"/"autonomy_level" too. */
    const char *ap = jx_str(root, "approval", NULL);
    if (ap) p->approval = agent_config_parse_approval(ap);
    if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root, "autonomy_level")))
        p->autonomy = jx_int(root, "autonomy_level", p->autonomy);

    cJSON *tools = jx_obj(root, "tools");
    cJSON *scan = tools ? tools->child : root->child;
    for (cJSON *it = scan; it; it = it->next) {
        if (!cJSON_IsObject(it) || !it->string) continue;
        if (!strcmp(it->string, "approval") || !strcmp(it->string, "autonomy_level") ||
            !strcmp(it->string, "tools"))
            continue;
        int enabled = jx_bool(it, "enabled", 1);
        int conf = jx_bool(it, "require_confirmation", 0);
        permissions_set(p, it->string, enabled, conf);
    }
    cJSON_Delete(root);
    return 0;
}

int permissions_is_enabled(const Permissions *p, const char *tool)
{
    for (int i = 0; i < p->n; i++)
        if (!strcmp(p->tools[i].name, tool)) return p->tools[i].enabled;
    return 1;   /* default: enabled unless explicitly disabled */
}

/* Autonomy levels (build prompt §40):
 *   0 chat, 1 suggest, 2 execute safe tools, 3 autonomous multi-step,
 *   4 autonomous coding/research. Higher levels auto-run higher security tools. */
int permissions_autonomy_allows(const Permissions *p, SecurityLevel lvl)
{
    switch (p->autonomy) {
    case 0: return 0;                       /* chat only */
    case 1: return 0;                       /* suggest only: nothing auto-runs */
    case 2: return lvl <= SEC_SAFE;
    case 3: return lvl <= SEC_MEDIUM_RISK;  /* fs + shell auto, destructive gated */
    case 4: return lvl <= SEC_HIGH_RISK;    /* still never auto CRITICAL */
    default: return lvl <= SEC_SAFE;
    }
}

int permissions_needs_confirmation(const Permissions *p, const char *tool,
                                   SecurityLevel lvl)
{
    const ToolPerm *tp = NULL;
    for (int i = 0; i < p->n; i++)
        if (!strcmp(p->tools[i].name, tool)) { tp = &p->tools[i]; break; }

    if (p->approval == APPROVAL_NEVER) {
        /* never prompt EXCEPT for CRITICAL, which must never auto-run: even in
         * unattended mode a system-configuration action requires a human. */
        return lvl >= SEC_CRITICAL ? 1 : 0;
    }
    if (p->approval == APPROVAL_ALWAYS)
        return 1;

    /* APPROVAL_RISKY (default): confirm when the tool asks for it, or when the
     * security level exceeds what the current autonomy level auto-allows. */
    if (tp && tp->has_override && tp->require_confirmation) return 1;
    if (!permissions_autonomy_allows(p, lvl)) return 1;
    /* MEDIUM_RISK and above always confirm under risky mode unless autonomy 4. */
    if (lvl >= SEC_MEDIUM_RISK && p->autonomy < 4) return 1;
    if (lvl >= SEC_HIGH_RISK) return 1;
    return 0;
}

ApprovalMode permissions_approval(const Permissions *p) { return p->approval; }
int          permissions_autonomy(const Permissions *p) { return p->autonomy; }
