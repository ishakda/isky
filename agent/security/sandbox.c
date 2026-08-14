/* SPDX-License-Identifier: Apache-2.0 */
#include "sandbox.h"
#include "../util/platform.h"
#include "../util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct Sandbox {
    char root[4096];      /* canonical absolute path */
    int  enabled;
    int  backup_seq;
};

Sandbox *sandbox_create(const char *root, int enabled)
{
    Sandbox *s = (Sandbox *)calloc(1, sizeof *s);
    if (!s) return NULL;
    if (fs_mkdir_p(root) != 0) { LOG_E("sandbox", "cannot create workspace %s", root); }
    if (fs_realpath(root, s->root, sizeof s->root) != 0) {
        snprintf(s->root, sizeof s->root, "%s", root);
    }
    s->enabled = enabled;
    /* ensure backup dir exists */
    char bdir[4200];
    snprintf(bdir, sizeof bdir, "%s/.agent/backups", s->root);
    fs_mkdir_p(bdir);
    return s;
}

void sandbox_destroy(Sandbox *s) { free(s); }
const char *sandbox_root(const Sandbox *s) { return s->root; }

static int within(const char *root, const char *abspath)
{
    size_t rl = strlen(root);
    if (strncmp(abspath, root, rl) != 0) return 0;
    /* boundary: exact match, or next char is a separator */
    return abspath[rl] == 0 || abspath[rl] == '/';
}

int sandbox_resolve(const Sandbox *s, const char *path, char *out, size_t outsz)
{
    if (!path || !path[0]) return -1;

    char joined[4096];
    if (path[0] == '/') {
        snprintf(joined, sizeof joined, "%s", path);
    } else {
        int n = snprintf(joined, sizeof joined, "%s/%s", s->root, path);
        if (n < 0 || (size_t)n >= sizeof joined) return -1;
    }

    char resolved[4096];
    if (fs_realpath(joined, resolved, sizeof resolved) != 0) {
        LOG_D("sandbox", "cannot resolve path %s", joined);
        return -1;
    }
    if (s->enabled && !within(s->root, resolved)) {
        LOG_W("sandbox", "REJECTED path escape: %s -> %s (root %s)",
              path, resolved, s->root);
        return -1;
    }
    if (strlen(resolved) + 1 > outsz) return -1;
    strcpy(out, resolved);
    return 0;
}

/* backups live at <root>/.agent/backups/<basename>.<seq>.<ts>.bak, newest last */
int sandbox_backup(Sandbox *s, const char *abspath)
{
    if (!fs_exists(abspath)) return 0;
    const char *base = strrchr(abspath, '/');
    base = base ? base + 1 : abspath;
    char dst[4300];
    snprintf(dst, sizeof dst, "%s/.agent/backups/%s.%d.%ld.bak",
             s->root, base, s->backup_seq++, (long)time(NULL));
    if (fs_copy_file(abspath, dst) != 0) {
        LOG_W("sandbox", "backup of %s failed", abspath);
        return -1;
    }
    LOG_D("sandbox", "backed up %s -> %s", abspath, dst);
    return 0;
}

int sandbox_rollback(Sandbox *s, const char *abspath)
{
    const char *base = strrchr(abspath, '/');
    base = base ? base + 1 : abspath;
    char bdir[4200];
    snprintf(bdir, sizeof bdir, "%s/.agent/backups", s->root);

    DirEntryInfo *ents = NULL;
    int n = 0;
    if (fs_list_dir(bdir, &ents, &n) != 0) return -1;

    /* find the newest backup whose name starts with "<base>." */
    char prefix[512];
    snprintf(prefix, sizeof prefix, "%s.", base);
    size_t pl = strlen(prefix);
    char best[512] = {0};
    time_t best_mt = 0;
    for (int i = 0; i < n; i++) {
        if (strncmp(ents[i].name, prefix, pl) == 0 && ents[i].mtime >= best_mt) {
            best_mt = ents[i].mtime;
            snprintf(best, sizeof best, "%s", ents[i].name);
        }
    }
    free(ents);
    if (!best[0]) return -1;
    char src[4400];
    snprintf(src, sizeof src, "%s/%s", bdir, best);
    if (fs_copy_file(src, abspath) != 0) return -1;
    LOG_I("sandbox", "rolled back %s from %s", abspath, best);
    return 0;
}
