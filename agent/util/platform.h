/* SPDX-License-Identifier: Apache-2.0 */
/* platform.h - the ONLY place the agent touches platform-specific APIs.
 * POSIX is implemented; a Win32 port implements the same signatures. */
#ifndef AGENT_PLATFORM_H
#define AGENT_PLATFORM_H

#include <stddef.h>
#include <time.h>

/* ---- process execution ---- */
typedef struct {
    int    exit_code;      /* child exit code, or -1 when not exited normally */
    int    timed_out;      /* 1 when killed by the timeout */
    int    signaled;       /* 1 when terminated by a signal */
    int    output_truncated;
    char  *output;         /* merged stdout+stderr, malloc'd, NUL-terminated */
    size_t output_len;
    double seconds;        /* wall time */
} ProcResult;

/* Run `command` through the shell in `workdir` (NULL = inherit cwd) with a wall
 * timeout and an output byte cap. `clean_env` non-zero runs the child with a
 * minimal environment (PATH, HOME, LANG, TMPDIR only) so credentials in the
 * parent environment never leak into tool subprocesses.
 * Returns 0 when the child was spawned (inspect result), -1 on spawn failure.
 * Caller frees result->output. */
int proc_run(const char *command, const char *workdir, int timeout_s,
             size_t max_output, int clean_env, ProcResult *result);

/* ---- filesystem ---- */
typedef struct {
    char   name[256];
    int    is_dir;
    long long size;
    time_t mtime;
} DirEntryInfo;

/* List a directory. Returns malloc'd array (caller frees), count in *n; -1 on error. */
int fs_list_dir(const char *path, DirEntryInfo **entries, int *n);
int fs_mkdir_p(const char *path);
int fs_exists(const char *path);
int fs_is_dir(const char *path);
long long fs_file_size(const char *path);
int fs_copy_file(const char *src, const char *dst);
int fs_remove_file(const char *path);

/* Canonicalize `path` (resolving ., .., symlinks for existing prefixes) into
 * `out`. Works for not-yet-existing leaf files as long as the parent exists.
 * Returns 0 on success. */
int fs_realpath(const char *path, char *out, size_t outsz);

/* ---- misc ---- */
double mono_seconds(void);           /* monotonic clock */
void   msleep(int ms);
long   pf_getpid(void);
/* cryptographically-random-ish hex id (time+pid+counter fallback) */
void   gen_id(char *out, size_t outsz, const char *prefix);

#endif
