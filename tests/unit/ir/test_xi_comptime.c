/*
 * Unit tests for the IR constant-simplification fixpoint.
 *
 * The fixpoint re-runs four passes the optimizer policy can withhold by name.
 * These cases are about that mask reaching them: a pass the caller withheld
 * must not run again inside this one.
 */

#include "../../../src/ir/xi_opt_comptime.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_core_api.h"
#include "../../../src/runtime/value/xtype.h"
#include <stdio.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

static int passed, failed;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (cond) {                                                                                \
            passed++;                                                                              \
        } else {                                                                                   \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__);                                     \
            failed++;                                                                              \
        }                                                                                          \
    } while (0)

/* Const-folding and SCCP both replace a constant sum, so isolating either one
 * means withholding both. */
#define WITHHOLD_CONSTANT_PASSES (XI_OPT_DISABLE_CONSTFOLD | XI_OPT_DISABLE_SCCP)

static void test_null(void) {
    XiPassChange c = xi_opt_const_fixpoint(NULL, XI_OPT_DISABLE_NONE);
    CHECK(!c.values_changed);
}

/* entry: sum = const(2) + const(3); return sum */
static XiFunc *build_foldable_sum(XiValue **out_sum) {
    XiFunc *f = xi_func_new("const_fixpoint_test", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;

    XiValue *a = xi_const_int(f, entry, 2, &stub_int);
    XiValue *b = xi_const_int(f, entry, 3, &stub_int);
    XiValue *sum = xi_binary(f, entry, XI_ADD, &stub_int, a, b);
    xi_block_set_return(entry, sum);

    *out_sum = sum;
    return f;
}

/* entry: dead = const(7) + const(8) (never used); return const(1) */
static XiFunc *build_dead_value(XiBlock **out_entry) {
    XiFunc *f = xi_func_new("const_fixpoint_dce_test", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;

    XiValue *a = xi_const_int(f, entry, 7, &stub_int);
    XiValue *b = xi_const_int(f, entry, 8, &stub_int);
    (void) xi_binary(f, entry, XI_ADD, &stub_int, a, b);
    xi_block_set_return(entry, xi_const_int(f, entry, 1, &stub_int));

    *out_entry = entry;
    return f;
}

static void test_folds_when_nothing_is_withheld(void) {
    XiValue *sum = NULL;
    XiFunc *f = build_foldable_sum(&sum);

    XiPassChange c = xi_opt_const_fixpoint(f, XI_OPT_DISABLE_NONE);
    CHECK(c.values_changed);
    CHECK(sum->op == XI_CONST);
    CHECK(sum->aux_int == 5);

    xi_func_free(f);
}

static void test_withheld_constant_passes_do_not_run_here(void) {
    XiValue *sum = NULL;
    XiFunc *f = build_foldable_sum(&sum);

    /* The driver skips these for this mask. Re-running them inside the
     * fixpoint would fold the sum anyway and report a saving that belongs to
     * a pass the caller switched off. */
    XiPassChange c = xi_opt_const_fixpoint(f, WITHHOLD_CONSTANT_PASSES);
    CHECK(!c.values_changed);
    CHECK(sum->op == XI_ADD);

    xi_func_free(f);
}

static void test_withheld_dce_does_not_run_here(void) {
    XiBlock *entry = NULL;
    XiFunc *f = build_dead_value(&entry);
    uint32_t before = entry->nvalues;

    XiPassChange c = xi_opt_const_fixpoint(
        f, WITHHOLD_CONSTANT_PASSES | XI_OPT_DISABLE_COPY_PROP | XI_OPT_DISABLE_DCE);
    CHECK(!c.values_changed);
    CHECK(entry->nvalues == before);

    xi_func_free(f);

    /* Same mask minus DCE: the dead value goes, which is what makes the case
     * above evidence that the mask was read rather than that nothing was
     * removable. */
    f = build_dead_value(&entry);
    before = entry->nvalues;
    c = xi_opt_const_fixpoint(f, WITHHOLD_CONSTANT_PASSES | XI_OPT_DISABLE_COPY_PROP);
    CHECK(c.values_changed);
    CHECK(entry->nvalues < before);

    xi_func_free(f);
}

static void test_withholding_every_constituent_leaves_the_ir_alone(void) {
    XiValue *sum = NULL;
    XiFunc *f = build_foldable_sum(&sum);

    XiPassChange c = xi_opt_const_fixpoint(f, XI_OPT_DISABLE_CONSTFOLD | XI_OPT_DISABLE_COPY_PROP |
                                                  XI_OPT_DISABLE_SCCP | XI_OPT_DISABLE_DCE);
    CHECK(!c.values_changed);
    CHECK(!c.cfg_changed);
    CHECK(sum->op == XI_ADD);

    xi_func_free(f);
}

static void test_an_unrelated_pass_in_the_mask_changes_nothing(void) {
    XiValue *sum = NULL;
    XiFunc *f = build_foldable_sum(&sum);

    /* Only the four constituents are read; withholding some other pass must
     * not switch the fixpoint off. */
    XiPassChange c = xi_opt_const_fixpoint(f, XI_OPT_DISABLE_LICM);
    CHECK(c.values_changed);
    CHECK(sum->op == XI_CONST);

    xi_func_free(f);
}

int main(void) {
    printf("=== Xi const fixpoint tests ===\n");
    test_null();
    test_folds_when_nothing_is_withheld();
    test_withheld_constant_passes_do_not_run_here();
    test_withheld_dce_does_not_run_here();
    test_withholding_every_constituent_leaves_the_ir_alone();
    test_an_unrelated_pass_in_the_mask_changes_nothing();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
