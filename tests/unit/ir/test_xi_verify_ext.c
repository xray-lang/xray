/*
 * Unit tests for extended Xi verifier contracts.
 * Covers bounds checks, tail-call safety, TBAA metadata, and backend legality.
 */

#include "../../../src/ir/xi_verify.h"
#include "../../../src/ir/xi_tbaa.h"
#include "../../../src/ir/xi.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_func = {.kind = XR_KIND_FUNCTION, .id = 2, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 3, .frozen = true};

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

static XiFunc *make_func(const char *name) {
    XiFunc *f = xi_func_new(name, &stub_int);
    if (!f)
        return NULL;
    XiBlock *entry = xi_block_new(f);
    if (!entry) {
        xi_func_free(f);
        return NULL;
    }
    entry->sealed = true;
    return f;
}

static bool verify_ok(const XiFunc *f) {
    char err[256] = {0};
    return xi_verify(f, err, sizeof(err));
}

static bool verify_fail(const XiFunc *f) {
    char err[256] = {0};
    bool ok = xi_verify(f, err, sizeof(err));
    if (ok)
        return false;
    return err[0] != '\0';
}

static bool verify_stage_fail(const XiFunc *f, XiStage stage) {
    char err[256] = {0};
    bool ok = xi_verify_stage(f, stage, err, sizeof(err));
    if (ok)
        return false;
    return err[0] != '\0';
}

/* ========== Bounds check contracts ========== */

TEST(bounds_check_valid) {
    XiFunc *f = make_func("bounds_valid");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *idx = xi_const_int(f, entry, 2, &stub_int);
    XiValue *len = xi_const_int(f, entry, 8, &stub_int);
    XiValue *bc = xi_value_new(f, entry, XI_BOUNDS_CHECK, &stub_int, 2);
    bc->args[0] = idx;
    bc->args[1] = len;
    xi_block_set_return(entry, bc);

    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(bounds_check_arity_failure) {
    XiFunc *f = make_func("bounds_arity");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *idx = xi_const_int(f, entry, 2, &stub_int);
    XiValue *bc = xi_value_new(f, entry, XI_BOUNDS_CHECK, &stub_int, 1);
    bc->args[0] = idx;
    xi_block_set_return(entry, bc);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(bounds_check_effect_failure) {
    XiFunc *f = make_func("bounds_effect");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *idx = xi_const_int(f, entry, 2, &stub_int);
    XiValue *len = xi_const_int(f, entry, 8, &stub_int);
    XiValue *bc = xi_value_new(f, entry, XI_BOUNDS_CHECK, &stub_int, 2);
    bc->args[0] = idx;
    bc->args[1] = len;
    bc->flags = 0;
    xi_block_set_return(entry, bc);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

/* ========== Tail-call flag safety ========== */

TEST(tail_flag_on_non_call_fails) {
    XiFunc *f = make_func("tail_non_call");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *v = xi_const_int(f, entry, 1, &stub_int);
    v->flags |= XI_FLAG_TAIL;
    xi_block_set_return(entry, v);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(tail_call_with_non_function_callee_fails) {
    XiFunc *f = make_func("tail_bad_callee");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *callee = xi_const_int(f, entry, 1, &stub_int);
    XiValue *call = xi_value_new(f, entry, XI_CALL, &stub_int, 1);
    call->args[0] = callee;
    call->flags |= XI_FLAG_TAIL;
    xi_block_set_return(entry, call);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(tail_call_with_function_callee_passes) {
    XiFunc *f = make_func("tail_good_callee");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *callee = xi_value_new(f, entry, XI_CLOSURE_NEW, &stub_func, 0);
    XiValue *call = xi_value_new(f, entry, XI_CALL, &stub_int, 1);
    call->args[0] = callee;
    call->flags |= XI_FLAG_TAIL;
    xi_block_set_return(entry, call);

    ASSERT(verify_ok(f));
    xi_func_free(f);
}

/* ========== TBAA metadata consistency ========== */

TEST(tbaa_memory_op_requires_mem_group) {
    XiFunc *f = make_func("tbaa_missing_group");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *arr = xi_const_int(f, entry, 0, &stub_int);
    XiValue *idx = xi_const_int(f, entry, 1, &stub_int);
    XiValue *load = xi_value_new(f, entry, XI_INDEX_GET, &stub_int, 2);
    load->args[0] = arr;
    load->args[1] = idx;
    load->mem_group = XI_MEM_NONE;
    f->invariant_mask |= XI_INV_TBAA_ANNOTATED;
    xi_block_set_return(entry, load);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(tbaa_memory_op_with_group_passes) {
    XiFunc *f = make_func("tbaa_with_group");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *arr = xi_const_int(f, entry, 0, &stub_int);
    XiValue *idx = xi_const_int(f, entry, 1, &stub_int);
    XiValue *load = xi_value_new(f, entry, XI_INDEX_GET, &stub_int, 2);
    load->args[0] = arr;
    load->args[1] = idx;
    load->mem_group = XI_MEM_ARRAY;
    f->invariant_mask |= XI_INV_TBAA_ANNOTATED;
    xi_block_set_return(entry, load);

    ASSERT(verify_ok(f));
    xi_func_free(f);
}

/* ========== Type contracts ========== */

TEST(select_with_non_bool_condition_fails) {
    XiFunc *f = make_func("select_bad_cond");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    /* cond is an int constant, not bool. SELECT must reject this. */
    XiValue *cond = xi_const_int(f, entry, 1, &stub_int);
    XiValue *t = xi_const_int(f, entry, 10, &stub_int);
    XiValue *fv = xi_const_int(f, entry, 20, &stub_int);
    XiValue *sel = xi_value_new(f, entry, XI_SELECT, &stub_int, 3);
    sel->args[0] = cond;
    sel->args[1] = t;
    sel->args[2] = fv;
    xi_block_set_return(entry, sel);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(select_with_bool_condition_passes) {
    XiFunc *f = make_func("select_good_cond");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *cond = xi_const_bool(f, entry, true, &stub_bool);
    XiValue *t = xi_const_int(f, entry, 10, &stub_int);
    XiValue *fv = xi_const_int(f, entry, 20, &stub_int);
    XiValue *sel = xi_value_new(f, entry, XI_SELECT, &stub_int, 3);
    sel->args[0] = cond;
    sel->args[1] = t;
    sel->args[2] = fv;
    xi_block_set_return(entry, sel);

    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(extract_from_non_call_fails) {
    XiFunc *f = make_func("extract_bad_source");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    /* EXTRACT must take a call/multi_ret as its source. A const int
     * is illegal and the verifier must report it. */
    XiValue *src = xi_const_int(f, entry, 7, &stub_int);
    XiValue *ext = xi_value_new(f, entry, XI_EXTRACT, &stub_int, 1);
    ext->args[0] = src;
    ext->aux_int = 0;
    xi_block_set_return(entry, ext);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(comparison_must_produce_bool_fails) {
    XiFunc *f = make_func("eq_bad_result_type");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    /* XI_EQ is a bool-producing op; result type must be bool / unknown. */
    XiValue *a = xi_const_int(f, entry, 1, &stub_int);
    XiValue *b = xi_const_int(f, entry, 2, &stub_int);
    XiValue *eq = xi_value_new(f, entry, XI_EQ, &stub_int, 2);
    eq->args[0] = a;
    eq->args[1] = b;
    xi_block_set_return(entry, eq);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

/* ========== XI_CALL_METHOD contract ========== */

TEST(call_method_missing_aux_fails) {
    XiFunc *f = make_func("call_method_no_aux");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    /* aux must carry the method name string; NULL is a hard error. */
    XiValue *recv = xi_const_int(f, entry, 0, &stub_int);
    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, &stub_int, 1);
    call->args[0] = recv;
    call->aux = NULL;
    call->aux_int = 0; /* sym=0, is_super=0 */
    xi_block_set_return(entry, call);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(call_method_zero_args_fails) {
    XiFunc *f = make_func("call_method_no_recv");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    /* CALL_METHOD must have at least the receiver argument. */
    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, &stub_int, 0);
    call->aux = (void *) "foo";
    call->aux_int = 0;
    xi_block_set_return(entry, call);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

/* ========== Unique value IDs ========== */

TEST(duplicate_value_id_fails) {
    XiFunc *f = make_func("dup_value_id");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *a = xi_const_int(f, entry, 1, &stub_int);
    XiValue *b = xi_const_int(f, entry, 2, &stub_int);
    /* Force a duplicate SSA id within the function. */
    b->id = a->id;
    xi_block_set_return(entry, b);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(phi_arg_count_mismatch_fails) {
    XiFunc *f = make_func("phi_arg_count");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiBlock *merge = xi_block_new(f);
    ASSERT(merge != NULL);

    xi_block_set_jump(entry, merge);
    XiPhi *phi = xi_phi_new(f, merge, &stub_int, 0);
    ASSERT(phi != NULL);
    xi_block_set_return(merge, &phi->value);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(use_not_dominated_by_def_fails) {
    XiFunc *f = make_func("dom_bad_use");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    ASSERT(then_blk != NULL);
    ASSERT(else_blk != NULL);

    XiValue *cond = xi_const_bool(f, entry, true, &stub_bool);
    xi_block_set_if(entry, cond, then_blk, else_blk);
    XiValue *then_val = xi_const_int(f, then_blk, 1, &stub_int);
    xi_block_set_return(then_blk, then_val);
    XiValue *else_val = xi_const_int(f, else_blk, 2, &stub_int);
    XiValue *bad_use = xi_value_new(f, else_blk, XI_ADD, &stub_int, 2);
    bad_use->args[0] = then_val;
    bad_use->args[1] = else_val;
    xi_block_set_return(else_blk, bad_use);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(closed_stage_rejects_bad_upval_index) {
    XiFunc *f = make_func("closed_bad_upval");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *up = xi_value_new(f, entry, XI_LOAD_UPVAL, &stub_int, 0);
    ASSERT(up != NULL);
    up->aux_int = 0;
    xi_block_set_return(entry, up);
    f->stage = XI_STAGE_CLOSED;
    f->invariant_mask = xi_stage_invariants(XI_STAGE_CLOSED);

    ASSERT(verify_stage_fail(f, XI_STAGE_CLOSED));
    xi_func_free(f);
}

TEST(repped_stage_rejects_box_i64_rep) {
    XiFunc *f = make_func("repped_bad_box");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *src = xi_const_int(f, entry, 1, &stub_int);
    XiValue *box = xi_value_new(f, entry, XI_BOX, &stub_int, 1);
    ASSERT(box != NULL);
    box->args[0] = src;
    box->rep = XR_REP_I64;
    xi_block_set_return(entry, box);
    f->stage = XI_STAGE_REPPED;
    f->invariant_mask = xi_stage_invariants(XI_STAGE_REPPED);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(stage_invariant_mask_missing_bits_fails) {
    XiFunc *f = make_func("stage_missing_bits");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *v = xi_const_int(f, entry, 1, &stub_int);
    xi_block_set_return(entry, v);
    f->stage = XI_STAGE_CANONICAL;
    f->invariant_mask = xi_stage_invariants(XI_STAGE_RAW);

    ASSERT(verify_stage_fail(f, XI_STAGE_CANONICAL));
    xi_func_free(f);
}

/* ========== Backend legality ========== */

TEST(backend_rejects_unlowered_alloc) {
    XiFunc *f = make_func("backend_illegal");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *size = xi_const_int(f, entry, 4, &stub_int);
    XiValue *arr = xi_value_new(f, entry, XI_ARRAY_NEW, &stub_int, 1);
    arr->args[0] = size;
    xi_block_set_return(entry, arr);
    f->stage = XI_STAGE_BACKEND;

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi Extended Verifier Tests ===\n\n");

    run_bounds_check_valid();
    run_bounds_check_arity_failure();
    run_bounds_check_effect_failure();
    run_tail_flag_on_non_call_fails();
    run_tail_call_with_non_function_callee_fails();
    run_tail_call_with_function_callee_passes();
    run_tbaa_memory_op_requires_mem_group();
    run_tbaa_memory_op_with_group_passes();
    run_select_with_non_bool_condition_fails();
    run_select_with_bool_condition_passes();
    run_extract_from_non_call_fails();
    run_comparison_must_produce_bool_fails();
    run_call_method_missing_aux_fails();
    run_call_method_zero_args_fails();
    run_duplicate_value_id_fails();
    run_phi_arg_count_mismatch_fails();
    run_use_not_dominated_by_def_fails();
    run_closed_stage_rejects_bad_upval_index();
    run_repped_stage_rejects_box_i64_rep();
    run_stage_invariant_mask_missing_bits_fails();
    run_backend_rejects_unlowered_alloc();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
