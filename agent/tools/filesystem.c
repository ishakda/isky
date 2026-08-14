/* SPDX-License-Identifier: Apache-2.0 */
/* filesystem.c - read_file, write_file, edit_file, list_directory (build prompt §9).
 * Every path goes through the sandbox; destructive ops back up first. */
#include <stdio.h>
#include "tool_registry.h"
#include "../security/sandbox.h"
#include "../reasoning/verification.h"
#include "../util/buf.h"
#include "../util/jsonx.h"
#include "../util/platform.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_READ_BYTES (256 * 1024)

static Sandbox *sb(ToolContext *ctx) { return (Sandbox *)ctx->sandbox; }

/* ---- read_file ---- */
static int t_read_file(const AgentTool *self, const cJSON *args,
                       ToolContext *ctx, ToolResult *res)
{
    (void)self;
    const char *path = jx_str(args, "path", NULL);
    if (!path) { tool_result_fail(res, REC_INVALID_ARGUMENT, "read_file needs 'path'"); return 0; }
    int max_bytes = jx_int(args, "max_bytes", MAX_READ_BYTES);
    if (max_bytes <= 0 || max_bytes > MAX_READ_BYTES) max_bytes = MAX_READ_BYTES;

    char abspath[4096];
    if (sandbox_resolve(sb(ctx), path, abspath, sizeof abspath) != 0) {
        tool_result_fail(res, REC_PERMISSION_ERROR,
                         "path '%s' is outside the workspace", path);
        return 0;
    }
    if (!fs_exists(abspath)) {
        tool_result_fail(res, REC_FILE_NOT_FOUND, "no such file: %s", path);
        return 0;
    }
    size_t len = 0; int trunc = 0;
    char *data = read_entire_file(abspath, (size_t)max_bytes, &len, &trunc);
    if (!data) { tool_result_fail(res, REC_TOOL_ERROR, "cannot read %s", path); return 0; }

    /* number the lines */
    ABuf out; ab_init(&out);
    int line = 1;
    ab_printf(&out, "%s (%zu bytes%s):\n", path, len, trunc ? ", truncated" : "");
    const char *p = data, *nl;
    while (*p) {
        nl = strchr(p, '\n');
        size_t seg = nl ? (size_t)(nl - p) : strlen(p);
        ab_printf(&out, "%6d\t", line++);
        ab_append(&out, p, seg);
        ab_putc(&out, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    free(data);
    res->truncated = trunc;
    tool_result_ok(res, ab_take(&out));
    return 0;
}

/* ---- write_file ---- */
static int t_write_file(const AgentTool *self, const cJSON *args,
                        ToolContext *ctx, ToolResult *res)
{
    (void)self;
    const char *path = jx_str(args, "path", NULL);
    const char *content = jx_str(args, "content", NULL);
    const char *mode = jx_str(args, "mode", "overwrite");   /* overwrite|append|create */
    if (!path || !content) {
        tool_result_fail(res, REC_INVALID_ARGUMENT, "write_file needs 'path' and 'content'");
        return 0;
    }
    char abspath[4096];
    if (sandbox_resolve(sb(ctx), path, abspath, sizeof abspath) != 0) {
        tool_result_fail(res, REC_PERMISSION_ERROR, "path '%s' outside workspace", path);
        return 0;
    }
    int existed = fs_exists(abspath);
    if (!strcmp(mode, "create") && existed) {
        tool_result_fail(res, REC_INVALID_ARGUMENT, "file exists and mode=create: %s", path);
        return 0;
    }
    if (existed) sandbox_backup(sb(ctx), abspath);

    /* ensure parent dir exists */
    char parent[4096];
    snprintf(parent, sizeof parent, "%s", abspath);
    char *slash = strrchr(parent, '/');
    if (slash) { *slash = 0; fs_mkdir_p(parent); }

    size_t clen = strlen(content);
    int rc;
    if (!strcmp(mode, "append")) {
        FILE *f = fopen(abspath, "ab");
        if (!f) { tool_result_fail(res, REC_TOOL_ERROR, "cannot open %s", path); return 0; }
        rc = (fwrite(content, 1, clen, f) == clen) ? 0 : -1;
        if (fclose(f) != 0) rc = -1;
    } else {
        rc = write_entire_file(abspath, content, clen);
    }
    if (rc != 0) { tool_result_fail(res, REC_TOOL_ERROR, "write failed: %s", path); return 0; }
    tool_result_okf(res, "wrote %zu bytes to %s (%s)%s", clen, path, mode,
                    existed ? " [backed up prior version]" : "");
    return 0;
}

/* ---- edit_file: patch-based, validates the original block exists (build prompt §9.3) */
static int t_edit_file(const AgentTool *self, const cJSON *args,
                       ToolContext *ctx, ToolResult *res)
{
    (void)self;
    const char *path = jx_str(args, "path", NULL);
    const char *olds = jx_str(args, "old", NULL);
    const char *news = jx_str(args, "new", NULL);
    if (!path || olds == NULL || news == NULL) {
        tool_result_fail(res, REC_INVALID_ARGUMENT,
                         "edit_file needs 'path', 'old', 'new'");
        return 0;
    }
    char abspath[4096];
    if (sandbox_resolve(sb(ctx), path, abspath, sizeof abspath) != 0) {
        tool_result_fail(res, REC_PERMISSION_ERROR, "path '%s' outside workspace", path);
        return 0;
    }
    if (!fs_exists(abspath)) {
        tool_result_fail(res, REC_FILE_NOT_FOUND, "no such file: %s", path);
        return 0;
    }
    size_t len = 0;
    char *data = read_entire_file(abspath, 0, &len, NULL);
    if (!data) { tool_result_fail(res, REC_TOOL_ERROR, "cannot read %s", path); return 0; }

    /* find old block; require exactly one occurrence for a safe, unambiguous patch */
    const char *first = strstr(data, olds);
    if (!first) {
        free(data);
        tool_result_fail(res, REC_INVALID_ARGUMENT,
            "the 'old' block was not found in %s; read the file and copy exact text", path);
        return 0;
    }
    const char *second = strstr(first + 1, olds);
    if (second && olds[0]) {
        free(data);
        tool_result_fail(res, REC_INVALID_ARGUMENT,
            "the 'old' block occurs more than once in %s; include more context to disambiguate",
            path);
        return 0;
    }
    size_t oldlen = strlen(olds), newlen = strlen(news);
    size_t off = (size_t)(first - data);
    ABuf out; ab_init(&out);
    ab_append(&out, data, off);
    ab_append(&out, news, newlen);
    ab_append(&out, data + off + oldlen, len - off - oldlen);
    free(data);

    sandbox_backup(sb(ctx), abspath);
    int rc = write_entire_file(abspath, out.data, out.len);
    ab_free(&out);
    if (rc != 0) { tool_result_fail(res, REC_TOOL_ERROR, "write failed: %s", path); return 0; }
    tool_result_okf(res, "patched %s (%zu -> %zu bytes in the edited region) [backed up]",
                    path, oldlen, newlen);
    return 0;
}

/* ---- list_directory ---- */
static int cmp_entry(const void *a, const void *b)
{
    const DirEntryInfo *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;   /* dirs first */
    return strcmp(x->name, y->name);
}

static int t_list_directory(const AgentTool *self, const cJSON *args,
                            ToolContext *ctx, ToolResult *res)
{
    (void)self;
    const char *path = jx_str(args, "path", ".");
    char abspath[4096];
    if (sandbox_resolve(sb(ctx), path, abspath, sizeof abspath) != 0) {
        tool_result_fail(res, REC_PERMISSION_ERROR, "path '%s' outside workspace", path);
        return 0;
    }
    if (!fs_is_dir(abspath)) {
        tool_result_fail(res, REC_FILE_NOT_FOUND, "not a directory: %s", path);
        return 0;
    }
    DirEntryInfo *ents = NULL; int n = 0;
    if (fs_list_dir(abspath, &ents, &n) != 0) {
        tool_result_fail(res, REC_TOOL_ERROR, "cannot list %s", path);
        return 0;
    }
    qsort(ents, (size_t)n, sizeof *ents, cmp_entry);
    ABuf out; ab_init(&out);
    ab_printf(&out, "%s (%d entries):\n", path, n);
    for (int i = 0; i < n; i++) {
        char ts[32];
        struct tm tmv;
#if defined(_WIN32)
        gmtime_s(&tmv, &ents[i].mtime);
#else
        gmtime_r(&ents[i].mtime, &tmv);
#endif
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", &tmv);
        ab_printf(&out, "  %s  %-40s  %10lld  %s\n",
                  ents[i].is_dir ? "d" : "-", ents[i].name, ents[i].size, ts);
    }
    free(ents);
    tool_result_ok(res, ab_take(&out));
    return 0;
}

/* ---- delete_file (HIGH_RISK) ---- */
static int t_delete_file(const AgentTool *self, const cJSON *args,
                         ToolContext *ctx, ToolResult *res)
{
    (void)self;
    const char *path = jx_str(args, "path", NULL);
    if (!path) { tool_result_fail(res, REC_INVALID_ARGUMENT, "delete_file needs 'path'"); return 0; }
    char abspath[4096];
    if (sandbox_resolve(sb(ctx), path, abspath, sizeof abspath) != 0) {
        tool_result_fail(res, REC_PERMISSION_ERROR, "path '%s' outside workspace", path);
        return 0;
    }
    if (!fs_exists(abspath)) {
        tool_result_fail(res, REC_FILE_NOT_FOUND, "no such file: %s", path);
        return 0;
    }
    sandbox_backup(sb(ctx), abspath);
    if (fs_remove_file(abspath) != 0) {
        tool_result_fail(res, REC_TOOL_ERROR, "delete failed: %s", path);
        return 0;
    }
    tool_result_okf(res, "deleted %s [backed up; recoverable via rollback]", path);
    return 0;
}

void fs_tools_register(ToolRegistry *r)
{
    AgentTool t;

    memset(&t, 0, sizeof t);
    t.name = "read_file"; t.security_level = SEC_SAFE;
    t.description = "Read a text file with line numbers.";
    t.args_schema = "{\"path\":\"str\",\"max_bytes\":int?}";
    t.execute = t_read_file;
    tool_registry_register(r, &t);

    memset(&t, 0, sizeof t);
    t.name = "write_file"; t.security_level = SEC_LOW_RISK;
    t.description = "Create/overwrite/append a file. Backs up any prior version.";
    t.args_schema = "{\"path\":\"str\",\"content\":\"str\",\"mode\":\"overwrite|append|create\"}";
    t.execute = t_write_file;
    tool_registry_register(r, &t);

    memset(&t, 0, sizeof t);
    t.name = "edit_file"; t.security_level = SEC_LOW_RISK;
    t.description = "Patch a file by replacing an exact unique 'old' block with 'new'.";
    t.args_schema = "{\"path\":\"str\",\"old\":\"str\",\"new\":\"str\"}";
    t.execute = t_edit_file;
    tool_registry_register(r, &t);

    memset(&t, 0, sizeof t);
    t.name = "list_directory"; t.security_level = SEC_SAFE;
    t.description = "List a directory: name, type, size, modified time.";
    t.args_schema = "{\"path\":\"str\"}";
    t.execute = t_list_directory;
    tool_registry_register(r, &t);

    memset(&t, 0, sizeof t);
    t.name = "delete_file"; t.security_level = SEC_HIGH_RISK;
    t.requires_confirmation = 1;
    t.description = "Delete a file (backed up first; recoverable).";
    t.args_schema = "{\"path\":\"str\"}";
    t.execute = t_delete_file;
    tool_registry_register(r, &t);
}
