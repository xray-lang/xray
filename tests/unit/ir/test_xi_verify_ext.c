/*
 * Unit tests for extended Xi verifier contracts.
 * Covers bounds checks, tail-call safety, TBAA metadata, and backend legality.
 */

#include "../../../src/ir/xi_verify.h"
#include "../../../src/ir/xi_evidence.h"
#include "../../../src/ir/xi_tbaa.h"
#include "../../../src/ir/xi_backend.h"
#include "../../../src/ir/xi_coro_analyze.h"
#include "../../../src/ir/xi_coro_lower.h"
#include "../../../src/ir/xi.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/runtime/value/xffi_sig.h"
#include "../../../src/shared/xr_array_core.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_func = {.kind = XR_KIND_FUNCTION, .id = 2, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 3, .frozen = true};
static XrType stub_str = {.kind = XR_KIND_STRING, .id = 4, .frozen = true};
static XrType stub_i8 = {.kind = XR_KIND_INT, .id = 5, .frozen = true, .scalar_rep = XR_NATIVE_I8};
static XrType stub_u16 = {
    .kind = XR_KIND_INT, .id = 11, .frozen = true, .scalar_rep = XR_NATIVE_U16};
static XrType stub_u64 = {
    .kind = XR_KIND_INT, .id = 6, .frozen = true, .scalar_rep = XR_NATIVE_U64};
static XrType stub_unit = {.kind = XR_KIND_UNIT, .id = 9, .frozen = true};
static XrType stub_null = {.kind = XR_KIND_NULL, .id = 10, .frozen = true};
static XrType stub_error = {.kind = XR_KIND_ERROR, .id = 12, .frozen = true};
static XrType stub_usize = {
    .kind = XR_KIND_INT, .id = 14, .frozen = true, .scalar_rep = XR_NATIVE_USIZE};
static XrType stub_pointer = {
    .kind = XR_KIND_POINTER, .id = 15, .frozen = true, .scalar_rep = XR_NATIVE_POINTER};
static XrType stub_array_error = {
    .kind = XR_KIND_ARRAY, .id = 13, .frozen = true, .container = {.element_type = &stub_error}};
static XrType stub_array_i8 = {
    .kind = XR_KIND_ARRAY, .id = 7, .frozen = true, .container = {.element_type = &stub_i8}};
static XrType stub_array_u64 = {
    .kind = XR_KIND_ARRAY, .id = 8, .frozen = true, .container = {.element_type = &stub_u64}};
static XrType stub_slice_i8 = {
    .kind = XR_KIND_SLICE, .id = 16, .frozen = true, .container = {.element_type = &stub_i8}};

static int tests_passed = 0;
static int tests_failed = 0;
static const char *test_filter = NULL;

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
        if (test_filter && !strstr(#name, test_filter))                                            \
            return;                                                                                \
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
    bool ok = xi_verify(f, err, sizeof(err));
    if (!ok)
        printf("  verifier error: %s\n", err);
    return ok;
}

static bool verify_fail(const XiFunc *f) {
    char err[256] = {0};
    bool ok = xi_verify(f, err, sizeof(err));
    if (ok)
        return false;
    return err[0] != '\0';
}

static bool verify_error_prefix(const XiFunc *f, const char *prefix) {
    char err[256] = {0};
    bool ok = xi_verify(f, err, sizeof(err));
    return !ok && prefix && strncmp(err, prefix, strlen(prefix)) == 0;
}

static bool verify_stage_fail(const XiFunc *f, XiStage stage) {
    char err[256] = {0};
    bool ok = xi_verify_stage(f, stage, err, sizeof(err));
    if (ok)
        return false;
    return err[0] != '\0';
}

static void mark_coro_lower_input(XiFunc *f) {
    f->stage = XI_STAGE_SEMANTIC_LOWERED;
    f->invariant_mask = xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
}

static int known_suspend_call(void *ud, const XiFunc *current, const XiValue *call);

static bool coro_point_has_live_value(const XiCoroSuspendPoint *point, const XiValue *value) {
    for (uint32_t i = 0; point && i < point->nlive; i++) {
        if (point->live[i] == value)
            return true;
    }
    return false;
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

/* ========== Return and raw-memory executable contracts ========== */

TEST(non_unit_return_requires_value) {
    XiFunc *f = make_func("return_missing_value");
    ASSERT(f != NULL);
    xi_block_set_return(f->entry, NULL);
    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(unit_return_rejects_value) {
    XiFunc *f = make_func("unit_return_with_value");
    ASSERT(f != NULL);
    f->return_type = &stub_unit;
    XiValue *value = xi_const_int(f, f->entry, 1, &stub_int);
    xi_block_set_return(f->entry, value);
    ASSERT(verify_fail(f));
    xi_func_free(f);
}

static XiValue *make_pointer_const(XiFunc *f) {
    return xi_const_null(f, f->entry, &stub_pointer);
}

TEST(ptr_load_usize_contract_passes) {
    XiFunc *f = make_func("ptr_load_usize_valid");
    ASSERT(f != NULL);
    f->return_type = &stub_usize;
    XiValue *addr = make_pointer_const(f);
    XiValue *endian = xi_const_int(f, f->entry, XR_ENDIAN_NATIVE, &stub_int);
    XiValue *load = xi_value_new(f, f->entry, XI_PTR_LOAD, &stub_usize, 2);
    ASSERT(addr && endian && load);
    load->args[0] = addr;
    load->args[1] = endian;
    load->aux_int = xr_ffi_ptr_aux(XR_FFI_T_SIZE, false);
    xi_block_set_return(f->entry, load);
    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(ptr_load_rejects_reserved_aux_bits) {
    XiFunc *f = make_func("ptr_load_reserved_aux");
    ASSERT(f != NULL);
    f->return_type = &stub_usize;
    XiValue *load = xi_value_new(f, f->entry, XI_PTR_LOAD, &stub_usize, 2);
    load->args[0] = make_pointer_const(f);
    load->args[1] = xi_const_int(f, f->entry, XR_ENDIAN_NATIVE, &stub_int);
    load->aux_int = XR_FFI_T_SIZE | 0x20;
    xi_block_set_return(f->entry, load);
    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(ptr_load_rejects_result_scalar_mismatch) {
    XiFunc *f = make_func("ptr_load_type_mismatch");
    ASSERT(f != NULL);
    XiValue *load = xi_value_new(f, f->entry, XI_PTR_LOAD, &stub_int, 2);
    load->args[0] = make_pointer_const(f);
    load->args[1] = xi_const_int(f, f->entry, XR_ENDIAN_NATIVE, &stub_int);
    load->aux_int = xr_ffi_ptr_aux(XR_FFI_T_SIZE, false);
    xi_block_set_return(f->entry, load);
    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(ptr_access_rejects_non_pointer_address) {
    XiFunc *f = make_func("ptr_load_bad_address");
    ASSERT(f != NULL);
    XiValue *load = xi_value_new(f, f->entry, XI_PTR_LOAD, &stub_int, 2);
    load->args[0] = xi_const_int(f, f->entry, 0, &stub_int);
    load->args[1] = xi_const_int(f, f->entry, XR_ENDIAN_NATIVE, &stub_int);
    load->aux_int = xr_ffi_ptr_aux(XR_FFI_T_I64, false);
    xi_block_set_return(f->entry, load);
    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(pointer_load_requires_native_endian) {
    XiFunc *f = make_func("ptr_load_pointer_endian");
    ASSERT(f != NULL);
    f->return_type = &stub_pointer;
    XiValue *load = xi_value_new(f, f->entry, XI_PTR_LOAD, &stub_pointer, 2);
    load->args[0] = make_pointer_const(f);
    load->args[1] = xi_const_int(f, f->entry, XR_ENDIAN_LE, &stub_int);
    load->aux_int = xr_ffi_ptr_aux(XR_FFI_T_PTR, false);
    xi_block_set_return(f->entry, load);
    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(raw_slice_requires_view_evidence) {
    XiFunc *f = make_func("raw_slice_missing_evidence");
    ASSERT(f != NULL);
    f->return_type = &stub_slice_i8;
    XiValue *slice = xi_value_new(f, f->entry, XI_SLICE_FROM_PTR, &stub_slice_i8, 3);
    ASSERT(slice != NULL);
    slice->args[0] = make_pointer_const(f);
    slice->args[1] = xi_const_int(f, f->entry, 1, &stub_int);
    slice->args[2] = slice->args[0];
    slice->flags |= XI_FLAG_MAY_THROW | XI_FLAG_READS_MEM;
    xi_block_set_return(f->entry, slice);
    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(raw_slice_accepts_complete_view_evidence) {
    XiFunc *f = make_func("raw_slice_with_evidence");
    ASSERT(f != NULL);
    f->return_type = &stub_slice_i8;
    XiValue *owner = make_pointer_const(f);
    XiValue *slice = xi_value_new(f, f->entry, XI_SLICE_FROM_PTR, &stub_slice_i8, 3);
    ASSERT(owner && slice);
    slice->args[0] = owner;
    slice->args[1] = xi_const_int(f, f->entry, 1, &stub_int);
    slice->args[2] = owner;
    slice->flags |= XI_FLAG_MAY_THROW | XI_FLAG_READS_MEM;
    slice->view_evidence.origin = XI_VIEW_ORIGIN_FOREIGN;
    slice->view_evidence.source_operand = 2;
    slice->view_evidence.source_param = -1;
    slice->view_evidence.root_value_id = owner->id;
    slice->view_evidence.capability = 1;
    slice->view_evidence.lifetime = 1;
    slice->view_evidence.complete = 1;
    xi_block_set_return(f->entry, slice);
    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(slice_call_requires_view_evidence) {
    XiFunc *f = make_func("slice_call_missing_evidence");
    ASSERT(f != NULL);
    f->return_type = &stub_slice_i8;
    XiValue *callee = xi_const_null(f, f->entry, &stub_func);
    XiValue *call = xi_value_new(f, f->entry, XI_CALL, &stub_slice_i8, 1);
    ASSERT(callee && call);
    call->args[0] = callee;
    call->flags |= XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(f->entry, call);
    ASSERT(verify_fail(f));
    xi_func_free(f);
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

/* ========== Ref/out call-plan contracts ========== */

static XiCallPlan *make_single_ref_call_plan(XiFunc *f, XiValue *place) {
    XiCallPlan *plan = (XiCallPlan *) xi_func_arena_alloc(f, sizeof(*plan));
    XiCallArgPlan *arg = (XiCallArgPlan *) xi_func_arena_alloc(f, sizeof(*arg));
    if (!plan || !arg)
        return NULL;
    memset(plan, 0, sizeof(*plan));
    memset(arg, 0, sizeof(*arg));
    arg->param_mode = XR_PARAM_REF;
    arg->access = XR_CALL_ARG_REF;
    arg->origin = XI_PLACE_ORIGIN_STACK_LOCAL;
    arg->lifetime = XI_PLACE_LIFETIME_CALL_BOUND;
    arg->escape = XI_PLACE_ESCAPE_NONE;
    arg->addressable = true;
    arg->origin_var_id = 0;
    arg->place = place;
    plan->args = arg;
    plan->nargs = 1;
    plan->verified = true;
    return plan;
}

static XiValue *make_ref_call(XiFunc *f, XiBlock *entry, XiValue **place_out) {
    f->source_var_count = 1;
    XiValue *source = xi_const_int(f, entry, 1, &stub_int);
    source->var_id = 0;
    XiValue *place = xi_value_new(f, entry, XI_LOCAL_ADDR, &stub_int, 1);
    XiValue *callee = xi_value_new(f, entry, XI_CLOSURE_NEW, &stub_func, 0);
    XiValue *call = xi_value_new(f, entry, XI_CALL, &stub_int, 2);
    if (!place || !callee || !call)
        return NULL;
    place->args[0] = source;
    call->args[0] = callee;
    call->args[1] = place;
    call->call_plan = make_single_ref_call_plan(f, place);
    if (place_out)
        *place_out = place;
    return call;
}

static XiValue *make_move_call(XiFunc *f, XiBlock *entry) {
    XiValue *source = xi_const_int(f, entry, 1, &stub_int);
    XiValue *moved = xi_value_new(f, entry, XI_SOURCE_MOVE, &stub_int, 1);
    XiValue *callee = xi_value_new(f, entry, XI_CLOSURE_NEW, &stub_func, 0);
    XiValue *call = xi_value_new(f, entry, XI_CALL, &stub_int, 2);
    XiCallPlan *plan = (XiCallPlan *) xi_func_arena_alloc(f, sizeof(*plan));
    XiCallArgPlan *arg = (XiCallArgPlan *) xi_func_arena_alloc(f, sizeof(*arg));
    if (!source || !moved || !callee || !call || !plan || !arg)
        return NULL;
    moved->args[0] = source;
    call->args[0] = callee;
    call->args[1] = moved;
    memset(plan, 0, sizeof(*plan));
    memset(arg, 0, sizeof(*arg));
    arg->param_mode = XR_PARAM_MOVE;
    arg->access = XR_CALL_ARG_MOVE;
    arg->origin_var_id = XI_NO_VAR_ID;
    plan->args = arg;
    plan->nargs = 1;
    plan->verified = true;
    call->call_plan = plan;
    return call;
}

static XiValue *make_place_receiver_call(XiFunc *f, XiBlock *entry, XrParamMode mode,
                                         XiValue **place_out) {
    f->source_var_count = 1;
    XiValue *source = xi_const_int(f, entry, 1, &stub_int);
    XiValue *place = xi_value_new(f, entry, XI_LOCAL_ADDR, &stub_int, 1);
    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, &stub_int, 1);
    XiCallPlan *plan = (XiCallPlan *) xi_func_arena_alloc(f, sizeof(*plan));
    if (!source || !place || !call || !plan)
        return NULL;
    source->var_id = 0;
    place->args[0] = source;
    call->args[0] = place;
    call->aux = (void *) "valueMethod";
    memset(plan, 0, sizeof(*plan));
    plan->receiver.param_mode = mode;
    plan->receiver.access = XR_CALL_ARG_PLAIN;
    plan->receiver.origin = XI_PLACE_ORIGIN_STACK_LOCAL;
    plan->receiver.lifetime = XI_PLACE_LIFETIME_CALL_BOUND;
    plan->receiver.escape = XI_PLACE_ESCAPE_NONE;
    plan->receiver.addressable = true;
    plan->receiver.origin_var_id = 0;
    plan->receiver.place = place;
    plan->has_receiver = true;
    plan->verified = true;
    call->call_plan = plan;
    if (place_out)
        *place_out = place;
    return call;
}

TEST(call_plan_valid_ref_local_place_passes) {
    XiFunc *f = make_func("call_plan_valid");
    ASSERT(f != NULL);
    XiValue *place = NULL;
    XiValue *call = make_ref_call(f, f->entry, &place);
    ASSERT(call != NULL && call->call_plan != NULL && place != NULL);
    XiValue *load = xi_value_new(f, f->entry, XI_PLACE_LOAD, &stub_int, 1);
    ASSERT(load != NULL);
    load->args[0] = place;
    xi_block_set_return(f->entry, load);

    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(call_plan_valid_explicit_move_passes) {
    XiFunc *f = make_func("call_plan_move_valid");
    ASSERT(f != NULL);
    XiValue *call = make_move_call(f, f->entry);
    ASSERT(call != NULL && call->call_plan != NULL);
    xi_block_set_return(f->entry, call);

    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(call_plan_rejects_unverified_plan) {
    XiFunc *f = make_func("call_plan_unverified");
    ASSERT(f != NULL);
    XiValue *place = NULL;
    XiValue *call = make_ref_call(f, f->entry, &place);
    ASSERT(call != NULL && call->call_plan != NULL);
    call->call_plan->verified = false;
    xi_block_set_return(f->entry, call);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(call_plan_rejects_place_mismatch) {
    XiFunc *f = make_func("call_plan_place_mismatch");
    ASSERT(f != NULL);
    XiValue *place = NULL;
    XiValue *call = make_ref_call(f, f->entry, &place);
    ASSERT(call != NULL && call->call_plan != NULL && place != NULL);
    call->call_plan->args[0].place = place->args[0];
    xi_block_set_return(f->entry, call);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(call_plan_rejects_declared_escape) {
    XiFunc *f = make_func("call_plan_escape");
    ASSERT(f != NULL);
    XiValue *call = make_ref_call(f, f->entry, NULL);
    ASSERT(call != NULL && call->call_plan != NULL);
    call->call_plan->args[0].escape = XI_PLACE_ESCAPE_RETURN;
    xi_block_set_return(f->entry, call);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(call_bound_place_rejects_return_escape) {
    XiFunc *f = make_func("call_place_return_escape");
    ASSERT(f != NULL);
    XiValue *source = xi_const_int(f, f->entry, 1, &stub_int);
    XiValue *place = xi_value_new(f, f->entry, XI_LOCAL_ADDR, &stub_int, 1);
    ASSERT(source != NULL && place != NULL);
    place->args[0] = source;
    xi_block_set_return(f->entry, place);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

static XiValue *make_ref_param(XiFunc *f) {
    f->nparams = 1;
    f->params = (XiValue **) xr_calloc(1, sizeof(XiValue *));
    if (!f->params)
        return NULL;
    XiValue *param = xi_param(f, f->entry, 0, &stub_int);
    if (!param)
        return NULL;
    f->params[0] = param;
    if (!xi_func_set_param_passing_mode(f, 0, XR_PARAM_REF))
        return NULL;
    return param;
}

TEST(call_bound_param_last_use_before_suspend_passes) {
    XiFunc *f = make_func("call_place_nll_before_suspend");
    ASSERT(f != NULL);
    XiValue *param = make_ref_param(f);
    ASSERT(param != NULL);
    XiValue *load = xi_value_new(f, f->entry, XI_PLACE_LOAD, &stub_int, 1);
    XiValue *yield = xi_value_new(f, f->entry, XI_YIELD, &stub_unit, 0);
    XiValue *result = xi_const_int(f, f->entry, 0, &stub_int);
    ASSERT(load != NULL && yield != NULL && result != NULL);
    load->args[0] = param;
    xi_block_set_return(f->entry, result);

    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(call_bound_param_use_after_suspend_fails) {
    XiFunc *f = make_func("call_place_live_across_suspend");
    ASSERT(f != NULL);
    XiValue *param = make_ref_param(f);
    ASSERT(param != NULL);
    XiValue *yield = xi_value_new(f, f->entry, XI_YIELD, &stub_unit, 0);
    XiValue *load = xi_value_new(f, f->entry, XI_PLACE_LOAD, &stub_int, 1);
    ASSERT(yield != NULL && load != NULL);
    load->args[0] = param;
    xi_block_set_return(f->entry, load);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(call_plan_suspendable_boundary_uses_frame_place) {
    XiFunc *f = make_func("call_place_suspendable_call");
    ASSERT(f != NULL);
    XiValue *place = NULL;
    XiValue *call = make_ref_call(f, f->entry, &place);
    ASSERT(call != NULL && place != NULL && place->args[0] != NULL);
    call->flags |= XI_FLAG_MAY_SUSPEND;
    call->lowering_flags |= XI_LOWERING_FLAG_RETRY_SUSPEND_OPERANDS;
    xi_block_set_return(f->entry, call);

    mark_coro_lower_input(f);
    XiCoroResolver resolver = {0};
    resolver.call_suspendability = known_suspend_call;
    ASSERT(xi_coro_lower(f, &resolver));
    ASSERT(f->coro_plan != NULL && f->coro_plan->nstates == 1);
    const XiCoroSuspendPoint *point = &f->coro_plan->points[0];
    ASSERT(coro_point_has_live_value(point, place));
    ASSERT(coro_point_has_live_value(point, place->args[0]));
    ASSERT(xi_coro_plan_find_slot(f->coro_plan, place) != NULL);
    ASSERT(xi_coro_plan_find_slot(f->coro_plan, place->args[0]) != NULL);
    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(call_plan_valid_method_receiver_place_passes) {
    XiFunc *f = make_func("call_plan_method_receiver");
    ASSERT(f != NULL);
    XiValue *place = NULL;
    XiValue *call = make_place_receiver_call(f, f->entry, XR_PARAM_REF, &place);
    ASSERT(call != NULL && place != NULL);
    XiValue *load = xi_value_new(f, f->entry, XI_PLACE_LOAD, &stub_int, 1);
    ASSERT(load != NULL);
    load->args[0] = place;
    xi_block_set_return(f->entry, load);

    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(call_plan_rejects_method_receiver_place_mismatch) {
    XiFunc *f = make_func("call_plan_method_receiver_mismatch");
    ASSERT(f != NULL);
    XiValue *place = NULL;
    XiValue *call = make_place_receiver_call(f, f->entry, XR_PARAM_READ, &place);
    ASSERT(call != NULL && place != NULL && call->call_plan != NULL);
    call->call_plan->receiver.place = place->args[0];
    xi_block_set_return(f->entry, call);

    ASSERT(verify_fail(f));
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
    xi_evidence_publish(f, XI_EVD_ALIAS, xi_evidence_subject_function(), XI_PROOF_PROVEN,
                        XI_EVIDENCE_REASON_NONE, XI_EVIDENCE_PRODUCER_TEST, 0, NULL);
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
    xi_evidence_publish(f, XI_EVD_ALIAS, xi_evidence_subject_function(), XI_PROOF_PROVEN,
                        XI_EVIDENCE_REASON_NONE, XI_EVIDENCE_PRODUCER_TEST, 0, NULL);
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
    xi_evidence_publish(f, XI_EVD_ALIAS, xi_evidence_subject_function(), XI_PROOF_PROVEN,
                        XI_EVIDENCE_REASON_NONE, XI_EVIDENCE_PRODUCER_TEST, 0, NULL);
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
    xi_evidence_publish(f, XI_EVD_ALIAS, xi_evidence_subject_function(), XI_PROOF_PROVEN,
                        XI_EVIDENCE_REASON_NONE, XI_EVIDENCE_PRODUCER_TEST, 0, NULL);
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

TEST(error_return_type_fails) {
    XiFunc *f = xi_func_new("error_return_type", &stub_error);
    ASSERT(f != NULL);
    XiBlock *entry = xi_block_new(f);
    ASSERT(entry != NULL);
    entry->sealed = true;
    xi_block_set_return(entry, xi_const_int(f, entry, 1, &stub_int));

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(nested_error_return_type_fails) {
    XiFunc *f = xi_func_new("nested_error_return_type", &stub_array_error);
    ASSERT(f != NULL);
    XiBlock *entry = xi_block_new(f);
    ASSERT(entry != NULL);
    entry->sealed = true;
    xi_block_set_return(entry, xi_const_int(f, entry, 1, &stub_int));

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(error_source_var_type_fails) {
    XiFunc *f = make_func("error_source_var_type");
    ASSERT(f != NULL);
    const char *names[] = {"bad"};
    XrType *types[] = {&stub_array_error};
    f->source_var_count = 1;
    f->source_var_names = names;
    f->source_var_types = types;

    xi_block_set_return(f->entry, xi_const_int(f, f->entry, 1, &stub_int));

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(error_capture_type_fails) {
    XiFunc *f = make_func("error_capture_type");
    ASSERT(f != NULL);
    f->captures[0] = (XiCapture) {
        .source = XI_CAPTURE_SRC_REG,
        .index = 0,
        .name = "bad",
        .type = &stub_array_error,
    };
    f->ncaptures = 1;

    xi_block_set_return(f->entry, xi_const_int(f, f->entry, 1, &stub_int));

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(error_value_type_fails) {
    XiFunc *f = make_func("error_value_type");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *v = xi_value_new(f, entry, XI_CONST, &stub_error, 0);
    xi_block_set_return(entry, v);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(error_phi_type_fails) {
    XiFunc *f = make_func("error_phi_type");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiBlock *left = xi_block_new(f);
    XiBlock *right = xi_block_new(f);
    XiBlock *merge = xi_block_new(f);
    ASSERT(left != NULL);
    ASSERT(right != NULL);
    ASSERT(merge != NULL);

    XiValue *cond = xi_const_bool(f, entry, true, &stub_bool);
    xi_block_set_if(entry, cond, left, right);
    xi_block_add_pred(merge, left);
    xi_block_add_pred(merge, right);
    left->succs[0] = merge;
    right->succs[0] = merge;

    XiValue *left_v = xi_const_int(f, left, 1, &stub_int);
    XiValue *right_v = xi_const_int(f, right, 2, &stub_int);
    XiPhi *phi = xi_phi_new(f, merge, &stub_error, 2);
    ASSERT(phi != NULL);
    phi->value.args[0] = left_v;
    phi->value.args[1] = right_v;
    xi_block_set_return(merge, &phi->value);

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

/* ========== Exact-width integer bit-op contracts ========== */

TEST(exact_bit_rotate_accepts_canonical_int_width_tag) {
    XiFunc *f = make_func("exact_bit_int");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *value = xi_param(f, entry, 0, &stub_int);
    XiValue *count = xi_const_int(f, entry, -1, &stub_int);
    XiValue *rot = xi_value_new(f, entry, XI_BIT_ROTL, &stub_int, 2);
    rot->args[0] = value;
    rot->args[1] = count;
    rot->aux_int = 0;
    xi_block_set_return(entry, rot);

    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(exact_bit_receiver_result_width_mismatch_fails) {
    XiFunc *f = make_func("exact_bit_result_width");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *value = xi_param(f, entry, 0, &stub_u16);
    XiValue *swap = xi_value_new(f, entry, XI_BIT_BSWAP, &stub_int, 1);
    swap->args[0] = value;
    swap->aux_int = XR_NATIVE_U16;
    xi_block_set_return(entry, swap);

    ASSERT(verify_fail(f));
    xi_func_free(f);
}

TEST(exact_bit_scalar_rep_tag_mismatch_fails) {
    XiFunc *f = make_func("exact_bit_aux_width");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *value = xi_param(f, entry, 0, &stub_u16);
    XiValue *count = xi_value_new(f, entry, XI_BIT_POPCOUNT, &stub_int, 1);
    count->args[0] = value;
    count->aux_int = XR_NATIVE_U64;
    xi_block_set_return(entry, count);

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

/* ========== Coroutine plan contracts ========== */

TEST(coro_plan_accepts_dense_state_ids) {
    XiFunc *f = make_func("coro_plan_dense");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *susp = xi_value_new(f, entry, XI_YIELD, &stub_unit, 0);
    XiValue *ret = xi_const_int(f, entry, 1, &stub_int);
    ASSERT(susp != NULL);
    ASSERT(ret != NULL);
    xi_block_set_return(entry, ret);

    XiCoroSuspendPoint points[1] = {{0}};
    points[0].state_id = 1;
    points[0].op = susp;
    points[0].kind = XI_CORO_SUSP_YIELD;

    XiCoroPlan plan = {0};
    plan.is_coroutine = true;
    plan.nstates = 1;
    plan.points = points;
    f->coro_plan = &plan;

    ASSERT(verify_ok(f));
    f->coro_plan = NULL;
    xi_func_free(f);
}

TEST(coro_plan_rejects_sparse_state_id) {
    XiFunc *f = make_func("coro_plan_sparse");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *susp = xi_value_new(f, entry, XI_YIELD, &stub_unit, 0);
    XiValue *ret = xi_const_int(f, entry, 1, &stub_int);
    ASSERT(susp != NULL);
    ASSERT(ret != NULL);
    xi_block_set_return(entry, ret);

    XiCoroSuspendPoint points[1] = {{0}};
    points[0].state_id = 2;
    points[0].op = susp;
    points[0].kind = XI_CORO_SUSP_YIELD;

    XiCoroPlan plan = {0};
    plan.is_coroutine = true;
    plan.nstates = 1;
    plan.points = points;
    f->coro_plan = &plan;

    ASSERT(verify_fail(f));
    f->coro_plan = NULL;
    xi_func_free(f);
}

TEST(coro_plan_rejects_live_value_without_slot) {
    XiFunc *f = make_func("coro_plan_live_no_slot");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *susp = xi_value_new(f, entry, XI_YIELD, &stub_unit, 0);
    XiValue *ret = xi_const_int(f, entry, 1, &stub_int);
    ASSERT(susp != NULL);
    ASSERT(ret != NULL);
    xi_block_set_return(entry, ret);

    XiValue *live[1] = {ret};
    XiCoroSuspendPoint points[1] = {{0}};
    points[0].state_id = 1;
    points[0].op = susp;
    points[0].kind = XI_CORO_SUSP_YIELD;
    points[0].live = live;
    points[0].nlive = 1;

    XiCoroPlan plan = {0};
    plan.is_coroutine = true;
    plan.nstates = 1;
    plan.points = points;
    f->coro_plan = &plan;

    ASSERT(verify_fail(f));
    f->coro_plan = NULL;
    xi_func_free(f);
}

TEST(coro_plan_rejects_stale_point_op) {
    XiFunc *f = make_func("coro_plan_stale_op");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *susp = xi_value_new(f, entry, XI_YIELD, &stub_unit, 0);
    XiValue *ret = xi_const_int(f, entry, 1, &stub_int);
    ASSERT(susp != NULL);
    ASSERT(ret != NULL);
    xi_block_set_return(entry, ret);

    XiValue stale = *susp;
    stale.id = susp->id + 100;
    XiCoroSuspendPoint points[1] = {{0}};
    points[0].state_id = 1;
    points[0].op = &stale;
    points[0].kind = XI_CORO_SUSP_YIELD;

    XiCoroPlan plan = {0};
    plan.is_coroutine = true;
    plan.nstates = 1;
    plan.points = points;
    f->coro_plan = &plan;

    ASSERT(verify_fail(f));
    f->coro_plan = NULL;
    xi_func_free(f);
}

TEST(coro_plan_rejects_root_count_mismatch) {
    XiFunc *f = make_func("coro_plan_root_count");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *susp = xi_value_new(f, entry, XI_YIELD, &stub_unit, 0);
    XiValue *ret = xi_const_int(f, entry, 1, &stub_int);
    ASSERT(susp != NULL);
    ASSERT(ret != NULL);
    xi_block_set_return(entry, ret);

    XiCoroSuspendPoint points[1] = {{0}};
    points[0].state_id = 1;
    points[0].op = susp;
    points[0].kind = XI_CORO_SUSP_YIELD;

    XiCoroSlot slots[1] = {{0}};
    slots[0].value = ret;
    slots[0].type = ret->type;
    slots[0].logical_rep = XR_REP_TAGGED;
    slots[0].kind = XI_CORO_SLOT_VALUE;
    slots[0].is_root = true;
    slots[0].live_across = true;
    slots[0].frame_root = true;

    XiCoroPlan plan = {0};
    plan.is_coroutine = true;
    plan.nstates = 1;
    plan.points = points;
    plan.slots = slots;
    plan.nslots = 1;
    plan.root_count = 0;
    f->coro_plan = &plan;

    ASSERT(verify_fail(f));
    f->coro_plan = NULL;
    xi_func_free(f);
}

TEST(coro_plan_does_not_own_borrowed_place_load) {
    XiFunc *f = xi_func_new("coro_borrowed_place_slot", &stub_array_i8);
    ASSERT(f != NULL);
    XiBlock *entry = xi_block_new(f);
    ASSERT(entry != NULL);
    entry->sealed = true;

    XiValue *owner = xi_value_new(f, entry, XI_ARRAY_NEW, &stub_array_i8, 0);
    XiValue *place = xi_value_new(f, entry, XI_LOCAL_ADDR, &stub_array_i8, 1);
    XiValue *alias = xi_value_new(f, entry, XI_PLACE_LOAD, &stub_array_i8, 1);
    XiValue *suspend = xi_value_new(f, entry, XI_YIELD, &stub_unit, 0);
    ASSERT(owner != NULL && place != NULL && alias != NULL && suspend != NULL);
    owner->rep = XR_REP_TAGGED;
    place->args[0] = owner;
    alias->args[0] = place;
    alias->rep = XR_REP_PTR;
    xi_block_set_return(entry, alias);

    XiCoroPlan *plan = xi_coro_analyze(f, NULL);
    ASSERT(plan != NULL);
    const XiCoroSlot *slot = xi_coro_plan_find_slot(plan, alias);
    ASSERT(slot != NULL);
    ASSERT(slot->live_across);
    ASSERT(slot->is_root);
    ASSERT(!slot->needs_release);
    ASSERT(!slot->frame_root);
    ASSERT(!slot->frame_release);

    xi_func_free(f);
}

TEST(coro_plan_records_go_as_scheduler_reduction_point) {
    XiFunc *f = xi_func_new("coro_go_scheduler_reduction", &stub_unit);
    ASSERT(f != NULL);
    XiBlock *entry = xi_block_new(f);
    ASSERT(entry != NULL);
    entry->sealed = true;

    XiValue *callee = xi_value_new(f, entry, XI_CONST, &stub_func, 0);
    XiValue *go = xi_value_new(f, entry, XI_GO, &stub_unit, 1);
    ASSERT(callee != NULL && go != NULL);
    go->args[0] = callee;
    xi_block_set_return(entry, NULL);

    ASSERT(xi_coro_is_suspend_point(f, go, NULL));
    XiCoroPlan *plan = xi_coro_analyze(f, NULL);
    ASSERT(plan != NULL);
    ASSERT(plan->nstates == 1);
    ASSERT(plan->points[0].op == go && plan->points[0].state_id == 1);
    xi_func_free(f);
}

TEST(coro_plan_transfers_owned_rep_alias_into_frame) {
    XiFunc *f = xi_func_new("coro_owned_rep_alias", &stub_str);
    ASSERT(f != NULL);
    XiBlock *entry = xi_block_new(f);
    ASSERT(entry != NULL);
    entry->sealed = true;

    XiValue *a = xi_const_str(f, entry, "a", &stub_str);
    XiValue *b = xi_const_str(f, entry, "b", &stub_str);
    XiValue *owner = xi_value_new(f, entry, XI_STR_CONCAT, &stub_str, 2);
    XiValue *alias = xi_value_new(f, entry, XI_UNBOX, &stub_str, 1);
    XiValue *suspend = xi_value_new(f, entry, XI_YIELD, &stub_unit, 0);
    ASSERT(a && b && owner && alias && suspend);
    owner->args[0] = a;
    owner->args[1] = b;
    owner->rep = XR_REP_TAGGED;
    alias->args[0] = owner;
    alias->rep = XR_REP_PTR;
    xi_block_set_return(entry, alias);

    mark_coro_lower_input(f);
    XiCoroPlan *plan = xi_coro_analyze(f, NULL);
    ASSERT(plan != NULL);
    const XiCoroSlot *slot = xi_coro_plan_find_slot(plan, alias);
    ASSERT(slot != NULL);
    ASSERT(slot->owner_value_id == owner->id);
    ASSERT(slot->needs_release);
    ASSERT(slot->frame_root);
    ASSERT(slot->frame_release);
    ASSERT(plan->points[0].nroots == 1 && plan->points[0].roots[0] == alias);
    ASSERT(plan->points[0].ndrops == 1 && plan->points[0].drops[0] == alias);
    xi_func_free(f);
}

static XiFunc *make_lowered_two_state_coro(void) {
    XiFunc *f = xi_func_new("coro_lower_two_state", &stub_array_i8);
    if (!f)
        return NULL;
    XiBlock *entry = xi_block_new(f);
    if (!entry) {
        xi_func_free(f);
        return NULL;
    }
    entry->sealed = true;
    XiValue *owner = xi_value_new(f, entry, XI_ARRAY_NEW, &stub_array_i8, 0);
    XiValue *later_owner = xi_value_new(f, entry, XI_ARRAY_NEW, &stub_array_i8, 0);
    XiValue *first = xi_value_new(f, entry, XI_YIELD, &stub_unit, 0);
    XiValue *consume = xi_value_new(f, entry, XI_PRINT, &stub_unit, 1);
    XiValue *second = xi_value_new(f, entry, XI_YIELD, &stub_unit, 0);
    if (!owner || !first || !consume || !later_owner || !second) {
        xi_func_free(f);
        return NULL;
    }
    consume->args[0] = owner;
    xi_block_set_return(entry, later_owner);
    mark_coro_lower_input(f);
    if (!xi_coro_lower(f, NULL)) {
        xi_func_free(f);
        return NULL;
    }
    return f;
}

TEST(coro_lower_splits_cfg_and_records_cleanup_obligations) {
    XiFunc *f = make_lowered_two_state_coro();
    ASSERT(f != NULL);
    XiCoroPlan *plan = f->coro_plan;
    ASSERT(plan != NULL);
    ASSERT(plan->cfg_rewritten);
    ASSERT(plan->nstates == 2);
    ASSERT(plan->ndispatch == 3);
    ASSERT(plan->dispatch[0].target == f->entry);
    ASSERT(f->nblocks == 5);

    for (uint32_t i = 0; i < plan->nstates; i++) {
        XiCoroSuspendPoint *point = &plan->points[i];
        ASSERT(point->state_id == i + 1);
        ASSERT(point->pre_block->succs[0] == point->suspend_block);
        ASSERT(point->suspend_block->nvalues == 1);
        ASSERT(point->suspend_block->values[0] == point->op);
        ASSERT(point->suspend_block->succs[0] == point->resume_block);
        ASSERT(plan->dispatch[i + 1].target == point->suspend_block);
        uint32_t expected_live = i == 0 ? 2u : 1u;
        ASSERT(point->nlive == expected_live);
        ASSERT(point->nroots == expected_live);
        ASSERT(point->ndrops == expected_live);
        ASSERT(xi_coro_point_find_edge(point, XI_CORO_EDGE_RESUME) != NULL);
        ASSERT(xi_coro_point_find_edge(point, XI_CORO_EDGE_ERROR) != NULL);
        ASSERT(xi_coro_point_find_edge(point, XI_CORO_EDGE_PANIC) != NULL);
        ASSERT(xi_coro_point_find_edge(point, XI_CORO_EDGE_CANCEL) != NULL);
        ASSERT(xi_coro_point_find_edge(point, XI_CORO_EDGE_DROP) != NULL);
    }
    ASSERT(plan->spill_count == 3);
    ASSERT(plan->edge_count == 10);
    ASSERT(plan->actions_materialized);
    ASSERT(plan->frame_action_count > plan->spill_count);
    ASSERT(plan->action_fingerprint != 0);
    ASSERT(plan->fingerprint != 0);
    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(coro_lower_is_deterministic_and_idempotent) {
    XiFunc *a = make_lowered_two_state_coro();
    XiFunc *b = make_lowered_two_state_coro();
    ASSERT(a != NULL && b != NULL);
    ASSERT(a->coro_plan->fingerprint == b->coro_plan->fingerprint);
    uint32_t blocks = a->nblocks;
    uint64_t fingerprint = a->coro_plan->fingerprint;
    ASSERT(xi_coro_lower(a, NULL));
    ASSERT(a->nblocks == blocks);
    ASSERT(a->coro_plan->fingerprint == fingerprint);
    ASSERT(verify_ok(a));
    xi_func_free(a);
    xi_func_free(b);
}

TEST(coro_lower_mutation_rejects_missing_spill) {
    XiFunc *f = make_lowered_two_state_coro();
    ASSERT(f != NULL);
    ASSERT(f->coro_plan->points[1].nlive == 1);
    f->coro_plan->points[1].nlive = 0;
    ASSERT(verify_error_prefix(f, "XR_CORO_4001"));
    xi_func_free(f);
}

TEST(coro_lower_mutation_rejects_missing_drop) {
    XiFunc *f = make_lowered_two_state_coro();
    ASSERT(f != NULL);
    ASSERT(f->coro_plan->points[1].ndrops == 1);
    f->coro_plan->points[1].ndrops = 0;
    ASSERT(verify_error_prefix(f, "XR_CORO_4002"));
    xi_func_free(f);
}

TEST(coro_lower_mutation_rejects_missing_root) {
    XiFunc *f = make_lowered_two_state_coro();
    ASSERT(f != NULL);
    ASSERT(f->coro_plan->points[1].nroots == 1);
    f->coro_plan->points[1].nroots = 0;
    ASSERT(verify_error_prefix(f, "XR_CORO_4002"));
    xi_func_free(f);
}

TEST(coro_lower_mutation_rejects_extra_spill) {
    XiFunc *f = make_lowered_two_state_coro();
    ASSERT(f != NULL);
    XiCoroPlan *plan = f->coro_plan;
    XiCoroSuspendPoint *point = &plan->points[1];
    XiValue *extra = NULL;
    for (uint32_t i = 0; i < plan->nslots; i++) {
        if (plan->slots[i].value != point->live[0]) {
            extra = plan->slots[i].value;
            break;
        }
    }
    ASSERT(extra != NULL);
    point->live[point->nlive++] = extra;
    ASSERT(verify_error_prefix(f, "XR_CORO_4001"));
    xi_func_free(f);
}

TEST(coro_lower_mutation_rejects_duplicate_root) {
    XiFunc *f = make_lowered_two_state_coro();
    ASSERT(f != NULL);
    XiCoroSuspendPoint *point = &f->coro_plan->points[1];
    ASSERT(point->nroots == 1);
    point->roots[1] = point->roots[0];
    point->nroots = 2;
    ASSERT(verify_error_prefix(f, "XR_CORO_4002"));
    xi_func_free(f);
}

TEST(coro_lower_mutation_rejects_duplicate_drop) {
    XiFunc *f = make_lowered_two_state_coro();
    ASSERT(f != NULL);
    XiCoroSuspendPoint *point = &f->coro_plan->points[1];
    ASSERT(point->ndrops == 1);
    point->drops[1] = point->drops[0];
    point->ndrops = 2;
    ASSERT(verify_error_prefix(f, "XR_CORO_4002"));
    xi_func_free(f);
}

TEST(coro_lower_mutation_rejects_foreign_root) {
    XiFunc *f = make_lowered_two_state_coro();
    ASSERT(f != NULL);
    XiCoroPlan *plan = f->coro_plan;
    XiCoroSuspendPoint *point = &plan->points[1];
    ASSERT(point->nroots == 1);
    XiValue *foreign = NULL;
    for (uint32_t i = 0; i < plan->nslots; i++) {
        if (plan->slots[i].frame_root && plan->slots[i].value != point->roots[0]) {
            foreign = plan->slots[i].value;
            break;
        }
    }
    ASSERT(foreign != NULL);
    point->roots[0] = foreign;
    ASSERT(verify_error_prefix(f, "XR_CORO_4002"));
    xi_func_free(f);
}

TEST(coro_lower_mutation_rejects_sparse_state) {
    XiFunc *f = make_lowered_two_state_coro();
    ASSERT(f != NULL);
    f->coro_plan->points[0].state_id = 7;
    ASSERT(verify_error_prefix(f, "XR_CORO_4000"));
    xi_func_free(f);
}

TEST(coro_lower_mutation_rejects_invalid_dispatch_target) {
    XiFunc *f = make_lowered_two_state_coro();
    ASSERT(f != NULL);
    f->coro_plan->dispatch[1].target = f->entry;
    ASSERT(verify_error_prefix(f, "XR_CORO_4000"));
    xi_func_free(f);
}

TEST(coro_lower_mutation_rejects_stale_revision) {
    XiFunc *f = make_lowered_two_state_coro();
    ASSERT(f != NULL);
    f->cfg_version++;
    ASSERT(verify_error_prefix(f, "XR_CORO_4000"));
    xi_func_free(f);
}

static int known_suspend_call(void *ud, const XiFunc *current, const XiValue *call) {
    (void) ud;
    (void) current;
    (void) call;
    return 1;
}

TEST(coro_lower_mutation_rejects_invalid_child_edge) {
    XiFunc *f = make_func("coro_child_edge");
    ASSERT(f != NULL);
    XiValue *callee = xi_value_new(f, f->entry, XI_CONST, &stub_func, 0);
    XiValue *call = xi_value_new(f, f->entry, XI_CALL, &stub_int, 1);
    ASSERT(callee != NULL && call != NULL);
    call->args[0] = callee;
    call->flags |= XI_FLAG_MAY_SUSPEND;
    xi_block_set_return(f->entry, call);
    mark_coro_lower_input(f);
    XiCoroResolver resolver = {0};
    resolver.call_suspendability = known_suspend_call;
    ASSERT(xi_coro_lower(f, &resolver));
    ASSERT(f->coro_plan->nstates == 1);
    ASSERT(f->coro_plan->points[0].nlive == 1);
    ASSERT(f->coro_plan->points[0].live[0] == call);
    XiCoroEdge *child = (XiCoroEdge *) xi_coro_point_find_edge(
        &f->coro_plan->points[0], XI_CORO_EDGE_CHILD);
    ASSERT(child != NULL);
    ASSERT(child->child == callee);
    ASSERT(verify_ok(f));
    child->child = NULL;
    ASSERT(verify_error_prefix(f, "XR_CORO_4003"));
    xi_func_free(f);
}

TEST(coro_lower_rejects_exception_region_before_cfg_mutation) {
    XiFunc *f = make_func("coro_try_region");
    ASSERT(f != NULL);
    XiValue *try_op = xi_value_new(f, f->entry, XI_TRY, &stub_unit, 0);
    XiValue *yield = xi_value_new(f, f->entry, XI_YIELD, &stub_unit, 0);
    XiValue *end_try = xi_value_new(f, f->entry, XI_END_TRY, &stub_unit, 0);
    XiValue *result = xi_const_int(f, f->entry, 1, &stub_int);
    ASSERT(try_op && yield && end_try && result);
    xi_block_set_return(f->entry, result);
    mark_coro_lower_input(f);
    uint32_t blocks = f->nblocks;
    ASSERT(!xi_coro_lower(f, NULL));
    ASSERT(f->nblocks == blocks);
    ASSERT(f->coro_plan == NULL);
    xi_func_free(f);
}

TEST(coro_lower_accepts_frame_backed_static_cleanup_region) {
    XiFunc *f = make_func("coro_static_cleanup_region");
    ASSERT(f != NULL);
    XiBlock *body = xi_block_new(f);
    XiBlock *handler = xi_block_new(f);
    ASSERT(body != NULL && handler != NULL);
    body->sealed = true;
    handler->sealed = true;

    XiValue *try_op = xi_value_new(f, f->entry, XI_TRY, &stub_unit, 0);
    ASSERT(try_op != NULL);
    try_op->aux = handler;
    try_op->aux_int = XI_TRY_AUX_STATIC_CLEANUP;
    try_op->flags |= XI_FLAG_SIDE_EFFECT;
    xi_block_add_pred(handler, f->entry);
    xi_block_set_jump(f->entry, body);

    XiValue *yield = xi_value_new(f, body, XI_YIELD, &stub_unit, 0);
    XiValue *end_try = xi_value_new(f, body, XI_END_TRY, &stub_unit, 0);
    XiValue *result = xi_const_int(f, body, 1, &stub_int);
    ASSERT(yield != NULL && end_try != NULL && result != NULL);
    end_try->aux = try_op;
    end_try->flags |= XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(body, result);

    XiValue *caught = xi_value_new(f, handler, XI_CATCH, &stub_unit, 0);
    XiValue *handler_result = xi_const_int(f, handler, 2, &stub_int);
    ASSERT(caught != NULL && handler_result != NULL);
    caught->aux = try_op;
    caught->flags |= XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(handler, handler_result);

    mark_coro_lower_input(f);
    ASSERT(xi_coro_lower(f, NULL));
    ASSERT(f->coro_plan != NULL);
    ASSERT(f->coro_plan->cfg_rewritten);
    ASSERT(f->coro_plan->nstates == 1);
    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(coro_lower_accepts_open_callable_as_state_obligation) {
    XiFunc *f = make_func("coro_open_callable");
    ASSERT(f != NULL);
    XiValue *callee = xi_value_new(f, f->entry, XI_CONST, &stub_func, 0);
    XiValue *call = xi_value_new(f, f->entry, XI_CALL, &stub_int, 1);
    ASSERT(callee != NULL && call != NULL);
    call->args[0] = callee;
    call->flags |= XI_FLAG_MAY_SUSPEND;
    xi_block_set_return(f->entry, call);
    mark_coro_lower_input(f);

    uint32_t blocks = f->nblocks;
    ASSERT(xi_coro_lower(f, NULL));
    ASSERT(f->nblocks == blocks + 2);
    ASSERT(f->coro_plan != NULL);
    ASSERT(f->coro_plan->nstates == 1);
    ASSERT(f->coro_plan->points[0].op == call);
    ASSERT(f->coro_plan->points[0].resolved_callee == NULL);
    ASSERT(f->coro_plan->points[0].edges[XI_CORO_EDGE_CHILD].indirect_child);
    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(coro_lower_rejects_raw_stage_before_analysis) {
    XiFunc *f = make_func("coro_raw_stage");
    ASSERT(f != NULL);
    XiValue *yield = xi_value_new(f, f->entry, XI_YIELD, &stub_unit, 0);
    XiValue *result = xi_const_int(f, f->entry, 1, &stub_int);
    ASSERT(yield && result);
    xi_block_set_return(f->entry, result);
    uint32_t blocks = f->nblocks;
    ASSERT(!xi_coro_lower(f, NULL));
    ASSERT(f->nblocks == blocks);
    ASSERT(f->coro_plan == NULL);
    xi_func_free(f);
}

TEST(coro_lower_preserves_successor_phi_pred_position) {
    XiFunc *f = make_func("coro_successor_phi");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiBlock *merge = xi_block_new(f);
    ASSERT(merge != NULL);
    merge->sealed = true;
    XiValue *value = xi_const_int(f, entry, 7, &stub_int);
    XiValue *yield = xi_value_new(f, entry, XI_YIELD, &stub_unit, 0);
    ASSERT(value && yield);
    xi_block_set_jump(entry, merge);
    XiPhi *phi = xi_phi_new(f, merge, &stub_int, 1);
    ASSERT(phi != NULL);
    phi->value.args[0] = value;
    xi_block_set_return(merge, &phi->value);

    mark_coro_lower_input(f);
    ASSERT(xi_coro_lower(f, NULL));
    XiCoroSuspendPoint *point = &f->coro_plan->points[0];
    ASSERT(merge->npreds == 1);
    ASSERT(merge->preds[0] == point->resume_block);
    ASSERT(phi->value.args[0] == value);
    ASSERT(verify_ok(f));
    xi_func_free(f);
}

TEST(coro_lower_rejects_stale_cached_analysis) {
    XiFunc *f = make_func("coro_stale_analysis");
    ASSERT(f != NULL);
    XiValue *yield = xi_value_new(f, f->entry, XI_YIELD, &stub_unit, 0);
    XiValue *result = xi_const_int(f, f->entry, 1, &stub_int);
    ASSERT(yield && result);
    xi_block_set_return(f->entry, result);
    mark_coro_lower_input(f);
    ASSERT(xi_coro_analyze(f, NULL) != NULL);
    uint32_t blocks = f->nblocks;
    xi_cfg_invalidate(f);
    ASSERT(!xi_coro_lower(f, NULL));
    ASSERT(f->nblocks == blocks);
    ASSERT(!f->coro_plan->cfg_rewritten);
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
    xi_evidence_publish(f, XI_EVD_ALIAS, xi_evidence_subject_function(), XI_PROOF_PROVEN,
                        XI_EVIDENCE_REASON_NONE, XI_EVIDENCE_PRODUCER_TEST, 0, NULL);
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
    xi_evidence_publish(f, XI_EVD_ALIAS, xi_evidence_subject_function(), XI_PROOF_PROVEN,
                        XI_EVIDENCE_REASON_NONE, XI_EVIDENCE_PRODUCER_TEST, 0, NULL);
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
    xi_evidence_publish(f, XI_EVD_ALIAS, xi_evidence_subject_function(), XI_PROOF_PROVEN,
                        XI_EVIDENCE_REASON_NONE, XI_EVIDENCE_PRODUCER_TEST, 0, NULL);
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
    f->return_type = &stub_str;
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

/* ========== Coroutine suspendability depth bound ========== */

/* Resolver for a synthetic linear chain f0 -> f1 -> ... -> fN. Prepared
 * suspendability is deliberately UNKNOWN (-1) for every function, forcing
 * the bounded local proof walk. */
typedef struct {
    XiFunc **chain;
    int len;
} ChainResolverCtx;

static const XiFunc *chain_resolve_callee(void *ud, const XiFunc *caller, const XiValue *callee) {
    (void) callee;
    ChainResolverCtx *c = (ChainResolverCtx *) ud;
    for (int i = 0; i + 1 < c->len; i++) {
        if (c->chain[i] == caller)
            return c->chain[i + 1];
    }
    return NULL;
}

static int chain_unknown_suspendability(void *ud, const XiFunc *func) {
    (void) ud;
    (void) func;
    return -1;
}

TEST(coro_depth_bound_fails_closed) {
    enum {
        CHAIN = 12
    }; /* deeper than XI_CORO_RESOLVE_DEPTH_MAX (8) */
    XiFunc *chain[CHAIN];
    for (int i = 0; i < CHAIN; i++) {
        chain[i] = make_func("chain_fn");
        ASSERT(chain[i] != NULL);
        if (i > 0) {
            /* caller i-1 gets a call value the resolver maps to i */
            XiValue *fnv = xi_value_new(chain[i - 1], chain[i - 1]->entry, XI_CONST, &stub_int, 0);
            ASSERT(fnv != NULL);
            XiValue *call = xi_value_new(chain[i - 1], chain[i - 1]->entry, XI_CALL, &stub_int, 1);
            ASSERT(call != NULL);
            call->args[0] = fnv;
        }
    }
    ChainResolverCtx ctx = {.chain = chain, .len = CHAIN};
    XiCoroResolver resolver = {0};
    resolver.resolve_callee = chain_resolve_callee;
    resolver.func_suspendability = chain_unknown_suspendability;
    resolver.ud = &ctx;

    /* Past the recursion bound nothing is proven either way. The only safe
     * answer is "may suspend": a resumable frame the program did not need
     * costs cycles, a plain sync ABI on a function that does suspend is a
     * miscompile. The old direction (report non-suspendable) was fail-open;
     * this pins the flip. */
    ASSERT(xi_coro_func_is_suspendable(chain[0], &resolver));

    /* Within the bound, an all-unknown chain with no suspend evidence stays
     * non-suspendable: the flip only covers what the walk cannot see. */
    ChainResolverCtx short_ctx = {.chain = chain + (CHAIN - 4), .len = 4};
    XiCoroResolver short_resolver = resolver;
    short_resolver.ud = &short_ctx;
    ASSERT(!xi_coro_func_is_suspendable(chain[CHAIN - 4], &short_resolver));

    for (int i = 0; i < CHAIN; i++)
        xi_func_free(chain[i]);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi Extended Verifier Tests ===\n\n");
    test_filter = getenv("XRAY_TEST_FILTER");

    run_coro_depth_bound_fails_closed();

    run_non_unit_return_requires_value();
    run_unit_return_rejects_value();
    run_ptr_load_usize_contract_passes();
    run_ptr_load_rejects_reserved_aux_bits();
    run_ptr_load_rejects_result_scalar_mismatch();
    run_ptr_access_rejects_non_pointer_address();
    run_pointer_load_requires_native_endian();
    run_raw_slice_requires_view_evidence();
    run_raw_slice_accepts_complete_view_evidence();
    run_slice_call_requires_view_evidence();

    run_bounds_check_valid();
    run_bounds_check_arity_failure();
    run_bounds_check_effect_failure();
    run_tail_flag_on_non_call_fails();
    run_tail_call_with_non_function_callee_fails();
    run_tail_call_with_function_callee_passes();
    run_call_plan_valid_ref_local_place_passes();
    run_call_plan_valid_explicit_move_passes();
    run_call_plan_rejects_unverified_plan();
    run_call_plan_rejects_place_mismatch();
    run_call_plan_rejects_declared_escape();
    run_call_bound_place_rejects_return_escape();
    run_call_bound_param_last_use_before_suspend_passes();
    run_call_bound_param_use_after_suspend_fails();
    run_call_plan_suspendable_boundary_uses_frame_place();
    run_call_plan_valid_method_receiver_place_passes();
    run_call_plan_rejects_method_receiver_place_mismatch();
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
    run_error_return_type_fails();
    run_nested_error_return_type_fails();
    run_error_source_var_type_fails();
    run_error_capture_type_fails();
    run_error_value_type_fails();
    run_error_phi_type_fails();
    run_call_method_missing_aux_fails();
    run_call_method_zero_args_fails();
    run_call_method_direct_missing_aux_fails();
    run_call_method_direct_bad_index_fails();
    run_call_method_direct_zero_args_fails();
    run_typed_array_store_without_narrow_fails();
    run_typed_array_store_with_narrow_passes();
    run_typed_array_store_with_wrong_narrow_fails();
    run_typed_array_store_u64_without_narrow_passes();
    run_exact_bit_rotate_accepts_canonical_int_width_tag();
    run_exact_bit_receiver_result_width_mismatch_fails();
    run_exact_bit_scalar_rep_tag_mismatch_fails();
    run_duplicate_value_id_fails();
    run_phi_arg_count_mismatch_fails();
    run_use_not_dominated_by_def_fails();
    run_phi_arg_not_dominated_by_pred_fails();
    run_block_id_mismatch_with_array_index_fails();
    run_block_control_not_dominated_by_def_fails();
    run_closed_stage_rejects_bad_upval_index();
    run_repped_stage_rejects_box_i64_rep();
    run_stage_invariant_mask_missing_bits_fails();
    run_coro_plan_accepts_dense_state_ids();
    run_coro_plan_rejects_sparse_state_id();
    run_coro_plan_rejects_live_value_without_slot();
    run_coro_plan_rejects_stale_point_op();
    run_coro_plan_rejects_root_count_mismatch();
    run_coro_plan_does_not_own_borrowed_place_load();
    run_coro_plan_records_go_as_scheduler_reduction_point();
    run_coro_plan_transfers_owned_rep_alias_into_frame();
    run_coro_lower_splits_cfg_and_records_cleanup_obligations();
    run_coro_lower_is_deterministic_and_idempotent();
    run_coro_lower_mutation_rejects_missing_spill();
    run_coro_lower_mutation_rejects_missing_drop();
    run_coro_lower_mutation_rejects_missing_root();
    run_coro_lower_mutation_rejects_extra_spill();
    run_coro_lower_mutation_rejects_duplicate_root();
    run_coro_lower_mutation_rejects_duplicate_drop();
    run_coro_lower_mutation_rejects_foreign_root();
    run_coro_lower_mutation_rejects_sparse_state();
    run_coro_lower_mutation_rejects_invalid_dispatch_target();
    run_coro_lower_mutation_rejects_stale_revision();
    run_coro_lower_mutation_rejects_invalid_child_edge();
    run_coro_lower_rejects_exception_region_before_cfg_mutation();
    run_coro_lower_accepts_frame_backed_static_cleanup_region();
    run_coro_lower_accepts_open_callable_as_state_obligation();
    run_coro_lower_rejects_raw_stage_before_analysis();
    run_coro_lower_preserves_successor_phi_pred_position();
    run_coro_lower_rejects_stale_cached_analysis();
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
