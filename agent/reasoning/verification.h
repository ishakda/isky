/* SPDX-License-Identifier: Apache-2.0 */
/* verification.h - error classification (build prompt §20) and the verification
 * stage (§19). Recovery strategy selection lives in agent_loop. */
#ifndef AGENT_VERIFICATION_H
#define AGENT_VERIFICATION_H

#include "cJSON.h"

typedef enum {
    REC_NONE = 0,
    REC_TOOL_ERROR,
    REC_TIMEOUT,
    REC_INVALID_ARGUMENT,
    REC_MODEL_ERROR,
    REC_PARSER_ERROR,
    REC_PERMISSION_ERROR,
    REC_FILE_NOT_FOUND,
    REC_COMPILATION_ERROR,
    REC_TEST_FAILURE,
    REC_NETWORK_ERROR
} RecoveryClass;

const char *recovery_class_name(RecoveryClass c);

/* Recovery strategy the loop should try next for a given failure class and how
 * many times that class has already failed on the current step. */
typedef enum {
    STRAT_RETRY = 0,     /* try the same action again (transient) */
    STRAT_REPLAN,        /* ask the model to choose a different action */
    STRAT_ASK_USER,      /* escalate to the human */
    STRAT_ABORT          /* give up on the task */
} RecoveryStrategy;

/* max_retries bounds transient/model retries; max_repair bounds the compile/test
 * fix loop (build prompt §11 max_repair_attempts). Pass max_repair<=0 to disable
 * the cap (falls back to unlimited replans, still bounded by task max_iterations). */
RecoveryStrategy recovery_strategy_for2(RecoveryClass c, int attempts,
                                        int max_retries, int max_repair);
/* Back-compat wrapper: max_repair defaults to a generous 5. */
RecoveryStrategy recovery_strategy_for(RecoveryClass c, int attempts, int max_retries);

/* Verification of a completed step or task. Returns 1 pass, 0 fail; when it
 * fails, *reason (caller-freed) explains why. `check` describes what to verify
 * (from the plan's "verification" field); observation is the tool output. This
 * is deterministic structural verification (file exists, exit code 0, output
 * contains expected substring); model-judged verification is a separate hook. */
typedef struct {
    const char *kind;        /* "file_exists" | "contains" | "exit_zero" | "nonempty" */
    const char *arg;         /* path / substring, per kind */
} VerifyCheck;

int verify_check(const VerifyCheck *vc, const char *workspace,
                 const char *observation, int last_exit_code, char **reason);

#endif
