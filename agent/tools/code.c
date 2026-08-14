/* SPDX-License-Identifier: Apache-2.0 */
/* code.c - coding helpers (build prompt §11): search_files (grep-like), and
 * compile/run/test convenience wrappers that are really structured shell calls.
 * The compile->test->diagnose->fix loop is orchestrated by agent_loop; these are
 * the primitives it uses. Kept thin so the security model stays in shell.c. */
#include <stdio.h>
#include "tool_registry.h"
#include "../security/sandbox.h"
#include "../reasoning/verification.h"
#include "../util/buf.h"
#include "../util/jsonx.h"
#include "../util/platform.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* recursive text search: name pattern (substring) + content pattern (substring) */
static void search_dir(const char *base, const char *rel, const char *name_sub,
                       const char *content_sub, ABuf *out, int *hits, int max_hits)
{
    if (*hits >= max_hits) return;
    char dir[4096];
    snprintf(dir, sizeof dir, "%s%s%s", base, rel[0] ? "/" : "", rel);
    DirEntryInfo *ents = NULL; int n = 0;
    if (fs_list_dir(dir, &ents, &n) != 0) return;
    for (int i = 0; i < n && *hits < max_hits; i++) {
        if (ents[i].name[0] == '.') continue;   /* skip hidden, incl .agent, .git */
        char childrel[4096];
        snprintf(childrel, sizeof childrel, "%s%s%s", rel, rel[0] ? "/" : "", ents[i].name);
        if (ents[i].is_dir) {
            search_dir(base, childrel, name_sub, content_sub, out, hits, max_hits);
            continue;
        }
        int name_ok = !name_sub || !name_sub[0] || strstr(ents[i].name, name_sub);
        if (!name_ok) continue;
        char full[4200];
        snprintf(full, sizeof full, "%s/%s", dir, ents[i].name);
        if (content_sub && content_sub[0]) {
            size_t len = 0;
            char *data = read_entire_file(full, 512 * 1024, &len, NULL);
            if (!data) continue;
            int line = 1;
            for (const char *p = data; *p; ) {
                const char *nl = strchr(p, '\n');
                size_t seg = nl ? (size_t)(nl - p) : strlen(p);
                char save = p[seg];
                ((char *)p)[seg] = 0;
                if (strstr(p, content_sub)) {
                    ab_printf(out, "%s:%d: %s\n", childrel, line, p);
                    if (++(*hits) >= max_hits) { ((char *)p)[seg] = save; break; }
                }
                ((char *)p)[seg] = save;
                line++;
                if (!nl) break;
                p = nl + 1;
            }
            free(data);
        } else {
            ab_printf(out, "%s\n", childrel);
            (*hits)++;
        }
    }
    free(ents);
}

static int t_search_files(const AgentTool *self, const cJSON *args,
                          ToolContext *ctx, ToolResult *res)
{
    (void)self;
    const char *root = jx_str(args, "path", ".");
    const char *name_sub = jx_str(args, "name_contains", NULL);
    const char *content_sub = jx_str(args, "content_contains", NULL);
    int max_hits = jx_int(args, "max_results", 200);
    if (max_hits <= 0 || max_hits > 2000) max_hits = 200;

    char abspath[4096];
    if (sandbox_resolve((Sandbox *)ctx->sandbox, root, abspath, sizeof abspath) != 0) {
        tool_result_fail(res, REC_PERMISSION_ERROR, "path '%s' outside workspace", root);
        return 0;
    }
    if (!fs_is_dir(abspath)) {
        tool_result_fail(res, REC_FILE_NOT_FOUND, "not a directory: %s", root);
        return 0;
    }
    ABuf out; ab_init(&out);
    int hits = 0;
    search_dir(abspath, "", name_sub, content_sub, &out, &hits, max_hits);
    if (hits == 0) ab_puts(&out, "(no matches)\n");
    else ab_printf(&out, "\n%d match(es)%s\n", hits, hits >= max_hits ? " (capped)" : "");
    tool_result_ok(res, ab_take(&out));
    return 0;
}

void code_tools_register(ToolRegistry *r)
{
    AgentTool t;
    memset(&t, 0, sizeof t);
    t.name = "search_files"; t.security_level = SEC_SAFE;
    t.description = "Recursively search the workspace by filename and/or content substring.";
    t.args_schema = "{\"path\":\"str\",\"name_contains\":\"str?\",\"content_contains\":\"str?\",\"max_results\":int?}";
    t.execute = t_search_files;
    tool_registry_register(r, &t);
}
