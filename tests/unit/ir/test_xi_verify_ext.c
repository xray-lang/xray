/*
 * Unit tests for extended Xi verifier contracts.
 * Covers bounds checks, tail-call safety, TBAA metadata, and backend legality.
 */

#include "../../../src/ir/xi_verify.h"
#include "../../../src/ir/xi_tbaa.h"
#include "../../../src/ir/xi_backend.h"
#include "../../../src/ir/xi.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_func = {.kind = XR_KIND_FUNCTION, .id = 2, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 3, .frozen = true};
static XrType stub_str = {.kind = XR_KIND_STRING, .id = 4, .frozen = true};
static XrType stub_i8 = {
    .kind = XR_KIND_INT, .id = 5, .frozen = true, .native_width = XR_NATIVE_I8};
static XrType stub_u16 = {
    .kind = XR_KIND_INT, .id = 11, .frozen = true, .native_width = XR_NATIVE_U16};
static XrType stub_u64 = {
    .kind = XR_KIND_INT, .id = 6, .frozen = true, .native_width = XR_NATIVE_U64};
static XrType stub_unit = {.kind = XR_KIND_UNIT, .id = 9, .frozen = true};
static XrType stub_null = {.kind = XR_KIND_NULL, .id = 10, .frozen = true};
static XrType stub_array_i8 = {
    .kind = XR_KIND_ARRAY, .id = 7, .frozen = true, .container = {.element_type = &stub_i8}};
static XrType stub_array_u64 = {
    .kind = XR_KIND_ARRAY, .id = 8, .frozen = true, .container = {.element_type = &stub_u64}};

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

static void make_if_returning_ints(XiFunc *f, XiValue *cond) {
    XiBlock *entry = f->entry;
    XiBlock *then_b = xi_block_new(f);
    XiBlock *else_b = xi_block_new(f);
    XiValue *then_v = xi_const_int(f, then_b, 1, &stub_int);
    XiValue *else_v = xi_const_int(f, else_b, 0, &stub_int);
    xi_block_set_return(then_b, then_v);
    xi_block_set_return(else_b, else_v);
    xi_block_set_if(entry, cond, then_b, else_b);
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

TEST(tbaa_store_requires_mem_group) {
    XiFunc *f = make_func("tbaa_store_missing_group");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *arr = xi_const_int(f, entry, 0, &stub_int);
    XiValue *idx = xi_const_int(f, entry, 1, &stub_int);
    XiValue *val = xi_const_int(f, entry, 42, &stub_int);
    XiValue *store = xi_value_new(f, entry, XI_INDEX_SET, &stub_unit, 3);
    store->args[0] = arr;
    store->args[1] = idx;
    store->args[2] = val;
    store->mem_group = XI_MEM_NONE;
    f->invariant_mask |= XI_INV_TBAA_ANNOTATED;
    xi_block_set_return(entry, val);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(tbaa_non_memory_op_with_group_fails) {
    XiFunc *f = make_func("tbaa_non_memory_with_group");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *a = xi_const_int(f, entry, 1, &stub_int);
    XiValue *b = xi_const_int(f, entry, 2, &stub_int);
    XiValue *add = xi_value_new(f, entry, XI_ADD, &stub_int, 2);
    add->args[0] = a;
    add->args[1] = b;
    add->mem_group = XI_MEM_ARRAY;
    f->invariant_mask |= XI_INV_TBAA_ANNOTATED;
    xi_block_set_return(entry, add);

    ASSERT(verify_fail(f));
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

TEST(if_with_int_condition_passes) {
    XiFunc *f = make_func("if_int_cond");
    ASSERT(f != NULL);

    XiValue *cond = xi_const_int(f, f->entry, 1, &stub_int);
    make_if_returning_ints(f, cond);

    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(if_with_string_condition_passes) {
    XiFunc *f = make_func("if_string_cond");
    ASSERT(f != NULL);

    XiValue *cond = xi_const_str(f, f->entry, "x", &stub_str);
    make_if_returning_ints(f, cond);

    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(if_with_null_condition_passes) {
    XiFunc *f = make_func("if_null_cond");
    ASSERT(f != NULL);

    XiValue *cond = xi_const_null(f, f->entry, &stub_null);
    make_if_returning_ints(f, cond);

    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(select_with_incompatible_arm_fails) {
    XiFunc *f = make_func("select_bad_arm");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *cond = xi_const_bool(f, entry, true, &stub_bool);
    XiValue *t = xi_const_int(f, entry, 10, &stub_int);
    XiValue *fv = xi_const_str(f, entry, "bad", &stub_str);
    XiValue *sel = xi_value_new(f, entry, XI_SELECT, &stub_int, 3);
    sel->args[0] = cond;
    sel->args[1] = t;
    sel->args[2] = fv;
    xi_block_set_return(entry, sel);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(if_with_unit_control_fails) {
    XiFunc *f = make_func("if_unit_cond");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    ASSERT(then_blk != NULL);
    ASSERT(else_blk != NULL);

    XiValue *cond = xi_value_new(f, entry, XI_CONST, &stub_unit, 0);
    xi_block_set_if(entry, cond, then_blk, else_blk);
    xi_block_set_return(then_blk, xi_const_int(f, then_blk, 1, &stub_int));
    xi_block_set_return(else_blk, xi_const_int(f, else_blk, 0, &stub_int));

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(obsolete_multi_return_ops_fail) {
    XiFunc *f = make_func("obsolete_extract_op");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *src = xi_const_int(f, entry, 1, &stub_int);
    XiValue *ext = xi_value_new(f, entry, XI_EXTRACT, &stub_int, 1);
    ASSERT(src != NULL);
    ASSERT(ext != NULL);
    ext->args[0] = src;
    xi_block_set_return(entry, ext);

    ASSERT(verify_fail(f));
    xi_func_free(f);

    f = make_func("obsolete_multi_ret_op");
    ASSERT(f != NULL);
    entry = f->entry;

    src = xi_const_int(f, entry, 2, &stub_int);
    XiValue *mret = xi_value_new(f, entry, XI_MULTI_RET, &stub_int, 1);
    ASSERT(src != NULL);
    ASSERT(mret != NULL);
    mret->args[0] = src;
    xi_block_set_return(entry, mret);

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

TEST(call_method_direct_missing_aux_fails) {
    XiFunc *f = make_func("call_method_direct_no_aux");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *recv = xi_const_int(f, entry, 0, &stub_int);
    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD_DIRECT, &stub_int, 1);
    call->args[0] = recv;
    call->aux = NULL;
    call->aux_int = 0;
    xi_block_set_return(entry, call);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(call_method_direct_bad_index_fails) {
    XiFunc *f = make_func("call_method_direct_bad_index");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *recv = xi_const_int(f, entry, 0, &stub_int);
    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD_DIRECT, &stub_int, 1);
    call->args[0] = recv;
    call->aux = (void *) "run";
    call->aux_int = 256;
    xi_block_set_return(entry, call);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(call_method_direct_zero_args_fails) {
    XiFunc *f = make_func("call_method_direct_no_recv");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD_DIRECT, &stub_int, 0);
    call->aux = (void *) "run";
    call->aux_int = 0;
    xi_block_set_return(entry, call);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(typed_array_store_without_narrow_fails) {
    XiFunc *f = make_func("typed_store_no_narrow");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *arr = xi_param(f, entry, 0, &stub_array_i8);
    XiValue *idx = xi_const_int(f, entry, 0, &stub_int);
    XiValue *val = xi_const_int(f, entry, 257, &stub_int);
    XiValue *store = xi_value_new(f, entry, XI_INDEX_SET, &stub_int, 3);
    store->args[0] = arr;
    store->args[1] = idx;
    store->args[2] = val;
    xi_block_set_return(entry, store);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(typed_array_store_with_narrow_passes) {
    XiFunc *f = make_func("typed_store_narrow");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *arr = xi_param(f, entry, 0, &stub_array_i8);
    XiValue *idx = xi_const_int(f, entry, 0, &stub_int);
    XiValue *val = xi_const_int(f, entry, 257, &stub_int);
    XiValue *narrow = xi_value_new(f, entry, XI_NARROW_I8, &stub_int, 1);
    narrow->args[0] = val;
    XiValue *store = xi_value_new(f, entry, XI_INDEX_SET, &stub_int, 3);
    store->args[0] = arr;
    store->args[1] = idx;
    store->args[2] = narrow;
    xi_block_set_return(entry, store);

    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(typed_array_store_with_wrong_narrow_fails) {
    XiFunc *f = make_func("typed_store_wrong_narrow");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *arr = xi_param(f, entry, 0, &stub_array_i8);
    XiValue *idx = xi_const_int(f, entry, 0, &stub_int);
    XiValue *val = xi_const_int(f, entry, 257, &stub_int);
    XiValue *narrow = xi_value_new(f, entry, XI_NARROW_U16, &stub_u16, 1);
    narrow->args[0] = val;
    XiValue *store = xi_value_new(f, entry, XI_INDEX_SET, &stub_int, 3);
    store->args[0] = arr;
    store->args[1] = idx;
    store->args[2] = narrow;
    xi_block_set_return(entry, store);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(typed_array_store_u64_without_narrow_passes) {
    XiFunc *f = make_func("typed_store_u64");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *arr = xi_param(f, entry, 0, &stub_array_u64);
    XiValue *idx = xi_const_int(f, entry, 0, &stub_int);
    XiValue *val = xi_const_int(f, entry, 257, &stub_int);
    XiValue *store = xi_value_new(f, entry, XI_INDEX_SET, &stub_int, 3);
    store->args[0] = arr;
    store->args[1] = idx;
    store->args[2] = val;
    xi_block_set_return(entry, store);

    ASSERT(verify_ok(f));
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

TEST(phi_arg_not_dominated_by_pred_fails) {
    /*
     * entry --(if)--> a_blk and b_blk; both jump to c_blk.
     * c_blk has phi(arg0 from a_blk, arg1 from b_blk) but both args
     * reference v_a defined in a_blk.  arg[1] is sourced from pred b_blk,
     * and a_blk does not dominate b_blk -> verifier must reject.
     */
    XiFunc *f = make_func("phi_arg_bad_dom");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiBlock *a_blk = xi_block_new(f);
    XiBlock *b_blk = xi_block_new(f);
    XiBlock *c_blk = xi_block_new(f);
    ASSERT(a_blk != NULL);
    ASSERT(b_blk != NULL);
    ASSERT(c_blk != NULL);

    XiValue *e_cond = xi_const_bool(f, entry, true, &stub_bool);
    xi_block_set_if(entry, e_cond, a_blk, b_blk);

    XiValue *v_a = xi_const_int(f, a_blk, 1, &stub_int);
    xi_block_set_jump(a_blk, c_blk);
    XiValue *v_b = xi_const_int(f, b_blk, 2, &stub_int);
    (void) v_b;
    xi_block_set_jump(b_blk, c_blk);

    XiPhi *phi = xi_phi_new(f, c_blk, &stub_int, 2);
    ASSERT(phi != NULL);
    /* arg[1] sources v_a (defined in a_blk) from pred b_blk; illegal. */
    phi->value.args[0] = v_a;
    phi->value.args[1] = v_a;
    xi_block_set_return(c_blk, &phi->value);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(block_id_mismatch_with_array_index_fails) {
    /*
     * Xi IR's per-block scratch arrays in SCCP / xi_loop / codegen all
     * index by block->id.  Verifier must reject any function whose
     * block->id does not equal its position in f->blocks[].
     */
    XiFunc *f = make_func("blocks_id_mismatch");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiBlock *tail = xi_block_new(f);
    ASSERT(tail != NULL);

    XiValue *v = xi_const_int(f, entry, 1, &stub_int);
    xi_block_set_jump(entry, tail);
    xi_block_set_return(tail, v);

    /* Force the per-block id to drift from its array index. */
    tail->id = 99;

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(block_control_not_dominated_by_def_fails) {
    /*
     * entry --(if)--> a_blk and b_blk; a_blk defines cond_a then returns,
     * b_blk uses cond_a as its IF condition.  cond_a's defining block
     * (a_blk) does not dominate b_blk -> verifier must reject control
     * value not dominated by its definition.
     */
    XiFunc *f = make_func("control_bad_dom");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiBlock *a_blk = xi_block_new(f);
    XiBlock *b_blk = xi_block_new(f);
    XiBlock *then_b = xi_block_new(f);
    XiBlock *else_b = xi_block_new(f);
    ASSERT(a_blk != NULL);
    ASSERT(b_blk != NULL);
    ASSERT(then_b != NULL);
    ASSERT(else_b != NULL);

    XiValue *e_cond = xi_const_bool(f, entry, true, &stub_bool);
    xi_block_set_if(entry, e_cond, a_blk, b_blk);

    XiValue *cond_a = xi_const_bool(f, a_blk, true, &stub_bool);
    xi_block_set_return(a_blk, xi_const_int(f, a_blk, 1, &stub_int));

    /* b_blk uses cond_a even though a_blk does not dominate b_blk. */
    xi_block_set_if(b_blk, cond_a, then_b, else_b);
    xi_block_set_return(then_b, xi_const_int(f, then_b, 2, &stub_int));
    xi_block_set_return(else_b, xi_const_int(f, else_b, 3, &stub_int));

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

TEST(backend_rejects_unlowered_iter_op) {
    XiFunc *f = make_func("backend_unlowered_iter_op");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *source = xi_const_int(f, entry, 4, &stub_int);
    XiValue *iter = xi_value_new(f, entry, XI_ITER_NEW, &stub_int, 1);
    iter->args[0] = source;
    xi_block_set_return(entry, iter);
    f->stage = XI_STAGE_BACKEND;
    f->invariant_mask = xi_stage_invariants(XI_STAGE_BACKEND);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(backend_rejects_malformed_call_method) {
    XiFunc *f = make_func("backend_malformed_call_method");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *recv = xi_const_int(f, entry, 0, &stub_int);
    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, &stub_int, 2);
    call->args[0] = recv;
    call->args[1] = xi_const_int(f, entry, 0, &stub_int);
    xi_block_set_return(entry, call);
    f->stage = XI_STAGE_BACKEND;
    f->invariant_mask = xi_stage_invariants(XI_STAGE_BACKEND);

    ASSERT(xi_op_is_backend_legal(XI_CALL_METHOD));
    ASSERT(verify_fail(f));
    xi_func_free(f);
}

/* ========== CFG Mutation Negative Tests ========== */

TEST(cfg_entry_with_predecessors_fails) {
    XiFunc *f = make_func("cfg_entry_preds");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiBlock *tail = xi_block_new(f);
    ASSERT(tail != NULL);

    XiValue *v = xi_const_int(f, entry, 1, &stub_int);
    xi_block_set_jump(entry, tail);
    xi_block_set_return(tail, v);

    entry->preds = (XiBlock **) xr_malloc(sizeof(XiBlock *));
    entry->preds[0] = tail;
    entry->npreds = 1;

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(cfg_if_block_null_control_fails) {
    XiFunc *f = make_func("cfg_if_null_ctrl");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiBlock *a = xi_block_new(f);
    XiBlock *b = xi_block_new(f);
    ASSERT(a != NULL && b != NULL);

    xi_block_set_return(a, xi_const_int(f, a, 1, &stub_int));
    xi_block_set_return(b, xi_const_int(f, b, 2, &stub_int));

    entry->kind = XI_BLOCK_IF;
    entry->control = NULL;
    entry->succs[0] = a;
    entry->succs[1] = b;

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(cfg_if_block_missing_successor_fails) {
    XiFunc *f = make_func("cfg_if_missing_succ");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiBlock *a = xi_block_new(f);
    ASSERT(a != NULL);

    XiValue *cond = xi_const_bool(f, entry, true, &stub_bool);
    xi_block_set_return(a, xi_const_int(f, a, 1, &stub_int));

    entry->kind = XI_BLOCK_IF;
    entry->control = cond;
    entry->succs[0] = a;
    entry->succs[1] = NULL;

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(cfg_return_block_with_successors_fails) {
    XiFunc *f = make_func("cfg_ret_with_succ");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiBlock *tail = xi_block_new(f);
    ASSERT(tail != NULL);

    XiValue *v = xi_const_int(f, entry, 1, &stub_int);
    xi_block_set_return(entry, v);
    xi_block_set_return(tail, v);

    entry->succs[0] = tail;

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(cfg_succ_not_in_pred_list_fails) {
    XiFunc *f = make_func("cfg_succ_no_pred");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiBlock *a = xi_block_new(f);
    ASSERT(a != NULL);

    XiValue *v = xi_const_int(f, entry, 1, &stub_int);
    xi_block_set_jump(entry, a);
    xi_block_set_return(a, v);

    a->npreds = 0;

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(cfg_invalid_block_kind_fails) {
    XiFunc *f = make_func("cfg_bad_kind");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *v = xi_const_int(f, entry, 1, &stub_int);
    xi_block_set_return(entry, v);
    entry->kind = 255;

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

/* ========== TBAA Negative Tests (additional) ========== */

TEST(tbaa_field_load_requires_mem_group) {
    XiFunc *f = make_func("tbaa_field_no_group");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *obj = xi_const_int(f, entry, 0, &stub_int);
    XiValue *load = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
    load->args[0] = obj;
    load->aux_int = 0;
    load->mem_group = XI_MEM_NONE;
    f->invariant_mask |= XI_INV_TBAA_ANNOTATED;
    xi_block_set_return(entry, load);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(tbaa_field_store_requires_mem_group) {
    XiFunc *f = make_func("tbaa_field_store_no_group");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *obj = xi_const_int(f, entry, 0, &stub_int);
    XiValue *val = xi_const_int(f, entry, 42, &stub_int);
    XiValue *store = xi_value_new(f, entry, XI_STORE_FIELD, &stub_unit, 2);
    store->args[0] = obj;
    store->args[1] = val;
    store->aux_int = 0;
    store->mem_group = XI_MEM_NONE;
    f->invariant_mask |= XI_INV_TBAA_ANNOTATED;
    xi_block_set_return(entry, val);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(tbaa_upval_load_requires_mem_group) {
    XiFunc *f = make_func("tbaa_upval_no_group");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *up = xi_value_new(f, entry, XI_LOAD_UPVAL, &stub_int, 0);
    up->aux_int = 0;
    up->mem_group = XI_MEM_NONE;
    f->invariant_mask |= XI_INV_TBAA_ANNOTATED;
    xi_block_set_return(entry, up);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

/* ========== Backend Negative Tests (additional) ========== */

TEST(backend_accepts_print_op) {
    XiFunc *f = make_func("backend_print");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *arg = xi_const_int(f, entry, 0, &stub_int);
    XiValue *print = xi_value_new(f, entry, XI_PRINT, &stub_unit, 1);
    print->args[0] = arg;
    print->flags |= XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(entry, arg);
    f->stage = XI_STAGE_BACKEND;
    f->invariant_mask = xi_stage_invariants(XI_STAGE_BACKEND);

    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(backend_accepts_map_new) {
    XiFunc *f = make_func("backend_map_new");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *map = xi_value_new(f, entry, XI_MAP_NEW, &stub_int, 0);
    xi_block_set_return(entry, map);
    f->stage = XI_STAGE_BACKEND;
    f->invariant_mask = xi_stage_invariants(XI_STAGE_BACKEND);

    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(backend_accepts_str_concat) {
    XiFunc *f = make_func("backend_str_concat");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *a = xi_const_str(f, entry, "hello", &stub_str);
    XiValue *b = xi_const_str(f, entry, "world", &stub_str);
    XiValue *concat = xi_value_new(f, entry, XI_STR_CONCAT, &stub_str, 2);
    concat->args[0] = a;
    concat->args[1] = b;
    xi_block_set_return(entry, concat);
    f->stage = XI_STAGE_BACKEND;
    f->invariant_mask = xi_stage_invariants(XI_STAGE_BACKEND);

    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(backend_accepts_range_op) {
    XiFunc *f = make_func("backend_range_op");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *start = xi_const_int(f, entry, 0, &stub_int);
    XiValue *end = xi_const_int(f, entry, 4, &stub_int);
    XiValue *range = xi_value_new(f, entry, XI_RANGE, &stub_int, 2);
    range->args[0] = start;
    range->args[1] = end;
    xi_block_set_return(entry, range);
    f->stage = XI_STAGE_BACKEND;
    f->invariant_mask = xi_stage_invariants(XI_STAGE_BACKEND);

    ASSERT(verify_ok(f));
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
    run_tbaa_store_requires_mem_group();
    run_tbaa_non_memory_op_with_group_fails();
    run_select_with_non_bool_condition_fails();
    run_select_with_bool_condition_passes();
    run_if_with_int_condition_passes();
    run_if_with_string_condition_passes();
    run_if_with_null_condition_passes();
    run_select_with_incompatible_arm_fails();
    run_if_with_unit_control_fails();
    run_obsolete_multi_return_ops_fail();
    run_comparison_must_produce_bool_fails();
    run_call_method_missing_aux_fails();
    run_call_method_zero_args_fails();
    run_call_method_direct_missing_aux_fails();
    run_call_method_direct_bad_index_fails();
    run_call_method_direct_zero_args_fails();
    run_typed_array_store_without_narrow_fails();
    run_typed_array_store_with_narrow_passes();
    run_typed_array_store_with_wrong_narrow_fails();
    run_typed_array_store_u64_without_narrow_passes();
    run_duplicate_value_id_fails();
    run_phi_arg_count_mismatch_fails();
    run_use_not_dominated_by_def_fails();
    run_phi_arg_not_dominated_by_pred_fails();
    run_block_id_mismatch_with_array_index_fails();
    run_block_control_not_dominated_by_def_fails();
    run_closed_stage_rejects_bad_upval_index();
    run_repped_stage_rejects_box_i64_rep();
    run_stage_invariant_mask_missing_bits_fails();
    run_backend_rejects_unlowered_iter_op();
    run_backend_rejects_malformed_call_method();

    printf("\n--- CFG Mutation Negatives ---\n");
    run_cfg_entry_with_predecessors_fails();
    run_cfg_if_block_null_control_fails();
    run_cfg_if_block_missing_successor_fails();
    run_cfg_return_block_with_successors_fails();
    run_cfg_succ_not_in_pred_list_fails();
    run_cfg_invalid_block_kind_fails();

    printf("\n--- TBAA Negatives (additional) ---\n");
    run_tbaa_field_load_requires_mem_group();
    run_tbaa_field_store_requires_mem_group();
    run_tbaa_upval_load_requires_mem_group();

    printf("\n--- Backend Direct Ops And Negatives ---\n");
    run_backend_accepts_print_op();
    run_backend_accepts_map_new();
    run_backend_accepts_str_concat();
    run_backend_accepts_range_op();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
