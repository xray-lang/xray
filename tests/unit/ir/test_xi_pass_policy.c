/*
 * Unit tests for the session optimizer policy.
 *
 * The policy decides which middle-end passes run. Every one of these cases is
 * about refusing a request rather than guessing at one: a spec the caller did
 * not write must never resolve to some optimization level by accident.
 */

#include "../../../src/ir/xi_pass_policy.h"

#include <stdio.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__);                                     \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- " #name " ---\n");                                                             \
        test_##name();                                                                             \
        printf("  PASS\n");                                                                        \
        tests_passed++;                                                                            \
    }                                                                                              \
    static void test_##name(void)

/* Applying `spec` must fail and must leave the policy exactly as it was. */
static bool rejects(const char *spec) {
    XiOptPolicy policy = xi_pass_policy_builtin_default();
    XiOptPolicy before = policy;
    char err[256];
    err[0] = '\0';
    if (xi_pass_policy_apply_spec(&policy, spec, err, sizeof(err)))
        return false;
    if (err[0] == '\0')
        return false;
    return memcmp(&policy, &before, sizeof(policy)) == 0;
}

TEST(pass_names_round_trip_through_ids) {
    for (int i = 0; i < 64; i++) {
        const char *name = xi_pass_name_by_id(i);
        if (!name)
            continue;
        ASSERT(xi_pass_id_by_name(name) == i);
    }
    /* The table is addressed by bit index, so it must be exactly as long as
     * the mask is wide at its widest use. */
    ASSERT(xi_pass_name_by_id(-1) == NULL);
    ASSERT(xi_pass_id_by_name("no_such_pass") == -1);
    ASSERT(xi_pass_id_by_name("") == -1);
    ASSERT(xi_pass_id_by_name(NULL) == -1);
}

TEST(builtin_default_uses_one_shared_light_preplan_set) {
    XiOptPolicy policy = xi_pass_policy_builtin_default();
    ASSERT(policy.pipelines[XI_OPT_PIPELINE_VM].level == XI_OPT_LIGHT);
    ASSERT(policy.pipelines[XI_OPT_PIPELINE_AOT].level == XI_OPT_LIGHT);
    ASSERT(policy.pipelines[XI_OPT_PIPELINE_VM].disabled == XI_OPT_DISABLE_NONE);
    ASSERT(policy.pipelines[XI_OPT_PIPELINE_AOT].disabled == XI_OPT_DISABLE_NONE);
    ASSERT(memcmp(&policy.pipelines[XI_OPT_PIPELINE_VM],
                  &policy.pipelines[XI_OPT_PIPELINE_AOT],
                  sizeof(policy.pipelines[XI_OPT_PIPELINE_VM])) == 0);
    ASSERT(xi_pass_policy_is_builtin_default(&policy));
}

TEST(names_a_level_for_one_pipeline) {
    XiOptPolicy policy = xi_pass_policy_builtin_default();
    XiOptDisableMask aot_before = policy.pipelines[XI_OPT_PIPELINE_AOT].disabled;
    char err[256] = {0};
    ASSERT(xi_pass_policy_apply_spec(&policy, "vm=full", err, sizeof(err)));
    ASSERT(policy.pipelines[XI_OPT_PIPELINE_VM].level == XI_OPT_FULL);
    /* The other pipeline is untouched: one entry states one pipeline. */
    ASSERT(policy.pipelines[XI_OPT_PIPELINE_AOT].level == XI_OPT_LIGHT);
    ASSERT(policy.pipelines[XI_OPT_PIPELINE_AOT].disabled == aot_before);
}

TEST(withholds_named_passes_from_a_level) {
    XiOptPolicy policy = xi_pass_policy_builtin_default();
    char err[256] = {0};
    ASSERT(xi_pass_policy_apply_spec(&policy, "aot=full-ifconv-loop_split", err, sizeof(err)));
    ASSERT(policy.pipelines[XI_OPT_PIPELINE_AOT].level == XI_OPT_FULL);
    ASSERT(policy.pipelines[XI_OPT_PIPELINE_AOT].disabled ==
           (XI_OPT_DISABLE_IFCONV | XI_OPT_DISABLE_LOOP_SPLIT));
    /* An entry states the whole set for its pipeline, so a second entry for
     * the same pipeline replaces the first rather than accumulating. */
    ASSERT(xi_pass_policy_apply_spec(&policy, "aot=full-gvn", err, sizeof(err)));
    ASSERT(policy.pipelines[XI_OPT_PIPELINE_AOT].disabled == XI_OPT_DISABLE_GVN);
}

TEST(states_both_pipelines_in_one_spec) {
    XiOptPolicy policy = xi_pass_policy_builtin_default();
    char err[256] = {0};
    ASSERT(xi_pass_policy_apply_spec(&policy, "vm=full,aot=light", err, sizeof(err)));
    ASSERT(policy.pipelines[XI_OPT_PIPELINE_VM].level == XI_OPT_FULL);
    ASSERT(policy.pipelines[XI_OPT_PIPELINE_AOT].level == XI_OPT_LIGHT);
    ASSERT(!xi_pass_policy_is_builtin_default(&policy));
}

TEST(refuses_every_unknown_token) {
    ASSERT(rejects("jit=full"));             /* unknown pipeline */
    ASSERT(rejects("vm=turbo"));             /* unknown level */
    ASSERT(rejects("vm=full-no_such_pass")); /* unknown pass */
    ASSERT(rejects("full"));                 /* no pipeline */
    ASSERT(rejects("vm"));                   /* no level */
    ASSERT(rejects("vm=full-"));             /* empty pass name */
    ASSERT(rejects(""));                     /* nothing named */
    ASSERT(rejects(",,"));                   /* nothing named */
    ASSERT(rejects("vm=full,aot=bogus"));    /* second entry is bad */
    ASSERT(rejects("VM=FULL"));              /* names are exact */
}

TEST(refuses_to_withhold_a_required_pass) {
    /* tbaa carries XI_PASS_REQUIRED, so the driver runs it whether or not its
     * bit is set. Accepting the name would hand back a policy that renders as
     * "tbaa withheld" while tbaa keeps running, and every difference measured
     * against it would be credited to a pass that never stopped. */
    ASSERT(xi_pass_id_by_name("tbaa") >= 0);
    ASSERT(xi_pass_id_is_required(xi_pass_id_by_name("tbaa")));
    ASSERT(rejects("vm=full-tbaa"));
    ASSERT(rejects("aot=full-tbaa"));
    /* The refusal is per pass name, not per spec: a good name alongside a
     * required one still leaves the policy untouched. */
    ASSERT(rejects("vm=full-dce-tbaa"));
    ASSERT(!xi_pass_id_is_required(xi_pass_id_by_name("dce")));
}

TEST(a_rejected_spec_changes_nothing) {
    /* The interesting half of fail-closed: the first entry of a two-entry
     * spec must not survive when the second entry is rejected. */
    XiOptPolicy policy = xi_pass_policy_builtin_default();
    char err[256] = {0};
    ASSERT(!xi_pass_policy_apply_spec(&policy, "vm=full,aot=nope", err, sizeof(err)));
    ASSERT(policy.pipelines[XI_OPT_PIPELINE_VM].level == XI_OPT_LIGHT);
    ASSERT(xi_pass_policy_is_builtin_default(&policy));
}

TEST(renders_back_to_a_spec_it_accepts) {
    XiOptPolicy policy = xi_pass_policy_builtin_default();
    XiOptPolicy reparsed;
    char text[XI_PASS_POLICY_TEXT_MAX];
    char err[256] = {0};

    ASSERT(xi_pass_policy_apply_spec(&policy, "vm=full-dce,aot=full-ifconv-loop_split", err,
                                     sizeof(err)));
    ASSERT(xi_pass_policy_render(&policy, text, sizeof(text)));
    /* Rendering is canonical, not the order the spec named: passes come out in
     * pass-table order so one configuration has exactly one recorded form. */
    ASSERT(strcmp(text, "vm=full-dce,aot=full-loop_split-ifconv") == 0);

    reparsed = xi_pass_policy_builtin_default();
    ASSERT(xi_pass_policy_apply_spec(&reparsed, text, err, sizeof(err)));
    ASSERT(memcmp(&reparsed, &policy, sizeof(policy)) == 0);
}

TEST(rendering_holds_every_pass_of_both_pipelines) {
    /* The provenance buffer has to fit the widest policy that exists, or a
     * recorded configuration would be a truncated lie. */
    XiOptPolicy policy;
    char text[XI_PASS_POLICY_TEXT_MAX];
    memset(&policy, 0, sizeof(policy));
    for (int p = 0; p < (int) XI_OPT_PIPELINE_COUNT; p++) {
        policy.pipelines[p].level = XI_OPT_FULL;
        for (int i = 0; i < (int) XI_OPT_PASS_ID_COUNT; i++)
            policy.pipelines[p].disabled |= XI_OPT_DISABLE_BIT(i);
    }
    ASSERT(xi_pass_policy_render(&policy, text, sizeof(text)));
    /* One byte short must fail rather than truncate. */
    ASSERT(!xi_pass_policy_render(&policy, text, strlen(text)));
}

TEST(session_policy_seals_when_a_pipeline_reads_it) {
    char err[256] = {0};
    xi_pass_session_policy_reset_for_testing();
    ASSERT(xi_pass_session_policy_apply_spec("vm=full", "--xi-opt", err, sizeof(err)));
    ASSERT(xi_pass_session_pipeline_policy(XI_OPT_PIPELINE_VM).level == XI_OPT_FULL);
    /* A second module of the same session cannot be optimized under different
     * rules than the first, so the change is refused, not applied late. */
    ASSERT(!xi_pass_session_policy_apply_spec("vm=light", "--xi-opt", err, sizeof(err)));
    ASSERT(err[0] != '\0');
    ASSERT(xi_pass_session_pipeline_policy(XI_OPT_PIPELINE_VM).level == XI_OPT_FULL);
    xi_pass_session_policy_reset_for_testing();
}

TEST(session_policy_refuses_a_bad_spec_without_changing) {
    char err[256] = {0};
    xi_pass_session_policy_reset_for_testing();
    ASSERT(!xi_pass_session_policy_apply_spec("aot=turbo", "--xi-opt", err, sizeof(err)));
    ASSERT(xi_pass_policy_is_builtin_default(xi_pass_session_policy()));
    xi_pass_session_policy_reset_for_testing();
}

int main(void) {
    printf("=== Xi Optimizer Policy Tests ===\n\n");

    run_pass_names_round_trip_through_ids();
    run_builtin_default_uses_one_shared_light_preplan_set();
    run_names_a_level_for_one_pipeline();
    run_withholds_named_passes_from_a_level();
    run_states_both_pipelines_in_one_spec();
    run_refuses_every_unknown_token();
    run_refuses_to_withhold_a_required_pass();
    run_a_rejected_spec_changes_nothing();
    run_renders_back_to_a_spec_it_accepts();
    run_rendering_holds_every_pass_of_both_pipelines();
    run_session_policy_seals_when_a_pipeline_reads_it();
    run_session_policy_refuses_a_bad_spec_without_changing();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
