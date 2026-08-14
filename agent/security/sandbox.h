/* SPDX-License-Identifier: Apache-2.0 */
/* sandbox.h - workspace confinement (build prompt §42) and file-change safety
 * with backups + rollback (§43). All tool file access resolves through here. */
#ifndef AGENT_SANDBOX_H
#define AGENT_SANDBOX_H

#include <stddef.h>

typedef struct Sandbox Sandbox;

/* root is the workspace directory; it is created if missing and canonicalized.
 * When enabled, every resolved path must stay within root (traversal rejected). */
Sandbox *sandbox_create(const char *root, int enabled);
void     sandbox_destroy(Sandbox *s);
const char *sandbox_root(const Sandbox *s);

/* Resolve a user/model-supplied path (relative to the workspace, or absolute) to
 * an absolute path in `out`. Returns 0 if allowed, -1 if it escapes the sandbox
 * or is malformed. When the sandbox is disabled, only canonicalization happens. */
int sandbox_resolve(const Sandbox *s, const char *path, char *out, size_t outsz);

/* Back up a file into <root>/.agent/backups/ before it is modified/deleted.
 * Returns 0 (also when the file does not exist yet: nothing to back up). */
int sandbox_backup(Sandbox *s, const char *abspath);
/* Roll back the most recent backup of `abspath`. 0 on success, -1 if none. */
int sandbox_rollback(Sandbox *s, const char *abspath);

#endif
