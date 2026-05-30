/*
 * Unit tests for Xi loop induction-variable analysis.
 */

#include "../../../src/ir/xi_analysis.h"
#include "../../../src/ir/xi_loop.h"
#include "../../../src/ir/xi_verify.h"
#include "../../../src/ir/xi.h"
#include "../../../src/runtime/value/xtype.h"

#include <stdio.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 2, .frozen = true};

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

typedef struct LoopFixture {
    XiFunc *f;
    XiBlock *entry;
    XiBlock *header;
    XiBlock *body;
    XiBlock *exit_blk;
    XiPhi *phi;
    XiValue *start;
    XiValue *step;
    XiValue *next;
} LoopFixture;

static int pred_index(const XiBlock *blk, const XiBlock *pred) {
    if (!blk || !pred)
        return -1;
    for (uint16_t i = 0; i < blk->npreds; i++)
        if (blk->preds[i] == pred)
            return (int) i;
    return -1;
}

static bool verify_func(const XiFunc *f) {
    char err[256] = {0};
    bool ok = xi_verify(f, err, sizeof(err));
    if (!ok)
        printf("  verify error: %s\n", err);
    return ok;
}

static bool make_counted_loop(LoopFixture *fx, int64_t step_value, uint16_t step_op) {
    fx->f = xi_func_new("loop_iv", &stub_int);
    if (!fx->f)
        return false;
    fx->entry = xi_block_new(fx->f);
    fx->header = xi_block_new(fx->f);
    fx->body = xi_block_new(fx->f);
    fx->exit_blk = xi_block_new(fx->f);
    if (!fx->entry || !fx->header || !fx->body || !fx->exit_blk)
        return false;

    fx->start = xi_const_int(fx->f, fx->entry, 0, &stub_int);
    fx->step = xi_const_int(fx->f, fx->entry, step_value, &stub_int);
    XiValue *cond = xi_const_bool(fx->f, fx->header, true, &stub_bool);
    if (!fx->start || !fx->step || !cond)
        return false;

    xi_block_set_jump(fx->entry, fx->header);
    xi_block_set_if(fx->header, cond, fx->body, fx->exit_blk);
    xi_block_set_jump(fx->body, fx->header);

    fx->phi = xi_phi_new(fx->f, fx->header, &stub_int, fx->header->npreds);
    if (!fx->phi)
        return false;
    fx->next = xi_binary(fx->f, fx->body, step_op, &stub_int, &fx->phi->value, fx->step);
    if (!fx->next)
        return false;

    int pre_idx = pred_index(fx->header, fx->entry);
    int latch_idx = pred_index(fx->header, fx->body);
    if (pre_idx < 0 || latch_idx < 0)
        return false;
    fx->phi->value.args[pre_idx] = fx->start;
    fx->phi->value.args[latch_idx] = fx->next;

    xi_block_set_return(fx->exit_blk, &fx->phi->value);
    fx->entry->sealed = true;
    fx->header->sealed = true;
    fx->body->sealed = true;
    fx->exit_blk->sealed = true;
    return true;
}

static XiLoopInfo *compute_loop_info(XiFunc *f) {
    xi_compute_rpo(f);
    xi_compute_dominators(f);
    return xi_compute_loops(f);
}

TEST(basic_iv_add_step) {
    LoopFixture fx = {0};
    ASSERT(make_counted_loop(&fx, 1, XI_ADD));
    ASSERT(verify_func(fx.f));

    XiLoopInfo *info = compute_loop_info(fx.f);
    ASSERT(info != NULL);
    ASSERT(info->nloop == 1);
    XiLoop *loop = info->all_loops[0];
    ASSERT(loop->nbasic_ivs == 1);
    ASSERT(loop->basic_ivs[0].phi == &fx.phi->value);
    ASSERT(loop->basic_ivs[0].start == fx.start);
    ASSERT(loop->basic_ivs[0].next == fx.next);
    ASSERT(loop->basic_ivs[0].step == fx.step);
    ASSERT(loop->basic_ivs[0].has_const_step);
    ASSERT(loop->basic_ivs[0].step_const == 1);
    ASSERT(loop->nderived_ivs == 0);
    ASSERT(loop->npolynomial_ivs == 0);

    xi_loopinfo_free(info);
    xi_func_free(fx.f);
}

TEST(basic_iv_sub_step) {
    LoopFixture fx = {0};
    ASSERT(make_counted_loop(&fx, 2, XI_SUB));
    ASSERT(verify_func(fx.f));

    XiLoopInfo *info = compute_loop_info(fx.f);
    ASSERT(info != NULL);
    XiLoop *loop = info->all_loops[0];
    ASSERT(loop->nbasic_ivs == 1);
    ASSERT(loop->basic_ivs[0].step_op == XI_SUB);
    ASSERT(loop->basic_ivs[0].has_const_step);
    ASSERT(loop->basic_ivs[0].step_const == -2);

    xi_loopinfo_free(info);
    xi_func_free(fx.f);
}

TEST(derived_linear_ivs) {
    LoopFixture fx = {0};
    ASSERT(make_counted_loop(&fx, 1, XI_ADD));

    XiValue *scale = xi_const_int(fx.f, fx.entry, 4, &stub_int);
    XiValue *offset = xi_const_int(fx.f, fx.entry, 7, &stub_int);
    XiValue *scaled = xi_binary(fx.f, fx.body, XI_MUL, &stub_int, &fx.phi->value, scale);
    XiValue *derived = xi_binary(fx.f, fx.body, XI_ADD, &stub_int, scaled, offset);
    ASSERT(scale != NULL && offset != NULL && scaled != NULL && derived != NULL);
    ASSERT(verify_func(fx.f));

    XiLoopInfo *info = compute_loop_info(fx.f);
    ASSERT(info != NULL);
    XiLoop *loop = info->all_loops[0];
    ASSERT(loop->nbasic_ivs == 1);
    ASSERT(loop->nderived_ivs == 2);
    ASSERT(loop->derived_ivs[0].value == scaled);
    ASSERT(loop->derived_ivs[0].base == &fx.phi->value);
    ASSERT(loop->derived_ivs[0].scale == scale);
    ASSERT(loop->derived_ivs[0].has_const_scale);
    ASSERT(loop->derived_ivs[0].scale_const == 4);
    ASSERT(loop->derived_ivs[0].has_const_offset);
    ASSERT(loop->derived_ivs[0].offset_const == 0);
    ASSERT(loop->derived_ivs[1].value == derived);
    ASSERT(loop->derived_ivs[1].base == &fx.phi->value);
    ASSERT(loop->derived_ivs[1].scale == scale);
    ASSERT(loop->derived_ivs[1].offset == offset);
    ASSERT(loop->derived_ivs[1].has_const_offset);
    ASSERT(loop->derived_ivs[1].offset_const == 7);

    xi_loopinfo_free(info);
    xi_func_free(fx.f);
}

TEST(polynomial_iv_marker) {
    LoopFixture fx = {0};
    ASSERT(make_counted_loop(&fx, 1, XI_ADD));

    XiValue *square = xi_binary(fx.f, fx.body, XI_MUL, &stub_int, &fx.phi->value, &fx.phi->value);
    ASSERT(square != NULL);
    ASSERT(verify_func(fx.f));

    XiLoopInfo *info = compute_loop_info(fx.f);
    ASSERT(info != NULL);
    XiLoop *loop = info->all_loops[0];
    ASSERT(loop->nbasic_ivs == 1);
    ASSERT(loop->nderived_ivs == 0);
    ASSERT(loop->npolynomial_ivs == 1);
    ASSERT(loop->polynomial_ivs[0].value == square);
    ASSERT(loop->polynomial_ivs[0].base == &fx.phi->value);

    xi_loopinfo_free(info);
    xi_func_free(fx.f);
}

/* ========== Trip Count Tests ========== */

typedef struct TripCountFixture {
    XiFunc *f;
    XiBlock *entry;
    XiBlock *header;
    XiBlock *body;
    XiBlock *exit_blk;
    XiPhi *phi;
    XiValue *start_val;
    XiValue *limit_val;
    XiValue *step_val;
    XiValue *next;
    XiValue *cond;
} TripCountFixture;

static bool make_trip_count_loop(TripCountFixture *fx, int64_t start, int64_t limit, int64_t step,
                                 uint16_t step_op, uint16_t cmp_op) {
    fx->f = xi_func_new("trip_count_test", &stub_int);
    if (!fx->f)
        return false;
    fx->entry = xi_block_new(fx->f);
    fx->header = xi_block_new(fx->f);
    fx->body = xi_block_new(fx->f);
    fx->exit_blk = xi_block_new(fx->f);
    if (!fx->entry || !fx->header || !fx->body || !fx->exit_blk)
        return false;

    fx->start_val = xi_const_int(fx->f, fx->entry, start, &stub_int);
    fx->limit_val = xi_const_int(fx->f, fx->header, limit, &stub_int);
    fx->step_val = xi_const_int(fx->f, fx->entry, step, &stub_int);
    if (!fx->start_val || !fx->limit_val || !fx->step_val)
        return false;

    xi_block_set_jump(fx->entry, fx->header);

    fx->phi = xi_phi_new(fx->f, fx->header, &stub_int, fx->header->npreds + 1);
    if (!fx->phi)
        return false;

    fx->cond = xi_binary(fx->f, fx->header, cmp_op, &stub_bool, &fx->phi->value, fx->limit_val);
    if (!fx->cond)
        return false;

    xi_block_set_if(fx->header, fx->cond, fx->body, fx->exit_blk);
    xi_block_set_jump(fx->body, fx->header);

    fx->next = xi_binary(fx->f, fx->body, step_op, &stub_int, &fx->phi->value, fx->step_val);
    if (!fx->next)
        return false;

    int pre_idx = pred_index(fx->header, fx->entry);
    int latch_idx = pred_index(fx->header, fx->body);
    if (pre_idx < 0 || latch_idx < 0)
        return false;
    fx->phi->value.args[pre_idx] = fx->start_val;
    fx->phi->value.args[latch_idx] = fx->next;

    xi_block_set_return(fx->exit_blk, &fx->phi->value);
    fx->entry->sealed = true;
    fx->header->sealed = true;
    fx->body->sealed = true;
    fx->exit_blk->sealed = true;
    return true;
}

TEST(trip_count_simple_lt) {
    TripCountFixture fx = {0};
    ASSERT(make_trip_count_loop(&fx, 0, 10, 1, XI_ADD, XI_LT));
    ASSERT(verify_func(fx.f));

    XiLoopInfo *info = compute_loop_info(fx.f);
    ASSERT(info != NULL);
    ASSERT(info->nloop == 1);
    XiLoop *loop = info->all_loops[0];
    ASSERT(loop->has_trip_count);
    ASSERT(loop->trip_count == 10);

    xi_loopinfo_free(info);
    xi_func_free(fx.f);
}

TEST(trip_count_le) {
    TripCountFixture fx = {0};
    ASSERT(make_trip_count_loop(&fx, 0, 9, 1, XI_ADD, XI_LE));
    ASSERT(verify_func(fx.f));

    XiLoopInfo *info = compute_loop_info(fx.f);
    ASSERT(info != NULL);
    XiLoop *loop = info->all_loops[0];
    ASSERT(loop->has_trip_count);
    ASSERT(loop->trip_count == 10);

    xi_loopinfo_free(info);
    xi_func_free(fx.f);
}

TEST(trip_count_step_2) {
    TripCountFixture fx = {0};
    ASSERT(make_trip_count_loop(&fx, 0, 10, 2, XI_ADD, XI_LT));
    ASSERT(verify_func(fx.f));

    XiLoopInfo *info = compute_loop_info(fx.f);
    ASSERT(info != NULL);
    XiLoop *loop = info->all_loops[0];
    ASSERT(loop->has_trip_count);
    ASSERT(loop->trip_count == 5);

    xi_loopinfo_free(info);
    xi_func_free(fx.f);
}

TEST(trip_count_step_3_uneven) {
    TripCountFixture fx = {0};
    ASSERT(make_trip_count_loop(&fx, 0, 10, 3, XI_ADD, XI_LT));
    ASSERT(verify_func(fx.f));

    XiLoopInfo *info = compute_loop_info(fx.f);
    ASSERT(info != NULL);
    XiLoop *loop = info->all_loops[0];
    ASSERT(loop->has_trip_count);
    ASSERT(loop->trip_count == 4);

    xi_loopinfo_free(info);
    xi_func_free(fx.f);
}

TEST(trip_count_nonzero_start) {
    TripCountFixture fx = {0};
    ASSERT(make_trip_count_loop(&fx, 5, 15, 1, XI_ADD, XI_LT));
    ASSERT(verify_func(fx.f));

    XiLoopInfo *info = compute_loop_info(fx.f);
    ASSERT(info != NULL);
    XiLoop *loop = info->all_loops[0];
    ASSERT(loop->has_trip_count);
    ASSERT(loop->trip_count == 10);

    xi_loopinfo_free(info);
    xi_func_free(fx.f);
}

TEST(trip_count_countdown_sub) {
    TripCountFixture fx = {0};
    ASSERT(make_trip_count_loop(&fx, 10, 0, 1, XI_SUB, XI_GT));
    ASSERT(verify_func(fx.f));

    XiLoopInfo *info = compute_loop_info(fx.f);
    ASSERT(info != NULL);
    XiLoop *loop = info->all_loops[0];
    ASSERT(loop->has_trip_count);
    ASSERT(loop->trip_count == 10);

    xi_loopinfo_free(info);
    xi_func_free(fx.f);
}

TEST(trip_count_countdown_ge) {
    TripCountFixture fx = {0};
    ASSERT(make_trip_count_loop(&fx, 10, 1, 1, XI_SUB, XI_GE));
    ASSERT(verify_func(fx.f));

    XiLoopInfo *info = compute_loop_info(fx.f);
    ASSERT(info != NULL);
    XiLoop *loop = info->all_loops[0];
    ASSERT(loop->has_trip_count);
    ASSERT(loop->trip_count == 10);

    xi_loopinfo_free(info);
    xi_func_free(fx.f);
}

TEST(trip_count_unknown_non_const_bound) {
    LoopFixture fx = {0};
    ASSERT(make_counted_loop(&fx, 1, XI_ADD));
    ASSERT(verify_func(fx.f));

    XiLoopInfo *info = compute_loop_info(fx.f);
    ASSERT(info != NULL);
    XiLoop *loop = info->all_loops[0];
    ASSERT(!loop->has_trip_count);
    ASSERT(loop->trip_count == 0);

    xi_loopinfo_free(info);
    xi_func_free(fx.f);
}

TEST(trip_count_ne_exact) {
    TripCountFixture fx = {0};
    ASSERT(make_trip_count_loop(&fx, 0, 10, 2, XI_ADD, XI_NE));
    ASSERT(verify_func(fx.f));

    XiLoopInfo *info = compute_loop_info(fx.f);
    ASSERT(info != NULL);
    XiLoop *loop = info->all_loops[0];
    ASSERT(loop->has_trip_count);
    ASSERT(loop->trip_count == 5);

    xi_loopinfo_free(info);
    xi_func_free(fx.f);
}

TEST(trip_count_ne_inexact_rejected) {
    TripCountFixture fx = {0};
    ASSERT(make_trip_count_loop(&fx, 0, 10, 3, XI_ADD, XI_NE));
    ASSERT(verify_func(fx.f));

    XiLoopInfo *info = compute_loop_info(fx.f);
    ASSERT(info != NULL);
    XiLoop *loop = info->all_loops[0];
    ASSERT(!loop->has_trip_count);

    xi_loopinfo_free(info);
    xi_func_free(fx.f);
}

TEST(trip_count_api_direct_call) {
    TripCountFixture fx = {0};
    ASSERT(make_trip_count_loop(&fx, 0, 4, 1, XI_ADD, XI_LT));
    ASSERT(verify_func(fx.f));

    XiLoopInfo *info = compute_loop_info(fx.f);
    ASSERT(info != NULL);
    XiLoop *loop = info->all_loops[0];

    uint32_t tc = xi_loop_trip_count(loop);
    ASSERT(tc == 4);
    ASSERT(loop->has_trip_count);
    ASSERT(loop->trip_count == tc);

    xi_loopinfo_free(info);
    xi_func_free(fx.f);
}

TEST(trip_count_null_safety) {
    ASSERT(xi_loop_trip_count(NULL) == 0);
}

TEST(reject_loop_variant_step) {
    LoopFixture fx = {0};
    ASSERT(make_counted_loop(&fx, 1, XI_ADD));

    XiValue *variant_step = xi_binary(fx.f, fx.body, XI_ADD, &stub_int, fx.step, fx.step);
    ASSERT(variant_step != NULL);
    fx.next->args[1] = variant_step;
    ASSERT(verify_func(fx.f));

    XiLoopInfo *info = compute_loop_info(fx.f);
    ASSERT(info != NULL);
    XiLoop *loop = info->all_loops[0];
    ASSERT(loop->nbasic_ivs == 0);
    ASSERT(loop->nderived_ivs == 0);
    ASSERT(loop->npolynomial_ivs == 0);

    xi_loopinfo_free(info);
    xi_func_free(fx.f);
}

int main(void) {
    printf("=== Xi Loop IV Tests ===\n\n");

    run_basic_iv_add_step();
    run_basic_iv_sub_step();
    run_derived_linear_ivs();
    run_polynomial_iv_marker();
    run_reject_loop_variant_step();

    printf("\n--- Trip Count Tests ---\n\n");
    run_trip_count_simple_lt();
    run_trip_count_le();
    run_trip_count_step_2();
    run_trip_count_step_3_uneven();
    run_trip_count_nonzero_start();
    run_trip_count_countdown_sub();
    run_trip_count_countdown_ge();
    run_trip_count_unknown_non_const_bound();
    run_trip_count_ne_exact();
    run_trip_count_ne_inexact_rejected();
    run_trip_count_api_direct_call();
    run_trip_count_null_safety();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
