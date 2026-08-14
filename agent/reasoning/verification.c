/* SPDX-License-Identifier: Apache-2.0 */
#include "verification.h"
#include "../util/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *recovery_class_name(RecoveryClass c)
{
    switch (c) {
    case REC_NONE:              return "NONE";
    case REC_TOOL_ERROR:        return "TOOL_ERROR";
    case REC_TIMEOUT:           return "TIMEOUT";
    case REC_INVALID_ARGUMENT:  return "INVALID_ARGUMENT";
    case REC_MODEL_ERROR:       return "MODEL_ERROR";
    case REC_PARSER_ERROR:      return "PARSER_ERROR";
    case REC_PERMISSION_ERROR:  return "PERMISSION_ERROR";
    case REC_FILE_NOT_FOUND:    return "FILE_NOT_FOUND";
    case REC_COMPILATION_ERROR: return "COMPILATION_ERROR";
    case REC_TEST_FAILURE:      return "TEST_FAILURE";
    case REC_NETWORK_ERROR:     return "NETWORK_ERROR";
    }
    return "?";
}

RecoveryStrategy recovery_strategy_for(RecoveryClass c, int attempts, int max_retries)
{
    return recovery_strategy_for2(c, attempts, max_retries, 5);
}

RecoveryStrategy recovery_strategy_for2(RecoveryClass c, int attempts,
                                        int max_retries, int max_repair)
{
    /* Never retry the same failing action indefinitely (build prompt §20). */
    switch (c) {
    case REC_TIMEOUT:
    case REC_NETWORK_ERROR:
    case REC_TOOL_ERROR:
        /* transient: a couple of retries, then re-plan */
        if (attempts < max_retries) return STRAT_RETRY;
        return STRAT_REPLAN;
    case REC_COMPILATION_ERROR:
    case REC_TEST_FAILURE:
        /* the model can likely fix these by choosing a different edit, but the
         * fix loop is bounded by max_repair_attempts (build prompt §11). */
        if (max_repair > 0 && attempts >= max_repair) return STRAT_ABORT;
        return STRAT_REPLAN;
    case REC_INVALID_ARGUMENT:
    case REC_PARSER_ERROR:
        /* the model produced a bad action; regenerate/replan, don't retry verbatim */
        if (attempts < max_retries) return STRAT_REPLAN;
        return STRAT_ASK_USER;
    case REC_FILE_NOT_FOUND:
        return STRAT_REPLAN;
    case REC_PERMISSION_ERROR:
        /* only the user can widen permissions */
        return STRAT_ASK_USER;
    case REC_MODEL_ERROR:
        if (attempts < max_retries) return STRAT_RETRY;
        return STRAT_ABORT;
    default:
        return STRAT_ABORT;
    }
}

static int contains_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) return 0;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (strncasecmp(p, needle, nl) == 0) return 1;
    return 0;
}

int verify_check(const VerifyCheck *vc, const char *workspace,
                 const char *observation, int last_exit_code, char **reason)
{
    if (reason) *reason = NULL;
    if (!vc || !vc->kind) return 1;   /* nothing to verify -> vacuously true */

    if (!strcmp(vc->kind, "file_exists")) {
        char path[4096];
        if (vc->arg && vc->arg[0] == '/')
            snprintf(path, sizeof path, "%s", vc->arg);
        else
            snprintf(path, sizeof path, "%s/%s", workspace ? workspace : ".",
                     vc->arg ? vc->arg : "");
        if (fs_exists(path)) return 1;
        if (reason) { char b[4200]; snprintf(b, sizeof b, "file does not exist: %s", path); *reason = strdup(b); }
        return 0;
    }
    if (!strcmp(vc->kind, "contains")) {
        if (contains_ci(observation, vc->arg)) return 1;
        if (reason) { char b[512]; snprintf(b, sizeof b, "output does not contain '%s'", vc->arg ? vc->arg : ""); *reason = strdup(b); }
        return 0;
    }
    if (!strcmp(vc->kind, "exit_zero")) {
        if (last_exit_code == 0) return 1;
        if (reason) { char b[128]; snprintf(b, sizeof b, "exit code was %d, expected 0", last_exit_code); *reason = strdup(b); }
        return 0;
    }
    if (!strcmp(vc->kind, "nonempty")) {
        if (observation && observation[0]) return 1;
        if (reason) *reason = strdup("observation was empty");
        return 0;
    }
    /* unknown kind: do not block completion, but say so */
    if (reason) { char b[128]; snprintf(b, sizeof b, "unknown verification kind '%s'", vc->kind); *reason = strdup(b); }
    return 1;
}
