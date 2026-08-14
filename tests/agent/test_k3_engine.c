/* SPDX-License-Identifier: Apache-2.0 */
/* test_k3_engine.c - gate the reusable engine session against the upstream CLI.
 *
 * The CLI (src/cli/k3_run.c) is the oracle-validated forward path. This engine
 * (agent/model/k3_engine.c) reimplements the same forward with sampling/streaming
 * on top. If they ever diverge, greedy decoding of the same prompt would produce
 * different ids — so this test decodes the tiny checkpoint greedily and compares to
 * the ids the CLI emits for the same prompt. Skips cleanly if no checkpoint dir is
 * given (so it never blocks a CI run that lacks the fixture). */
#include "test_util.h"
#include "../../agent/model/k3_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    /* argv[1] is the fixtures dir from ctest; the checkpoint is passed via env
     * K3_TINY_CKPT so the test is opt-in and does not need a fixture in-tree. */
    const char *ckpt = getenv("K3_TINY_CKPT");
    if (!ckpt) {
        printf("SKIPPED: set K3_TINY_CKPT to a runnable checkpoint dir to run this gate\n");
        return 0;
    }
    (void)argc; (void)argv;

    K3EngineOpts o;
    memset(&o, 0, sizeof o);
    o.model_dir = ckpt;
    o.cache_gb = 0.05;
    o.max_gen_default = 6;

    char err[512];
    K3Engine *e = NULL;
    if (k3_engine_open(&e, &o, err, sizeof err) != 0) {
        fprintf(stderr, "engine open failed: %s\n", err);
        return 1;
    }

    int prompt[4] = {1, 2, 3, 4};
    K3SampleOpts s;
    memset(&s, 0, sizeof s);
    s.temperature = 0.0f;   /* greedy, to match the CLI */
    s.max_tokens = 6;
    K3GenOut out;
    if (k3_engine_generate_ids(e, prompt, 4, &s, &out, err, sizeof err) != 0) {
        fprintf(stderr, "generate failed: %s\n", err);
        k3_engine_close(e);
        return 1;
    }

    /* The CLI produced these for --ids 1,2,3,4 --gen 6 on this generator. */
    int expected[6] = {75, 241, 220, 147, 244, 162};
    CHECK(out.n_ids == 6, "expected 6 generated ids, got %d", out.n_ids);
    for (int i = 0; i < out.n_ids && i < 6; i++)
        CHECK(out.ids[i] == expected[i],
              "id[%d] = %d, expected %d (engine diverged from CLI oracle)",
              i, out.ids[i], expected[i]);

    k3_gen_out_free(&out);
    k3_engine_close(e);
    return test_report("test_k3_engine");
}
