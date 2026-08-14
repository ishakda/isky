/* SPDX-License-Identifier: Apache-2.0 */
/* POSIX implementation of platform.h. A Win32 port lives behind the same API. */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "platform.h"
#include "buf.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

double mono_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

void msleep(int ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

long pf_getpid(void) { return (long)getpid(); }

void gen_id(char *out, size_t outsz, const char *prefix)
{
    static unsigned long counter = 0;
    unsigned long r = 0;
    FILE *u = fopen("/dev/urandom", "rb");
    if (u) {
        if (fread(&r, sizeof r, 1, u) != 1) r = 0;
        fclose(u);
    }
    if (!r) r = (unsigned long)time(NULL) ^ ((unsigned long)getpid() << 16);
    r ^= ++counter * 2654435761UL;
    snprintf(out, outsz, "%s_%08lx%04lx", prefix, r & 0xffffffffUL,
             (unsigned long)(time(NULL) & 0xffff));
}

/* ---- process execution ----------------------------------------------------- */

int proc_run(const char *command, const char *workdir, int timeout_s,
             size_t max_output, int clean_env, ProcResult *result)
{
    memset(result, 0, sizeof *result);
    result->exit_code = -1;

    int fds[2];
    if (pipe(fds) != 0) return -1;

    double t0 = mono_seconds();
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }

    if (pid == 0) {
        /* child */
        close(fds[0]);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[1]);
        /* new process group so the timeout can kill the whole tree */
        setpgid(0, 0);
        if (workdir && chdir(workdir) != 0) {
            fprintf(stderr, "cannot chdir to %s: %s\n", workdir, strerror(errno));
            _exit(126);
        }
        if (clean_env) {
            const char *keep[] = { "PATH", "HOME", "LANG", "TMPDIR", "TERM", NULL };
            char *saved[8] = {0};
            for (int i = 0; keep[i]; i++) {
                const char *v = getenv(keep[i]);
                saved[i] = v ? strdup(v) : NULL;
            }
#if defined(__GLIBC__) || defined(__APPLE__)
            extern char **environ;
            environ = NULL;
#endif
            for (int i = 0; keep[i]; i++)
                if (saved[i]) setenv(keep[i], saved[i], 1);
        }
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    /* parent */
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);

    ABuf out;
    ab_init(&out);
    int done = 0, status = 0;
    double deadline = timeout_s > 0 ? t0 + timeout_s : 0.0;

    while (!done) {
        struct pollfd p = { fds[0], POLLIN, 0 };
        int ptimeout = 200;
        if (deadline > 0.0) {
            double left = deadline - mono_seconds();
            if (left <= 0) {
                /* timeout: kill the process group, then reap */
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
                result->timed_out = 1;
                /* drain whatever is buffered */
            } else if (left * 1000 < ptimeout) {
                ptimeout = (int)(left * 1000) + 1;
            }
        }
        int pr = poll(&p, 1, ptimeout);
        if (pr > 0 && (p.revents & (POLLIN | POLLHUP))) {
            char tmp[4096];
            ssize_t n;
            while ((n = read(fds[0], tmp, sizeof tmp)) > 0) {
                if (max_output == 0 || out.len < max_output) {
                    size_t take = (size_t)n;
                    if (max_output > 0 && out.len + take > max_output) {
                        take = max_output - out.len;
                        result->output_truncated = 1;
                    }
                    ab_append(&out, tmp, take);
                } else {
                    result->output_truncated = 1;
                }
            }
            if (n == 0) { /* EOF */
                /* keep looping until child reaped below */
            }
        }
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            /* final drain */
            char tmp[4096];
            ssize_t n;
            while ((n = read(fds[0], tmp, sizeof tmp)) > 0) {
                if (max_output == 0 || out.len < max_output) {
                    size_t take = (size_t)n;
                    if (max_output > 0 && out.len + take > max_output) {
                        take = max_output - out.len;
                        result->output_truncated = 1;
                    }
                    ab_append(&out, tmp, take);
                } else result->output_truncated = 1;
            }
            done = 1;
        } else if (result->timed_out) {
            /* ensure we do not spin forever if the child ignores SIGKILL (it cannot) */
            pid_t w2 = waitpid(pid, &status, 0);
            (void)w2;
            done = 1;
        }
    }
    close(fds[0]);

    result->seconds = mono_seconds() - t0;
    if (WIFEXITED(status)) result->exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) { result->signaled = 1; result->exit_code = -1; }
    if (result->timed_out) result->exit_code = -1;
    result->output_len = out.len;
    result->output = ab_take(&out);
    ab_free(&out);
    return 0;
}

/* ---- filesystem ------------------------------------------------------------ */

int fs_list_dir(const char *path, DirEntryInfo **entries, int *n)
{
    *entries = NULL; *n = 0;
    DIR *d = opendir(path);
    if (!d) return -1;
    int cap = 32, cnt = 0;
    DirEntryInfo *arr = (DirEntryInfo *)malloc((size_t)cap * sizeof *arr);
    if (!arr) { closedir(d); return -1; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (cnt == cap) {
            cap *= 2;
            DirEntryInfo *na = (DirEntryInfo *)realloc(arr, (size_t)cap * sizeof *arr);
            if (!na) { free(arr); closedir(d); return -1; }
            arr = na;
        }
        DirEntryInfo *it = &arr[cnt];
        memset(it, 0, sizeof *it);
        snprintf(it->name, sizeof it->name, "%s", e->d_name);
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            it->is_dir = S_ISDIR(st.st_mode);
            it->size = (long long)st.st_size;
            it->mtime = st.st_mtime;
        }
        cnt++;
    }
    closedir(d);
    *entries = arr; *n = cnt;
    return 0;
}

int fs_mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int fs_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

int fs_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

long long fs_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long long)st.st_size;
}

int fs_copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { rc = -1; break; }
    if (ferror(in)) rc = -1;
    fclose(in);
    if (fclose(out) != 0) rc = -1;
    return rc;
}

int fs_remove_file(const char *path) { return remove(path); }

int fs_realpath(const char *path, char *out, size_t outsz)
{
    char resolved[PATH_MAX];
    if (realpath(path, resolved)) {
        if (strlen(resolved) + 1 > outsz) return -1;
        strcpy(out, resolved);
        return 0;
    }
    /* leaf may not exist yet: resolve the parent, then append the leaf */
    if (errno != ENOENT) return -1;
    char parent[PATH_MAX], leaf[PATH_MAX];
    const char *slash = strrchr(path, '/');
    if (!slash) {
        if (!getcwd(parent, sizeof parent)) return -1;
        snprintf(leaf, sizeof leaf, "%s", path);
    } else if (slash == path) {
        strcpy(parent, "/");
        snprintf(leaf, sizeof leaf, "%s", slash + 1);
    } else {
        size_t plen = (size_t)(slash - path);
        if (plen >= sizeof parent) return -1;
        memcpy(parent, path, plen);
        parent[plen] = 0;
        snprintf(leaf, sizeof leaf, "%s", slash + 1);
    }
    if (strstr(leaf, "..")) return -1;   /* refuse traversal in a non-existent leaf */
    char rparent[PATH_MAX];
    if (!realpath(parent, rparent)) return -1;
    int n = snprintf(out, outsz, "%s/%s", rparent, leaf);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}
