/* SPDX-License-Identifier: Apache-2.0 */
/* permissions.h - per-tool permission policy (build prompt §21). Loaded from
 * config/permissions.json; user-configurable. Gates whether a tool is enabled at
 * all, and whether it needs confirmation, independent of the tool's intrinsic
 * security level. */
#ifndef AGENT_PERMISSIONS_H
#define AGENT_PERMISSIONS_H

#include "../tools/tool.h"
#include "../core/config.h"

typedef struct Permissions Permissions;

Permissions *permissions_create(ApprovalMode approval, int autonomy_level);
void         permissions_destroy(Permissions *p);
int          permissions_load(Permissions *p, const char *path);

/* Per-tool overrides. */
void permissions_set(Permissions *p, const char *tool, int enabled,
                     int require_confirmation);

int  permissions_is_enabled(const Permissions *p, const char *tool);
/* Does this invocation need explicit user confirmation? Combines: per-tool
 * override, the tool's security level, the approval mode, and autonomy level. */
int  permissions_needs_confirmation(const Permissions *p, const char *tool,
                                    SecurityLevel lvl);
/* Is a tool of this security level permitted at the current autonomy level at all,
 * without confirmation? (Autonomy 2 = safe tools only auto; higher = more.) */
int  permissions_autonomy_allows(const Permissions *p, SecurityLevel lvl);

ApprovalMode permissions_approval(const Permissions *p);
int          permissions_autonomy(const Permissions *p);

#endif
