/*
 * test_xi_cgen.c - Unit tests for Xi IR to C code generation
 *
 * Tests the xi_cgen_* functions by:
 *   1. Compiling xray source through the pipeline to Xi IR
 *   2. Running select_rep to insert BOX/UNBOX
 *   3. Generating C code via xi_cgen_program
 *   4. Verifying the output contains expected constructs
 */

#include "../../../src/ir/xi.h"
#include "../../../src/aot/xi_cgen.h"
#include "../../../src/aot/xaot_bundle.h"
#include "../../../src/aot/xaot_class_layout.h"
#include "../../../src/aot/xaot_prepare.h"
#include "../../../src/aot/xaot_struct_name.h"
#include "../../../src/aot/xaot_verify.h"
#include "../../../src/aot/emit_c/xr_c_emission_plan.h"
#include "../../../src/aot/refine/xr_aot_scalar_value.h"
#include "../../../src/ir/xi_opt.h"
#include "../../../src/ir/xi_own.h"
#include "../../../src/ir/xi_effect.h"
#include "../../../src/ir/xi_arc.h"
#include "../../../src/ir/xi_escape.h"
#include "../../../src/ir/xi_coro_analyze.h"
#include "../../../src/ir/xi_coro_lower.h"
#include "../../../src/ir/xi_pipeline.h"
#include "../../../src/ir/xi_stage.h"
#include "../../../src/ir/xi_value_query.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/semantic/xr_semantic_panic_info_shape.h"
#include "../../../src/plan/semantic/xr_semantic_plan.h"
#include "../../../src/plan/semantic/xr_semantic_string_shape.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_profile.h"
#include "../../../src/ir/xi_backend_lower.h"
#include "../../../src/ir/xi_module.h"
#include "../../../src/module/xnative_package.h"
#include "../../../src/module/xmodule.h"
#include "../../../src/module/xmodule_graph.h"
#include "../../../src/runtime/xisolate_api.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/runtime/value/xchunk.h"
#include "../../../src/runtime/value/xstruct_layout.h"
#include "../../../src/frontend/parser/xparse.h"
#include "../../../src/frontend/analyzer/xanalyzer.h"
#include "../../../src/frontend/analyzer/xanalyzer_xrd.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/base/xmemstream.h"
#include "../../../src/base/xglobal_indices.h"
#include "../../../src/shared/xr_int_arith_core.h"
#include "../../../src/shared/xr_semantic_owner_ids_gen.h"
#include "../../../src/plan/semantic/xr_semantic_ops_gen.h"
#include "../../../include/xray.h"
#include "../plan/target_profile_test_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ========== Test Infrastructure ========== */

static XrVMRuntime *g_iso = NULL;
static int tests_passed = 0;
static int tests_failed = 0;
static const char *g_test_filter = NULL;
static const char *g_string_runes_c_output = NULL;
static const char *g_rune_to_string_c_output = NULL;
static const char *g_native_target_leaf_c_output = NULL;

typedef struct TestAotPlan {
    XaotBundle bundle;
    XgGlobalEvidence evidence;
    const XrCEmissionPlan **emission_plans;
    uint32_t nemission_plans;
    bool initialized;
    bool evidence_initialized;
} TestAotPlan;

#define TEST_REQUIRE(cond, msg)                                                                    \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL: %s\n", (msg));                                                \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

/* Generated-C contracts remain mandatory in Release builds. The standard
 * assert macro must not erase either checks or helper calls under NDEBUG. */
#ifdef NDEBUG
#undef assert
#define assert(condition) TEST_REQUIRE((condition), #condition)
#endif

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        if (g_test_filter && !strstr(#name, g_test_filter))                                        \
            return;                                                                                \
        printf("--- " #name " ---\n");                                                             \
        test_##name();                                                                             \
        tests_passed++;                                                                            \
        printf("  PASS\n");                                                                        \
    }                                                                                              \
    static void test_##name(void)

static void set_single_param_ownership_contract(XiFunc *function, uint8_t parameter_ownership,
                                                uint8_t return_ownership,
                                                int16_t return_parameter) {
    function->arc_borrow_sig =
        (XiBorrowSig *) xi_func_arena_alloc(function, (uint32_t) sizeof(*function->arc_borrow_sig));
    TEST_REQUIRE(function->arc_borrow_sig != NULL, "manual ownership signature allocated");
    function->arc_borrow_sig->nparams = 1;
    function->arc_borrow_sig->param_own[0] = parameter_ownership;
    function->arc_borrow_sig->valid = true;
    function->arc_return_ownership.kind = return_ownership;
    function->arc_return_ownership.param_index = return_parameter;
    function->arc_return_ownership.complete = true;
}

static void setup(void) {
    if (!g_iso) {
        XrVMConfig p = {0};
        g_iso = xray_vm_new_full(&p);
    }
}

static void teardown(void) {
    if (g_iso) {
        xray_vm_delete(g_iso);
        g_iso = NULL;
    }
}

TEST(u64_mul_wide_returns_both_exact_halves) {
    uint64_t high = UINT64_MAX;
    uint64_t low = xr_u64_mul_wide(UINT64_MAX, UINT64_C(2), &high);
    TEST_REQUIRE(low == UINT64_MAX - UINT64_C(1) && high == UINT64_C(1),
                 "max times two returns exact low and high halves");

    high = UINT64_MAX;
    low = xr_u64_mul_wide(UINT64_C(0x8000000000000000), UINT64_C(2), &high);
    TEST_REQUIRE(low == UINT64_C(0) && high == UINT64_C(1),
                 "top bit times two carries into the high half");

    high = UINT64_MAX;
    low = xr_u64_mul_wide(UINT64_MAX, UINT64_MAX, &high);
    TEST_REQUIRE(low == UINT64_C(1) && high == UINT64_MAX - UINT64_C(1),
                 "max squared returns exact low and high halves");
}

TEST(aot_type_fingerprint_includes_param_modes) {
    XrType *param_types[] = {xr_type_new_int(NULL)};
    XrType *ret = xr_type_new_bool(NULL);

    XrType *read_fn = xr_type_new_function(g_iso, param_types, 1, ret, false);
    XrType *ref_fn = xr_type_new_function(g_iso, param_types, 1, ret, false);
    XrType *move_fn = xr_type_new_function(g_iso, param_types, 1, ret, false);
    TEST_REQUIRE(read_fn && ref_fn && move_fn, "function types created");

    TEST_REQUIRE(xr_type_function_set_param_mode(ref_fn, 0, XR_PARAM_REF),
                 "ref param mode assigned");
    TEST_REQUIRE(xr_type_function_set_param_mode(move_fn, 0, XR_PARAM_MOVE),
                 "move param mode assigned");

    uint64_t read_hash = xaot_type_fingerprint(read_fn);
    uint64_t ref_hash = xaot_type_fingerprint(ref_fn);
    uint64_t move_hash = xaot_type_fingerprint(move_fn);

    TEST_REQUIRE(read_hash != ref_hash, "read and ref param modes must hash differently");
    TEST_REQUIRE(read_hash != move_hash, "read and move param modes must hash differently");
    TEST_REQUIRE(ref_hash != move_hash, "ref and move param modes must hash differently");
}

TEST(aot_type_fingerprint_separates_error_recovery) {
    XrType *error_type = xr_type_new_error(NULL);
    XrType *unknown_type = xr_type_new_unknown(NULL);
    XrType *error_array = xr_type_new_array(g_iso, error_type);
    XrType *unknown_array = xr_type_new_array(g_iso, unknown_type);
    TEST_REQUIRE(error_type && unknown_type && error_array && unknown_array,
                 "error and unknown fingerprint fixtures created");

    uint64_t error_hash = xaot_type_fingerprint(error_type);
    uint64_t error_hash_again = xaot_type_fingerprint(xr_type_new_error(NULL));
    uint64_t unknown_hash = xaot_type_fingerprint(unknown_type);
    uint64_t error_array_hash = xaot_type_fingerprint(error_array);
    uint64_t unknown_array_hash = xaot_type_fingerprint(unknown_array);

    TEST_REQUIRE(error_hash == error_hash_again, "ErrorType fingerprint is stable");
    TEST_REQUIRE(error_hash != unknown_hash, "ErrorType fingerprint differs from unknown");
    TEST_REQUIRE(error_array_hash != unknown_array_hash,
                 "nested ErrorType fingerprint differs from nested unknown");
}

static XiFunc *make_manual_extern_func(const char *source_name, const char *link_symbol,
                                       XrType *param_type, XrType *return_type) {
    XiFunc *func = xi_func_new(source_name, return_type);
    TEST_REQUIRE(func != NULL, "manual extern function allocated");
    func->is_extern = true;
    func->extern_symbol = link_symbol;
    if (param_type) {
        func->nparams = 1;
        func->params = (XiValue **) xr_calloc(1, sizeof(XiValue *));
        TEST_REQUIRE(func->params != NULL, "manual extern parameter array allocated");
        XiBlock *entry = xi_block_new(func);
        TEST_REQUIRE(entry != NULL, "manual extern entry block allocated");
        func->params[0] = xi_param(func, entry, 0, param_type);
        TEST_REQUIRE(func->params[0] != NULL, "manual extern parameter allocated");
    }
    return func;
}

TEST(aot_extern_registry_deduplicates_and_rejects_conflicts) {
    XrType unit_type = {
        .kind = XR_KIND_UNIT, .scalar_rep = XR_SCALAR_REP_NONE, .id = 120, .frozen = true};
    XrType int32_type = {
        .kind = XR_KIND_INT, .id = 121, .scalar_rep = XR_NATIVE_I32, .frozen = true};
    XrType float32_type = {
        .kind = XR_KIND_FLOAT, .id = 122, .scalar_rep = XR_NATIVE_F32, .frozen = true};
    XiFunc *init = xi_func_new("<module-init>", &unit_type);
    XiFunc *first =
        make_manual_extern_func("first_alias", "task208_same_symbol", &int32_type, &int32_type);
    XiFunc *duplicate =
        make_manual_extern_func("second_alias", "task208_same_symbol", &int32_type, &int32_type);
    XiFunc *conflict =
        make_manual_extern_func("bad_alias", "task208_same_symbol", &int32_type, &float32_type);
    XiFunc *unused =
        make_manual_extern_func("unused", "task208_unused_symbol", &int32_type, &int32_type);
    XiModule module = {.init = init, .name = "extern_registry_test"};
    XiModule *modules[] = {&module};
    XaotBundle bundle;
    TEST_REQUIRE(xaot_bundle_init(&bundle, modules, 1, 0), "extern registry bundle initialized");

    XiFunc *funcs[] = {first, duplicate, conflict, unused};
    for (uint32_t i = 0; i < sizeof(funcs) / sizeof(funcs[0]); i++) {
        XaotFuncPlan *plan = xaot_bundle_add_func_plan(&bundle, funcs[i], 0, 1);
        TEST_REQUIRE(plan != NULL, "manual extern function plan allocated");
        TEST_REQUIRE(xaot_abi_build_func(&plan->abi, &bundle, funcs[i], false),
                     "manual extern ABI built");
    }

    TEST_REQUIRE(xaot_bundle_register_extern_decl(&bundle, first, 11),
                 "first extern declaration registered");
    TEST_REQUIRE(xaot_bundle_register_extern_decl(&bundle, duplicate, 22),
                 "identical extern declaration deduplicated");
    TEST_REQUIRE(bundle.nextern_decls == 1, "identical link contracts share one registry row");
    TEST_REQUIRE(xaot_bundle_find_extern_decl_for_func(&bundle, first) ==
                     xaot_bundle_find_extern_decl_for_func(&bundle, duplicate),
                 "duplicate Xi functions map to the same stable declaration");
    TEST_REQUIRE(xaot_bundle_find_extern_decl_for_func(&bundle, unused) == NULL,
                 "unused extern declaration is absent from the registry");
    TEST_REQUIRE(!xaot_bundle_register_extern_decl(&bundle, conflict, 33),
                 "same link symbol with a different ABI is rejected");
    TEST_REQUIRE(bundle.error_msg && strstr(bundle.error_msg, "conflicting extern declarations"),
                 "extern conflict has a stable prepare diagnostic");

    xaot_bundle_free(&bundle);
    xi_func_free(first);
    xi_func_free(duplicate);
    xi_func_free(conflict);
    xi_func_free(unused);
    xi_func_free(init);
}

/* CGen fixtures bypass the global producer, so synthesize strong body anchors for strict plans. */
typedef struct TestAotEvidenceIds {
    XgFuncId next_func_id;
    XgDeclId next_decl_id;
    uint32_t next_source_node_id;
} TestAotEvidenceIds;

static const XiFunc *test_aot_direct_call_target(const XiFunc *owner, const XiValue *value) {
    if (!value || (value->op != XI_CALL && value->op != XI_TAIL_CALL) || value->nargs == 0)
        return NULL;
    const XiValue *callee = value->args[0];
    for (int depth = 0; callee && depth < 8; depth++) {
        if (callee->op == XI_GET_SHARED && owner && owner->shared_slot_funcs &&
            callee->aux_int >= 0 && callee->aux_int < owner->shared_slot_func_count)
            return owner->shared_slot_funcs[callee->aux_int];
        if ((callee->op == XI_CLOSURE_NEW ||
             (callee->op == XI_STACK_ALLOC && callee->aux_int == XI_CLOSURE_NEW)) &&
            callee->aux)
            return (const XiFunc *) callee->aux;
        if ((callee->op == XI_BOX || callee->op == XI_UNBOX || callee->op == XI_COPY ||
             xi_op_is_identity_forward(callee->op)) &&
            callee->nargs > 0) {
            callee = callee->args[0];
            continue;
        }
        break;
    }
    return NULL;
}

static bool test_aot_value_is_sync_runtime_op(const XiValue *value) {
    if (!value)
        return false;
    if (value->op == XI_PAR_FOR || value->op == XI_PAR_MAP || value->op == XI_PAR_REDUCE ||
        value->op == XI_CHAN_NEW || value->op == XI_CHAN_TRY_SEND ||
        value->op == XI_CHAN_TRY_RECV || value->op == XI_CHAN_IS_CLOSED)
        return true;
    if ((value->op == XI_CALL_METHOD || value->op == XI_CALL_METHOD_DIRECT) && value->aux) {
        const char *name = (const char *) value->aux;
        return strcmp(name, "close") == 0 || strcmp(name, "trySend") == 0 ||
               strcmp(name, "tryRecv") == 0 || strcmp(name, "isClosed") == 0;
    }
    return false;
}

static bool test_aot_value_may_suspend(const XiValue *value) {
    if (!value || test_aot_value_is_sync_runtime_op(value))
        return false;
    if ((value->flags & XI_FLAG_MAY_SUSPEND) != 0)
        return true;
    if (xi_value_is_channel_method_call(value, "send", 1) ||
        xi_value_is_channel_method_call(value, "sendTimeout", 2) ||
        xi_value_is_channel_method_call(value, "recv", 0) ||
        xi_value_is_channel_method_call(value, "recvOr", 1) ||
        xi_value_is_channel_method_call(value, "recvTimeout", 1))
        return true;
    if ((value->op == XI_CALL_METHOD || value->op == XI_CALL_METHOD_DIRECT) && value->aux &&
        strcmp((const char *) value->aux, "sleep") == 0)
        return true;
    return xi_value_is_blocking_task_method_call(value) ||
           xi_value_is_blocking_work_queue_method_call(value) ||
           xi_value_is_blocking_result_group_method_call(value) ||
           xi_value_is_blocking_countdown_latch_method_call(value) ||
           xi_value_is_blocking_semaphore_method_call(value) ||
           xi_value_is_blocking_event_count_method_call(value);
}

static uint32_t test_aot_value_runtime_capabilities(const XiValue *value) {
    if (!value)
        return 0;
    const XiValue *receiver = value->nargs >= 1 ? value->args[0] : NULL;
    if (xi_value_type_is_channel(value) || xi_value_type_is_channel(receiver))
        return XG_CAP_CHANNEL | XG_CAP_COROUTINE | XG_CAP_OBJECTS;
    if (xi_value_type_is_task(value) || xi_value_type_is_task(receiver))
        return XG_CAP_TASK | XG_CAP_COROUTINE | XG_CAP_OBJECTS;
    if (xi_value_type_is_atomic(value) || xi_value_type_is_atomic(receiver))
        return XG_CAP_ATOMIC | XG_CAP_OBJECTS;
    if (xi_value_type_is_work_queue(value) || xi_value_type_is_work_queue(receiver))
        return XG_CAP_WORK_QUEUE | XG_CAP_COROUTINE | XG_CAP_OBJECTS;
    if (xi_value_type_is_result_group(value) || xi_value_type_is_result_group(receiver))
        return XG_CAP_RESULT_GROUP | XG_CAP_COROUTINE | XG_CAP_OBJECTS;
    if (xi_value_type_is_countdown_latch(value) || xi_value_type_is_countdown_latch(receiver))
        return XG_CAP_COUNTDOWN_LATCH | XG_CAP_COROUTINE | XG_CAP_OBJECTS;
    if (xi_value_type_is_semaphore(value) || xi_value_type_is_semaphore(receiver))
        return XG_CAP_SEMAPHORE | XG_CAP_COROUTINE | XG_CAP_OBJECTS;
    if (xi_value_type_is_event_count(value) || xi_value_type_is_event_count(receiver))
        return XG_CAP_EVENT_COUNT | XG_CAP_COROUTINE | XG_CAP_OBJECTS;
    return 0;
}

static uint32_t test_aot_effect_bits_depth(const XiFunc *func, int depth) {
    uint32_t effects = 0;
    if (!func || depth > 16)
        return 0;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *value = block->values[vi];
            if (!value)
                continue;
            if (test_aot_value_may_suspend(value))
                effects |= XG_BODY_MAY_SUSPEND;
            if ((value->flags & XI_FLAG_MAY_THROW) != 0)
                effects |= XG_BODY_MAY_ERROR;
            if ((value->flags & XI_FLAG_WRITES_MEM) != 0)
                effects |= XG_BODY_MAY_MUTATE;
            if ((value->flags & XI_FLAG_READS_MEM) != 0)
                effects |= XG_BODY_MAY_READ_MEM;
            if (value->op == XI_GO || value->op == XI_THREAD_SPAWN)
                effects |= XG_BODY_MAY_SPAWN;
            const XiFunc *target = test_aot_direct_call_target(func, value);
            if (target && target != func)
                effects |= test_aot_effect_bits_depth(target, depth + 1);
        }
    }
    return effects;
}

static uint32_t test_aot_effect_bits(const XiFunc *func) {
    return test_aot_effect_bits_depth(func, 0);
}

static uint32_t test_aot_capability_bits_depth(const XiFunc *func, int depth) {
    uint32_t capabilities = 0;
    if (!func || depth > 16)
        return 0;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *value = block->values[vi];
            if (!value)
                continue;
            bool parallel =
                value->op == XI_PAR_FOR || value->op == XI_PAR_MAP || value->op == XI_PAR_REDUCE;
            if (test_aot_value_may_suspend(value))
                capabilities |= XG_CAP_COROUTINE;
            if (parallel)
                capabilities |= XG_CAP_PARALLEL;
            if (value->op == XI_CHAN_NEW || value->op == XI_CHAN_TRY_SEND ||
                value->op == XI_CHAN_TRY_RECV || value->op == XI_CHAN_IS_CLOSED)
                capabilities |= XG_CAP_CHANNEL | XG_CAP_OBJECTS;
            if (value->op == XI_GO)
                capabilities |= XG_CAP_COROUTINE | XG_CAP_TASK;
            if (value->op == XI_THREAD_SPAWN)
                capabilities |= XG_CAP_COROUTINE | XG_CAP_TASK | XG_CAP_SYS_THREAD;
            capabilities |= test_aot_value_runtime_capabilities(value);
            const XiFunc *target = test_aot_direct_call_target(func, value);
            if (target && target != func)
                capabilities |= test_aot_capability_bits_depth(target, depth + 1);
        }
    }
    return capabilities;
}

static uint32_t test_aot_capability_bits(const XiFunc *func) {
    return test_aot_capability_bits_depth(func, 0);
}

static void test_aot_add_function_evidence(TestAotPlan *plan, XiFunc *func, XgModuleId module_id,
                                           TestAotEvidenceIds *ids) {
    if (!func)
        return;

    XgFuncId func_id = ids->next_func_id++;
    XgDeclId decl_id = ids->next_decl_id++;
    uint32_t source_node_id = ids->next_source_node_id++;
    uint32_t name_id = xg_name_id(func->name ? func->name : "<anonymous>");
    uint32_t signature_key = source_node_id;
    XgDeclSummary decl = {
        .module_id = module_id,
        .source_node_id = source_node_id,
        .decl_id = decl_id,
        .kind = XG_DECL_FUNC,
        .name_id = name_id,
        .signature_key = signature_key,
        .source_span_id = source_node_id,
    };
    XgBodySummary body = {
        .func_id = func_id,
        .module_id = module_id,
        .source_node_id = source_node_id,
        .owner_decl_id = decl_id,
        .name_id = name_id,
        .signature_key = signature_key,
        .source_span_id = source_node_id,
        .kind = XG_BODY_FUNCTION,
        .effect_bits = test_aot_effect_bits(func),
        .capability_bits = test_aot_capability_bits(func),
        .body_hash = ((uint64_t) module_id << 32) | func_id,
    };

    func->xg_body_func_id = func_id;
    TEST_REQUIRE(xg_global_evidence_add_decl(&plan->evidence, &decl) != NULL,
                 "AOT function declaration evidence allocation failed");
    TEST_REQUIRE(xg_global_evidence_add_body(&plan->evidence, &body) != NULL,
                 "AOT function body evidence allocation failed");

    for (uint16_t i = 0; i < func->nchildren; i++)
        test_aot_add_function_evidence(plan, func->children[i], module_id, ids);
}

static bool test_aot_rebase_coroutine_plans(XiFunc *func) {
    if (!func)
        return true;
    if (func->coro_plan && !xi_coro_plan_rebase(func))
        return false;
    for (uint16_t i = 0; i < func->nchildren; i++) {
        if (!test_aot_rebase_coroutine_plans(func->children[i]))
            return false;
    }
    return true;
}

static uint8_t test_aot_class_field_semantic_kind(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_I8:
            return XG_CLASS_FIELD_TYPE_I8;
        case XR_NATIVE_U8:
            return XG_CLASS_FIELD_TYPE_U8;
        case XR_NATIVE_I16:
            return XG_CLASS_FIELD_TYPE_I16;
        case XR_NATIVE_U16:
            return XG_CLASS_FIELD_TYPE_U16;
        case XR_NATIVE_I32:
            return XG_CLASS_FIELD_TYPE_I32;
        case XR_NATIVE_U32:
            return XG_CLASS_FIELD_TYPE_U32;
        case XR_NATIVE_I64:
            return XG_CLASS_FIELD_TYPE_I64;
        case XR_NATIVE_U64:
            return XG_CLASS_FIELD_TYPE_U64;
        case XR_NATIVE_ISIZE:
            return XG_CLASS_FIELD_TYPE_ISIZE;
        case XR_NATIVE_USIZE:
            return XG_CLASS_FIELD_TYPE_USIZE;
        case XR_NATIVE_F32:
            return XG_CLASS_FIELD_TYPE_F32;
        case XR_NATIVE_F64:
            return XG_CLASS_FIELD_TYPE_F64;
        case XR_NATIVE_BOOL:
            return XG_CLASS_FIELD_TYPE_BOOL;
        case XR_NATIVE_STRING:
            return XG_CLASS_FIELD_TYPE_STRING;
        case XR_NATIVE_ARRAY_REF:
            return XG_CLASS_FIELD_TYPE_ARRAY;
        case XR_NATIVE_MAP_REF:
            return XG_CLASS_FIELD_TYPE_MAP;
        case XR_NATIVE_SET_REF:
            return XG_CLASS_FIELD_TYPE_SET;
        default:
            return XG_CLASS_FIELD_TYPE_DYNAMIC;
    }
}

static uint8_t test_aot_class_field_scalar_rep(uint8_t semantic_kind, uint8_t native_type) {
    switch ((XgClassFieldTypeKind) semantic_kind) {
        case XG_CLASS_FIELD_TYPE_I8:
        case XG_CLASS_FIELD_TYPE_U8:
        case XG_CLASS_FIELD_TYPE_I16:
        case XG_CLASS_FIELD_TYPE_U16:
        case XG_CLASS_FIELD_TYPE_I32:
        case XG_CLASS_FIELD_TYPE_U32:
        case XG_CLASS_FIELD_TYPE_I64:
        case XG_CLASS_FIELD_TYPE_U64:
        case XG_CLASS_FIELD_TYPE_ISIZE:
        case XG_CLASS_FIELD_TYPE_USIZE:
        case XG_CLASS_FIELD_TYPE_F32:
        case XG_CLASS_FIELD_TYPE_F64:
            return native_type;
        default:
            return XR_SCALAR_REP_NONE;
    }
}

static uint32_t test_aot_class_type_key(const char *name, uint32_t fallback) {
    uint32_t key = xg_name_id(name ? name : "");
    return key ? key : fallback;
}

static XgClassSummary *test_aot_find_class_by_name_mut(XgGlobalEvidence *ev, const char *name) {
    uint32_t name_id = xg_name_id(name ? name : "");
    if (!ev || name_id == 0)
        return NULL;
    for (uint32_t i = 0; i < ev->nclasses; i++) {
        XgClassSummary *cls = &ev->classes[i];
        if (cls->name_id == name_id)
            return cls;
    }
    return NULL;
}

static void test_aot_add_class_evidence(TestAotPlan *plan, XiModule *module, XgModuleId module_id,
                                        TestAotEvidenceIds *ids) {
    if (!plan || !module || !ids)
        return;
    for (uint16_t ci = 0; ci < module->nclasses; ci++) {
        const XiClassData *cd = module->classes ? module->classes[ci] : NULL;
        const XrAggregateLayout *layout = cd ? cd->instance_layout : NULL;
        uint32_t total_fields = layout ? layout->field_count : 0;
        uint32_t inherited_fields = cd ? cd->inherited_field_count : 0;
        uint32_t own_field_count =
            total_fields > inherited_fields ? total_fields - inherited_fields : 0;
        XgDeclId decl_id;
        XgClassId class_id;
        uint32_t source_node_id;
        uint32_t name_id;
        uint32_t field_start;
        XgClassSummary *parent_summary;
        XgDeclSummary decl;
        XgClassSummary cls;
        if (!cd || !cd->class_name || cd->struct_layout)
            continue;
        decl_id = ids->next_decl_id++;
        class_id = (XgClassId) (plan->evidence.nclasses + 1);
        source_node_id = ids->next_source_node_id++;
        name_id = xg_name_id(cd->class_name);
        field_start = own_field_count > 0 ? plan->evidence.nclass_fields + 1 : 0;

        memset(&decl, 0, sizeof(decl));
        decl.module_id = module_id;
        decl.source_node_id = source_node_id;
        decl.decl_id = decl_id;
        decl.kind = XG_DECL_CLASS;
        decl.name_id = name_id;
        decl.source_span_id = source_node_id;
        TEST_REQUIRE(xg_global_evidence_add_decl(&plan->evidence, &decl) != NULL,
                     "AOT class declaration evidence allocation failed");

        for (uint32_t fi = 0; fi < own_field_count; fi++) {
            uint32_t layout_idx = inherited_fields + fi;
            const XrAggregateFieldLayout *layout_field = &layout->fields[layout_idx];
            const char *field_name = layout->field_names && layout_idx < layout->field_count
                                         ? layout->field_names[layout_idx]
                                         : NULL;
            uint8_t semantic_kind = test_aot_class_field_semantic_kind(layout_field->native_type);
            XaotClassFieldPhysicalLayout physical;
            XgClassFieldSummary field;
            memset(&field, 0, sizeof(field));
            field.field_id = (XgFieldId) (plan->evidence.nclass_fields + 1);
            field.module_id = module_id;
            field.source_node_id = ids->next_source_node_id++;
            field.owner_class_id = class_id;
            field.name_id = xg_name_id(field_name ? field_name : "");
            field.type_key = test_aot_class_type_key(field_name, field.source_node_id);
            field.decl_ordinal = fi;
            field.instance_slot = layout_idx;
            field.semantic_kind = semantic_kind;
            field.scalar_rep =
                test_aot_class_field_scalar_rep(semantic_kind, layout_field->native_type);
            if (xaot_class_field_physical_layout(&plan->bundle.target_data_layout, semantic_kind,
                                                 &physical) &&
                physical.ownership == XAOT_CLASS_FIELD_OWNERSHIP_OWNED)
                field.flags |= XG_CLASS_FIELD_OWNED_REF;
            TEST_REQUIRE(xg_global_evidence_add_class_field(&plan->evidence, &field) != NULL,
                         "AOT class field evidence allocation failed");
        }

        memset(&cls, 0, sizeof(cls));
        cls.module_id = module_id;
        cls.decl_id = decl_id;
        cls.class_id = class_id;
        parent_summary = cd->super_name
                             ? test_aot_find_class_by_name_mut(&plan->evidence, cd->super_name)
                             : NULL;
        cls.parent_class_id = parent_summary ? parent_summary->class_id : XG_NO_ID;
        cls.name_id = name_id;
        cls.flags = XG_CLASS_INFERRED_FINAL;
        cls.field_start = field_start;
        cls.field_count = own_field_count;
        cls.decl_kind = XG_DECL_CLASS;
        TEST_REQUIRE(xg_global_evidence_add_class(&plan->evidence, &cls) != NULL,
                     "AOT class evidence allocation failed");
        if (parent_summary) {
            parent_summary->flags |= XG_CLASS_HAS_SUBCLASS;
            parent_summary->flags &= ~XG_CLASS_INFERRED_FINAL;
        }
    }
}

static const XgClassFieldSummary *test_aot_find_class_field_for_value(const XgGlobalEvidence *ev,
                                                                      const XiValue *v) {
    const XrType *receiver_type;
    uint32_t class_name_id;
    uint32_t field_name_id;
    const XgClassSummary *cls = NULL;
    if (!ev || !v || (v->op != XI_LOAD_FIELD && v->op != XI_STORE_FIELD) ||
        v->xg_class_field_id != 0 || !v->aux || v->nargs == 0 || !v->args || !v->args[0])
        return NULL;
    receiver_type = v->args[0]->type;
    if (!receiver_type ||
        (receiver_type->kind != XR_KIND_CLASS && receiver_type->kind != XR_KIND_INSTANCE) ||
        !receiver_type->instance.class_name)
        return NULL;
    class_name_id = xg_name_id(receiver_type->instance.class_name);
    field_name_id = xg_name_id((const char *) v->aux);
    if (class_name_id == 0 || field_name_id == 0)
        return NULL;
    for (uint32_t i = 0; i < ev->nclasses; i++) {
        if (ev->classes[i].name_id == class_name_id) {
            cls = &ev->classes[i];
            break;
        }
    }
    for (uint32_t depth = 0; cls && depth < 64; depth++) {
        uint32_t start = cls->field_start ? cls->field_start - 1 : 0;
        for (uint32_t i = 0; i < cls->field_count && start + i < ev->nclass_fields; i++) {
            const XgClassFieldSummary *field = &ev->class_fields[start + i];
            if (field->owner_class_id == cls->class_id && field->name_id == field_name_id &&
                (field->flags & XG_CLASS_FIELD_STATIC) == 0)
                return field;
        }
        if (cls->parent_class_id == XG_NO_ID)
            return NULL;
        XgClassId parent_class_id = cls->parent_class_id;
        cls = NULL;
        for (uint32_t i = 0; i < ev->nclasses; i++) {
            if (ev->classes[i].class_id == parent_class_id) {
                cls = &ev->classes[i];
                break;
            }
        }
    }
    return NULL;
}

static void test_aot_annotate_class_field_values_in_func(XiFunc *func, const XgGlobalEvidence *ev) {
    if (!func || !ev)
        return;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            XiValue *v = block->values[vi];
            const XgClassFieldSummary *field = test_aot_find_class_field_for_value(ev, v);
            if (field)
                v->xg_class_field_id = field->field_id;
        }
    }
    for (uint16_t ci = 0; ci < func->nchildren; ci++)
        test_aot_annotate_class_field_values_in_func(func->children[ci], ev);
}

static void test_aot_annotate_class_field_values(XiModule **modules, uint32_t nmodules,
                                                 const XgGlobalEvidence *ev) {
    for (uint32_t mi = 0; mi < nmodules; mi++) {
        XiModule *module = modules[mi];
        if (module && module->init)
            test_aot_annotate_class_field_values_in_func(module->init, ev);
    }
}

/* One emission plan per module, from the TargetPlan the bundle already holds.
 * The bundle borrows the rows; the fixture owns them. */
static bool test_aot_plan_build_emission_plans(TestAotPlan *plan) {
    if (!plan || !plan->bundle.module_emission_plans || plan->bundle.nmodules == 0)
        return false;
    plan->emission_plans =
        (const XrCEmissionPlan **) xr_calloc(plan->bundle.nmodules, sizeof(*plan->emission_plans));
    if (!plan->emission_plans)
        return false;
    plan->nemission_plans = plan->bundle.nmodules;
    for (uint32_t i = 0; i < plan->nemission_plans; i++) {
        const XrTargetPlan *target_plan = xaot_bundle_program_semantic_for_module(&plan->bundle, i)
                                              ? xaot_bundle_program_target_plan(&plan->bundle)
                                              : NULL;
        if (!target_plan)
            continue;
        XrCEmissionPlan *emission_plan = NULL;
        char error[512] = {0};
        if (!xr_c_emission_plan_build(
                target_plan, xr_target_profile_fingerprint(xr_target_plan_profile(target_plan)),
                &emission_plan, error, sizeof(error)) ||
            !xr_c_emission_plan_is_verified(emission_plan)) {
            fprintf(stderr, "  C emission plan fixture error: %s\n",
                    error[0] ? error : "unverified emission plan");
            xr_c_emission_plan_free(emission_plan);
            return false;
        }
        plan->emission_plans[i] = emission_plan;
        plan->bundle.module_emission_plans[i] = emission_plan;
    }
    return true;
}

static bool test_aot_plan_try_prepare(TestAotPlan *plan, XiModule **modules, uint32_t nmodules,
                                      uint32_t entry_module) {
    char verify_err[256];

    TEST_REQUIRE(plan != NULL, "AOT plan holder is NULL");
    memset(plan, 0, sizeof(*plan));
    TEST_REQUIRE(xaot_bundle_init(&plan->bundle, modules, nmodules, entry_module),
                 "AOT bundle init failed");
    plan->initialized = true;
    XgBuildKey key = {.source_hash = 0,
                      .compiler_semver_hash = 0x171,
                      .profile_hash = 0,
                      .imported_summary_hash = 0,
                      .module_id = entry_module + 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    xg_global_evidence_init(&plan->evidence, key);
    plan->evidence_initialized = true;
    TestAotEvidenceIds ids = {.next_func_id = 1, .next_decl_id = 1, .next_source_node_id = 1};
    for (uint32_t i = 0; i < nmodules; i++) {
        XiModule *module = modules[i];
        XgModuleId module_id = (XgModuleId) (i + 1);
        XgFuncId func_id;
        XgBodySummary body;

        if (!module || !module->init)
            continue;
        test_aot_add_class_evidence(plan, module, module_id, &ids);
        func_id = ids.next_func_id++;
        body = (XgBodySummary) {
            .func_id = func_id,
            .module_id = module_id,
            .name_id = xg_name_id("<module-init>"),
            .kind = XG_BODY_MODULE_INIT,
            .effect_bits = test_aot_effect_bits(module->init),
            .capability_bits = test_aot_capability_bits(module->init),
            .body_hash = ((uint64_t) module_id << 32) | func_id,
        };
        module->init->xg_body_func_id = func_id;
        TEST_REQUIRE(xg_global_evidence_add_body(&plan->evidence, &body) != NULL,
                     "AOT module-init body evidence allocation failed");
        for (uint32_t slot = 0; slot < module->nslots; slot++) {
            const char *name =
                module->init->slot_owned_names ? module->init->slot_owned_names[slot] : NULL;
            if (!name)
                continue;
            bool is_const =
                module->init->slot_owned_consts && module->init->slot_owned_consts[slot] != 0;
            uint32_t source_node_id = ids.next_source_node_id++;
            XgDeclSummary storage = {
                .module_id = module_id,
                .decl_id = ids.next_decl_id++,
                .kind = XG_DECL_GLOBAL,
                .name_id = xg_name_id(name),
                .source_node_id = source_node_id,
                .source_span_id = source_node_id,
                .signature_key = source_node_id,
                .storage_domain = XR_STORAGE_MODULE_STATIC,
                .storage_mutability = is_const ? XR_STORAGE_READONLY : XR_STORAGE_MUTABLE,
                .address_identity = XR_ADDRESS_MODULE_STABLE,
                .materialization_kind =
                    is_const ? XR_MATERIALIZE_STATIC_DATA : XR_MATERIALIZE_STATIC_DATA,
            };
            TEST_REQUIRE(xg_global_evidence_add_decl(&plan->evidence, &storage) != NULL,
                         "AOT storage declaration evidence allocation failed");
        }
        for (uint16_t ci = 0; ci < module->init->nchildren; ci++)
            test_aot_add_function_evidence(plan, module->init->children[ci], module_id, &ids);
    }
    for (uint32_t i = 0; i < nmodules; i++) {
        TEST_REQUIRE(!modules[i] || test_aot_rebase_coroutine_plans(modules[i]->init),
                     "AOT evidence IDs rebase frozen coroutine plans");
    }
    if (!xaot_bundle_set_global_evidence(&plan->bundle, &plan->evidence, XG_BUILD_NATIVE_RELEASE))
        return false;
    XrTargetProfile *target_profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    if (!target_profile)
        return false;
    XiModule *module = nmodules == 1u ? modules[0] : NULL;
    XrTargetPlan *target_plan = NULL;
    char target_error[512] = {0};
    if (!module || !module->init || !module->init->semantic_plan ||
        !xr_target_plan_build(module->init->semantic_plan, target_profile, &target_plan,
                              target_error, sizeof(target_error)) ||
        !xaot_bundle_set_program_target_plan(&plan->bundle, target_plan)) {
        fprintf(stderr, "  Program TargetPlan fixture error: %s\n",
                target_error[0] ? target_error : "multi-module fixture lacks program authority");
        xr_target_plan_free(target_plan);
        xr_target_profile_free(target_profile);
        return false;
    }
    xr_target_plan_free(target_plan);
    xr_target_profile_free(target_profile);
    /* Built before prepare and hung on the bundle, as the driver does. Prepare
     * asks the emission plan for the C spelling of every raw pointer, so a
     * bundle without one cannot prepare a function that takes an address. */
    if (!test_aot_plan_build_emission_plans(plan))
        return false;
    if (!xaot_prepare_bundle(&plan->bundle, NULL))
        return false;
    test_aot_annotate_class_field_values(modules, nmodules, &plan->evidence);
    if (!xaot_verify_bundle(&plan->bundle, verify_err, sizeof(verify_err))) {
        fprintf(stderr, "  AOT verify error: %s\n", verify_err);
        for (uint32_t i = 0; i < plan->evidence.nclasses; i++) {
            const XgClassSummary *cls = &plan->evidence.classes[i];
            fprintf(stderr, "  class[%u] id=%u parent=%u name=%u flags=0x%x fields=%u+%u\n", i,
                    cls->class_id, cls->parent_class_id, cls->name_id, cls->flags, cls->field_start,
                    cls->field_count);
        }
        return false;
    }
    return true;
}

static void test_aot_plan_prepare(TestAotPlan *plan, XiModule **modules, uint32_t nmodules,
                                  uint32_t entry_module) {
    bool prepared = test_aot_plan_try_prepare(plan, modules, nmodules, entry_module);
    if (!prepared && plan && plan->bundle.error_msg)
        fprintf(stderr, "  AOT plan error: %s\n", plan->bundle.error_msg);
    TEST_REQUIRE(prepared, "AOT plan preparation failed");
}

static void test_aot_plan_free(TestAotPlan *plan) {
    if (plan && plan->emission_plans) {
        for (uint32_t i = 0; i < plan->nemission_plans; i++)
            xr_c_emission_plan_free((XrCEmissionPlan *) plan->emission_plans[i]);
        xr_free((void *) plan->emission_plans);
        plan->emission_plans = NULL;
        plan->nemission_plans = 0;
    }
    if (plan && plan->initialized)
        xaot_bundle_free(&plan->bundle);
    if (plan && plan->evidence_initialized)
        xg_global_evidence_free(&plan->evidence);
}

typedef struct TestCEmissionRegistry {
    const XrCEmissionPlan **plans;
    uint32_t count;
} TestCEmissionRegistry;

static void test_c_emission_registry_free(TestCEmissionRegistry *registry) {
    if (!registry)
        return;
    for (uint32_t i = 0; i < registry->count; i++)
        xr_c_emission_plan_free((XrCEmissionPlan *) registry->plans[i]);
    xr_free(registry->plans);
    memset(registry, 0, sizeof(*registry));
}

/* A hand-built XI_PRINT carries the same authority a lowered one does: the
 * frozen plan, and a source span that agrees with the plan's location field
 * for field. The semantic builder compares the two, so a fixture that sets
 * neither is refused before code generation is ever reached. */
static bool test_attach_print_plan(XiFunc *func, XiValue *print, uint32_t line) {
    XrLocation source = {
        .file = "test.xr",
        .line = line,
        .column = 1,
        .end_line = line,
        .end_column = 6,
    };
    XrPrintPlan plan;
    if (xr_print_plan_build(XR_CORE_BUILTIN_PRINT, print->nargs, source,
                            XR_CORE_INTRINSIC_TARGET_OUTPUT_ALL, XR_PRINT_CAPABILITY_NONE,
                            &plan) != XR_PRINT_PLAN_OK ||
        !xi_value_set_print_plan(func, print, &plan))
        return false;
    print->source_span = (XiSourceSpan) {
        .start_line = source.line,
        .start_column = source.column,
        .end_line = source.end_line,
        .end_column = source.end_column,
    };
    print->flags = xi_op_default_effects(XI_PRINT);
    print->line = line;
    return true;
}

static bool test_c_emission_registry_install(TestCEmissionRegistry *registry, XiCgenCtx *ctx,
                                             const XaotBundle *bundle) {
    if (!registry || !ctx || !bundle || !bundle->program_target_plan || bundle->nmodules == 0)
        return false;
    memset(registry, 0, sizeof(*registry));
    registry->plans =
        (const XrCEmissionPlan **) xr_calloc(bundle->nmodules, sizeof(*registry->plans));
    if (!registry->plans)
        return false;
    registry->count = bundle->nmodules;
    for (uint32_t i = 0; i < registry->count; i++) {
        const XrTargetPlan *target_plan = xaot_bundle_program_semantic_for_module(bundle, i)
                                              ? xaot_bundle_program_target_plan(bundle)
                                              : NULL;
        XrCEmissionPlan *emission_plan = NULL;
        char error[512] = {0};
        if (!target_plan ||
            !xr_c_emission_plan_build(
                target_plan, xr_target_profile_fingerprint(xr_target_plan_profile(target_plan)),
                &emission_plan, error, sizeof(error))) {
            fprintf(stderr, "  C emission registry fixture error: %s\n",
                    error[0] ? error : "missing TargetPlan authority");
            test_c_emission_registry_free(registry);
            return false;
        }
        registry->plans[i] = emission_plan;
    }
    if (!xi_cgen_ctx_set_value_emission_plans(ctx, registry->plans, registry->count)) {
        test_c_emission_registry_free(registry);
        return false;
    }
    return true;
}

static char *generate_c_with_status(XiFunc *ir, const char *module_name, bool *had_error);

/* Compile source to Xi IR (without emitting bytecode). */
static XiFunc *compile_to_ir_with_config(const char *source, XiPipelineConfig cfg) {
    assert(g_iso != NULL);

    XrCompilerSession *session = xr_compiler_session_current_for_isolate(g_iso);
    XaAnalyzer *analyzer = xa_analyzer_new(session);
    if (!analyzer)
        return NULL;

    AstNode *program = xr_parse(session, source);
    if (!program) {
        fprintf(stderr, "  PARSE FAILED for: %.60s...\n", source);
        xa_analyzer_free(analyzer);
        return NULL;
    }

    /* These cases exercise the concurrency primitives whose types are declared
     * for the runtime rather than exported from a resolvable module. This
     * analyzer runs without a module graph, so `import ... from sync` cannot
     * resolve here even though it does in a real compilation; analyzing them
     * as the sync module itself puts those declarations in scope. */
    const char *analyzer_file =
        (strstr(source, "WorkQueue<i64>") || strstr(source, "CountdownLatch") ||
         strstr(source, "ResultGroup") || strstr(source, "Semaphore"))
            ? "stdlib/sync/sync.xr"
            : "test.xr";
    xa_analyzer_analyze(analyzer, analyzer_file, program);

    cfg.run_emit = false; /* cgen tests need the IR tree, not bytecode */
    cfg.source_file = analyzer_file;
    cfg.module_identity = "memory-module-v1:id=18:xi-cgen-fixture-v1";

    XiPipelineResult res = xi_pipeline_compile_program(program, analyzer, g_iso, &cfg);

    xa_analyzer_free(analyzer);
    xr_program_destroy(program);

    if (res.status != XI_PIPE_OK) {
        fprintf(stderr, "  PIPELINE FAILED: %s%s%s\n", xi_pipe_status_str(res.status),
                res.error.detail[0] ? ": " : "", res.error.detail);
        xi_pipeline_result_free(&res);
        return NULL;
    }

    XiFunc *ir = res.ir;
    res.ir = NULL;
    xi_pipeline_result_free(&res);

    return ir;
}

static XiFunc *compile_to_ir(const char *source) {
    XiPipelineConfig cfg = xi_pipeline_default_config();
    cfg.run_optimize = false;
    return compile_to_ir_with_config(source, cfg);
}

TEST(target_plan_owned_string_lifecycle_from_source) {
    const char *source = "fn probe() -> i64 {\n"
                         "    var text = string(7) + \"-frame\"\n"
                         "    Coro.yield()\n"
                         "    return len(text)\n"
                         "}\n"
                         "print(probe())\n";
    XiFunc *ir = compile_to_ir(source);
    TEST_REQUIRE(ir && ir->semantic_plan, "source lifecycle fixture froze SemanticPlan authority");
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    TEST_REQUIRE(profile &&
                     xr_target_plan_build(ir->semantic_plan, profile, &plan, error, sizeof(error)),
                 "source lifecycle fixture built exact TargetPlan authority");

    uint32_t root_count = 0;
    uint32_t root_slot_count = 0;
    uint32_t cleanup_count = 0;
    const XrTargetRootMapRecord *roots = xr_target_plan_root_maps(plan, &root_count);
    const uint32_t *root_slots = xr_target_plan_root_slots(plan, &root_slot_count);
    const XrTargetCleanupRecord *cleanups = xr_target_plan_cleanups(plan, &cleanup_count);
    TEST_REQUIRE(roots && root_slots && cleanups && root_count == 1 && root_slot_count == 1 &&
                     cleanup_count == 2 &&
                     roots[0].flags ==
                         (XR_TARGET_ROOT_SUSPEND | XR_TARGET_ROOT_CANCEL | XR_TARGET_ROOT_EXIT) &&
                     roots[0].slot_begin == 0 && roots[0].slot_count == 1,
                 "source lifecycle fixture froze one exact root set");
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
    const XrTargetSlotRecord *slot =
        slots && root_slots[0] < slot_count ? &slots[root_slots[0]] : NULL;
    const XrTargetMachineRepRecord *rep =
        slot ? xr_target_plan_machine_rep(plan, slot->memory_rep) : NULL;
    TEST_REQUIRE(slot && rep && rep->kind == XR_MACHINE_REP_DYN_VALUE &&
                     rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
                     rep->ownership == XR_TARGET_OWNERSHIP_OWNED,
                 "source lifecycle root names exact owned String storage");
    uint32_t normal = 0;
    uint32_t terminal = 0;
    for (uint32_t i = 0; i < cleanup_count; i++) {
        TEST_REQUIRE(cleanups[i].slot == slot->id &&
                         cleanups[i].action == XR_TARGET_CLEANUP_RELEASE &&
                         cleanups[i].provider == 0,
                     "source lifecycle cleanup names the exact rooted slot");
        normal += cleanups[i].flags == 0;
        terminal += cleanups[i].flags == (XR_TARGET_CLEANUP_CANCEL | XR_TARGET_CLEANUP_EXIT);
    }
    TEST_REQUIRE(normal == 1 && terminal == 1,
                 "source lifecycle freezes normal and terminal release");

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xi_func_free(ir);
}

/* Selective imports are resolved from the source module's semantic export
 * table. Keep graph-sensitive tests on the same single-source-of-truth path as
 * production compilation instead of duplicating .xr declarations as analyzer
 * builtins. */
static XiFunc *compile_to_ir_with_module_graph_config(const char *source, XiPipelineConfig cfg) {
    assert(g_iso != NULL);

    XrCompilerSession *session = xr_compiler_session_current_for_isolate(g_iso);
    XrModuleRegistry *registry = xr_isolate_get_module_registry(g_iso);
    XrModuleResolver *resolver = xr_module_registry_get_resolver(registry);
    XrModuleGraph *graph = resolver ? xr_module_graph_new(session, resolver) : NULL;
    XaAnalyzer *analyzer = NULL;
    XiFunc *ir = NULL;
    XiModule **graph_modules = NULL;
    if (!graph)
        goto cleanup;

    char *build_error = NULL;
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_MEMORY,
        .namespace_id = "cgen-test",
    };
    if (xr_module_graph_build_source(graph, &authority, source, &build_error) != 0) {
        fprintf(stderr, "  MODULE GRAPH FAILED: %s\n",
                build_error ? build_error : "unknown graph build error");
        xr_free(build_error);
        goto cleanup;
    }
    xr_free(build_error);
    if (xr_module_graph_topological_sort(graph) != 0 || graph->has_cycle) {
        fprintf(stderr, "  MODULE GRAPH FAILED: %s\n",
                graph->cycle_desc ? graph->cycle_desc : "import cycle");
        goto cleanup;
    }

    analyzer = xa_analyzer_new(session);
    if (!analyzer)
        goto cleanup;
    xa_analyzer_set_graph(analyzer, graph);

    for (int ti = 0; ti < graph->topo_count; ti++) {
        int idx = graph->topo_order[ti];
        XrModuleSpec *spec = &graph->specs[idx];
        if (!spec->ast)
            continue;
        const char *file = spec->source_path ? spec->source_path : "<cgen-test>";
        xa_analyzer_analyze(analyzer, file, (XrAstNode *) spec->ast);

        int diag_count = 0;
        XaDiagnostic *diagnostics = xa_analyzer_get_diagnostics(analyzer, &diag_count);
        bool has_error = false;
        for (XaDiagnostic *diag = diagnostics; diag; diag = diag->next) {
            if (diag->severity != XR_DIAG_SEV_ERROR)
                continue;
            fprintf(stderr, "  ANALYZE FAILED (%s:%u:%u): %s\n", file, diag->location.line,
                    diag->location.column, diag->message);
            has_error = true;
        }
        if (has_error)
            goto cleanup;

        XrHashMap *exports = NULL;
        if (!xa_analyzer_collect_export_symbols_checked(analyzer, (XrAstNode *) spec->ast,
                                                        &exports))
            goto cleanup;
        spec->export_symbols = exports;
        xa_analyzer_clear_diagnostics(analyzer);
    }

    XrModuleSpec *entry = &graph->specs[graph->entry_index];
    /* This helper compiles only the entry module. A zeroed topo array still
     * lets the production resolver close named native imports: an absent
     * source module is then an exact native-registry lookup, while a graph
     * source dependency has no XiModule pointer and remains fail-closed. */
    graph_modules = (XiModule **) xr_calloc((size_t) graph->topo_count, sizeof(*graph_modules));
    if (!graph_modules)
        goto cleanup;
    cfg.module_identity = entry->canonical;
    /* The analyzer registered this unit's file scope under the same fallback
     * name below, and lowering asks for that scope back by whatever name it is
     * given here. A memory module has no source path, so the two must agree on
     * one placeholder -- naming it only on the analyzer side left lowering with
     * no file scope, and every core intrinsic that needs a complete source
     * location was refused. */
    cfg.source_file = entry->source_path ? entry->source_path : "<cgen-test>";
    cfg.run_emit = false;
    cfg.module_graph = graph;
    cfg.graph_modules = graph_modules;
    cfg.graph_module_count = graph->topo_count;
    XiPipelineResult result = xi_pipeline_compile_program(entry->ast, analyzer, g_iso, &cfg);
    if (result.status != XI_PIPE_OK) {
        fprintf(stderr, "  PIPELINE FAILED: %s%s%s\n", xi_pipe_status_str(result.status),
                result.error.detail[0] ? ": " : "", result.error.detail);
        xi_pipeline_result_free(&result);
        goto cleanup;
    }
    ir = result.ir;
    result.ir = NULL;
    xi_pipeline_result_free(&result);

cleanup:
    xr_free(graph_modules);
    if (analyzer) {
        xa_analyzer_set_graph(analyzer, NULL);
        xa_analyzer_free(analyzer);
    }
    if (graph)
        xr_module_graph_free(graph);
    return ir;
}

static XiFunc *compile_to_ir_with_module_graph(const char *source) {
    XiPipelineConfig cfg = xi_pipeline_default_config();
    cfg.run_optimize = false;
    return compile_to_ir_with_module_graph_config(source, cfg);
}

TEST(target_plan_scalar_ref_c_emission_from_source) {
    const char *source = "fn set(value: ref i64) { value = 42 }\n"
                         "fn run() -> i64 {\n"
                         "    var value = 0\n"
                         "    set(ref value)\n"
                         "    return value\n"
                         "}\n"
                         "print(run())\n";
    XiFunc *ir = compile_to_ir(source);
    TEST_REQUIRE(ir && ir->semantic_plan, "source scalar-ref fixture froze SemanticPlan authority");

    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    XrTargetPlan *target = NULL;
    XrCEmissionPlan *emission = NULL;
    char error[512] = {0};
    TEST_REQUIRE(
        profile && xr_target_plan_build(ir->semantic_plan, profile, &target, error, sizeof(error)),
        "source scalar-ref fixture built TargetPlan authority");

    uint32_t argument_count = 0;
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(target, &argument_count);
    TEST_REQUIRE(arguments && argument_count == 1,
                 "source scalar-ref fixture has one frozen call argument");
    const XrTargetCallArgumentRecord *argument = &arguments[0];
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target, &call_count);
    TEST_REQUIRE(calls && argument->call < call_count,
                 "source scalar-ref argument names its frozen call");

    TEST_REQUIRE(xr_c_emission_plan_build(target, xr_target_profile_fingerprint(profile), &emission,
                                          error, sizeof(error)),
                 "source scalar-ref fixture built C-emission authority");
    XrCCallArgumentEmissionView view = {0};
    TEST_REQUIRE(
        xr_c_emission_plan_call_argument_view(emission, calls[argument->call].result_value,
                                              argument->ordinal, &view, error, sizeof(error)) &&
            view.semantic_operand == argument->semantic_operand &&
            view.semantic_value == argument->semantic_value &&
            view.mode == XR_TARGET_CALL_REFERENCE && view.ownership == XR_TARGET_CALL_BORROW &&
            view.transfer_mode == XR_TRANSFER_SHARE &&
            view.flags == XR_TARGET_CALL_ARGUMENT_ADDRESSABLE &&
            view.caller_register_kind == XR_MACHINE_REP_I64 &&
            view.caller_memory_kind == XR_MACHINE_REP_I64 &&
            view.callee_register_kind == XR_MACHINE_REP_I64 &&
            view.callee_memory_kind == XR_MACHINE_REP_I64 &&
            view.array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE && view.c_type &&
            strcmp(view.c_type, "int64_t *") == 0,
        "source scalar-ref fixture projects one exact immutable C row");

    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(ir);
}

static void require_detached_semantic_snapshot(const XiFunc *func) {
    TEST_REQUIRE(func != NULL, "detached semantic snapshot function exists");
    TEST_REQUIRE(func->semantic_snapshot_detached,
                 "escaped Xi function carries a detached semantic snapshot");
    TEST_REQUIRE(func->analyzer == NULL, "escaped Xi function has no analyzer back-pointer");
    for (uint16_t i = 0; i < func->nchildren; i++)
        require_detached_semantic_snapshot(func->children[i]);
}

TEST(aot_semantic_snapshot_survives_analyzer_pool_churn) {
    const char *src = "enum SnapshotValue { Text(value: string) }\n"
                      "fn echo(v: SnapshotValue) -> SnapshotValue { return v }\n"
                      "var value = echo(SnapshotValue.Text(\"ok\"))\n"
                      "print(value)\n";
    XiPipelineConfig cfg = xi_pipeline_aot_config();
    XiFunc *ir = compile_to_ir_with_module_graph_config(src, cfg);
    TEST_REQUIRE(ir != NULL, "semantic snapshot fixture compiled to AOT IR");
    require_detached_semantic_snapshot(ir);

    /* The compiling analyzer and its type arena were destroyed by the helper.
     * Repeatedly allocate and destroy a new analyzer pool so stale semantic
     * pointers are likely to be overwritten before AOT planning consumes IR. */
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(g_iso);
    XaAnalyzer *churn = xa_analyzer_new(session);
    TEST_REQUIRE(churn != NULL, "type-pool churn analyzer created");
    for (int i = 0; i < 512; i++) {
        XrType *element = xr_type_new_array(g_iso, xr_type_new_string(NULL));
        XrType *params[] = {element};
        TEST_REQUIRE(element != NULL && xr_type_new_function(g_iso, params, 1,
                                                             xr_type_new_bool(NULL), false) != NULL,
                     "type-pool churn allocation succeeded");
    }
    xa_analyzer_free(churn);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "snapshot", &had_error);
    TEST_REQUIRE(code != NULL && !had_error,
                 "AOT planning and C generation use only the detached snapshot");
    TEST_REQUIRE(strstr(code, "xrt_enum_aggregate_box(XRT_ENUM_AGGREGATE_MAKE(") != NULL,
                 "payload enum constructor consumes the immutable C recipe");
    TEST_REQUIRE(strstr(code, "\"SnapshotValue\", \"Text\",") != NULL,
                 "payload enum recipe freezes the nominal type and member");

    xr_free(code);
    xi_func_free(ir);
}

/* CGen fixtures must explicitly produce a verified Backend program. CGen is
 * deliberately incapable of repairing an earlier-stage input. */
static bool test_prepare_backend_ir(XiFunc *ir) {
    if (!ir)
        return false;
    if (ir->stage == XI_STAGE_BACKEND)
        return true;

    XiModule fixture_module = {
        .identity = "memory-module-v1:id=18:xi-cgen-fixture-v1",
        .path = "xi-cgen-fixture.xr",
        .name = "xi_cgen_fixture",
        .init = ir,
    };
    XiModule *saved_module = ir->module;
    if (!saved_module)
        ir->module = &fixture_module;

    char error[512] = {0};
    XiOptimizedProgram *optimized = NULL;
    if (ir->stage == XI_STAGE_RAW) {
        XiRawProgram *raw = xi_stage_adopt_raw(ir, error, sizeof(error));
        if (!raw)
            goto fail;
        XiCanonicalProgram *canonical = xi_program_canonicalize(raw, error, sizeof(error));
        if (!canonical)
            goto fail;
        xi_pass_close(ir);
        XiClosedProgram *closed = xi_program_close(canonical, error, sizeof(error));
        if (!closed)
            goto fail;
        XiOwnedProgram *owned = xi_program_make_owned(closed, error, sizeof(error));
        if (!owned)
            goto fail;
        XiSemanticLoweredProgram *semantic_lowered =
            xi_program_lower_semantics(owned, error, sizeof(error));
        if (!semantic_lowered)
            goto fail;
        XiCoroLoweredProgram *coro_lowered =
            xi_program_lower_coroutines(semantic_lowered, NULL, error, sizeof(error));
        if (!coro_lowered)
            goto fail;
        optimized = xi_program_finish_optimization(coro_lowered, error, sizeof(error));
    } else if (ir->stage == XI_STAGE_OPTIMIZED) {
        optimized = xi_stage_adopt_optimized(ir, error, sizeof(error));
    }

    XiSemanticPlannedProgram *semantic = NULL;
    if (optimized) {
        if (!xr_semantic_plan_build_and_attach(ir, error, sizeof(error)))
            goto fail;
        semantic = xi_program_freeze_semantics(optimized, error, sizeof(error));
    } else if (ir->stage == XI_STAGE_SEMANTIC_PLANNED) {
        semantic = xi_stage_adopt_semantic_planned(ir, error, sizeof(error));
    }

    XiReppedProgram *repped = NULL;
    if (semantic) {
        XiRepPolicy policy = xi_rep_policy_native_boundary();
        xi_opt_select_rep_with_policy(ir, &policy);
        xi_opt_box_elim(ir);
        repped = xi_program_select_reps(semantic, error, sizeof(error));
    } else if (ir->stage == XI_STAGE_REPPED) {
        repped = xi_stage_adopt_repped(ir, error, sizeof(error));
    }
    if (!repped)
        goto fail;

    xi_backend_lower(ir);
    /* Mirror the production AOT pipeline: unit ERR_CHECK cleanup operands are
     * published only after representation and backend lowering are final. */
    xi_arc_attach_error_cleanups(ir);
    XiBackendProgram *backend = xi_program_plan_backend(repped, error, sizeof(error));
    if (!backend)
        goto fail;
    bool released = xi_backend_program_release(backend) == ir;
    ir->module = saved_module;
    return released;

fail:
    ir->module = saved_module;
    fprintf(stderr, "  fixture Backend transition failed for '%s': %s\n",
            ir->name ? ir->name : "<anonymous>", error);
    return false;
}

static char *test_failed_codegen_result(bool *had_error) {
    if (had_error)
        *had_error = true;
    char *empty = (char *) xr_malloc(1);
    TEST_REQUIRE(empty != NULL, "failed-codegen result allocation failed");
    empty[0] = '\0';
    return empty;
}

/* Generate C code for Xi IR into an xr_malloc-owned string.
 * Caller releases the returned string with xr_free(). */
static char *generate_c_with_status_and_stats_for_artifact(XiFunc *ir, const char *module_name,
                                                           bool *had_error,
                                                           XiCgenCoroFrameStats *coro_stats,
                                                           XaotArtifactKind artifact_kind) {
    assert(ir != NULL);

    if (!test_prepare_backend_ir(ir)) {
        if (coro_stats)
            memset(coro_stats, 0, sizeof(*coro_stats));
        if (had_error)
            return test_failed_codegen_result(had_error);
        TEST_REQUIRE(false, "fixture Backend preparation failed");
    }

    /* Build module metadata if the pipeline didn't (e.g. standalone tests) */
    XiModule *mod = ir->module;
    bool own_mod = false;
    if (!mod) {
        mod = xi_module_new("test.xr", module_name, ir);
        assert(mod != NULL);
        assert(xi_module_set_identity(mod, "memory-module-v1:id=18:xi-cgen-fixture-v1"));
        own_mod = true;
    } else {
        if (!mod->name)
            mod->name = module_name;
    }
    XiModule *modules[] = {mod};
    TestAotPlan plan;
    test_aot_plan_prepare(&plan, modules, 1, 0);

    XiCgenCtx *ctx = xi_cgen_ctx_new();
    assert(ctx != NULL);
    xi_cgen_ctx_set_aot_bundle(ctx, &plan.bundle);
    TestCEmissionRegistry emission_registry;
    TEST_REQUIRE(test_c_emission_registry_install(&emission_registry, ctx, &plan.bundle),
                 "C emission registry fixture installation failed");
    xi_cgen_ctx_set_artifact_kind(ctx, artifact_kind);

    char *buf = NULL;
    size_t bufsz = 0;
    FILE *mem = xr_open_memstream(&buf, &bufsz);
    assert(mem != NULL);

    xi_cgen_program(ctx, mem, mod);
    int rc = xr_close_memstream(mem, &buf, &bufsz);
    assert(rc == 0);
    if (had_error)
        *had_error = xi_cgen_has_error(ctx);
    if (coro_stats)
        *coro_stats = xi_cgen_coro_frame_stats(ctx);

    xi_cgen_ctx_free(ctx);
    test_c_emission_registry_free(&emission_registry);
    test_aot_plan_free(&plan);
    if (own_mod) {
        mod->init = NULL; /* don't double-free ir */
        xi_module_free(mod);
    }

    return buf;
}

static char *generate_c_with_status_and_stats(XiFunc *ir, const char *module_name, bool *had_error,
                                              XiCgenCoroFrameStats *coro_stats) {
    return generate_c_with_status_and_stats_for_artifact(ir, module_name, had_error, coro_stats,
                                                         XAOT_ARTIFACT_EXECUTABLE);
}

static char *generate_c_with_status_and_cgen_stats(XiFunc *ir, const char *module_name,
                                                   bool *had_error, XiCgenStats *cgen_stats) {
    assert(ir != NULL);

    if (!test_prepare_backend_ir(ir)) {
        if (cgen_stats)
            memset(cgen_stats, 0, sizeof(*cgen_stats));
        if (had_error)
            return test_failed_codegen_result(had_error);
        TEST_REQUIRE(false, "fixture Backend preparation failed");
    }

    XiModule *mod = ir->module;
    bool own_mod = false;
    if (!mod) {
        mod = xi_module_new("test.xr", module_name, ir);
        assert(mod != NULL);
        assert(xi_module_set_identity(mod, "memory-module-v1:id=18:xi-cgen-fixture-v1"));
        own_mod = true;
    } else {
        if (!mod->name)
            mod->name = module_name;
    }
    XiModule *modules[] = {mod};
    TestAotPlan plan;
    test_aot_plan_prepare(&plan, modules, 1, 0);

    XiCgenCtx *ctx = xi_cgen_ctx_new();
    assert(ctx != NULL);
    xi_cgen_ctx_set_aot_bundle(ctx, &plan.bundle);
    TestCEmissionRegistry emission_registry;
    TEST_REQUIRE(test_c_emission_registry_install(&emission_registry, ctx, &plan.bundle),
                 "C emission registry fixture installation failed");

    char *buf = NULL;
    size_t bufsz = 0;
    FILE *mem = xr_open_memstream(&buf, &bufsz);
    assert(mem != NULL);

    xi_cgen_program(ctx, mem, mod);
    int rc = xr_close_memstream(mem, &buf, &bufsz);
    assert(rc == 0);
    if (had_error)
        *had_error = xi_cgen_has_error(ctx);
    if (cgen_stats)
        *cgen_stats = xi_cgen_stats(ctx);

    xi_cgen_ctx_free(ctx);
    test_c_emission_registry_free(&emission_registry);
    test_aot_plan_free(&plan);
    if (own_mod) {
        mod->init = NULL; /* don't double-free ir */
        xi_module_free(mod);
    }

    return buf;
}

static char *generate_c_with_status(XiFunc *ir, const char *module_name, bool *had_error) {
    return generate_c_with_status_and_stats(ir, module_name, had_error, NULL);
}

static char *generate_c(XiFunc *ir, const char *module_name) {
    return generate_c_with_status(ir, module_name, NULL);
}

/* Check that `haystack` contains `needle`. */
static bool contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

static XiFunc *test_find_child_function(XiFunc *root, const char *name) {
    if (!root || !name)
        return NULL;
    if (root->name && strcmp(root->name, name) == 0)
        return root;
    for (uint16_t i = 0; i < root->nchildren; i++) {
        XiFunc *found = test_find_child_function(root->children[i], name);
        if (found)
            return found;
    }
    return NULL;
}

TEST(cgen_extern_symbol_binding_is_portable_and_verified) {
    const char *source = "extern \"C\" { fn abs(x: i32) -> i32 }\n"
                         "print(unsafe { abs(-7) })\n";
    XiFunc *ir = compile_to_ir(source);
    TEST_REQUIRE(ir && test_prepare_backend_ir(ir),
                 "portable extern binding fixture reached Backend");
    XiModule *module = ir->module;
    bool own_module = false;
    if (!module) {
        module = xi_module_new("extern_binding.xr", "extern_binding", ir);
        TEST_REQUIRE(module != NULL, "portable extern binding module allocated");
        own_module = true;
    }
    XiModule *modules[] = {module};
    TestAotPlan plan;
    test_aot_plan_prepare(&plan, modules, 1, 0);
    TEST_REQUIRE(plan.bundle.nextern_decls == 1 &&
                     plan.bundle.extern_decls[0].c_binding == XAOT_EXTERN_C_BINDING_PORTABLE_DIRECT,
                 "valid native identifier selects one portable direct binding");

    XiCgenCtx *ctx = xi_cgen_ctx_new();
    TEST_REQUIRE(ctx != NULL && xi_cgen_ctx_set_aot_bundle(ctx, &plan.bundle),
                 "portable extern binding installed in CGen");
    TestCEmissionRegistry emission_registry;
    TEST_REQUIRE(test_c_emission_registry_install(&emission_registry, ctx, &plan.bundle),
                 "portable extern C emission registry installed");
    char *code = NULL;
    size_t code_size = 0;
    FILE *mem = xr_open_memstream(&code, &code_size);
    TEST_REQUIRE(mem != NULL, "portable extern C output stream opened");
    xi_cgen_program(ctx, mem, module);
    TEST_REQUIRE(xr_close_memstream(mem, &code, &code_size) == 0,
                 "portable extern C output stream closed");
    TEST_REQUIRE(!xi_cgen_has_error(ctx) && code &&
                     contains(code, "extern int32_t abs(int32_t);") && contains(code, "abs(") &&
                     !contains(code, ") __asm__(XR_FFI_ASMNAME(\"abs\"))") &&
                     !contains(code, "xr_ffi_1("),
                 "MSVC ABI emits and calls the portable native symbol without GNU asm labels");

    char verify_error[256] = {0};
    plan.bundle.extern_decls[0].c_binding = XAOT_EXTERN_C_BINDING_GNU_ASM_LABEL;
    TEST_REQUIRE(!xaot_verify_bundle(&plan.bundle, verify_error, sizeof(verify_error)) &&
                     strstr(verify_error, "MSVC extern symbol") != NULL,
                 "independent verifier rejects a mutated extern C binding answer");

    xr_free(code);
    test_c_emission_registry_free(&emission_registry);
    xi_cgen_ctx_free(ctx);
    test_aot_plan_free(&plan);
    if (own_module) {
        module->init = NULL;
        xi_module_free(module);
    }
    xi_func_free(ir);
}

TEST(aot_extern_symbol_rename_requires_typed_qualification) {
    const char *source = "extern \"C\" { fn abs(x: i32) -> i32 }\n"
                         "print(unsafe { abs(-7) })\n";
    XiFunc *ir = compile_to_ir(source);
    TEST_REQUIRE(ir && test_prepare_backend_ir(ir),
                 "extern rename authority fixture reached Backend");
    XiFunc *foreign = test_find_child_function(ir, "abs");
    TEST_REQUIRE(foreign && foreign->is_extern && !foreign->extern_symbol_qualified,
                 "source-only extern has no typed native-symbol qualification");
    foreign->extern_symbol = "qualified_abs_name";

    XiModule *module = ir->module;
    bool own_module = false;
    if (!module) {
        module = xi_module_new("extern_rename.xr", "extern_rename", ir);
        TEST_REQUIRE(module != NULL, "extern rename authority module allocated");
        own_module = true;
    }
    XiModule *modules[] = {module};
    TestAotPlan plan;
    bool prepared = test_aot_plan_try_prepare(&plan, modules, 1, 0);
    TEST_REQUIRE(!prepared && plan.bundle.error_msg &&
                     strstr(plan.bundle.error_msg, "lacks typed provider qualification") != NULL,
                 "unqualified source/native rename fails before C emission");

    test_aot_plan_free(&plan);
    if (own_module) {
        module->init = NULL;
        xi_module_free(module);
    }
    xi_func_free(ir);
}

static size_t count_between(const char *start, const char *end, const char *needle) {
    size_t count = 0;
    size_t needle_len = strlen(needle);
    const char *p = start;
    assert(start != NULL);
    assert(end != NULL);
    assert(needle != NULL && needle_len > 0);
    while (p < end) {
        const char *hit = strstr(p, needle);
        if (!hit || hit >= end)
            break;
        count++;
        p = hit + needle_len;
    }
    return count;
}

static bool contains_between(const char *start, const char *end, const char *needle) {
    return count_between(start, end, needle) > 0;
}

static size_t count_lines_outside_debug_locals_with_prefix(const char *code, const char *end,
                                                           const char *prefix, const char *needle) {
    size_t count = 0;
    unsigned debug_depth = 0;
    const char *line = code;
    assert(code != NULL);
    assert(end != NULL && end >= code);
    assert(prefix != NULL);
    assert(needle != NULL && needle[0] != '\0');

    while (line < end && *line != '\0') {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t) (end - line) : strlen(line);
        if (len == strlen("#if defined(XRAY_AOT_DEBUG_LOCALS)") &&
            strncmp(line, "#if defined(XRAY_AOT_DEBUG_LOCALS)", len) == 0) {
            debug_depth++;
        } else if (len == strlen("#endif") && strncmp(line, "#endif", len) == 0) {
            if (debug_depth > 0)
                debug_depth--;
        } else if (debug_depth == 0) {
            const char *hit = strstr(line, needle);
            size_t prefix_len = strlen(prefix);
            if (hit && (size_t) (hit - line) < len && len >= prefix_len &&
                strncmp(line, prefix, prefix_len) == 0)
                count++;
        }
        if (!end)
            break;
        line = end + 1;
    }
    return count;
}

static size_t count_lines_outside_debug_locals(const char *code, const char *end,
                                               const char *needle) {
    return count_lines_outside_debug_locals_with_prefix(code, end, "    xrt_struct_abi_", needle);
}

static size_t count_op_in_func(const XiFunc *func, XiOp op) {
    size_t count = 0;
    if (!func)
        return 0;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *value = block->values[vi];
            if (value && value->op == op)
                count++;
        }
    }
    for (uint16_t ci = 0; ci < func->nchildren; ci++)
        count += count_op_in_func(func->children[ci], op);
    return count;
}

static size_t count_intrinsic_in_func(const XiFunc *func, XaIntrinsicId intrinsic_id) {
    size_t count = 0;
    if (!func)
        return 0;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        for (uint32_t vi = 0; block && vi < block->nvalues; vi++) {
            const XiValue *value = block->values[vi];
            if (value && value->xa_intrinsic_id == (uint32_t) intrinsic_id)
                count++;
        }
    }
    for (uint16_t ci = 0; ci < func->nchildren; ci++)
        count += count_intrinsic_in_func(func->children[ci], intrinsic_id);
    return count;
}

static XiValue *find_unique_intrinsic_in_func(XiFunc *func, XaIntrinsicId intrinsic_id) {
    XiValue *match = NULL;
    if (!func)
        return NULL;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        XiBlock *block = func->blocks[bi];
        for (uint32_t vi = 0; block && vi < block->nvalues; vi++) {
            XiValue *value = block->values[vi];
            if (!value || value->xa_intrinsic_id != (uint32_t) intrinsic_id)
                continue;
            if (match)
                return NULL;
            match = value;
        }
    }
    for (uint16_t ci = 0; ci < func->nchildren; ci++) {
        XiValue *child_match = find_unique_intrinsic_in_func(func->children[ci], intrinsic_id);
        if (!child_match)
            continue;
        if (match)
            return NULL;
        match = child_match;
    }
    return match;
}

static const char *next_static_after(const char *fn) {
    assert(fn != NULL);
    const char *next = strstr(fn + 1, "\nstatic ");
    assert(next != NULL && "generated function should be followed by another static declaration");
    return next;
}

static const char *find_static_function_definition(const char *code, const char *prefix) {
    assert(code != NULL);
    assert(prefix != NULL);
    size_t prefix_len = strlen(prefix);
    const char *p = code;
    while ((p = strstr(p, prefix)) != NULL) {
        const char *brace = strchr(p, '{');
        const char *semi = strchr(p, ';');
        if (brace && (!semi || brace < semi))
            return p;
        p += prefix_len;
    }
    return NULL;
}

static bool nonzero_state_precedes_call(const char *code, const char *call) {
    const char *call_pos = strstr(code, call);
    const char *last_state = NULL;
    const char *p = code;
    assert(code != NULL);
    assert(call != NULL);
    if (!call_pos)
        return false;

    while ((p = strstr(p, "f->state = ")) != NULL && p < call_pos) {
        last_state = p;
        p++;
    }
    if (!last_state)
        return false;

    const char *value = strstr(last_state, "= ");
    return value && value[2] != '0';
}

/* ========== Tests ========== */

TEST(cgen_restricted_c90_header_is_explicit_and_minimal) {
    XiCgenCtx *ctx = xi_cgen_ctx_new();
    char *code = NULL;
    size_t code_size = 0;
    FILE *out = xr_open_memstream(&code, &code_size);
    TEST_REQUIRE(ctx != NULL && out != NULL, "C90 header fixture allocated");

    xi_cgen_ctx_set_freestanding_profile(ctx, true);
    xi_cgen_ctx_set_c_dialect(ctx, XI_CGEN_C_DIALECT_C90);
    xi_cgen_header(ctx, out);
    TEST_REQUIRE(xr_close_memstream(out, &code, &code_size) == 0, "C90 header fixture closed");
    TEST_REQUIRE(contains(code, "#include \"xrt_c90.h\""),
                 "restricted C90 selects the minimal kernel header");
    TEST_REQUIRE(!contains(code, "#include \"xrt_core_freestanding.h\"") &&
                     !contains(code, "#include \"xrt.h\"") && !contains(code, "xrt_builtins"),
                 "restricted C90 excludes the ordinary runtime and builtin table");
    TEST_REQUIRE(!contains(code, "-Wdeclaration-after-statement"),
                 "restricted C90 does not suppress declaration-order diagnostics");

    xr_free(code);
    xi_cgen_ctx_free(ctx);
}

TEST(cgen_simple_arith) {
    /* Pure arithmetic: 1 + 2 printed */
    const char *src = "print(1 + 2)";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    char *code = generate_c(ir, "test");
    assert(code != NULL && "C code generation failed");

    assert(contains(code, "printf(\"%lld\", (long long)") &&
           "native i64 print should use direct printf");
    assert(!contains(code, "xrt_println(") && !contains(code, "xrt_print(") &&
           "native i64 print should not call the generic tagged printer");
    assert(!contains(code, "XR_FROM_INT(v") &&
           "print-only i64 boxes should be elided from generated C");
    /* The native process entry point follows the host C ABI. Source-language
     * exact scalar names must never leak into this generated signature. */
    assert(contains(code, "int main(int argc, char **argv)") && "should have main()");
    /* Should include xrt.h */
    assert(contains(code, "#include \"xrt.h\"") && "should include xrt.h");
    const char *linux_feature_test = strstr(code, "#define _GNU_SOURCE 1");
    const char *first_system_header = strstr(code, "#include <math.h>");
    TEST_REQUIRE(linux_feature_test && first_system_header &&
                     linux_feature_test < first_system_header,
                 "hosted Linux feature-test macro precedes every system header");
    assert(contains(code, "#if defined(__cplusplus)\nextern \"C\" {\n#endif") &&
           contains(code, "#if defined(__cplusplus)\n}\n#endif") &&
           "generated definitions must retain C linkage under a C++ compiler");
    assert(contains(code, "#if defined(__GNUC__) && !defined(__clang__)") &&
           contains(code, "#pragma GCC diagnostic ignored \"-Wattributes\"") &&
           contains(code, "#pragma GCC diagnostic ignored \"-Wunused-variable\"") &&
           contains(code, "#pragma GCC diagnostic ignored \"-Wunused-but-set-variable\"") &&
           contains(code, "#pragma GCC diagnostic ignored \"-Wunused-label\"") &&
           contains(code, "#pragma GCC diagnostic ignored \"-Wsign-compare\"") &&
           contains(code, "#pragma GCC diagnostic ignored \"-Wsign-conversion\"") &&
           contains(code, "#pragma GCC diagnostic ignored \"-Wcast-align\"") &&
           contains(code, "#pragma GCC diagnostic ignored \"-Wcast-qual\"") &&
           contains(code, "#pragma GCC diagnostic ignored \"-Wconversion\"") &&
           contains(code, "#pragma GCC diagnostic ignored \"-Wdeclaration-after-statement\"") &&
           contains(code, "#pragma GCC diagnostic ignored \"-Wfloat-equal\"") &&
           contains(code, "#pragma GCC diagnostic ignored \"-Wredundant-decls\"") &&
           contains(code, "#pragma GCC diagnostic ignored \"-Wstrict-aliasing\"") &&
           contains(code, "#pragma GCC diagnostic ignored \"-Wswitch-enum\"") &&
           "generated translation units should isolate GCC-only warning suppressions");
    assert(contains(code, "#if defined(__clang__)") &&
           contains(code, "#pragma clang diagnostic ignored \"-Wunused-variable\"") &&
           contains(code, "#pragma clang diagnostic ignored \"-Wunused-but-set-variable\"") &&
           contains(code, "#pragma clang diagnostic ignored \"-Wunused-label\"") &&
           contains(code, "#pragma clang diagnostic ignored \"-Wsign-compare\"") &&
           contains(code, "#pragma clang diagnostic ignored \"-Wcast-align\"") &&
           contains(code, "#pragma clang diagnostic ignored \"-Wcast-qual\"") &&
           contains(code, "#pragma clang diagnostic ignored \"-Wmissing-braces\"") &&
           contains(code, "#pragma clang diagnostic ignored \"-Wdeclaration-after-statement\"") &&
           contains(code, "#pragma clang diagnostic ignored \"-Wfloat-equal\"") &&
           contains(code, "#pragma clang diagnostic ignored \"-Wswitch-enum\"") &&
           contains(code, "#pragma clang diagnostic ignored \"-Wimplicit-int-conversion\"") &&
           contains(code, "#pragma clang diagnostic ignored \"-Wshorten-64-to-32\"") &&
           "generated translation units should isolate Clang-only warning suppressions");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_target_layout_queries_emit_source_backed_constants) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 995, .scalar_rep = XR_NATIVE_I64, .frozen = true};
    XiFunc *ir = xi_func_new("target_layout_constants", &int_type);
    TEST_REQUIRE(ir != NULL, "target-layout CGen function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "target-layout CGen entry allocated");
    entry->sealed = true;

    XiValue *size = xi_value_new(ir, entry, XI_TARGET_SIZEOF, &int_type, 0);
    XiValue *align = xi_value_new(ir, entry, XI_TARGET_ALIGNOF, &int_type, 0);
    TEST_REQUIRE(size != NULL && align != NULL, "target-layout CGen queries allocated");
    size->aux_int = XR_NATIVE_USIZE;
    align->aux_int = XR_NATIVE_ISIZE;
    XiValue *sum = xi_value_new(ir, entry, XI_ADD, &int_type, 2);
    TEST_REQUIRE(sum != NULL, "target-layout CGen sum allocated");
    sum->args[0] = size;
    sum->args[1] = align;
    xi_block_set_return(entry, sum);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "target_layout", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "target-layout CGen succeeds");
    const char *first_constant = strstr(code, "INT64_C(8)");
    TEST_REQUIRE(first_constant && strstr(first_constant + 1, "INT64_C(8)"),
                 "CGen consumes the LP64 bundle layout for size and alignment");
    TEST_REQUIRE(!contains(code, "sizeof(size_t)") && !contains(code, "_Alignof(size_t)") &&
                     !contains(code, "sizeof(ptrdiff_t)") && !contains(code, "_Alignof(ptrdiff_t)"),
                 "CGen does not ask the host compiler to rediscover target layout");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_rep_identical_source_alias_shares_immutable_c_local) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 919, .scalar_rep = XR_NATIVE_I64, .frozen = true};
    XiFunc *ir = xi_func_new("manual_c_alias", &int_type);
    TEST_REQUIRE(ir != NULL, "manual C-alias function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual C-alias entry block allocated");
    entry->sealed = true;

    ir->nparams = 1;
    ir->min_params = 1;
    ir->params = (XiValue **) xr_calloc(1, sizeof(XiValue *));
    TEST_REQUIRE(ir->params != NULL, "manual C-alias parameter table allocated");
    XiValue *source = xi_param(ir, entry, 0, &int_type);
    TEST_REQUIRE(source != NULL, "manual C-alias source allocated");
    ir->params[0] = source;
    ir->source_var_count = 2;
    ir->source_var_names =
        (const char **) xi_func_arena_alloc(ir, 2 * sizeof(*ir->source_var_names));
    ir->source_var_types = (XrType **) xi_func_arena_alloc(ir, 2 * sizeof(*ir->source_var_types));
    TEST_REQUIRE(ir->source_var_names && ir->source_var_types,
                 "manual C-alias source-variable tables allocated");
    ir->source_var_names[0] = "source";
    ir->source_var_names[1] = "alias";
    ir->source_var_types[0] = &int_type;
    ir->source_var_types[1] = &int_type;
    source->var_id = 0;
    XiValue *alias = xi_value_new(ir, entry, XI_COPY, &int_type, 1);
    TEST_REQUIRE(alias != NULL, "manual C-alias boundary allocated");
    alias->args[0] = source;
    alias->aux_int = XI_COPY_KIND_IDENTITY;
    alias->var_id = 1; /* keep this source-variable domain through Xi copy-prop */
    XiValue *sum = xi_value_new(ir, entry, XI_ADD, &int_type, 2);
    TEST_REQUIRE(sum != NULL, "manual C-alias consumer allocated");
    sum->args[0] = alias;
    sum->args[1] = source;
    xi_block_set_return(entry, sum);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "manual C-alias C generation failed");
    TEST_REQUIRE(!had_error, "representation-identical C alias should generate");
    const char *fn = find_static_function_definition(code, "manual_c_alias");
    TEST_REQUIRE(fn != NULL, "manual C-alias function definition emitted");
    const char *fn_end = strstr(fn, "\n}\n");
    TEST_REQUIRE(fn_end != NULL, "manual C-alias function end emitted");
    TEST_REQUIRE(!contains_between(fn, fn_end, "int64_t v1 = v0;"),
                 "release C must not materialize a representation-identical source alias");
    TEST_REQUIRE(!contains_between(fn, fn_end, "v1"),
                 "ordinary C consumers must not reference the elided alias local");
    TEST_REQUIRE(contains_between(fn, fn_end, "int64_t v2 ="),
                 "the alias consumer should remain as an ordinary native computation");

    printf("  Generated representation-identical C alias coalescing %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_rep_identical_unbox_shares_immutable_c_local) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 920, .scalar_rep = XR_NATIVE_I64, .frozen = true};
    XiFunc *ir = xi_func_new("manual_c_unbox_alias", &int_type);
    TEST_REQUIRE(ir != NULL, "manual C-unbox function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual C-unbox entry block allocated");
    entry->sealed = true;

    ir->nparams = 1;
    ir->min_params = 1;
    ir->params = (XiValue **) xr_calloc(1, sizeof(XiValue *));
    TEST_REQUIRE(ir->params != NULL, "manual C-unbox parameter table allocated");
    XiValue *source = xi_param(ir, entry, 0, &int_type);
    TEST_REQUIRE(source != NULL, "manual C-unbox source allocated");
    ir->params[0] = source;
    XiValue *alias = xi_value_new(ir, entry, XI_UNBOX, &int_type, 1);
    TEST_REQUIRE(alias != NULL, "manual C-unbox boundary allocated");
    alias->args[0] = source;
    XiValue *sum = xi_value_new(ir, entry, XI_ADD, &int_type, 2);
    TEST_REQUIRE(sum != NULL, "manual C-unbox consumer allocated");
    sum->args[0] = alias;
    sum->args[1] = source;
    xi_block_set_return(entry, sum);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "manual C-unbox C generation failed");
    TEST_REQUIRE(!had_error, "representation-identical C unbox should generate");
    const char *fn = find_static_function_definition(code, "manual_c_unbox_alias");
    TEST_REQUIRE(fn != NULL, "manual C-unbox function definition emitted");
    const char *fn_end = strstr(fn, "\n}\n");
    TEST_REQUIRE(fn_end != NULL, "manual C-unbox function end emitted");
    TEST_REQUIRE(!contains_between(fn, fn_end, "int64_t v1 = v0;"),
                 "release C must not materialize a representation-identical unbox");
    TEST_REQUIRE(!contains_between(fn, fn_end, "v1"),
                 "ordinary C consumers must not reference the elided unbox local");
    TEST_REQUIRE(contains_between(fn, fn_end, "int64_t v2 ="),
                 "the unbox consumer should remain as an ordinary native computation");

    printf("  Generated representation-identical C unbox coalescing %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_fixed_array_alias_address_projection_shares_backing_c_local) {
    const char *src = "fn fixedArrayAlias() -> u32 {\n"
                      "    var lanes: [u32; 4] = [0; 4]\n"
                      "    lanes[0] = 7\n"
                      "    return lanes[0]\n"
                      "}\n"
                      "print(fixedArrayAlias())\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "fixed-array alias source compiled to Xi");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "fixed-array alias C generation failed");
    TEST_REQUIRE(!had_error, "fixed-array alias address projection should generate");
    const char *fn = find_static_function_definition(code, "fixedArrayAlias");
    TEST_REQUIRE(fn != NULL, "fixed-array alias function definition emitted");
    const char *fn_end = strstr(fn, "\n}\n");
    TEST_REQUIRE(fn_end != NULL, "fixed-array alias function end emitted");
    TEST_REQUIRE(contains_between(fn, fn_end, "_fa"),
                 "fixed-array local should keep native lane storage");
    TEST_REQUIRE(count_between(fn, fn_end, "XrValue v") == 1,
                 "fixed-array LOCAL_ADDR must not force a redundant tagged alias local");
    TEST_REQUIRE(
        contains_between(fn, fn_end,
                         "#if defined(XRAY_AOT_DEBUG_LOCALS)\n    XrValue v3 = xr_array_ref"),
        "the remaining fixed-array wrapper must be debug-only");

    printf("  Generated fixed-array alias projection in %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_scalar_alias_materializes_when_c_address_is_taken) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 928, .scalar_rep = XR_NATIVE_I64, .frozen = true};
    XrType *parameter_types[] = {&int_type};
    XrType *function_type = xr_type_new_function(g_iso, parameter_types, 1, &int_type, false);
    TEST_REQUIRE(function_type != NULL &&
                     xr_type_function_set_param_mode(function_type, 0, XR_PARAM_REF),
                 "addressed C-alias ref function type allocated");
    XiFunc *ir = xi_func_new("manual_c_addressed_alias", &int_type);
    TEST_REQUIRE(ir != NULL, "addressed C-alias function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "addressed C-alias entry block allocated");
    entry->sealed = true;

    ir->nparams = 1;
    ir->min_params = 1;
    ir->params = (XiValue **) xr_calloc(1, sizeof(XiValue *));
    TEST_REQUIRE(ir->params != NULL, "addressed C-alias parameter table allocated");
    XiValue *source = xi_param(ir, entry, 0, &int_type);
    TEST_REQUIRE(source != NULL, "addressed C-alias source allocated");
    ir->params[0] = source;
    ir->source_var_count = 2;
    ir->source_var_names =
        (const char **) xi_func_arena_alloc(ir, 2 * sizeof(*ir->source_var_names));
    ir->source_var_types = (XrType **) xi_func_arena_alloc(ir, 2 * sizeof(*ir->source_var_types));
    TEST_REQUIRE(ir->source_var_names && ir->source_var_types,
                 "addressed C-alias source-variable tables allocated");
    ir->source_var_names[0] = "source";
    ir->source_var_names[1] = "alias";
    ir->source_var_types[0] = &int_type;
    ir->source_var_types[1] = &int_type;
    source->var_id = 0;
    XiValue *alias = xi_value_new(ir, entry, XI_UNBOX, &int_type, 1);
    TEST_REQUIRE(alias != NULL, "addressed C-alias boundary allocated");
    alias->args[0] = source;
    alias->var_id = 1;
    XiValue *place = xi_value_new(ir, entry, XI_LOCAL_ADDR, &int_type, 1);
    TEST_REQUIRE(place != NULL, "addressed C-alias place allocated");
    place->args[0] = alias;
    /* The opcode is shared by four operations, and storage is answered per
     * operation rather than per opcode: the cleanup capture below, a raw
     * dereference, a direct projection, and the plain address a `ref` argument
     * takes, whose storage the families that know about ref parameters answer.
     * Without the cleanup bit this place is none of them, so no family claims
     * it and the scalar family binds the address as its pointee's integer. */
    place->aux_int |= XI_LOCAL_ADDR_AUX_CLEANUP_LIVE;
    XiValue *load = xi_value_new(ir, entry, XI_PLACE_LOAD, &int_type, 1);
    TEST_REQUIRE(load != NULL, "addressed C-alias load allocated");
    load->args[0] = place;
    xi_block_set_return(entry, load);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "addressed C-alias C generation failed");
    TEST_REQUIRE(!had_error, "addressed scalar C alias should generate");
    const char *fn = find_static_function_definition(code, "manual_c_addressed_alias");
    TEST_REQUIRE(fn != NULL, "addressed C-alias function definition emitted");
    const char *fn_end = strstr(fn, "\n}\n");
    TEST_REQUIRE(fn_end != NULL, "addressed C-alias function end emitted");
    TEST_REQUIRE(contains_between(fn, fn_end, "int64_t v1 = v0;"),
                 "a scalar alias whose C address is taken must remain materialized");
    TEST_REQUIRE(contains_between(fn, fn_end, "(void *)(&v1)"),
                 "the scalar place must address the materialized alias local");

    printf("  Kept addressed scalar alias in %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_forward_use_predeclarations_have_no_dead_initializers) {
    /* Use clean narrow arithmetic here: unlike native i64 wrap arithmetic,
     * its emitter intentionally references the SSA locals.  That keeps this
     * fixture focused on forward declarations rather than literal folding. */
    XrType int_type = {.kind = XR_KIND_INT, .id = 932, .scalar_rep = XR_NATIVE_U32, .frozen = true};
    XiFunc *ir = xi_func_new("manual_forward_predecl", &int_type);
    TEST_REQUIRE(ir != NULL, "manual forward-predecl function allocated");
    XiBlock *entry = xi_block_new(ir);
    XiBlock *use = xi_block_new(ir);
    XiBlock *def = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL && use != NULL && def != NULL,
                 "manual forward-predecl blocks allocated");

    XiValue *value = xi_const_int(ir, def, 41, &int_type);
    XiValue *one = xi_const_int(ir, def, 1, &int_type);
    XiValue *sum = xi_value_new(ir, use, XI_ADD, &int_type, 2);
    TEST_REQUIRE(value != NULL && one != NULL && sum != NULL,
                 "manual forward-predecl values allocated");
    sum->args[0] = value;
    sum->args[1] = one;
    xi_block_set_jump(entry, def);
    xi_block_set_jump(def, use);
    xi_block_set_return(use, sum);
    entry->sealed = true;
    use->sealed = true;
    def->sealed = true;

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "manual forward-predecl C generation failed");
    TEST_REQUIRE(!had_error, "manual forward-predecl fixture should generate");
    const char *fn = find_static_function_definition(code, "manual_forward_predecl");
    TEST_REQUIRE(fn != NULL, "manual forward-predecl definition emitted");
    const char *fn_end = strstr(fn, "\n}\n");
    TEST_REQUIRE(fn_end != NULL, "manual forward-predecl function end emitted");
    TEST_REQUIRE(contains_between(fn, fn_end, "uint32_t v0;") &&
                     contains_between(fn, fn_end, "uint32_t v1;"),
                 "forward definitions must be declared before their earlier C use block");
    TEST_REQUIRE(!contains_between(fn, fn_end, "uint32_t v0 = 0;") &&
                     !contains_between(fn, fn_end, "uint32_t v1 = 0;"),
                 "ordinary SSA dominance must not create dead defensive initializers");
    TEST_REQUIRE(contains_between(fn, fn_end, "v0 = INT64_C(41);") &&
                     contains_between(fn, fn_end, "v1 = INT64_C(1);"),
                 "the real definition blocks must still assign both forward values");

    printf("  Generated initializer-free forward predeclarations in %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_trivial_span_value_clone_shares_immutable_c_local) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 921, .scalar_rep = XR_NATIVE_I64, .frozen = true};
    /* The aggregate kind must dominate incidental zero-valued scalar metadata
     * when an uncalled function states its own C boundary. */
    XrType span_type = {
        .kind = XR_KIND_SLICE,
        .id = 922,
        .scalar_rep = XR_NATIVE_I64,
        .frozen = true,
    };
    span_type.container.element_type = &int_type;
    XiFunc *ir = xi_func_new("manual_c_span_clone", &span_type);
    TEST_REQUIRE(ir != NULL, "manual C-span-clone function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual C-span-clone entry block allocated");
    entry->sealed = true;

    ir->nparams = 1;
    ir->min_params = 1;
    ir->params = (XiValue **) xr_calloc(1, sizeof(XiValue *));
    TEST_REQUIRE(ir->params != NULL, "manual C-span-clone parameter table allocated");
    XiValue *source = xi_param(ir, entry, 0, &span_type);
    TEST_REQUIRE(source != NULL, "manual C-span-clone source allocated");
    ir->params[0] = source;
    set_single_param_ownership_contract(ir, XI_OWN_BORROWED, XI_RETURN_OWNERSHIP_BORROWED_PARAM, 0);
    ir->view_return_source = XR_VIEW_RETURN_PARAM;
    ir->view_return_param = 0;
    ir->view_return_complete = true;
    XiValue *clone = xi_value_new(ir, entry, XI_COPY, &span_type, 1);
    TEST_REQUIRE(clone != NULL, "manual C-span-clone boundary allocated");
    clone->args[0] = source;
    clone->aux_int = XI_COPY_KIND_VALUE_CLONE;
    xi_block_set_return(entry, clone);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "manual C-span-clone C generation failed");
    TEST_REQUIRE(!had_error, "trivial span clone should generate");
    const char *fn = find_static_function_definition(code, "manual_c_span_clone");
    TEST_REQUIRE(fn != NULL, "manual C-span-clone function definition emitted");
    const char *fn_end = strstr(fn, "\n}\n");
    TEST_REQUIRE(fn_end != NULL, "manual C-span-clone function end emitted");
    TEST_REQUIRE(!contains_between(fn, fn_end, "xr_span_t v1 = v0;"),
                 "release C must not materialize a trivial span value clone");
    TEST_REQUIRE(!contains_between(fn, fn_end, "v1"),
                 "ordinary C consumers must not reference the elided span clone local");

    printf("  Generated trivial span clone coalescing %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_rep_identical_span_box_shares_immutable_c_local) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 924, .scalar_rep = XR_NATIVE_I64, .frozen = true};
    XrType span_type = {.kind = XR_KIND_SLICE, .id = 925, .frozen = true};
    span_type.container.element_type = &int_type;
    XiFunc *ir = xi_func_new("manual_c_span_box", &span_type);
    TEST_REQUIRE(ir != NULL, "manual C-span-box function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual C-span-box entry block allocated");
    entry->sealed = true;

    ir->nparams = 1;
    ir->min_params = 1;
    ir->params = (XiValue **) xr_calloc(1, sizeof(XiValue *));
    TEST_REQUIRE(ir->params != NULL, "manual C-span-box parameter table allocated");
    XiValue *source = xi_param(ir, entry, 0, &span_type);
    TEST_REQUIRE(source != NULL, "manual C-span-box source allocated");
    ir->params[0] = source;
    set_single_param_ownership_contract(ir, XI_OWN_BORROWED, XI_RETURN_OWNERSHIP_BORROWED_PARAM, 0);
    ir->view_return_source = XR_VIEW_RETURN_PARAM;
    ir->view_return_param = 0;
    ir->view_return_complete = true;
    XiValue *box = xi_value_new(ir, entry, XI_BOX, &span_type, 1);
    TEST_REQUIRE(box != NULL, "manual C-span-box boundary allocated");
    box->args[0] = source;
    xi_block_set_return(entry, box);

    TEST_REQUIRE(test_prepare_backend_ir(ir), "manual C-span-box backend prepared");
    XiValue *adapter = entry->control;
    TEST_REQUIRE(adapter && adapter != box && adapter->op == XI_UNBOX && adapter->nargs == 1 &&
                     adapter->args[0] == box &&
                     adapter->backend_origin == XI_BACKEND_VALUE_REP_UNBOX,
                 "span return retains the exact backend adapter over its frozen BOX");

    XiModule *mutation_module = xi_module_new("span_box_mutation.xr", "test", ir);
    TEST_REQUIRE(mutation_module != NULL, "span-box mutation module allocated");
    XiModule *mutation_modules[] = {mutation_module};
    TestAotPlan rejected_plan;
    adapter->backend_origin = XI_BACKEND_VALUE_NONE;
    TEST_REQUIRE(!test_aot_plan_try_prepare(&rejected_plan, mutation_modules, 1, 0),
                 "missing adapter provenance must fail closed");
    TEST_REQUIRE(rejected_plan.bundle.error_msg &&
                     strstr(rejected_plan.bundle.error_msg, "exact TargetPlan semantic identity"),
                 "mutation rejection must name the missing exact identity");
    test_aot_plan_free(&rejected_plan);
    adapter->backend_origin = XI_BACKEND_VALUE_REP_UNBOX;
    mutation_module->init = NULL;
    xi_module_free(mutation_module);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "manual C-span-box C generation failed");
    TEST_REQUIRE(!had_error, "representation-identical span box should generate");
    const char *fn = find_static_function_definition(code, "manual_c_span_box");
    TEST_REQUIRE(fn != NULL, "manual C-span-box function definition emitted");
    const char *fn_end = strstr(fn, "\n}\n");
    TEST_REQUIRE(fn_end != NULL, "manual C-span-box function end emitted");
    TEST_REQUIRE(!contains_between(fn, fn_end, "xr_span_t v1 = v0;"),
                 "release C must not materialize a representation-identical span box");
    TEST_REQUIRE(!contains_between(fn, fn_end, "v1"),
                 "ordinary C consumers must not reference the elided span box local");
    TEST_REQUIRE(!contains_between(fn, fn_end, "v2"),
                 "the exact backend unbox must share the original span local");

    printf("  Generated representation-identical span box coalescing %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_scalar_value_clone_remains_distinct_c_local) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 923, .scalar_rep = XR_NATIVE_I64, .frozen = true};
    XiFunc *ir = xi_func_new("manual_c_scalar_clone", &int_type);
    TEST_REQUIRE(ir != NULL, "manual C-scalar-clone function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual C-scalar-clone entry block allocated");
    entry->sealed = true;

    ir->nparams = 1;
    ir->min_params = 1;
    ir->params = (XiValue **) xr_calloc(1, sizeof(XiValue *));
    TEST_REQUIRE(ir->params != NULL, "manual C-scalar-clone parameter table allocated");
    XiValue *source = xi_param(ir, entry, 0, &int_type);
    TEST_REQUIRE(source != NULL, "manual C-scalar-clone source allocated");
    ir->params[0] = source;
    XiValue *clone = xi_value_new(ir, entry, XI_COPY, &int_type, 1);
    TEST_REQUIRE(clone != NULL, "manual C-scalar-clone boundary allocated");
    clone->args[0] = source;
    clone->aux_int = XI_COPY_KIND_VALUE_CLONE;
    xi_block_set_return(entry, clone);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "manual C-scalar-clone C generation failed");
    TEST_REQUIRE(!had_error, "nontrivial scalar clone should generate");
    const char *fn = find_static_function_definition(code, "manual_c_scalar_clone");
    TEST_REQUIRE(fn != NULL, "manual C-scalar-clone function definition emitted");
    const char *fn_end = strstr(fn, "\n}\n");
    TEST_REQUIRE(fn_end != NULL, "manual C-scalar-clone function end emitted");
    TEST_REQUIRE(contains_between(fn, fn_end, "xrt_value_clone_for_coro("),
                 "nontrivial scalar value clones must remain distinct C locals");

    printf("  Kept nontrivial scalar clone in %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_immediate_scalar_constant_keeps_debug_sync_without_release_local) {
    XrType u64_type = {.kind = XR_KIND_INT, .id = 929, .scalar_rep = XR_NATIVE_U64, .frozen = true};
    XiFunc *ir = xi_func_new("manual_debug_const", &u64_type);
    TEST_REQUIRE(ir != NULL, "manual debug-constant function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual debug-constant entry block allocated");
    entry->sealed = true;

    ir->nparams = 1;
    ir->min_params = 1;
    ir->params = (XiValue **) xr_calloc(1, sizeof(XiValue *));
    TEST_REQUIRE(ir->params != NULL, "manual debug-constant parameter table allocated");
    XiValue *source = xi_param(ir, entry, 0, &u64_type);
    TEST_REQUIRE(source != NULL, "manual debug-constant source allocated");
    ir->params[0] = source;
    XiValue *literal = xi_const_int(ir, entry, 255, &u64_type);
    TEST_REQUIRE(literal != NULL, "manual debug-constant literal allocated");
    literal->var_id = 0;
    ir->source_var_count = 1;
    ir->source_var_names = (const char **) xi_func_arena_alloc(ir, sizeof(*ir->source_var_names));
    TEST_REQUIRE(ir->source_var_names != NULL, "manual debug-constant name table allocated");
    ir->source_var_names[0] = "mask";
    XiValue *masked_value = xi_value_new(ir, entry, XI_BXOR, &u64_type, 2);
    TEST_REQUIRE(masked_value != NULL, "manual debug-constant consumer allocated");
    masked_value->args[0] = source;
    masked_value->args[1] = literal;
    xi_block_set_return(entry, masked_value);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "immediate-constant C generation failed");
    TEST_REQUIRE(!had_error, "immediate-constant fixture should generate");
    const char *masked = find_static_function_definition(code, "manual_debug_const");
    TEST_REQUIRE(masked != NULL, "manual debug-constant definition should be emitted");
    const char *masked_end = strstr(masked, "\n}\n");
    TEST_REQUIRE(masked_end != NULL, "manual debug-constant function end emitted");
    TEST_REQUIRE(contains_between(masked, masked_end, "uint64_t mask = 0;"),
                 "debug-local builds must declare the source-level constant slot");
    TEST_REQUIRE(contains_between(masked, masked_end, "mask = (uint64_t)INT64_C(255);"),
                 "debug-local builds must synchronize the source-level constant slot");
    TEST_REQUIRE(contains_between(masked, masked_end, "INT64_C(255)"),
                 "release expression must retain the exact constant literal");
    TEST_REQUIRE(count_between(masked, masked_end, " = INT64_C(255);") == 0,
                 "release C must not materialize a constant used only by literal-aware ops");

    printf("  Generated debug-synchronized immediate constant %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_returned_scalar_constant_emits_immediate_without_local) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 936, .scalar_rep = XR_NATIVE_I64, .frozen = true};
    XiFunc *ir = xi_func_new("manual_return_scalar", &int_type);
    TEST_REQUIRE(ir != NULL, "manual returned-scalar function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual returned-scalar entry block allocated");
    entry->sealed = true;

    XiValue *literal = xi_const_int(ir, entry, 42, &int_type);
    TEST_REQUIRE(literal != NULL, "manual returned-scalar literal allocated");
    xi_block_set_return(entry, literal);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "returned-scalar C generation failed");
    TEST_REQUIRE(!had_error, "returned-scalar fixture should generate");
    const char *fn = find_static_function_definition(code, "manual_return_scalar");
    TEST_REQUIRE(fn != NULL, "manual returned-scalar definition should be emitted");
    const char *fn_end = strstr(fn, "\n}\n");
    TEST_REQUIRE(fn_end != NULL, "manual returned-scalar function end emitted");
    TEST_REQUIRE(!contains_between(fn, fn_end, "int64_t v0 = INT64_C(42);"),
                 "a returned scalar literal must not leave a dead C local");
    TEST_REQUIRE(contains_between(fn, fn_end, "return ") &&
                     contains_between(fn, fn_end, "INT64_C(42)"),
                 "the returned scalar literal must remain exact at the return site");

    printf("  Generated immediate returned scalar %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_returned_null_constant_emits_immediate_without_local) {
    XrType null_type = {
        .kind = XR_KIND_NULL, .scalar_rep = XR_SCALAR_REP_NONE, .id = 935, .frozen = true};
    XiFunc *ir = xi_func_new("manual_return_null", &null_type);
    TEST_REQUIRE(ir != NULL, "manual returned-null function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual returned-null entry block allocated");
    entry->sealed = true;

    XiValue *literal = xi_const_null(ir, entry, &null_type);
    TEST_REQUIRE(literal != NULL, "manual returned-null literal allocated");
    xi_block_set_return(entry, literal);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "returned-null C generation failed");
    TEST_REQUIRE(!had_error, "returned-null fixture should generate");
    const char *fn = find_static_function_definition(code, "manual_return_null");
    TEST_REQUIRE(fn != NULL, "manual returned-null definition should be emitted");
    const char *fn_end = strstr(fn, "\n}\n");
    TEST_REQUIRE(fn_end != NULL, "manual returned-null function end emitted");
    TEST_REQUIRE(!contains_between(fn, fn_end, "XrValue v0 = XR_NULL_VAL;"),
                 "a returned null literal must not leave a dead C local");
    TEST_REQUIRE(contains_between(fn, fn_end, "return XR_NULL_VAL;"),
                 "the returned null literal must remain exact at the return site");

    printf("  Generated immediate returned null %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_multi_concat_string_constants_emit_immediate_without_locals) {
    XrType string_type = {
        .kind = XR_KIND_STRING, .id = 937, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XiFunc *ir = xi_func_new("manual_concat_literals", &string_type);
    TEST_REQUIRE(ir != NULL, "manual concat-literal function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual concat-literal entry block allocated");
    entry->sealed = true;

    XiValue *left = xi_const_str(ir, entry, "left", &string_type);
    XiValue *right = xi_const_str(ir, entry, "right", &string_type);
    TEST_REQUIRE(left != NULL && right != NULL, "manual concat literals allocated");
    XiValue *concat = xi_value_new(ir, entry, XI_STR_CONCAT, &string_type, 2);
    TEST_REQUIRE(concat != NULL, "manual multi-part concat allocated");
    concat->args[0] = left;
    concat->args[1] = right;
    xi_block_set_return(entry, concat);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "multi-part concat C generation failed");
    TEST_REQUIRE(!had_error, "multi-part concat fixture should generate");
    const char *fn = find_static_function_definition(code, "manual_concat_literals");
    TEST_REQUIRE(fn != NULL, "manual concat-literal definition should be emitted");
    const char *fn_end = strstr(fn, "\n}\n");
    TEST_REQUIRE(fn_end != NULL, "manual concat-literal function end emitted");
    TEST_REQUIRE(!contains_between(fn, fn_end, "XrValue v0 = xr_str_lit(") &&
                     !contains_between(fn, fn_end, "XrValue v1 = xr_str_lit("),
                 "multi-part concat literals must not leave dead C locals");
    TEST_REQUIRE(count_between(fn, fn_end, "xr_str_lit(") >= 2,
                 "multi-part concat must retain both exact string literals at its use site");
    TEST_REQUIRE(contains_between(fn, fn_end, "xrt_str_concat_parts(2,"),
                 "multi-part concat must retain the single-allocation helper");

    printf("  Generated immediate multi-part concat literals %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_shared_string_constant_emits_immediate_without_local) {
    XrType null_type = {
        .kind = XR_KIND_NULL, .scalar_rep = XR_SCALAR_REP_NONE, .id = 938, .frozen = true};
    XrType string_type = {
        .kind = XR_KIND_STRING, .id = 939, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType unit_type = {
        .kind = XR_KIND_UNIT, .scalar_rep = XR_SCALAR_REP_NONE, .id = 940, .frozen = true};
    XiFunc *ir = xi_func_new("manual_shared_literal", &null_type);
    TEST_REQUIRE(ir != NULL, "manual shared-literal function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual shared-literal entry block allocated");
    entry->sealed = true;
    ir->nshared = 1;

    XiValue *literal = xi_const_str(ir, entry, "shared", &string_type);
    TEST_REQUIRE(literal != NULL, "manual shared literal allocated");
    XiValue *store = xi_value_new(ir, entry, XI_SET_SHARED, &unit_type, 1);
    TEST_REQUIRE(store != NULL, "manual shared-literal store allocated");
    store->args[0] = literal;
    store->aux_int = 0;
    XiValue *result = xi_const_null(ir, entry, &null_type);
    TEST_REQUIRE(result != NULL, "manual shared-literal return allocated");
    xi_block_set_return(entry, result);

    XiModule *mod = xi_module_new("test.xr", "test", ir);
    TEST_REQUIRE(mod != NULL, "manual shared-literal module allocated");
    TEST_REQUIRE(
        xi_module_set_identity(mod, "memory-module-v1:id=32:manual-shared-literal-fixture-v1"),
        "manual shared-literal module identity published");
    mod->nslots = 1;
    ir->module = mod;

    TEST_REQUIRE(test_prepare_backend_ir(ir), "shared-literal fixture reached Backend");
    const char *saved_literal = (const char *) literal->aux;
    literal->aux = "forged-live-shared";
    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    literal->aux = (void *) saved_literal;
    TEST_REQUIRE(code != NULL, "shared-literal C generation failed");
    TEST_REQUIRE(!had_error, "shared-literal fixture should generate");
    const char *fn = find_static_function_definition(code, "manual_shared_literal");
    TEST_REQUIRE(fn != NULL, "manual shared-literal definition should be emitted");
    const char *fn_end = strstr(fn, "\n}\n");
    TEST_REQUIRE(fn_end != NULL, "manual shared-literal function end emitted");
    TEST_REQUIRE(!contains_between(fn, fn_end, "XrValue v0 = xr_str_lit("),
                 "a shared string literal must not leave a dead C local");
    TEST_REQUIRE(contains_between(fn, fn_end, "xrt_array_ref_ensure_owned(xr_str_lit("),
                 "the portable shared-slot ownership handoff must retain the exact string literal");
    TEST_REQUIRE(strstr(code, "\"shared\"") != NULL && strstr(code, "forged-live-shared") == NULL,
                 "shared-slot emission must use the immutable literal recipe");

    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    XrTargetPlan *target_plan = NULL;
    char identity_error[512] = {0};
    TEST_REQUIRE(profile != NULL && xr_target_plan_build(ir->semantic_plan, profile, &target_plan,
                                                         identity_error, sizeof(identity_error)),
                 "shared-literal mutation TargetPlan built");
    uint8_t saved_scalar_rep = string_type.scalar_rep;
    string_type.scalar_rep = XR_NATIVE_I8;
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    TEST_REQUIRE(!xr_aot_scalar_semantic_value_id(target_plan, ir, literal, &semantic_function,
                                                  &semantic_value, identity_error,
                                                  sizeof(identity_error)) &&
                     strncmp(identity_error, "XR_TARGET_1001", strlen("XR_TARGET_1001")) == 0,
                 "live String scalar identity drift must fail closed");
    string_type.scalar_rep = saved_scalar_rep;

    printf("  Generated immediate shared string literal %zu bytes of C code\n", strlen(code));
    xr_target_plan_free(target_plan);
    xr_target_profile_free(profile);
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_unused_call_result_emits_effect_statement_without_local) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 156, .frozen = true};
    XrType func_type = {.kind = XR_KIND_FUNCTION, .id = 157, .frozen = true};
    func_type.function.return_type = &int_type;
    func_type.function.param_count = 0;
    func_type.function.min_params = 0;

    XiFunc *ir = xi_func_new("unused_call_result", &int_type);
    assert(ir != NULL);
    XiBlock *entry = xi_block_new(ir);
    assert(entry != NULL);
    entry->sealed = true;

    XiFunc *child = xi_func_new("effect", &int_type);
    assert(child != NULL);
    child->parent_func = ir;
    XiBlock *child_entry = xi_block_new(child);
    assert(child_entry != NULL);
    child_entry->sealed = true;
    XiValue *child_ret = xi_const_int(child, child_entry, 7, &int_type);
    assert(child_ret != NULL);
    xi_block_set_return(child_entry, child_ret);

    ir->children = (XiFunc **) xr_calloc(1, sizeof(XiFunc *));
    assert(ir->children != NULL);
    ir->children[0] = child;
    ir->children_cap = 1;
    ir->nchildren = 1;

    XiValue *callee = xi_value_new(ir, entry, XI_CLOSURE_NEW, &func_type, 0);
    assert(callee != NULL);
    callee->aux = (void *) child;
    XiValue *unused = xi_value_new(ir, entry, XI_CALL, &int_type, 1);
    TEST_REQUIRE(unused != NULL, "unused call allocated");
    unused->args[0] = callee;
    /* Model the conservative/stale use metadata that can remain after DCE. */
    unused->uses = 1;
    unused->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_CALL_EFFECTS;
    XiValue *release = xi_value_new(ir, entry, XI_RELEASE, &func_type, 1);
    TEST_REQUIRE(release != NULL, "callee release allocated");
    release->args[0] = callee;
    release->flags = XI_FLAG_SIDE_EFFECT;
    XiValue *ret = xi_const_int(ir, entry, 1, &int_type);
    TEST_REQUIRE(ret != NULL, "unused call return allocated");
    xi_block_set_return(entry, ret);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "unused call result should generate");

    char dead_decl[64];
    snprintf(dead_decl, sizeof(dead_decl), "int64_t v%u =", (unsigned) unused->id);
    TEST_REQUIRE(!contains(code, dead_decl), "unused call result must not create a C local");
    TEST_REQUIRE(contains(code, "    (void)("),
                 "unused call must remain as an effect-preserving expression statement");

    printf("  Generated unused call-result statement %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_unused_array_reserve_result_emits_effect_statement_without_local) {
    const char *src = "@noinline\n"
                      "fn grow(flag: bool) -> i64 {\n"
                      "    var bytes: Array<u8> = [1, 2, 3]\n"
                      "    if (flag) { bytes.reserve(len(bytes) + 4) }\n"
                      "    return len(bytes)\n"
                      "}\n"
                      "print(grow(true))\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "unused array reserve fixture should compile");
    XiValue *reserve = find_unique_intrinsic_in_func(ir, XA_INTRINSIC_ARRAY_RESERVE);
    TEST_REQUIRE(
        reserve && reserve->op == XI_CALL_BUILTIN && reserve->nargs == 2 && reserve->args &&
            reserve->args[0] && reserve->args[1] && reserve->aux == NULL && reserve->aux_int == 0 &&
            reserve->aux_kind == XI_AUX_KIND_NONE && reserve->result_alias_operand == 0,
        "Array.reserve lowering must preserve stable intrinsic identity without a selector");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "unused array reserve fixture should generate");
    const char *fn = find_static_function_definition(code, "test_grow_");
    TEST_REQUIRE(fn != NULL, "array reserve function should emit");
    const char *fn_end = next_static_after(fn);
    TEST_REQUIRE(!contains_between(fn, fn_end, " = xrt_array_reserve_trusted_raw("),
                 "unused array reserve result must not materialize a C local");
    TEST_REQUIRE(contains_between(fn, fn_end, "(void)(xrt_array_reserve_trusted_raw("),
                 "array reserve side effect must remain emitted");
    TEST_REQUIRE(strstr(code, "({") == NULL,
                 "array reserve effect emission must remain portable C11");

    printf("  Generated unused array reserve statement %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_dead_native_box_without_source_storage_is_elided) {
    XrType u64_type = {.kind = XR_KIND_INT, .id = 162, .scalar_rep = XR_NATIVE_U64, .frozen = true};
    XiFunc *ir = xi_func_new("dead_native_box", &u64_type);
    TEST_REQUIRE(ir != NULL, "dead native box function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "dead native box entry allocated");
    entry->sealed = true;

    XiValue *inner = xi_const_int(ir, entry, 7, &u64_type);
    TEST_REQUIRE(inner != NULL, "dead native box input allocated");
    XiValue *box = xi_value_new(ir, entry, XI_BOX, &u64_type, 1);
    TEST_REQUIRE(box != NULL, "dead native box allocated");
    box->args[0] = inner;
    box->uses = 1;
    XiValue *ret = xi_const_int(ir, entry, 1, &u64_type);
    TEST_REQUIRE(ret != NULL, "dead native box return allocated");
    xi_block_set_return(entry, ret);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "dead native box fixture should generate");
    const char *fn = find_static_function_definition(code, "dead_native_box");
    TEST_REQUIRE(fn != NULL, "dead native box function should emit");
    const char *fn_end = strstr(fn, "\n}\n");
    TEST_REQUIRE(fn_end != NULL, "dead native box function end emitted");
    char dead_decl[64];
    snprintf(dead_decl, sizeof(dead_decl), "v%u = XR_FROM_INT(", (unsigned) box->id);
    TEST_REQUIRE(!contains_between(fn, fn_end, dead_decl),
                 "dead native box must not materialize a C local");

    printf("  Generated dead native box elision %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_native_unsigned_interpolation_consumes_inner_without_box_local) {
    const char *src = "fn label(value: u64) -> string { return \"value=${value}\" }\n"
                      "print(label(7))\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "native unsigned interpolation fixture should compile");
    TEST_REQUIRE(test_prepare_backend_ir(ir),
                 "native unsigned interpolation Backend plan should freeze");
    char semantic_hex[XR_FINGERPRINT_BYTES * 2u + 1u];
    xr_fingerprint_hex(xr_semantic_plan_fingerprint(ir->semantic_plan), semantic_hex);
    /* Re-anchored because a SemanticPlan fingerprint covers the whole stdlib
     * metadata registry: xr_semantic_plan.c hashes plan->stdlib_registry_fingerprint,
     * which xr_stdlib_metadata_registry_fingerprint derives from every .def entry.
     * Publishing http2, compress, mem and regex from .xr bodies renames their
     * entries, so this digest moves even though the fixture below imports
     * nothing.  Old: 9a99849f192ca8108c6ba9502a8dcc43f03f6d93251e03551d19f1df2155a02b. */
    TEST_REQUIRE(strcmp(semantic_hex,
                        "fedfc7c88a77cdbee8134c13448dc6ad5fd571af159636eecf22df7a48eba1b4") == 0,
                 "native unsigned interpolation preserves the frozen SemanticPlan KAT");

    XiFunc *label = NULL;
    for (uint16_t i = 0; i < ir->nchildren; i++) {
        if (ir->children[i] && ir->children[i]->name &&
            strcmp(ir->children[i]->name, "label") == 0) {
            TEST_REQUIRE(label == NULL, "native unsigned interpolation label is unique");
            label = ir->children[i];
        }
    }
    TEST_REQUIRE(label && label->nparams == 1 && label->params[0],
                 "native unsigned interpolation label authority is complete");
    XiValue *concat = NULL;
    for (uint32_t b = 0; b < label->nblocks; b++) {
        XiBlock *block = label->blocks[b];
        for (uint32_t v = 0; block && v < block->nvalues; v++) {
            XiValue *candidate = block->values[v];
            if (candidate && candidate->op == XI_STR_CONCAT) {
                TEST_REQUIRE(concat == NULL, "native unsigned interpolation concat is unique");
                concat = candidate;
            }
        }
    }
    TEST_REQUIRE(concat && concat->nargs == 2 && concat->args[0] && concat->args[1],
                 "native unsigned interpolation concat shape is exact");

    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    XrTargetPlan *target = NULL;
    char error[512] = {0};
    TEST_REQUIRE(
        profile && ir->semantic_plan &&
            xr_target_plan_build(ir->semantic_plan, profile, &target, error, sizeof(error)),
        "native unsigned interpolation TargetPlan should build");
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    XrCEmissionPlan *same_emission = NULL;
    TEST_REQUIRE(
        xr_c_emission_plan_build(target, profile_fingerprint, &emission, error, sizeof(error)),
        "native unsigned interpolation CEmission plan should build");
    TEST_REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &same_emission, error,
                                          sizeof(error)) &&
                     xr_fingerprint_equal(xr_c_emission_plan_fingerprint(emission),
                                          xr_c_emission_plan_fingerprint(same_emission)),
                 "native unsigned interpolation CEmission fingerprint is deterministic");

    const XrSemanticOperationRecord *semantic_concat = NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(ir->semantic_plan); i++) {
        const XrSemanticOperationRecord *candidate =
            xr_semantic_plan_operation(ir->semantic_plan, i);
        if (candidate && candidate->opcode == XI_STR_CONCAT) {
            TEST_REQUIRE(semantic_concat == NULL, "native unsigned semantic concat is unique");
            semantic_concat = candidate;
        }
    }
    uint32_t semantic_operand_count = 0;
    const XrSemanticOperandRecord *semantic_operands =
        xr_semantic_plan_operands(ir->semantic_plan, &semantic_operand_count);
    TEST_REQUIRE(semantic_concat && semantic_concat->operand_count == 2 && semantic_operands &&
                     semantic_concat->operand_begin <= semantic_operand_count &&
                     semantic_concat->operand_count <=
                         semantic_operand_count - semantic_concat->operand_begin,
                 "native unsigned semantic concat shape is exact");
    TEST_REQUIRE(xr_semantic_string_concat_is_exact(ir->semantic_plan, semantic_concat),
                 "native unsigned semantic concat authority is exact");
    uint32_t concat_value = semantic_concat->result_value;
    uint32_t literal_value = semantic_operands[semantic_concat->operand_begin].value;
    uint32_t logical_value = semantic_operands[semantic_concat->operand_begin + 1u].value;
    XrCValueEmissionView view = {0};
    bool view_found =
        xr_c_emission_plan_value_view(emission, concat_value, &view, error, sizeof(error));
    TEST_REQUIRE(view_found && view.materialization == XR_C_VALUE_MATERIALIZATION_STRING_CONCAT &&
                     view.recipe_argument_count == 2 && view.recipe_arguments &&
                     view.recipe_arguments[0].kind == XR_C_RECIPE_ARGUMENT_STRING_VALUE &&
                     view.recipe_arguments[0].semantic_value == literal_value &&
                     view.recipe_arguments[0].source_semantic_value == literal_value &&
                     view.recipe_arguments[1].kind == XR_C_RECIPE_ARGUMENT_STRING_DIRECT_U64 &&
                     view.recipe_arguments[1].semantic_value == logical_value &&
                     view.recipe_arguments[1].source_semantic_value == logical_value,
                 "native unsigned interpolation CEmission recipe is exact");

    XrCRecipeArgumentView *direct = (XrCRecipeArgumentView *) &view.recipe_arguments[1];
    uint8_t saved_kind = direct->kind;
    direct->kind = XR_C_RECIPE_ARGUMENT_STRING_VALUE;
    TEST_REQUIRE(
        !xr_c_emission_plan_verify(emission, target, profile_fingerprint, error, sizeof(error)),
        "direct u64 recipe kind mutation fails closed");
    direct->kind = saved_kind;
    uint32_t saved_logical = direct->semantic_value;
    direct->semantic_value = literal_value;
    TEST_REQUIRE(
        !xr_c_emission_plan_verify(emission, target, profile_fingerprint, error, sizeof(error)),
        "direct u64 logical identity mutation fails closed");
    direct->semantic_value = saved_logical;
    uint32_t saved_source = direct->source_semantic_value;
    direct->source_semantic_value = literal_value;
    TEST_REQUIRE(
        !xr_c_emission_plan_verify(emission, target, profile_fingerprint, error, sizeof(error)),
        "direct u64 source identity mutation fails closed");
    direct->source_semantic_value = saved_source;
    XrTargetValueRepRecord *source_binding =
        (XrTargetValueRepRecord *) xr_target_plan_value_rep(target, logical_value);
    const XrTargetValueRepRecord *literal_binding = xr_target_plan_value_rep(target, literal_value);
    TEST_REQUIRE(source_binding && literal_binding, "direct u64 Target bindings are complete");
    uint16_t saved_register_rep = source_binding->register_rep;
    uint16_t saved_memory_rep = source_binding->memory_rep;
    source_binding->register_rep = literal_binding->register_rep;
    source_binding->memory_rep = literal_binding->memory_rep;
    TEST_REQUIRE(
        !xr_c_emission_plan_verify(emission, target, profile_fingerprint, error, sizeof(error)),
        "direct u64 Target representation mutation fails closed");
    source_binding->register_rep = saved_register_rep;
    source_binding->memory_rep = saved_memory_rep;
    TEST_REQUIRE(
        xr_c_emission_plan_verify(emission, target, profile_fingerprint, error, sizeof(error)),
        "restored direct u64 CEmission recipe verifies");
    xr_c_emission_plan_free(same_emission);
    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error,
                 "native unsigned interpolation fixture should generate");
    const char *fn = find_static_function_definition(code, "test_label_");
    TEST_REQUIRE(fn != NULL, "native unsigned interpolation function should emit");
    const char *fn_end = next_static_after(fn);
    TEST_REQUIRE(!contains_between(fn, fn_end, " = XR_FROM_INT("),
                 "native unsigned interpolation must not materialize a box local");
    TEST_REQUIRE(contains_between(fn, fn_end, "xrt_strpart_init_u64("),
                 "native unsigned interpolation must use the direct u64 string part");
    TEST_REQUIRE(!contains_between(fn, fn_end, "(abort()") && strstr(code, "({") == NULL,
                 "native unsigned interpolation must remain portable generated C11");

    printf("  Generated native unsigned interpolation without box %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_panicinfo_constructor_token_emits_no_local) {
    const char *src = "fn requireValue(value: i64) -> i64 {\n"
                      "    return match (value) { 1 -> value }\n"
                      "}\n"
                      "print(requireValue(1))\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "PanicInfo token fixture should compile");
    TEST_REQUIRE(test_prepare_backend_ir(ir), "PanicInfo token fixture reached Backend");
    uint32_t exact_constructor_count = 0;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(ir->semantic_plan); i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(ir->semantic_plan, i);
        if (xr_semantic_panic_info_constructor_with_receiver_is_exact(ir->semantic_plan, operation,
                                                                      NULL))
            exact_constructor_count++;
    }
    TEST_REQUIRE(exact_constructor_count == 1,
                 "PanicInfo fixture preserves its frozen reserved-receiver call identity");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "PanicInfo token fixture should generate");
    const char *fn = find_static_function_definition(code, "test_requireValue_");
    TEST_REQUIRE(fn != NULL, "PanicInfo token function should emit");
    const char *fn_end = next_static_after(fn);
    TEST_REQUIRE(!contains_between(fn, fn_end, "builtin native class token: PanicInfo"),
                 "PanicInfo constructor receiver must not materialize a C local");
    TEST_REQUIRE(contains_between(fn, fn_end, "xrt_exception_from_message_value("),
                 "PanicInfo constructor effect must remain emitted");

    printf("  Generated PanicInfo constructor token elision %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_direct_stdlib_import_call_emits_no_function_token_local) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 160, .scalar_rep = XR_NATIVE_I64, .frozen = true};
    XrType func_type = {.kind = XR_KIND_FUNCTION, .id = 161, .frozen = true};
    func_type.function.params = NULL;
    func_type.function.param_count = 0;
    func_type.function.min_params = 0;
    func_type.function.return_type = &int_type;
    XiFunc *ir = xi_func_new("direct_stdlib_import", &int_type);
    TEST_REQUIRE(ir != NULL, "direct stdlib import function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "direct stdlib import entry allocated");
    entry->sealed = true;

    XiImportRef *ref = (XiImportRef *) xi_func_arena_alloc(ir, sizeof(XiImportRef));
    TEST_REQUIRE(ref != NULL, "direct stdlib import metadata allocated");
    /* The leaf this names must be one the target layer can actually claim: a
     * frozen target-leaf entry, integer-returning, taking no tagged arguments.
     * `io.__fileClose` looked like a direct import too, but its declaration
     * passes a native int and carries no target-leaf entry, so no family
     * covers it and the plan is refused before code generation is reached. */
    memset(ref, 0, sizeof(*ref));
    ref->module_path = "os";
    ref->member_name = "__getpid";
    /* A native leaf is grounded only once resolution has run over it and found
     * no source module: the predicate reads the attempt, not just the empty
     * result, so a reference that was never resolved is not authority. */
    ref->resolution_attempted = true;
    ref->resolved_mod_index = -1;
    ref->resolved_shared_slot = -1;
    ref->resolved_export_slot = -1;
    XiValue *import = xi_value_new(ir, entry, XI_IMPORT_REF, &func_type, 0);
    TEST_REQUIRE(import != NULL, "direct stdlib import token allocated");
    import->aux = ref;
    import->aux_int = -1;
    XiValue *call = xi_value_new(ir, entry, XI_CALL, &int_type, 1);
    TEST_REQUIRE(call != NULL, "direct stdlib import call allocated");
    call->args[0] = import;
    call->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_CALL_EFFECTS;
    xi_block_set_return(entry, call);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "direct stdlib import fixture should generate");
    char dead_decl[64];
    snprintf(dead_decl, sizeof(dead_decl), "XrValue v%u =", (unsigned) import->id);
    TEST_REQUIRE(!contains(code, dead_decl),
                 "direct stdlib function token must not materialize a C local");
    TEST_REQUIRE(contains(code, "xr_os_core_getpid("), "direct stdlib call must remain emitted");

    printf("  Generated direct stdlib import token elision %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_native_target_leaf_consumes_numeric_target_authority) {
    XrType int_type = {
        .kind = XR_KIND_INT,
        .id = 162,
        .scalar_rep = XR_NATIVE_I64,
        .frozen = true,
    };
    XrType function_type = {
        .kind = XR_KIND_FUNCTION,
        .id = 163,
        .frozen = true,
        .function =
            {
                .return_type = &int_type,
                .throw_effect = XR_FN_EFFECT_NO_THROW,
            },
    };
    XiFunc *ir = xi_func_new("native_target_leaf", &int_type);
    XiBlock *entry = ir ? xi_block_new(ir) : NULL;
    TEST_REQUIRE(ir != NULL && entry != NULL, "native target leaf fixture allocated");
    XiImportRef *ref = (XiImportRef *) xi_func_arena_alloc(ir, sizeof(*ref));
    TEST_REQUIRE(ref != NULL, "native target leaf import metadata allocated");
    *ref = (XiImportRef) {
        .module_path = "os",
        .member_name = "__getpid",
        .resolved_mod_index = -1,
        .resolved_shared_slot = -1,
        .resolved_export_slot = -1,
        .resolution_attempted = true,
    };
    XiValue *import = xi_value_new(ir, entry, XI_IMPORT_REF, &function_type, 0);
    XiValue *call = xi_value_new(ir, entry, XI_CALL, &int_type, 1);
    TEST_REQUIRE(import != NULL && call != NULL, "native target leaf call allocated");
    import->aux = ref;
    call->args[0] = import;
    xi_block_set_return(entry, call);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "native_target_leaf", &had_error);
    TEST_REQUIRE(code != NULL && !had_error,
                 "native target leaf fixture should generate from TargetPlan authority");
    TEST_REQUIRE(contains(code, "xr_os_core_getpid()"),
                 "numeric target leaf must project to the scalar OS core symbol");
    TEST_REQUIRE(!contains(code, "xrt_os_getpid("),
                 "native target leaf must not use the tagged legacy AOT wrapper");
    char import_decl[64];
    snprintf(import_decl, sizeof(import_decl), "XrValue v%u =", (unsigned) import->id);
    TEST_REQUIRE(!contains(code, import_decl),
                 "native target leaf import token must not materialize a C local");

    if (g_native_target_leaf_c_output) {
        FILE *generated = fopen(g_native_target_leaf_c_output, "wb");
        size_t length = strlen(code);
        TEST_REQUIRE(generated != NULL, "native target leaf C output should open");
        TEST_REQUIRE(fwrite(code, 1, length, generated) == length && fclose(generated) == 0,
                     "native target leaf C output should be written exactly");
    }

    printf("  Generated numeric native target leaf %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_string_literal_runes_receiver_emits_immediate_without_local) {
    const char *src = "fn hexDigit() -> rune {\n"
                      "    return \"0123456789abcdef\".runes().nth(10)\n"
                      "}\n"
                      "print(hexDigit())\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "string literal runes fixture should compile");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "string literal runes fixture should generate");
    const char *fn = find_static_function_definition(code, "test_hexDigit_");
    TEST_REQUIRE(fn != NULL, "string literal runes function should emit");
    const char *fn_end = next_static_after(fn);
    TEST_REQUIRE(!contains_between(fn, fn_end, "XrValue v0 = xr_str_lit("),
                 "literal runes receiver must not materialize a C local");
    /* The receiver reaches the call inline. Which callee spelling carries it is
     * a code-generation choice -- a direct `xrt_string_runes` today, a dynamic
     * `xrt_method_0` before -- so hold the property, not the spelling. */
    TEST_REQUIRE(contains_between(fn, fn_end, "(xr_str_lit(&_xstr_0))"),
                 "literal runes receiver must remain inline at its call");

    printf("  Generated immediate string runes receiver %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_string_runes_consumes_immutable_emission_recipe) {
    XrType unit_type = {
        .kind = XR_KIND_UNIT, .id = 1165, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType string_type = {
        .kind = XR_KIND_STRING, .id = 1166, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType rune_type = {
        .kind = XR_KIND_RUNE, .id = 1167, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType *iterator_args[] = {&rune_type};
    XrType iterator_type = {
        .kind = XR_KIND_INSTANCE,
        .id = 1168,
        .scalar_rep = XR_SCALAR_REP_NONE,
        .frozen = true,
        .instance = {.class_name = "Iterator", .type_args = iterator_args, .type_arg_count = 1},
    };
    XiFunc *ir = xi_func_new("string_runes_recipe", &unit_type);
    XiBlock *entry = ir ? xi_block_new(ir) : NULL;
    TEST_REQUIRE(entry != NULL, "String.runes recipe fixture allocated");
    XiModule fixture_module = {
        .identity = "memory-module-v1:id=28:string-runes-cgen-fixture-v1",
        .path = "string-runes-cgen-fixture.xr",
        .name = "string_runes_cgen_fixture",
        .init = ir,
    };
    ir->module = &fixture_module;
    entry->sealed = true;
    XiValue *source = xi_const_str(ir, entry, "0123456789abcdef", &string_type);
    XiValue *runes = xi_value_new(ir, entry, XI_CALL_METHOD, &iterator_type, 1);
    XiValue *release = xi_value_new(ir, entry, XI_RELEASE, &unit_type, 1);
    TEST_REQUIRE(source && runes && release, "String.runes recipe values allocated");
    runes->args[0] = source;
    runes->aux = (void *) "runes";
    runes->aux_int = (int64_t) XI_METHOD_SYMBOL_RUNES << 1;
    runes->call_return_ownership.kind = XI_RETURN_OWNERSHIP_OWNED;
    runes->call_return_ownership.param_index = -1;
    runes->call_return_ownership.complete = true;
    release->args[0] = runes;
    xi_block_set_return(entry, NULL);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "string_runes_recipe", &had_error);
    ir->module = NULL;
    TEST_REQUIRE(code != NULL && !had_error, "sealed String.runes recipe should generate");
    TEST_REQUIRE(count_between(code, code + strlen(code), "xrt_string_runes(xr_str_lit(") == 1,
                 "CGen must consume the exact String.runes recipe once");
    TEST_REQUIRE(!contains(code, "xrt_method_0(") && !contains(code, "XRT_SYM_RUNES"),
                 "String.runes must not select a runtime member by name or symbol id");
    if (g_string_runes_c_output) {
        FILE *generated = fopen(g_string_runes_c_output, "wb");
        TEST_REQUIRE(generated != NULL, "String.runes generated-C output opened");
        size_t length = strlen(code);
        TEST_REQUIRE(fwrite(code, 1, length, generated) == length && fclose(generated) == 0,
                     "String.runes generated-C output written");
    }

    printf("  Generated immutable String.runes recipe %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_string_slice_range_consumes_immutable_emission_recipe) {
    XrType string_type = {
        .kind = XR_KIND_STRING, .id = 985, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType int_type = {.kind = XR_KIND_INT, .id = 986, .scalar_rep = XR_NATIVE_I64, .frozen = true};
    XiFunc *ir = xi_func_new("string_slice_range_recipe", &string_type);
    XiBlock *entry = ir ? xi_block_new(ir) : NULL;
    TEST_REQUIRE(entry != NULL, "String range-slice recipe fixture allocated");
    entry->sealed = true;
    XiValue *source = xi_const_str(ir, entry, "0123456789abcdef", &string_type);
    XiValue *start = xi_const_int(ir, entry, 1, &int_type);
    XiValue *end = xi_const_int(ir, entry, 4, &int_type);
    XiValue *slice = xi_value_new(ir, entry, XI_CALL_METHOD, &string_type, 3);
    TEST_REQUIRE(source && start && end && slice, "String range-slice recipe values allocated");
    slice->args[0] = source;
    slice->args[1] = start;
    slice->args[2] = end;
    slice->aux = (void *) "slice";
    slice->aux_int = 32;
    slice->flags |= XI_FLAG_TAIL;
    slice->call_return_ownership.kind = XI_RETURN_OWNERSHIP_OWNED;
    slice->call_return_ownership.param_index = -1;
    slice->call_return_ownership.complete = true;
    ir->arc_return_ownership.kind = XI_RETURN_OWNERSHIP_OWNED;
    ir->arc_return_ownership.param_index = -1;
    ir->arc_return_ownership.complete = true;
    xi_block_set_return(entry, slice);
    bool had_error = false;
    char *code = generate_c_with_status(ir, "string_slice_range_recipe", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "sealed String range-slice recipe should generate");
    size_t slice_call_count = count_between(code, code + strlen(code), "xrt_string_slice_range(");
    TEST_REQUIRE(slice_call_count == 1, "CGen must consume the exact range-slice recipe once");
    TEST_REQUIRE(!contains(code, "XRT_SYM_SLICE"),
                 "CGen must not select String.slice by symbol id");
    TEST_REQUIRE(contains(code, "xrt_has_pending_error("),
                 "String.slice must preserve the pending-error poll");
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_iterator_rune_has_next_consumes_immutable_emission_recipe) {
    XiFunc *ir = compile_to_ir("var iter = \"0123456789abcdef\".runes()\n"
                               "print(iter.hasNext())\n");
    TEST_REQUIRE(ir != NULL, "Iterator<rune>.hasNext recipe fixture should compile");
    bool had_error = false;
    char *code = generate_c_with_status(ir, "iterator_rune_has_next_recipe", &had_error);
    TEST_REQUIRE(code != NULL && !had_error,
                 "sealed Iterator<rune>.hasNext recipe should generate");
    TEST_REQUIRE(count_between(code, code + strlen(code), "xrt_iterator_rune_has_next(") == 1,
                 "CGen must consume the exact hasNext recipe once");
    TEST_REQUIRE(!contains(code, "XRT_SYM_HAS_NEXT"),
                 "CGen must not select Iterator.hasNext by symbol id");
    TEST_REQUIRE(contains(code, "xrt_has_pending_error("),
                 "Iterator.hasNext must preserve the pending-error poll");
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_iterator_rune_next_consumes_immutable_emission_recipe) {
    XrType unit_type = {
        .kind = XR_KIND_UNIT, .id = 970, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType string_type = {
        .kind = XR_KIND_STRING, .id = 971, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType rune_type = {
        .kind = XR_KIND_RUNE, .id = 972, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType *iterator_args[] = {&rune_type};
    XrType iterator_type = {
        .kind = XR_KIND_INSTANCE,
        .id = 973,
        .scalar_rep = XR_SCALAR_REP_NONE,
        .frozen = true,
        .instance = {.class_name = "Iterator", .type_args = iterator_args, .type_arg_count = 1},
    };
    XiFunc *ir = xi_func_new("iterator_rune_next_recipe", &unit_type);
    XiBlock *entry = ir ? xi_block_new(ir) : NULL;
    TEST_REQUIRE(entry != NULL, "Iterator<rune>.next recipe fixture allocated");
    entry->sealed = true;
    XiValue *source = xi_const_str(ir, entry, "0123456789abcdef", &string_type);
    XiValue *runes = xi_value_new(ir, entry, XI_CALL_METHOD, &iterator_type, 1);
    XiValue *next = xi_value_new(ir, entry, XI_CALL_METHOD, &rune_type, 1);
    XiValue *print = xi_value_new(ir, entry, XI_PRINT, &unit_type, 1);
    TEST_REQUIRE(print && test_attach_print_plan(ir, print, 1u),
                 "hand-built print carries its frozen plan");
    XiValue *release = xi_value_new(ir, entry, XI_RELEASE, &unit_type, 1);
    TEST_REQUIRE(source && runes && next && print && release,
                 "Iterator<rune>.next recipe values allocated");
    runes->args[0] = source;
    runes->aux = (void *) "runes";
    runes->aux_int = 470;
    next->args[0] = runes;
    next->aux = (void *) "next";
    next->aux_int = 114;
    next->call_return_ownership.kind = XI_RETURN_OWNERSHIP_OWNED;
    next->call_return_ownership.param_index = -1;
    next->call_return_ownership.complete = true;
    print->args[0] = next;
    release->args[0] = runes;
    xi_block_set_return(entry, NULL);
    bool had_error = false;
    char *code = generate_c_with_status(ir, "iterator_rune_next_recipe", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "sealed Iterator<rune>.next recipe should generate");
    TEST_REQUIRE(count_between(code, code + strlen(code), "xrt_iterator_rune_next(") == 1,
                 "CGen must consume the exact next recipe once");
    TEST_REQUIRE(!contains(code, "XRT_SYM_NEXT"),
                 "CGen must not select Iterator.next by symbol id");
    TEST_REQUIRE(contains(code, "xrt_has_pending_error("),
                 "Iterator.next must preserve the pending-error poll");
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_iterator_rune_nth_consumes_immutable_emission_recipe) {
    XrType unit_type = {
        .kind = XR_KIND_UNIT, .id = 1070, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType string_type = {
        .kind = XR_KIND_STRING, .id = 1071, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType int_type = {
        .kind = XR_KIND_INT, .id = 1072, .scalar_rep = XR_NATIVE_I64, .frozen = true};
    XrType rune_type = {
        .kind = XR_KIND_RUNE, .id = 1073, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType *iterator_args[] = {&rune_type};
    XrType iterator_type = {
        .kind = XR_KIND_INSTANCE,
        .id = 1074,
        .scalar_rep = XR_SCALAR_REP_NONE,
        .frozen = true,
        .instance = {.class_name = "Iterator", .type_args = iterator_args, .type_arg_count = 1},
    };
    XiFunc *ir = xi_func_new("iterator_rune_nth_recipe", &unit_type);
    XiBlock *entry = ir ? xi_block_new(ir) : NULL;
    TEST_REQUIRE(entry != NULL, "Iterator<rune>.nth recipe fixture allocated");
    entry->sealed = true;
    XiValue *source = xi_const_str(ir, entry, "0123456789abcdef", &string_type);
    XiValue *runes = xi_value_new(ir, entry, XI_CALL_METHOD, &iterator_type, 1);
    XiValue *index = xi_const_int(ir, entry, 1, &int_type);
    XiValue *nth = xi_value_new(ir, entry, XI_CALL_METHOD, &rune_type, 2);
    XiValue *print = xi_value_new(ir, entry, XI_PRINT, &unit_type, 1);
    TEST_REQUIRE(print && test_attach_print_plan(ir, print, 1u),
                 "hand-built print carries its frozen plan");
    XiValue *release = xi_value_new(ir, entry, XI_RELEASE, &unit_type, 1);
    TEST_REQUIRE(source && runes && index && nth && print && release,
                 "Iterator<rune>.nth recipe values allocated");
    runes->args[0] = source;
    runes->aux = (void *) "runes";
    runes->aux_int = (int64_t) XI_METHOD_SYMBOL_RUNES << 1;
    nth->args[0] = runes;
    nth->args[1] = index;
    nth->aux = (void *) "nth";
    nth->aux_int = (int64_t) XI_METHOD_SYMBOL_NTH << 1;
    nth->call_return_ownership.kind = XI_RETURN_OWNERSHIP_OWNED;
    nth->call_return_ownership.param_index = -1;
    nth->call_return_ownership.complete = true;
    print->args[0] = nth;
    release->args[0] = runes;
    xi_block_set_return(entry, NULL);
    bool had_error = false;
    char *code = generate_c_with_status(ir, "iterator_rune_nth_recipe", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "sealed Iterator<rune>.nth recipe should generate");
    const char *nth_call = strstr(code, "xrt_iterator_rune_nth(");
    TEST_REQUIRE(count_between(code, code + strlen(code), "xrt_iterator_rune_nth(") == 1,
                 "CGen must consume the exact nth recipe once");
    TEST_REQUIRE(nth_call && strstr(nth_call, "xrt_iterator_rune_nth(v1, INT64_C(1))") == nth_call,
                 "CGen must pass the typed iterator and i64 index operands");
    TEST_REQUIRE(!contains(code, "XRT_SYM_NTH"), "CGen must not select Iterator.nth by symbol id");
    TEST_REQUIRE(contains(code, "xrt_has_pending_error("),
                 "Iterator.nth must preserve the pending-error poll");
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_rune_to_uint32_consumes_immutable_emission_recipe) {
    XrType unit_type = {
        .kind = XR_KIND_UNIT, .id = 974, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType string_type = {
        .kind = XR_KIND_STRING, .id = 975, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType rune_type = {
        .kind = XR_KIND_RUNE, .id = 976, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType u32_type = {.kind = XR_KIND_INT, .id = 977, .scalar_rep = XR_NATIVE_U32, .frozen = true};
    XrType *iterator_args[] = {&rune_type};
    XrType iterator_type = {
        .kind = XR_KIND_INSTANCE,
        .id = 978,
        .scalar_rep = XR_SCALAR_REP_NONE,
        .frozen = true,
        .instance = {.class_name = "Iterator", .type_args = iterator_args, .type_arg_count = 1},
    };
    XiFunc *ir = xi_func_new("rune_to_uint32_recipe", &unit_type);
    XiBlock *entry = ir ? xi_block_new(ir) : NULL;
    TEST_REQUIRE(entry != NULL, "rune.toUInt32 recipe fixture allocated");
    entry->sealed = true;
    XiValue *source = xi_const_str(ir, entry, "0123456789abcdef", &string_type);
    XiValue *runes = xi_value_new(ir, entry, XI_CALL_METHOD, &iterator_type, 1);
    XiValue *next = xi_value_new(ir, entry, XI_CALL_METHOD, &rune_type, 1);
    XiValue *to_u32 = xi_value_new(ir, entry, XI_CALL_METHOD, &u32_type, 1);
    XiValue *print = xi_value_new(ir, entry, XI_PRINT, &unit_type, 1);
    TEST_REQUIRE(print && test_attach_print_plan(ir, print, 1u),
                 "hand-built print carries its frozen plan");
    XiValue *release = xi_value_new(ir, entry, XI_RELEASE, &unit_type, 1);
    TEST_REQUIRE(source && runes && next && to_u32 && print && release,
                 "rune.toUInt32 recipe values allocated");
    runes->args[0] = source;
    runes->aux = (void *) "runes";
    runes->aux_int = 470;
    next->args[0] = runes;
    next->aux = (void *) "next";
    next->aux_int = 114;
    next->call_return_ownership.kind = XI_RETURN_OWNERSHIP_OWNED;
    next->call_return_ownership.param_index = -1;
    next->call_return_ownership.complete = true;
    to_u32->args[0] = next;
    to_u32->aux = (void *) "toUInt32";
    to_u32->aux_int = 474;
    print->args[0] = to_u32;
    release->args[0] = runes;
    xi_block_set_return(entry, NULL);
    bool had_error = false;
    char *code = generate_c_with_status(ir, "rune_to_uint32_recipe", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "sealed rune.toUInt32 recipe should generate");
    TEST_REQUIRE(count_between(code, code + strlen(code), "xrt_rune_to_uint32(") == 1,
                 "CGen must consume the exact rune-to-u32 recipe once");
    TEST_REQUIRE(!contains(code, "XRT_SYM_TO_UINT32"),
                 "CGen must not select rune.toUInt32 by symbol id");
    TEST_REQUIRE(!contains(code, "(uint32_t)("),
                 "CGen must not use the removed live-type numeric fallback");
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_rune_to_string_consumes_immutable_emission_recipe) {
    XrType unit_type = {
        .kind = XR_KIND_UNIT, .id = 1170, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType string_type = {
        .kind = XR_KIND_STRING, .id = 1171, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType int_type = {
        .kind = XR_KIND_INT, .id = 1172, .scalar_rep = XR_NATIVE_I64, .frozen = true};
    XrType rune_type = {
        .kind = XR_KIND_RUNE, .id = 1173, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType *iterator_args[] = {&rune_type};
    XrType iterator_type = {
        .kind = XR_KIND_INSTANCE,
        .id = 1174,
        .scalar_rep = XR_SCALAR_REP_NONE,
        .frozen = true,
        .instance = {.class_name = "Iterator", .type_args = iterator_args, .type_arg_count = 1},
    };
    XiFunc *ir = xi_func_new("rune_to_string_recipe", &unit_type);
    XiBlock *entry = ir ? xi_block_new(ir) : NULL;
    TEST_REQUIRE(entry != NULL, "rune.toString recipe fixture allocated");
    XiModule fixture_module = {
        .identity = "memory-module-v1:id=30:rune-to-string-cgen-fixture-v1",
        .path = "rune-to-string-cgen-fixture.xr",
        .name = "rune_to_string_cgen_fixture",
        .init = ir,
    };
    ir->module = &fixture_module;
    entry->sealed = true;
    XiValue *source = xi_const_str(ir, entry, "0123456789abcdef", &string_type);
    XiValue *runes = xi_value_new(ir, entry, XI_CALL_METHOD, &iterator_type, 1);
    XiValue *index = xi_const_int(ir, entry, 1, &int_type);
    XiValue *nth = xi_value_new(ir, entry, XI_CALL_METHOD, &rune_type, 2);
    XiValue *to_string = xi_value_new(ir, entry, XI_CALL_METHOD, &string_type, 1);
    XiValue *print = xi_value_new(ir, entry, XI_PRINT, &unit_type, 1);
    TEST_REQUIRE(print && test_attach_print_plan(ir, print, 1u),
                 "hand-built print carries its frozen plan");
    XiValue *release_string = xi_value_new(ir, entry, XI_RELEASE, &unit_type, 1);
    XiValue *release_runes = xi_value_new(ir, entry, XI_RELEASE, &unit_type, 1);
    TEST_REQUIRE(source && runes && index && nth && to_string && print && release_string &&
                     release_runes,
                 "rune.toString recipe values allocated");
    runes->args[0] = source;
    runes->aux = (void *) "runes";
    runes->aux_int = (int64_t) XI_METHOD_SYMBOL_RUNES << 1;
    nth->args[0] = runes;
    nth->args[1] = index;
    nth->aux = (void *) "nth";
    nth->aux_int = (int64_t) XI_METHOD_SYMBOL_NTH << 1;
    nth->call_return_ownership.kind = XI_RETURN_OWNERSHIP_OWNED;
    nth->call_return_ownership.param_index = -1;
    nth->call_return_ownership.complete = true;
    to_string->args[0] = nth;
    to_string->aux = (void *) "toString";
    to_string->aux_int = (int64_t) XI_METHOD_SYMBOL_TOSTRING << 1;
    to_string->call_return_ownership.kind = XI_RETURN_OWNERSHIP_OWNED;
    to_string->call_return_ownership.param_index = -1;
    to_string->call_return_ownership.complete = true;
    print->args[0] = to_string;
    release_string->args[0] = to_string;
    release_runes->args[0] = runes;
    xi_block_set_return(entry, NULL);
    bool had_error = false;
    char *code = generate_c_with_status(ir, "rune_to_string_recipe", &had_error);
    ir->module = NULL;
    TEST_REQUIRE(code != NULL && !had_error, "sealed rune.toString recipe should generate");
    const char *to_string_call = strstr(code, "xrt_rune_to_string(");
    TEST_REQUIRE(count_between(code, code + strlen(code), "xrt_rune_to_string(") == 1,
                 "CGen must consume the exact rune-to-string recipe once");
    TEST_REQUIRE(to_string_call &&
                     strstr(to_string_call, "xrt_rune_to_string(v3)") == to_string_call,
                 "CGen must pass the native Rune operand");
    TEST_REQUIRE(!contains(code, "XRT_SYM_TOSTRING"),
                 "CGen must not select rune.toString by symbol id");
    if (g_rune_to_string_c_output) {
        FILE *generated = fopen(g_rune_to_string_c_output, "wb");
        TEST_REQUIRE(generated != NULL, "rune.toString generated-C output opened");
        size_t length = strlen(code);
        TEST_REQUIRE(fwrite(code, 1, length, generated) == length && fclose(generated) == 0,
                     "rune.toString generated-C output written");
    }
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_rune_is_whitespace_consumes_immutable_emission_recipe) {
    XrType unit_type = {
        .kind = XR_KIND_UNIT, .id = 979, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType string_type = {
        .kind = XR_KIND_STRING, .id = 980, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType rune_type = {
        .kind = XR_KIND_RUNE, .id = 981, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType bool_type = {
        .kind = XR_KIND_BOOL, .id = 982, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType *iterator_args[] = {&rune_type};
    XrType iterator_type = {
        .kind = XR_KIND_INSTANCE,
        .id = 983,
        .scalar_rep = XR_SCALAR_REP_NONE,
        .frozen = true,
        .instance = {.class_name = "Iterator", .type_args = iterator_args, .type_arg_count = 1},
    };
    XiFunc *ir = xi_func_new("rune_is_whitespace_recipe", &unit_type);
    XiBlock *entry = ir ? xi_block_new(ir) : NULL;
    TEST_REQUIRE(entry != NULL, "rune.isWhitespace recipe fixture allocated");
    entry->sealed = true;
    XiValue *source = xi_const_str(ir, entry, " 0123456789abcdef", &string_type);
    XiValue *runes = xi_value_new(ir, entry, XI_CALL_METHOD, &iterator_type, 1);
    XiValue *next = xi_value_new(ir, entry, XI_CALL_METHOD, &rune_type, 1);
    XiValue *is_whitespace = xi_value_new(ir, entry, XI_CALL_METHOD, &bool_type, 1);
    XiValue *print = xi_value_new(ir, entry, XI_PRINT, &unit_type, 1);
    TEST_REQUIRE(print && test_attach_print_plan(ir, print, 1u),
                 "hand-built print carries its frozen plan");
    XiValue *release = xi_value_new(ir, entry, XI_RELEASE, &unit_type, 1);
    TEST_REQUIRE(source && runes && next && is_whitespace && print && release,
                 "rune.isWhitespace recipe values allocated");
    runes->args[0] = source;
    runes->aux = (void *) "runes";
    runes->aux_int = 470;
    next->args[0] = runes;
    next->aux = (void *) "next";
    next->aux_int = 114;
    next->call_return_ownership.kind = XI_RETURN_OWNERSHIP_OWNED;
    next->call_return_ownership.param_index = -1;
    next->call_return_ownership.complete = true;
    is_whitespace->args[0] = next;
    is_whitespace->aux = (void *) "isWhitespace";
    is_whitespace->aux_int = 90;
    print->args[0] = is_whitespace;
    release->args[0] = runes;
    xi_block_set_return(entry, NULL);
    bool had_error = false;
    char *code = generate_c_with_status(ir, "rune_is_whitespace_recipe", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "sealed rune.isWhitespace recipe should generate");
    TEST_REQUIRE(count_between(code, code + strlen(code), "xrt_rune_is_whitespace(") == 1,
                 "CGen must consume the exact whitespace recipe once");
    TEST_REQUIRE(!contains(code, "XRT_SYM_IS_WHITESPACE"),
                 "CGen must not select rune.isWhitespace by symbol id");
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_span_passed_only_to_direct_call_omits_data_cache) {
    const char *src = "fn viewLength(view: Slice<u8>) -> i64 { return len(view) }\n"
                      "fn slicedLength(bytes: Array<u8>) -> i64 {\n"
                      "    const view: Slice<u8> = bytes[1:]\n"
                      "    var decoded = string.fromUtf8(bytes)\n"
                      "    if (decoded == null) { return 0 }\n"
                      "    return viewLength(view)\n"
                      "}\n"
                      "var bytes: Array<u8> = [1, 2, 3]\n"
                      "print(slicedLength(bytes))\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "direct span argument fixture should compile");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "direct span argument fixture should generate");
    const char *fn = find_static_function_definition(code, "test_slicedLength_");
    TEST_REQUIRE(fn != NULL, "direct span argument function should emit");
    const char *fn_end = next_static_after(fn);
    TEST_REQUIRE(
        count_between(fn, fn_end, "_ad") == 0,
        "a Slice used only by a direct Slice parameter and error cleanup must not cache data");
    TEST_REQUIRE(contains_between(fn, fn_end, "xrt_has_pending_error()"),
                 "the direct Slice fixture must exercise an intervening fallible call");
    TEST_REQUIRE(contains_between(fn, fn_end, "test_viewLength_"),
                 "the direct Slice call must remain emitted");

    printf("  Generated direct span argument without data cache %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_span_print_materializes_scoped_portable_view) {
    const char *src = "fn run() {\n"
                      "    var values: Array<i64> = [1, 2, 3, 4]\n"
                      "    const tail: Slice<i64> = values[-2:]\n"
                      "    print(tail)\n"
                      "}\n"
                      "run()\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "Slice print fixture should compile");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "Slice print fixture should generate");
    TEST_REQUIRE(contains(code, "xrt_array_t _xspan_print_view_"),
                 "Slice print must materialize one scoped native view");
    TEST_REQUIRE(contains(code, "xrt_array_stack_borrow_span_view_init(&_xspan_print_view_"),
                 "Slice print must initialize the scoped view through the typed runtime owner");
    TEST_REQUIRE(!contains(code, "xrt_array_stack_borrow_span_view_typed") &&
                     !contains(code, "({ xr_span_t"),
                 "Slice print must not retain the retired GNU statement-expression path");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_unused_shared_load_is_debug_only_when_source_bound) {
    XrType u64_type = {.kind = XR_KIND_INT, .id = 933, .scalar_rep = XR_NATIVE_U64, .frozen = true};
    XiFunc *ir = xi_func_new("manual_debug_shared", &u64_type);
    TEST_REQUIRE(ir != NULL, "manual debug-shared function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual debug-shared entry block allocated");
    entry->sealed = true;

    ir->nshared = 1;
    XiValue *load = xi_value_new(ir, entry, XI_GET_SHARED, &u64_type, 0);
    TEST_REQUIRE(load != NULL, "manual debug-shared load allocated");
    load->aux_int = 0;
    load->var_id = 0;
    ir->source_var_count = 1;
    ir->source_var_names = (const char **) xi_func_arena_alloc(ir, sizeof(*ir->source_var_names));
    TEST_REQUIRE(ir->source_var_names != NULL, "manual debug-shared name table allocated");
    ir->source_var_names[0] = "sharedValue";
    XiValue *zero = xi_const_int(ir, entry, 0, &u64_type);
    TEST_REQUIRE(zero != NULL, "manual debug-shared return allocated");
    xi_block_set_return(entry, zero);

    XiModule *mod = xi_module_new("test.xr", "test", ir);
    TEST_REQUIRE(mod != NULL, "manual debug-shared module allocated");
    mod->nslots = 1;
    ir->module = mod;

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "unused shared-load C generation failed");
    TEST_REQUIRE(!had_error, "unused shared-load fixture should generate");
    const char *fn = find_static_function_definition(code, "manual_debug_shared");
    TEST_REQUIRE(fn != NULL, "manual debug-shared definition should be emitted");
    const char *fn_end = strstr(fn, "\n}\n");
    TEST_REQUIRE(fn_end != NULL, "manual debug-shared function end emitted");
    TEST_REQUIRE(contains_between(fn, fn_end, "XrValue sharedValue = XR_NULL_VAL;"),
                 "debug-local builds must declare the source-level shared slot");
    TEST_REQUIRE(contains_between(fn, fn_end, "#if defined(XRAY_AOT_DEBUG_LOCALS)"),
                 "the unused source-bound shared load must be debug-only");
    TEST_REQUIRE(count_between(fn, fn_end, "xrt_shared[0]") == 1,
                 "release C must not duplicate an unused shared-slot load");

    printf("  Generated debug-only unused shared load %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_consumed_shared_load_stays_release_materialized) {
    XrType u64_type = {.kind = XR_KIND_INT, .id = 934, .scalar_rep = XR_NATIVE_U64, .frozen = true};
    XiFunc *ir = xi_func_new("manual_consumed_shared", &u64_type);
    TEST_REQUIRE(ir != NULL, "manual consumed-shared function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual consumed-shared entry block allocated");
    entry->sealed = true;
    ir->nshared = 1;
    XiValue *load = xi_value_new(ir, entry, XI_GET_SHARED, &u64_type, 0);
    TEST_REQUIRE(load != NULL, "manual consumed-shared load allocated");
    load->aux_int = 0;
    xi_block_set_return(entry, load);

    XiModule *mod = xi_module_new("test.xr", "test", ir);
    TEST_REQUIRE(mod != NULL, "manual consumed-shared module allocated");
    mod->nslots = 1;
    ir->module = mod;

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "consumed shared-load C generation failed");
    TEST_REQUIRE(!had_error, "consumed shared-load fixture should generate");
    const char *fn = find_static_function_definition(code, "manual_consumed_shared");
    TEST_REQUIRE(fn != NULL, "manual consumed-shared definition should be emitted");
    const char *fn_end = strstr(fn, "\n}\n");
    TEST_REQUIRE(fn_end != NULL, "manual consumed-shared function end emitted");
    TEST_REQUIRE(contains_between(fn, fn_end, "XrValue v0 = xrt_shared[0];"),
                 "a shared load consumed by the return must remain materialized");
    TEST_REQUIRE(contains_between(fn, fn_end, "XR_TO_INT(v0)"),
                 "the consumed shared local must feed the native return conversion");

    printf("  Kept consumed shared load in %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_shared_store_uses_portable_owned_value_helper) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 935, .scalar_rep = XR_NATIVE_I64, .frozen = true};
    XrType unit_type = {
        .kind = XR_KIND_UNIT, .scalar_rep = XR_SCALAR_REP_NONE, .id = 936, .frozen = true};
    XiFunc *ir = xi_func_new("manual_portable_shared_store", &int_type);
    TEST_REQUIRE(ir != NULL, "manual shared-store function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual shared-store entry block allocated");
    entry->sealed = true;
    ir->nshared = 1;

    XiValue *literal = xi_const_int(ir, entry, 42, &int_type);
    TEST_REQUIRE(literal != NULL, "manual shared-store literal allocated");
    XiValue *store = xi_value_new(ir, entry, XI_SET_SHARED, &unit_type, 1);
    TEST_REQUIRE(store != NULL, "manual shared-store operation allocated");
    store->args[0] = literal;
    store->aux_int = 0;
    store->flags |= XI_FLAG_WRITES_MEM | XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(entry, literal);

    XiModule *mod = xi_module_new("test.xr", "test", ir);
    TEST_REQUIRE(mod != NULL, "manual shared-store module allocated");
    mod->nslots = 1;
    ir->module = mod;

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "manual shared-store C generation failed");
    TEST_REQUIRE(!had_error, "manual shared-store fixture should generate");
    TEST_REQUIRE(contains(code, "xrt_array_ref_ensure_owned("),
                 "shared stores must use the portable ownership helper");
    TEST_REQUIRE(!contains(code, "({ XrValue _xsv"),
                 "shared stores must not emit a GNU statement expression");

    printf("  Generated portable shared store %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_immediate_scalar_constant_inlines_into_as_cast) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 926, .scalar_rep = XR_NATIVE_I64, .frozen = true};
    XrType u64_type = {.kind = XR_KIND_INT, .id = 927, .scalar_rep = XR_NATIVE_U64, .frozen = true};
    XiFunc *ir = xi_func_new("manual_const_as", &u64_type);
    TEST_REQUIRE(ir != NULL, "manual const-as function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual const-as entry block allocated");
    entry->sealed = true;

    XiValue *literal = xi_const_int(ir, entry, 64, &int_type);
    TEST_REQUIRE(literal != NULL, "manual const-as literal allocated");
    XiValue *cast = xi_value_new(ir, entry, XI_CONVERT, &u64_type, 1);
    TEST_REQUIRE(cast != NULL, "manual const-as cast allocated");
    cast->args[0] = literal;
    cast->conversion = (XrConversionWitness) {
        .kind = XR_CONVERSION_EXPLICIT_SIGN_CHANGE,
        .source_scalar_rep = XR_NATIVE_I64,
        .target_scalar_rep = XR_NATIVE_U64,
        .is_implicit = false,
        .is_compile_time = true,
    };
    xi_block_set_return(entry, cast);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "manual const-as C generation failed");
    TEST_REQUIRE(!had_error, "manual const-as fixture should generate");
    const char *fn = find_static_function_definition(code, "manual_const_as");
    TEST_REQUIRE(fn != NULL, "manual const-as definition should be emitted");
    const char *fn_end = strstr(fn, "\n}\n");
    TEST_REQUIRE(fn_end != NULL, "manual const-as function end emitted");
    TEST_REQUIRE(!contains_between(fn, fn_end, "int64_t v0 = INT64_C(64);"),
                 "literal-aware XI_CONVERT must not require a source constant local");
    TEST_REQUIRE(contains_between(fn, fn_end, "INT64_C(64)"),
                 "XI_CONVERT result must still contain the exact scalar literal");

    printf("  Generated immediate scalar cast %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_dynamic_conversion_inlines_null_literal_without_forward_ref) {
    const char *source = "fn nullToBool() -> bool { return bool(null) }\n"
                         "print(nullToBool())\n";
    XiFunc *ir = compile_to_ir(source);
    TEST_REQUIRE(ir != NULL, "null-to-bool source compiled to Xi");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "null-to-bool C generation failed");
    TEST_REQUIRE(!had_error, "null-to-bool fixture should generate");
    TEST_REQUIRE(!contains(code, "xrt_to_bool(v"),
                 "elided null literal must not be referenced through a source C local");
    TEST_REQUIRE(contains(code, "xrt_to_bool(XR_NULL_VAL)"),
                 "dynamic conversion must consume the exact null immediate");

    printf("  Generated immediate dynamic conversion %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_immediate_scalar_constant_inlines_into_place_store) {
    XrType u64_type = {.kind = XR_KIND_INT, .id = 930, .scalar_rep = XR_NATIVE_U64, .frozen = true};
    XrType unit_type = {
        .kind = XR_KIND_UNIT, .scalar_rep = XR_SCALAR_REP_NONE, .id = 931, .frozen = true};
    XiFunc *ir = xi_func_new("manual_const_place_store", &u64_type);
    TEST_REQUIRE(ir != NULL, "manual constant place-store function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual constant place-store entry block allocated");
    entry->sealed = true;

    ir->nparams = 1;
    ir->min_params = 1;
    ir->params = (XiValue **) xr_calloc(1, sizeof(XiValue *));
    TEST_REQUIRE(ir->params != NULL, "manual constant place-store parameter table allocated");
    XiValue *place = xi_param(ir, entry, 0, &u64_type);
    TEST_REQUIRE(place != NULL, "manual constant place-store parameter allocated");
    place->param_mode = XR_PARAM_REF;
    ir->params[0] = place;
    XiValue *literal = xi_const_int(ir, entry, 64, &u64_type);
    TEST_REQUIRE(literal != NULL, "manual constant place-store literal allocated");
    XiValue *store = xi_value_new(ir, entry, XI_PLACE_STORE, &unit_type, 2);
    TEST_REQUIRE(store != NULL, "manual constant place-store operation allocated");
    store->args[0] = place;
    store->args[1] = literal;
    store->flags |= XI_FLAG_WRITES_MEM | XI_FLAG_SIDE_EFFECT;
    XiValue *load = xi_value_new(ir, entry, XI_PLACE_LOAD, &u64_type, 1);
    TEST_REQUIRE(load != NULL, "manual constant place-load operation allocated");
    load->args[0] = place;
    load->flags |= XI_FLAG_READS_MEM;
    xi_block_set_return(entry, load);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "constant place-store C generation failed");
    TEST_REQUIRE(!had_error, "constant place-store fixture should generate");
    const char *reset = find_static_function_definition(code, "manual_const_place_store");
    TEST_REQUIRE(reset != NULL, "manual constant place-store definition should be emitted");
    const char *reset_end = strstr(reset, "\n}\n");
    TEST_REQUIRE(reset_end != NULL, "manual constant place-store function end emitted");
    TEST_REQUIRE(contains_between(reset, reset_end, ")) = ") &&
                     contains_between(reset, reset_end, "INT64_C(64)"),
                 "scalar place store must receive the immediate literal directly");
    TEST_REQUIRE(count_between(reset, reset_end, " = INT64_C(64);") == 0,
                 "scalar place store must not retain a dead constant local");

    printf("  Generated immediate scalar place store %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_native_signed_i64_constant_emits_immediate_without_local) {
    const char *src = "@noinline\n"
                      "fn addOne(value: i64) -> i64 {\n"
                      "    return value + 1\n"
                      "}\n"
                      "print(addOne(41))\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "signed i64 constant IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "signed i64 constant C generation failed");
    TEST_REQUIRE(!had_error, "signed i64 constant fixture should generate");
    const char *fn = find_static_function_definition(code, "addOne_");
    TEST_REQUIRE(fn != NULL, "signed i64 constant definition should be emitted");
    const char *fn_end = next_static_after(fn);
    TEST_REQUIRE(!contains_between(fn, fn_end, " = INT64_C(1);"),
                 "signed wrap arithmetic must not retain a dead constant local");
    TEST_REQUIRE(contains_between(fn, fn_end, "(uint64_t)(INT64_C(1))"),
                 "signed wrap arithmetic must retain the exact immediate operand");

    printf("  Generated immediate signed i64 arithmetic in %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_runtime_string_slice_constant_emits_immediate_without_local) {
    const char *src = "@noinline\n"
                      "fn firstArgumentTail() -> string {\n"
                      "    var value = process.args[0]\n"
                      "    return value.slice(2)\n"
                      "}\n"
                      "print(firstArgumentTail())\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "runtime string-slice constant IR compilation failed");

    XiFunc *tail = NULL;
    for (uint16_t i = 0; i < ir->nchildren; i++) {
        if (ir->children[i] && ir->children[i]->name &&
            strcmp(ir->children[i]->name, "firstArgumentTail") == 0) {
            tail = ir->children[i];
            break;
        }
    }
    TEST_REQUIRE(tail != NULL, "runtime string-slice function IR exists");
    XiValue *dynamic_slice = NULL;
    XiValue *checked_slice = NULL;
    XiValue *promotion = NULL;
    uint32_t promotion_count = 0;
    for (uint32_t b = 0; b < tail->nblocks; b++) {
        XiBlock *block = tail->blocks[b];
        for (uint32_t i = 0; block && i < block->nvalues; i++) {
            XiValue *value = block->values[i];
            if (value && value->op == XI_CALL_METHOD && value->aux &&
                strcmp((const char *) value->aux, "slice") == 0)
                dynamic_slice = value;
            else if (value && value->op == XI_CHECKTYPE)
                checked_slice = value;
            else if (value && value->op == XI_RETAIN && checked_slice && value->nargs == 1 &&
                     value->args[0] == checked_slice) {
                promotion = value;
                promotion_count++;
            }
        }
    }
    TEST_REQUIRE(dynamic_slice && !dynamic_slice->call_return_ownership.complete,
                 "dynamic string-slice result remains alias-uncertain");
    TEST_REQUIRE(checked_slice && checked_slice->nargs == 1 &&
                     checked_slice->args[0] == dynamic_slice,
                 "CHECKTYPE borrows the unresolved call result before promotion");
    TEST_REQUIRE(promotion && promotion->nargs == 1 && promotion->args[0] == checked_slice,
                 "return transfer promotes the checked unresolved result exactly once");
    TEST_REQUIRE(promotion_count == 1, "return transfer has exactly one checked-result promotion");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "runtime string-slice constant C generation failed");
    TEST_REQUIRE(!had_error, "runtime string-slice constant fixture should generate");
    const char *fn = find_static_function_definition(code, "firstArgumentTail_");
    TEST_REQUIRE(fn != NULL, "runtime string-slice definition should be emitted");
    const char *fn_end = next_static_after(fn);
    TEST_REQUIRE(!contains_between(fn, fn_end, " = INT64_C(2);"),
                 "dynamic string slice must not retain a dead constant local");
    TEST_REQUIRE(contains_between(fn, fn_end, "XR_FROM_INT(INT64_C(2))"),
                 "dynamic string slice must retain the exact immediate operand");

    printf("  Generated immediate dynamic string slice in %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_constants_emit_immediate_without_locals) {
    const char *src = "@noinline\n"
                      "fn bytes() -> Array<u8> {\n"
                      "    var out = Array<u8>(1)\n"
                      "    out.push(92)\n"
                      "    return out\n"
                      "}\n"
                      "print(len(bytes()))\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "typed-array constant IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "typed-array constant C generation failed");
    TEST_REQUIRE(!had_error, "typed-array constant fixture should generate");
    const char *fn = find_static_function_definition(code, "bytes_");
    TEST_REQUIRE(fn != NULL, "typed-array constant definition should be emitted");
    const char *fn_end = next_static_after(fn);
    TEST_REQUIRE(!contains_between(fn, fn_end, " = INT64_C(1);"),
                 "typed array capacity constant must not leave a local");
    TEST_REQUIRE(!contains_between(fn, fn_end, " = INT64_C(92);"),
                 "typed array push constant must not leave a local");
    TEST_REQUIRE(contains_between(fn, fn_end, "INT64_C(92)"),
                 "typed array push must retain its exact immediate value");

    printf("  Generated immediate typed-array constants in %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_clean_narrow_arithmetic_keeps_required_constant_local) {
    const char *src = "@noinline\n"
                      "fn narrow(value: u32) -> u32 {\n"
                      "    return value * 2246822519\n"
                      "}\n"
                      "print(narrow(7))\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "clean-narrow constant IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "manual narrow-constant C generation failed");
    TEST_REQUIRE(!had_error, "manual narrow-constant fixture should generate");
    const char *fn = find_static_function_definition(code, "narrow_");
    TEST_REQUIRE(fn != NULL, "clean-narrow constant definition should be emitted");
    const char *fn_end = next_static_after(fn);
    TEST_REQUIRE(contains_between(fn, fn_end, " = INT64_C(2246822519);") &&
                     contains_between(fn, fn_end, "2246822519") &&
                     contains_between(fn, fn_end, "(v"),
                 "clean-narrow arithmetic must retain constants referenced by emit_vref");

    printf("  Preserved clean-narrow constant local in %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_scalar_emission_plan_owns_local_rep_and_c_spelling) {
    XrType u32_type = {.kind = XR_KIND_INT, .id = 961, .scalar_rep = XR_NATIVE_U32, .frozen = true};
    XrType string_type = {
        .kind = XR_KIND_STRING, .id = 964, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType unit_type = {
        .kind = XR_KIND_UNIT, .id = 965, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XrType channel_type = {
        .kind = XR_KIND_CHANNEL, .id = 966, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    channel_type.container.element_type = &u32_type;
    XiFunc *ir = xi_func_new("scalar_emission_consumer", &u32_type);
    TEST_REQUIRE(ir != NULL, "scalar emission consumer function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "scalar emission consumer entry allocated");
    entry->sealed = true;
    ir->nparams = 1;
    ir->min_params = 1;
    ir->params = (XiValue **) xr_calloc(1, sizeof(*ir->params));
    TEST_REQUIRE(ir->params != NULL, "scalar emission consumer parameter table allocated");
    XiValue *parameter = xi_param(ir, entry, 0, &u32_type);
    XiValue *one = xi_const_int(ir, entry, 1, &u32_type);
    XiValue *sum = xi_value_new(ir, entry, XI_ADD, &u32_type, 2);
    XiValue *literal = xi_const_str(ir, entry, "immutable-authority", &string_type);
    XiValue *print = xi_value_new(ir, entry, XI_PRINT, &unit_type, 1);
    TEST_REQUIRE(print && test_attach_print_plan(ir, print, 1u),
                 "hand-built print carries its frozen plan");
    XiValue *capacity = xi_const_int(ir, entry, 3, &u32_type);
    XiValue *channel = xi_value_new(ir, entry, XI_CHAN_NEW, &channel_type, 1);
    XiValue *receive = xi_value_new(ir, entry, XI_CHAN_TRY_RECV, &u32_type, 1);
    TEST_REQUIRE(parameter && one && sum && literal && print && capacity && channel && receive,
                 "value emission consumer values allocated");
    ir->params[0] = parameter;
    sum->args[0] = parameter;
    sum->args[1] = one;
    print->args[0] = literal;
    channel->args[0] = capacity;
    receive->args[0] = channel;
    xi_block_set_return(entry, sum);
    TEST_REQUIRE(test_prepare_backend_ir(ir), "scalar emission consumer backend prepared");

    XiModule *module = xi_module_new("scalar_consumer.xr", "scalar_consumer", ir);
    TEST_REQUIRE(module != NULL, "scalar emission consumer module allocated");
    XiModule *modules[] = {module};
    TestAotPlan legacy_plan;
    test_aot_plan_prepare(&legacy_plan, modules, 1, 0);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    TEST_REQUIRE(profile != NULL, "scalar emission consumer profile built");
    XrTargetPlan *target_plan = NULL;
    XrCEmissionPlan *emission_plan = NULL;
    char error[512] = {0};
    TEST_REQUIRE(
        xr_target_plan_build(ir->semantic_plan, profile, &target_plan, error, sizeof(error)),
        "scalar emission consumer TargetPlan built");

    TEST_REQUIRE(xr_c_emission_plan_build(target_plan, xr_target_profile_fingerprint(profile),
                                          &emission_plan, error, sizeof(error)),
                 "scalar emission consumer C plan built");
    TEST_REQUIRE(xaot_bundle_set_program_target_plan(&legacy_plan.bundle, target_plan),
                 "scalar emission TargetPlan attached to module registry");

    TEST_REQUIRE(xaot_bundle_find_value_plan(&legacy_plan.bundle, parameter) == NULL &&
                     xaot_bundle_find_value_plan(&legacy_plan.bundle, one) == NULL &&
                     xaot_bundle_find_value_plan(&legacy_plan.bundle, sum) == NULL &&
                     xaot_bundle_find_value_plan(&legacy_plan.bundle, channel) == NULL &&
                     xaot_bundle_find_value_plan(&legacy_plan.bundle, receive) == NULL,
                 "migrated value families have no legacy Xaot rows");

    XiCgenCtx *ctx = xi_cgen_ctx_new();
    TEST_REQUIRE(ctx != NULL, "scalar emission consumer context allocated");
    xi_cgen_ctx_set_aot_bundle(ctx, &legacy_plan.bundle);
    const XrCEmissionPlan *emission_plans[] = {emission_plan};
    TEST_REQUIRE(xi_cgen_ctx_set_value_emission_plans(ctx, emission_plans, 1),
                 "verified value emission plans attached");
    emission_plans[0] = NULL;

    /* The normal plan carries no scalar legacy row. Inject a corrupt residue
     * after registry validation to prove that neither it nor the mutable Xi
     * representation can override the immutable emission authority. */
    TEST_REQUIRE(xaot_bundle_find_value_plan(&legacy_plan.bundle, sum) == NULL,
                 "bound scalar has no legacy value row");
    XaotValuePlan *legacy_value = xaot_bundle_add_value_plan(&legacy_plan.bundle, ir, sum);
    TEST_REQUIRE(legacy_value != NULL, "legacy scalar residue injected");
    legacy_value->rep.kind = XAOT_VALUE_TAGGED;
    legacy_value->rep.rep = XAOT_REP_TAGGED;
    legacy_value->rep.c_type = "XrValue";
    legacy_value->rep.flags = 0;
    uint8_t saved_xi_rep = sum->rep;
    uint8_t saved_channel_rep = channel->rep;
    uint8_t saved_receive_rep = receive->rep;
    sum->rep = XR_REP_TAGGED;
    channel->rep = XR_REP_PTR;
    receive->rep = XR_REP_TAGGED;
    const char *saved_literal = (const char *) literal->aux;
    literal->aux = "forged-live-xi";

    char *buf = NULL;
    size_t bufsz = 0;
    FILE *mem = xr_open_memstream(&buf, &bufsz);
    TEST_REQUIRE(mem != NULL, "scalar emission consumer stream opened");
    xi_cgen_program(ctx, mem, module);
    TEST_REQUIRE(xr_close_memstream(mem, &buf, &bufsz) == 0,
                 "scalar emission consumer stream closed");
    sum->rep = saved_xi_rep;
    channel->rep = saved_channel_rep;
    receive->rep = saved_receive_rep;
    literal->aux = (void *) saved_literal;

    TEST_REQUIRE(!xi_cgen_has_error(ctx), "scalar emission consumer generated without error");
    const char *fn = find_static_function_definition(buf, "scalar_emission_consumer");
    TEST_REQUIRE(fn != NULL, "scalar emission consumer definition emitted");
    const char *fn_end = strstr(fn, "\n}\n");
    TEST_REQUIRE(fn_end != NULL, "scalar emission consumer definition terminated");
    char scalar_decl[64];
    char poisoned_scalar_decl[64];
    snprintf(scalar_decl, sizeof(scalar_decl), "uint32_t v%u =", (unsigned) sum->id);
    snprintf(poisoned_scalar_decl, sizeof(poisoned_scalar_decl), "XrValue v%u", (unsigned) sum->id);
    TEST_REQUIRE(contains_between(fn, fn_end, scalar_decl),
                 "immutable C emission plan owns scalar local spelling");
    TEST_REQUIRE(!contains_between(fn, fn_end, poisoned_scalar_decl),
                 "poisoned legacy scalar spelling is unreachable");
    TEST_REQUIRE(contains(buf, "immutable-authority") && !contains(buf, "forged-live-xi"),
                 "String literal CGen mechanically consumes immutable plan-owned bytes");
    TEST_REQUIRE(contains(buf, "xr_aot_channel_new(") && !contains(buf, "xr_aot_channel_new(NULL"),
                 "Channel CGen mechanically consumes the immutable recipe despite poisoned Xi rep");
    char receive_assign[96];
    snprintf(receive_assign, sizeof(receive_assign),
             "v%u = XR_TO_INT(xr_aot_bridge_value_to_xrt(xr_aot_recv_payload(",
             (unsigned) receive->id);
    TEST_REQUIRE(contains(buf, receive_assign),
                 "Channel receive CGen mechanically consumes the immutable unbox recipe");
    emission_plans[0] = emission_plan;

    xi_cgen_ctx_set_aot_bundle(ctx, &legacy_plan.bundle);
    TEST_REQUIRE(xi_cgen_has_error(ctx),
                 "installed scalar registry seals the AOT bundle authority");

    XiCgenCtx *missing_install = xi_cgen_ctx_new();
    TEST_REQUIRE(missing_install != NULL, "missing-plan context allocated");
    xi_cgen_ctx_set_aot_bundle(missing_install, &legacy_plan.bundle);
    TEST_REQUIRE(!xi_cgen_ctx_set_value_emission_plans(missing_install, NULL, 1) &&
                     xi_cgen_has_error(missing_install),
                 "missing value emission plan fails closed");
    TEST_REQUIRE(!xi_cgen_ctx_set_value_emission_plans(missing_install, emission_plans, 1) &&
                     xi_cgen_has_error(missing_install),
                 "sticky registry error cannot be cleared by a later valid install");
    xi_cgen_ctx_free(missing_install);

    XiCgenCtx *unregistered = xi_cgen_ctx_new();
    TEST_REQUIRE(unregistered != NULL, "unregistered context allocated");
    xi_cgen_ctx_set_aot_bundle(unregistered, &legacy_plan.bundle);
    char *missing_buf = NULL;
    size_t missing_bufsz = 0;
    FILE *missing_mem = xr_open_memstream(&missing_buf, &missing_bufsz);
    TEST_REQUIRE(missing_mem != NULL, "missing-plan stream opened");
    xi_cgen_program(unregistered, missing_mem, module);
    TEST_REQUIRE(xr_close_memstream(missing_mem, &missing_buf, &missing_bufsz) == 0,
                 "missing-plan stream closed");
    TEST_REQUIRE(xi_cgen_has_error(unregistered),
                 "frozen scalar without registry fails closed despite injected residue");

    xi_cgen_ctx_free(unregistered);
    xr_free(missing_buf);
    xr_free(buf);
    xi_cgen_ctx_free(ctx);
    xr_c_emission_plan_free(emission_plan);
    xr_target_plan_free(target_plan);
    xr_target_profile_free(profile);
    test_aot_plan_free(&legacy_plan);
    module->init = NULL;
    xi_module_free(module);
    xi_func_free(ir);
}

TEST(cgen_struct_fixed_array_index_keeps_required_constant_local) {
    const char *src = "struct Lanes { data: [u64; 4] }\n"
                      "@noinline\n"
                      "fn pick(view: ref Lanes) -> u64 {\n"
                      "    return view.data[1]\n"
                      "}\n"
                      "var lanes = Lanes{data: [1, 2, 3, 4]}\n"
                      "print(pick(ref lanes))\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "struct fixed-array index IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "struct fixed-array index C generation failed");
    TEST_REQUIRE(!had_error, "struct fixed-array index fixture should generate");
    const char *pick = find_static_function_definition(code, "pick_");
    TEST_REQUIRE(pick != NULL, "pick definition should be emitted");
    const char *pick_end = next_static_after(pick);
    TEST_REQUIRE(count_between(pick, pick_end, " = INT64_C(1);") == 1,
                 "struct fixed-array index must retain its ctx-less constant local");
    TEST_REQUIRE(contains_between(pick, pick_end, "->data[v"),
                 "struct fixed-array field access must reference the retained index local");

    printf("  Preserved struct fixed-array index local in %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_skips_unused_process_builtin_init) {
    const char *src = "print(1 + 2)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "simple AOT program should generate");
    assert(!contains(code, "xrt_process_new(") &&
           "AOT entry must not construct process.args when process is unused");
    assert(!contains(code, "xrt_builtins[5] =") &&
           "unused process builtin slot must not be initialized");

    printf("  Generated process-free entry %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_initializes_used_process_builtin) {
    const char *src = "print(len(process.args))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "process.args AOT program should generate");
    assert(contains(code, "xrt_builtins[5] = xrt_process_new(") &&
           "used process builtin slot must be initialized");
    assert(contains(code, "xrt_process_new(\"test\",") &&
           "process builtin must receive the entry source path");
    assert(contains(code, "argc > 1 ? argc - 1 : 0") &&
           "process.args must still receive script arguments");

    printf("  Generated process-aware entry %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_initializes_file_dir_builtins_from_entry_source) {
    const char *src = "print(__file__)\n"
                      "print(__dir__)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");
    assert(ir->module != NULL && "lowering should attach module metadata");
    ir->module->path = "/tmp/xray/main.xr";

    bool had_error = false;
    char *code = generate_c_with_status(ir, "main", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "__file__/__dir__ AOT program should generate");
    assert(contains(code, "xrt_builtins[6] = xr_box_str(\"/tmp/xray/main.xr\")") &&
           "__file__ must be initialized from the entry source path");
    assert(contains(code, "xrt_builtins[7] = xr_box_str(\"/tmp/xray\")") &&
           "__dir__ must be initialized from the entry source directory");
    assert(!contains(code, "xray_vm_set_script_info") &&
           "AOT generated C must not route script info through VM isolate");

    printf("  Generated file/dir-aware entry %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_runtime_file_dir_stays_runtime_owned) {
    const char *src = "print(__file__)\n"
                      "print(__dir__)\n"
                      "Coro.yield()\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "main", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "runtime-backed __file__/__dir__ AOT program should generate");
    assert(contains(code, "runtime_cfg.file = \"main\";") &&
           "runtime-backed generated main must pass script info to XrAotRuntimeConfig");
    assert(contains(code, "xrt_builtins[6] = xr_box_str(\"main\")") &&
           "sync helpers may still initialize standalone __file__");
    assert(!contains(code, "xr_aot_runtime_set_builtin(rt, 6") &&
           "runtime-owned __file__ must not be overwritten with an xrt string");
    assert(!contains(code, "xr_aot_runtime_set_builtin(rt, 7") &&
           "runtime-owned __dir__ must not be overwritten with an xrt string");
    assert(!contains(code, "xray_vm_set_script_info") &&
           "AOT generated C must not route script info through VM isolate");

    printf("  Generated runtime-owned file/dir entry %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_standalone_prelude_enum_globals_generate_static_members) {
    const char *src = "fn sent(r: SendResult) -> bool {\n"
                      "    return match (r) {\n"
                      "        SendResult.Sent -> true\n"
                      "        _ -> false\n"
                      "    }\n"
                      "}\n"
                      "fn empty(r: Recv<i64>) -> bool {\n"
                      "    return match (r) {\n"
                      "        Recv.Empty -> true\n"
                      "        _ -> false\n"
                      "    }\n"
                      "}\n"
                      "print(sent(SendResult.Sent))\n"
                      "print(empty(Recv.Empty))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "standalone AOT prelude enum globals should generate");
    assert(contains(code, "xrt_map_new(4)") &&
           "standalone builtin enum globals should use lightweight enum maps");
    /* Which of the two representations a prelude enum gets is decided by
     * whether ANY of its variants carries a payload -- see
     * cg_mark_prelude_enum_scalar_sidecar. SendResult is payload-free
     * throughout and lowers to the scalar layout; Recv.Value carries one, so
     * Recv keeps the boxed form and its payload-free members retain nominal
     * enum identity in the lightweight enum map. */
    assert(contains(code, "xrt_enum_scalar_box(&_xprelude_enum_scalar_layout_") &&
           "a payload-free prelude enum lowers to the scalar layout");
    assert(!contains(code, "_ev_SendResult_") &&
           "a scalar-lowered enum must not also emit boxed member globals");
    assert(contains(code, "static const XrAotEnumBox _xenum_box_test_prelude_") &&
           "no-payload Recv members must have a module-scoped static box");
    assert(contains(code, "NULL, \"Recv\", \"Empty\", 1u, 0u,") &&
           "the static box must retain the Recv.Empty nominal identity");
    assert(contains(code, "xrt_enum_box_from_static(&_xenum_box_test_prelude_") &&
           "Recv.Empty uses must reference the immortal static box");
    assert(!contains(code, "xr_aot_load_builtin_field(ctx,") &&
           "standalone AOT must not require a coroutine isolate for prelude enum fields");

    printf("  Generated standalone prelude enum globals %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_cancelled_builtin_generates_false) {
    const char *src = "print(cancelled())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT cancelled() should generate");
    assert(contains(code, "XR_FROM_BOOL(false)") && "cancelled() currently lowers to false");
    assert(!contains(code, "unknown builtin") &&
           "cancelled() must not be treated as a named builtin");

    printf("  Generated cancelled() builtin %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_variable_and_print) {
    /* Variable assignment and print */
    const char *src = "var x = 42\n"
                      "print(x)\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) {
        printf("  SKIP\n");
        return;
    }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    /* Should contain the constant 42 */
    assert(contains(code, "42") && "should contain constant 42");
    assert(contains(code, "printf(\"%lld\", (long long)") && contains(code, "putchar('\\n')") &&
           "native i64 print should use direct printf/putchar");
    assert(!contains(code, "xrt_println(") && !contains(code, "xrt_print(") &&
           "native i64 print should not call the generic tagged printer");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_if_else) {
    /* Conditional control flow */
    const char *src = "var x = 10\n"
                      "if (x > 5) {\n"
                      "    print(1)\n"
                      "} else {\n"
                      "    print(0)\n"
                      "}\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) {
        printf("  SKIP\n");
        return;
    }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    /* Should contain goto (blocks) and if */
    assert(contains(code, "goto L") && "should have goto for blocks");
    assert(contains(code, "if (") && "should have if branch");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_ordinary_bool_control_has_no_probability_wrapper) {
    const char *src = "fn choose(flag: bool) -> i64 {\n"
                      "    if (flag) {\n"
                      "        return 1\n"
                      "    }\n"
                      "    return 0\n"
                      "}\n"
                      "print(choose(true))\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) {
        printf("  SKIP\n");
        return;
    }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    const char *choose = find_static_function_definition(code, "test_choose_");
    TEST_REQUIRE(choose != NULL, "ordinary bool function should be generated");
    const char *choose_end = next_static_after(choose);
    const char *internal_likely = strstr(choose, "XR_LIKELY(");
    const char *internal_unlikely = strstr(choose, "XR_UNLIKELY(");
    TEST_REQUIRE((!internal_likely || internal_likely >= choose_end) &&
                     (!internal_unlikely || internal_unlikely >= choose_end),
                 "ordinary bool control must remain probability-neutral");
    TEST_REQUIRE(count_op_in_func(ir, XI_COPY) == 0,
                 "ordinary bool control must not retain an identity wrapper");

    printf("  Generated ordinary bool control %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_multi_print) {
    /* Multiple print statements */
    const char *src = "var a = 10\n"
                      "var b = 20\n"
                      "var c = a + b\n"
                      "print(c)\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) {
        printf("  SKIP\n");
        return;
    }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    assert(contains(code, "10") && "should contain 10");
    assert(contains(code, "20") && "should contain 20");
    assert(contains(code, "+") && "should contain addition");
    assert(contains(code, "printf(\"%lld\", (long long)") &&
           "native i64 print should use direct printf");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_while_loop) {
    /* While loop generates blocks and back edges */
    const char *src = "fn hot() -> i64 {\n"
                      "    var i = 0\n"
                      "    while (i < 5) {\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    return i\n"
                      "}\n"
                      "print(hot())\n";

    XiPipelineConfig cfg = xi_pipeline_aot_config();
    XiFunc *ir = compile_to_ir_with_config(src, cfg);
    if (!ir) {
        printf("  SKIP\n");
        return;
    }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    /* Should have labels and gotos for loop */
    assert(contains(code, "goto L") && "should have goto for loop");
    /* Should have comparison */
    assert(contains(code, "<") && "should have comparison");
    const char *hot = find_static_function_definition(code, "hot_");
    TEST_REQUIRE(hot != NULL, "hot loop function should be emitted");
    const char *hot_end = next_static_after(hot);
    TEST_REQUIRE(contains_between(hot, hot_end, "while (") &&
                     contains_between(hot, hot_end, "INT64_C(5)"),
                 "structured loop guard must retain its exact literal condition");
    TEST_REQUIRE(count_lines_outside_debug_locals_with_prefix(hot, hot_end, "    int64_t v",
                                                              " = INT64_C(5);") == 0 &&
                     count_lines_outside_debug_locals_with_prefix(hot, hot_end, "    v",
                                                                  " = INT64_C(5);") == 0,
                 "an inlined structured-loop bound must not leave a release C assignment");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_string_literal) {
    const char *src = "print(\"hello world\")";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) {
        printf("  SKIP\n");
        return;
    }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    assert(contains(code, "hello world") && "should contain string literal");
    assert(contains(code, "static const xrt_str_t _xstr_") &&
           "literal should have a static header with precomputed hash");
    assert(contains(code, "xr_str_lit(&_xstr_") && "use site should reference the static header");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_str_concat_uses_single_allocation_helper) {
#define CHECK_CGEN_STR_CONCAT(cond, msg)                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL: %s\n", (msg));                                                \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    const char *src = "enum Kind { Ready }\n"
                      "var s = \"a\" + string(1) + \"b\" + string(2) + true + null + Kind.Ready\n"
                      "print(s)\n"
                      "print(\"q\" + string(3))\n"
                      "print(\"${42}\")\n";

    XiFunc *ir = compile_to_ir(src);
    CHECK_CGEN_STR_CONCAT(ir != NULL, "IR compilation failed");

    char *code = generate_c(ir, "test");
    CHECK_CGEN_STR_CONCAT(code != NULL, "C code generation failed");

    size_t code_len = strlen(code);
    const char *code_end = code + code_len;
    size_t add_calls = count_between(code, code_end, "xrt_add(");
    size_t strbuf_new_calls = count_between(code, code_end, "xrt_strbuf_new()");
    size_t part_init_calls = count_between(code, code_end, "xrt_strpart_init(");
    size_t concat_parts_calls = count_between(code, code_end, "xrt_str_concat_parts(");
    CHECK_CGEN_STR_CONCAT(add_calls == 0 && !contains(code, "xrt_add("),
                          "AOT STR_CONCAT must not lower to nested binary xrt_add calls");
    CHECK_CGEN_STR_CONCAT(strbuf_new_calls == 0,
                          "multi-part concat sites should not allocate temporary StringBuilder");
    CHECK_CGEN_STR_CONCAT(part_init_calls >= 8,
                          "AOT STR_CONCAT should materialize every part exactly once");
    CHECK_CGEN_STR_CONCAT(concat_parts_calls >= 2,
                          "AOT STR_CONCAT should use the single-allocation concat helper");

    printf("  Generated single-allocation string concat %zu bytes of C code "
           "(add=%zu strbuf_new=%zu parts=%zu concat=%zu)\n",
           code_len, add_calls, strbuf_new_calls, part_init_calls, concat_parts_calls);
    xr_free(code);
    xi_func_free(ir);

#undef CHECK_CGEN_STR_CONCAT
}

TEST(cgen_string_concat_cleanup_consumes_immutable_emission) {
    const char *src = "fn consume() {\n"
                      "    var value = \"left\" + \"right\"\n"
                      "    print(value)\n"
                      "}\n"
                      "consume()\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "String concat cleanup fixture should compile");
    bool had_error = false;
    char *code = generate_c_with_status(ir, "cleanup", &had_error);
    TEST_REQUIRE(code != NULL && !had_error,
                 "String concat cleanup must consume frozen cleanup authority");
    TEST_REQUIRE(contains(code, "xrt_str_concat_parts("),
                 "String concat must consume its immutable allocation recipe");
    TEST_REQUIRE(contains(code, "xrt_release("),
                 "String concat owner must consume its immutable release recipe");
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_function_call) {
    /* Function definition and call */
    const char *src = "fn add(a: i64, b: i64) -> i64 { return a + b }\n"
                      "var r = add(3, 4)\n"
                      "print(r)\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) {
        printf("  SKIP\n");
        return;
    }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    /* Should contain a child function for 'add' */
    assert(contains(code, "add") && "should have add function");
    assert(contains(code, "static XrValue") && "should have static funcs");
    assert(contains(code, "return") && "add should have return");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_canonical_generic_function_body_is_executable) {
    const char *src = "fn keep<T>(value: T) -> T { return value }\n"
                      "print(keep(41))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "generic function IR compilation failed");
    XiFunc *keep = NULL;
    for (uint16_t i = 0; i < ir->nchildren; i++) {
        if (ir->children[i] && ir->children[i]->name &&
            strcmp(ir->children[i]->name, "keep") == 0) {
            keep = ir->children[i];
            break;
        }
    }
    assert(keep != NULL && "canonical generic function body must be lowered");
    assert(!keep->is_generic_template &&
           "a function's own type parameters must retain the canonical erased ABI");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && !had_error && "canonical generic body should generate");
    assert(contains(code, "keep") &&
           "reachable canonical generic body must not be pruned as an open owner template");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_plain_function_does_not_emit_public_c_abi_wrapper) {
    const char *src = "fn add(a: i32, b: i32) -> i32 {\n"
                      "    return a + b\n"
                      "}\n"
                      "print(add(3, 4))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "plain scalar function should generate");
    assert(!contains(code, "\nint32_t xr_add(int32_t p0, int32_t p1);") &&
           "plain source functions must not create a public C ABI wrapper");
    assert(contains(code, "test_add_") && "the internal Xray implementation must remain available");

    printf("  Generated private function %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_multimodule_private_helpers_are_file_local_inline) {
    const char *lib_src = "var bias: [i64; 1] = [1]\n"
                          "fn helper(x: i64) -> i64 {\n"
                          "    return x + 1\n"
                          "}\n"
                          "export fn public(x: i64) -> i64 {\n"
                          "    return helper(x) + bias[0]\n"
                          "}\n"
                          "public(0)\n";
    const char *app_src = "print(0)\n";

    XiFunc *lib_ir = compile_to_ir(lib_src);
    XiFunc *app_ir = compile_to_ir(app_src);
    assert(lib_ir != NULL && app_ir != NULL && "IR compilation failed");
    assert(lib_ir->module != NULL && app_ir->module != NULL && "module metadata required");
    lib_ir->module->name = "lib";
    lib_ir->module->path = "lib.xr";
    app_ir->module->name = "app";
    app_ir->module->path = "app.xr";
    XiModule *modules[] = {lib_ir->module, app_ir->module};

    assert(test_prepare_backend_ir(lib_ir));
    assert(test_prepare_backend_ir(app_ir));

    TestAotPlan plan;
    test_aot_plan_prepare(&plan, modules, 2, 1);

    XiCgenCtx *ctx = xi_cgen_ctx_new();
    assert(ctx != NULL);
    xi_cgen_ctx_set_aot_bundle(ctx, &plan.bundle);
    xi_cgen_resolve_module_imports(ctx, modules, 2);

    char *buf = NULL;
    size_t bufsz = 0;
    FILE *mem = xr_open_memstream(&buf, &bufsz);
    assert(mem != NULL);
    xi_cgen_module_tu(ctx, mem, modules, 2, 0, 1);
    int rc = xr_close_memstream(mem, &buf, &bufsz);
    assert(rc == 0);
    assert(!xi_cgen_has_error(ctx) && "multi-module C generation should succeed");

    assert((contains(buf, "\nstatic XR_AINLINE int64_t lib_helper_") ||
            contains(buf, "\nstatic XR_AINLINE XRT_FN_CONST int64_t lib_helper_")) &&
           "private helper should be file-local and inlineable in multi-module C");
    assert((contains(buf, "\nXRT_INTERNAL int64_t lib_public_exp(") ||
            contains(buf, "\nXRT_INTERNAL XRT_FN_CONST int64_t lib_public_exp(") ||
            contains(buf, "\nXRT_INTERNAL XR_FORCEINLINE int64_t lib_public_exp(") ||
            contains(buf, "\nXRT_INTERNAL XR_FORCEINLINE XRT_FN_CONST int64_t lib_public_exp(")) &&
           "module-ABI functions need an out-of-line definition on every C provider; the "
           "owning unit may add XR_FORCEINLINE while the cross-unit declaration strips it");
    assert(!contains(buf, "static XR_AINLINE int64_t lib_public_exp(") &&
           !contains(buf, "static XR_AINLINE XRT_FN_CONST int64_t lib_public_exp(") &&
           "exported function must not become file-static");
    assert(!contains(buf, "\nint64_t lib_helper_") &&
           "private helper must not keep external linkage");

    printf("  Generated multi-module TU with private inline helper %zu bytes of C code\n",
           strlen(buf));
    xr_free(buf);
    xi_cgen_ctx_free(ctx);
    test_aot_plan_free(&plan);
    xi_func_free(lib_ir);
    xi_func_free(app_ir);
}

TEST(cgen_multimodule_branching_dispatcher_defers_to_native_inliner) {
    const char *lib_src = "fn path0(value: i64) -> i64 { return value + 1 }\n"
                          "fn path1(value: i64) -> i64 { return value + 2 }\n"
                          "fn path2(value: i64) -> i64 { return value + 3 }\n"
                          "export fn dispatch(tag: i64, value: i64) -> i64 {\n"
                          "    var mixed = value ^ (tag << 1)\n"
                          "    mixed = (mixed + 7) ^ (mixed >> 3)\n"
                          "    mixed = mixed ^ tag\n"
                          "    if (tag == 0) { return path0(mixed) }\n"
                          "    if (tag == 1) { return path1(mixed) }\n"
                          "    return path2(mixed)\n"
                          "}\n"
                          "dispatch(0, 0)\n";
    const char *app_src = "print(0)\n";

    XiFunc *lib_ir = compile_to_ir(lib_src);
    XiFunc *app_ir = compile_to_ir(app_src);
    TEST_REQUIRE(lib_ir != NULL && app_ir != NULL, "IR compilation failed");
    TEST_REQUIRE(lib_ir->module != NULL && app_ir->module != NULL, "module metadata required");
    lib_ir->module->name = "lib";
    lib_ir->module->path = "lib.xr";
    app_ir->module->name = "app";
    app_ir->module->path = "app.xr";
    XiModule *modules[] = {lib_ir->module, app_ir->module};

    TEST_REQUIRE(test_prepare_backend_ir(lib_ir), "library backend preparation failed");
    TEST_REQUIRE(test_prepare_backend_ir(app_ir), "app backend preparation failed");

    TestAotPlan plan;
    test_aot_plan_prepare(&plan, modules, 2, 1);
    XiCgenCtx *ctx = xi_cgen_ctx_new();
    TEST_REQUIRE(ctx != NULL, "CGen context allocated");
    xi_cgen_ctx_set_aot_bundle(ctx, &plan.bundle);
    xi_cgen_resolve_module_imports(ctx, modules, 2);

    char *code = NULL;
    size_t code_size = 0;
    FILE *mem = xr_open_memstream(&code, &code_size);
    TEST_REQUIRE(mem != NULL, "CGen output stream allocated");
    xi_cgen_module_tu(ctx, mem, modules, 2, 0, 1);
    TEST_REQUIRE(xr_close_memstream(mem, &code, &code_size) == 0, "CGen output stream closed");
    TEST_REQUIRE(code != NULL && !xi_cgen_has_error(ctx), "multi-module C generation succeeded");
    TEST_REQUIRE(contains(code, "lib_dispatch_exp("), "dispatcher definition generated");
    TEST_REQUIRE(!contains(code, "XR_FORCEINLINE int64_t lib_dispatch_exp(") &&
                     !contains(code, "XR_FORCEINLINE XRT_FN_CONST int64_t lib_dispatch_exp("),
                 "three-way call dispatcher must not be forced into every caller");
    TEST_REQUIRE(contains(code, "XRT_INTERNAL int64_t lib_dispatch_exp(") ||
                     contains(code, "XRT_INTERNAL XRT_FN_CONST int64_t lib_dispatch_exp("),
                 "dispatcher keeps ordinary hidden cross-module linkage");

    printf("  Generated native-inliner dispatcher boundary %zu bytes of C code\n", code_size);
    xr_free(code);
    xi_cgen_ctx_free(ctx);
    test_aot_plan_free(&plan);
    xi_func_free(lib_ir);
    xi_func_free(app_ir);
}

TEST(cgen_noinline_attribute_preserves_native_boundary) {
    const char *src = "@noinline\n"
                      "fn boundary(value: i64) -> i64 { return value + 1 }\n"
                      "print(boundary(41))\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");
    XiFunc *boundary = NULL;
    for (uint16_t i = 0; i < ir->nchildren; i++) {
        if (ir->children[i] && ir->children[i]->name &&
            strcmp(ir->children[i]->name, "boundary") == 0) {
            boundary = ir->children[i];
            break;
        }
    }
    TEST_REQUIRE(boundary != NULL && boundary->inline_policy == XI_INLINE_PRESERVE_CALL,
                 "@noinline must survive AST-to-Xi lowering");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "@noinline fixture should generate");
    TEST_REQUIRE(contains(code, "static XR_NOINLINE int64_t test_boundary_") ||
                     contains(code, "static XR_NOINLINE XRT_FN_CONST int64_t test_boundary_"),
                 "@noinline must emit a native noinline function boundary");

    printf("  Generated explicit noinline boundary %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_inline_attribute_forces_native_expansion) {
    const char *src = "@inline\n"
                      "fn expansion(value: i64) -> i64 {\n"
                      "    var scratch: [i64; 4] = [value, 2, 3, 4]\n"
                      "    var result = value\n"
                      "    var i = 0\n"
                      "    while (i < 64) {\n"
                      "        result = (result * 33) ^ i\n"
                      "        i += 1\n"
                      "    }\n"
                      "    return result + scratch[0]\n"
                      "}\n"
                      "print(expansion(7))\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");
    XiFunc *expansion = NULL;
    for (uint16_t i = 0; i < ir->nchildren; i++) {
        if (ir->children[i] && ir->children[i]->name &&
            strcmp(ir->children[i]->name, "expansion") == 0) {
            expansion = ir->children[i];
            break;
        }
    }
    TEST_REQUIRE(expansion != NULL && expansion->inline_policy == XI_INLINE_PREFER,
                 "@inline must survive AST-to-Xi lowering");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "@inline fixture should generate");
    TEST_REQUIRE(contains(code, "static XR_AINLINE int64_t test_expansion_") ||
                     contains(code, "static XR_AINLINE XRT_FN_CONST int64_t test_expansion_"),
                 "@inline must emit an always-inline native expansion boundary");

    printf("  Generated explicit inline expansion %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_c_export_wrapper_keeps_default_visibility) {
    const char *src = "fn bridge(x: i64) -> i64 { return x + 1 }\n"
                      "bridge(0)\n";
    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    XiFunc *bridge = NULL;
    for (uint16_t i = 0; i < ir->nchildren; i++) {
        if (ir->children[i] && ir->children[i]->name &&
            strcmp(ir->children[i]->name, "bridge") == 0) {
            bridge = ir->children[i];
            break;
        }
    }
    assert(bridge != NULL && "bridge function should be lowered");
    XrCExportPlan export_plan = {
        .xray_name = "bridge",
        .symbol = "xr_bridge_visible",
        .visibility = "default",
        .header = true,
    };
    bridge->export_plan = &export_plan;

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && !had_error && "C-export wrapper should generate");
    assert(contains(code, "\nXR_EXPORT_SYM int64_t xr_bridge_visible(int64_t p0)") &&
           "the public C-export wrapper must carry explicit external visibility");
    assert(!contains(code, "XRT_INTERNAL int64_t xr_bridge_visible(") &&
           !contains(code, "XRT_INTERNAL XR_FORCEINLINE int64_t xr_bridge_visible(") &&
           "internal implementation visibility must never leak onto the public wrapper");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_stats_tracks_native_abi) {
    const char *src = "fn inc(x: i64) -> i64 { return x + 1 }\n"
                      "print(inc(41))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    XiCgenStats stats = {0};
    char *code = generate_c_with_status_and_cgen_stats(ir, "test", &had_error, &stats);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "stats smoke program should generate");
    assert(stats.functions_total >= 2 && "module init plus static function should be counted");
    assert(stats.functions_native_abi >= 1 && "inc should use native scalar ABI");
    assert(stats.functions_tagged_abi >= 1 && "module init remains a tagged boundary");
    assert(stats.boxed_adapters == 0 &&
           "direct-only native function should not expose a boxed adapter");
    assert(!contains(code, "xrt_closure_new(&_xr_callable_") &&
           "direct-only shared function should not allocate a runtime closure");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_module_prefix_is_c_identifier) {
    const char *src = "fn compute(n: i64) -> i64 { return n * n }\n"
                      "print(compute(3))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "1127_coro_numeric_prefix", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "numeric module prefixes should generate");
    assert(contains(code, "_1127_coro_numeric_prefix_compute_") &&
           "numeric module prefixes must be emitted as legal C identifiers");
    assert(!contains(code, " 1127_coro_numeric_prefix_compute_") &&
           "numeric module prefixes must not be emitted raw");

    printf("  Generated numeric-prefix module %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_emits_source_line_directives) {
    const char *src = "print(1)\n"
                      "print(2)\n"
                      "print(3)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");
    assert(ir->module != NULL && "pipeline should produce module metadata");
    ir->module->path = "debug_map.xr";

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "source-line directive test should generate");
    assert(contains(code, "#line 1 \"debug_map.xr\"") &&
           "first source statement should be mapped to the module path");
    assert(contains(code, "#line 2 \"debug_map.xr\"") &&
           "second source statement should be mapped to the module path");
    assert(contains(code, "#line 3 \"debug_map.xr\"") &&
           "print statement should be mapped to the module path");

    printf("  Generated source-line mapped C %zu bytes\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_emits_debug_source_var_slots) {
    const char *src = "fn compute(seed: i64) -> i64 {\n"
                      "    var answer = seed + 1\n"
                      "    var doubled = answer * 2\n"
                      "    var ratio = (doubled as f64) / 2.0\n"
                      "    var ok = ratio == 21.0\n"
                      "    if (!ok) { return 0 }\n"
                      "    return doubled\n"
                      "}\n"
                      "print(compute(20))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "debug source-var slot test should generate");
    assert(contains(code, "#if defined(XRAY_AOT_DEBUG_LOCALS)") &&
           "debug source locals should be guarded by the debug-local define");
    assert(contains(code, "int64_t seed = 0;") && "source parameter should get a debug local");
    assert(contains(code, "int64_t answer = 0;") && "source local should get a debug local");
    assert(contains(code, "int64_t doubled = 0;") &&
           "second source local should get a debug local");
    assert(contains(code, "double ratio = 0;") && "f64 source local should get a debug local");
    assert(contains(code, "uint8_t ok = 0;") && "bool source local should get a debug local");
    assert(contains(code, "seed = (int64_t)") && "source parameter should be synchronized");
    assert(contains(code, "answer = (int64_t)") && "answer should be synchronized");
    assert(contains(code, "doubled = (int64_t)") && "doubled should be synchronized");
    assert(contains(code, "ratio = (double)") && "ratio should be synchronized");
    assert(contains(code, "ok = (uint8_t)") && "ok should be synchronized");

    printf("  Generated debug source-var mapped C %zu bytes\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_emits_shadowed_debug_source_var_slots) {
    const char *src = "fn compute(seed: i64) -> i64 {\n"
                      "    var answer = seed + 1\n"
                      "    if (answer > 0) {\n"
                      "        var shadowSeed = seed + 10\n"
                      "        var answer = shadowSeed\n"
                      "        print(answer)\n"
                      "    }\n"
                      "    return answer\n"
                      "}\n"
                      "print(compute(20))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "shadowed debug source-var slot test should generate");
    assert(contains(code, "int64_t answer = 0;") &&
           "first source variable with a repeated name should keep its source name");
    assert(contains(code, "int64_t _xray_dbg_shadow_1_answer = 0;") &&
           "shadowed source variable should get a stable debug fallback name");
    assert(contains(code, "answer = (int64_t)") && "outer answer should be synchronized");
    assert(contains(code, "_xray_dbg_shadow_1_answer = (int64_t)") &&
           "shadowed answer should be synchronized");

    printf("  Generated shadowed debug source-var mapped C %zu bytes\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_struct_debug_source_var_slots_use_typed_pointers) {
    const char *src = "struct Point {\n"
                      "    x: i32\n"
                      "    y: i32\n"
                      "}\n"
                      "fn make(seed: i32) -> Point {\n"
                      "    if (seed < 0) { return Point{x: 0, y: 0} }\n"
                      "    var p = Point{x: seed + 1, y: seed + 2}\n"
                      "    var q = p\n"
                      "    return q\n"
                      "}\n"
                      "var out = make(20)\n"
                      "print(out.x)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "struct debug source-var slot test should generate");
    assert(contains(code, "#if defined(XRAY_AOT_DEBUG_LOCALS)") &&
           "struct debug source locals should be guarded by the debug-local define");
    assert(contains(code, "xrt_struct_abi_") &&
           "value structs should emit canonical native storage types");
    assert(contains(code, " p = ((xrt_struct_abi_") &&
           "value-ABI struct source local p should use a native aggregate slot");
    assert(contains(code, " q = ((xrt_struct_abi_") &&
           "value-ABI struct source local q should use a native aggregate slot");
    assert(contains(code, "\n    p = v") &&
           "value-ABI struct source local p should synchronize from the native aggregate");
    assert(contains(code, "\n    q = v") &&
           "value-ABI struct source local q should synchronize from the native aggregate");
    assert(!contains(code, "XrValue p = XR_NULL_VAL;") &&
           "struct source local p should not degrade to an opaque XrValue debug slot");
    assert(!contains(code, "XrValue q = XR_NULL_VAL;") &&
           "struct source local q should not degrade to an opaque XrValue debug slot");

    printf("  Generated struct debug source-var mapped C %zu bytes\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_local_value_struct_copy_consumes_named_aggregate_emission) {
    const char *src = "struct Pair {\n"
                      "    low: u64\n"
                      "    high: u64\n"
                      "}\n"
                      "fn localCopy(seed: u64) -> u64 {\n"
                      "    var original = Pair{low: seed + 1, high: seed + 2}\n"
                      "    var changed = original\n"
                      "    changed.low += 7\n"
                      "    changed.high ^= 3\n"
                      "    return original.low + changed.low + changed.high\n"
                      "}\n"
                      "print(localCopy(11))\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "local_struct_copy", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "local value-struct copy should generate");
    const char *copy = find_static_function_definition(code, "localCopy");
    TEST_REQUIRE(copy != NULL, "localCopy definition should be emitted");
    const char *copy_end = next_static_after(copy);
    TEST_REQUIRE(contains_between(copy, copy_end, " = ((xrt_struct_abi_"),
                 "AGG_NEW must use the verified named-aggregate C type");
    TEST_REQUIRE(!contains_between(copy, copy_end, "({"),
                 "named aggregates must not use GNU statement expressions");
    TEST_REQUIRE(!contains_between(copy, copy_end, "xrt_arc_alloc("),
                 "native POD struct construction must not allocate");
    TEST_REQUIRE(!contains_between(copy, copy_end, "xrt_value_clone_for_coro("),
                 "native POD struct copy must use C assignment");
    TEST_REQUIRE(!contains_between(copy, copy_end, ".ptr"),
                 "native POD struct fields must not use tagged payload access");
    TEST_REQUIRE(!contains_between(copy, copy_end, "xrt_release("),
                 "native POD struct locals must not receive tagged cleanup");

    printf("  Generated immutable named-aggregate copy %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_struct_field_only_place_loads_are_debug_guarded) {
    const char *src = "struct Pair {\n"
                      "    left: i64\n"
                      "    right: i64\n"
                      "    sum() -> i64 {\n"
                      "        return this.left + this.right\n"
                      "    }\n"
                      "}\n"
                      "var pair = Pair{left: 20, right: 22}\n"
                      "print(pair.sum())\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");
    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "field-only struct place-load test should generate");
    const char *sum = find_static_function_definition(code, "sum_");
    TEST_REQUIRE(sum != NULL, "struct method definition should be emitted");
    const char *sum_end = next_static_after(sum);
    TEST_REQUIRE(count_between(sum, sum_end, " = (*(xrt_struct_abi_") > 0,
                 "debug build should retain a whole-struct source local materialization");
    TEST_REQUIRE(count_lines_outside_debug_locals(sum, sum_end, " = (*(xrt_struct_abi_") == 0,
                 "field-only whole-struct loads must not survive outside debug-local guards");
    TEST_REQUIRE(contains_between(sum, sum_end, ")).left") &&
                     contains_between(sum, sum_end, ")).right"),
                 "ordinary field consumers should address the original aggregate place directly");

    printf("  Generated debug-only aggregate place loads %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_struct_raw_deref_method_receiver_skips_release_copy) {
    const char *src = "struct State {\n"
                      "    value: u64\n"
                      "    tail: [u8; 4]\n"
                      "    reset(value: u64) {\n"
                      "        this.value = value\n"
                      "        this.tail = [1, 2, 3, 4]\n"
                      "    }\n"
                      "    digest() -> u64 {\n"
                      "        return this.value + this.tail[0] + this.tail[3]\n"
                      "    }\n"
                      "}\n"
                      "export fn digestState(source: Ptr<State>) -> u64 {\n"
                      "    unsafe { return source.deref().digest() }\n"
                      "}\n"
                      "export fn loadState(source: Ptr<State>) -> State {\n"
                      "    unsafe { return source.deref() }\n"
                      "}\n"
                      "var pointer = MutPtr<State>.null()\n"
                      "digestState(pointer)\n"
                      "var loaded = loadState(pointer)\n"
                      "print(loaded.value)\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "raw-deref struct receiver test should generate");
    const char *mutate = find_static_function_definition(code, "digestState");
    TEST_REQUIRE(mutate != NULL, "raw-deref receiver function should be emitted");
    const char *mutate_end = next_static_after(mutate);
    TEST_REQUIRE(count_lines_outside_debug_locals(mutate, mutate_end, " = (*(xrt_struct_abi_") == 0,
                 "raw-deref method receivers must not copy the whole struct in release C");
    TEST_REQUIRE(contains_between(mutate, mutate_end, "(xrt_struct_abi_"),
                 "raw-deref method receivers should address the original pointer target");

    const char *load = find_static_function_definition(code, "loadState");
    TEST_REQUIRE(load != NULL, "raw-deref value-load function should be emitted");
    const char *load_end = next_static_after(load);
    TEST_REQUIRE(count_lines_outside_debug_locals(load, load_end,
                                                  " = xrt_raw_scalar_access(xrt_struct_abi_") > 0,
                 "a raw dereference returned by value must still materialize the struct load");

    printf("  Generated release-zero-copy raw-deref struct receivers %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_struct_scalar_field_ref_skips_release_load) {
    const char *src = "fn bumpPair(left: ref i64, right: ref i64) {\n"
                      "    left += 1\n"
                      "    right += 2\n"
                      "}\n"
                      "struct Counter {\n"
                      "    left: i64\n"
                      "    right: i64\n"
                      "    advance() { bumpPair(ref this.left, ref this.right) }\n"
                      "}\n"
                      "var counter = Counter{left: 3, right: 5}\n"
                      "counter.advance()\n"
                      "print(counter.left + counter.right)\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "scalar aggregate-field ref test should generate");
    const char *advance = find_static_function_definition(code, "advance_");
    TEST_REQUIRE(advance != NULL, "struct advance method should be emitted");
    const char *advance_end = next_static_after(advance);
    TEST_REQUIRE(count_lines_outside_debug_locals_with_prefix(advance, advance_end, "    int64_t v",
                                                              " = (*(xrt_struct_abi_") == 0,
                 "direct scalar-field refs must not retain receiver or field loads in release C");
    TEST_REQUIRE(contains_between(advance, advance_end, "(int64_t *)(&(*(xrt_struct_abi_"),
                 "scalar-field ref arguments should address the original aggregate fields");

    printf("  Generated release-zero-load scalar aggregate refs %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_mem_slice_struct_pointer_owner_load_is_elided) {
    const char *src = "import mem\n"
                      "struct Holder {\n"
                      "    pointer: Ptr<u8>\n"
                      "    viewLength(size: i64) -> i64 {\n"
                      "        var bytes: const Slice<u8> = unsafe {\n"
                      "            mem.slice<u8>(this.pointer, size, this.pointer)\n"
                      "        }\n"
                      "        return len(bytes)\n"
                      "    }\n"
                      "}\n"
                      "var holder = Holder{pointer: Ptr<u8>.null()}\n"
                      "print(holder.viewLength(0))\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "mem.slice struct-owner test should generate");
    const char *view = find_static_function_definition(code, "viewLength_");
    TEST_REQUIRE(view != NULL, "Holder.viewLength method should be emitted");
    const char *view_end = next_static_after(view);
    TEST_REQUIRE(count_between(view, view_end, ")).pointer") == 1,
                 "mem.slice must load a struct pointer field only for its emitted pointer operand");
    TEST_REQUIRE(contains_between(view, view_end, "caller-proven mem.slice raw view"),
                 "the native mem.slice proof-preserving path should remain intact");

    printf("  Elided mem.slice lifetime-only pointer field load in %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_native_bool_assert_does_not_materialize_box) {
    const char *src = "fn checkRange(value: i64) {\n"
                      "    assert(value >= 0)\n"
                      "    assert(value < 16)\n"
                      "}\n"
                      "checkRange(7)\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "native bool assert test should generate");
    const char *check = find_static_function_definition(code, "checkRange");
    TEST_REQUIRE(check != NULL, "asserting function should be emitted");
    const char *check_end = next_static_after(check);
    TEST_REQUIRE(!contains_between(check, check_end, "XR_FROM_BOOL("),
                 "native bool assert conditions must not materialize tagged boxes");
    TEST_REQUIRE(count_between(check, check_end, "xrt_assertion_condition(") == 2,
                 "both assertions must consume the typed condition adapter");
    TEST_REQUIRE(!contains_between(check, check_end, "xrt_assert_condition_failed("),
                 "typed assertions must not retain the retired condition helper");
    TEST_REQUIRE(!contains_between(check, check_end, "({"),
                 "assert lowering must remain portable C11");

    printf("  Generated native unboxed assert conditions %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_assertion_calls_use_typed_adapters) {
    const char *src = "fn checkEquality(value: i64) {\n"
                      "    assertEqual(value, 7)\n"
                      "    assert(!(value == 8))\n"
                      "}\n"
                      "checkEquality(7)\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "assert equality owner test should generate");
    const char *check = find_static_function_definition(code, "checkEquality");
    TEST_REQUIRE(check != NULL, "assert equality function should be emitted");
    const char *check_end = next_static_after(check);
    TEST_REQUIRE(count_between(check, check_end, "xrt_assertion_equal(") == 1,
                 "assertEqual must consume the typed equality adapter");
    TEST_REQUIRE(count_between(check, check_end, "xrt_assertion_condition(") == 1,
                 "negated equality must remain an ordinary typed condition assertion");
    TEST_REQUIRE(!contains_between(check, check_end, "xrt_assert_condition_failed("),
                 "typed assertion calls must not retain the retired condition helper");

    printf("  Generated owner-backed assertion equality %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_span_phi_snapshot_is_debug_only) {
    const char *src =
        "import mem\n"
        "fn selectedLength(flag: bool, first: Slice<u8>, second: Slice<u8>, "
        "output: MutPtr<u8>) -> i64 {\n"
        "    var selected: const Slice<u8> = first\n"
        "    if (flag) { selected = second }\n"
        "    unsafe { mem.set(output, 0, 0) }\n"
        "    return len(selected)\n"
        "}\n"
        "var bytes = Array<u8>(2)\n"
        "unsafe { print(selectedLength(true, bytes[:], bytes[:], bytes.mutPtr())) }\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "span phi snapshot test should generate");
    const char *selected = find_static_function_definition(code, "selectedLength");
    TEST_REQUIRE(selected != NULL, "span-selection function should be emitted");
    const char *selected_end = next_static_after(selected);
    TEST_REQUIRE(contains_between(selected, selected_end, "xr_span_t phi"),
                 "branch-selected Slice should retain its native phi");
    TEST_REQUIRE(count_lines_outside_debug_locals_with_prefix(selected, selected_end,
                                                              "    xr_span_t v", " = phi") == 0,
                 "consumer-free Slice phi snapshots must not be copied in release C");
    TEST_REQUIRE(contains_between(selected, selected_end, "selected = phi"),
                 "debug source storage should still synchronize from the Slice phi");
    TEST_REQUIRE(contains_between(selected, selected_end, "xrt_has_pending_error()"),
                 "fixture must retain an ERR_CHECK cold edge with the Slice live");

    printf("  Generated debug-only Slice phi snapshots %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_span_ref_only_value_omits_unused_data_cache) {
    const char *src = "fn touch(view: ref Slice<u8>) {\n"
                      "    if (len(view) > 0) { view[0] = 7 }\n"
                      "}\n"
                      "fn allocate(size: i64) -> Array<u8> {\n"
                      "    var output = Array<u8>(size)\n"
                      "    var view: Slice<u8> = output[:]\n"
                      "    touch(ref view)\n"
                      "    return output\n"
                      "}\n"
                      "print(len(allocate(4)))\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "ref-only Slice cache test should generate");
    const char *allocate = find_static_function_definition(code, "allocate");
    TEST_REQUIRE(allocate != NULL, "Slice allocating function should be emitted");
    const char *allocate_end = next_static_after(allocate);
    TEST_REQUIRE(
        !contains_between(allocate, allocate_end, "_ad"),
        "a Slice used only through an addressable ref must not declare an unused data cache");
    TEST_REQUIRE(contains_between(allocate, allocate_end, "xr_span_t") &&
                     contains_between(allocate, allocate_end, "(xr_span_t *)(&"),
                 "the addressable Slice value itself must remain materialized");

    printf("  Omitted ref-only Slice data cache in %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_struct_value_abi_uses_canonical_layout_typedef) {
    const char *src =
        "struct Totals {\n"
        "    bytes: i64\n"
        "    checksum: i64\n"
        "}\n"
        "fn make(a: i64, b: i64) -> Totals {\n"
        "    return Totals{bytes: a, checksum: b}\n"
        "}\n"
        "fn combine(a: Totals, b: Totals) -> Totals {\n"
        "    return Totals{bytes: a.bytes + b.bytes, checksum: a.checksum + b.checksum}\n"
        "}\n"
        "fn run(n: i64) -> i64 {\n"
        "    var t = combine(make(n, 1), make(2, 3))\n"
        "    return t.bytes + t.checksum\n"
        "}\n"
        "print(run(10))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "struct value ABI should generate without adapter errors");
    assert(contains(code, "#ifndef XRT_TYPEDEF_xrt_struct_abi_") &&
           contains(code, "typedef struct xrt_struct_abi_") &&
           "native value struct typedef should use canonical layout identity");
    assert(contains(code, "static xrt_struct_abi_") &&
           "struct-returning helpers should use native value struct ABI");
    assert(contains(code, ", xrt_struct_abi_") &&
           "struct parameters should pass by value through the native ABI");
    assert(!contains(code, "xrt_struct_mod_") &&
           "child function ABI must not fall back to the generic mod prefix");

    printf("  Generated struct value ABI path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_emits_source_line_directives) {
    const char *src = "fn worker(n: i64) -> i64 {\n"
                      "    Coro.yield()\n"
                      "    return n + 1\n"
                      "}\n"
                      "var task = go worker(41)\n"
                      "var result = await task\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");
    assert(ir->module != NULL && "pipeline should produce module metadata");
    ir->module->path = "debug_coro.xr";

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "coroutine source-line directive test should generate");
    assert(contains(code, "_aot_resume") && "test source should emit a coroutine resume body");
    assert(contains(code, "#line 2 \"debug_coro.xr\"") &&
           "yield should be mapped inside the coroutine resume body");
    assert(contains(code, "#line 3 \"debug_coro.xr\"") &&
           "coroutine return should be mapped to the source return line");
    assert(contains(code, "#line 6 \"debug_coro.xr\"") &&
           "await should be mapped inside the entry coroutine resume body");
    assert(contains(code, "#line 7 \"debug_coro.xr\"") &&
           "post-await print should be mapped inside the entry coroutine resume body");
    assert(contains(code, "#line 2 \"debug_coro.xr\"\n"
                          "    return xr_aot_yielded();\n"
                          "#line 1 \"<xray-generated>\"") &&
           "yield should reset generated control flow after the source-mapped yield");
    assert(contains(code, "#line 5 \"debug_coro.xr\"\n"
                          "    xrt_guard_task_spawn();\n"
                          "    void *_child_frame_") &&
           "go and its fail-closed spawn guard should share the source operation mapping");
    assert(contains(code, ");\n"
                          "#line 1 \"<xray-generated>\"\n"
                          "    XrAotSpawnResult _spawn_") &&
           "go frame setup should reset before generated spawn bookkeeping");
    assert(contains(code, "#line 6 \"debug_coro.xr\"\n"
                          "    XrAotResult _await_") &&
           "await helper call should carry the await source line");
    assert(contains(code, "false);\n"
                          "#line 1 \"<xray-generated>\"\n"
                          "    if (_await_") &&
           "await helper should reset before generated blocked/error checks");

    printf("  Generated coroutine source-line mapped C %zu bytes\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_emits_debug_source_var_slots) {
    const char *src = "fn worker(seed: i64) -> i64 {\n"
                      "    var answer = seed + 1\n"
                      "    Coro.yield()\n"
                      "    var doubled = answer * 2\n"
                      "    var ratio = (doubled as f64) / 2.0\n"
                      "    var ok = ratio == 21.0\n"
                      "    if (!ok) { return 0 }\n"
                      "    return doubled\n"
                      "}\n"
                      "var task = go worker(20)\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");
    assert(ir->module != NULL && "pipeline should produce module metadata");
    ir->module->path = "debug_coro_locals.xr";

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "coroutine debug source-var slot test should generate");
    assert(contains(code, "_aot_resume") && "test source should emit a coroutine resume body");
    assert(contains(code, "#line 2 \"debug_coro_locals.xr\"") &&
           "pre-suspend local initializer should carry its source line");
    assert(contains(code, "#line 4 \"debug_coro_locals.xr\"") &&
           "post-resume local initializer should carry its source line");
    assert(contains(code, "#line 8 \"debug_coro_locals.xr\"") &&
           "coroutine return terminator should carry its source line");
    assert(contains(code, "#if defined(XRAY_AOT_DEBUG_LOCALS)") &&
           "coroutine debug source locals should be guarded by the debug-local define");
    assert(contains(code, "int64_t seed = 0;") &&
           "coroutine source parameter should get a debug local");
    assert(contains(code, "int64_t answer = 0;") &&
           "coroutine source local before suspension should get a debug local");
    assert(contains(code, "int64_t doubled = 0;") &&
           "coroutine source local after suspension should get a debug local");
    assert(contains(code, "double ratio = 0;") &&
           "coroutine f64 source local should get a debug local");
    assert(contains(code, "uint8_t ok = 0;") &&
           "coroutine bool source local should get a debug local");
    assert(contains(code, "seed = (int64_t)") &&
           "coroutine source parameter should be synchronized from the frame");
    assert(contains(code, "answer = (int64_t)") &&
           "coroutine source local should be synchronized after assignment/resume");
    assert(contains(code, "doubled = (int64_t)") &&
           "post-resume source local should be synchronized after assignment");
    assert(contains(code, "ratio = (double)") &&
           "coroutine f64 source local should be synchronized after assignment");
    assert(contains(code, "ok = (uint8_t)") &&
           "coroutine bool source local should be synchronized after assignment");

    printf("  Generated coroutine debug source-var mapped C %zu bytes\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_syncs_helper_result_debug_source_vars) {
    const char *src = "fn produce() -> i64 {\n"
                      "    return 41\n"
                      "}\n"
                      "fn worker(ch: Channel<i64>) -> i64 {\n"
                      "    var task = go produce()\n"
                      "    var result = await task\n"
                      "    ch.send(result)\n"
                      "    var received = ch.recv()\n"
                      "    return match (received) {\n"
                      "        Recv.Value(value) -> value + 1\n"
                      "        _ -> 0\n"
                      "    }\n"
                      "}\n"
                      "const ch: Channel<i64> = Channel(1)\n"
                      "var task = go worker(ch)\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");
    TEST_REQUIRE(ir->module != NULL, "pipeline should produce module metadata");
    ir->module->path = "debug_coro_helper_results.xr";

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "coroutine helper debug source-var test should generate");
    TEST_REQUIRE(contains(code, "_aot_resume"), "test source should emit coroutine resume bodies");
    TEST_REQUIRE(contains(code, "xr_aot_await_task"),
                 "test should exercise the await helper result path");
    TEST_REQUIRE(contains(code, "xr_aot_chan_recv_slot"),
                 "test should exercise the channel recv helper result path");
    TEST_REQUIRE(contains(code, "XrValue result = XR_NULL_VAL;"),
                 "await result source variable should get a debug local");
    TEST_REQUIRE(contains(code, "XrValue received = XR_NULL_VAL;"),
                 "recv result source variable should get a debug local");
    TEST_REQUIRE(contains(code, "\n    result = v"),
                 "await helper result should be synchronized into the source debug local");
    TEST_REQUIRE(contains(code, "\n    received = v"),
                 "recv helper result should be synchronized into the source debug local");

    printf("  Generated coroutine helper debug source-var mapped C %zu bytes\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_recursive) {
    /* Recursive function: factorial */
    const char *src = "fn fact(n: i64) -> i64 {\n"
                      "    if (n <= 1) { return 1 }\n"
                      "    return n * fact(n - 1)\n"
                      "}\n"
                      "print(fact(5))\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) {
        printf("  SKIP\n");
        return;
    }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    assert(contains(code, "fact") && "should have fact function");
    assert(contains(code, "if (") && "should have conditional");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_for_loop) {
    const char *src = "var sum = 0\n"
                      "for (var i = 1; i <= 10; i = i + 1) {\n"
                      "    sum = sum + i\n"
                      "}\n"
                      "print(sum)\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) {
        printf("  SKIP\n");
        return;
    }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    assert(contains(code, "goto L") && "should have goto for loop");
    assert(contains(code, "+") && "should have addition");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_parallel_for_each_uses_runtime_executor) {
    const char *src = "import parallel\n"
                      "fn run(n: i64) {\n"
                      "    const base = 10\n"
                      "    parallel.forEach(0..n, (i) -> {\n"
                      "        print(i + base)\n"
                      "    }, parallel.Options(2))\n"
                      "}\n"
                      "run(3)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL);

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    assert(contains(code, "xr_parallel_for_range_i64(") &&
           "parallel.forEach should lower to the AOT runtime executor");
    assert(contains(code, "_xr_par_workers_") && "workers expression should be evaluated once");
    assert(contains(code, "(XrParallelRangeI64Fn)") &&
           "runtime executor should receive the native range callback");
    assert(contains(code, "_par_range_") &&
           "parallel.forEach should emit a chunk range wrapper around the item body");
    assert(!contains(code, "xr_aot_spawn_child") &&
           "parallel.forEach must not fall back to per-iteration task spawning");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_parallel_map_into_scalar_lanes_use_direct_storage) {
    const char *src = "import parallel\n"
                      "const bias = 2\n"
                      "var floats: Array<f64> = []\n"
                      "floats.reserve(8)\n"
                      "parallel.mapInto(0..4, ref floats, (i) -> f64(i + bias) + 0.5, "
                      "parallel.Options(2))\n"
                      "var flags: Array<bool> = []\n"
                      "flags.reserve(8)\n"
                      "parallel.mapInto(0..4, ref flags, (i) -> ((i + bias) % 2) == 0, "
                      "parallel.Options(2))\n"
                      "fn byteValue() -> u8 { return 2 }\n"
                      "var bytes: Array<u8> = []\n"
                      "bytes.reserve(8)\n"
                      "parallel.mapInto(0..4, ref bytes, (i) -> byteValue(), parallel.Options(2))\n"
                      "print(len(floats))\n"
                      "print(len(flags))\n"
                      "print(len(bytes))\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "scalar parallel.mapInto should generate");

    const char *code_end = code + strlen(code);
    TEST_REQUIRE(count_between(code, code_end, "xr_parallel_for_range_i64(") == 3,
                 "scalar mapInto should use one range executor per map");
    TEST_REQUIRE(count_between(code, code_end, "((double*)") >= 1,
                 "Array<f64> mapInto should store through direct double storage");
    TEST_REQUIRE(count_between(code, code_end, "((uint8_t*)") >= 2,
                 "Array<bool>/Array<u8> mapInto should store through direct byte storage");
    TEST_REQUIRE(count_between(code, code_end, "xrt_array_write_preallocated(") == 0,
                 "scalar mapInto must not box through preallocated array writes");

    printf("  Generated scalar parallel.mapInto direct storage %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_parallel_map_return_uses_runtime_executor) {
    const char *src = "import parallel\n"
                      "var xs = parallel.map(0..8, (i) -> i + 1, parallel.Options(4))\n"
                      "print(len(xs))\n"
                      "print(xs[7])\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "returning parallel.map should generate");

    const char *code_end = code + strlen(code);
    TEST_REQUIRE(count_between(code, code_end, "xr_parallel_for_range_i64(") == 1,
                 "returning map should use one range executor");
    TEST_REQUIRE(count_between(code, code_end, "xrt_array_new_typed") >= 1,
                 "returning map should preallocate a result array before dispatch");
    TEST_REQUIRE(count_between(code, code_end, "xr_aot_spawn_child") == 0,
                 "returning map must not fall back to per-item task spawning");

    printf("  Generated returning parallel.map executor path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_parallel_reduce_uses_runtime_executor) {
    const char *src = "import parallel\n"
                      "fn run(n: i64) -> i64 {\n"
                      "    const base = 10\n"
                      "    return parallel.reduce(0..n, 0, (i) -> i + base, "
                      "(a, b) -> a + b, parallel.Options(2))\n"
                      "}\n"
                      "print(run(3))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL);
    assert(!had_error && "parallel.reduce AOT program should generate");

    assert(contains(code, "xr_parallel_reduce_i64(") &&
           "parallel.reduce should lower to the AOT runtime reducer");
    assert(contains(code, "_xr_pr_workers_") && "workers expression should be evaluated once");
    assert(contains(code, "(XrParallelReduceRangeI64Fn)") &&
           "runtime reducer should receive the native range callback");
    assert(contains(code, "(XrParallelReduceCombineI64Fn)") &&
           "runtime reducer should receive the native combine callback");
    assert(contains(code, "_par_reduce_range_") &&
           "parallel.reduce should emit a chunk range wrapper around the item body");
    assert(contains(code, "_par_reduce_combine_") &&
           "parallel.reduce should emit a native combine wrapper");
    assert(!contains(code, "xr_aot_spawn_child") &&
           "parallel.reduce must not fall back to per-iteration task spawning");
    assert(!contains(code, "xr_aot_await_all") &&
           "parallel.reduce must not route scalar aggregation through await all");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_parallel_reduce_struct_accumulator_uses_aggregate_runtime) {
    const char *src = "import parallel\n"
                      "struct Totals {\n"
                      "    bytes: i64\n"
                      "    checksum: i64\n"
                      "}\n"
                      "fn run(n: i64) -> i64 {\n"
                      "    var totals = parallel.reduce(0..n, Totals{bytes: 0, checksum: 0}, "
                      "(i) -> Totals{bytes: i, checksum: 1}, "
                      "(a, b) -> Totals{bytes: a.bytes + b.bytes, "
                      "checksum: a.checksum + b.checksum}, parallel.Options(2))\n"
                      "    return totals.bytes + totals.checksum\n"
                      "}\n"
                      "print(run(3))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL);
    assert(!had_error && "struct accumulator parallel.reduce should generate");
    assert(contains(code, "xr_parallel_reduce_agg(") &&
           "struct reduce should lower to the aggregate runtime reducer");
    assert(contains(code, "(XrParallelReduceRangeAggFn)") &&
           "aggregate reducer should receive the native range callback");
    assert(contains(code, "(XrParallelReduceCombineAggFn)") &&
           "aggregate reducer should receive the native combine callback");
    assert(contains(code, "static XR_AINLINE xrt_struct_abi_") &&
           "reduce body/combine should be inline native struct callbacks");
    assert(!contains(code, "xr_parallel_reduce_i64(") &&
           "struct reduce must not route through the i64 runtime reducer");
    assert(!contains(code, "_boxed") &&
           "direct aggregate reduce callbacks should not require boxed adapters");

    xr_free(code);
    xi_func_free(ir);
}

TEST(lower_parallel_call_plan_resolves_selective_aliases) {
    const char *src = "import { forEach as each, map as parMap, reduce as fold, "
                      "Options as ParOptions } from parallel\n"
                      "const total = Atomic(0)\n"
                      "each(0..4, (i) -> {\n"
                      "    total.fetchAdd(i, Ordering.Relaxed)\n"
                      "}, ParOptions(2))\n"
                      "var xs = parMap(0..4, (i) -> i + 1, ParOptions(2))\n"
                      "var sum = fold(0..4, 0, (i) -> i + 1, (a, b) -> a + b, "
                      "ParOptions(2))\n"
                      "print(total.load(Ordering.Relaxed))\n"
                      "print(xs[3])\n"
                      "print(sum)\n";

    XiFunc *ir = compile_to_ir_with_module_graph(src);
    TEST_REQUIRE(ir != NULL, "selective alias parallel call-plan source should lower to IR");

    TEST_REQUIRE(count_op_in_func(ir, XI_PAR_FOR) == 1,
                 "resolved forEach alias should lower to XI_PAR_FOR");
    TEST_REQUIRE(count_op_in_func(ir, XI_PAR_MAP) == 1,
                 "resolved map alias should lower to XI_PAR_MAP");
    TEST_REQUIRE(count_op_in_func(ir, XI_PAR_REDUCE) == 1,
                 "resolved reduce alias should lower to XI_PAR_REDUCE");
    TEST_REQUIRE(count_intrinsic_in_func(ir, XA_INTRINSIC_PARALLEL_FOR_EACH) == 1,
                 "forEach alias must preserve canonical identity on XI_PAR_FOR");
    TEST_REQUIRE(count_intrinsic_in_func(ir, XA_INTRINSIC_PARALLEL_MAP) == 1,
                 "map alias must preserve canonical identity on XI_PAR_MAP");
    TEST_REQUIRE(count_intrinsic_in_func(ir, XA_INTRINSIC_PARALLEL_REDUCE) == 1,
                 "reduce alias must preserve canonical identity on XI_PAR_REDUCE");

    xi_func_free(ir);
}

TEST(lower_parallel_plan_methods_preserve_intrinsic_identity) {
    const char *src = "import parallel\n"
                      "fn initLane(lane: i64) -> i64 { return lane }\n"
                      "var plan = parallel.Plan<i64>(parallel.Options(2), initLane)\n"
                      "plan.forEach(0..2, (state, i) -> {})\n"
                      "var mapped = plan.map(0..2, (state, i) -> state + i)\n"
                      "var out: Array<i64> = []\n"
                      "out.reserve(2)\n"
                      "plan.mapInto(0..2, ref out, (state, i) -> state + i)\n"
                      "var sum = plan.reduce(0..2, 0, (state, i) -> state + i, "
                      "(a, b) -> a + b)\n"
                      "print(len(mapped), len(out), sum)\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "parallel Plan methods should lower to IR");
    TEST_REQUIRE(count_intrinsic_in_func(ir, XA_INTRINSIC_PARALLEL_PLAN_FOR_EACH) == 1,
                 "Plan.forEach must preserve its canonical identity");
    TEST_REQUIRE(count_intrinsic_in_func(ir, XA_INTRINSIC_PARALLEL_PLAN_MAP) == 1,
                 "Plan.map must preserve its canonical identity");
    TEST_REQUIRE(count_intrinsic_in_func(ir, XA_INTRINSIC_PARALLEL_PLAN_MAP_INTO) == 1,
                 "Plan.mapInto must preserve its canonical identity on XI_PAR_MAP");
    TEST_REQUIRE(count_intrinsic_in_func(ir, XA_INTRINSIC_PARALLEL_PLAN_REDUCE) == 1,
                 "Plan.reduce must preserve its canonical identity");
    xi_func_free(ir);
}

TEST(cgen_parallel_for_each_allows_atomic_i64_direct_body) {
    const char *src = "import parallel\n"
                      "const total = Atomic(0)\n"
                      "fn run(n: i64) {\n"
                      "    parallel.forEach(0..n, (i) -> {\n"
                      "        total.fetchAdd(i, Ordering.Relaxed)\n"
                      "    }, parallel.Options(2))\n"
                      "}\n"
                      "run(3)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL);
    assert(count_op_in_func(ir, XI_ATOMIC_RMW) == 1 &&
           "Atomic.fetchAdd in a contextual parallel callback must be canonical before CGen");
    assert(count_op_in_func(ir, XI_CALL_METHOD) == 0 &&
           "parallel callback lowering must not leak an ordinary Atomic method call");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL);
    assert(!had_error && "parallel.forEach AOT body should allow direct Atomic<i64> RMW ops");
    assert(contains(code, "xr_parallel_for_range_i64(") &&
           "Atomic body should still use the AOT runtime executor");
    assert(contains(code, "atomic_fetch_add_explicit(") &&
           "Atomic<i64>.fetchAdd should keep the direct C11 atomic lowering");

    xr_free(code);
    xi_func_free(ir);
}

TEST(analyzer_parallel_for_each_rejects_throwing_body) {
    const char *src = "import parallel\n"
                      "fn run(n: i64) {\n"
                      "    parallel.forEach(0..n, (i) -> {\n"
                      "        assert(false)\n"
                      "    }, parallel.Options(2))\n"
                      "}\n"
                      "run(3)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir == NULL && "parallel callback effects must be rejected during semantic analysis");
}

TEST(cgen_parallel_for_body_closure_stack_allocates) {
    XrType unit_type = {
        .kind = XR_KIND_UNIT, .scalar_rep = XR_SCALAR_REP_NONE, .id = 906, .frozen = true};
    XrType int_type = {.kind = XR_KIND_INT, .id = 907, .frozen = true};
    XrType func_type = {.kind = XR_KIND_FUNCTION, .id = 908, .frozen = true};
    XrFunctionParam func_params[2] = {
        {.type = &int_type, .mode = XR_PARAM_READ},
        {.type = &int_type, .mode = XR_PARAM_READ},
    };
    func_type.function.params = func_params;
    func_type.function.param_count = 2;
    func_type.function.min_params = 2;
    func_type.function.return_type = &unit_type;

    XiFunc *ir = xi_func_new("manual_parfor_stack_closure", &unit_type);
    assert(ir != NULL);
    XiBlock *entry = xi_block_new(ir);
    entry->sealed = true;

    XiValue *captured = xi_const_int(ir, entry, 7, &int_type);
    XiValue *start = xi_const_int(ir, entry, 0, &int_type);
    XiValue *end = xi_const_int(ir, entry, 8, &int_type);
    XiValue *workers = xi_const_int(ir, entry, 2, &int_type);

    XiFunc *child = xi_func_new("manual_parfor_stack_child", &unit_type);
    assert(child != NULL);
    child->parent_func = ir;
    child->native_callback_kind = XI_NATIVE_CALLBACK_PAR_FOR_I64;
    child->nparams = 2;
    child->min_params = 2;
    child->params = (XiValue **) xr_calloc(2, sizeof(XiValue *));
    assert(child->params != NULL);

    XiBlock *child_entry = xi_block_new(child);
    child_entry->sealed = true;
    child->params[0] = xi_param(child, child_entry, 0, &int_type);
    child->params[1] = xi_param(child, child_entry, 1, &int_type);
    assert(child->params[0] != NULL && child->params[1] != NULL);
    child->captures[0] = (XiCapture) {
        .source = XI_CAPTURE_SRC_REG,
        .capture_kind = XI_CAPTURE_BY_COPY,
        .needs_cell = false,
        .type = &int_type,
        .value = captured,
        .name = "captured",
        .storage_domain = XR_STORAGE_EXEC_LOCAL,
        .value_capability = XA_CAP_CONST,
    };
    child->ncaptures = 1;
    xi_block_set_return(child_entry, NULL);

    ir->children = (XiFunc **) xr_calloc(1, sizeof(XiFunc *));
    assert(ir->children != NULL);
    ir->children[0] = child;
    ir->children_cap = 1;
    ir->nchildren = 1;

    XiValue *closure = xi_value_new(ir, entry, XI_CLOSURE_NEW, &func_type, 1);
    assert(closure != NULL);
    closure->aux = (void *) child;
    closure->aux_int = 0;
    closure->args[0] = captured;

    XiParallelForData *data =
        (XiParallelForData *) xi_func_arena_alloc(ir, (uint32_t) sizeof(XiParallelForData));
    assert(data != NULL);
    memset(data, 0, sizeof(*data));
    data->body_func = child;

    XiValue *par = xi_value_new(ir, entry, XI_PAR_FOR, &unit_type, 4);
    assert(par != NULL);
    par->args[0] = start;
    par->args[1] = end;
    par->args[2] = workers;
    par->args[3] = closure;
    par->aux = data;
    par->aux_kind = XI_AUX_KIND_PAR_FOR;
    xi_block_set_return(entry, NULL);

    xi_escape_analyze(ir);
    xi_stack_alloc_rewrite(ir);
    assert(closure->op == XI_STACK_ALLOC &&
           "XI_PAR_FOR body closure should be recognized as no-escape");
    xi_arc_insert(ir);
    xi_arc_elim(ir);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "XI_PAR_FOR stack closure should generate");
    assert(contains(code, "xr_parallel_for_range_i64(") &&
           "XI_PAR_FOR should still use the AOT runtime executor");
    assert(contains(code, "_xr_par_closure_storage_") &&
           "XI_PAR_FOR no-escape closure should use a scoped C closure env");
    assert(contains(code, "xrt_closure_init(_xr_par_closure_") &&
           "XI_PAR_FOR scoped closure env should initialize the runtime closure view");
    assert(!contains(code, "xrt_closure_stack_new(&_xr_callable_") &&
           "XI_PAR_FOR scoped closure env must not use function-lifetime alloca");
    assert(!contains(code, "xrt_closure_new(&_xr_callable_") &&
           "XI_PAR_FOR no-escape closure must not allocate a heap closure");

    printf("  Generated parallel-for stack closure path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_uses_raw_storage_fast_path) {
    const char *src = "fn sum() -> i64 {\n"
                      "    var values: Array<i64> = []\n"
                      "    values.push(41)\n"
                      "    return values[0] + len(values)\n"
                      "}\n"
                      "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed array fast path should generate");
    assert((contains(code, "xrt_array_new_typed(") || contains(code, "xrt_array_new_typed_ptr(")) &&
           "Array<i64> creation must preserve typed storage");
    assert(contains(code, "XR_ELEM_I64") && "Array<i64> must use the I64 typed element layout");
    assert(contains(code, "((int64_t*)_a->data)") &&
           "Array<i64> index reads and writes must access raw typed storage");
    assert((contains(code, "((xrt_array_t*)") || contains(code, "->len")) &&
           "len(Array<i64>) must read the runtime array length directly");
    assert(!contains(code, "xrt_method_1(") &&
           "Array<i64>.push must not fall back to dynamic method dispatch");
    assert(!contains(code, "xrt_getprop(") &&
           "len(Array<i64>) must not fall back to dynamic property dispatch");
    assert(!contains(code, "xrt_index_get(") &&
           "Array<i64> index read must not fall back to runtime index dispatch");

    printf("  Generated typed array fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_checked_typed_array_store_proves_nonnull_data) {
    const char *src = "fn put(values: ref Array<u64>, index: i64, value: u64) {\n"
                      "    values[index] = value\n"
                      "}\n"
                      "var values: Array<u64> = [1, 2]\n"
                      "put(ref values, 1, 9)\n"
                      "print(values[1])\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "checked typed-array store should generate");

    const char *fn = find_static_function_definition(code, "test_put_");
    assert(fn != NULL && "put definition should exist");
    const char *fn_end = next_static_after(fn);
    assert(fn_end != NULL && "put function body should be bounded");
    assert(count_between(fn, fn_end, "XR_ASSUME(_a->data != NULL)") == 1 &&
           "successful checked typed-array store must expose its non-null storage invariant");
    assert(count_between(fn, fn_end, "xrt_index_oob(") == 1 &&
           "checked typed-array store must preserve its out-of-bounds trap");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_stringbuilder_constructor_consumes_emission_recipe) {
    XiFunc *ir = compile_to_ir("var out = StringBuilder()\n");
    assert(ir != NULL && "StringBuilder constructor IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "stringbuilder_sync", &had_error);
    assert(code != NULL && "StringBuilder constructor C generation failed");
    assert(!had_error && "sealed StringBuilder constructor recipe should generate");
    assert(count_between(code, code + strlen(code), "xrt_strbuf_new()") == 1 &&
           "ordinary CGen must consume exactly one fixed StringBuilder allocation recipe");
    assert(!contains(code, "xrt_call_builtin") &&
           "StringBuilder construction must not use a generic builtin fallback");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_stringbuilder_constructor_consumes_emission_recipe) {
    XiFunc *ir = compile_to_ir("var out = StringBuilder()\nCoro.yield()\n");
    assert(ir != NULL && "coroutine StringBuilder constructor IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "stringbuilder_coro", &had_error);
    assert(code != NULL && "coroutine StringBuilder constructor C generation failed");
    assert(!had_error && "sealed coroutine StringBuilder constructor recipe should generate");
    assert(contains(code, "_aot_resume") &&
           "StringBuilder coroutine fixture must exercise the resume-body emitter");
    assert(count_between(code, code + strlen(code), "xrt_strbuf_new()") == 1 &&
           "coroutine CGen must consume exactly one fixed StringBuilder allocation recipe");
    assert(!contains(code, "xrt_call_builtin") &&
           "coroutine StringBuilder construction must not use a generic builtin fallback");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_builtin_iterator_pull_methods_preserve_error_polls) {
    const char *src = "fn copyRunes(text: string) -> string {\n"
                      "    var out = StringBuilder()\n"
                      "    var iter = text.runes()\n"
                      "    while (iter.hasNext()) {\n"
                      "        var r = iter.next()\n"
                      "        var cp = r.toUInt32()\n"
                      "        if (cp > 0) { out.append(r) }\n"
                      "    }\n"
                      "    return out.toString()\n"
                      "}\n"
                      "print(copyRunes(\"Ab\"))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "StringBuilder/Iterator direct runtime methods should generate");

    const char *fn = find_static_function_definition(code, "test_copyRunes_");
    assert(fn != NULL && "copyRunes definition should exist");
    const char *fn_end = next_static_after(fn);
    assert(fn_end != NULL && "copyRunes function body should be bounded");
    assert(count_between(fn, fn_end, "xrt_has_pending_error(") == 2 &&
           "Iterator.hasNext/next must poll the error channel because the static handle may be "
           "generator-backed");
    assert(count_between(fn, fn_end, "xrt_release(") >= 2 &&
           "fresh StringBuilder and Iterator owners must both be released");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_err_check_releases_live_arc_owners_on_cold_edge) {
    const char *src = "fn decode(bytes: Array<u8>) -> string? {\n"
                      "    var out = StringBuilder()\n"
                      "    out.append(\"prefix\")\n"
                      "    var decoded = string.fromUtf8(bytes)\n"
                      "    out.append(\"suffix\")\n"
                      "    return decoded\n"
                      "}\n"
                      "print(decode([65 as u8]))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "error-edge ARC cleanup fixture should generate");

    const char *fn = find_static_function_definition(code, "test_decode_");
    assert(fn != NULL && "decode definition should exist");
    const char *fn_end = next_static_after(fn);
    assert(fn_end != NULL && "decode function body should be bounded");
    const char *check = strstr(fn, "if (XR_UNLIKELY(xrt_has_pending_error())) {");
    assert(check != NULL && check < fn_end && "fallible UTF-8 decode must retain its error check");
    const char *check_end = strstr(check, "    }");
    assert(check_end != NULL && check_end < fn_end &&
           "pending-error branch should have a bounded body");
    assert(count_between(check, check_end, "xrt_release(") >= 1 &&
           "pending-error branch must release the live StringBuilder owner");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_u8_uses_byte_storage_fast_path) {
    const char *src = "fn sum() -> i64 {\n"
                      "    var bytes: Array<u8> = []\n"
                      "    bytes.push(200)\n"
                      "    unsafe {\n"
                      "        var value = 42\n"
                      "        bytes.set(0, value as u8)\n"
                      "    }\n"
                      "    return i64(bytes[0]) + len(bytes)\n"
                      "}\n"
                      "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed byte array fast path should generate");
    assert((contains(code, "xrt_array_new_typed(") || contains(code, "xrt_array_new_typed_ptr(")) &&
           "Array<u8> creation must preserve typed storage");
    assert(contains(code, "XR_ELEM_U8") && "Array<u8> must use the U8 typed element layout");
    assert(contains(code, "((uint8_t*)_a->data)") &&
           "Array<u8> index reads and writes must access raw byte storage");
    assert(contains(code, "(uint8_t)") && "Array<u8> writes must narrow to the byte storage width");
    assert(!contains(code, "xrt_method_1(") &&
           "Array<u8>.push must not fall back to dynamic method dispatch");
    assert(!contains(code, "xrt_getprop(") &&
           "len(Array<u8>) must not fall back to dynamic property dispatch");
    assert(!contains(code, "xrt_index_get(") &&
           "Array<u8> index read must not fall back to runtime index dispatch");
    assert(!contains(code, "xrt_index_set(") &&
           "Array<u8>.set must not fall back to runtime index dispatch");

    printf("  Generated typed byte array fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_source_class_array_push_consumes_generated_emission_recipe) {
    const char *src = "class Item {\n"
                      "    value: i64\n"
                      "    constructor(value: i64) { this.value = value }\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var items: Array<Item> = []\n"
                      "    var item = Item(42)\n"
                      "    items.push(item)\n"
                      "    return len(items)\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "source-class Array.push IR compilation succeeded");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "source-class Array.push C output exists");
    TEST_REQUIRE(!had_error, "source-class Array.push C generation succeeded");
    const char *run = find_static_function_definition(code, "test_run_");
    TEST_REQUIRE(run != NULL, "source-class Array.push function definition exists");
    const char *run_end = next_static_after(run);
    TEST_REQUIRE(run_end != NULL, "source-class Array.push function body is bounded");
    TEST_REQUIRE(count_between(run, run_end, "xrt_array_push(") == 1,
                 "source-class Array.push uses the canonical tagged runtime owner once");
    TEST_REQUIRE(!contains_between(run, run_end, "xrt_method_1("),
                 "source-class Array.push does not use dynamic selector dispatch");
    TEST_REQUIRE(!contains_between(run, run_end, "xrt_array_check_store_or_abort(") &&
                     !contains_between(run, run_end, "XR_ARRAY_MARK_MUTATED("),
                 "source-class Array.push does not inline the old mutation body");

    printf("  Generated source-class Array.push recipe %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_string_copy_bytes_preserves_byte_storage_fast_path) {
    const char *src = "fn first(s: string) -> i64 {\n"
                      "    var bytes = s.copyBytes()\n"
                      "    return i64(bytes[0])\n"
                      "}\n"
                      "print(first(\"A\"))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "string.copyBytes typed byte path should generate");
    assert(contains(code, "xrt_str_to_bytes(") &&
           "string.copyBytes must use the owned UTF-8 byte bridge");
    assert(contains(code, "((uint8_t*)_a->data)") &&
           "string.copyBytes indexing must preserve raw byte storage");
    assert(!contains(code, "((XrValue*)_a->data)") &&
           "string.copyBytes indexing must not reinterpret bytes as tagged values");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_zero_fill_range_uses_memset) {
    const char *src = "fn run() -> i64 {\n"
                      "    var values = Array<u32>(8, 7)\n"
                      "    values.fill(0, 0, 8)\n"
                      "    unsafe {\n"
                      "        return i64(values.get(1)) + i64(values.get(6))\n"
                      "    }\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed array fill should generate");
    assert(contains(code, "xr_array_core_fill_range(") &&
           "range fill should preserve Array.fill range normalization");
    assert(contains(code, "memset((uint8_t*)_a->data") &&
           "zero fill on typed arrays should lower to direct memset");
    assert(!contains(code, "xrt_array_fill_value(") &&
           "typed zero fill with an explicit range must not use boxed fill helper");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_byte_slice_safe_methods_use_stable_owners) {
    const char *src =
        "fn run() -> i64 {\n"
        "    var src = Array<u8>(16)\n"
        "    src[0] = 1\n"
        "    src[1] = 2\n"
        "    src[2] = 3\n"
        "    src[3] = 4\n"
        "    var view: Slice<u8> = src[:]\n"
        "    var dst: Array<u8> = Array.withCapacity(460)\n"
        "    dst.reserve(460)\n"
        "    dst.appendFrom(view[0:4])\n"
        "    dst.repeatFrom(2, 2)\n"
        "    dst.appendFrom(view[0:4])\n"
        "    var dstView: Slice<u8> = dst[:]\n"
        "    dstView.repeatFrom(6, 2, 2)\n"
        "    var dstWindow: Slice<u8> = dstView[8:10]\n"
        "    var srcWindow: Slice<u8> = dstView[6:8]\n"
        "    dstWindow.copyFrom(srcWindow)\n"
        "    var prefixLeft: Slice<u8> = dstView[0:2]\n"
        "    var prefixRight: Slice<u8> = dstView[4:6]\n"
        "    var prefix = prefixLeft.commonPrefix(prefixRight)\n"
        "    var v16: u16 = view.load<u16>(0, Endian.LE)\n"
        "    var v32: u32 = view.load<u32>(0, Endian.LE)\n"
        "    var v64: u64 = view.load<u64>(0, Endian.LE)\n"
        "    view.store<u16>(8, v16, Endian.LE)\n"
        "    return i64(v16) + i64(v32) + i64(v64) + i64(dst[5]) + i64(dst[9]) + prefix\n"
        "}\n"
        "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "Array<u8> raw memory helpers should generate");

    const char *fn = strstr(code, "static int64_t test_run_");
    assert(fn != NULL && "run declaration should exist");
    fn = strstr(fn + 1, "static int64_t test_run_");
    assert(fn != NULL && "run definition should exist");
    const char *fn_body = strchr(fn, '{');
    const char *fn_end = fn_body ? strstr(fn_body, "\n}\n\nstatic") : NULL;
    assert(fn_body != NULL && fn_end != NULL && fn_body < fn_end &&
           "run function body should be bounded");

    assert(count_between(fn_body, fn_end, "xrt_byte_slice_load_u16_le_unchecked_raw(") > 0 &&
           "Slice<u8>.load<u16>(Endian.LE) must lower to the trusted LE load helper");
    assert(count_between(fn_body, fn_end, "xrt_byte_slice_load_u32_le_unchecked_raw(") > 0 &&
           "Slice<u8>.load<u32>(Endian.LE) must lower to the trusted LE load helper");
    assert(count_between(fn_body, fn_end, "xrt_byte_slice_load_u64_le_unchecked_raw(") > 0 &&
           "Slice<u8>.load<u64>(Endian.LE) must lower to the trusted LE load helper");
    assert(count_between(fn_body, fn_end, "xr_array_core_bytes_store_u16(") > 0 &&
           "Slice<u8>.store<u16> must lower to the plan-driven core store helper");
    assert(count_between(fn_body, fn_end, "xr_raw_memory_copy_nonoverlap(") > 0 &&
           "Array<u8>.appendFrom must lower to the inline Array<u8>+Slice<u8> non-overlap "
           "fast path");
    assert(count_between(fn_body, fn_end, "xrt_byte_slice_copy_checked_raw(") > 0 &&
           "Slice<u8>.copyFrom must lower through the stable owner adapter");
    assert(count_between(fn_body, fn_end, "xrt_byte_array_append_from_span_raw(") > 0 &&
           "Array<u8>.appendFrom must lower through the stable owner adapter");
    assert(count_between(fn_body, fn_end, "xrt_byte_array_append_from_span_slow_raw(") == 0 &&
           count_between(fn_body, fn_end, "xrt_array_reserve_trusted_raw(") == 0 &&
           count_between(fn_body, fn_end, "xr_raw_memory_copy_nonoverlap(") == 0 &&
           "Array<u8>.appendFrom CGen must not recreate grow, alias, or copy semantics");
    assert(count_between(fn_body, fn_end, "xrt_byte_array_repeat_from_tail_raw(") > 0 &&
           "Array<u8>.repeatFrom must lower through the stable owner adapter");
    assert(count_between(fn_body, fn_end, "xr_array_core_bytes_repeat_copy(") == 0 &&
           count_between(fn_body, fn_end, "xrt_array_reserve_trusted_raw(") == 0 &&
           "Array<u8>.repeatFrom CGen must not recreate repeat or reserve semantics");
    assert(count_between(fn_body, fn_end, "xrt_byte_slice_repeat_from_checked_raw(") > 0 &&
           "Slice<u8>.repeatFrom must lower through the stable owner adapter");
    assert(count_between(fn_body, fn_end, "xr_array_core_bytes_repeat_from(") == 0 &&
           "static Slice<u8>.repeatFrom hot path must not keep the generic repeat wrapper");
    assert(count_between(fn_body, fn_end, "xr_array_core_bytes_copy_from(") == 0 &&
           "static Slice<u8>.copyFrom hot path must not keep the generic copy wrapper");
    assert(count_between(fn_body, fn_end, "xrt_byte_slice_common_prefix_checked_raw(") > 0 &&
           "Slice<u8>.commonPrefix must lower through the stable owner adapter");
    assert(count_between(fn_body, fn_end, "xrt_array_reserve_trusted_raw(") > 0 &&
           "Array<u8>.reserve must lower to the raw AOT helper");
    assert(count_between(fn_body, fn_end, "xrt_byte_array_load_u16_le_value(") == 0 &&
           count_between(fn_body, fn_end, "xrt_byte_array_load_u32_le_value(") == 0 &&
           count_between(fn_body, fn_end, "xrt_byte_array_load_u64_le_value(") == 0 &&
           count_between(fn_body, fn_end, "xrt_byte_array_append_from_value(") == 0 &&
           count_between(fn_body, fn_end, "xrt_byte_array_repeat_from_value(") == 0 &&
           count_between(fn_body, fn_end, "xrt_byte_slice_copy_value(") == 0 &&
           count_between(fn_body, fn_end, "xrt_byte_slice_common_prefix_value(") == 0 &&
           count_between(fn_body, fn_end, "xrt_array_reserve_value(") == 0 &&
           "Array<u8> hot path must not call boxed value helpers");
    assert(count_between(fn_body, fn_end, "xrt_byte_slice_load_u16_checked_raw(") == 0 &&
           count_between(fn_body, fn_end, "xrt_byte_slice_load_u32_checked_raw(") == 0 &&
           count_between(fn_body, fn_end, "xrt_byte_slice_load_u64_checked_raw(") == 0 &&
           "static Slice<u8>.load hot path must not keep dynamic span checks");
    assert(count_between(fn_body, fn_end, "xrt_byte_slice_store_u16_checked_raw(") == 0 &&
           "static Slice<u8>.store hot path must not keep dynamic span checks");
    assert(count_between(fn_body, fn_end, "xr_array_core_bytes_common_prefix_raw(") == 0 &&
           "generated C must not revive the retired common-prefix kernel");
    assert(count_between(fn_body, fn_end, "xr_array_core_bytes_repeat_copy(_span.data") == 0 &&
           "Slice<u8>.repeatFrom must not recreate owner semantics");
    assert(count_between(fn_body, fn_end, "memmove(_dst.data, _src.data") == 0 &&
           "Slice<u8>.copyFrom must not recreate owner semantics");
    assert(count_between(fn_body, fn_end, "xrt_method_") == 0 &&
           count_between(fn_body, fn_end, "xrt_index_get(") == 0 &&
           "Array<u8> hot path must not fall back to dynamic dispatch");
    assert(count_between(fn_body, fn_end, "strcmp(") == 0 &&
           count_between(fn_body, fn_end, "\"appendFrom\"") == 0 &&
           count_between(fn_body, fn_end, "\"repeatFrom\"") == 0 &&
           count_between(fn_body, fn_end, "\"copyFrom\"") == 0 &&
           count_between(fn_body, fn_end, "\"commonPrefix\"") == 0 &&
           count_between(fn_body, fn_end, "\"load\"") == 0 &&
           count_between(fn_body, fn_end, "\"store\"") == 0 &&
           "Array<u8>/Slice<u8> hot path must not use hidden method-name matching");
    assert(count_between(fn_body, fn_end, "XR_ELEM_ANY") == 0 &&
           "Array<u8>/Slice<u8> hot path must not degrade to tagged element storage");
    assert(count_between(fn_body, fn_end, "((XrValue*)") == 0 &&
           "Array<u8>/Slice<u8> hot path must not reinterpret raw bytes as boxed values");
    assert(count_between(fn_body, fn_end, "xrt_map_new(3)") == 0 &&
           count_between(fn_body, fn_end, "xrt_getprop_name(") == 0 &&
           count_between(fn_body, fn_end, "_ev_Endian_LE") == 0 &&
           "Slice<u8>.load/store hot path must fold direct Endian constants");

    printf("  Generated Array<u8> raw helper fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_byte_array_copy_uses_stable_owner_adapter) {
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_COPY_HI,
                                          XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_COPY_LO,
                                          XR_SEM_CONSUMER_CGEN));
    const char *adapter = xr_semantic_owner_cgen_adapter(XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_COPY_HI,
                                                         XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_COPY_LO);
    TEST_REQUIRE(adapter != NULL && strcmp(adapter, "xrt_byte_array_copy_checked_raw") == 0,
                 "byte-array copy publishes its stable CGen adapter");

    XrType *array_type = xr_type_new_u8_array(g_iso);
    XrType int_type = {.kind = XR_KIND_INT, .id = 946, .scalar_rep = XR_NATIVE_I64, .frozen = true};
    TEST_REQUIRE(array_type != NULL, "manual byte-array copy type allocated");
    XiFunc *ir = xi_func_new("manual_byte_array_copy_owner", array_type);
    TEST_REQUIRE(ir != NULL, "manual byte-array copy function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual byte-array copy entry allocated");
    entry->sealed = true;
    ir->nparams = 2;
    ir->min_params = 2;
    ir->params = (XiValue **) xr_calloc(2, sizeof(XiValue *));
    TEST_REQUIRE(ir->params != NULL, "manual byte-array copy parameters allocated");
    XiValue *dst = xi_param(ir, entry, 0, array_type);
    XiValue *src = xi_param(ir, entry, 1, array_type);
    ir->params[0] = dst;
    ir->params[1] = src;
    ir->arc_borrow_sig =
        (XiBorrowSig *) xi_func_arena_alloc(ir, (uint32_t) sizeof(*ir->arc_borrow_sig));
    TEST_REQUIRE(ir->arc_borrow_sig != NULL, "manual byte-array copy ownership allocated");
    ir->arc_borrow_sig->nparams = 2;
    ir->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
    ir->arc_borrow_sig->param_own[1] = XI_OWN_BORROWED;
    ir->arc_borrow_sig->valid = true;
    ir->arc_return_ownership.kind = XI_RETURN_OWNERSHIP_BORROWED_PARAM;
    ir->arc_return_ownership.param_index = 0;
    ir->arc_return_ownership.complete = true;
    XiValue *zero = xi_const_int(ir, entry, 0, &int_type);
    XiValue *two = xi_const_int(ir, entry, 2, &int_type);
    XiValue *within = xi_value_new(ir, entry, XI_BYTE_ARRAY_COPY_WITHIN, array_type, 4);
    XiValue *from = xi_value_new(ir, entry, XI_BYTE_ARRAY_COPY_FROM, array_type, 5);
    TEST_REQUIRE(dst != NULL && src != NULL && zero != NULL && two != NULL && within != NULL &&
                     from != NULL,
                 "manual byte-array copy values allocated");
    within->args[0] = dst;
    within->args[1] = two;
    within->args[2] = zero;
    within->args[3] = two;
    from->args[0] = within;
    from->args[1] = src;
    from->args[2] = zero;
    from->args[3] = zero;
    from->args[4] = two;
    xi_block_set_return(entry, from);

    TEST_REQUIRE(test_prepare_backend_ir(ir), "byte-array copy owner fixture reached Backend");
    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "byte-array copy owner fixture generated C");
    TEST_REQUIRE(count_between(code, code + strlen(code), "xrt_byte_array_copy_checked_raw(") == 2,
                 "both byte-array copy operations call the stable owner adapter");
    TEST_REQUIRE(contains(code, "XR_BYTE_ARRAY_COPY_WITHIN") &&
                     contains(code, "XR_BYTE_ARRAY_COPY_FROM"),
                 "generated C selects the exact copy owner operation");
    TEST_REQUIRE(!contains(code, "xrt_byte_array_copy_within_raw(") &&
                     !contains(code, "xrt_byte_array_copy_from_raw(") &&
                     !contains(code, "xr_byte_array_copy_core("),
                 "generated C does not recreate or revive copy semantics");
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_byte_slice_native_load_elides_endian_box) {
    const char *src =
        "fn roundtrip(view: ref Slice<u8>, value: u64, endian: Endian, flip: bool) -> u64 {\n"
        "    var bias: u64 = 1\n"
        "    if (flip) { bias = 2 }\n"
        "    unsafe {\n"
        "        view.store<u64>(0, value, endian)\n"
        "        return view.load<u64>(0, endian) + bias\n"
        "    }\n"
        "}\n"
        "fn run() -> u64 {\n"
        "    var bytes = Array<u8>(8)\n"
        "    var view: Slice<u8> = bytes[:]\n"
        "    return roundtrip(ref view, 42, Endian.LE, false)\n"
        "}\n"
        "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "byte-slice native load test should generate");
    const char *roundtrip = find_static_function_definition(code, "roundtrip_");
    TEST_REQUIRE(roundtrip != NULL, "byte-slice roundtrip definition should be emitted");
    const char *roundtrip_end = next_static_after(roundtrip);
    TEST_REQUIRE(contains_between(roundtrip, roundtrip_end, "byte_slice_store_u64") &&
                     contains_between(roundtrip, roundtrip_end, "byte_slice_load_u64"),
                 "planned byte-slice operations must use native raw helpers");
    TEST_REQUIRE(contains_between(roundtrip, roundtrip_end, "phi"),
                 "regression fixture must retain an unrelated branch phi");
    TEST_REQUIRE(!contains_between(roundtrip, roundtrip_end, "XR_FROM_INT("),
                 "unrelated phis must not block native byte-slice operand box elision");

    printf("  Generated byte-slice native endian operand %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_span_window_and_mem_slice_elide_boxed_operands) {
    const char *src = "import mem\n"
                      "fn rawWindowLength(source: Slice<u8>, start: i64, count: u32) -> i64 {\n"
                      "    var window = source.window(start, count as i64)\n"
                      "    unsafe {\n"
                      "        var raw = mem.slice<u8>(window.ptr(), len(window), window)\n"
                      "        return len(raw)\n"
                      "    }\n"
                      "}\n"
                      "var source: [u8; 4] = [1, 2, 3, 4]\n"
                      "print(rawWindowLength(source[:], 1, 2))\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "span-window/mem-slice operand test should generate");
    const char *raw_window = find_static_function_definition(code, "rawWindowLength_");
    TEST_REQUIRE(raw_window != NULL, "rawWindowLength definition should be emitted");
    const char *raw_window_end = next_static_after(raw_window);
    TEST_REQUIRE(
        contains_between(raw_window, raw_window_end, "/* caller-proven mem.slice raw view */"),
        "mem.slice must retain its caller-proven native lowering");
    TEST_REQUIRE(!contains_between(raw_window, raw_window_end, "XR_FROM_INT("),
                 "span-window and mem.slice native operands must not retain boxes");

    printf("  Generated span-window/mem-slice native operands %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_borrowed_bytes_param_reserve_skips_arc) {
    const char *src = "fn hot(dst: ref Array<u8>) -> i64 {\n"
                      "    dst.reserve(8)\n"
                      "    return len(dst)\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var dst = Array<u8>(1)\n"
                      "    return hot(ref dst)\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "Borrowed Array<u8> parameter reserve should generate");

    const char *fn = strstr(code, "static int64_t test_hot_");
    assert(fn != NULL && "hot declaration should exist");
    fn = strstr(fn + 1, "static int64_t test_hot_");
    assert(fn != NULL && "hot definition should exist");
    const char *fn_end = next_static_after(fn);
    assert(fn_end != NULL && "hot function body should be bounded");

    assert(count_between(fn, fn_end, "xrt_array_reserve_trusted_raw(") > 0 &&
           "borrowed Array<u8>.reserve must still lower to the raw helper");
    assert(count_between(fn, fn_end, "xrt_retain(") == 0 &&
           count_between(fn, fn_end, "xrt_release(") == 0 &&
           "borrowed Array<u8>.reserve receiver must not force parameter ARC");

    printf("  Generated borrowed Array<u8> reserve fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_direct_call_converts_bytes_to_byte_slice_arg) {
    const char *src = "fn sum(src: Slice<u8>) -> i64 {\n"
                      "    var v: u32 = src.load<u32>(0, Endian.LE)\n"
                      "    return i64(v)\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var bytes = Array<u8>(4)\n"
                      "    bytes[0] = 1\n"
                      "    bytes[1] = 2\n"
                      "    bytes[2] = 3\n"
                      "    bytes[3] = 4\n"
                      "    return sum(bytes[:])\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "direct Slice<u8> arguments should generate");
    assert(contains(code, "xrt_span_from_array_slice(") &&
           "direct call should pass an explicit Array<u8> slice as a native span");
    assert(contains(code, "test_sum_") && "helper should be emitted as a direct call target");
    assert(!contains(code, "cannot pass non-aggregate") &&
           "direct call argument ABI should not reject Array<u8>-to-Slice<u8> conversion");

    printf("  Generated direct Array<u8>-to-Slice<u8> argument conversion %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_boxed_adapter_converts_byte_slice_arg) {
    const char *src = "fn apply(f: fn(Slice<u8>) -> i64, src: Array<u8>) -> i64 {\n"
                      "    return f(src[:])\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var bytes = Array<u8>(4)\n"
                      "    bytes[0] = 1\n"
                      "    bytes[1] = 2\n"
                      "    bytes[2] = 3\n"
                      "    bytes[3] = 4\n"
                      "    return apply(fn(src: Slice<u8>) -> i64 {\n"
                      "        var v: u32 = src.load<u32>(0, Endian.LE)\n"
                      "        return i64(v)\n"
                      "    }, bytes)\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    XiCgenStats stats = {0};
    char *code = generate_c_with_status_and_cgen_stats(ir, "test", &had_error, &stats);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "boxed Slice<u8> adapter should generate");
    assert(stats.boxed_adapters >= 1 && "dynamic Slice<u8> callback should keep a boxed adapter");
    assert(contains(code, "xrt_span_from_value_ref(p0") &&
           "boxed adapter should convert its boxed parameter to the canonical span ABI");
    assert(!contains(code, "_cl, p0)") &&
           "boxed adapter must not pass XrValue directly to a raw Slice<u8> ABI slot");

    printf("  Generated boxed Slice<u8> adapter conversion %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_array_data_ptr_unchecked_uses_raw_pointer_path) {
    const char *src = "fn run() -> i64 {\n"
                      "    var src = Array<u8>(2)\n"
                      "    src[0] = 7\n"
                      "    src[1] = 9\n"
                      "    var out = Array<u8>(4)\n"
                      "    out.resize(2, 0)\n"
                      "    var sum = 0\n"
                      "    unsafe {\n"
                      "        var p = out.mutPtr()\n"
                      "        p[0] = 0\n"
                      "        var sp = src.ptr()\n"
                      "        p.copyFromNonOverlapping(sp, 2)\n"
                      "        var view: Slice<u8> = out[:]\n"
                      "        var rp = view.ptr()\n"
                      "        sum = i64(rp[0]) + i64(rp[1])\n"
                      "    }\n"
                      "    return sum\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "Array/Slice data pointer lowering should generate");
    assert(contains(code, "->data)") && "ptr must lower to raw array data");
    assert(!contains(code, "\"mutPtr\"") && !contains(code, "\"ptr\"") &&
           "data pointer methods must not survive as dynamic method names");
    assert(!contains(code, "xrt_method_0(") &&
           "data pointer methods must not fall back to dynamic method dispatch");
    assert(contains(code, "xrt_data_pointer_project(") &&
           contains(code, "XR_DATA_POINTER_OWNER_BORROW).address") &&
           "Array/Slice pointers must route through the stable data-pointer owner");
    const char *fn = find_static_function_definition(code, "static int64_t test_run_");
    assert(fn != NULL && "run definition should exist");
    const char *fn_end = next_static_after(fn);
    assert(fn_end != NULL && "run function body should be bounded");
    assert(count_between(fn, fn_end, "void *") > 0 &&
           "Ptr/MutPtr locals must use native pointer C storage");
    assert(count_between(fn, fn_end, "xrt_raw_scalar_access_store_i64(") > 0 &&
           count_between(fn, fn_end, "xrt_raw_scalar_access_load_i64(") > 0 &&
           "MutPtr<u8>/Ptr<u8> accesses must route through the stable raw scalar owner");
    assert(count_between(fn, fn_end, "xrt_raw_memory_copy_nonoverlap(") > 0 &&
           "MutPtr.copyFromNonOverlapping must lower through the stable owner adapter");
    assert(count_between(fn, fn_end, "INT64_C(2)") > 0 &&
           "constant-size MutPtr.copyFromNonOverlapping should expose literal byte count");
    assert(count_between(fn, fn_end, "memcpy(") == 0 &&
           count_between(fn, fn_end, "XR_ASSUME(_xr_dst") == 0 &&
           "CGen must not recreate raw-memory copy semantics");
    assert(count_between(fn, fn_end, "(uintptr_t)") == 0 &&
           "Ptr/MutPtr hot locals must not round-trip through integer pointer casts");
    assert(count_between(fn, fn_end, "memcpy((void *)(uintptr_t)") == 0 &&
           "Ptr memcpy must use native pointer locals directly");
    const char *slice_call = strstr(fn, "xrt_span_from_array_slice(");
    assert(slice_call != NULL && "test should still exercise Slice<u8> ptr() after array slice");
    assert(count_between(fn, slice_call, "XR_TO_INT(") == 0 &&
           count_between(fn, fn_end, "XR_TAG_PTR") == 0 &&
           "Ptr/MutPtr locals must stay in native address representation");
    assert(count_between(fn, fn_end, "xrt_release(") == 2 &&
           "each owning Array<u8> (src, out) must be released exactly once; raw pointers add "
           "no ARC");
    assert(count_between(fn, fn_end, "xrt_retain(") == 0 &&
           "raw pointer views must not add retain traffic on the owning arrays");

    printf("  Generated Array/Slice data pointer fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_zero_byte_rawptr_copy_accepts_null_without_memcpy) {
    const char *src = "fn run() -> i64 {\n"
                      "    unsafe {\n"
                      "        var dst = MutPtr<u8>.null()\n"
                      "        var src = Ptr<u8>.null()\n"
                      "        dst.copyFromNonOverlapping(src, 0)\n"
                      "    }\n"
                      "    return 1\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "zero-byte raw-pointer copy should generate");
    const char *fn = find_static_function_definition(code, "static int64_t test_run_");
    TEST_REQUIRE(fn != NULL, "run definition should exist");
    const char *fn_end = next_static_after(fn);
    TEST_REQUIRE(fn_end != NULL, "run function body should be bounded");
    TEST_REQUIRE(count_between(fn, fn_end, "memcpy(") == 0,
                 "zero-byte raw-pointer copy must not call C memcpy with null pointers");
    TEST_REQUIRE(count_between(fn, fn_end, "xrt_raw_memory_copy_nonoverlap(") == 1,
                 "zero-byte raw-pointer copy must still route through the stable owner adapter");
    TEST_REQUIRE(count_between(fn, fn_end, "XR_ASSUME(_xr_dst") == 0,
                 "zero-byte raw-pointer copy must not establish a false non-null proof");

    printf("  Generated zero-byte raw pointer copy %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_cfn_local_coercion_uses_native_function_address) {
    const char *src = "type Op = CFn<fn(i64) -> i64>\n"
                      "fn inc(value: i64) -> i64 { return value + 1 }\n"
                      "fn run() -> i64 {\n"
                      "    var operation: Op = inc\n"
                      "    return operation(41)\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "CFn local coercion IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "CFn local coercion C generation failed");
    TEST_REQUIRE(!had_error, "verified top-level function must materialize as a CFn");
    const char *run = find_static_function_definition(code, "static int64_t test_run_");
    TEST_REQUIRE(run != NULL, "run definition should exist");
    const char *run_end = next_static_after(run);
    TEST_REQUIRE(run_end != NULL, "run function body should be bounded");
    TEST_REQUIRE(contains_between(run, run_end, "(void *)test_inc_") &&
                     !contains_between(run, run_end, "XR_TO_INT("),
                 "CFn coercion must use the verified native entry, not a tagged integer cast");

    printf("  Generated native CFn local coercion in %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_rawptr_copy_forwarded_constant_has_no_release_local) {
    const char *src = "fn copyTwo(enabled: bool, destination: MutPtr<u8>, source: Ptr<u8>) {\n"
                      "    if (enabled) {\n"
                      "        unsafe { destination.copyFromNonOverlapping(source, 2) }\n"
                      "    }\n"
                      "}\n"
                      "var source = Array<u8>(2)\n"
                      "var destination = Array<u8>(2)\n"
                      "unsafe { copyTwo(true, destination.mutPtr(), source.ptr()) }\n";

    XiPipelineConfig cfg = xi_pipeline_aot_config();
    XiFunc *ir = compile_to_ir_with_config(src, cfg);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "forwarded raw-pointer-copy C generation failed");
    TEST_REQUIRE(!had_error, "forwarded raw-pointer-copy fixture should generate");
    const char *fn = find_static_function_definition(code, "copyTwo");
    TEST_REQUIRE(fn != NULL, "copyTwo definition should exist");
    const char *fn_end = next_static_after(fn);
    TEST_REQUIRE(fn_end != NULL, "copyTwo function body should be bounded");
    TEST_REQUIRE(contains_between(fn, fn_end, "xrt_raw_memory_copy_nonoverlap(") &&
                     contains_between(fn, fn_end, "INT64_C(2)"),
                 "forwarded constant copy count must remain literal at the owner adapter");
    TEST_REQUIRE(count_lines_outside_debug_locals_with_prefix(fn, fn_end, "    int64_t v",
                                                              " = INT64_C(2);") == 0 &&
                     count_lines_outside_debug_locals_with_prefix(fn, fn_end, "    v",
                                                                  " = INT64_C(2);") == 0,
                 "forwarded memcpy constants must not leave a release C local or assignment");

    printf("  Elided forwarded raw-pointer-copy constant in %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_rawptr_parallel_for_each_capture_is_rejected) {
    const char *src = "import parallel\n"
                      "fn run(n: i64) {\n"
                      "    var slots: Array<i64> = [0]\n"
                      "    parallel.forEach(0..n, (i) -> {\n"
                      "        unsafe {\n"
                      "            var p = slots.mutPtr()\n"
                      "            var first = p[0]\n"
                      "            p[0] = first + i\n"
                      "        }\n"
                      "    }, parallel.Options(2))\n"
                      "}\n"
                      "run(4)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir == NULL &&
           "mutable owner/raw pointer access across parallel execution must fail closed");
}

TEST(cgen_span_index_get_elides_dead_err_check) {
    const char *src = "fn sum(src: Slice<u8>, n: i64) -> i64 {\n"
                      "    var total = 0\n"
                      "    var i = 0\n"
                      "    while (i < n) {\n"
                      "        total = total + i64(src[i])\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    return total\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var bytes = Array<u8>(4)\n"
                      "    bytes[0] = 1\n"
                      "    bytes[1] = 2\n"
                      "    bytes[2] = 3\n"
                      "    bytes[3] = 4\n"
                      "    const view: Slice<u8> = bytes[:]\n"
                      "    return sum(view, 4)\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "Slice<u8> index read should generate");

    const char *fn = find_static_function_definition(code, "test_sum_");
    assert(fn != NULL && "sum definition should exist");
    const char *fn_end = next_static_after(fn);
    assert(fn_end != NULL && "sum function body should be bounded");

    assert(count_between(fn, fn_end, "((uint8_t*)_s.data)[_idx]") > 0 &&
           "Slice<u8> index read must stay in native span storage");
    assert(count_between(fn, fn_end, "xrt_index_oob(") > 0 &&
           "safe Slice<u8> index read must keep its bounds trap");
    assert(count_between(fn, fn_end, "xrt_has_pending_error(") == 0 &&
           "Slice<u8> index read must not keep a dead ERR_CHECK after the noreturn bounds trap");

    printf("  Generated Slice<u8> index get fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_span_slice_elides_dead_err_check) {
    const char *src = "fn windowLen(src: Slice<u8>, start: i64, n: i64) -> i64 {\n"
                      "    const part: Slice<u8> = src[start:start + n]\n"
                      "    return len(part)\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var bytes = Array<u8>(8)\n"
                      "    const view: Slice<u8> = bytes[:]\n"
                      "    return windowLen(view, 2, 4)\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "Slice<u8> slice should generate");

    const char *fn = strstr(code, "static int64_t test_windowLen_");
    assert(fn != NULL && "windowLen declaration should exist");
    fn = strstr(fn + 1, "static int64_t test_windowLen_");
    assert(fn != NULL && "windowLen definition should exist");
    const char *fn_end = next_static_after(fn);
    assert(fn_end != NULL && "windowLen function body should be bounded");

    assert(count_between(fn, fn_end, "xrt_span_from_span_slice(") > 0 &&
           "Slice<u8> slice-of-slice must stay in native span storage");
    assert(count_between(fn, fn_end, "xrt_has_pending_error(") == 0 &&
           "Slice<u8> slice-of-slice must not keep a dead ERR_CHECK");

    printf("  Generated Slice<u8> slice fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_byte_array_append_from_slice_elides_dead_err_check) {
    const char *src = "fn appendRange(out: ref Array<u8>, src: Slice<u8>, start: i64, n: i64) {\n"
                      "    out.appendFrom(src[start:start + n])\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var out = Array<u8>()\n"
                      "    var src = Array<u8>(8)\n"
                      "    const view: Slice<u8> = src[:]\n"
                      "    appendRange(ref out, view, 2, 4)\n"
                      "    return len(out)\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "Slice<u8> append should generate");

    const char *fn = strstr(code, "static void test_appendRange_");
    assert(fn != NULL && "appendRange declaration should exist");
    fn = strstr(fn + 1, "static void test_appendRange_");
    assert(fn != NULL && "appendRange definition should exist");
    const char *fn_end = next_static_after(fn);
    assert(fn_end != NULL && "appendRange function body should be bounded");

    assert(count_between(fn, fn_end, "xrt_span_from_span_slice(") > 0 &&
           "appendRange must keep the Slice<u8> slice in native span storage");
    assert(count_between(fn, fn_end, "xrt_byte_array_append_from_span_raw(") > 0 &&
           "appendRange must lower appendFrom(Slice<u8>) through the stable owner adapter");
    assert(count_between(fn, fn_end, "xrt_has_pending_error(") == 0 &&
           "appendFrom(Slice<u8> slice) must not keep dead ERR_CHECKs after proven native paths");

    printf("  Generated Slice<u8> append-from-slice fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_byte_array_repeat_from_tail_elides_dead_err_check) {
    const char *src = "fn extend(out: ref Array<u8>) {\n"
                      "    out.repeatFrom(2, 4)\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var out = Array<u8>()\n"
                      "    out.push(1)\n"
                      "    out.push(2)\n"
                      "    extend(ref out)\n"
                      "    return len(out)\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "Array<u8>.repeatFrom should generate");

    const char *fn = strstr(code, "static void test_extend_");
    assert(fn != NULL && "extend declaration should exist");
    fn = strstr(fn + 1, "static void test_extend_");
    assert(fn != NULL && "extend definition should exist");
    const char *fn_end = next_static_after(fn);
    assert(fn_end != NULL && "extend function body should be bounded");

    assert(count_between(fn, fn_end, "xrt_byte_array_repeat_from_tail_raw(") > 0 &&
           "Array<u8>.repeatFrom must lower to the raw tail repeat helper");
    assert(count_between(fn, fn_end, "xrt_has_pending_error(") == 0 &&
           "Array<u8>.repeatFrom native helper must not keep a dead ERR_CHECK");

    printf("  Generated Array<u8>.repeatFrom fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_verified_span_helper_drop_elides_pending_error_checks) {
    const char *src = "fn hot(dst: ref Slice<u8>, src: Slice<u8>) -> i64 {\n"
                      "    var copied: Slice<u8> = dst.copyFrom(src)\n"
                      "    var word: u16 = dst.load<u16>(0, Endian.LE)\n"
                      "    dst.store<u16>(0, word + 1, Endian.LE)\n"
                      "    return len(copied) + dst.compare(src)\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var dst = Array<u8>(8, 0)\n"
                      "    var src = Array<u8>(8, 1)\n"
                      "    var dstView: Slice<u8> = dst[:]\n"
                      "    return hot(ref dstView, src[:])\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "verified Slice helper-drop path should generate");

    const char *fn = find_static_function_definition(code, "test_hot_");
    assert(fn != NULL && "hot definition should exist");
    const char *fn_end = next_static_after(fn);
    assert(fn_end != NULL && "hot function body should be bounded");
    assert(count_between(fn, fn_end, "xrt_has_pending_error(") == 0 &&
           "verified inline Slice operations must not retain pending-error polls");

    printf("  Generated verified Slice helper-drop path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_mem_load_uses_pointer_helper) {
    const char *src = "import mem\n"
                      "fn read(src: Array<u8>) -> i64 {\n"
                      "    var view: Slice<u8> = src[:]\n"
                      "    var sum = 0\n"
                      "    unsafe {\n"
                      "        var p = view.ptr()\n"
                      "        var v16: u16 = mem.load<u16>(p, 1, Endian.LE)\n"
                      "        var v32: u32 = mem.load<u32>(p, 0, Endian.LE)\n"
                      "        var v64: u64 = mem.load<u64>(p, 0, Endian.LE)\n"
                      "        sum = i64(v16) + i64(v32) + i64(v64)\n"
                      "    }\n"
                      "    return sum\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var src = Array<u8>(8)\n"
                      "    src[0] = 1\n"
                      "    src[1] = 2\n"
                      "    src[2] = 3\n"
                      "    src[3] = 4\n"
                      "    src[4] = 5\n"
                      "    src[5] = 6\n"
                      "    src[6] = 7\n"
                      "    src[7] = 8\n"
                      "    return read(src)\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "mem.load should generate");

    const char *fn = strstr(code, "static int64_t test_read_");
    assert(fn != NULL && "read declaration should exist");
    fn = strstr(fn + 1, "static int64_t test_read_");
    assert(fn != NULL && "read definition should exist");
    const char *fn_end = next_static_after(fn);
    assert(fn_end != NULL && "read function body should be bounded");

    assert(count_between(fn, fn_end, "xrt_raw_scalar_access_load_i64(") == 3 &&
           "mem.load widths must lower through one raw scalar owner adapter");
    assert(count_between(fn, fn_end, "xr_raw_load_u") == 0 &&
           count_between(fn, fn_end, "xr_raw_u16_from_") == 0 &&
           count_between(fn, fn_end, "xr_raw_u32_from_") == 0 &&
           count_between(fn, fn_end, "xr_raw_u64_from_") == 0 &&
           "CGen must not recreate raw scalar width or endian semantics");
    assert(count_between(fn, fn_end, "void *") > 0 &&
           "mem.load should keep the data pointer in native C storage");
    assert(count_between(fn, fn_end, "(uintptr_t)") == 0 &&
           "mem.load hot path must not round-trip through integer pointer casts");
    assert(count_between(fn, fn_end, "xrt_byte_array_load_u16_le_") == 0 &&
           count_between(fn, fn_end, "xrt_byte_array_load_u32_le_") == 0 &&
           count_between(fn, fn_end, "xrt_byte_array_load_u64_le_") == 0 &&
           "mem.load must not route back through Array<u8>/Slice<u8> helpers");
    assert(count_between(fn, fn_end, "xr_array_core_bytes_") == 0 &&
           count_between(fn, fn_end, "bool ok") == 0 &&
           "unchecked loads must not contain checked Array core state");
    assert(count_between(fn, fn_end, "xrt_has_pending_error(") == 0 &&
           "mem.load hot path must not keep a dead ERR_CHECK");

    printf("  Generated mem.load fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_mem_store_uses_pointer_helper) {
    const char *src = "import mem\n"
                      "fn write(dst: Array<u8>) {\n"
                      "    var view: Slice<u8> = dst[:]\n"
                      "    unsafe {\n"
                      "        var p = view.mutPtr()\n"
                      "        mem.store<u16>(p, 1, 0x1234, Endian.LE)\n"
                      "        mem.store<u32>(p, 4, 0x01020304, Endian.LE)\n"
                      "        mem.store<u64>(p, 8, 0x0102030405060708, Endian.LE)\n"
                      "    }\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var dst = Array<u8>(16)\n"
                      "    write(dst)\n"
                      "    return i64(dst[1]) + i64(dst[4]) + i64(dst[8])\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "mem.store should generate");

    const char *fn = strstr(code, "static void test_write_");
    assert(fn != NULL && "write declaration should exist");
    fn = strstr(fn + 1, "static void test_write_");
    assert(fn != NULL && "write definition should exist");
    const char *fn_end = next_static_after(fn);
    assert(fn_end != NULL && "write function body should be bounded");

    assert(count_between(fn, fn_end, "xrt_raw_scalar_access_store_i64(") == 3 &&
           "mem.store widths must lower through one raw scalar owner adapter");
    assert(count_between(fn, fn_end, "xr_raw_store_u") == 0 &&
           count_between(fn, fn_end, "xr_raw_u16_from_") == 0 &&
           count_between(fn, fn_end, "xr_raw_u32_from_") == 0 &&
           count_between(fn, fn_end, "xr_raw_u64_from_") == 0 &&
           "CGen must not recreate raw scalar width or endian semantics");
    assert(count_between(fn, fn_end, "void *") > 0 &&
           "mem.store should keep the data pointer in native C storage");
    assert(count_between(fn, fn_end, "(uintptr_t)") == 0 &&
           "mem.store hot path must not round-trip through integer pointer casts");
    assert(count_between(fn, fn_end, "xrt_byte_slice_store_") == 0 &&
           "mem.store must not route back through Slice<u8> helpers");
    assert(count_between(fn, fn_end, "xr_array_core_bytes_") == 0 &&
           count_between(fn, fn_end, "bool ok") == 0 &&
           "unchecked stores must not contain checked Array core state");
    assert(count_between(fn, fn_end, "xrt_has_pending_error(") == 0 &&
           "mem.store hot path must not keep a dead ERR_CHECK");

    printf("  Generated mem.store fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_stack_borrow_slice_allows_local_rawptr_read_chain) {
    const char *src = "fn readByteAt(src: Slice<u8>, pos: i64) -> i64 {\n"
                      "    unsafe {\n"
                      "        return i64(src.ptr().offset(pos)[0])\n"
                      "    }\n"
                      "}\n"
                      "fn callWindow(bytes: Array<u8>) -> i64 {\n"
                      "    var view: Slice<u8> = bytes[1:3]\n"
                      "    return readByteAt(view, 1)\n"
                      "}\n"
                      "var bytes = Array<u8>(4)\n"
                      "bytes[0] = 5\n"
                      "bytes[1] = 7\n"
                      "bytes[2] = 11\n"
                      "bytes[3] = 13\n"
                      "print(callWindow(bytes))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "local Ptr read chain should generate");
    assert(contains(code, "xrt_span_from_array_slice(") &&
           "read Slice<u8> call should pass a native span slice view");
    assert(!contains(code, "xrt_array_stack_slice_view_release(") &&
           "borrowed read Slice<u8> stack slice must not release storage");
    assert(!contains(code, "xrt_slice(") &&
           "local Ptr read chain must not force a heap slice view");
    const char *read_fn = find_static_function_definition(code, "readByteAt_");
    assert(read_fn != NULL && "raw pointer reader definition should exist");
    const char *read_end = next_static_after(read_fn);
    assert(read_end != NULL && "raw pointer reader should be bounded");
    assert(contains_between(read_fn, read_end, "xr_raw_const_ptr_offset(") &&
           "unsafe Ptr.offset must use the standard-C helper carrying its non-null proof");
    assert(!contains_between(read_fn, read_end, "({") &&
           "Ptr.offset must not emit a GNU statement expression");
    assert(!contains_between(read_fn, read_end, "(uintptr_t)") &&
           "Ptr.offset must keep native pointer arithmetic without integer round-trips");

    printf("  Generated stack-borrow slice Ptr read-chain fast path %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_stack_borrow_slice_rejects_returned_rawptr) {
    const char *src = "fn leakPtr(src: Slice<u8>) -> Ptr<u8> {\n"
                      "    unsafe {\n"
                      "        return src.ptr()\n"
                      "    }\n"
                      "}\n"
                      "fn callWindow(bytes: Array<u8>) -> i64 {\n"
                      "    var view: Slice<u8> = bytes[1:3]\n"
                      "    var p = leakPtr(view)\n"
                      "    unsafe {\n"
                      "        return i64(p[0])\n"
                      "    }\n"
                      "}\n"
                      "var bytes = Array<u8>(4)\n"
                      "bytes[0] = 5\n"
                      "bytes[1] = 7\n"
                      "bytes[2] = 11\n"
                      "bytes[3] = 13\n"
                      "print(callWindow(bytes))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir == NULL && "returned borrowed Ptr must fail semantic analysis");

    printf("  Rejected returned Ptr that escapes its verified borrow lifetime\n");
}

TEST(cgen_typed_array_i16_and_u32_use_raw_storage_fast_path) {
    const char *src = "fn mix() -> i64 {\n"
                      "    var i16s: Array<i16> = []\n"
                      "    var u32s: Array<u32> = []\n"
                      "    i16s.push(32767)\n"
                      "    u32s.push(4294967295)\n"
                      "    return i64(i16s[0]) + i64(u32s[0]) + len(i16s) + len(u32s)\n"
                      "}\n"
                      "print(mix())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed sub-width array fast path should generate");
    assert(contains(code, "XR_ELEM_I16") && "Array<i16> must use the I16 typed element layout");
    assert(contains(code, "((int16_t*)_a->data)") &&
           "Array<i16> index reads and writes must access raw i16 storage");
    assert(contains(code, "(int16_t)") &&
           "Array<i16> writes must narrow to the signed 16-bit storage width");
    assert(contains(code, "XR_ELEM_U32") && "Array<u32> must use the U32 typed element layout");
    assert(contains(code, "((uint32_t*)_a->data)") &&
           "Array<u32> index reads and writes must access raw u32 storage");
    assert(contains(code, "(uint32_t)") &&
           "Array<u32> writes must narrow to the unsigned 32-bit storage width");
    assert(!contains(code, "xrt_method_1(") &&
           "sub-width typed array push must not fall back to dynamic method dispatch");
    assert(!contains(code, "xrt_index_get(") &&
           "sub-width typed array index read must not fall back to runtime index dispatch");

    printf("  Generated typed sub-width array fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_float_and_bool_use_raw_storage_fast_path) {
    const char *src = "fn mix() -> f64 {\n"
                      "    var values: Array<f64> = []\n"
                      "    var samples: Array<f32> = []\n"
                      "    var flags: Array<bool> = []\n"
                      "    values.push(3.5)\n"
                      "    samples.push(1.25)\n"
                      "    flags.push(true)\n"
                      "    if (flags[0]) {\n"
                      "        return values[0] + samples[0] + (len(values) as f64)\n"
                      "    }\n"
                      "    return 0.0\n"
                      "}\n"
                      "print(mix())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed f64/bool array fast path should generate");
    assert(contains(code, "XR_ELEM_F64") && "Array<f64> must use the F64 typed element layout");
    assert(contains(code, "((double*)_a->data)") &&
           "Array<f64> index reads and writes must access raw double storage");
    assert(contains(code, "XR_ELEM_F32") && "Array<f32> must use the F32 typed element layout");
    assert(contains(code, "((f64*)_a->data)") &&
           "Array<f32> index reads and writes must access raw f64 storage");
    assert(!contains(code, "(double)(f64)((f64*)") &&
           "Array<f32> raw loads are already f64-rounded");
    assert(contains(code, "XR_ELEM_BOOL") && "Array<bool> must use the BOOL typed element layout");
    assert(contains(code, "((uint8_t*)_a->data)") &&
           "Array<bool> index reads and writes must access raw byte storage");
    assert(!contains(code, "xrt_method_1(") &&
           "typed array push must not fall back to dynamic method dispatch");
    assert(!contains(code, "xrt_getprop(") &&
           "typed array length must not fall back to dynamic property dispatch");
    assert(!contains(code, "xrt_index_get(") &&
           "typed array index read must not fall back to runtime index dispatch");

    printf("  Generated typed f64/bool array fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_rune_uses_scalar_storage_with_rune_boxing) {
    const char *src = "fn first() -> rune {\n"
                      "    var chars: Array<rune> = []\n"
                      "    chars.push('b')\n"
                      "    chars[0] = 'a'\n"
                      "    return chars[0]\n"
                      "}\n"
                      "print(first())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed rune array fast path should generate");
    assert(contains(code, "XR_ELEM_RUNE") && "Array<rune> must use the RUNE typed element layout");
    assert(contains(code, "uint32_t") && "Array<rune> storage must be a compact scalar buffer");
    assert(contains(code, "XR_TO_RUNE(") && "Array<rune> writes must unbox tagged rune values");
    assert(contains(code, "XR_FROM_RUNE((uint32_t)") &&
           "Array<rune> reads must re-box raw scalars as rune");
    assert(!contains(code, "XR_ELEM_U32") && "Array<rune> must not degrade to Array<u32>");
    assert(!contains(code, "xrt_method_1(") &&
           "Array<rune>.push must not fall back to dynamic method dispatch");
    assert(!contains(code, "xrt_index_get(") &&
           "Array<rune> index read must not fall back to runtime index dispatch");

    printf("  Generated typed rune array fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_inlined_struct_uses_native_field_storage) {
    const char *src = "struct Sample {\n"
                      "    x: i64\n"
                      "    y: f64\n"
                      "    ok: bool\n"
                      "    octet: u8\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var p = Sample{x: 41, y: 2.5, ok: true, octet: 200}\n"
                      "    p.x = p.x + 1\n"
                      "    p.octet = p.octet + 1\n"
                      "    if (p.ok) {\n"
                      "        return p.x + i64(p.octet)\n"
                      "    }\n"
                      "    return 0\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "inlined struct native fields should generate");
    assert(contains(code, "struct { int64_t x; double y; uint8_t ok; uint8_t octet; }") &&
           "inlined struct must use native C field storage");
    assert(contains(code, "(uint8_t)") && "sub-width struct stores must narrow to storage width");
    assert(!contains(code, "XrValue x;") && !contains(code, "XrValue x =") &&
           "inlined scalar struct fields must not be boxed");
    assert(!contains(code, "xrt_getprop(") &&
           "inlined scalar struct field reads must not use dynamic property dispatch");
    assert(!contains(code, "xrt_map_set(") &&
           "inlined scalar struct field writes must not use map-style dynamic storage");

    printf("  Generated native struct field fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_escaping_struct_uses_heap_native_storage) {
    const char *src = "struct Sample {\n"
                      "    x: i64\n"
                      "    y: f64\n"
                      "    ok: bool\n"
                      "    octet: u8\n"
                      "}\n"
                      "var p = Sample{x: 41, y: 2.5, ok: true, octet: 200}\n"
                      "p.x = p.x + 1\n"
                      "p.octet = p.octet + 1\n"
                      "if (p.ok) {\n"
                      "    print(p.x + i64(p.octet))\n"
                      "} else {\n"
                      "    print(0)\n"
                      "}\n"
                      "print(p.y)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "escaping struct heap-native path should generate");
    assert(contains(code, "typedef struct xrt_struct_abi_") &&
           "escaping primitive struct must emit a native heap layout");
    assert((contains(code, "xr_aggregate_ref(") || contains(code, "xrt_aggregate_clone_bytes(")) &&
           "escaping primitive struct must allocate as an AOT struct reference; the "
           "ownership-safe lowering publishes it through the owned clone-bytes helper");
    assert(contains(code, "->x") && contains(code, "->y") && contains(code, "->ok") &&
           contains(code, "->octet") && "escaping primitive struct fields must use direct access");
    assert(!contains(code, "xrt_map_new(") && "primitive struct must not allocate runtime map");
    assert(!contains(code, "xrt_map_get(") && "primitive struct reads must not use runtime map");
    assert(!contains(code, "xrt_map_set(") && "primitive struct writes must not use runtime map");
    assert(!contains(code, "xrt_call_method(") &&
           "escaping struct path must not call an undeclared constructor helper");

    printf("  Generated escaping struct heap-native path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_same_shape_structs_keep_distinct_source_field_names) {
    const char *src = "struct Point {\n"
                      "    x: i64\n"
                      "    y: i64\n"
                      "}\n"
                      "struct Pair {\n"
                      "    a: i64\n"
                      "    b: i64\n"
                      "}\n"
                      "var p = Point{x: 1, y: 2}\n"
                      "var q = Pair{a: 3, b: 4}\n"
                      "print(p.x + p.y + q.a + q.b)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "same-shape source-named structs should generate");
    const char *code_end = code + strlen(code);
    assert(count_between(code, code_end, "typedef struct xrt_struct_abi_") >= 2 &&
           "same-shape structs with different source field names must not share one typedef");
    assert(contains(code, "int64_t x; int64_t y;") &&
           "Point native layout must preserve source field names");
    assert(contains(code, "int64_t a; int64_t b;") &&
           "Pair native layout must preserve source field names");
    assert(contains(code, "->x") && contains(code, "->y") && contains(code, "->a") &&
           contains(code, "->b") && "field access must use each struct's source field names");

    printf("  Generated distinct same-shape struct layouts %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_escaping_struct_string_field_uses_heap_native_storage) {
    const char *src = "struct Item {\n"
                      "    count: i64\n"
                      "    name: string\n"
                      "}\n"
                      "var item = Item{count: 2, name: \"hi\"}\n"
                      "item.count = item.count + 3\n"
                      "print(item.count)\n"
                      "print(item.name)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "string-field heap-native struct path should generate");
    assert(contains(code, "typedef struct xrt_struct_abi_") &&
           "mixed scalar/string struct must emit a native heap layout");
    assert(contains(code, "XrValue name") &&
           "string struct field must be stored as a tagged immutable reference field");
    assert((contains(code, "xr_aggregate_ref(") || contains(code, "xrt_aggregate_clone_bytes(")) &&
           "mixed scalar/string struct must allocate as an AOT struct reference");
    assert(contains(code, "->count") && contains(code, "->name") &&
           "mixed scalar/string struct fields must use direct access");
    assert(!contains(code, "xrt_map_new(") && "mixed scalar/string struct must not allocate map");
    assert(!contains(code, "xrt_map_get(") && "mixed scalar/string struct reads must not use map");
    assert(!contains(code, "xrt_map_set(") && "mixed scalar/string struct writes must not use map");

    printf("  Generated string-field struct heap-native path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_fixed_layout_struct_omits_native_header) {
    const char *src = "struct CPair {\n"
                      "    a: i32\n"
                      "    b: u8\n"
                      "}\n"
                      "var p = CPair{a: 41, b: 1}\n"
                      "p.a = p.a + 1\n"
                      "print(p.a + (p.b as i32))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "fixed-layout struct path should generate");

    const char *typedef_start = strstr(code, "typedef struct xrt_struct_abi_");
    assert(typedef_start != NULL && "fixed-layout struct must emit a native typedef");
    const char *typedef_end = strstr(typedef_start, ";\n");
    assert(typedef_end != NULL && "typedef should be bounded");
    assert(count_between(typedef_start, typedef_end, "_size") == 0 &&
           "fixed-layout typedef must not include the Xray size header");
    assert(count_between(typedef_start, typedef_end, "_layout") == 0 &&
           "fixed-layout typedef must not include the Xray layout header");
    assert(count_between(typedef_start, typedef_end, "int32_t a") == 1 &&
           "fixed-layout i32 field must be placed at payload offset 0");
    assert(count_between(typedef_start, typedef_end, "uint8_t b") == 1 &&
           "fixed-layout u8 field must be emitted as raw C storage");
    assert((contains(code, "xr_aggregate_ref(_s, (uint16_t)sizeof(") ||
            contains(code, "xrt_aggregate_clone_bytes(")) &&
           "fixed-layout struct refs must carry storage size outside the payload; the "
           "ownership-safe lowering passes it through the owned clone-bytes helper");

    printf("  Generated fixed-layout struct path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_nested_struct_field_uses_embedded_heap_native_storage) {
    const char *src = "struct Point {\n"
                      "    x: i64\n"
                      "    y: i64\n"
                      "}\n"
                      "struct Box {\n"
                      "    p: Point\n"
                      "    z: i64\n"
                      "}\n"
                      "var b = Box{p: Point{x: 1, y: 2}, z: 3}\n"
                      "b.p.x = b.p.x + 4\n"
                      "print(b.p.x + b.p.y + b.z)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "nested struct heap-native path should generate");
    const char *code_end = code + strlen(code);
    assert(count_between(code, code_end, "typedef struct xrt_struct_abi_") >= 2 &&
           "nested struct path must emit parent and child native heap layouts");
    assert(contains(code, "xrt_struct_abi_") && contains(code, " p;") &&
           "parent heap layout must embed the child native struct field");
    assert(contains(code, "memcpy(&") &&
           "nested struct field assignment must copy embedded native bytes");
    assert(contains(code, "*)&") &&
           "nested struct field reads must use direct access through the embedded field address");
    assert(!contains(code, "xrt_map_new(") && "nested struct must not allocate map storage");
    assert(!contains(code, "xrt_map_get(") && "nested struct reads must not use map storage");
    assert(!contains(code, "xrt_map_set(") && "nested struct writes must not use map storage");

    printf("  Generated nested struct heap-native path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_fixed_array_struct_field_uses_embedded_heap_native_storage) {
    const char *src = "struct Buf {\n"
                      "    data: [u8; 4]\n"
                      "    bias: i64\n"
                      "}\n"
                      "var buf = Buf{data: [1, 2, 3, 4], bias: 5}\n"
                      "fn run(n: i64) -> i64 {\n"
                      "    var i = 0\n"
                      "    while (i < n) {\n"
                      "        buf.data[0] = i as u8\n"
                      "        buf.data[1] = buf.data[0]\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    return i64(buf.data[1])\n"
                      "}\n"
                      "print(run(10))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "fixed-array struct field heap-native path should generate");

    const char *fn = strstr(code, "static int64_t test_run_");
    assert(fn != NULL && "run declaration should exist");
    fn = strstr(fn + 1, "static int64_t test_run_");
    assert(fn != NULL && "run definition should exist");
    const char *fn_body = strchr(fn, '{');
    const char *fn_end = fn_body ? strstr(fn_body, "\n}\n\nstatic") : NULL;
    assert(fn_body != NULL && fn_end != NULL && fn_body < fn_end &&
           "run function body should be bounded");

    assert(contains(code, "uint8_t data[4]") &&
           "fixed array field must be embedded in the native heap layout");
    assert(contains(code, "memmove(") &&
           "fixed array field initialization must copy directly into embedded storage");
    assert(!contains(code, "xrt_fixed_array_copy(") &&
           "fixed array field initialization must not use the generic runtime helper");
    assert(count_between(fn_body, fn_end, "data[") > 0 &&
           "fixed array hot path must use direct C array indexing");
    assert(count_between(fn_body, fn_end, "\n    XrValue v") == 0 &&
           "fixed array hot function must not materialize tagged array refs");
    assert(count_between(fn_body, fn_end, "xrt_index_get(") == 0 &&
           count_between(fn_body, fn_end, "xrt_index_set(") == 0 &&
           "fixed array hot path must not call generic index helpers");
    assert(!contains(code, "xrt_map_new(") && !contains(code, "xrt_map_get(") &&
           !contains(code, "xrt_map_set(") && "fixed array struct must not use map storage");

    printf("  Generated fixed-array struct heap-native path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_fixed_array_local_uses_stack_array_ref_storage) {
    const char *src = "fn first(key: [u8; 4]) -> i64 {\n"
                      "    return i64(key[0])\n"
                      "}\n"
                      "fn at(key: [u8; 4], i: i64) -> i64 {\n"
                      "    return i64(key[i])\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var key: [u8; 4] = [1, 2, 3, 4]\n"
                      "    key[1] = 9\n"
                      "    return len(key) + first(key) + i64(key[1]) + at(key, 2)\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "local fixed-array path should generate");

    assert(contains(code, "uint8_t _fa") && "local fixed array must allocate native stack storage");
    assert(contains(code, "xr_array_ref(_fa") &&
           "local fixed array must expose storage as an array ref");
    assert(contains(code, "INT64_C(4)") && !contains(code, "XR_ARRAY_REF_ELEM_COUNT") &&
           "len(local fixed array) must use its native static extent without a metadata read");
    assert(contains(code, "[INT64_C(1)] = (uint8_t)") &&
           "local fixed array constant stores should use direct stack lanes");
    assert(contains(code, "((uint8_t*)") && contains(code, "[INT64_C(0)]") &&
           "fixed-array parameters should use direct typed lanes");
    assert(contains(code, "xrt_fixed_index_checked(") &&
           "dynamic fixed-array parameter indexes should use the portable checked-index helper");
    assert(!contains(code, "({") &&
           "fixed-array checked loads must remain portable ISO C expressions");
    assert(!contains(code, "xrt_index_get(") && !contains(code, "xrt_index_set(") &&
           "fixed array index operations should not call generic index helpers");
    assert(!contains(code, "((xrt_array_t*)") &&
           "local fixed array array-ref storage must not be cast to dynamic array header");

    printf("  Generated local fixed-array stack path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_fixed_array_local_return_clones_borrowed_stack_storage) {
    const char *src = "fn bump(lanes: ref [u8; 4]) {\n"
                      "    lanes[2] = 9\n"
                      "}\n"
                      "fn make() -> [u8; 4] {\n"
                      "    var out: [u8; 4] = [1, 2, 3, 4]\n"
                      "    bump(ref out)\n"
                      "    return out\n"
                      "}\n"
                      "var value = make()\n"
                      "print(value[2])\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "fixed-array local return should generate");
    const char *fn = find_static_function_definition(code, "make_");
    TEST_REQUIRE(fn != NULL, "make definition should exist");
    const char *fn_end = next_static_after(fn);
    TEST_REQUIRE(fn_end != NULL, "make function body should be bounded");
    TEST_REQUIRE(contains_between(fn, fn_end, "return xrt_array_ref_clone_value("),
                 "borrowed stack fixed-array return must clone directly into owned storage");
    TEST_REQUIRE(
        !contains_between(fn, fn_end, "xrt_array_ref_to_owned("),
        "known borrowed stack storage must not retain an analyzer-opaque ownership branch");

    printf("  Generated owned fixed-array return %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_fixed_array_index_ops_elide_boxed_operands) {
    const char *src = "fn run(i: i64) -> i64 {\n"
                      "    var lanes: [u64; 4] = [1, 2, 3, 4]\n"
                      "    lanes[1] = lanes[i]\n"
                      "    return lanes[1] as i64\n"
                      "}\n"
                      "print(run(2))\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "fixed-array native operand test should generate");
    const char *run = find_static_function_definition(code, "run_");
    TEST_REQUIRE(run != NULL, "fixed-array run definition should be emitted");
    const char *run_end = next_static_after(run);
    TEST_REQUIRE(!contains_between(run, run_end, "XR_FROM_INT("),
                 "fixed-array native indexes and lanes must not retain boxed temporaries");
    TEST_REQUIRE(contains_between(run, run_end, "_fa") &&
                     contains_between(run, run_end, "xrt_fixed_index_checked("),
                 "fixed-array direct storage and dynamic bounds checks must remain");
    TEST_REQUIRE(!contains_between(run, run_end, "({"),
                 "fixed-array native checked loads must not use GNU statement expressions");

    printf("  Generated fixed-array native operands %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_static_method_call_elides_class_descriptor_receiver) {
    const char *lib_src = "export struct Pair {\n"
                          "    lanes: [u64; 2]\n"
                          "    static fromLanes(lanes: [u64; 2]) -> Pair {\n"
                          "        return Pair{lanes: lanes}\n"
                          "    }\n"
                          "}\n"
                          "export fn make() -> Pair {\n"
                          "    return Pair.fromLanes([20, 22])\n"
                          "}\n"
                          "make()\n";
    const char *app_src = "print(0)\n";

    XiFunc *lib_ir = compile_to_ir(lib_src);
    XiFunc *app_ir = compile_to_ir(app_src);
    TEST_REQUIRE(lib_ir && app_ir, "IR compilation succeeded");
    TEST_REQUIRE(lib_ir->module && app_ir->module, "module metadata available");
    lib_ir->module->name = "lib";
    lib_ir->module->path = "lib.xr";
    app_ir->module->name = "app";
    app_ir->module->path = "app.xr";
    XiModule *modules[] = {lib_ir->module, app_ir->module};

    TEST_REQUIRE(test_prepare_backend_ir(lib_ir), "library backend preparation succeeded");
    TEST_REQUIRE(test_prepare_backend_ir(app_ir), "app backend preparation succeeded");
    TestAotPlan plan;
    test_aot_plan_prepare(&plan, modules, 2, 1);

    XiCgenCtx *ctx = xi_cgen_ctx_new();
    TEST_REQUIRE(ctx != NULL, "CGen context allocated");
    xi_cgen_ctx_set_aot_bundle(ctx, &plan.bundle);
    xi_cgen_resolve_module_imports(ctx, modules, 2);

    char *code = NULL;
    size_t code_size = 0;
    FILE *mem = xr_open_memstream(&code, &code_size);
    TEST_REQUIRE(mem != NULL, "CGen output stream allocated");
    xi_cgen_module_tu(ctx, mem, modules, 2, 0, 1);
    TEST_REQUIRE(xr_close_memstream(mem, &code, &code_size) == 0, "CGen output stream closed");
    TEST_REQUIRE(code && !xi_cgen_has_error(ctx), "multi-module C generation succeeded");
    TEST_REQUIRE(contains(code, "xrt_shared_lib["),
                 "exported class descriptor storage remains available to other modules");

    const char *make = find_static_function_definition(code, "lib_make_exp");
    TEST_REQUIRE(make != NULL, "make definition exists");
    const char *make_end = strstr(make, "\n}\n\n");
    TEST_REQUIRE(make_end != NULL, "make function body bounded");
    TEST_REQUIRE(count_between(make, make_end, "fromLanes") > 0,
                 "static method lowers to a direct function call");
    TEST_REQUIRE(count_between(make, make_end, "xrt_shared_lib[") == 0,
                 "direct static method call omits its class descriptor receiver load");

    printf("  Generated static method descriptor-free direct call %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_cgen_ctx_free(ctx);
    test_aot_plan_free(&plan);
    xi_func_free(lib_ir);
    xi_func_free(app_ir);
}

TEST(cgen_map_class_static_factory_is_not_constructor) {
    const char *src = "class Box {\n"
                      "    kind: string\n"
                      "    value: i64\n"
                      "    constructor(kind: string) {\n"
                      "        this.kind = kind\n"
                      "        this.value = 0\n"
                      "    }\n"
                      "    static Int(value: i64) -> Box {\n"
                      "        var out = Box(\"i64\")\n"
                      "        out.value = value\n"
                      "        return out\n"
                      "    }\n"
                      "}\n"
                      "fn make() -> Box { return Box.Int(42) }\n"
                      "print(make().value)\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "map-backed class static factory should generate");
    const char *make = find_static_function_definition(code, "make_");
    TEST_REQUIRE(make != NULL, "make definition should exist");
    const char *make_end = next_static_after(make);
    TEST_REQUIRE(make_end != NULL, "make function body should be bounded");
    TEST_REQUIRE(!contains_between(make, make_end, "Box_constructor_m"),
                 "static factory call must not be rewritten as a constructor");
    TEST_REQUIRE(!contains_between(make, make_end, "_portable_map_inst_"),
                 "caller must not allocate a second map-backed instance");

    printf("  Generated map-backed static factory call %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_shared_struct_alias_elides_tagged_hot_locals) {
    const char *src = "struct Cell {\n"
                      "    a: i64\n"
                      "    b: i64\n"
                      "    step: i64\n"
                      "}\n"
                      "var cell = Cell{a: 1, b: 2, step: 3}\n"
                      "fn run(n: i64) -> i64 {\n"
                      "    var p = cell\n"
                      "    var i = 0\n"
                      "    var sum = 0\n"
                      "    while (i < n) {\n"
                      "        p.a = p.a + i\n"
                      "        p.b = p.b + p.step\n"
                      "        sum = sum + p.a - p.b\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    return sum + p.a + p.b\n"
                      "}\n"
                      "print(run(10))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "shared struct alias fast path should generate");

    const char *fn = strstr(code, "static int64_t test_run_");
    assert(fn != NULL && "run declaration should exist");
    fn = strstr(fn + 1, "static int64_t test_run_");
    assert(fn != NULL && "run definition should exist");
    const char *fn_body = strchr(fn, '{');
    const char *fn_end = fn_body ? strstr(fn_body, "\n}\n\nstatic") : NULL;
    assert(fn_body != NULL && fn_end != NULL && fn_body < fn_end &&
           "run function body should be bounded");

    assert((contains(code, "xr_aggregate_ref(") || contains(code, "xrt_aggregate_clone_bytes(")) &&
           "shared primitive struct must use native heap storage");
    assert(count_between(fn_body, fn_end, "xrt_value_clone_for_coro(") > 0 &&
           "mutable local struct copy should clone the shared slot value before mutation");
    assert(count_between(fn_body, fn_end, ")->a") > 0 &&
           count_between(fn_body, fn_end, ")->b") > 0 &&
           "mutable local struct copy should still use native field storage");
    assert(count_between(fn_body, fn_end, "xrt_release(") == 1 &&
           "the mutable clone must be released exactly once without releasing the shared borrow");
    assert(count_between(fn_body, fn_end, "xrt_map_get") == 0 &&
           count_between(fn_body, fn_end, "xrt_map_set") == 0 &&
           "shared struct hot path must not cross the map boundary");

    printf("  Generated shared struct alias fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_class_method_caches_receiver_scalar_fields) {
    const char *src = "class Counter {\n"
                      "    value: i64\n"
                      "    step: i64\n"
                      "    constructor(init: i64, step: i64) {\n"
                      "        this.value = init\n"
                      "        this.step = step\n"
                      "    }\n"
                      "    bump(n: i64) -> i64 {\n"
                      "        var i = 0\n"
                      "        var sum = 0\n"
                      "        while (i < n) {\n"
                      "            this.value = this.value + this.step\n"
                      "            sum = sum + this.value\n"
                      "            i = i + 1\n"
                      "        }\n"
                      "        return sum + this.value\n"
                      "    }\n"
                      "}\n"
                      "var c = Counter(1, 3)\n"
                      "print(c.bump(10))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "class receiver field cache should generate");

    const char *fn = strstr(code, "static int64_t test_bump_");
    assert(fn != NULL && "bump method should use typed ABI");
    fn = strstr(fn + 1, "static int64_t test_bump_");
    assert(fn != NULL && "bump method definition should follow its declaration");
    const char *fn_end = next_static_after(fn);

    assert(contains(code, "typedef struct xrt_native_test_Counter") &&
           "primitive-layout classes should emit an AOT native receiver type");
    assert(contains(fn, "xrt_native_test_Counter *p0") &&
           "bump method receiver should use native pointer ABI");
    assert(count_between(fn, fn_end, "_cf") > 0 &&
           "receiver scalar fields should be cached in native locals");
    assert(count_between(fn, fn_end, "xrt_map_get") == 0 &&
           "native receiver method body should not read fields through map boundary");
    assert(count_between(fn, fn_end, "xrt_map_set") == 0 &&
           "native receiver method body should not flush fields through map boundary");
    const char *value_load = strstr(fn, "p0->f0");
    const char *step_load = strstr(fn, "p0->f1");
    assert(value_load != NULL && value_load < fn_end && step_load != NULL && step_load < fn_end &&
           value_load < step_load && "receiver scalar field cache should follow class layout");
    assert(count_between(fn, fn_end, "XR_FROM_INT(v") == 0 &&
           "method body should not box scalar field store temporaries");

    printf("  Generated class receiver field cache %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_local_class_direct_native_methods_omit_boxed_adapters) {
    const char *src = "class Counter {\n"
                      "    value: i64\n"
                      "    constructor(init: i64) { this.value = init }\n"
                      "    bump(n: i64) -> i64 {\n"
                      "        this.value = this.value + n\n"
                      "        return this.value\n"
                      "    }\n"
                      "    get() -> i64 { return this.value }\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var c = Counter(2)\n"
                      "    var a = c.bump(5)\n"
                      "    return a + c.get()\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "local direct native class method path should generate");

    assert(contains(code, "typedef struct xrt_native_test_Counter") &&
           "local class should still use a native receiver layout");
    assert(contains(code, "int64_t test_bump_") && contains(code, "int64_t test_get_") &&
           "direct native methods should keep typed entry points");
    assert(!contains(code, "static XrValue test_bump_") &&
           !contains(code, "static XrValue test_get_") &&
           "pure local native class flow must not emit dead boxed method adapters");

    const char *run = strstr(code, "static int64_t test_run_");
    assert(run != NULL && "run function should use typed ABI");
    run = strstr(run + 1, "static int64_t test_run_");
    assert(run != NULL && "run function definition should follow its declaration");
    const char *run_body = strchr(run, '{');
    const char *run_end = run_body ? strstr(run_body, "\n}\n\nstatic") : NULL;
    assert(run_body != NULL && run_end != NULL && run_body < run_end &&
           "run function body should be bounded");
    assert(count_between(run, run_end, "xrt_map_new(") == 0 &&
           count_between(run, run_end, "xrt_map_get(") == 0 &&
           count_between(run, run_end, "xrt_map_set(") == 0 &&
           "pure local native class flow must stay out of the map boundary");

    printf("  Generated local direct native class method path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_native_receiver_static_cleanup_borrows_without_closure_arc) {
    const char *src = "class Box {\n"
                      "    n: i64\n"
                      "    constructor(n: i64) { this.n = n }\n"
                      "    bump() -> i64 {\n"
                      "        defer { this.n = this.n + 100 }\n"
                      "        return this.n\n"
                      "    }\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var b = Box(5)\n"
                      "    return b.bump()\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "native receiver static cleanup should generate");

    const char *bump = find_static_function_definition(code, "test_bump_");
    assert(bump != NULL && "bump method definition should be present");
    const char *bump_end = next_static_after(bump);
    assert(bump_end != NULL && "bump method body should be bounded");
    assert(count_between(bump, bump_end, "xrt_cleanup_enter()") == 2 &&
           "the defer body must execute on normal and exceptional exits");
    assert(count_between(bump, bump_end, "(p0)->f0") >= 4 &&
           "both static cleanup paths must access the borrowed native receiver directly");
    assert(count_between(bump, bump_end, "xrt_closure_new(") == 0 &&
           count_between(bump, bump_end, "_c->upvals[") == 0 &&
           count_between(bump, bump_end, "xrt_retain(xrt_box_obj(") == 0 &&
           "a non-escaping static defer must not allocate or retain a receiver closure");

    const char *run = find_static_function_definition(code, "test_run_");
    assert(run != NULL && "run function definition should be present");
    const char *run_end = next_static_after(run);
    assert(count_between(run, run_end, "xrt_obj_alloc(") == 0 &&
           count_between(run, run_end, "xrt_native_test_Box _ci") == 1 &&
           "a receiver used only by a synchronous static defer must remain stack allocated");

    printf("  Generated native receiver static cleanup path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_class_constructor_returns_heap_native_instance) {
    const char *src = "class Counter {\n"
                      "    value: i64\n"
                      "    constructor(init: i64) { this.value = init }\n"
                      "    bump(n: i64) -> i64 {\n"
                      "        this.value = this.value + n\n"
                      "        return this.value\n"
                      "    }\n"
                      "    get() -> i64 { return this.value }\n"
                      "}\n"
                      "fn make(n: i64) -> Counter {\n"
                      "    return Counter(n)\n"
                      "}\n"
                      "fn touch(c: ref Counter) -> i64 {\n"
                      "    c.value = c.value + 1\n"
                      "    return c.get()\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var c = make(2)\n"
                      "    var a = c.bump(5)\n"
                      "    var b = c.value\n"
                      "    var d = touch(ref c)\n"
                      "    return a + b + d + c.get()\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "heap native class instance path should generate");

    assert(contains(code, "xrt_obj_alloc((uint16_t)") &&
           "escaping native class constructor should allocate a typed heap object");
    /* Boxing the instance is legal in exactly one position: as the argument
     * that reaches the object header to stamp a storage class. What must stay
     * a raw pointer is the VALUE the constructor expression yields. Banning
     * the text outright also outlawed that storage tag, which a constructor
     * inlined into its caller legitimately emits, so the rule is spelled as
     * "every boxing is a storage tag" instead. */
    assert(count_between(code, code + strlen(code), "xrt_box_obj(_portable_inst_") ==
               count_between(code, code + strlen(code),
                             "xrt_value_set_storage(xrt_box_obj(_portable_inst_") &&
           "the only boxing of a native instance is the storage-class tag");
    assert(contains(code, "= _portable_inst_") &&
           "native class constructor values should stay as pointers inside typed AOT code");
    assert(!contains(code, "heap_type == XR_TINSTANCE") &&
           "closed-world typed class flow should not retain boxed instance discrimination");
    assert(!contains(code, "xrt_instanceof(") &&
           "closed-world typed class flow should not retain boxed instanceof guards");

    const char *make_sig = "static xrt_native_test_Counter * test_make_";
    const char *make = strstr(code, make_sig);
    assert(make != NULL && "make function should return a native class pointer");
    make = strstr(make + 1, make_sig);
    assert(make != NULL && "make function definition should follow its declaration");
    const char *make_end = next_static_after(make);
    assert(count_between(make, make_end, "xrt_obj_alloc(") == 1 &&
           "make should allocate exactly one heap native instance");
    /* Same rule as above, scoped to the producer: it returns a native pointer
     * (the signature matched on says so) and boxes only to stamp the storage
     * class on the header. */
    assert(count_between(make, make_end, "xrt_box_obj(_portable_inst_") ==
               count_between(make, make_end, "xrt_value_set_storage(xrt_box_obj(_portable_inst_") &&
           "the producer boxes only to stamp the storage class");
    assert(count_between(make, make_end, "xrt_map_new(") == 0 &&
           "escaping native class constructor must not allocate a map instance");
    assert(!contains(code, "static XrValue test_make_") &&
           "direct-only native class producer should not emit a boxed adapter");

    const char *touch = strstr(code, "static int64_t test_touch_");
    assert(touch != NULL && "touch function should use typed scalar return ABI");
    touch = strstr(touch + 1, "static int64_t test_touch_");
    assert(touch != NULL && "touch function definition should follow its declaration");
    const char *touch_end = next_static_after(touch);
    assert(count_between(touch, touch_end, "xrt_getprop_name(") == 0 &&
           count_between(touch, touch_end, "xrt_setprop_name(") == 0 &&
           count_between(touch, touch_end, "xrt_map_get(") == 0 &&
           count_between(touch, touch_end, "xrt_map_set(") == 0 &&
           "native class pointer parameters must not fall back to Map field access");
    assert(count_between(touch, touch_end, "test_get_") >= 1 &&
           count_between(touch, touch_end, "test_get_3_boxed(") == 0 &&
           "native class pointer method calls should use the typed method entry directly");

    assert(!contains(code, "static XrValue test_bump_") &&
           "pure typed native class flow should not keep a dead boxed method adapter");

    printf("  Generated heap native class instance path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_native_class_ref_field_constructor_result_uses_ptr_storage) {
    const char *src = "class IntBag {\n"
                      "    values: Array<i64>\n"
                      "    constructor(values: Array<i64>) { this.values = values }\n"
                      "    scan(rounds: i64) -> i64 {\n"
                      "        var r = 0\n"
                      "        var sum = 0\n"
                      "        while (r < rounds) {\n"
                      "            var i = 0\n"
                      "            while (i < len(this.values)) {\n"
                      "                sum = sum + this.values[i]\n"
                      "                i = i + 1\n"
                      "            }\n"
                      "            r = r + 1\n"
                      "        }\n"
                      "        return sum\n"
                      "    }\n"
                      "}\n"
                      "fn make_values(n: i64) -> Array<i64> {\n"
                      "    var values: Array<i64> = []\n"
                      "    var i = 0\n"
                      "    while (i < n) {\n"
                      "        values.push(i * 3 + 1)\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    return values\n"
                      "}\n"
                      "fn run(rounds: i64) -> i64 {\n"
                      "    var bag = IntBag(make_values(8))\n"
                      "    return bag.scan(rounds)\n"
                      "}\n"
                      "print(run(10))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "native class ref-field constructor path should generate");

    const char *run = find_static_function_definition(code, "test_run_");
    assert(run != NULL && "run function definition should follow its declaration");
    const char *run_end = next_static_after(run);
    assert(count_between(run, run_end, "xrt_obj_alloc(") == 0 &&
           "non-escaping ref-field native class should be stack-constructed");
    assert(count_between(run, run_end, "xrt_native_test_IntBag _ci") == 1 &&
           count_between(run, run_end, "xrt_native_test_IntBag_dtor(&_ci") == 1 &&
           "stack-constructed collection ref field class should release fields at exit");
    assert(count_between(run, run_end, "xrt_box_obj(_inst)") == 0 &&
           "local native class constructor result must not be boxed before direct method calls");
    assert(count_between(run, run_end, "test_scan_") >= 1 &&
           count_between(run, run_end, "test_scan_2_boxed(") == 0 &&
           "local native class method call should target the typed receiver entry");
    assert(count_between(run, run_end, "XR_FROM_INT(test_scan_") == 0 &&
           "stack-return path should keep the method result native until return");
    assert(count_between(run, run_end, "xrt_getprop_name(") == 0 &&
           count_between(run, run_end, "xrt_setprop_name(") == 0 &&
           count_between(run, run_end, "xrt_map_get(") == 0 &&
           count_between(run, run_end, "xrt_map_set(") == 0 &&
           "native-layout class ref-field flow must not fall back to Map storage");

    printf("  Generated native class ref-field constructor ptr path %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_native_class_collection_ref_fields_use_arc) {
    const char *src = "class Bag {\n"
                      "    values: Array<i64>\n"
                      "    constructor(values: Array<i64>) {\n"
                      "        this.values = values\n"
                      "    }\n"
                      "    replace(next: Array<i64>) -> i64 {\n"
                      "        this.values = next\n"
                      "        return len(this.values)\n"
                      "    }\n"
                      "    selfAssign() -> i64 {\n"
                      "        this.values = this.values\n"
                      "        return len(this.values)\n"
                      "    }\n"
                      "}\n"
                      "fn make(values: Array<i64>) -> Bag {\n"
                      "    return Bag(values)\n"
                      "}\n"
                      "fn swap(b: ref Bag, next: Array<i64>) -> i64 {\n"
                      "    b.values = next\n"
                      "    return len(b.values)\n"
                      "}\n"
                      "fn local(values: Array<i64>) -> i64 {\n"
                      "    var bag = Bag(values)\n"
                      "    return len(bag.values)\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var a = [1]\n"
                      "    var b = [2, 3]\n"
                      "    var bag = make(a)\n"
                      "    return bag.replace(b) + bag.selfAssign() + swap(ref bag, a) + local(a)\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "native class ref field ownership path should generate");

    assert(contains(code, "static void xrt_native_test_Bag_dtor(void *obj)") &&
           "AOT collection ref fields are RC-managed and need a destructor");
    assert(contains(code, "xrt_release(xr_mkptr((self)->f0, XR_TAG_ARRAY));") &&
           "collection ref field destructors should release stored containers");
    assert(contains(code, "static void xrt_native_test_Bag_promote_storage(void *obj, "
                          "uint8_t storage_mode)") &&
           contains(code, "xrt_value_set_storage(xr_mkptr((self)->f0, XR_TAG_ARRAY), "
                          "storage_mode)") &&
           "native class ref fields should publish their complete owned graph");
    assert(contains(code, "xrt_type_register_hot(0, NULL, 0, xrt_native_test_Bag_dtor, "
                          "xrt_native_test_Bag_promote_storage, "
                          "(uint32_t)sizeof(xrt_native_test_Bag))") &&
           "class hot type registration should wire destructor and storage promoter");
    assert(contains(code, "static void *xrt_native_test_Bag_runtime_clone(void *obj)") &&
           contains(code, "xrt_value_clone_for_coro(xr_mkptr((src)->f0, XR_TAG_ARRAY))") &&
           "language-level copy should recursively clone owned native class fields");
    assert(contains(code, "xrt_type_set_runtime_clone(_tid, "
                          "xrt_native_test_Bag_runtime_clone)") &&
           "native classes should register their language-level copy callback");
    assert(contains(code, "xrt_type_set_name(_tid, \"Bag\", NULL)") &&
           "default hosted AOT should install class name metadata");

    const char *replace = strstr(code, "static int64_t test_replace_");
    assert(replace != NULL && "replace method should use typed ABI");
    replace = strstr(replace + 1, "static int64_t test_replace_");
    assert(replace != NULL && "replace method definition should follow its declaration");
    const char *replace_end = next_static_after(replace);
    /* The store is one comma expression: release the old container, then
     * assign. The ownership-safe lowering hands the incoming container to the
     * store as an owned +1 transfer, so `this.values = this.values` stays
     * alive - the incoming reference is distinct from the field's own count -
     * and no defensive retain is required. The temporary is an SSA value
     * (`vN`), so match the shape, not a fixed name. */
    const char *store = strstr(replace, "xrt_release(xr_mkptr((p0)->f0, XR_TAG_ARRAY)), "
                                        "(p0)->f0 = (xrt_array_t *)");
    assert(store && store < replace_end &&
           count_between(replace, replace_end, "xrt_release(xr_mkptr((p0)->f0, XR_TAG_ARRAY))") ==
               1 &&
           "direct native receiver collection ref stores release the old container once");
    /* Ownership of the stored container arrives from the call site, which
     * retains an argument it still uses afterwards and moves one it does not.
     * The callee therefore never retains again: a second retain here would be
     * the leak that pairs with the single release above. */
    assert(count_between(replace, replace_end, "xrt_retain(") == 0 &&
           "the callee does not retain the stored container; the call site owns that decision");

    const char *self_assign = strstr(code, "static int64_t test_selfAssign_");
    assert(self_assign != NULL && "selfAssign method should use typed ABI");
    self_assign = strstr(self_assign + 1, "static int64_t test_selfAssign_");
    assert(self_assign != NULL && "selfAssign method definition should follow its declaration");
    const char *self_assign_end = next_static_after(self_assign);
    const char *self_assign_retain = strstr(self_assign, "xrt_retain(");
    const char *self_assign_store =
        strstr(self_assign, "xrt_release(xr_mkptr((p0)->f0, XR_TAG_ARRAY)), "
                            "(p0)->f0 = (xrt_array_t *)");
    assert(self_assign_retain && self_assign_retain < self_assign_store &&
           self_assign_store < self_assign_end &&
           count_between(self_assign, self_assign_end, "xrt_retain(") == 1 &&
           count_between(self_assign, self_assign_end,
                         "xrt_release(xr_mkptr((p0)->f0, XR_TAG_ARRAY))") == 1 &&
           "ARC must retain a self-assigned field before the consuming store releases it");

    const char *swap = strstr(code, "static int64_t test_swap_");
    assert(swap != NULL && "swap function should use typed scalar return ABI");
    swap = strstr(swap + 1, "static int64_t test_swap_");
    assert(swap != NULL && "swap function definition should follow its declaration");
    const char *swap_end = next_static_after(swap);
    assert(count_between(swap, swap_end, "heap_type == XR_TINSTANCE") == 0 &&
           count_between(swap, swap_end, "xrt_getprop_name(") == 0 &&
           count_between(swap, swap_end, "xrt_setprop_name(") == 0 &&
           count_between(swap, swap_end, "xrt_map_get(") == 0 &&
           count_between(swap, swap_end, "xrt_map_set(") == 0 &&
           "native class pointer parameters should access ref fields without Map fallback");
    /* Same comma-expression store as the direct receiver above, through the
     * heap instance pointer this time. The portable lowering names that
     * pointer `_portable_native_N`, so match the shape, not a fixed name. */
    const char *swap_store = strstr(swap, "xrt_release(xr_mkptr((_portable_native_");
    assert(swap_store && swap_store < swap_end &&
           count_between(swap, swap_end, "xrt_release(xr_mkptr((_portable_native_") == 1 &&
           "heap native instance collection ref stores release the old container once");
    assert(count_between(swap, swap_end, "xrt_retain(") == 0 &&
           "pointer field transfer must not add an unbalanced retain");

    const char *local = strstr(code, "static int64_t test_local_");
    assert(local != NULL && "local function should use typed scalar return ABI");
    local = strstr(local + 1, "static int64_t test_local_");
    assert(local != NULL && "local function definition should follow its declaration");
    const char *local_end = next_static_after(local);
    assert(count_between(local, local_end, "xrt_obj_alloc(") == 1 &&
           count_between(local, local_end, "_ci") == 0 &&
           "ref-field native classes should not stack-inline without stack destructors");

    printf("  Generated native class collection ref field ARC path %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_class_set_length_size_sum_uses_native_arithmetic) {
    const char *src = "class Bag {\n"
                      "    values: Set<i64>\n"
                      "    constructor() {\n"
                      "        this.values = #[]\n"
                      "    }\n"
                      "    fill(n: i64) -> i64 {\n"
                      "        var i = 0\n"
                      "        while (i < n) {\n"
                      "            this.values.add(i)\n"
                      "            i = i + 1\n"
                      "        }\n"
                      "        return len(this.values) + len(this.values)\n"
                      "    }\n"
                      "}\n"
                      "var bag = Bag()\n"
                      "print(bag.fill(10))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "class Set<i64> length/size sum should generate");

    const char *fn = strstr(code, "static int64_t test_fill_");
    assert(fn != NULL && "fill method should use typed ABI");
    fn = strstr(fn + 1, "static int64_t test_fill_");
    assert(fn != NULL && "fill method definition should follow its declaration");
    const char *fn_end = next_static_after(fn);

    assert(contains(code, "xrt_set_new_typed(0, XR_ELEM_I64)") &&
           "Set<i64> class field constructor should use typed i64 set storage");
    assert(count_between(fn, fn_end, "xrt_set_add_i64(") == 1 &&
           "Set<i64>.add should use the i64 direct helper");
    assert(count_between(fn, fn_end, "int64_t v") > 0 && count_between(fn, fn_end, "->len") > 0 &&
           "len(Set<i64>) should be materialized as a scalar field load");
    assert(count_between(fn, fn_end, "XR_FROM_INT((p0)->") == 0 &&
           "len(Set<i64>) should not box the native length field");
    assert(count_between(fn, fn_end, "xrt_add(") == 0 &&
           "repeated len(Set<i64>) should use native integer arithmetic");
    assert(count_between(fn, fn_end, "(int64_t)((uint64_t)(") > 0 &&
           count_between(fn, fn_end, "xrt_i64_add(") == 0 &&
           "repeated len(Set<i64>) should emit inline native wrap arithmetic");

    printf("  Generated class Set<i64> scalar length/size fast path %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_class_set_u8_uses_typed_direct_helpers) {
    const char *src = "class Bag {\n"
                      "    values: Set<u8>\n"
                      "    constructor() {\n"
                      "        this.values = #[]\n"
                      "    }\n"
                      "    fill(n: i64) -> i64 {\n"
                      "        var i = 0\n"
                      "        while (i < n) {\n"
                      "            this.values.add(i as u8)\n"
                      "            i = i + 1\n"
                      "        }\n"
                      "        return len(this.values)\n"
                      "    }\n"
                      "    scan(n: i64) -> i64 {\n"
                      "        var i = 0\n"
                      "        var hits = 0\n"
                      "        while (i < n) {\n"
                      "            if (this.values.contains(i as u8)) {\n"
                      "                hits = hits + i\n"
                      "            }\n"
                      "            i = i + 1\n"
                      "        }\n"
                      "        return hits\n"
                      "    }\n"
                      "    prune(n: i64) -> i64 {\n"
                      "        var i = 0\n"
                      "        var removed = 0\n"
                      "        while (i < n) {\n"
                      "            if (this.values.delete(i as u8)) {\n"
                      "                removed = removed + 1\n"
                      "            }\n"
                      "            i = i + 2\n"
                      "        }\n"
                      "        return removed\n"
                      "    }\n"
                      "}\n"
                      "var bag = Bag()\n"
                      "print(bag.fill(10) + bag.scan(10) + bag.prune(10))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "class Set<u8> direct helpers should generate");

    const char *fill = strstr(code, "static int64_t test_fill_");
    assert(fill != NULL && "fill method should use typed ABI");
    fill = strstr(fill + 1, "static int64_t test_fill_");
    assert(fill != NULL && "fill method definition should follow its declaration");
    const char *fill_end = next_static_after(fill);
    const char *scan = strstr(code, "static int64_t test_scan_");
    assert(scan != NULL && "scan method should use typed ABI");
    scan = strstr(scan + 1, "static int64_t test_scan_");
    assert(scan != NULL && "scan method definition should follow its declaration");
    const char *scan_end = next_static_after(scan);
    const char *prune = strstr(code, "static int64_t test_prune_");
    assert(prune != NULL && "prune method should use typed ABI");
    prune = strstr(prune + 1, "static int64_t test_prune_");
    assert(prune != NULL && "prune method definition should follow its declaration");
    const char *prune_end = next_static_after(prune);

    assert(contains(code, "xrt_set_new_typed(0, XR_ELEM_U8)") &&
           "Set<u8> class field constructor should use byte set storage");
    assert(count_between(fill, fill_end, "xrt_set_add_i64_typed(") == 1 &&
           "Set<u8>.add should use the typed integer direct helper");
    assert(count_between(scan, scan_end, "xrt_set_has_i64_typed(") == 1 &&
           "Set<u8>.has should use the typed integer direct helper");
    assert(count_between(prune, prune_end, "xrt_set_delete_i64_typed(") == 1 &&
           "Set<u8>.delete should use the typed integer direct helper");
    assert(contains(code, "XR_ELEM_U8") && "Set<u8> helper calls should pass XR_ELEM_U8");
    assert(!contains(code, "xrt_set_add(") && !contains(code, "xrt_set_has(") &&
           !contains(code, "xrt_set_delete(") &&
           "Set<u8> class hot methods should not fall back to tagged set helpers");

    xr_free(code);
    xi_func_free(ir);
}
TEST(cgen_class_map_i64_i64_uses_typed_direct_helpers) {
    const char *src = "class Bag {\n"
                      "    values: Map<i64, i64>\n"
                      "    constructor() {\n"
                      "        this.values = #{}\n"
                      "    }\n"
                      "    fill(n: i64) -> i64 {\n"
                      "        var i = 0\n"
                      "        while (i < n) {\n"
                      "            this.values.set(i, i * 3 + 1)\n"
                      "            i = i + 1\n"
                      "        }\n"
                      "        return len(this.values)\n"
                      "    }\n"
                      "    scan(n: i64) -> i64 {\n"
                      "        var i = 0\n"
                      "        var hits = 0\n"
                      "        while (i < n) {\n"
                      "            if (this.values.containsKey(i)) {\n"
                      "                hits = hits + this.values.get(i)!\n"
                      "            }\n"
                      "            i = i + 1\n"
                      "        }\n"
                      "        return hits\n"
                      "    }\n"
                      "    prune(n: i64) -> i64 {\n"
                      "        var i = 0\n"
                      "        var removed = 0\n"
                      "        while (i < n) {\n"
                      "            if (this.values.delete(i)) {\n"
                      "                removed = removed + 1\n"
                      "            }\n"
                      "            i = i + 2\n"
                      "        }\n"
                      "        return removed\n"
                      "    }\n"
                      "}\n"
                      "var bag = Bag()\n"
                      "print(bag.fill(10) + bag.scan(10) + bag.prune(10))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "class Map<i64,i64> direct helpers should generate");

    const char *fill = strstr(code, "static int64_t test_fill_");
    assert(fill != NULL && "fill method should use typed ABI");
    fill = strstr(fill + 1, "static int64_t test_fill_");
    assert(fill != NULL && "fill method definition should follow its declaration");
    const char *fill_end = next_static_after(fill);
    const char *scan = strstr(code, "static int64_t test_scan_");
    assert(scan != NULL && "scan method should use typed ABI");
    scan = strstr(scan + 1, "static int64_t test_scan_");
    assert(scan != NULL && "scan method definition should follow its declaration");
    const char *scan_end = next_static_after(scan);
    const char *prune = strstr(code, "static int64_t test_prune_");
    assert(prune != NULL && "prune method should use typed ABI");
    prune = strstr(prune + 1, "static int64_t test_prune_");
    assert(prune != NULL && "prune method definition should follow its declaration");
    const char *prune_end = next_static_after(prune);

    assert(contains(code, "xrt_map_new_typed(0, XR_ELEM_I64, XR_ELEM_I64)") &&
           "Map<i64,i64> class field constructor should use typed map storage");
    assert(count_between(fill, fill_end, "xrt_map_set_i64_i64_typed(") == 1 &&
           "Map<i64,i64>.set should use the typed integer direct helper");
    assert(count_between(scan, scan_end, "xrt_map_has_i64_typed(") == 0 &&
           "Map<i64,i64>.has guarding a get fuses into the get's find, not a separate has probe");
    assert(count_between(scan, scan_end, "xrt_map_find_i64_typed(") == 1 &&
           count_between(scan, scan_end, "xrt_map_get_i64_value_typed(") == 1 &&
           "Map<i64,i64>.has+get guarded should share one typed find plus one typed value load");
    assert(count_between(scan, scan_end, "_mf") >= 2 &&
           "the fused has should write _mf and the guarded get should read it back");
    assert(count_between(scan, scan_end, "\n    XrValue v") == 0 &&
           count_between(scan, scan_end, "XR_FROM_INT(") == 0 &&
           count_between(scan, scan_end, "xrt_add(") == 0 &&
           "Map<i64,i64>.get guarded by has should avoid tagged result and arithmetic");
    assert(count_between(prune, prune_end, "xrt_map_delete_i64_typed(") == 1 &&
           "Map<i64,i64>.delete should use the typed integer direct helper");
    assert(count_between(fill, fill_end, "xrt_map_set(") == 0 &&
           count_between(scan, scan_end, "xrt_map_has(") == 0 &&
           count_between(scan, scan_end, "xrt_map_get(") == 0 &&
           count_between(prune, prune_end, "xrt_map_delete(") == 0 &&
           "Map<i64,i64> class hot methods should not fall back to boxed map helpers");

    xr_free(code);
    xi_func_free(ir);
}
TEST(cgen_class_bool_key_map_uses_specialized_direct_helpers) {
    const char *src =
        "class Bag { values: Map<bool, f32>\n"
        "constructor() { this.values = #{} } fill() -> i64 { this.values.set(true, 1.5); "
        "this.values.set(false, 2.25); return len(this.values) } "
        "scan() -> f64 { var sum = 0.0; if (this.values.containsKey(true)) { sum = sum + "
        "this.values.get(true)! }; if (this.values.containsKey(false)) { sum = sum + "
        "this.values.get(false)! "
        "}; return sum } prune() -> i64 { if (this.values.delete(false)) { return "
        "len(this.values) }; return 0 }\n"
        "} class IntBag { values: Map<bool, i64>\n"
        "constructor() { this.values = #{} } fill() -> i64 { this.values.set(true, 11); "
        "this.values.set(false, 23); return len(this.values) } "
        "scan() -> i64 { var sum = 0; if (this.values.containsKey(true)) { sum = sum + "
        "this.values.get(true)! }; if (this.values.containsKey(false)) { sum = sum + "
        "this.values.get(false)! "
        "}; return sum } prune() -> i64 { if (this.values.delete(false)) { return "
        "len(this.values) }; return 0 }\n"
        "} var bag = Bag(); print(bag.fill() + bag.prune()); print(bag.scan())\n"
        "var int_bag = IntBag(); print(int_bag.fill() + int_bag.prune()); print(int_bag.scan())\n";
    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");
    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "class bool-key Map direct helpers should generate");
    const char *code_end = code + strlen(code);
    /* Map<bool,i64|f32> lowers to the 2-slot boolmap representation: created via
     * xrt_boolmap_new_typed and probed with the dispatch-free boolmap helpers. */
    assert(contains(code, "xrt_boolmap_new_typed(0, XR_ELEM_F32)"));
    assert(count_between(code, code_end, "xrt_map_set_bool_f32_typed(") == 2 &&
           count_between(code, code_end, "xrt_map_delete_bool_f32_typed(") == 1);
    assert(contains(code, "xrt_boolmap_new_typed(0, XR_ELEM_I64)"));
    assert(count_between(code, code_end, "xrt_map_set_bool_i64_typed(") == 2 &&
           count_between(code, code_end, "xrt_map_delete_bool_i64_typed(") == 1);
    /* Each guarded get loads a slot value through the boolmap value helpers,
     * regardless of has+get probe fusion (a fused has writes the slot index the
     * get reads back; an unfused has keeps its own boolmap find). */
    assert(count_between(code, code_end, "xrt_boolmap_value_f64(") == 2 &&
           count_between(code, code_end, "xrt_boolmap_value_i64(") == 2 &&
           "bool-key Map.get should load values via the boolmap helpers");
    assert(count_between(code, code_end, "xrt_boolmap_find(") >= 2 &&
           "guarded bool-key gets should probe via the boolmap find helper");
    assert(!contains(code, "xrt_map_has(") && !contains(code, "xrt_map_get(") &&
           "bool-key Map hot methods must not fall back to boxed map helpers");
    assert(!contains(code, "xrt_map_find_bool_typed(") &&
           !contains(code, "xrt_map_get_i64_value_typed(") &&
           !contains(code, "xrt_map_get_f64_value_typed(") &&
           "Map<bool,i64|f32> must use the boolmap representation, not the generic typed probes");
    assert(!contains(code, "xrt_map_set_i64_i64_typed(") &&
           !contains(code, "xrt_map_find_i64_typed(") &&
           !contains(code, "xrt_map_get_i64_i64_typed(") &&
           !contains(code, "xrt_map_set_i64_f64_typed(") &&
           !contains(code, "xrt_map_has_i64_typed(") &&
           !contains(code, "xrt_map_get_i64_f64_typed(") &&
           !contains(code, "xrt_map_delete_i64_typed(") &&
           "bool-key Map class hot methods should not use generic i64 helpers");
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_class_map_bool_value_guarded_condition_uses_native) {
    const char *src =
        "class Bag { values: Map<i64, bool>\n"
        "constructor() { this.values = #{} } "
        "fill(n: i64) -> i64 { var i = 0; while (i < n) { this.values.set(i, i % 3 == 0); "
        "i = i + 1 }; return len(this.values) } "
        "count(n: i64) -> i64 { var i = 0; var total = 0; while (i < n) { "
        "if (this.values.containsKey(i)) { if (this.values.get(i) == true) { total = total + 1 } "
        "}; "
        "i = i + 1 }; return total }\n"
        "} var bag = Bag(); print(bag.fill(8)); print(bag.count(8))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");
    char *code = generate_c(ir, "test");
    assert(code != NULL && "C code generation failed");
    const char *code_end = code + strlen(code);
    const char *count = strstr(code, "static int64_t test_count_");
    assert(count != NULL && "count method should use typed ABI");
    count = strstr(count + 1, "static int64_t test_count_");
    assert(count != NULL && "count method definition should follow its declaration");
    const char *count_end = next_static_after(count);
    assert(count_between(code, code_end, "xrt_map_find_i64_typed(") >= 1 &&
           count_between(code, code_end, "xrt_map_get_i64_value_typed(") >= 1 &&
           "Map<i64,bool>.get guarded by has should keep typed storage");
    assert(count_between(count, count_end, "\n    XrValue v") == 0 &&
           count_between(count, count_end, "XR_FROM_BOOL(") >= 1 &&
           count_between(count, count_end, "xr_truthy(") >= 1 &&
           "Map<i64,bool>.get condition should consume the stable truthiness owner adapter");

    printf("  Generated guarded bool map condition %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_class_map_bool_value_unguarded_explicit_true_uses_tagged_compare) {
    const char *src = "class Bag { values: Map<i64, bool>\n"
                      "constructor() { this.values = #{}; this.values.set(1, true) } "
                      "count() -> i64 { if (this.values.get(1) == true) { return 1 }; return 0 }\n"
                      "} var bag = Bag(); print(bag.count())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");
    char *code = generate_c(ir, "test");
    assert(code != NULL && "C code generation failed");
    const char *count = strstr(code, "static int64_t test_count_");
    assert(count != NULL && "count method should use typed ABI");
    count = strstr(count + 1, "static int64_t test_count_");
    assert(count != NULL && "count method definition should follow its declaration");
    const char *count_end = next_static_after(count);
    assert(count_between(count, count_end, "XrValue ") >= 1 &&
           count_between(count, count_end, "XR_FROM_BOOL(") >= 1 &&
           count_between(count, count_end, "xrt_eq(") >= 1 &&
           count_between(count, count_end, "xr_truthy(") >= 1 &&
           "unguarded Map<i64,bool>.get comparison should consume the stable truthiness owner "
           "adapter");

    printf("  Generated unguarded bool map explicit comparison %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_inherited_class_uses_native_base_layout) {
    const char *src = "class Shape {\n"
                      "    kind: i64\n"
                      "    constructor(kind: i64) {\n"
                      "        this.kind = kind\n"
                      "    }\n"
                      "    kind_plus() -> i64 {\n"
                      "        return this.kind + 7\n"
                      "    }\n"
                      "}\n"
                      "class Rect extends Shape {\n"
                      "    w: i64\n"
                      "    h: i64\n"
                      "    constructor(w: i64, h: i64) {\n"
                      "        super(1)\n"
                      "        this.w = w\n"
                      "        this.h = h\n"
                      "    }\n"
                      "    area() -> i64 {\n"
                      "        return this.w * this.h\n"
                      "    }\n"
                      "    kind_plus() -> i64 {\n"
                      "        return this.kind + this.w\n"
                      "    }\n"
                      "    score_with_area() -> i64 {\n"
                      "        return this.area() + this.kind\n"
                      "    }\n"
                      "}\n"
                      "var r = Rect(2, 3)\n"
                      "print(r.area())\n"
                      "print(r.kind_plus())\n"
                      "print(r.score_with_area())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "inherited class native layout should generate");

    assert(
        contains(code, "typedef struct xrt_native_test_Shape { XrObjHeader hdr; int64_t f0; }") &&
        "base class should embed the canonical object header at offset zero");
    assert(contains(code, "typedef struct xrt_native_test_Rect { xrt_native_test_Shape base; "
                          "int64_t f1; int64_t f2; }") &&
           "derived class should embed the base layout as a prefix");
    assert(contains(code, "&((p0)->base)") &&
           "super constructor should receive a pointer to the embedded base layout");

    const char *area = strstr(code, "int64_t test_area_");
    assert(area != NULL && "area method should use typed ABI");
    area = strstr(area + 1, "int64_t test_area_");
    assert(area != NULL && "area method definition should follow its declaration");
    const char *area_end = next_static_after(area);
    assert(contains(area, "xrt_native_test_Rect *p0") &&
           "derived method receiver should use native pointer ABI");
    assert(count_between(area, area_end, "p0->f1") > 0 &&
           count_between(area, area_end, "p0->f2") > 0 &&
           "derived method should read own fields from native layout");
    assert(count_between(area, area_end, "xrt_map_get") == 0 &&
           count_between(area, area_end, "xrt_map_set") == 0 &&
           "derived method body should not cross the map boundary");

    const char *kind = strstr(area_end, "int64_t test_kind_plus_");
    assert(kind != NULL && "derived kind_plus method should follow area adapter");
    const char *kind_end = next_static_after(kind);
    assert(contains(kind, "xrt_native_test_Rect *p0") &&
           "derived override receiver should use native pointer ABI");
    assert(count_between(kind, kind_end, "p0->base.f0") > 0 &&
           "derived method should read inherited fields through the native base prefix");
    assert(count_between(kind, kind_end, "p0->f1") > 0 &&
           "derived method should still read own fields directly");
    assert(count_between(kind, kind_end, "xrt_map_get") == 0 &&
           count_between(kind, kind_end, "xrt_map_set") == 0 &&
           "inherited field hot path should not cross the map boundary");

    const char *score = strstr(kind_end, "int64_t test_score_with_area_");
    assert(score != NULL && "score_with_area method should follow kind_plus adapter");
    const char *score_end = next_static_after(score);
    assert(count_between(score, score_end, "test_area_") > 0 &&
           "nested native method call should remain a direct C call");
    assert(count_between(score, score_end, "base.f0") > 0 &&
           "nested native method call body should still read inherited fields directly");
    assert(count_between(score, score_end, "xrt_has_pending_error") == 0 &&
           "no-throw nested native method call must not keep a dead error check");

    printf("  Generated inherited class native layout %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_slice_preserves_raw_storage_fast_path) {
    const char *src = "fn sum() -> i64 {\n"
                      "    var bytes: Array<u8> = []\n"
                      "    bytes.push(1)\n"
                      "    bytes.push(2)\n"
                      "    bytes.push(3)\n"
                      "    var mid: Slice<u8> = bytes[1:3]\n"
                      "    return i64(mid[0]) + len(mid)\n"
                      "}\n"
                      "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed array slice fast path should generate");
    assert(contains(code, "xrt_span_from_array_slice(") &&
           "typed array slice must stay in native borrowed-span storage");
    assert(contains(code, "XR_ELEM_U8") && "Array<u8> source must keep byte storage");
    assert((contains(code, "((uint8_t*)_a->data)") || contains(code, "uint8_t *_ad")) &&
           "Array<u8> slice reads must access raw byte storage");
    assert(contains(code, "((xrt_array_t*)") &&
           "Array<u8> slice length must read the runtime array length directly");
    assert(!contains(code, "xrt_index_get(") &&
           "Array<u8> slice index read must not fall back to runtime index dispatch");
    assert(!contains(code, "xrt_getprop(") &&
           "Array<u8> slice length must not fall back to dynamic property dispatch");
    assert(!contains(code, "xrt_method_") &&
           "Array<u8> slice expression must not use dynamic method dispatch");

    printf("  Generated typed array slice fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typename_as_and_slice_use_direct_drivers) {
    const char *src = "fn run() -> i64 {\n"
                      "    var arr: Array<i64> = [1, 2, 3]\n"
                      "    var s = copy(arr[0:2])\n"
                      "    var label = 42 as string\n"
                      "    if (typeName(s) == \"Array\" && label == \"42\") {\n"
                      "        return len(s)\n"
                      "    }\n"
                      "    return 0\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typename/as/slice direct drivers should generate");

    assert(contains(code, "xrt_typename(") &&
           "typeName() must use the direct AOT type-name helper");
    assert(contains(code, "xrt_to_string(") &&
           "unsafe as string must use the direct AOT conversion helper");
    assert(contains(code, "xrt_span_from_array_slice(") &&
           "slice expression must use the native borrowed-span helper before copy");
    assert(!contains(code, "xr_typename(") && !contains(code, "xr_typeof_id(") &&
           "AOT code must not reference stale typeof helper names");

    printf("  Generated typename/as/slice direct drivers %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typeid_uses_stable_owner_adapter) {
    const char *src = "var stableType = Type.i64\n"
                      "stableType = typeOf(42)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "typeOf IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && !had_error && "stable-owner typeOf C generation failed");
    assert(contains(code, "xrt_typeof_id(") &&
           "XI_TYPEID must consume the generated stable-owner CGen adapter");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_exact_bits_use_stable_owner_adapter) {
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_BITS_HI,
                                          XR_SEM_OWNER_ID_SHARED_BITS_LO, XR_SEM_CONSUMER_CGEN) &&
           "exact-width bits owner must publish CGen as a mechanical consumer");
    const char *adapter = xr_semantic_owner_cgen_adapter(XR_SEM_OWNER_ID_SHARED_BITS_HI,
                                                         XR_SEM_OWNER_ID_SHARED_BITS_LO);
    assert(adapter != NULL && strcmp(adapter, "xrt_bits_exact_eval") == 0 &&
           "CGen must resolve the stable exact-width bit owner adapter");
}

TEST(cgen_bits_not_uses_stable_owner_adapter) {
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_BITS_NOT_HI,
                                          XR_SEM_OWNER_ID_SHARED_BITS_NOT_LO,
                                          XR_SEM_CONSUMER_CGEN) &&
           "bitwise-not owner must publish CGen as a mechanical consumer");
    const char *adapter = xr_semantic_owner_cgen_adapter(XR_SEM_OWNER_ID_SHARED_BITS_NOT_HI,
                                                         XR_SEM_OWNER_ID_SHARED_BITS_NOT_LO);
    assert(adapter != NULL && strcmp(adapter, "xrt_bits_not_eval") == 0 &&
           "CGen must resolve the stable bitwise-not owner adapter");

    const char *src = "fn bitsNotOwner(value: i64) -> i64 {\n"
                      "    return ~value\n"
                      "}\n"
                      "print(bitsNotOwner(-1))\n";
    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "bitwise-not IR compilation failed");
    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && !had_error && "stable-owner bitwise-not C generation failed");
    assert(contains(code, "xrt_bits_not_eval(") &&
           "generated C must call the stable bitwise-not owner adapter");
    assert(!contains(code, "~(") && "generated C must not recreate bitwise-not semantics");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_numeric_neg_uses_stable_owner_adapter) {
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_HI,
                                          XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_LO,
                                          XR_SEM_CONSUMER_CGEN) &&
           "numeric-neg owner must publish CGen as a mechanical consumer");
    const char *adapter = xr_semantic_owner_cgen_adapter(XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_HI,
                                                         XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_LO);
    assert(adapter != NULL && strcmp(adapter, "xrt_numeric_neg_eval") == 0 &&
           "CGen must resolve the stable numeric-neg owner adapter");

    const char *src = "fn negInt(value: i64) -> i64 { return -value }\n"
                      "fn negFloat(value: f64) -> f64 { return -value }\n"
                      "fn negBig(value: BigInt) -> BigInt { return -value }\n"
                      "print(negInt(42))\n"
                      "print(negFloat(1.5))\n";
    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "numeric-neg IR compilation failed");
    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && !had_error && "stable-owner numeric-neg C generation failed");
    assert(contains(code, "xrt_numeric_neg_eval(XR_NUMERIC_NEG_I64") &&
           contains(code, "xrt_numeric_neg_eval(XR_NUMERIC_NEG_F64") &&
           "generated scalar C must call the numeric-neg owner adapter");
    assert(!contains(code, "-(uint64_t)") &&
           "generated C must not recreate integer negation semantics");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_bitwise_binary_uses_stable_owner_adapter) {
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_BITWISE_BINARY_HI,
                                          XR_SEM_OWNER_ID_SHARED_BITWISE_BINARY_LO,
                                          XR_SEM_CONSUMER_CGEN) &&
           "bitwise-binary owner must publish CGen as a mechanical consumer");
    const char *adapter = xr_semantic_owner_cgen_adapter(XR_SEM_OWNER_ID_SHARED_BITWISE_BINARY_HI,
                                                         XR_SEM_OWNER_ID_SHARED_BITWISE_BINARY_LO);
    assert(adapter != NULL && strcmp(adapter, "xrt_bitwise_binary_eval") == 0 &&
           "CGen must resolve the stable bitwise-binary owner adapter");

    const char *src = "fn ownerAnd(a: i64, b: i64) -> i64 { return a & b }\n"
                      "fn ownerOr(a: i64, b: i64) -> i64 { return a | b }\n"
                      "fn ownerXor(a: i64, b: i64) -> i64 { return a ^ b }\n"
                      "print(ownerAnd(-1, 85))\n"
                      "print(ownerOr(-8, 3))\n"
                      "print(ownerXor(-1, -9223372036854775807 - 1))\n";
    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "bitwise-binary IR compilation failed");
    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && !had_error && "stable-owner bitwise-binary C generation failed");
    assert(count_between(code, code + strlen(code), "xrt_bitwise_binary_eval(") >= 3 &&
           "all bitwise-binary operators must call the stable owner adapter");
    assert(!contains(code, "xrt_bigint_and_val") && !contains(code, "xrt_bigint_or_val") &&
           !contains(code, "xrt_bigint_xor_val") &&
           "generated C must not name retired per-operation BigInt helpers");
    assert(!contains(code, ") & (") && !contains(code, ") | (") && !contains(code, ") ^ (") &&
           "generated C must not recreate raw bitwise-binary semantics");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_numeric_width_uses_stable_owner_adapter) {
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,
                                          XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO,
                                          XR_SEM_CONSUMER_CGEN) &&
           "numeric width owner must publish CGen as a mechanical consumer");
    const char *adapter = xr_semantic_owner_cgen_adapter(
        XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI, XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO);
    assert(adapter != NULL && strcmp(adapter, "xrt_numeric_width_eval") == 0 &&
           "CGen must resolve the stable numeric width owner adapter");
}

TEST(cgen_byte_slice_scalar_uses_stable_owner_adapter) {
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_HI,
                                          XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_LO,
                                          XR_SEM_CONSUMER_CGEN) &&
           "byte-slice scalar owner must publish CGen as a mechanical consumer");
    const char *adapter = xr_semantic_owner_cgen_adapter(
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_HI, XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_LO);
    assert(adapter != NULL && strcmp(adapter, "xrt_byte_slice_scalar_eval") == 0 &&
           "CGen must resolve the stable byte-slice scalar owner adapter");
}

TEST(cgen_byte_slice_compare_uses_stable_owner_adapter) {
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMPARE_HI,
                                          XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMPARE_LO,
                                          XR_SEM_CONSUMER_CGEN) &&
           "byte-slice compare owner must publish CGen as a mechanical consumer");
    const char *adapter = xr_semantic_owner_cgen_adapter(
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMPARE_HI, XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMPARE_LO);
    assert(adapter != NULL && strcmp(adapter, "xrt_byte_slice_compare_checked_raw") == 0 &&
           "CGen must resolve the stable byte-slice compare owner adapter");

    const char *src = "fn compareBytes() -> i64 {\n"
                      "    var left = Array<u8>(2)\n"
                      "    var right = Array<u8>(3)\n"
                      "    var leftView: Slice<u8> = left[:]\n"
                      "    return leftView.compare(right[:])\n"
                      "}\n"
                      "print(compareBytes())\n";
    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "byte-slice compare IR compilation failed");
    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && !had_error && "stable-owner byte-slice compare C generation failed");
    const char *fn = find_static_function_definition(code, "test_compareBytes_");
    assert(fn != NULL && "byte-slice compare definition must be emitted");
    const char *fn_end = next_static_after(fn);
    assert(contains_between(fn, fn_end, "xrt_byte_slice_compare_checked_raw(") &&
           "generated C must call the stable byte-slice compare owner adapter");
    assert(!contains_between(fn, fn_end, "memcmp(") &&
           "generated C must not recreate byte-slice compare semantics");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_byte_slice_fill_uses_stable_owner_adapter) {
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_FILL_HI,
                                          XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_FILL_LO,
                                          XR_SEM_CONSUMER_CGEN) &&
           "byte-slice fill owner must publish CGen as a mechanical consumer");
    const char *adapter = xr_semantic_owner_cgen_adapter(XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_FILL_HI,
                                                         XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_FILL_LO);
    assert(adapter != NULL && strcmp(adapter, "xrt_byte_slice_fill_checked_raw") == 0 &&
           "CGen must resolve the stable byte-slice fill owner adapter");

    const char *src = "fn fillBytes(value: u8) -> u8 {\n"
                      "    var bytes = Array<u8>(4)\n"
                      "    var view: Slice<u8> = bytes[:]\n"
                      "    view.fill(value)\n"
                      "    return view[0]\n"
                      "}\n"
                      "print(fillBytes(255))\n";
    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "byte-slice fill IR compilation failed");
    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && !had_error && "stable-owner byte-slice fill C generation failed");
    const char *fn = find_static_function_definition(code, "test_fillBytes_");
    assert(fn != NULL && "byte-slice fill definition must be emitted");
    const char *fn_end = next_static_after(fn);
    assert(contains_between(fn, fn_end, "xrt_byte_slice_fill_checked_raw(") &&
           "generated C must call the stable byte-slice fill owner adapter");
    assert(!contains_between(fn, fn_end, "memset(_s.data, (uint8_t)") &&
           !contains_between(fn, fn_end, "((uint8_t*)_s.data)[_i]") &&
           !contains_between(fn, fn_end, "({ xr_span_t _s =") &&
           "generated C must not recreate byte-slice fill semantics");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_byte_slice_mutation_uses_stable_owner_adapters) {
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COPY_HI,
                                          XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COPY_LO,
                                          XR_SEM_CONSUMER_CGEN));
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_REPEAT_HI,
                                          XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_REPEAT_LO,
                                          XR_SEM_CONSUMER_CGEN));
    assert(strcmp(xr_semantic_owner_cgen_adapter(XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COPY_HI,
                                                 XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COPY_LO),
                  "xrt_byte_slice_copy_checked_raw") == 0);
    assert(strcmp(xr_semantic_owner_cgen_adapter(XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_REPEAT_HI,
                                                 XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_REPEAT_LO),
                  "xrt_byte_slice_repeat_from_checked_raw") == 0);

    const char *src = "fn mutate() -> i64 {\n"
                      "    var bytes = Array<u8>(8)\n"
                      "    var view: Slice<u8> = bytes[:]\n"
                      "    var dst: Slice<u8> = view[2:6]\n"
                      "    var source: Slice<u8> = view[0:4]\n"
                      "    dst.copyFrom(source)\n"
                      "    view.repeatFrom(4, 2, 4)\n"
                      "    return i64(view[7])\n"
                      "}\n"
                      "print(mutate())\n";
    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "byte-slice mutation IR compilation failed");
    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && !had_error && "stable-owner byte-slice mutation generation failed");
    const char *fn = find_static_function_definition(code, "test_mutate_");
    assert(fn != NULL);
    const char *fn_end = next_static_after(fn);
    assert(contains_between(fn, fn_end, "xrt_byte_slice_copy_checked_raw(") &&
           contains_between(fn, fn_end, "xrt_byte_slice_repeat_from_checked_raw("));
    assert(!contains_between(fn, fn_end, "memcpy(_dst.data") &&
           !contains_between(fn, fn_end, "memmove(_dst.data") &&
           !contains_between(fn, fn_end, "((uint8_t*)_dst.data)") &&
           !contains_between(fn, fn_end, "xr_array_core_copy_or_move_bytes(_dst.data") &&
           !contains_between(fn, fn_end, "xr_array_core_bytes_repeat_copy(_span.data") &&
           !contains_between(fn, fn_end, "({ xr_span_t"));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_byte_slice_common_prefix_uses_stable_owner_adapter) {
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMMON_PREFIX_HI,
                                          XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMMON_PREFIX_LO,
                                          XR_SEM_CONSUMER_CGEN) &&
           "byte-slice common-prefix owner must publish CGen as a mechanical consumer");
    const char *adapter =
        xr_semantic_owner_cgen_adapter(XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMMON_PREFIX_HI,
                                       XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMMON_PREFIX_LO);
    assert(adapter != NULL && strcmp(adapter, "xrt_byte_slice_common_prefix_checked_raw") == 0 &&
           "CGen must resolve the stable byte-slice common-prefix owner adapter");

    const char *src = "fn commonPrefix() -> i64 {\n"
                      "    var left = Array<u8>(9)\n"
                      "    var right = Array<u8>(9)\n"
                      "    var leftView: Slice<u8> = left[:]\n"
                      "    return leftView.commonPrefix(right[:])\n"
                      "}\n"
                      "print(commonPrefix())\n";
    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "byte-slice common-prefix IR compilation failed");
    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && !had_error &&
           "stable-owner byte-slice common-prefix C generation failed");
    const char *fn = find_static_function_definition(code, "test_commonPrefix_");
    assert(fn != NULL && "byte-slice common-prefix definition must be emitted");
    const char *fn_end = next_static_after(fn);
    assert(contains_between(fn, fn_end, "xrt_byte_slice_common_prefix_checked_raw(") &&
           "generated C must call the stable byte-slice common-prefix owner adapter");
    assert(!contains_between(fn, fn_end, "xr_byte_slice_common_prefix_core(") &&
           !contains_between(fn, fn_end, "xr_array_core_bytes_common_prefix_raw(") &&
           "generated C must not recreate byte-slice common-prefix semantics");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_pod_slice_copy_compare_use_stable_owner_adapters) {
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_POD_SLICE_COPY_HI,
                                          XR_SEM_OWNER_ID_SHARED_POD_SLICE_COPY_LO,
                                          XR_SEM_CONSUMER_CGEN));
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_POD_SLICE_COMPARE_HI,
                                          XR_SEM_OWNER_ID_SHARED_POD_SLICE_COMPARE_LO,
                                          XR_SEM_CONSUMER_CGEN));
    assert(strcmp(xr_semantic_owner_cgen_adapter(XR_SEM_OWNER_ID_SHARED_POD_SLICE_COPY_HI,
                                                 XR_SEM_OWNER_ID_SHARED_POD_SLICE_COPY_LO),
                  "xrt_span_copy_checked_raw") == 0);
    assert(strcmp(xr_semantic_owner_cgen_adapter(XR_SEM_OWNER_ID_SHARED_POD_SLICE_COMPARE_HI,
                                                 XR_SEM_OWNER_ID_SHARED_POD_SLICE_COMPARE_LO),
                  "xrt_span_compare_checked_raw") == 0);
}

TEST(cgen_pod_slice_fill_uses_stable_owner_adapter) {
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_POD_SLICE_FILL_HI,
                                          XR_SEM_OWNER_ID_SHARED_POD_SLICE_FILL_LO,
                                          XR_SEM_CONSUMER_CGEN));
    assert(strcmp(xr_semantic_owner_cgen_adapter(XR_SEM_OWNER_ID_SHARED_POD_SLICE_FILL_HI,
                                                 XR_SEM_OWNER_ID_SHARED_POD_SLICE_FILL_LO),
                  "xrt_span_fill_checked_raw") == 0);
}

TEST(cgen_pod_slice_view_uses_stable_owner_adapter) {
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_POD_SLICE_VIEW_HI,
                                          XR_SEM_OWNER_ID_SHARED_POD_SLICE_VIEW_LO,
                                          XR_SEM_CONSUMER_CGEN));
    assert(strcmp(xr_semantic_owner_cgen_adapter(XR_SEM_OWNER_ID_SHARED_POD_SLICE_VIEW_HI,
                                                 XR_SEM_OWNER_ID_SHARED_POD_SLICE_VIEW_LO),
                  "xrt_pod_slice_view_checked_raw") == 0);

    const char *src = "fn views(values: Array<u32>) -> i64 {\n"
                      "    var words: Slice<u32> = values[:]\n"
                      "    var bytes: Slice<u8> = words.asBytes()\n"
                      "    var roundtrip: Slice<u32> = bytes.reinterpret<u32>()\n"
                      "    return len(roundtrip)\n"
                      "}\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "POD view owner fixture compiled");
    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "POD view owner fixture generated C");
    const char *fn = find_static_function_definition(code, "test_views_");
    TEST_REQUIRE(fn != NULL, "POD view owner function emitted");
    const char *fn_end = next_static_after(fn);
    TEST_REQUIRE(count_between(fn, fn_end, "xrt_pod_slice_view_checked_raw(") == 2,
                 "both POD view operations call the stable owner adapter");
    TEST_REQUIRE(!contains_between(fn, fn_end, "_s.length *") &&
                     !contains_between(fn, fn_end, "_s.length /") &&
                     !contains_between(fn, fn_end, "_s.length %"),
                 "generated C does not recreate POD view semantics");
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_raw_memory_copy_owner_registry_is_stable) {
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_RAW_MEMORY_COPY_HI,
                                          XR_SEM_OWNER_ID_SHARED_RAW_MEMORY_COPY_LO,
                                          XR_SEM_CONSUMER_CGEN) &&
           "raw-memory-copy owner must publish CGen as a mechanical consumer");
    const char *adapter = xr_semantic_owner_cgen_adapter(XR_SEM_OWNER_ID_SHARED_RAW_MEMORY_COPY_HI,
                                                         XR_SEM_OWNER_ID_SHARED_RAW_MEMORY_COPY_LO);
    assert(adapter != NULL && strcmp(adapter, "xrt_raw_memory_copy_nonoverlap") == 0 &&
           "CGen must resolve the stable raw-memory-copy owner adapter");
}

TEST(cgen_enum_metadata_access_uses_stable_owner_adapter) {
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_HI,
                                          XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_LO,
                                          XR_SEM_CONSUMER_CGEN) &&
           "enum-metadata owner must publish CGen as a mechanical consumer");
    const char *adapter =
        xr_semantic_owner_cgen_adapter(XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_HI,
                                       XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_LO);
    assert(adapter != NULL && strcmp(adapter, "xrt_enum_metadata_access") == 0 &&
           "CGen must resolve the stable enum-metadata owner adapter");

    XrType int_type = {
        .kind = XR_KIND_INT, .id = 1311, .scalar_rep = XR_NATIVE_I64, .frozen = true};
    XiFunc *ir = xi_func_new("manual_enum_metadata_access", &int_type);
    TEST_REQUIRE(ir != NULL, "manual enum metadata owner fixture allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual enum metadata owner entry allocated");
    entry->sealed = true;
    XiValue *variant_count = xi_const_int(ir, entry, 3, &int_type);
    XiValue *variant_index = xi_const_int(ir, entry, 1, &int_type);
    XiValue *variant = xi_value_new(ir, entry, XI_ENUM_VARIANT_AT, &int_type, 2);
    XiValue *payload_view =
        xi_const_int(ir, entry, (int64_t) ((UINT64_C(2) << 32) | UINT64_C(9)), &int_type);
    XiValue *payload_index = xi_const_int(ir, entry, 1, &int_type);
    XiValue *payload = xi_value_new(ir, entry, XI_ENUM_PAYLOAD_AT, &int_type, 2);
    TEST_REQUIRE(variant_count && variant_index && variant && payload_view && payload_index &&
                     payload,
                 "manual enum metadata owner values allocated");
    variant->args[0] = variant_count;
    variant->args[1] = variant_index;
    payload->args[0] = payload_view;
    payload->args[1] = payload_index;
    xi_block_set_return(entry, payload);
    TEST_REQUIRE(test_prepare_backend_ir(ir), "enum metadata owner fixture reached Backend");
    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "enum metadata owner fixture generated C");
    TEST_REQUIRE(contains(code, "xrt_enum_metadata_access_variant_at(") &&
                     contains(code, "xrt_enum_metadata_access_payload_at("),
                 "both enum metadata operations call the stable owner adapter");
    TEST_REQUIRE(!contains(code, "({ int64_t _n =") && !contains(code, "({ uint64_t _p =") &&
                     !contains(code, "enum variant index out of bounds") &&
                     !contains(code, "enum payload field index out of bounds"),
                 "generated C does not recreate enum metadata semantics");
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_cell_access_uses_stable_owner_adapter) {
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_CELL_ACCESS_HI,
                                          XR_SEM_OWNER_ID_SHARED_CELL_ACCESS_LO,
                                          XR_SEM_CONSUMER_CGEN));
    const char *adapter = xr_semantic_owner_cgen_adapter(XR_SEM_OWNER_ID_SHARED_CELL_ACCESS_HI,
                                                         XR_SEM_OWNER_ID_SHARED_CELL_ACCESS_LO);
    TEST_REQUIRE(adapter != NULL && strcmp(adapter, "xrt_cell_access") == 0,
                 "cell access publishes its stable CGen adapter");

    const char *src = "fn mutateCell() -> i64 {\n"
                      "    var n = 1\n"
                      "    fn bump() { n = n + 2 }\n"
                      "    bump()\n"
                      "    return n\n"
                      "}\n"
                      "print(mutateCell())\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "cell-access owner fixture compiled");
    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "cell-access owner fixture generated C");
    TEST_REQUIRE(contains(code, "xrt_cell_access_get(") && contains(code, "xrt_cell_access_set("),
                 "cell get and set call the stable owner adapter");
    TEST_REQUIRE(!contains(code, "xrt_cell_get(") && !contains(code, "xrt_cell_set("),
                 "retired cell access adapters are absent");
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_null_test_uses_stable_owner_adapter) {
    assert(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_NULL_TEST_HI,
                                          XR_SEM_OWNER_ID_SHARED_NULL_TEST_LO,
                                          XR_SEM_CONSUMER_CGEN));
    const char *adapter = xr_semantic_owner_cgen_adapter(XR_SEM_OWNER_ID_SHARED_NULL_TEST_HI,
                                                         XR_SEM_OWNER_ID_SHARED_NULL_TEST_LO);
    TEST_REQUIRE(adapter != NULL && strcmp(adapter, "xrt_null_test") == 0,
                 "null test publishes its stable CGen adapter");

    XrType tagged_type = {.kind = XR_KIND_INT,
                          .id = 942,
                          .scalar_rep = XR_NATIVE_I64,
                          .is_nullable = true,
                          .frozen = true};
    XrType bool_type = {
        .kind = XR_KIND_BOOL, .id = 944, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    XiFunc *ir = xi_func_new("manual_null_test_owner", &tagged_type);
    TEST_REQUIRE(ir != NULL, "manual null-test owner function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual null-test owner entry allocated");
    entry->sealed = true;
    ir->nparams = 1;
    ir->min_params = 1;
    ir->params = (XiValue **) xr_calloc(1, sizeof(XiValue *));
    TEST_REQUIRE(ir->params != NULL, "manual null-test owner parameters allocated");
    XiValue *source = xi_param(ir, entry, 0, &tagged_type);
    ir->params[0] = source;
    XiValue *tagged_test = xi_value_new(ir, entry, XI_ISNULL, &bool_type, 1);
    XiValue *one = xi_const_int(ir, entry, 1, &tagged_type);
    XiValue *zero = xi_const_int(ir, entry, 0, &tagged_type);
    XiValue *selected = xi_value_new(ir, entry, XI_SELECT, &tagged_type, 3);
    TEST_REQUIRE(tagged_test != NULL, "manual tagged null test allocated");
    TEST_REQUIRE(one != NULL && zero != NULL && selected != NULL,
                 "manual null-test result consumer allocated");
    TEST_REQUIRE(source != NULL, "manual tagged null-test source allocated");
    tagged_test->args[0] = source;
    selected->args[0] = tagged_test;
    selected->args[1] = one;
    selected->args[2] = zero;
    xi_block_set_return(entry, selected);
    TEST_REQUIRE(test_prepare_backend_ir(ir), "null-test owner fixture reached Backend");
    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "null-test owner fixture generated C");
    TEST_REQUIRE(contains(code, "xrt_null_test_tagged("),
                 "tagged null tests call the stable owner adapter");
    TEST_REQUIRE(!contains(code, ".tag == XR_TAG_NULL") && !contains(code, " == NULL)"),
                 "generated C does not recreate null-test semantics");
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_force_unwrap_checktype_uses_portable_borrowed_helper) {
    const char *src = "fn forceUtf8(data: Array<u8>) -> string {\n"
                      "    return string.fromUtf8(data[:])!\n"
                      "}\n"
                      "var bytes = Array<u8>(1)\n"
                      "bytes[0] = 65\n"
                      "print(forceUtf8(bytes))\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "force-unwrap CHECKTYPE fixture compiled");
    TEST_REQUIRE(count_op_in_func(ir, XI_CHECKTYPE) > 0,
                 "force unwrap must exercise a real CHECKTYPE operation");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "force-unwrap CHECKTYPE C generated");
    const char *fn = find_static_function_definition(code, "test_forceUtf8_");
    TEST_REQUIRE(fn != NULL, "force-unwrap CHECKTYPE function emitted");
    const char *fn_end = next_static_after(fn);
    TEST_REQUIRE(contains_between(fn, fn_end, "xrt_check_type_borrowed("),
                 "CHECKTYPE uses the portable borrowed-identity helper");
    TEST_REQUIRE(!contains(code, "({ XrValue _ct = "),
                 "CHECKTYPE must not emit a GNU statement expression");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_span_index_set_checks_readonly_flag) {
    const char *src = "fn write(dst: ref Slice<u8>) {\n"
                      "    dst[0] = 7\n"
                      "}\n"
                      "fn run() {\n"
                      "    var values = Array<u8>(1)\n"
                      "    var view: Slice<u8> = values[:]\n"
                      "    write(ref view)\n"
                      "}\n"
                      "run()\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "Slice readonly fixture compiled to IR");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "Slice index write generated C");

    const char *fn = find_static_function_definition(code, "test_write_");
    TEST_REQUIRE(fn != NULL, "write definition exists");
    const char *fn_end = next_static_after(fn);
    TEST_REQUIRE(fn_end != NULL, "write function body is bounded");
    TEST_REQUIRE(count_between(fn, fn_end, "xrt_span_require_mutable(_s);") == 0,
                 "Slice index writes keep the 16-byte ABI free of runtime readonly checks");
    TEST_REQUIRE(count_between(fn, fn_end, "((uint8_t*)_s.data)[_idx]") == 1,
                 "Slice index writes remain in native span storage");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_same_type_as_lowers_away_without_arc) {
    const char *src = "class Box {\n"
                      "    value: i64\n"
                      "    constructor(value: i64) { this.value = value }\n"
                      "}\n"
                      "fn pass(box: move Box) -> Box { return box }\n"
                      "fn cast(box: move Box) -> Box { return pass(move box) as Box }\n"
                      "print(cast(Box(7)).value)\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "same-type class-cast fixture compiled");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "same-type class-cast C generated");
    TEST_REQUIRE(!contains(code, "xrt_retain_identity("),
                 "same-type as lowers away without redundant ARC traffic");
    TEST_REQUIRE(!contains(code, "XrValue _as"),
                 "same-type as leaves no C-level conversion temporary");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_closure_values_and_indirect_calls_use_portable_c) {
    const char *src = "fn bump(value: ref i64) -> i64 {\n"
                      "    value = value + 1\n"
                      "    return value\n"
                      "}\n"
                      "fn invoke(action: fn(ref i64) -> i64, value: ref i64) -> i64 {\n"
                      "    return action(ref value)\n"
                      "}\n"
                      "var value = 4\n"
                      "print(invoke(bump, ref value))\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "indirect closure-call fixture compiled");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "indirect closure-call C generated");
    TEST_REQUIRE(!contains(code, "({"),
                 "closure construction and indirect calls emit standard C11");
    TEST_REQUIRE(contains(code, ".sync_entry=(void (*)(void))"),
                 "callable descriptors use a standard generic function pointer");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_cell_backed_function_upvalue_uses_boxed_indirect_call) {
    const char *src = "fn outer() {\n"
                      "    fn cleanup(msg: string) { print(msg) }\n"
                      "    fn worker() { defer { cleanup(\"upval-cleanup\") } }\n"
                      "    worker()\n"
                      "}\n"
                      "outer()\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "cell-backed function-upvalue fixture compiled");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error,
                 "CELL_GET call target generated through the boxed closure entry");
    TEST_REQUIRE(contains(code, "->callable->sync_entry"),
                 "cell-backed function upvalue invokes its closure descriptor");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_range_uses_direct_aot_driver) {
    const char *src = "var r = 2..6\n"
                      "var ri = 2..=6\n"
                      "print(r)\n"
                      "print(ri)\n"
                      "print(typeName(r))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "range direct driver should generate");

    assert(contains(code, "xrt_range_from_core(xrt_range_semantics(") &&
           "range expression must use the direct AOT range helper");
    assert(contains(code, ", false)") && "half-open range must pass inclusive=false");
    assert(contains(code, ", true)") && "inclusive range must pass inclusive=true");
    assert(contains(code, "xrt_typename(") &&
           "typeName(range) must use the direct type-name helper");
    assert(!contains(code, "xrt_range_from_i64(") && !contains(code, "xrt_range(XR_FROM_INT") &&
           "range creation must not box start/end before the AOT helper");

    printf("  Generated range direct driver %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_slice_loop_uses_guarded_unchecked_raw_load) {
    const char *src = "fn sum(n: i64) -> i64 {\n"
                      "    var bytes: Array<u8> = []\n"
                      "    var i = 0\n"
                      "    while (i < n) {\n"
                      "        bytes.push(i as u8)\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    var mid: Slice<u8> = bytes[1:n - 1]\n"
                      "    var total = 0\n"
                      "    i = 0\n"
                      "    while (i < len(mid)) {\n"
                      "        total = total + i64(mid[i])\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    return total\n"
                      "}\n"
                      "print(sum(200))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed array guarded slice loop should generate");
    const char *code_end = code + strlen(code);
    assert((contains(code, "xrt_array_new_typed_uninit(") ||
            contains(code, "xrt_array_new_typed_uninit_ptr(")) &&
           "guarded Array<u8> fill loop must preallocate uninitialized typed storage");
    assert(contains(code, "uint8_t *_ad") &&
           "guarded Array<u8> fill loop must cache raw byte storage");
    /* The slice loop reads through the span value directly: the array data
     * cache is driven by a verified array-cache plan, and spans carry no such
     * plan, so the read stays `((u8*)_s.data)[_idx]` rather than a hoisted
     * pointer. Pin both halves so extending the cache plan to spans updates
     * this guard deliberately instead of silently. */
    assert(count_between(code, code_end, "uint8_t *_ad") >= 1 &&
           "guarded Array<u8> fill loop must cache the source data pointer");
    assert(contains(code, "((uint8_t*)_s.data)[_idx]") &&
           "slice reads load through the span value while spans have no cache plan");
    assert(!contains(code, "_a->len =") &&
           "guarded typed array fill loop must use final len store outside the push body");
    assert(!contains(code, "_a->len >= _a->cap") &&
           "guarded typed array fill loop must not keep per-push capacity checks");
    assert(!contains(code, "XRT_REALLOC(_a->data") &&
           "guarded typed array fill loop must not keep per-push realloc paths");
    assert(!contains(code, "xrt_has_pending_error()) {\n        return 0;") &&
           "guarded typed array fill loop must not keep dead error propagation checks");
    assert(!contains(code, "xrt_index_get(") &&
           "guarded typed array loop must not fall back to runtime index dispatch");
    assert(!contains(code, "if (_idx < 0)") &&
           "guarded typed array loop must not keep negative-index adjustment");
    assert(!contains(code, "_idx >= 0 && _idx < _a->len") &&
           "guarded typed array loop must not keep the bounds ternary");
    assert(!contains(code, "XR_TO_INT(v") && "guarded typed array loop index must stay native");

    printf("  Generated guarded typed array slice loop %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_branchy_fill_loop_uses_preallocated_raw_store) {
    const char *src = "fn sum(n: i64) -> f64 {\n"
                      "    var values: Array<f64> = []\n"
                      "    var i = 0\n"
                      "    var x = 1.0\n"
                      "    while (i < n) {\n"
                      "        values.push(x)\n"
                      "        x = x + 0.25\n"
                      "        if (x > 17.0) {\n"
                      "            x = 1.0\n"
                      "        }\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    var total = 0.0\n"
                      "    i = 0\n"
                      "    while (i < len(values)) {\n"
                      "        total = total + values[i]\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    return total\n"
                      "}\n"
                      "print(sum(200))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed array branchy fill loop should generate");
    assert((contains(code, "xrt_array_new_typed_uninit(") ||
            contains(code, "xrt_array_new_typed_uninit_ptr(")) &&
           "branchy Array<f64> fill loop must preallocate uninitialized typed storage");
    assert((contains(code, "double *_ad") || contains(code, "double *XRT_RESTRICT _ad")) &&
           "branchy Array<f64> fill loop must cache raw double storage (optionally restrict)");
    assert(!contains(code, "_a->len =") &&
           "branchy typed array fill loop must use final len store outside the push body");
    assert(!contains(code, "_a->len >= _a->cap") &&
           "branchy typed array fill loop must not keep per-push capacity checks");
    assert(!contains(code, "XRT_REALLOC(_a->data") &&
           "branchy typed array fill loop must not keep per-push realloc paths");
    assert(!contains(code, "xrt_has_pending_error()) {\n        return 0;") &&
           "branchy typed array fill loop must not keep dead error propagation checks");
    assert(!contains(code, "xr_typed_get(") && !contains(code, "xr_typed_set(") &&
           "branchy typed array fill loop must not use typed runtime switches");
    assert(count_between(code, code + strlen(code), "xrt_release(") == 1 &&
           "branchy typed array must release its heap owner exactly once after the final read");

    printf("  Generated branchy typed array fill loop %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_filter_preserves_raw_storage_fast_path) {
    const char *src = "fn sum() -> i64 {\n"
                      "    var bytes: Array<u8> = []\n"
                      "    bytes.push(1)\n"
                      "    bytes.push(2)\n"
                      "    bytes.push(3)\n"
                      "    var kept = bytes.filter(fn(x: u8) -> bool { return x > 1 })\n"
                      "    kept.push(9)\n"
                      "    return i64(kept[0]) + i64(kept[2]) + len(kept)\n"
                      "}\n"
                      "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed array filter fast path should generate");
    assert(!contains(code, "xrt_array_filter_typed(") &&
           "pure Array<u8>.filter callback must inline instead of using boxed runtime helper");
    assert(!contains(code, "xrt_closure_new(&_xr_callable_") &&
           "pure inlined Array<u8>.filter must not allocate a callback closure");
    assert(!contains(code, "static XrValue test___anonymous__") &&
           "pure inlined Array<u8>.filter must not emit a dead boxed callback adapter");
    assert((contains(code, "xrt_array_new_typed_uninit(") ||
            contains(code, "xrt_array_new_typed_uninit_ptr(")) &&
           "inlined Array<u8>.filter must preallocate typed result storage");
    assert(contains(code, "XR_ELEM_U8") &&
           "Array<u8>.filter result must use the U8 typed element layout");
    assert(contains(code, "uint8_t *_dstd") &&
           "inlined Array<u8>.filter must write through a typed result pointer");
    assert(contains(code, "uint8_t *_srcd") &&
           "inlined Array<u8>.filter must read through a typed source pointer");
    assert(contains(code, "test___anonymous__") &&
           "inlined Array<u8>.filter must call the callback's native function");
    assert((contains(code, "((uint8_t*)_a->data)") || contains(code, "uint8_t *_dstd")) &&
           "Array<u8> filter result reads and writes must access raw byte storage");
    assert(!contains(code, "xrt_method_1(") &&
           "Array<u8>.filter must not fall back to dynamic method dispatch");
    assert(!contains(code, "({") && "Array<u8>.filter must emit portable C11 statements");
    assert(!contains(code, "xrt_index_get(") &&
           "Array<u8> filter result index read must not fall back to runtime index dispatch");
    assert(!contains(code, "xrt_getprop(") &&
           "Array<u8> filter result length must not fall back to dynamic property dispatch");

    printf("  Generated typed array filter fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_map_uses_typed_result_storage_fast_path) {
    const char *src = "fn sum() -> i64 {\n"
                      "    var values: Array<i64> = []\n"
                      "    values.push(1)\n"
                      "    values.push(2)\n"
                      "    values.push(3)\n"
                      "    var mapped = values.map(fn(x: i64) -> i64 { return x + 2 })\n"
                      "    mapped.push(9)\n"
                      "    return mapped[0] + mapped[3] + len(mapped)\n"
                      "}\n"
                      "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed array map fast path should generate");
    assert(!contains(code, "xrt_array_map_typed(") &&
           "pure Array<i64>.map callback must inline instead of using boxed runtime helper");
    assert(!contains(code, "xrt_closure_new(&_xr_callable_") &&
           "pure inlined Array<i64>.map must not allocate a callback closure");
    assert(!contains(code, "static XrValue test___anonymous__") &&
           "pure inlined Array<i64>.map must not emit a dead boxed callback adapter");
    assert((contains(code, "xrt_array_new_typed_uninit(") ||
            contains(code, "xrt_array_new_typed_uninit_ptr(")) &&
           "inlined Array<i64>.map must preallocate typed result storage");
    assert(contains(code, "XR_ELEM_I64") &&
           "Array<i64>.map result must use the I64 typed element layout");
    assert(contains(code, "int64_t *_dstd") &&
           "inlined Array<i64>.map must write through a typed result pointer");
    assert(contains(code, "int64_t *_srcd") &&
           "inlined Array<i64>.map must read through a typed source pointer");
    assert(contains(code, "test___anonymous__") &&
           "inlined Array<i64>.map must call the callback's native function");
    assert((contains(code, "((int64_t*)_a->data)") || contains(code, "int64_t *_dstd")) &&
           "Array<i64>.map result reads and writes must access raw i64 storage");
    assert(!contains(code, "xrt_method_1(") &&
           "Array<i64>.map must not fall back to dynamic method dispatch");
    assert(!contains(code, "({") && "Array<i64>.map must emit portable C11 statements");
    assert(!contains(code, "xrt_index_get(") &&
           "Array<i64>.map result index read must not fall back to runtime index dispatch");
    assert(!contains(code, "xrt_getprop(") &&
           "Array<i64>.map result length must not fall back to dynamic property dispatch");

    printf("  Generated typed array map fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_map_readonly_result_caches_data_pointer) {
    const char *src = "fn sum() -> i64 {\n"
                      "    var values: Array<i64> = []\n"
                      "    values.push(1)\n"
                      "    values.push(2)\n"
                      "    values.push(3)\n"
                      "    var mapped = values.map(fn(x: i64) -> i64 { return x + 2 })\n"
                      "    var i = 0\n"
                      "    var total = 0\n"
                      "    while (i < len(mapped)) {\n"
                      "        total += mapped[i]\n"
                      "        i += 1\n"
                      "    }\n"
                      "    return total\n"
                      "}\n"
                      "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    const char *code_end = code + strlen(code);
    assert(!had_error && "read-only typed array map scan should generate");
    assert(!contains(code, "xrt_array_map_typed(") &&
           "read-only pure Array<i64>.map must inline instead of using boxed runtime helper");
    assert(!contains(code, "xrt_closure_new(&_xr_callable_") &&
           "read-only pure Array<i64>.map must not allocate a callback closure");
    assert(!contains(code, "static XrValue test___anonymous__") &&
           "read-only pure Array<i64>.map must not emit a dead boxed callback adapter");
    assert((contains(code, "xrt_array_new_typed_uninit(") ||
            contains(code, "xrt_array_new_typed_uninit_ptr(")) &&
           "read-only pure Array<i64>.map must preallocate typed result storage");
    assert(contains(code, "int64_t *_ad") &&
           "read-only map result must cache the typed data pointer");
    assert(count_between(code, code_end, "int64_t *_ad") >= 1 &&
           "read-only map result should have at least one cached data pointer");
    assert(count_between(code, code_end, "_ad") >= 2 &&
           "read-only map result scan must use the cached data pointer");
    assert(!contains(code, "xrt_index_get(") &&
           "read-only map result scan must not fall back to runtime index dispatch");
    assert(!contains(code, "({") && "read-only Array<i64>.map must emit portable C11 statements");

    printf("  Generated read-only typed array map scan %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_map_captured_callback_fails_closed) {
    const char *src = "fn sum() -> i64 {\n"
                      "    var values: Array<i64> = []\n"
                      "    values.push(1)\n"
                      "    values.push(2)\n"
                      "    var offset = 3\n"
                      "    var mapped = values.map(fn(x: i64) -> i64 { return x + offset })\n"
                      "    return mapped[0] + mapped[1]\n"
                      "}\n"
                      "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir == NULL && "captured Array.map must fail closed before C generation until a frozen "
                         "runtime adapter exists");
}

TEST(cgen_typed_array_direct_hof_callback_extra_use_fails_closed) {
    const char *src = "fn run() -> i64 {\n"
                      "    var values = [1, 2]\n"
                      "    var callback = fn(x: i64) -> i64 { return x + 1 }\n"
                      "    var mapped = values.map(callback)\n"
                      "    return mapped[0] + callback(3)\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir == NULL &&
           "a callback with an extra use must not receive direct-symbol HOF authority");
}

TEST(cgen_dynamic_uncaptured_callback_keeps_boxed_adapter) {
    const char *src = "fn apply(f: fn(i64) -> i64, x: i64) -> i64 {\n"
                      "    return f(x)\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    return apply(fn(n: i64) -> i64 { return n + 1 }, 41)\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "dynamic uncaptured callback should generate");
    assert(contains(code, "xrt_closure_new(&_xr_callable_") &&
           "dynamic uncaptured callback must allocate a closure value");
    assert(contains(code, "static XrValue test___anonymous__") &&
           "dynamic uncaptured typed callback must keep its boxed adapter");

    printf("  Generated dynamic uncaptured callback path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(aot_closure_direct_symbol_requires_frozen_coroutine_plan) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 920, .frozen = true};
    XrType func_type = {.kind = XR_KIND_FUNCTION, .id = 921, .frozen = true};
    func_type.function.param_count = 0;
    func_type.function.min_params = 0;
    func_type.function.return_type = &int_type;

    XiFunc *owner = xi_func_new("closure_plan_owner", &int_type);
    XiBlock *owner_entry = xi_block_new(owner);
    XiFunc *sync_target = xi_func_new("closure_plan_sync", &int_type);
    XiBlock *sync_entry = xi_block_new(sync_target);
    XiValue *sync_result = xi_const_int(sync_target, sync_entry, 7, &int_type);
    TEST_REQUIRE(owner && owner_entry && sync_target && sync_entry && sync_result,
                 "closure plan fixtures allocated");
    owner_entry->sealed = true;
    sync_entry->sealed = true;
    xi_block_set_return(sync_entry, sync_result);

    XiValue *closure = xi_value_new(owner, owner_entry, XI_CLOSURE_NEW, &func_type, 0);
    XiValue *call = xi_value_new(owner, owner_entry, XI_CALL, &int_type, 1);
    TEST_REQUIRE(closure && call, "closure plan values allocated");
    closure->aux = sync_target;
    call->args[0] = closure;
    xi_block_set_return(owner_entry, call);

    XaotClosurePlan plan = {0};
    TEST_REQUIRE(xaot_prepare_closure_plan_for_value(NULL, owner, closure, &plan),
                 "closure plan rederived without coroutine proof");
    TEST_REQUIRE(plan.representation == XAOT_CLOSURE_RUNTIME,
                 "missing coroutine plan must reject direct symbol lowering");

    sync_target->stage = XI_STAGE_SEMANTIC_LOWERED;
    sync_target->invariant_mask = xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    TEST_REQUIRE(xi_coro_lower(sync_target, NULL), "synchronous coroutine plan frozen");
    TEST_REQUIRE(xaot_prepare_closure_plan_for_value(NULL, owner, closure, &plan),
                 "closure plan rederived from synchronous coroutine proof");
    TEST_REQUIRE(plan.representation == XAOT_CLOSURE_DIRECT_SYMBOL,
                 "fresh non-coroutine plan must permit direct symbol lowering");

    xi_cfg_invalidate(sync_target);
    TEST_REQUIRE(!xi_coro_plan_is_current(sync_target, sync_target->coro_plan),
                 "CFG mutation invalidates the synchronous coroutine plan");
    TEST_REQUIRE(xaot_prepare_closure_plan_for_value(NULL, owner, closure, &plan),
                 "closure plan rederived after coroutine proof becomes stale");
    TEST_REQUIRE(plan.representation == XAOT_CLOSURE_RUNTIME,
                 "stale coroutine plan must reject direct symbol lowering");

    XiFunc *coro_target = xi_func_new("closure_plan_coro", &int_type);
    XiBlock *coro_entry = xi_block_new(coro_target);
    XiValue *yield = xi_value_new(coro_target, coro_entry, XI_YIELD, &int_type, 0);
    XiValue *coro_result = xi_const_int(coro_target, coro_entry, 9, &int_type);
    TEST_REQUIRE(coro_target && coro_entry && yield && coro_result,
                 "coroutine closure target allocated");
    coro_entry->sealed = true;
    xi_block_set_return(coro_entry, coro_result);
    coro_target->stage = XI_STAGE_SEMANTIC_LOWERED;
    coro_target->invariant_mask = xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    TEST_REQUIRE(xi_coro_lower(coro_target, NULL), "suspendable coroutine plan frozen");
    closure->aux = coro_target;
    TEST_REQUIRE(xaot_prepare_closure_plan_for_value(NULL, owner, closure, &plan),
                 "closure plan rederived from suspendable coroutine proof");
    TEST_REQUIRE(plan.representation == XAOT_CLOSURE_RUNTIME,
                 "suspendable coroutine plan must reject direct symbol lowering");

    xi_func_free(owner);
    xi_func_free(sync_target);
    xi_func_free(coro_target);
}

TEST(cgen_closure_cell_var_id_above_255) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 900, .frozen = true};
    XrType func_type = {.kind = XR_KIND_FUNCTION, .id = 901, .frozen = true};
    func_type.function.param_count = 0;
    func_type.function.min_params = 0;
    func_type.function.return_type = &int_type;

    XiFunc *ir = xi_func_new("manual_high_cell", &func_type);
    assert(ir != NULL);
    XiBlock *entry = xi_block_new(ir);
    entry->sealed = true;

    XiValue *captured = xi_const_int(ir, entry, 7, &int_type);
    captured->var_id = 300;

    XiFunc *child = xi_func_new("manual_child", &int_type);
    assert(child != NULL);
    child->parent_func = ir;
    XiBlock *child_entry = xi_block_new(child);
    child_entry->sealed = true;
    XiValue *child_ret = xi_const_int(child, child_entry, 1, &int_type);
    xi_block_set_return(child_entry, child_ret);
    child->captures[0] = (XiCapture) {
        .source = XI_CAPTURE_SRC_REG,
        .capture_kind = XI_CAPTURE_BY_MUT_CELL,
        .needs_cell = true,
        .is_mutable = true,
        .type = &int_type,
        .value = captured,
        .name = "captured",
        .storage_domain = XR_STORAGE_EXEC_LOCAL,
        .value_capability = XA_CAP_MUTABLE,
    };
    child->ncaptures = 1;

    ir->children = (XiFunc **) xr_calloc(1, sizeof(XiFunc *));
    assert(ir->children != NULL);
    ir->children[0] = child;
    ir->children_cap = 1;
    ir->nchildren = 1;

    XiValue *closure = xi_value_new(ir, entry, XI_CLOSURE_NEW, &func_type, 1);
    assert(closure != NULL);
    closure->aux = (void *) child;
    closure->aux_int = 0;
    closure->args[0] = captured;
    xi_block_set_return(entry, closure);

    xi_pass_close(ir);
    assert(closure->args[0] != captured && closure->args[0]->op == XI_CELL_NEW &&
           "close pass must materialize a first-class cell XiValue");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "high var_id closure cell should generate");
    assert(contains(code, "xrt_cell_new(") && "mutable capture should allocate a cell");
    /* The 8-bit wall this case guards lives in the IR, not in a name. Cells
     * stopped being spelled `cell_<var_id>` in generated C once they became
     * first-class values -- they render as ordinary SSA temporaries -- so the
     * id is checked where it actually survives. */
    /* The 8-bit wall this case is named for was in the old design, where cells
     * lived in a side table indexed by var_id. Cells are first-class values
     * now (they render as ordinary SSA temporaries and carry no var_id at
     * all), so there is no width left to overflow and nothing to scan the
     * generated C for. What still has to hold is above: a capture of a
     * high-numbered variable materializes a real cell and generates. */

    printf("  Generated high-var closure cell path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_stack_alloc_direct_closure_uses_stack_runtime) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 910, .frozen = true};
    XrType func_type = {.kind = XR_KIND_FUNCTION, .id = 911, .frozen = true};
    func_type.function.param_count = 0;
    func_type.function.min_params = 0;
    func_type.function.return_type = &int_type;

    XiFunc *ir = xi_func_new("manual_stack_closure", &int_type);
    assert(ir != NULL);
    XiBlock *entry = xi_block_new(ir);
    entry->sealed = true;

    XiValue *captured = xi_const_int(ir, entry, 7, &int_type);

    XiFunc *child = xi_func_new("manual_stack_child", &int_type);
    assert(child != NULL);
    child->parent_func = ir;
    XiBlock *child_entry = xi_block_new(child);
    child_entry->sealed = true;
    XiValue *ret = xi_const_int(child, child_entry, 1, &int_type);
    xi_block_set_return(child_entry, ret);
    /* A mutable capture: what the assertion below pins is that even a closure
     * the escape analysis puts on the stack still reaches its variable through
     * the explicit cell. needs_cell = false contradicted that outright -- the
     * close pass materializes a cell only when the capture asks for one. */
    child->captures[0] = (XiCapture) {
        .source = XI_CAPTURE_SRC_REG,
        .capture_kind = XI_CAPTURE_BY_MUT_CELL,
        .needs_cell = true,
        .is_mutable = true,
        .type = &int_type,
        .value = captured,
        .name = "captured",
        .storage_domain = XR_STORAGE_EXEC_LOCAL,
        .value_capability = XA_CAP_MUTABLE,
    };
    child->ncaptures = 1;

    ir->children = (XiFunc **) xr_calloc(1, sizeof(XiFunc *));
    assert(ir->children != NULL);
    ir->children[0] = child;
    ir->children_cap = 1;
    ir->nchildren = 1;

    XiValue *closure = xi_value_new(ir, entry, XI_CLOSURE_NEW, &func_type, 1);
    assert(closure != NULL);
    closure->aux = (void *) child;
    closure->args[0] = captured;
    XiValue *call = xi_value_new(ir, entry, XI_CALL, &int_type, 1);
    assert(call != NULL);
    call->args[0] = closure;
    xi_block_set_return(entry, call);

    xi_pass_close(ir);
    assert(closure->args[0] != captured && closure->args[0]->op == XI_CELL_NEW &&
           "stack closure must capture the explicit cell XiValue");
    xi_escape_analyze(ir);
    xi_stack_alloc_rewrite(ir);
    assert(closure->op == XI_STACK_ALLOC && "direct closure should stack allocate");
    xi_arc_insert(ir);
    xi_arc_elim(ir);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "stack closure should generate");
    assert(contains(code, "xrt_closure_stack_new(&_xr_callable_") &&
           "direct no-escape closure should use stack closure runtime");
    assert(!contains(code, "xrt_closure_new(&_xr_callable_") &&
           "direct no-escape closure must not allocate a heap closure");
    /* The death point calls the stack-closure drop helper, which releases each
     * upvalue in the runtime rather than emitting a release per upvalue here.
     * Pin the call: without it the captured cells leak once per activation. */
    assert(contains(code, "xrt_closure_stack_drop(") && contains(code, "XR_TAG_CLOSURE") &&
           "stack closure should still run ARC destruction at its death point");

    printf("  Generated stack closure path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

/* Pull the SSA temporary out of `<id> = xrt_cell_new(` and check that the same
 * temporary is what the closure's upvalue slot receives. Cells are first-class
 * values now, so they are ordinary temporaries with no fixed name to match. */
static bool upvalue_receives_cell(const char *code) {
    const char *hit = strstr(code, " = xrt_cell_new(");
    if (!hit)
        return false;
    const char *start = hit;
    while (start > code &&
           ((start[-1] >= '0' && start[-1] <= '9') || (start[-1] >= 'a' && start[-1] <= 'z') ||
            (start[-1] >= 'A' && start[-1] <= 'Z') || start[-1] == '_'))
        start--;
    size_t len = (size_t) (hit - start);
    char expect[96];
    if (len == 0 || len + 20 >= sizeof(expect))
        return false;
    memcpy(expect, "_c->upvals[0] = ", 16);
    memcpy(expect + 16, start, len);
    expect[16 + len] = ';';
    expect[17 + len] = '\0';
    return strstr(code, expect) != NULL;
}

TEST(cgen_stack_alloc_closure_preserves_cell_capture) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 912, .frozen = true};
    XrType func_type = {.kind = XR_KIND_FUNCTION, .id = 913, .frozen = true};
    func_type.function.param_count = 0;
    func_type.function.min_params = 0;
    func_type.function.return_type = &int_type;

    XiFunc *ir = xi_func_new("manual_stack_cell_closure", &int_type);
    assert(ir != NULL);
    XiBlock *entry = xi_block_new(ir);
    entry->sealed = true;

    XiValue *captured = xi_const_int(ir, entry, 7, &int_type);
    captured->var_id = 37;

    XiFunc *child = xi_func_new("manual_stack_cell_child", &int_type);
    assert(child != NULL);
    child->parent_func = ir;
    XiBlock *child_entry = xi_block_new(child);
    child_entry->sealed = true;
    XiValue *ret = xi_const_int(child, child_entry, 1, &int_type);
    xi_block_set_return(child_entry, ret);
    child->captures[0] = (XiCapture) {
        .source = XI_CAPTURE_SRC_REG,
        .capture_kind = XI_CAPTURE_BY_MUT_CELL,
        .needs_cell = true,
        .is_mutable = true,
        .type = &int_type,
        .value = captured,
        .name = "captured",
        .storage_domain = XR_STORAGE_EXEC_LOCAL,
        .value_capability = XA_CAP_MUTABLE,
    };
    child->ncaptures = 1;

    ir->children = (XiFunc **) xr_calloc(1, sizeof(XiFunc *));
    assert(ir->children != NULL);
    ir->children[0] = child;
    ir->children_cap = 1;
    ir->nchildren = 1;

    XiValue *closure = xi_value_new(ir, entry, XI_CLOSURE_NEW, &func_type, 1);
    assert(closure != NULL);
    closure->aux = (void *) child;
    closure->args[0] = captured;
    XiValue *call = xi_value_new(ir, entry, XI_CALL, &int_type, 1);
    assert(call != NULL);
    call->args[0] = closure;
    xi_block_set_return(entry, call);

    /* Codegen consumes CLOSED IR: xi_pass_close is what materializes a
     * needs_cell capture into a first-class cell value. Skipping it left the
     * closure capturing the raw value, so the upvalue slot received the
     * variable instead of its cell -- an input the pipeline never produces. */
    xi_pass_close(ir);
    assert(closure->args[0] != captured && closure->args[0]->op == XI_CELL_NEW &&
           "close must materialize the mutable capture cell before codegen");
    xi_escape_analyze(ir);
    xi_stack_alloc_rewrite(ir);
    assert(closure->op == XI_STACK_ALLOC && "direct closure should stack allocate");
    xi_arc_insert(ir);
    xi_arc_elim(ir);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "stack closure cell capture should generate");
    assert(contains(code, "xrt_closure_stack_new(&_xr_callable_") &&
           "direct no-escape closure should use stack closure runtime");
    assert(contains(code, "xrt_cell_new(") &&
           "mutable capture for stack closure must allocate a cell");
    assert(upvalue_receives_cell(code) &&
           "stack closure upvalue must receive the mutable capture cell");

    printf("  Generated stack closure cell path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_filter_readonly_result_caches_data_pointer) {
    const char *src = "fn sum() -> i64 {\n"
                      "    var bytes: Array<u8> = []\n"
                      "    bytes.push(1)\n"
                      "    bytes.push(2)\n"
                      "    bytes.push(3)\n"
                      "    var kept = bytes.filter(fn(x: u8) -> bool { return x > 1 })\n"
                      "    var i = 0\n"
                      "    var total = 0\n"
                      "    while (i < len(kept)) {\n"
                      "        total += kept[i]\n"
                      "        i += 1\n"
                      "    }\n"
                      "    return total\n"
                      "}\n"
                      "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    const char *code_end = code + strlen(code);
    assert(!had_error && "read-only typed array filter scan should generate");
    assert(!contains(code, "xrt_array_filter_typed(") &&
           "read-only pure Array<u8>.filter must inline instead of using boxed runtime helper");
    assert(!contains(code, "xrt_closure_new(&_xr_callable_") &&
           "read-only pure Array<u8>.filter must not allocate a callback closure");
    assert(!contains(code, "static XrValue test___anonymous__") &&
           "read-only pure Array<u8>.filter must not emit a dead boxed callback adapter");
    assert((contains(code, "xrt_array_new_typed_uninit(") ||
            contains(code, "xrt_array_new_typed_uninit_ptr(")) &&
           "read-only pure Array<u8>.filter must preallocate typed result storage");
    assert(contains(code, "uint8_t *_ad") &&
           "read-only filter result must cache the typed data pointer");
    assert(count_between(code, code_end, "uint8_t *_ad") >= 1 &&
           "read-only filter result should have at least one cached data pointer");
    assert(count_between(code, code_end, "_ad") >= 2 &&
           "read-only filter result scan must use the cached data pointer");
    assert(!contains(code, "xrt_index_get(") &&
           "read-only filter result scan must not fall back to runtime index dispatch");
    assert(!contains(code, "({") && "read-only Array<u8>.filter must emit portable C11 statements");

    printf("  Generated read-only typed array filter scan %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_filter_captured_callback_fails_closed) {
    const char *src = "fn sum() -> i64 {\n"
                      "    var values: Array<i64> = []\n"
                      "    values.push(1)\n"
                      "    values.push(4)\n"
                      "    var limit = 2\n"
                      "    var kept = values.filter(fn(x: i64) -> bool { return x > limit })\n"
                      "    return kept[0]\n"
                      "}\n"
                      "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir == NULL && "captured Array.filter must fail closed before C generation until a "
                         "frozen runtime adapter exists");
}

TEST(cgen_typed_array_reduce_uses_native_accumulator_fast_path) {
    const char *src =
        "fn sum() -> i64 {\n"
        "    var values: Array<i64> = []\n"
        "    values.push(1)\n"
        "    values.push(2)\n"
        "    values.push(3)\n"
        "    return values.reduce(fn(acc: i64, x: i64) -> i64 { return acc + x }, 0)\n"
        "}\n"
        "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed array reduce fast path should generate");
    assert(!contains(code, "xrt_array_reduce_typed(") &&
           "pure Array<i64>.reduce callback must inline instead of using boxed runtime helper");
    assert(!contains(code, "xrt_closure_new(&_xr_callable_") &&
           "pure inlined Array<i64>.reduce must not allocate a callback closure");
    assert(!contains(code, "static XrValue test___anonymous__") &&
           "pure inlined Array<i64>.reduce must not emit a dead boxed callback adapter");
    assert(contains(code, "int64_t _acc") &&
           "inlined Array<i64>.reduce must use a native accumulator");
    assert(contains(code, "int64_t *_srcd") &&
           "inlined Array<i64>.reduce must read through a typed source pointer");
    assert(contains(code, "test___anonymous__") &&
           "inlined Array<i64>.reduce must call the callback's native function");
    assert(!contains(code, "xrt_method_2(") &&
           "Array<i64>.reduce must not fall back to dynamic method dispatch");
    assert(!contains(code, "({") && "Array<i64>.reduce must emit portable C11 statements");

    printf("  Generated typed array reduce fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_unused_reduce_still_executes_callback_loop) {
    const char *src = "fn run() -> i64 {\n"
                      "    var values: Array<i64> = []\n"
                      "    values.push(1)\n"
                      "    values.push(2)\n"
                      "    values.reduce(fn(acc: i64, x: i64) -> i64 { return acc + x }, 0)\n"
                      "    return 7\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "unused typed array reduce should generate");
    assert(contains(code, "for (int64_t _i = 0; _i < _n; _i++)") &&
           contains(code, "test___anonymous__") &&
           "unused reduce must still execute every callback invocation");
    assert(!contains(code, "xrt_array_reduce_typed(") && !contains(code, "xrt_method_2(") &&
           "unused reduce must consume the direct frozen recipe");
    assert(!contains(code, "({") && "unused reduce must remain portable C11");

    printf("  Generated unused typed array reduce loop %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

static bool cgen_array_hof_emit_with_prepared_plan(TestAotPlan *plan, XiModule *module) {
    XiCgenCtx *ctx = xi_cgen_ctx_new();
    TEST_REQUIRE(ctx != NULL, "Array HOF mutation CGen context allocated");
    xi_cgen_ctx_set_aot_bundle(ctx, &plan->bundle);
    TestCEmissionRegistry emission_registry;
    TEST_REQUIRE(test_c_emission_registry_install(&emission_registry, ctx, &plan->bundle),
                 "Array HOF mutation C emission registry installed");
    char *buf = NULL;
    size_t bufsz = 0;
    FILE *mem = xr_open_memstream(&buf, &bufsz);
    TEST_REQUIRE(mem != NULL, "Array HOF mutation stream opened");
    xi_cgen_program(ctx, mem, module);
    TEST_REQUIRE(xr_close_memstream(mem, &buf, &bufsz) == 0, "Array HOF mutation stream closed");
    bool ok = !xi_cgen_has_error(ctx);
    xr_free(buf);
    test_c_emission_registry_free(&emission_registry);
    xi_cgen_ctx_free(ctx);
    return ok;
}

TEST(cgen_typed_array_direct_hof_requires_exact_callback_abi_plan) {
    const char *src = "fn run() -> i64 {\n"
                      "    var values: Array<i64> = []\n"
                      "    values.push(1)\n"
                      "    var mapped = values.map(fn(x: i64) -> i64 { return x + 1 })\n"
                      "    return mapped[0]\n"
                      "}\n"
                      "print(run())\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL && test_prepare_backend_ir(ir),
                 "Array HOF ABI mutation fixture reached Backend");
    XiModule *module = ir->module;
    bool own_module = false;
    if (!module) {
        module = xi_module_new("array_hof_abi.xr", "test", ir);
        TEST_REQUIRE(module != NULL, "Array HOF ABI mutation module allocated");
        own_module = true;
    }
    XiModule *modules[] = {module};
    TestAotPlan plan;
    test_aot_plan_prepare(&plan, modules, 1, 0);

    uint32_t callable_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_operation_count =
        (uint32_t) xr_semantic_plan_operation_count(ir->semantic_plan);
    for (uint32_t i = 0; i < semantic_operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(ir->semantic_plan, i);
        if (!operation || operation->intrinsic_kind != XR_SEM_INTRINSIC_ARRAY_HOF)
            continue;
        TEST_REQUIRE(callable_function == XR_SEMANTIC_INDEX_NONE,
                     "Array HOF ABI mutation has one semantic callback");
        callable_function = operation->callable_function;
    }
    TEST_REQUIRE(callable_function != XR_SEMANTIC_INDEX_NONE,
                 "Array HOF ABI mutation semantic callback found");
    uint32_t callee_index = UINT32_MAX;
    for (uint32_t i = 0; i < plan.bundle.nfunc_plans; i++) {
        XaotFuncPlan *candidate = &plan.bundle.func_plans[i];
        if (!candidate->func || candidate->func->semantic_plan != ir->semantic_plan ||
            candidate->func->semantic_plan_function_index != callable_function)
            continue;
        TEST_REQUIRE(callee_index == UINT32_MAX, "Array HOF ABI mutation has one callback plan");
        callee_index = i;
    }
    TEST_REQUIRE(callee_index != UINT32_MAX, "Array HOF ABI mutation callback plan found");
    XaotFuncPlan *callee = &plan.bundle.func_plans[callee_index];
    TEST_REQUIRE(callee->reachable && !callee->may_suspend && callee->abi.kind == XAOT_ABI_NATIVE &&
                     callee->abi.nparams == 1 && callee->abi.params,
                 "Array HOF callback has exact native ABI prerequisite");
    TEST_REQUIRE(cgen_array_hof_emit_with_prepared_plan(&plan, module),
                 "exact Array HOF callback ABI generates");

    uint8_t saved_reachable = callee->reachable;
    callee->reachable = 0;
    TEST_REQUIRE(!cgen_array_hof_emit_with_prepared_plan(&plan, module),
                 "unreachable Array HOF callback plan fails closed");
    callee->reachable = saved_reachable;
    uint8_t saved_suspend = callee->may_suspend;
    callee->may_suspend = 1;
    TEST_REQUIRE(!cgen_array_hof_emit_with_prepared_plan(&plan, module),
                 "suspendable Array HOF callback plan fails closed");
    callee->may_suspend = saved_suspend;
    XaotAbiKind saved_kind = callee->abi.kind;
    callee->abi.kind = XAOT_ABI_TAGGED;
    TEST_REQUIRE(!cgen_array_hof_emit_with_prepared_plan(&plan, module),
                 "tagged Array HOF callback ABI fails closed");
    callee->abi.kind = saved_kind;
    uint16_t saved_nparams = callee->abi.nparams;
    callee->abi.nparams = 0;
    TEST_REQUIRE(!cgen_array_hof_emit_with_prepared_plan(&plan, module),
                 "wrong Array HOF callback arity fails closed");
    callee->abi.nparams = saved_nparams;

    XaotAbiSlot saved_param = callee->abi.params[0];
    callee->abi.params[0].cls = XAOT_ARG_PTR;
    TEST_REQUIRE(!cgen_array_hof_emit_with_prepared_plan(&plan, module),
                 "non-scalar Array HOF callback parameter fails closed");
    callee->abi.params[0] = saved_param;
    callee->abi.params[0].flags = XAOT_ABI_SLOT_BORROWED_PLACE;
    TEST_REQUIRE(!cgen_array_hof_emit_with_prepared_plan(&plan, module),
                 "borrowed-place Array HOF callback parameter fails closed");
    callee->abi.params[0] = saved_param;
    callee->abi.params[0].c_type = "forged_hof_param_t";
    TEST_REQUIRE(!cgen_array_hof_emit_with_prepared_plan(&plan, module),
                 "noncanonical Array HOF callback parameter C type fails closed");
    callee->abi.params[0] = saved_param;
    callee->abi.params[0].pointee_rep.kind = XAOT_VALUE_SCALAR;
    TEST_REQUIRE(!cgen_array_hof_emit_with_prepared_plan(&plan, module),
                 "forged Array HOF callback pointee representation fails closed");
    callee->abi.params[0] = saved_param;
    callee->abi.params[0].rep.rep = XAOT_REP_U64;
    TEST_REQUIRE(!cgen_array_hof_emit_with_prepared_plan(&plan, module),
                 "wrong Array HOF callback parameter scalar rep fails closed");
    callee->abi.params[0] = saved_param;
    callee->abi.params[0].rep.flags = XAOT_VALUE_FLAG_DYNAMIC_C_TYPE;
    TEST_REQUIRE(!cgen_array_hof_emit_with_prepared_plan(&plan, module),
                 "flagged Array HOF callback parameter rep fails closed");
    callee->abi.params[0] = saved_param;

    XaotAbiSlot saved_ret = callee->abi.ret;
    callee->abi.ret.cls = XAOT_ARG_TAGGED;
    TEST_REQUIRE(!cgen_array_hof_emit_with_prepared_plan(&plan, module),
                 "non-scalar Array HOF callback return fails closed");
    callee->abi.ret = saved_ret;
    callee->abi.ret.c_type = "forged_hof_return_t";
    TEST_REQUIRE(!cgen_array_hof_emit_with_prepared_plan(&plan, module),
                 "noncanonical Array HOF callback return C type fails closed");
    callee->abi.ret = saved_ret;
    callee->abi.ret.rep.rep = XAOT_REP_U64;
    TEST_REQUIRE(!cgen_array_hof_emit_with_prepared_plan(&plan, module),
                 "wrong Array HOF callback return scalar rep fails closed");
    callee->abi.ret = saved_ret;

    TEST_REQUIRE(plan.bundle.nfunc_plans < plan.bundle.func_plan_cap,
                 "Array HOF callback duplicate mutation has spare capacity");
    plan.bundle.func_plans[plan.bundle.nfunc_plans++] = *callee;
    TEST_REQUIRE(!cgen_array_hof_emit_with_prepared_plan(&plan, module),
                 "duplicate Array HOF callback function plan fails closed");
    plan.bundle.nfunc_plans--;

    uint32_t saved_count = plan.bundle.nfunc_plans;
    XaotFuncPlan saved_callee = *callee;
    plan.bundle.func_plans[callee_index] = plan.bundle.func_plans[saved_count - 1u];
    plan.bundle.nfunc_plans--;
    TEST_REQUIRE(!cgen_array_hof_emit_with_prepared_plan(&plan, module),
                 "missing Array HOF callback function plan fails closed");
    plan.bundle.nfunc_plans = saved_count;
    plan.bundle.func_plans[saved_count - 1u] = plan.bundle.func_plans[callee_index];
    plan.bundle.func_plans[callee_index] = saved_callee;
    callee = &plan.bundle.func_plans[callee_index];
    TEST_REQUIRE(cgen_array_hof_emit_with_prepared_plan(&plan, module),
                 "restored Array HOF callback ABI generates");

    test_aot_plan_free(&plan);
    if (own_module) {
        module->init = NULL;
        xi_module_free(module);
    }
    xi_func_free(ir);
}

TEST(cgen_typed_array_reduce_captured_callback_fails_closed) {
    const char *src =
        "fn sum() -> i64 {\n"
        "    var values: Array<i64> = []\n"
        "    values.push(1)\n"
        "    values.push(2)\n"
        "    var offset = 3\n"
        "    return values.reduce(fn(acc: i64, x: i64) -> i64 { return acc + x + offset }, 0)\n"
        "}\n"
        "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir == NULL && "captured Array.reduce must fail closed before C generation until a "
                         "frozen runtime adapter exists");
}

TEST(cgen_int_const_div_mod_uses_native_ops) {
    const char *src = "fn fast(n: i64) -> i64 {\n"
                      "    return (n / 5) + (n % 7)\n"
                      "}\n"
                      "fn checked(n: i64, d: i64) -> i64 {\n"
                      "    return n % d\n"
                      "}\n"
                      "print(fast(42))\n"
                      "print(checked(42, 6))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "integer div/mod fast path should generate");

    const char *code_end = code + strlen(code);
    assert(contains(code, "static int64_t test_fast_") &&
           contains(code, "static int64_t test_checked_") &&
           "both test functions should be generated");
    assert(count_between(code, code_end, "xrt_int_div(") == 0 &&
           "constant non-zero integer division must not reach the throwing helper");
    assert(count_between(code, code_end, "xrt_int_mod(") == 1 &&
           "an unproven integer modulo must still reach the throwing helper");
    assert(contains(code, "xrt_int_div_mod_eval(XR_INT_DIV_MOD_DIV, "
                          "XR_INT_DIV_MOD_PROOF_NONZERO") &&
           "a constant divisor must carry its nonzero proof to the shared owner");
    assert(contains(code, "xrt_int_div_mod_eval(XR_INT_DIV_MOD_MOD, "
                          "XR_INT_DIV_MOD_PROOF_NONZERO") &&
           "a constant modulus must carry its nonzero proof to the shared owner");
    assert(contains(code, ", INT64_C(5))") && contains(code, ", INT64_C(7))") &&
           "constant div/mod RHS must stay literal so C compilers can strength-reduce it");

    printf("  Generated integer div/mod fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_shift_uses_stable_owner_adapter) {
    const char *src = "fn fast(b: u8) -> i64 {\n"
                      "    var x = i64(b)\n"
                      "    return x >> 4\n"
                      "}\n"
                      "fn widen(b: u8) -> i64 {\n"
                      "    var x = i64(b)\n"
                      "    return x << 8\n"
                      "}\n"
                      "fn checked(n: i64, s: i64) -> i64 {\n"
                      "    return n >> s\n"
                      "}\n"
                      "fn checkedLeft(n: i64, s: i64) -> i64 {\n"
                      "    return n << s\n"
                      "}\n"
                      "fn wrapLeft(n: i64) -> i64 {\n"
                      "    return n << 8\n"
                      "}\n"
                      "fn wrapRight(n: i64) -> i64 {\n"
                      "    return n >> 8\n"
                      "}\n"
                      "print(fast(240))\n"
                      "print(widen(240))\n"
                      "print(checked(-8, 1))\n"
                      "print(checkedLeft(8, 1))\n"
                      "print(wrapLeft(-1))\n"
                      "print(wrapRight(-256))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "integer shift owner adapter should generate");

    const char *fast = find_static_function_definition(code, "test_fast_");
    assert(fast != NULL && "fast function should be generated");
    const char *fast_end = next_static_after(fast);
    assert(count_between(fast, fast_end, "xrt_shift_eval(XR_SHIFT_RIGHT_SIGNED") == 1 &&
           "constant right shift must use the stable owner adapter");

    const char *widen = find_static_function_definition(code, "test_widen_");
    assert(widen != NULL && "widen function should be generated");
    const char *widen_end = next_static_after(widen);
    assert(count_between(widen, widen_end, "xrt_shift_eval(XR_SHIFT_LEFT") == 1 &&
           "constant left shift must use the stable owner adapter");

    const char *checked = find_static_function_definition(code, "test_checked_");
    assert(checked != NULL && "checked function should be generated");
    const char *checked_end = next_static_after(checked);
    assert(count_between(checked, checked_end, "xrt_shift_eval(XR_SHIFT_RIGHT_SIGNED") == 1 &&
           "dynamic right shift must use the stable owner adapter");

    const char *checked_left = find_static_function_definition(code, "test_checkedLeft_");
    assert(checked_left != NULL && "checkedLeft function should be generated");
    const char *checked_left_end = next_static_after(checked_left);
    assert(count_between(checked_left, checked_left_end, "xrt_shift_eval(XR_SHIFT_LEFT") == 1 &&
           "dynamic left shift must use the stable owner adapter");

    const char *wrap_left = find_static_function_definition(code, "test_wrapLeft_");
    assert(wrap_left != NULL && "wrapLeft function should be generated");
    const char *wrap_left_end = next_static_after(wrap_left);
    assert(count_between(wrap_left, wrap_left_end, "xrt_shift_eval(XR_SHIFT_LEFT") == 1 &&
           count_between(wrap_left, wrap_left_end, " << ") == 0 &&
           "constant signed left shift must not revive raw C semantics");

    const char *wrap_right = find_static_function_definition(code, "test_wrapRight_");
    assert(wrap_right != NULL && "wrapRight function should be generated");
    const char *wrap_right_end = next_static_after(wrap_right);
    assert(count_between(wrap_right, wrap_right_end, "xrt_shift_eval(XR_SHIFT_RIGHT_SIGNED") == 1 &&
           count_between(wrap_right, wrap_right_end, " >> ") == 0 &&
           "constant signed right shift must not revive raw C semantics");

    printf("  Generated integer shift fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_unsigned_shift_uses_stable_owner_adapter) {
    const char *src = "fn unsignedShift(x: u64) -> u64 {\n"
                      "    var shifted = x << 24\n"
                      "    return shifted >> 52\n"
                      "}\n"
                      "fn dynamicShift(x: u64, s: i64) -> u64 {\n"
                      "    return x >> s\n"
                      "}\n"
                      "print(unsignedShift(889523592379))\n"
                      "print(dynamicShift(16, 1))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "unsigned integer shift owner adapter should generate");

    const char *fast = find_static_function_definition(code, "test_unsignedShift_");
    assert(fast != NULL && "unsignedShift function should be generated");
    const char *fast_end = next_static_after(fast);
    assert(count_between(fast, fast_end, "xrt_shift_eval(XR_SHIFT_LEFT") == 1 &&
           "u64 constant left shift must use the stable owner adapter");
    assert(count_between(fast, fast_end, "xrt_shift_eval(XR_SHIFT_RIGHT_UNSIGNED") == 1 &&
           "u64 constant right shift must use the unsigned owner mode");
    assert(count_between(fast, fast_end, " << ") == 0 &&
           count_between(fast, fast_end, " >> ") == 0 &&
           "u64 constant shifts must not revive raw C semantics");

    const char *dynamic = find_static_function_definition(code, "test_dynamicShift_");
    assert(dynamic != NULL && "dynamicShift function should be generated");
    const char *dynamic_end = next_static_after(dynamic);
    assert(count_between(dynamic, dynamic_end, "xrt_shift_eval(XR_SHIFT_RIGHT_UNSIGNED") == 1 &&
           "dynamic unsigned shift must use the unsigned owner mode");

    printf("  Generated unsigned integer shift fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_unsigned_arith_uses_native_unsigned_expr) {
    const char *src = "fn hash32(seq: u32) -> u32 {\n"
                      "    var prime: u32 = 2654435761\n"
                      "    return seq * prime\n"
                      "}\n"
                      "fn mix64(seq: u64) -> u64 {\n"
                      "    var prime: u64 = 889523592379\n"
                      "    return (seq + prime) * prime\n"
                      "}\n"
                      "fn signedMul(a: i64, b: i64) -> i64 {\n"
                      "    return a * b\n"
                      "}\n"
                      "print(hash32(123456))\n"
                      "print(mix64(123456))\n"
                      "print(signedMul(7, 9))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "unsigned arithmetic fast path should generate");

    const char *hash32 = find_static_function_definition(code, "test_hash32_");
    assert(hash32 != NULL && "hash32 function should be generated");
    const char *hash32_end = next_static_after(hash32);
    assert(count_between(hash32, hash32_end, "(uint32_t)(") >= 3 &&
           "u32 arithmetic should use width-precise unsigned C operands");
    assert(count_between(hash32, hash32_end, "(uint64_t)(") == 0 &&
           "u32 arithmetic should not widen through uint64_t");
    assert(count_between(hash32, hash32_end, "(int64_t)((uint64_t)") == 0 &&
           "u32 arithmetic should not cast the product through int64_t");

    const char *mix64 = find_static_function_definition(code, "test_mix64_");
    assert(mix64 != NULL && "mix64 function should be generated");
    const char *mix64_end = next_static_after(mix64);
    assert(count_between(mix64, mix64_end, "(uint64_t)(") >= 4 &&
           "u64 arithmetic should use unsigned C operands");
    assert(count_between(mix64, mix64_end, "(int64_t)((uint64_t)") == 0 &&
           "unboxed u64 arithmetic should not cast through int64_t");

    const char *signed_mul = find_static_function_definition(code, "test_signedMul_");
    assert(signed_mul != NULL && "signedMul function should be generated");
    const char *signed_mul_end = next_static_after(signed_mul);
    assert(count_between(signed_mul, signed_mul_end, "(int64_t)((uint64_t)") == 1 &&
           "signed i64 multiplication must keep signed-wrap-safe lowering");

    printf("  Generated unsigned arithmetic fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_elides_dead_err_checks_after_nothrow_scalar_helper_chain) {
    const char *src = "fn packParts(high: i64, low: i64) -> i64 {\n"
                      "    return (high << 32) | (low & 0xFFFFFFFF)\n"
                      "}\n"
                      "fn highPart(bits: i64) -> i64 {\n"
                      "    return bits >> 32\n"
                      "}\n"
                      "fn lowPart(bits: i64) -> i64 {\n"
                      "    return bits & 0xFFFFFFFF\n"
                      "}\n"
                      "fn combine(a: i64, b: i64) -> i64 {\n"
                      "    if (a < 0 || b < 0) { return -1 }\n"
                      "    return packParts(highPart(a) + highPart(b), "
                      "(lowPart(a) + lowPart(b)) & 0xFFFFFFFF)\n"
                      "}\n"
                      "print(combine(4294967296, 1))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "nothrow scalar helper chain should generate");

    const char *combine = find_static_function_definition(code, "test_combine_");
    assert(combine != NULL && "combine function should be generated");
    const char *combine_end = next_static_after(combine);
    const char *high_part = find_static_function_definition(code, "test_highPart_");
    assert(high_part != NULL && "highPart function should be generated");
    const char *high_part_end = next_static_after(high_part);
    const char *pack_parts = find_static_function_definition(code, "test_packParts_");
    assert(pack_parts != NULL && "packParts function should be generated");
    const char *pack_parts_end = next_static_after(pack_parts);
    assert(count_between(combine, combine_end, "xrt_has_pending_error") == 0 &&
           "inlined no-throw scalar helper chain must not keep dead error-channel checks");
    assert((count_between(combine, combine_end, "xrt_shift_eval(") > 0 ||
            count_between(high_part, high_part_end, "xrt_shift_eval(") > 0) &&
           "generated helper chain should still contain the right-shift scalar work under test");
    assert((count_between(combine, combine_end, " | ") > 0 ||
            count_between(combine, combine_end, "xrt_shift_eval(") > 0 ||
            count_between(pack_parts, pack_parts_end, " | ") > 0 ||
            count_between(pack_parts, pack_parts_end, "xrt_shift_eval(") > 0) &&
           "generated helper chain should still contain the pack scalar work under test");

    printf("  Generated no-throw scalar helper chain %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_codegen_controls_emit_provider_constructs_without_runtime_calls) {
    XrType u64_type = {.kind = XR_KIND_INT, .id = 964, .scalar_rep = XR_NATIVE_U64, .frozen = true};
    XrType unit_type = {
        .kind = XR_KIND_UNIT, .scalar_rep = XR_SCALAR_REP_NONE, .id = 965, .frozen = true};
    XiFunc *ir = xi_func_new("manual_codegen_controls", &u64_type);
    TEST_REQUIRE(ir != NULL, "manual codegen-controls function allocated");
    XiBlock *entry = xi_block_new(ir);
    TEST_REQUIRE(entry != NULL, "manual codegen-controls entry allocated");
    entry->sealed = true;
    ir->nparams = 1;
    ir->min_params = 1;
    ir->params = (XiValue **) xr_calloc(1, sizeof(XiValue *));
    TEST_REQUIRE(ir->params != NULL, "manual codegen-controls parameter table allocated");
    XiValue *value = xi_param(ir, entry, 0, &u64_type);
    TEST_REQUIRE(value != NULL, "manual codegen-controls parameter allocated");
    ir->params[0] = value;
    XiValue *opaque = xi_value_new(ir, entry, XI_CODEGEN_OPAQUE, &u64_type, 1);
    XiValue *fence = xi_value_new(ir, entry, XI_CODEGEN_COMPILER_FENCE, &unit_type, 0);
    TEST_REQUIRE(opaque != NULL && fence != NULL, "manual codegen controls allocated");
    opaque->args[0] = value;
    opaque->xa_intrinsic_id = XA_INTRINSIC_CODEGEN_OPAQUE;
    fence->xa_intrinsic_id = XA_INTRINSIC_CODEGEN_COMPILER_FENCE;
    xi_block_set_return(entry, opaque);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "codegen controls should generate");

    const char *guarded = find_static_function_definition(code, "manual_codegen_controls");
    TEST_REQUIRE(guarded != NULL, "guarded function should be generated");
    const char *guarded_end = strstr(guarded, "\n}\n");
    TEST_REQUIRE(guarded_end != NULL, "guarded function end should be generated");
    TEST_REQUIRE(count_between(guarded, guarded_end, "xrt_codegen_opaque_u64(") == 1,
                 "opaque must use its typed provider adapter");
    TEST_REQUIRE(count_between(guarded, guarded_end, "xrt_codegen_compiler_fence()") == 1,
                 "compilerFence must use its provider scheduling adapter");
    TEST_REQUIRE(count_between(guarded, guarded_end, "xrt_mem_compiler") == 0,
                 "removed mem compiler controls must not survive in generated C");
    TEST_REQUIRE(count_between(guarded, guarded_end, "xrt_has_pending_error") == 0,
                 "codegen controls must not poll the pending-error channel");

    printf("  Generated first-class codegen controls %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_uses_closed_world_effects_for_conservative_direct_call_checks) {
    const char *src = "fn layer0(value: i64) -> i64 { return value + 1 }\n"
                      "fn layer1(value: i64) -> i64 { return layer0(value) + 1 }\n"
                      "fn layer2(value: i64) -> i64 { return layer1(value) + 1 }\n"
                      "fn layer3(value: i64) -> i64 { return layer2(value) + 1 }\n"
                      "fn layer4(value: i64) -> i64 { return layer3(value) + 1 }\n"
                      "fn layer5(value: i64) -> i64 { return layer4(value) + 1 }\n"
                      "fn layer6(value: i64) -> i64 { return layer5(value) + 1 }\n"
                      "fn layer7(value: i64) -> i64 { return layer6(value) + 1 }\n"
                      "fn layer8(value: i64) -> i64 { return layer7(value) + 1 }\n"
                      "fn layer9(value: i64) -> i64 { return layer8(value) + 1 }\n"
                      "fn caller(value: i64) -> i64 {\n"
                      "    var result = layer9(value)\n"
                      "    return result + 1\n"
                      "}\n"
                      "print(caller(1))\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL && ir->module != NULL, "IR compilation failed");
    TEST_REQUIRE(test_prepare_backend_ir(ir), "backend preparation failed");
    ir->module->name = "test";
    XiModule *modules[] = {ir->module};
    TestAotPlan plan;
    test_aot_plan_prepare(&plan, modules, 1, 0);

    XiFunc *caller = NULL;
    XiValue *caller_call = NULL;
    for (uint16_t fi = 0; fi < ir->nchildren; fi++) {
        XiFunc *func = ir->children[fi];
        if (!func || !func->name)
            continue;
        if (strcmp(func->name, "caller") == 0)
            caller = func;
        if (strncmp(func->name, "layer", 5) != 0 && strcmp(func->name, "caller") != 0)
            continue;
        /* Model conservative imported-call flags after evidence production.
         * The evidence still records the semantic, closed-world no-throw fact;
         * a local recursive scan alone exceeds its fail-closed depth budget. */
        for (uint32_t bi = 0; bi < func->nblocks; bi++) {
            XiBlock *block = func->blocks[bi];
            if (!block)
                continue;
            for (uint32_t vi = 0; vi < block->nvalues; vi++) {
                XiValue *value = block->values[vi];
                if (!value || (value->op != XI_CALL && value->op != XI_TAIL_CALL))
                    continue;
                value->flags |= XI_FLAG_MAY_THROW;
                if (func == caller)
                    caller_call = value;
            }
        }
    }
    TEST_REQUIRE(caller != NULL && caller_call != NULL, "caller direct call must survive lowering");

    XrType unit_type = {
        .kind = XR_KIND_UNIT, .scalar_rep = XR_SCALAR_REP_NONE, .id = 1901, .frozen = true};
    XiValue *check =
        xi_value_insert_after(caller, caller_call->block, caller_call, XI_ERR_CHECK, &unit_type, 1);
    TEST_REQUIRE(check != NULL, "conservative error check inserted");
    check->args[0] = caller_call;

    XiCgenCtx *ctx = xi_cgen_ctx_new();
    TEST_REQUIRE(ctx != NULL, "CGen context allocated");
    xi_cgen_ctx_set_aot_bundle(ctx, &plan.bundle);
    char *code = NULL;
    size_t code_size = 0;
    FILE *mem = xr_open_memstream(&code, &code_size);
    TEST_REQUIRE(mem != NULL, "CGen output stream allocated");
    xi_cgen_program(ctx, mem, ir->module);
    TEST_REQUIRE(xr_close_memstream(mem, &code, &code_size) == 0, "CGen output stream closed");
    TEST_REQUIRE(code != NULL && !xi_cgen_has_error(ctx), "closed-world fixture should generate");

    const char *caller_def = find_static_function_definition(code, "test_caller_");
    TEST_REQUIRE(caller_def != NULL, "caller function should be generated");
    const char *caller_end = next_static_after(caller_def);
    TEST_REQUIRE(count_between(caller_def, caller_end, "xrt_has_pending_error") == 0,
                 "closed-world no-throw evidence must remove a conservative TLS error poll");

    printf("  Generated closed-world no-throw call fixture %zu bytes of C code\n", code_size);
    xr_free(code);
    xi_cgen_ctx_free(ctx);
    test_aot_plan_free(&plan);
    xi_func_free(ir);
}

TEST(cgen_static_cleanup_isolates_existing_pending_error) {
    const char *src = "enum E { Bad(code: i64) }\n"
                      "fn run() -> i64 {\n"
                      "    var log: Array<i64> = []\n"
                      "    fn body() {\n"
                      "        defer { log.push(100) }\n"
                      "        throw E.Bad(1)\n"
                      "    }\n"
                      "    try { body() } catch (e) { log.push(1) }\n"
                      "    return log[0] + log[1]\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "static cleanup over pending error should generate");

    assert(contains(code, "xrt_cleanup_enter();") &&
           "cleanup emission must isolate the pending error channel");
    assert(contains(code, "xrt_cleanup_leave();") &&
           "cleanup emission must restore the pending error channel");
    assert(contains(code, "XrtExcFrame _ef") &&
           "cleanup registration must use a static panic interval");
    assert(!contains(code, "XrtDeferScope") && !contains(code, "xrt_defer_") &&
           "generated C must not retain the dynamic defer runtime");
    assert(contains(code, "xrt_pending_error = ") &&
           "throw path must still route through the pending-error channel before draining defers");
    assert(contains(code, "xrt_pending_error = XR_NULL_VAL;") &&
           "catch path must still clear the handled pending error");

    printf("  Generated static cleanup pending-error isolation %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_err_return_stops_unreachable_tail) {
    const char *src = "enum E { Msg(s: string) }\n"
                      "fn failing() -> i64 {\n"
                      "    throw E.Msg(\"boom\")\n"
                      "    return 0\n"
                      "}\n"
                      "print(failing())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "throw plus unreachable return should generate");

    const char *fn = find_static_function_definition(code, "test_failing_");
    assert(fn != NULL && "failing function should be generated with native i64 ABI");
    const char *fn_end = next_static_after(fn);
    assert(count_between(fn, fn_end, "xrt_pending_error =") == 1 &&
           "throw outside try should lower to exactly one pending-error write");
    const char *err_set = strstr(fn, "xrt_pending_error =");
    assert(err_set != NULL && err_set < fn_end && "throw pending-error write should be in failing");
    assert(count_between(err_set, fn_end, "return 0;") == 1 &&
           "throw outside try should return the i64 ABI default once");
    assert(count_between(fn, fn_end, "return 0;\n    v") == 0 &&
           "AOT must not emit unreachable SSA statements after ERR_RETURN");
    assert(count_between(fn, fn_end, "return (int64_t)(v") == 0 &&
           "AOT must not emit the unreachable source return after ERR_RETURN");

    printf("  Generated ERR_RETURN tail stop %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_unsupported_coroutine_ops_fail_fast) {
    static const struct {
        XiOp op;
        const char *name;
    } cases[] = {
        {XI_GO, "GO"},
        {XI_AWAIT, "AWAIT"},
        {XI_CHAN_SEND, "CHAN_SEND"},
        {XI_CHAN_RECV, "CHAN_RECV"},
        {XI_CHAN_TRY_SEND, "CHAN_TRY_SEND"},
        {XI_CHAN_TRY_RECV, "CHAN_TRY_RECV"},
        {XI_CHAN_IS_CLOSED, "CHAN_IS_CLOSED"},
        {XI_SELECT_BLOCK, "SELECT_BLOCK"},
        {XI_TIME_AFTER, "TIME_AFTER"},
    };
    XrType stub_unit = {
        .kind = XR_KIND_UNIT, .scalar_rep = XR_SCALAR_REP_NONE, .id = 100, .frozen = true};
    XiFunc *ir = xi_func_new("main", &stub_unit);
    assert(ir != NULL);

    XiBlock *entry = xi_block_new(ir);
    assert(entry != NULL);

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        XiValue *value = xi_value_new(ir, entry, cases[i].op, &stub_unit, 0);
        assert(value != NULL);
    }
    xi_block_set_return(entry, NULL);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "rejected C generation should return an empty owned buffer");

    TEST_REQUIRE(had_error, "unsupported coroutine Xi ops must be rejected before publication");
    TEST_REQUIRE(code[0] == '\0',
                 "failed C generation must not publish a partial translation unit");
    printf("  Generated rejected %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_unresolved_import_fails_fast) {
    XrType stub_unit = {
        .kind = XR_KIND_UNIT, .scalar_rep = XR_SCALAR_REP_NONE, .id = 102, .frozen = true};
    XrType stub_string = {
        .kind = XR_KIND_STRING, .scalar_rep = XR_SCALAR_REP_NONE, .id = 103, .frozen = true};
    XiFunc *ir = xi_func_new("main", &stub_unit);
    assert(ir != NULL);

    XiBlock *entry = xi_block_new(ir);
    assert(entry != NULL);

    XiImportRef *ref = (XiImportRef *) xi_func_arena_alloc(ir, sizeof(XiImportRef));
    assert(ref != NULL);
    ref->module_path = "./missing";
    ref->member_name = "value";
    ref->resolved_mod_index = -1;
    ref->resolved_shared_slot = -1;
    ref->resolved_export_slot = -1;

    XiValue *import = xi_value_new(ir, entry, XI_IMPORT_REF, &stub_string, 0);
    assert(import != NULL);
    import->aux = ref;
    import->aux_int = -1;
    xi_block_set_return(entry, NULL);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL);

    assert(had_error && "unresolved AOT imports must reject code generation");
    assert(code[0] == '\0' && "unresolved imports must not publish partial C output");

    printf("  Generated rejected unresolved import %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_unknown_method_symbol_fails_fast) {
    XrType stub_unit = {
        .kind = XR_KIND_UNIT, .scalar_rep = XR_SCALAR_REP_NONE, .id = 104, .frozen = true};
    XrType stub_string = {
        .kind = XR_KIND_STRING, .scalar_rep = XR_SCALAR_REP_NONE, .id = 105, .frozen = true};
    XiFunc *ir = xi_func_new("main", &stub_unit);
    assert(ir != NULL);

    XiBlock *entry = xi_block_new(ir);
    assert(entry != NULL);

    XiValue *recv = xi_const_str(ir, entry, "abc", &stub_string);
    assert(recv != NULL);

    XiValue *call = xi_value_new(ir, entry, XI_CALL_METHOD, &stub_string, 1);
    assert(call != NULL);
    call->args[0] = recv;
    call->aux = (void *) "__aot_unknown_method__";
    call->flags |= XI_FLAG_CALL_EFFECTS;
    xi_block_set_return(entry, NULL);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL);

    assert(had_error && "unknown AOT method symbols must reject code generation");
    assert(code[0] == '\0' && "unknown methods must not publish partial C output");

    printf("  Generated rejected unknown method %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

static XiFunc *make_json_codec_preflight_ir(void) {
    static XrType stub_unit = {
        .kind = XR_KIND_UNIT, .scalar_rep = XR_SCALAR_REP_NONE, .id = 107, .frozen = true};
    static XrType stub_json = {.kind = XR_KIND_JSON, .id = 108, .frozen = true};
    static XrType stub_any = {.kind = XR_KIND_UNKNOWN, .id = 109, .frozen = true};
    static XrType stub_string = {
        .kind = XR_KIND_STRING, .scalar_rep = XR_SCALAR_REP_NONE, .id = 110, .frozen = true};
    XiFunc *ir = xi_func_new("main", &stub_unit);
    if (!ir)
        return NULL;
    XiBlock *entry = xi_block_new(ir);
    if (!entry) {
        xi_func_free(ir);
        return NULL;
    }
    XiValue *json_ns = xi_value_new(ir, entry, XI_GET_BUILTIN, &stub_any, 0);
    XiValue *text = xi_const_str(ir, entry, "{}", &stub_string);
    XiValue *site = xi_value_new(ir, entry, XI_CALL_METHOD, &stub_string, 2);
    if (!json_ns || !text || !site) {
        xi_func_free(ir);
        return NULL;
    }
    json_ns->aux_int = XR_GLOBAL_VAR_JSON;
    site->args[0] = json_ns;
    site->args[1] = text;
    site->op = XI_CONST;
    site->nargs = 0;
    site->flags = XI_FLAG_SIDE_EFFECT;
    site->line = 17;
    site->xg_json_codec_id = 1;
    site->json_decode_target_type = &stub_json;
    xi_block_set_return(entry, NULL);
    return ir;
}

static XiValue *find_marked_json_codec_site(XiFunc *func) {
    if (!func)
        return NULL;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            XiValue *value = block->values[vi];
            if (value && value->xg_json_codec_id == 1)
                return value;
        }
    }
    for (uint16_t ci = 0; ci < func->nchildren; ci++) {
        XiValue *site = find_marked_json_codec_site(func->children[ci]);
        if (site)
            return site;
    }
    return NULL;
}

static char *generate_c_with_injected_json_codec_summary(XiFunc *ir,
                                                         const XgJsonCodecSummary *codec_summary,
                                                         bool typed_site, bool refresh_hash,
                                                         bool verify_bundle, bool make_decode_site,
                                                         bool make_non_site, bool *had_error) {
    if (!test_prepare_backend_ir(ir))
        return test_failed_codegen_result(had_error);
    XiValue *site = find_marked_json_codec_site(ir);
    TEST_REQUIRE(site != NULL, "Json codec evidence site should survive Backend preparation");
    XiModule *mod = xi_module_new("test.xr", "test", ir);
    TEST_REQUIRE(mod != NULL, "Json codec evidence preflight module allocation failed");
    XiModule *modules[] = {mod};
    TestAotPlan plan;
    test_aot_plan_prepare(&plan, modules, 1, 0);
    if (codec_summary)
        TEST_REQUIRE(xg_global_evidence_add_json_codec(&plan.evidence, codec_summary) != NULL,
                     "Json codec evidence allocation failed");
    if (refresh_hash)
        plan.bundle.global_evidence_plan.evidence_hash = xg_global_evidence_hash(&plan.evidence);
    if (verify_bundle) {
        char verify_error[256] = {0};
        TEST_REQUIRE(xaot_verify_bundle(&plan.bundle, verify_error, sizeof(verify_error)),
                     verify_error[0] ? verify_error
                                     : "Json codec evidence bundle verification failed");
    }
    if (!make_non_site) {
        site->flags = XI_FLAG_SIDE_EFFECT;
        site->type = site->json_decode_target_type;
        if (typed_site) {
            site->op = XI_JSON_DECODE;
            site->nargs = 1;
            site->lowering_flags = make_decode_site ? 0 : XI_LOWERING_FLAG_JSON_TYPED_PARSE;
        } else {
            site->op = XI_CALL_METHOD;
            site->nargs = 2;
            site->aux = (void *) "parseValue";
            site->lowering_flags = 0;
        }
    }

    XiCgenCtx *ctx = xi_cgen_ctx_new();
    TEST_REQUIRE(ctx != NULL, "Json codec evidence preflight CGen context allocation failed");
    xi_cgen_ctx_set_aot_bundle(ctx, &plan.bundle);
    char *buf = NULL;
    size_t bufsz = 0;
    FILE *mem = xr_open_memstream(&buf, &bufsz);
    TEST_REQUIRE(mem != NULL, "Json codec evidence preflight memstream allocation failed");
    xi_cgen_program(ctx, mem, mod);
    int rc = xr_close_memstream(mem, &buf, &bufsz);
    TEST_REQUIRE(rc == 0, "Json codec evidence preflight memstream close failed");
    if (had_error)
        *had_error = xi_cgen_has_error(ctx);

    xi_cgen_ctx_free(ctx);
    test_aot_plan_free(&plan);
    mod->init = NULL;
    xi_module_free(mod);
    return buf;
}

TEST(cgen_json_codec_summary_preflight_is_exact_and_fail_closed) {
    XgJsonCodecSummary direct = {.codec_id = 1,
                                 .module_id = 1,
                                 .owner_func_id = 1,
                                 .source_node_id = 99,
                                 .source_span_id = 1,
                                 .codec_kind = XG_JSON_CODEC_PARSE,
                                 .input_type_key = 1};
    XgJsonCodecSummary typed = direct;
    typed.flags = XG_JSON_CODEC_HAS_TARGET_TYPE;
    typed.target_type_key = 2;
    XgJsonCodecSummary stale_owner = direct;
    stale_owner.owner_func_id = 7;
    XgJsonCodecSummary stale_source = direct;
    stale_source.source_node_id = 0;
    XgJsonCodecSummary wrong_kind = typed;
    wrong_kind.codec_kind = XG_JSON_CODEC_DECODE;
    XgJsonCodecSummary decode_without_target = direct;
    decode_without_target.codec_kind = XG_JSON_CODEC_DECODE;
    const XgJsonCodecSummary *invalid_direct_cases[] = {
        NULL, &stale_owner, &stale_source, &wrong_kind, &typed,
    };

    {
        XiFunc *ir = make_json_codec_preflight_ir();
        TEST_REQUIRE(ir != NULL && test_prepare_backend_ir(ir),
                     "exact Json codec evidence fixture reached Backend");
        XiModule *mod = xi_module_new("test.xr", "test", ir);
        TEST_REQUIRE(mod != NULL, "exact Json codec evidence module allocation failed");
        XiModule *modules[] = {mod};
        TestAotPlan plan;
        test_aot_plan_prepare(&plan, modules, 1, 0);
        TEST_REQUIRE(xg_global_evidence_add_json_codec(&plan.evidence, &direct) != NULL,
                     "exact Json codec evidence allocation failed");
        plan.bundle.global_evidence_plan.evidence_hash = xg_global_evidence_hash(&plan.evidence);
        char verify_error[256] = {0};
        TEST_REQUIRE(xaot_verify_bundle(&plan.bundle, verify_error, sizeof(verify_error)),
                     verify_error[0] ? verify_error
                                     : "exact Json codec evidence bundle verification failed");
        test_aot_plan_free(&plan);
        mod->init = NULL;
        xi_module_free(mod);
        xi_func_free(ir);
    }

    for (uint32_t i = 0; i < sizeof(invalid_direct_cases) / sizeof(invalid_direct_cases[0]); i++) {
        XiFunc *ir = make_json_codec_preflight_ir();
        TEST_REQUIRE(ir != NULL, "direct Json.parse negative fixture should build");
        bool had_error = false;
        bool summary_is_globally_valid = i >= 3;
        char *code = generate_c_with_injected_json_codec_summary(ir, invalid_direct_cases[i], false,
                                                                 true, summary_is_globally_valid,
                                                                 false, false, &had_error);
        TEST_REQUIRE(code != NULL, "Json codec evidence preflight should return a C buffer");
        TEST_REQUIRE(had_error, "invalid Json codec evidence must fail C generation");
        TEST_REQUIRE(code[0] == '\0', "Json codec evidence preflight must fail before emitting C");
        xr_free(code);
        xi_func_free(ir);
    }

    {
        XiFunc *ir = make_json_codec_preflight_ir();
        TEST_REQUIRE(ir != NULL, "typed/direct mismatch fixture should build");
        bool had_error = false;
        char *code = generate_c_with_injected_json_codec_summary(ir, &direct, true, true, true,
                                                                 false, false, &had_error);
        TEST_REQUIRE(code != NULL, "typed/direct mismatch should return a C buffer");
        TEST_REQUIRE(had_error, "typed parse must reject schema-less direct parse evidence");
        TEST_REQUIRE(code[0] == '\0', "typed/direct mismatch must fail before emitting C");
        xr_free(code);
        xi_func_free(ir);
    }

    {
        XiFunc *ir = make_json_codec_preflight_ir();
        TEST_REQUIRE(ir != NULL, "decode target-evidence fixture should build");
        bool had_error = false;
        char *code = generate_c_with_injected_json_codec_summary(
            ir, &decode_without_target, true, true, false, true, false, &had_error);
        TEST_REQUIRE(code != NULL, "decode target-evidence mismatch should return a C buffer");
        TEST_REQUIRE(had_error, "Json decode must require exact target-type evidence");
        TEST_REQUIRE(code[0] == '\0', "decode target mismatch must fail before emitting C");
        xr_free(code);
        xi_func_free(ir);
    }

    {
        XiFunc *ir = make_json_codec_preflight_ir();
        TEST_REQUIRE(ir != NULL, "stale Json evidence hash fixture should build");
        bool had_error = false;
        char *code = generate_c_with_injected_json_codec_summary(ir, &direct, false, false, false,
                                                                 false, false, &had_error);
        TEST_REQUIRE(code != NULL, "stale Json evidence hash should return a C buffer");
        TEST_REQUIRE(had_error, "stale Json evidence hash must fail C generation");
        TEST_REQUIRE(code[0] == '\0', "stale Json evidence hash must fail before emitting C");
        xr_free(code);
        xi_func_free(ir);
    }

    {
        XiFunc *ir = make_json_codec_preflight_ir();
        TEST_REQUIRE(ir != NULL, "non-Json evidence fixture should build");
        bool had_error = false;
        char *code = generate_c_with_injected_json_codec_summary(ir, &direct, false, true, true,
                                                                 false, true, &had_error);
        TEST_REQUIRE(code != NULL, "non-Json evidence preflight should return a C buffer");
        TEST_REQUIRE(had_error, "a non-Json site carrying a codec id must fail C generation");
        TEST_REQUIRE(code[0] == '\0', "non-Json codec identity must fail before emitting C");
        xr_free(code);
        xi_func_free(ir);
    }
}

TEST(cgen_suspendable_function_has_no_sync_wrapper) {
    const char *src = "fn worker(n: i64) -> i64 {\n"
                      "    Coro.yield()\n"
                      "    return n + 1\n"
                      "}\n"
                      "var task = go worker(41)\n"
                      "var result = await task\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "supported AOT coroutine should generate without cgen errors");
    assert(contains(code, "_aot_resume") && "suspendable function should emit AOT resume entry");
    assert(!contains(code, "return (abort(), XR_NULL_VAL);") &&
           "suspendable functions must not publish an aborting sync wrapper");

    printf("  Generated suspendable function %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_direct_suspend_call_propagates_cps) {
    const char *src = "fn worker(n: i64) -> i64 {\n"
                      "    Coro.yield()\n"
                      "    return n + 1\n"
                      "}\n"
                      "print(worker(41))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "static direct suspend calls should CPS-propagate to the caller");
    assert(contains(code, "call_frame_") &&
           "caller frame should own the child suspend frame while blocked");
    assert(contains(code, "_aot_resume") && "direct suspend call should use AOT resume entries");
    assert(nonzero_state_precedes_call(code, "_aot_resume(f->call_frame_") &&
           "direct suspend calls must publish caller state before child resume");
    assert(contains(code, "_aot_frame_init(f->call_frame_") &&
           "scalar direct suspend call frames should be reset and reused after synchronous "
           "completion");
    assert(contains(code, "    f->state = 0;\n") &&
           "reusable AOT frame init should reset entry state without clearing cached child frames");
    assert(contains(code, "    memset(f, 0, sizeof(*f));\n    if (!") &&
           "fresh AOT frame allocation should still clear owned frame storage once");
    assert(contains(code, "runtime_cfg.scheduler_workers = 1;") &&
           "single-scheduler entry plans must configure the same AOT default as VM");
    assert(!contains(code, "return (abort(), XR_NULL_VAL);") &&
           "direct suspend calls must not require a sync abort wrapper");
    assert(!contains(code, "unsupported AOT sync call") &&
           "diagnostics should go to stderr, not generated C comments");

    printf("  Generated direct suspend call %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_direct_suspend_call_borrows_read_argument) {
    const char *src = "fn worker(xs: Array<i64>) -> i64 {\n"
                      "    Coro.yield()\n"
                      "    return len(xs)\n"
                      "}\n"
                      "var xs = [1, 2, 3]\n"
                      "print(worker(xs))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "direct suspend call with READ argument should generate");
    assert(contains(code, "((void)xrt_retain(") &&
           "the child frame must retain its ordinary borrowed argument while suspended");
    assert(!contains(code, "xrt_value_clone_for_coro(") &&
           "ordinary suspend calls must not turn READ arguments into deep copies");

    printf("  Generated borrowed direct suspend call %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_direct_suspend_enum_result_consumes_owned_box) {
    const char *src =
        "import mem\n"
        "export enum Failure { Closed, Failed }\n"
        "export enum Outcome { Unit(id: i64, generation: i64), Echo(id: i64, generation: i64, "
        "payload: Buffer?), State(id: i64, generation: i64, n: i64), Failed(reason: Failure) }\n"
        "fn pause() { Coro.yield() }\n"
        "fn step() { pause() }\n"
        "fn worker(n: i64) -> Outcome {\n"
        "    var owned = Outcome.Echo(1, 2, mem.allocZeroed(n))\n"
        "    step()\n"
        "    return owned\n"
        "}\n"
        "fn run() -> i64 {\n"
        "    var result = worker(41)\n"
        "    return match (result) { Outcome.Echo(_id, _generation, payload) -> "
        "len(payload!.asBytes()), _ -> 0 }\n"
        "}\n"
        "print(run())\n";

    XiPipelineConfig cfg = xi_pipeline_aot_config();
    XiFunc *ir = compile_to_ir_with_config(src, cfg);
    TEST_REQUIRE(ir != NULL, "suspend enum result IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "suspend enum result C generation failed");
    TEST_REQUIRE(!had_error, "suspend enum result should lower through the typed aggregate ABI");
    TEST_REQUIRE(contains(code, "xrt_enum_aggregate_take_from_boxed(_call_value_"),
                 "caller consumes the owned suspend-result box");
    TEST_REQUIRE(contains(code, "xrt_enum_aggregate_box_from_borrowed("),
                 "frame-root enum return acquires payload owners before frame release");
    const char *take = strstr(code, "xrt_enum_aggregate_take_from_boxed(_call_value_");
    unsigned call_id = 0;
    TEST_REQUIRE(
        take && sscanf(take, "xrt_enum_aggregate_take_from_boxed(_call_value_%u)", &call_id) == 1,
        "owned suspend-result call id is recoverable from generated C");
    char retain_needle[64];
    snprintf(retain_needle, sizeof(retain_needle), "xrt_retain(_call_value_%u)", call_id);
    TEST_REQUIRE(!contains(code, retain_needle),
                 "inline enum result transfers payload ownership without retaining its wrapper");
    TEST_REQUIRE(contains(code, "xrt_enum_aggregate_release("),
                 "discarded local call result releases every owned inline enum payload lane");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_returned_suspendable_closure_uses_verified_child_frame) {
    const char *src = "fn makeWorker() -> fn() -> i64 {\n"
                      "    return fn() -> i64 {\n"
                      "        Coro.yield()\n"
                      "        return 41\n"
                      "    }\n"
                      "}\n"
                      "var worker = makeWorker()\n"
                      "print(worker())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "returned suspendable closure IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "returned suspendable closure C generation failed");
    assert(!had_error && "closed returned callable target should verify and lower");
    assert(contains(code, "call_frame_") &&
           "indirect suspend invocation should own a verified child frame");
    assert(contains(code, ".sync_entry=NULL") &&
           "suspendable returned closure must not expose a sync entry");
    assert(contains(code, "XrAotCallableDesc") &&
           contains(code, "xrt_closure_new(&_xr_callable_") && !contains(code, "->fn") &&
           "generated closure ABI must use descriptor identity only");
    assert(!contains(code, "return (abort(), XR_NULL_VAL);") &&
           "returned suspendable closure must not contain an abort wrapper");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_mixed_callable_targets_use_stable_descriptor_switch) {
    const char *src = "fn syncWorker() -> i64 { return 40 }\n"
                      "fn suspendWorker() -> i64 { Coro.yield(); return 41 }\n"
                      "var worker = syncWorker\n"
                      "fn choose(suspend: bool) {\n"
                      "    if (suspend) { worker = suspendWorker }\n"
                      "    else { worker = syncWorker }\n"
                      "}\n"
                      "choose(false)\n"
                      "print(worker())\n"
                      "choose(true)\n"
                      "print(worker())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "mixed callable target IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "mixed callable target C generation failed");
    assert(!had_error && "closed mixed callable target set should verify and lower");
    assert(contains(code, "switch (f->call_target_id_") &&
           contains(code, "->callable->target_id") &&
           "mixed callable invocation must dispatch on stable descriptor identity");
    assert(contains(code, "->callable->sync_entry") && contains(code, "_aot_frame_new") &&
           contains(code, "_aot_resume(f->call_frame_") &&
           "mixed target switch must separate sync entry from suspend frame dispatch");
    assert(!contains(code, "strcmp(") && !contains(code, "xrt_typename") &&
           "mixed callable dispatch must not recover target identity from strings or types");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_direct_suspend_method_call_propagates_cps) {
    const char *src = "class Box {\n"
                      "    constructor() {}\n"
                      "    bump(n: i64) -> i64 {\n"
                      "        Coro.yield()\n"
                      "        return n + 1\n"
                      "    }\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var box = Box()\n"
                      "    return box.bump(41)\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "static direct suspend method calls should CPS-propagate to the caller");
    assert(contains(code, "call_frame_") &&
           "caller frame should own the child method frame while blocked");
    assert(contains(code, "_aot_resume(f->call_frame_") &&
           "direct suspend method calls must resume through the child frame");
    assert(contains(code, "_aot_frame_new(") &&
           "direct suspend method calls must allocate a child frame");
    assert(!contains(code, "unsupported AOT sync method call") &&
           "method suspendability must be represented as CPS, not a sync-wrapper failure");

    printf("  Generated direct suspend method call %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_shared_static_function_retain_is_elided) {
    const char *src = "fn inc(n: i64) -> i64 {\n"
                      "    return n + 1\n"
                      "}\n"
                      "var result = inc(41)\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "static direct calls should generate without cgen errors");
    assert(contains(code, "test_inc_") && "static function should be emitted directly");
    assert(!contains(code, "xrt_retain(v") &&
           "direct calls to uncaptured shared functions should not retain the callee closure");

    printf("  Generated shared static call %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_shared_static_function_retain_is_elided) {
    const char *src = "fn inc(n: i64) -> i64 {\n"
                      "    return n + 1\n"
                      "}\n"
                      "fn worker(n: i64) -> i64 {\n"
                      "    Coro.yield()\n"
                      "    return inc(n)\n"
                      "}\n"
                      "var task = go worker(41)\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT coroutine static calls should generate without cgen errors");
    assert(contains(code, "test_inc_") && "static function should be emitted directly");
    const char *worker_resume =
        find_static_function_definition(code, "static XrAotResult test_worker_");
    assert(worker_resume != NULL && "worker coroutine resume function should be emitted");
    const char *worker_resume_end = next_static_after(worker_resume);
    assert(!contains_between(worker_resume, worker_resume_end, "xrt_retain(v") &&
           "AOT coroutine direct calls to uncaptured shared functions should not retain callee "
           "closure");

    printf("  Generated coroutine shared static call %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_suspendable_dependency_init_fails_fast) {
    XrType stub_unit = {
        .kind = XR_KIND_UNIT, .scalar_rep = XR_SCALAR_REP_NONE, .id = 101, .frozen = true};

    XiFunc *dep = xi_func_new("init", &stub_unit);
    XiFunc *entry = xi_func_new("init", &stub_unit);
    assert(dep != NULL && entry != NULL);

    XiBlock *dep_entry = xi_block_new(dep);
    XiBlock *main_entry = xi_block_new(entry);
    assert(dep_entry != NULL && main_entry != NULL);

    XiValue *yield = xi_value_new(dep, dep_entry, XI_YIELD, &stub_unit, 0);
    assert(yield != NULL);
    yield->flags |= XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(dep_entry, NULL);
    xi_block_set_return(main_entry, NULL);

    XiModule *dep_mod = xi_module_new("dep.xr", "dep", dep);
    XiModule *entry_mod = xi_module_new("main.xr", "main", entry);
    assert(dep_mod != NULL && entry_mod != NULL);
    XiModule *modules[] = {dep_mod, entry_mod};
    TestAotPlan plan;
    bool prepared = test_aot_plan_try_prepare(&plan, modules, 2, 1);
    assert(!prepared && "suspendable dependency init must fail AOT planning");
    assert(plan.bundle.error_msg != NULL);
    assert(strstr(plan.bundle.error_msg, "module initializer may not suspend") != NULL);

    printf("  Rejected suspendable dependency initializer during AOT planning\n");
    test_aot_plan_free(&plan);
    xi_module_free(dep_mod);
    xi_module_free(entry_mod);
    xi_func_free(dep);
    xi_func_free(entry);
}

TEST(cgen_coro_frame_params_use_typed_storage) {
    const char *src = "fn worker(n: i64) -> i64 {\n"
                      "    Coro.yield()\n"
                      "    return n + 1\n"
                      "}\n"
                      "var task = go worker(41)\n"
                      "var result = await task\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT coroutine with typed params should generate");
    assert(contains(code, "uint32_t state;\n    int64_t p0;") &&
           "AOT coroutine i64 params should be stored unboxed in the frame");
    assert(contains(code, "f->p0 = XR_TO_INT(p0);") &&
           "AOT coroutine frame factory should unbox i64 params at the boundary");
    assert(!contains(code, "p0.i") &&
           "AOT coroutine typed params must not be unboxed as tagged values");
    assert(!contains(code, "xr_aot_trace_frame_value(visitor, f->p0)") &&
           "scalar frame params must not be traced as XrValue roots");
    assert(!contains(code, "xrt_value_clone_for_coro(") &&
           "scalar go and await boundaries must not call the deep-copy helper");
    const char *worker_trace = find_static_function_definition(code, "test_worker_1_aot_trace");
    const char *worker_trace_end = next_static_after(worker_trace);
    assert(!contains_between(worker_trace, worker_trace_end, "xr_aot_trace_frame_value") &&
           "worker scalar frame must not trace XrValue roots");
    const char *worker_desc = strstr(code, "static const XrAotCoroDesc test_worker_1_aot_desc = {");
    assert(worker_desc != NULL && "worker coroutine descriptor should be generated");
    const char *worker_desc_end = strstr(worker_desc, "};");
    assert(worker_desc_end != NULL && "worker coroutine descriptor should be closed");
    assert(contains_between(worker_desc, worker_desc_end, ".root_count = 0,") &&
           "scalar coroutine frame should report zero traced roots");
    assert(contains_between(worker_desc, worker_desc_end, ".release_count = 0,") &&
           "scalar coroutine frame should report zero ARC release slots");

    printf("  Generated typed coroutine param frame %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_frame_skips_dead_ssa_slots) {
    const char *src = "fn worker(n: i64) -> i64 {\n"
                      "    var a = n + 1\n"
                      "    var b = a + 2\n"
                      "    Coro.yield()\n"
                      "    return n + 3\n"
                      "}\n"
                      "var task = go worker(41)\n"
                      "var result = await task\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT coroutine with dead pre-yield values should generate");

    const char *frame = strstr(code, "typedef struct test_worker_");
    assert(frame != NULL && "worker coroutine frame should be emitted");
    const char *frame_end = strstr(frame, "} test_worker_");
    assert(frame_end != NULL && "worker coroutine frame should have an end marker");
    assert(count_between(frame, frame_end, "\n    int64_t v") == 0 &&
           "dead scalar SSA values must stay out of the suspend frame");
    assert(count_between(frame, frame_end, "\n    XrValue v") == 0 &&
           "dead tagged SSA values must stay out of the suspend frame");
    assert(contains(code, "#define v0 (f->p0)") &&
           "parameter SSA references should alias the typed frame parameter");
    assert(contains(code, "int64_t v") &&
           "non-frame scalar SSA values should be resume-local variables");

    printf("  Generated compact coroutine frame %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_loop_tail_phi_uses_shared_suspend_plan) {
    const char *src = "fn worker(ch: Channel<i64>, timeout_ms: i64, value: i64) -> bool {\n"
                      "    return match (ch.sendTimeout(value, timeout_ms)) {\n"
                      "        SendResult.Timeout -> true\n"
                      "        _ -> false\n"
                      "    }\n"
                      "}\n"
                      "fn run_once(count: i64, timeout_ms: i64) -> i64 {\n"
                      "    const ch = Channel<i64>(1)\n"
                      "    ch.send(1)\n"
                      "    var tasks: Array<Task<bool>> = []\n"
                      "    for (var i = 0; i < count; i++) {\n"
                      "        tasks.push(go worker(ch, timeout_ms, i + 2))\n"
                      "    }\n"
                      "    var total = 0\n"
                      "    for (task in tasks) {\n"
                      "        if (await task) {\n"
                      "            total++\n"
                      "        }\n"
                      "    }\n"
                      "    var _drain = ch.recv()\n"
                      "    ch.close()\n"
                      "    return total\n"
                      "}\n"
                      "print(run_once(3, 10))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT coroutine loop-tail phi should generate");

    const char *frame = strstr(code, "typedef struct test_run_once_");
    assert(frame != NULL && "run_once coroutine frame should be emitted");
    const char *frame_end = strstr(frame, "} test_run_once_");
    assert(frame_end != NULL && "run_once coroutine frame should have an end marker");
    assert(contains_between(frame, frame_end, "\n    int64_t phi14;") &&
           contains_between(frame, frame_end, "\n    int64_t phi39;") &&
           contains_between(frame, frame_end, "\n    int64_t phi32;") &&
           "live loop-tail phi values must be stored in the shared suspend frame");

    const char *resume = strstr(frame_end, "test_run_once_");
    resume = resume ? strstr(resume, "_aot_resume(void *raw_frame") : NULL;
    const char *trace = resume ? strstr(resume, "_aot_trace(void *frame") : NULL;
    assert(resume != NULL && trace != NULL && "run_once resume/trace functions should exist");
    assert(!contains_between(resume, trace, "\n    int64_t phi20 = 0;") &&
           !contains_between(resume, trace, "\n    int64_t phi15 = 0;") &&
           !contains_between(resume, trace, "\n    int64_t phi14 = 0;") &&
           !contains_between(resume, trace, "\n    int64_t phi38 = 0;") &&
           !contains_between(resume, trace, "\n    int64_t phi33 = 0;") &&
           !contains_between(resume, trace, "\n    int64_t phi32 = 0;") &&
           "cross-suspend loop phi values must alias frame fields, not resume-local temps");
    assert(!contains(code, "_xr_aot_coro_poll_count") &&
           "CGen must not discover or insert coroutine poll states after plan freeze");
    assert(!contains(code, "_yield_poll_") &&
           "all emitted suspension states must come from the shared Xi coroutine plan");

    printf("  Generated loop-tail phi frame %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

/* The contract is general: a loop whose backedge runs into a blocking wait
 * needs no poll safepoint, because the wait is already a suspension point.
 * Written against a channel receive -- EventCount is a worker-pool internal
 * with no user-reachable type surface, so a source-level guard cannot spell
 * it. */
TEST(cgen_coro_wait_driven_loop_omits_redundant_poll) {
    const char *src = "const ch: Channel<i64> = Channel<i64>(1)\n"
                      "fn worker(workerId: i64) -> i64 {\n"
                      "    var seen = 0\n"
                      "    while (true) {\n"
                      "        match (ch.recv()) {\n"
                      "            Recv.Value(v) -> { seen = seen + v }\n"
                      "            _ -> { return seen }\n"
                      "        }\n"
                      "    }\n"
                      "    return seen\n"
                      "}\n"
                      "fn main() {\n"
                      "    var task = go worker(0)\n"
                      "    ch.send(5)\n"
                      "    ch.close()\n"
                      "    print(await task)\n"
                      "}\n"
                      "main()\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT wait-driven loop should generate");
    assert(contains(code, "xr_aot_chan_recv_pair_i64(ctx,") &&
           "test must exercise a blocking channel receive in a coroutine loop");
    assert(!contains(code, "xr_aot_poll_yield_kind(ctx)") &&
           "loop backedge into a blocking wait should not emit a redundant poll safepoint");

    printf("  Generated wait-driven loop without redundant poll %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_countdown_latch_methods_use_native_helpers) {
    const char *src = "const latch: CountdownLatch = CountdownLatch(0)\n"
                      "fn syncUse() -> i64 {\n"
                      "    var ok = latch.reset(2)\n"
                      "    var left = latch.done()\n"
                      "    var ready = latch.tryWait()\n"
                      "    latch.close()\n"
                      "    return (ok ? left : 0) + (ready ? 1 : 0)\n"
                      "}\n"
                      "fn worker() -> i64 {\n"
                      "    Coro.yield()\n"
                      "    var ok = latch.reset(1)\n"
                      "    var left = latch.done(1)\n"
                      "    if (latch.tryWait()) {\n"
                      "        return ok ? left : -1\n"
                      "    }\n"
                      "    return -2\n"
                      "}\n"
                      "fn main() {\n"
                      "    print(syncUse())\n"
                      "    var task = go worker()\n"
                      "    latch.close()\n"
                      "    print(await task)\n"
                      "}\n"
                      "main()\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT CountdownLatch methods should generate");
    assert(contains(code, "xr_aot_countdown_latch_reset_bool_sync(") &&
           "sync CountdownLatch.reset should use the native bool helper");
    assert(contains(code, "xr_aot_countdown_latch_done_i64_sync(") &&
           "sync CountdownLatch.done should use the native i64 helper");
    assert(contains(code, "xr_aot_countdown_latch_try_wait_bool_sync(") &&
           "sync CountdownLatch.tryWait should use the native bool helper");
    assert(contains(code, "xr_aot_countdown_latch_close_void_sync(") &&
           "sync CountdownLatch.close should use the native void helper");
    assert(contains(code, "xr_aot_countdown_latch_reset_bool(ctx,") &&
           "coroutine CountdownLatch.reset should use the native bool helper");
    assert(contains(code, "xr_aot_countdown_latch_done_i64(ctx,") &&
           "coroutine CountdownLatch.done should use the native i64 helper");
    assert(contains(code, "xr_aot_countdown_latch_try_wait_bool(ctx,") &&
           "coroutine CountdownLatch.tryWait should use the native bool helper");
    assert(contains(code, "xr_aot_countdown_latch_close_void(ctx,") &&
           "coroutine CountdownLatch.close should use the native void helper");
    assert(!contains(code, "XrValue _latch_reset_") &&
           "CountdownLatch.reset should not materialize an XrValue temp");
    assert(!contains(code, "XrValue _latch_done_") &&
           "CountdownLatch.done should not materialize an XrValue temp");
    assert(!contains(code, "XrValue _latch_try_wait_") &&
           "CountdownLatch.tryWait should not materialize an XrValue temp");
    assert(!contains(code, "XrValue _latch_close_") &&
           "CountdownLatch.close should not materialize a tagged Unit result");
    assert(!contains(code, "XR_TO_INT(_latch_") &&
           "CountdownLatch native helper results should not be immediately unboxed");
    assert(!contains(code, "xr_aot_countdown_latch_close_sync(") &&
           "sync CountdownLatch.close should not return tagged XrValue");

    printf("  Generated CountdownLatch native method bridge %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_semaphore_methods_use_native_helpers) {
    const char *src = "const sem: Semaphore = Semaphore(0)\n"
                      "fn syncUse() -> i64 {\n"
                      "    var released = sem.release(2)\n"
                      "    var ok = sem.tryAcquire()\n"
                      "    sem.close()\n"
                      "    return released + (ok ? 1 : 0)\n"
                      "}\n"
                      "fn worker() -> i64 {\n"
                      "    Coro.yield()\n"
                      "    var released = sem.release()\n"
                      "    var ok = sem.tryAcquire()\n"
                      "    sem.close()\n"
                      "    return released + (ok ? 1 : 0)\n"
                      "}\n"
                      "fn main() {\n"
                      "    print(syncUse())\n"
                      "    var task = go worker()\n"
                      "    print(await task)\n"
                      "}\n"
                      "main()\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT Semaphore methods should generate");
    assert(contains(code, "xr_aot_semaphore_release_i64_sync(") &&
           "sync Semaphore.release should use the native i64 helper");
    assert(contains(code, "xr_aot_semaphore_try_acquire_bool_sync(") &&
           "sync Semaphore.tryAcquire should use the native bool helper");
    assert(contains(code, "xr_aot_semaphore_close_void_sync(") &&
           "sync Semaphore.close should use the native void helper");
    assert(contains(code, "xr_aot_semaphore_release_i64(ctx,") &&
           "coroutine Semaphore.release should use the native i64 helper");
    assert(contains(code, "xr_aot_semaphore_try_acquire_bool(ctx,") &&
           "coroutine Semaphore.tryAcquire should use the native bool helper");
    assert(contains(code, "xr_aot_semaphore_close_void(ctx,") &&
           "coroutine Semaphore.close should use the native void helper");
    assert(!contains(code, "XrValue _sem_release_") &&
           "Semaphore.release should not materialize an XrValue temp");
    assert(!contains(code, "XrValue _sem_try_acquire_") &&
           "Semaphore.tryAcquire should not materialize an XrValue temp");
    assert(!contains(code, "XrValue _sem_close_") &&
           "Semaphore.close should not materialize a tagged Unit result");
    assert(!contains(code, "XR_TO_INT(_sem_release_") &&
           "Semaphore native helper results should not be immediately unboxed");
    assert(!contains(code, "xr_aot_semaphore_release_sync(") &&
           "sync Semaphore.release should not return tagged XrValue");
    assert(!contains(code, "xr_aot_semaphore_try_acquire_sync(") &&
           "sync Semaphore.tryAcquire should not return tagged XrValue");

    printf("  Generated Semaphore native method bridge %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_sync_blocking_direct_methods_mark_aot_coroutines) {
    const char *src = "const sem: Semaphore = Semaphore(0)\n"
                      "const latch: CountdownLatch = CountdownLatch(1)\n"
                      "fn worker() -> i64 {\n"
                      "    if (!sem.acquire()) {\n"
                      "        return -1\n"
                      "    }\n"
                      "    latch.done()\n"
                      "    return latch.wait() ? 1 : 0\n"
                      "}\n"
                      "fn main() {\n"
                      "    var task = go worker()\n"
                      "    sem.release()\n"
                      "    print(await task)\n"
                      "}\n"
                      "main()\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT blocking sync methods should generate as coroutine calls");
    assert(contains(code, "xr_aot_semaphore_acquire(ctx,") &&
           "Semaphore.acquire should lower through the coroutine wait helper");
    assert(contains(code, "xr_aot_countdown_latch_wait(ctx,") &&
           "CountdownLatch.wait should lower through the coroutine wait helper");
    assert(contains(code, "xr_aot_countdown_latch_done_i64(ctx,") &&
           "CountdownLatch.done in the coroutine body should use the native i64 helper");
    assert(contains(code, "xr_aot_semaphore_release_i64(ctx,") &&
           "Semaphore.release in the coroutine main should use the native i64 helper");
    assert(!contains(code, "unsupported AOT method") &&
           "sync primitive direct calls must not fall through to unsupported method dispatch");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_runtime_managed_types_skip_arc) {
    XrType task_type = {.kind = XR_KIND_INSTANCE};
    XrType channel_type = {.kind = XR_KIND_CHANNEL};
    XrType string_type = {.kind = XR_KIND_STRING};

    task_type.instance.class_name = "Task";

    /* Only Task/Coroutine are runtime-managed (executor-owned): the compiler
     * skips ARC for them. Channel is pure cross-coroutine shared DATA with no
     * executor owner, so it uses the atomic shared-RC like a string — the
     * compiler DOES track it (last drop frees), which is what stops channels
     * created and discarded in a loop from leaking. */
    assert(!xi_own_type_is_rc(&task_type) && "Task is owned by the coroutine runtime");
    assert(xi_own_type_is_rc(&channel_type) && "Channel is atomic shared-RC, compiler-tracked");
    assert(xi_own_type_is_rc(&string_type) && "String remains compiler ARC managed");
}

TEST(cgen_coro_frame_release_uses_aot_arc) {
    const char *src = "fn worker() -> string {\n"
                      "    var s = \"hello\" + \"_aot\"\n"
                      "    Coro.yield()\n"
                      "    return s\n"
                      "}\n"
                      "var task = go worker()\n"
                      "var result = await task\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT coroutine with ARC values should generate");
    assert(contains(code, "_aot_release(void *frame, struct XrCoroHeap *heap)") &&
           "frame release must receive coroutine heap context");
    assert(contains(code, "xrt_release(xr_str_value_from_ptr(f->v") &&
           "AOT ARC string ptr slot should be released after restoring XrValue tag");
    assert(contains(code, "xr_aot_trace_frame_value(visitor, xr_str_value_from_ptr(f->v") &&
           "AOT ARC string ptr slot should be traced after restoring XrValue tag");
    assert(contains(code, ".root_count = 1,") &&
           "AOT ARC string frame should report one traced root slot");
    assert(contains(code, ".release_count = 1,") &&
           "AOT ARC string frame should report one release slot");
    assert(contains(code, "xr_aot_frame_free(frame)") &&
           "AOT coroutine frame release should free the frame");
    assert(!contains(code, "xr_aot_release_frame_value(f->") &&
           "frame release must not call the old untyped release helper");

    printf("  Generated ARC-aware coroutine release %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_owner_forward_clears_moved_frame_root) {
    const char *src = "fn worker() -> string {\n"
                      "    var value = \"hello\" + \"_owner\"\n"
                      "    Coro.yield()\n"
                      "    var result = value\n"
                      "    return result\n"
                      "}\n"
                      "var task = go worker()\n"
                      "var result = await task\n"
                      "print(result)\n";

    XiPipelineConfig cfg = xi_pipeline_aot_config();
    XiFunc *ir = compile_to_ir_with_config(src, cfg);
    TEST_REQUIRE(ir != NULL, "owner-forward coroutine IR compilation failed");

    const XiFunc *worker = NULL;
    for (uint16_t i = 0; i < ir->nchildren; i++) {
        if (ir->children[i] && ir->children[i]->name &&
            strcmp(ir->children[i]->name, "worker") == 0) {
            worker = ir->children[i];
            break;
        }
    }
    TEST_REQUIRE(worker != NULL, "owner-forward coroutine worker function missing");

    const XiValue *retained_source = NULL;
    const XiValue *moved_source = NULL;
    const XiValue *forward_source = NULL;
    const XiValue *forward = NULL;
    XiBlock *forward_block = NULL;
    uint32_t retain_index = UINT32_MAX;
    uint32_t source_release_index = UINT32_MAX;
    for (uint32_t bi = 0; bi < worker->nblocks && !forward; bi++) {
        XiBlock *block = worker->blocks[bi];
        for (uint32_t vi = 0; block && vi < block->nvalues; vi++) {
            const XiValue *value = block->values[vi];
            if (!value || value->op != XI_OWNER_FORWARD || value->nargs < 1 || !value->args[0])
                continue;
            const XiValue *owner = value->args[0];
            while (owner &&
                   (owner->op == XI_BOX || owner->op == XI_UNBOX ||
                    xi_copy_is_identity_alias(owner)) &&
                   owner->nargs >= 1)
                owner = owner->args[0];
            if (!owner || !owner->type || owner->type->kind != XR_KIND_STRING)
                continue;
            const XiValue *prev = vi > 0 ? block->values[vi - 1] : NULL;
            if (!prev || prev->op != XI_RETAIN || prev->nargs < 1 ||
                (prev->args[0] != owner && prev->args[0] != value->args[0]))
                continue;
            retained_source = prev->args[0];
            moved_source = value->args[0];
            forward_source = value->args[0];
            forward = value;
            forward_block = block;
            retain_index = vi - 1;
            for (uint32_t ri = vi + 1; ri < block->nvalues; ri++) {
                const XiValue *release = block->values[ri];
                if (release && release->op == XI_RELEASE && release->nargs >= 1 &&
                    release->args[0] == retained_source) {
                    source_release_index = ri;
                    break;
                }
            }
            break;
        }
    }
    TEST_REQUIRE(retained_source && moved_source && forward_source && forward && forward_block &&
                     retain_index != UINT32_MAX && source_release_index != UINT32_MAX,
                 "fixture should contain a retained frame-owner forward");

    /* Model the legal last-owner shape produced on the HTTP close edge:
     * OWNER_FORWARD(source) without a balancing retain/release of source. The
     * backend plan and suspension liveness are unchanged; only ownership of the
     * already-planned frame slot transfers to the forward. */
    memmove(&forward_block->values[source_release_index],
            &forward_block->values[source_release_index + 1],
            (forward_block->nvalues - source_release_index - 1) * sizeof(XiValue *));
    forward_block->nvalues--;
    memmove(&forward_block->values[retain_index], &forward_block->values[retain_index + 1],
            (forward_block->nvalues - retain_index - 1) * sizeof(XiValue *));
    forward_block->nvalues--;

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "AOT owner-forward coroutine should generate");
    const char *resume = find_static_function_definition(code, "static XrAotResult test_worker_");
    TEST_REQUIRE(resume != NULL, "owner-forward coroutine resume definition missing");
    const char *resume_end = next_static_after(resume);

    char move_line[64];
    char clear_tagged_line[64];
    char clear_ptr_line[64];
    snprintf(move_line, sizeof(move_line), "v%u = v%u;", forward->id, forward_source->id);
    snprintf(clear_tagged_line, sizeof(clear_tagged_line), "v%u = XR_NULL_VAL;", moved_source->id);
    snprintf(clear_ptr_line, sizeof(clear_ptr_line), "v%u = NULL;", moved_source->id);
    const char *move_pos = strstr(resume, move_line);
    const char *clear_tagged_pos = move_pos ? strstr(move_pos, clear_tagged_line) : NULL;
    const char *clear_ptr_pos = move_pos ? strstr(move_pos, clear_ptr_line) : NULL;
    const char *clear_pos = clear_tagged_pos ? clear_tagged_pos : clear_ptr_pos;
    const char *return_pos = clear_pos ? strstr(clear_pos, "return xr_aot_done(") : NULL;
    TEST_REQUIRE(move_pos && move_pos < resume_end, "frame owner forward assignment missing");
    TEST_REQUIRE(clear_pos && clear_pos < resume_end,
                 "moved frame owner must be cleared before frame teardown");
    TEST_REQUIRE(return_pos && return_pos < resume_end,
                 "forwarded owner must remain available after the source frame slot is cleared");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_go_clones_tagged_args) {
    const char *src = "fn worker(xs: move Array<i64>) -> i64 {\n"
                      "    xs.push(99)\n"
                      "    Coro.yield()\n"
                      "    return len(xs)\n"
                      "}\n"
                      "var xs = [1, 2]\n"
                      "var task = go worker(copy(xs))\n"
                      "var result = await task\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT coroutine with tagged go args should generate");
    assert(contains(code, "xrt_value_clone_for_coro(") &&
           "explicit copy(...) go arguments must be cloned once at the coroutine boundary");

    printf("  Generated coroutine argument clone %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_go_sync_function_uses_wrapper_desc) {
    const char *src = "fn compute(n: i64) -> i64 {\n"
                      "    return n * n\n"
                      "}\n"
                      "fn mutate_copy(xs: move Array<i64>) -> i64 {\n"
                      "    xs.push(99)\n"
                      "    return len(xs)\n"
                      "}\n"
                      "fn identity_copy(xs: move Array<i64>) -> Array<i64> {\n"
                      "    return xs\n"
                      "}\n"
                      "var high = go(name: \"compute\") compute(5)\n"
                      "print(await high)\n"
                      "var xs = [1, 2]\n"
                      "var copied = go mutate_copy(copy(xs))\n"
                      "print(await copied)\n"
                      "print(len(xs))\n"
                      "var roundtrip = go identity_copy(copy(xs))\n"
                      "var ys = await roundtrip\n"
                      "print(len(ys))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT go of sync functions should generate");
    assert(contains(code, "int64_t _raw_result = test_compute_") &&
           "sync go wrapper must call typed normal function bodies");
    assert(contains(code, "int64_t _raw_result = test_mutate_copy_") &&
           "sync go wrapper must pass tagged params to typed normal functions");
    assert(contains(code, "void * _raw_result = test_identity_copy_") &&
           "sync go wrapper must support pointer results that alias frame params");
    assert(contains(code, "XrValue _result = XR_FROM_INT(_raw_result)") &&
           "sync go wrapper must box native scalar results for the coroutine ABI");
    assert(contains(code, "XrValue _result = xr_mkptr(_raw_result, XR_TAG_ARRAY)") &&
           "sync go wrapper must box native pointer results for the coroutine ABI");
    assert(contains(code, "xrt_retain(_result)") &&
           "sync go wrapper must retain a result that aliases an owned frame param");
    assert(contains(code, "xrt_release(xr_mkptr(f->p0, XR_TAG_ARRAY))") &&
           "sync go wrapper must release cloned pointer frame params after restoring the tag");
    assert(contains(code, ".release_count = 1,") &&
           "sync go wrapper descriptor must report cloned tagged param releases");
    assert(contains(code, "xr_aot_done(_result)") &&
           "sync go wrapper must complete through the AOT coroutine result ABI");
    assert(contains(code, "xr_aot_trace_frame_value(visitor, xr_mkptr(f->p0, XR_TAG_ARRAY))") &&
           "sync go wrapper pointer params must remain traceable while queued");
    assert(contains(code, "xrt_value_clone_for_coro(") &&
           "sync go tagged arguments must still cross the coroutine clone boundary");
    assert(contains(code, "_aot_desc, _child_frame_") &&
           "go lowering must spawn sync wrappers through an AOT descriptor");

    printf("  Generated sync go wrapper %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_go_sync_scalar_wrapper_skips_param_roots) {
    const char *src = "fn compute(n: i64, flag: bool) -> i64 {\n"
                      "    if (flag) { return n + 1 }\n"
                      "    return n\n"
                      "}\n"
                      "var task = go compute(3, true)\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT go of scalar sync functions should generate");

    const char *frame = strstr(code, "typedef struct test_compute_");
    assert(frame != NULL && "compute sync-go frame should be emitted");
    const char *frame_end = strstr(frame, "} test_compute_");
    assert(frame_end != NULL && "compute sync-go frame should close");
    assert(count_between(frame, frame_end, "int64_t p") == 2 &&
           "scalar sync-go params should be stored unboxed in the frame");
    assert(count_between(frame, frame_end, "XrValue p") == 0 &&
           "scalar sync-go params should not use tagged frame storage");
    assert(!contains(code, "xr_aot_trace_frame_value(visitor, f->p0)") &&
           "scalar sync-go params must not be traced as roots");
    assert(!contains(code, "xr_aot_trace_frame_value(visitor, f->p1)") &&
           "scalar sync-go params must not be traced as roots");
    assert(contains(code, ".root_count = 0,") &&
           "scalar sync-go wrapper should report zero root slots");
    assert(contains(code, ".release_count = 0,") &&
           "scalar sync-go wrapper should report zero release slots");

    printf("  Generated scalar sync-go wrapper %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_go_zero_state_sync_wrapper_has_nonempty_frame) {
    const char *src = "fn one() -> i64 {\n"
                      "    return 1\n"
                      "}\n"
                      "var task = go one()\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    XiCgenCoroFrameStats stats = {0};
    char *code = generate_c_with_status_and_stats(ir, "test", &had_error, &stats);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT go of zero-state sync function should generate");
    assert(contains(code, "uint8_t _empty;") &&
           "zero-state sync-go frame must be non-empty and C-portable");
    assert(contains(code, "xr_aot_frame_alloc(sizeof(*f))") &&
           "zero-state sync-go wrapper must still allocate a backend frame");
    assert(stats.coroutine_count == 2 &&
           "frame stats should count the main coroutine and zero-state sync-go frame");
    assert(stats.max_frame_bytes >= 1 && "zero-state sync-go frame must report non-zero size");

    printf("  Generated zero-state sync-go wrapper %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_sync_functions_without_go_emit_no_aot_wrappers) {
    const char *src = "fn helper(n: i64) -> i64 {\n"
                      "    return n + 1\n"
                      "}\n"
                      "print(helper(2))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    XiCgenCoroFrameStats stats = {0};
    char *code = generate_c_with_status_and_stats(ir, "test", &had_error, &stats);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT sync-only program should generate");
    assert(stats.coroutine_count == 0 &&
           "sync-only programs should not report coroutine frame stats");
    assert(!contains(code, "_aot_desc") && "unused sync functions should not emit AOT descs");
    assert(!contains(code, "typedef struct test_helper_") &&
           "unused sync functions should not emit sync-go frame structs");

    printf("  Generated sync-only program without wrappers %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_sync_go_wrappers_only_for_go_targets) {
    const char *src = "fn unused(n: i64) -> i64 {\n"
                      "    return n + 1\n"
                      "}\n"
                      "fn used(n: i64) -> i64 {\n"
                      "    return n * 2\n"
                      "}\n"
                      "var task = go used(3)\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    XiCgenCoroFrameStats stats = {0};
    char *code = generate_c_with_status_and_stats(ir, "test", &had_error, &stats);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT go of sync function should generate");
    assert(stats.coroutine_count == 2 &&
           "frame stats should count the main coroutine and the sync-go frame");
    assert(stats.total_frame_bytes >= stats.max_frame_bytes &&
           "frame stats should preserve aggregate frame bytes");
    assert(contains(code, "int64_t _raw_result = test_used_") &&
           "go target should keep its sync-go wrapper");
    assert(!contains(code, "_raw_result = test_unused_") &&
           "non-go sync functions must not emit sync-go wrappers");
    assert(!contains(code, "typedef struct test_unused_") &&
           "non-go sync functions must not emit sync-go frame structs");

    printf("  Generated only needed sync-go wrappers %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_sync_aot_backedge_heartbeat_only_for_runtime_reachable_loops) {
    const char *non_go_loop_src = "fn unused(n: i64) -> i64 {\n"
                                  "    var i = 0\n"
                                  "    var sum = 0\n"
                                  "    while (i < n) {\n"
                                  "        sum = sum + i\n"
                                  "        i = i + 1\n"
                                  "    }\n"
                                  "    return sum\n"
                                  "}\n"
                                  "fn used(n: i64) -> i64 {\n"
                                  "    return n + 1\n"
                                  "}\n"
                                  "var task = go used(3)\n"
                                  "print(await task)\n";

    XiFunc *ir = compile_to_ir(non_go_loop_src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "non-loop sync-go target should generate");
    assert(!contains(code, "xr_aot_sync_backedge_heartbeat();") &&
           "plain sync loops must not inherit sync-go heartbeat instrumentation");
    xr_free(code);
    xi_func_free(ir);

    const char *go_loop_src = "fn used(n: i64) -> i64 {\n"
                              "    var i = 0\n"
                              "    var sum = 0\n"
                              "    while (i < n) {\n"
                              "        sum = sum + i\n"
                              "        i = i + 1\n"
                              "    }\n"
                              "    return sum\n"
                              "}\n"
                              "var task = go used(3)\n"
                              "print(await task)\n";

    ir = compile_to_ir(go_loop_src);
    assert(ir != NULL && "IR compilation failed");

    had_error = false;
    code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "looping sync-go target should generate");
    assert(contains(code, "uint32_t _xr_aot_sync_backedge_count = 0;") &&
           "looping sync-go target should declare a throttled heartbeat counter");
    assert(contains(code, "xr_aot_sync_backedge_heartbeat();") &&
           "looping sync-go target should bump scheduler heartbeat on hot backedges");

    xr_free(code);
    xi_func_free(ir);

    const char *coro_direct_sync_loop_src = "fn syncLoop(n: i64) -> i64 {\n"
                                            "    var i = 0\n"
                                            "    var sum = 0\n"
                                            "    while (i < n) {\n"
                                            "        sum = sum + i\n"
                                            "        i = i + 1\n"
                                            "    }\n"
                                            "    return sum\n"
                                            "}\n"
                                            "fn used(n: i64) -> i64 {\n"
                                            "    return n + 1\n"
                                            "}\n"
                                            "var x = syncLoop(3)\n"
                                            "var task = go used(3)\n"
                                            "print((await task) + x)\n";

    ir = compile_to_ir(coro_direct_sync_loop_src);
    assert(ir != NULL && "IR compilation failed");

    had_error = false;
    code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "coroutine direct sync loop call should generate");
    assert(contains(code, "uint32_t _xr_aot_sync_backedge_count = 0;") &&
           "sync loop called from an AOT coroutine should declare a heartbeat counter");
    assert(contains(code, "xr_aot_sync_backedge_heartbeat();") &&
           "sync loop called from an AOT coroutine should bump scheduler heartbeat");

    printf("  Generated sync AOT backedge heartbeat instrumentation %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_channel_send_copy_uses_transfer_helper) {
    const char *src = "const ch: Channel<Array<i64>> = Channel(1)\n"
                      "var xs = [1, 2]\n"
                      "ch.send(copy(xs))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT channel send should generate");
    assert(contains(code, "xr_aot_chan_send_transfer(ctx,") &&
           "boxed channel copy send must use the transfer-aware AOT bridge");
    assert(nonzero_state_precedes_call(code, "xr_aot_chan_send_transfer(ctx,") &&
           "channel send must publish the AOT resume state before runtime blocking");
    assert(contains(code, "uint8_t _chan_send_transfer_mode_") && contains(code, " = 1;") &&
           contains(code, ", xr_slot_none(), -1, _chan_send_transfer_mode_") &&
           "copy send must encode XR_TRANSFER_COPY at the runtime boundary");
    const char *send_bridge = strstr(code, "XrValue _chan_send_send_value_");
    const char *send_call = strstr(code, "xr_aot_chan_send_transfer(ctx,");
    assert(send_bridge != NULL && send_call != NULL && send_bridge < send_call &&
           "transfer bridge should be emitted before the send call");
    assert(contains_between(send_bridge, send_call, "xr_aot_bridge_xrt_to_runtime(ctx,") &&
           "every boxed send must enter the backend-neutral runtime envelope");
    assert(!contains_between(send_bridge, send_call, "xrt_value_clone_for_coro(") &&
           "copy send must not clone before the transfer-aware channel helper");

    printf("  Generated channel send transfer %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_scalar_channel_send_skips_clone) {
    const char *src = "const ch = Channel<i64>(1)\n"
                      "ch.send(42)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT scalar channel send should generate");
    assert(contains(code, "xr_aot_chan_send_i64(ctx,") &&
           "scalar channel send must use the typed AOT bridge");
    assert(nonzero_state_precedes_call(code, "xr_aot_chan_send_i64(ctx,") &&
           "scalar channel send must publish the AOT resume state before runtime blocking");
    assert(!contains(code, "xr_aot_chan_send(ctx,") &&
           "scalar channel send must not re-box at the generated call site");
    assert(count_between(code, code + strlen(code), "XR_FROM_INT(") == 1 &&
           "scalar channel send should not emit a dead boxed send operand");
    const char *typed_send_call = strstr(code, "xr_aot_chan_send_i64(ctx,");
    assert(typed_send_call != NULL && "typed send call should exist");
    assert(!contains_between(typed_send_call, code + strlen(code), "xrt_value_clone_for_coro(") &&
           "scalar channel send values must not call the deep-copy helper");

    printf("  Generated scalar channel send %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_unit_match_send_omits_void_phi) {
    const char *src = "fn recv_timeout_until_close(ch: Channel<i64>, done: Channel<i64>) {\n"
                      "    match (ch.recvTimeout(1)) {\n"
                      "        Recv.Value(value) -> done.send(value)\n"
                      "        _ -> done.send(-1)\n"
                      "    }\n"
                      "}\n"
                      "const ch = Channel<i64>(0)\n"
                      "const done = Channel<i64>(1)\n"
                      "go recv_timeout_until_close(ch, done)\n"
                      "ch.close()\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT unit match coroutine should generate");
    assert(contains(code, "xr_aot_chan_send_i64(ctx,") &&
           "unit match branches should still emit typed channel sends");
    assert(!contains(code, "phi") && "unit/void phi values should not materialize in C");

    printf("  Generated unit match send without void phi %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_descriptor_scalar_channel_try_send_uses_typed_sync_bridge) {
    const char *src = "const ch = Channel<i64>(1)\n"
                      "var ok = ch.trySend(42)\n"
                      "print(ok)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT scalar channel trySend should generate");
    assert(contains(code, "xr_aot_chan_try_send_sync_i64(") &&
           "descriptor-root channel trySend must use the typed synchronous AOT bridge");
    assert(!contains(code, "xr_aot_poll_yield_kind(ctx)") &&
           "nonblocking trySend must not own a suspend/poll state");
    assert(!contains(code, "xr_aot_chan_try_send(ctx,") &&
           "scalar channel trySend must not re-box at the generated call site");
    assert(count_between(code, code + strlen(code), "XR_FROM_INT(") == 1 &&
           "scalar channel trySend should not emit a dead boxed send operand");
    const char *typed_try_send_call = strstr(code, "xr_aot_chan_try_send_sync_i64(");
    assert(typed_try_send_call != NULL && "typed trySend call should exist");
    assert(
        !contains_between(typed_try_send_call, code + strlen(code), "xrt_value_clone_for_coro(") &&
        "scalar channel trySend values must not call the deep-copy helper");
    assert(!contains(code, "xr_aot_bridge_value_to_xrt(") &&
           "trySend returns a native no-payload SendResult enum and must not be bridged");
    assert(!contains(code, "xr_aot_bridge_xrt_to_runtime(&xrt_global_ctx,") &&
           "scalar channel trySend must stay on the typed fast path");

    printf("  Generated scalar channel trySend %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_descriptor_tagged_channel_try_send_normalizes_runtime_envelope) {
    const char *src = "fn try_tuple(ch: Channel<(i64, i64)>) -> SendResult {\n"
                      "    var frame = (1, 2)\n"
                      "    return ch.trySend(move frame)\n"
                      "}\n"
                      "const ch = Channel<(i64, i64)>(1)\n"
                      "print(try_tuple(ch))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT tagged channel trySend should generate");
    const char *try_send_call = strstr(code, "xr_aot_chan_try_send_sync(");
    assert(try_send_call != NULL &&
           "tagged channel trySend must use the synchronous tagged bridge");
    const char *try_send_end = strchr(try_send_call, ';');
    assert(try_send_end != NULL &&
           contains_between(try_send_call, try_send_end,
                            "xr_aot_bridge_xrt_to_runtime(&xrt_global_ctx,") &&
           "tagged channel trySend must normalize the AOT value into the runtime envelope");
    assert(!contains_between(try_send_call, try_send_end, "xrt_value_clone_for_coro(") &&
           "move trySend must not deep-copy the transferred value");
    assert(!contains(code, "xr_aot_bridge_value_to_xrt(") &&
           "trySend returns a native no-payload SendResult enum and must not be bridged");

    printf("  Generated tagged channel trySend runtime envelope %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_builtin_no_payload_enum_fields_skip_bridge) {
    const char *src = "fn read_builtin_enums() -> i64 {\n"
                      "    Coro.yield()\n"
                      "    var sent = SendResult.Sent\n"
                      "    var closed = Recv.Closed\n"
                      "    var pending = TaskResult.Pending\n"
                      "    var status = TaskStatus.Success\n"
                      "    print(sent)\n"
                      "    print(closed)\n"
                      "    print(pending)\n"
                      "    print(status)\n"
                      "    return 1\n"
                      "}\n"
                      "var task = go read_builtin_enums()\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT builtin enum field coroutine should generate");
    assert(contains(code, "xr_aot_load_builtin_field(ctx,") &&
           "builtin enum fields should use the AOT builtin field helper");
    assert(!contains(code, "xr_aot_bridge_value_to_xrt(_builtin_field_") &&
           "no-payload builtin enum fields must be returned as native AOT enum keys");

    printf("  Generated builtin no-payload enum field bridge skip %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_await_clones_tagged_result) {
    const char *src = "fn worker() -> Array<i64> {\n"
                      "    Coro.yield()\n"
                      "    return [1, 2]\n"
                      "}\n"
                      "var task = go worker()\n"
                      "var result = await task\n"
                      "print(len(result))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT await should generate");
    assert(contains(code, "xr_aot_await_task(ctx,") && "await must use the AOT bridge");
    assert(nonzero_state_precedes_call(code, "xr_aot_await_task(ctx,") &&
           "await must publish the AOT resume state before runtime blocking");
    assert(contains(code, "xr_aot_await_task_resume(ctx, xr_slot_xvalue_ptr(&") &&
           "await resume must recover the task from coroutine wait state");
    assert(!contains(code, "xr_aot_await_task_resume(ctx, v") &&
           "await resume must not keep the task operand in the AOT frame");
    assert(contains(code, "xrt_value_clone_for_coro(") &&
           "tagged await results must be cloned at the coroutine boundary");

    printf("  Generated await result clone %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_hosted_string_array_boundary_uses_deep_value_bridge) {
    const char *src = "fn bridge(values: Array<string>) -> Array<string> { return values }\n"
                      "bridge([\"alpha\", \"beta\"])\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "hosted string-array fixture compiled");

    XiFunc *bridge = NULL;
    for (uint16_t i = 0; i < ir->nchildren; i++) {
        if (ir->children[i] && ir->children[i]->name &&
            strcmp(ir->children[i]->name, "bridge") == 0) {
            bridge = ir->children[i];
            break;
        }
    }
    TEST_REQUIRE(bridge != NULL, "hosted string-array bridge function lowered");
    XrCExportPlan export_plan = {
        .xray_name = "bridge",
        .symbol = "xr_hosted_string_array_bridge",
        .visibility = "hidden",
        .abi = "hosted-vm-v1",
        .header = true,
    };
    bridge->export_plan = &export_plan;

    bool had_error = false;
    char *code = generate_c_with_status_and_stats_for_artifact(ir, "test", &had_error, NULL,
                                                               XAOT_ARTIFACT_HOSTED_FRAGMENT);
    TEST_REQUIRE(code != NULL && !had_error, "hosted string-array C bridge generated");
    const XrSemanticFunctionRecord *contract =
        xr_semantic_plan_function(bridge->semantic_plan, bridge->semantic_plan_function_index);
    TEST_REQUIRE(contract && contract->return_provenance == XR_SEM_RETURN_BORROWED_PARAM &&
                     contract->return_parameter == 0,
                 "hosted adapter consumes the frozen borrowed-result contract");
    TEST_REQUIRE(contains(code, "XrHostedFragmentArrayView _hosted_array_view_0"),
                 "VM array is inspected through the hosted container ABI");
    TEST_REQUIRE(contains(code, "context->ops->array_get"),
                 "reference-bearing elements use host array access");
    TEST_REQUIRE(contains(code, "xrt_str_from_slice"),
                 "VM strings are copied into AOT-owned elements");
    TEST_REQUIRE(contains(code, "context->ops->array_new"),
                 "AOT result materializes a VM-owned array");
    TEST_REQUIRE(contains(code, "context->ops->array_set"),
                 "AOT string results populate through host ownership operations");
    /* `bridge` returns a borrowed view of its parameter. The adapter promotes
     * that view to an owned result before releasing its materialized argument,
     * so conversion can use the result without a use-after-free and both owned
     * references have one matching release. */
    const char *retain_result = strstr(code, "xrt_retain(_hosted_result)");
    const char *release_argument = strstr(code, "xrt_release(_hosted_arg_0)");
    TEST_REQUIRE(retain_result && release_argument && retain_result < release_argument,
                 "a borrowed result is promoted before its parameter owner is released");
    TEST_REQUIRE(contains(code, "xrt_release(_hosted_result)"),
                 "the promoted AOT result is released once the VM copy exists");
    const char *hosted_entry = strstr(code, "xr_hosted_string_array_bridge(");
    const char *hosted_initializer = strstr(code, "bool xr_hosted_fragment_initialize(");
    const char *runtime_scope =
        hosted_entry ? strstr(hosted_entry, "_hosted_previous_runtime_context") : NULL;
    TEST_REQUIRE(hosted_entry && hosted_initializer &&
                     (!runtime_scope || runtime_scope >= hosted_initializer),
                 "object-only hosted export keeps the zero-scope direct C fast path");
    const char *hosted_init_context =
        strstr(hosted_initializer, "xrt_hosted_aot_context = &_hosted_runtime_context;");
    const char *hosted_init_call =
        hosted_init_context ? strstr(hosted_init_context, "(NULL);\n") : NULL;
    const char *hosted_init_check =
        hosted_init_context ? strstr(hosted_init_context, "xrt_has_pending_error()") : NULL;
    TEST_REQUIRE(hosted_init_call && hosted_init_check && hosted_init_call < hosted_init_check,
                 "hosted initialization executes the module graph before exports");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_hosted_byte_slice_boundary_uses_layout_neutral_view) {
    const char *src = "fn bridge(data: Slice<u8>) -> i64 { return len(data) }\n"
                      "var text = \"abc\"\n"
                      "bridge(text.bytes())\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "hosted byte-slice fixture compiled");

    XiFunc *bridge = NULL;
    for (uint16_t i = 0; i < ir->nchildren; i++) {
        if (ir->children[i] && ir->children[i]->name &&
            strcmp(ir->children[i]->name, "bridge") == 0) {
            bridge = ir->children[i];
            break;
        }
    }
    TEST_REQUIRE(bridge != NULL, "hosted byte-slice bridge function lowered");
    XrCExportPlan export_plan = {
        .xray_name = "bridge",
        .symbol = "xr_hosted_byte_slice_bridge",
        .visibility = "hidden",
        .abi = "hosted-vm-v1",
        .header = true,
    };
    bridge->export_plan = &export_plan;

    bool had_error = false;
    char *code = generate_c_with_status_and_stats_for_artifact(ir, "test", &had_error, NULL,
                                                               XAOT_ARTIFACT_HOSTED_FRAGMENT);
    TEST_REQUIRE(code != NULL && !had_error, "hosted byte-slice C bridge generated");
    TEST_REQUIRE(contains(code, "xrt_span_from_string_bytes("),
                 "String.bytes mechanically consumes the frozen C emission recipe");
    TEST_REQUIRE(!contains(code, "xrt_str_to_bytes("),
                 "String.bytes has no legacy allocating conversion fallback");
    TEST_REQUIRE(contains(code, "XrHostedFragmentByteSpanView _hosted_byte_span_0"),
                 "byte slice is borrowed through the hosted byte-span ABI");
    TEST_REQUIRE(contains(code, "context->ops->byte_span_view"),
                 "byte slice uses the host operation");
    TEST_REQUIRE(!contains(code, "(xrt_array_t *)arguments[0].ptr"),
                 "host container headers are never reinterpreted");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_hosted_runtime_capability_installs_scoped_vm_context) {
    const char *src = "fn bridge(value: i64) -> i64 {\n"
                      "    var cell = Atomic(value)\n"
                      "    return cell.load(Ordering.Relaxed)\n"
                      "}\n"
                      "bridge(7)\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "hosted Atomic fixture compiled");

    XiFunc *bridge = NULL;
    for (uint16_t i = 0; i < ir->nchildren; i++) {
        if (ir->children[i] && ir->children[i]->name &&
            strcmp(ir->children[i]->name, "bridge") == 0) {
            bridge = ir->children[i];
            break;
        }
    }
    TEST_REQUIRE(bridge != NULL, "hosted Atomic bridge function lowered");
    XrCExportPlan export_plan = {
        .xray_name = "bridge",
        .symbol = "xr_hosted_atomic_bridge",
        .visibility = "hidden",
        .abi = "hosted-vm-v1",
        .header = true,
    };
    bridge->export_plan = &export_plan;

    bool had_error = false;
    char *code = generate_c_with_status_and_stats_for_artifact(ir, "test", &had_error, NULL,
                                                               XAOT_ARTIFACT_HOSTED_FRAGMENT);
    TEST_REQUIRE(code != NULL && !had_error, "hosted Atomic C bridge generated");
    TEST_REQUIRE(contains(code, "if (!context->runtime_ops)"),
                 "runtime-dependent hosted export rejects a missing VM context");
    TEST_REQUIRE(contains(code, "_hosted_previous_runtime_context"),
                 "runtime-dependent hosted export installs a scoped VM context");
    TEST_REQUIRE(contains(code, "xrt_hosted_aot_context = _hosted_previous_runtime_context"),
                 "runtime-dependent hosted export restores the previous VM context");
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_hosted_coroutine_export_publishes_resumable_continuation) {
    const char *src = "fn bridge(value: i64) -> i64 {\n"
                      "    Coro.yield()\n"
                      "    return value\n"
                      "}\n"
                      "var marker = 1\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "hosted coroutine fixture compiled");

    XiFunc *bridge = NULL;
    for (uint16_t i = 0; i < ir->nchildren; i++) {
        if (ir->children[i] && ir->children[i]->name &&
            strcmp(ir->children[i]->name, "bridge") == 0) {
            bridge = ir->children[i];
            break;
        }
    }
    TEST_REQUIRE(bridge != NULL, "hosted coroutine bridge function lowered");
    XrCExportPlan export_plan = {
        .xray_name = "bridge",
        .symbol = "xr_hosted_coroutine_bridge",
        .visibility = "hidden",
        .abi = "hosted-vm-v1",
        .header = true,
    };
    bridge->export_plan = &export_plan;

    bool had_error = false;
    char *code = generate_c_with_status_and_stats_for_artifact(ir, "test", &had_error, NULL,
                                                               XAOT_ARTIFACT_HOSTED_FRAGMENT);
    TEST_REQUIRE(code != NULL && !had_error, "hosted coroutine C bridge generated");
    TEST_REQUIRE(contains(code, "void *_hosted_continuation = context->continuation"),
                 "hosted coroutine accepts an opaque continuation on resume");
    TEST_REQUIRE(contains(code, "if (_hosted_continuation)"),
                 "hosted coroutine distinguishes first entry from resume");
    TEST_REQUIRE(contains(code, "goto _hosted_resume"),
                 "hosted coroutine skips argument materialization when resuming");
    TEST_REQUIRE(contains(code, "_aot_frame_new"),
                 "hosted coroutine allocates a generated AOT frame on first entry");
    TEST_REQUIRE(contains(code, "_aot_resume"),
                 "hosted coroutine re-enters through the canonical resume function");
    TEST_REQUIRE(contains(code, "context->signal->continuation = _hosted_continuation"),
                 "blocked or yielded execution publishes the owned continuation");
    TEST_REQUIRE(contains(code, "context->signal->suspend_kind ="),
                 "hosted coroutine distinguishes blocked and cooperative yield");
    TEST_REQUIRE(contains(code, "_aot_release(_hosted_continuation, NULL)"),
                 "terminal execution releases the generated frame exactly once");
    TEST_REQUIRE(contains(code, "bool xr_hosted_fragment_initialize("),
                 "hosted artifact exposes an explicit dependency initializer");
    TEST_REQUIRE(contains(code, "xrt_hosted_aot_context = &_hosted_runtime_context"),
                 "hosted initialization installs the borrowed VM runtime context");
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_hosted_class_boundary_uses_nominal_opaque_proxy) {
    const char *src = "class Box {\n"
                      "    value: i64\n"
                      "    constructor(value: i64) { this.value = value }\n"
                      "}\n"
                      "fn bridge(value: Box) -> Box { return value }\n"
                      "bridge(Box(7))\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "hosted class fixture compiled");
    TEST_REQUIRE(ir->module != NULL, "hosted class fixture has module metadata");
    ir->module->path = "E:\\repo\\stdlib\\objects\\objects.xr";
    ir->module->name = "objects_deadbeef";

    XiFunc *bridge = NULL;
    for (uint16_t i = 0; i < ir->nchildren; i++) {
        if (ir->children[i] && ir->children[i]->name &&
            strcmp(ir->children[i]->name, "bridge") == 0) {
            bridge = ir->children[i];
            break;
        }
    }
    TEST_REQUIRE(bridge != NULL, "hosted class bridge function lowered");
    XrCExportPlan export_plan = {
        .xray_name = "bridge",
        .symbol = "xr_hosted_class_bridge",
        .visibility = "hidden",
        .abi = "hosted-vm-v1",
        .header = true,
    };
    bridge->export_plan = &export_plan;

    bool had_error = false;
    char *code = generate_c_with_status_and_stats_for_artifact(ir, "test", &had_error, NULL,
                                                               XAOT_ARTIFACT_HOSTED_FRAGMENT);
    TEST_REQUIRE(code != NULL && !had_error, "hosted class C bridge generated");
    TEST_REQUIRE(contains(code, "XrHostedFragmentObjectView _hosted_object_0"),
                 "VM class proxy is unwrapped through the hosted object ABI");
    TEST_REQUIRE(contains(code, "context->ops->object_view"),
                 "class arguments never cast a VM instance layout in generated code");
    TEST_REQUIRE(contains(code, "strcmp(_hosted_object_0.type_name, \"Box\")"),
                 "class argument validates nominal type identity");
    TEST_REQUIRE(contains(code, "strcmp(_hosted_object_0.nominal_owner, \"objects\")"),
                 "class argument uses the source module's stable logical owner");
    TEST_REQUIRE(contains(code, "context->ops->object_new"),
                 "AOT class results transfer into a VM proxy");
    TEST_REQUIRE(contains(code, "context->host, \"objects\", \"Box\", _hosted_result"),
                 "class result publishes the same stable logical owner");
    TEST_REQUIRE(contains(code, "xrt_release(_hosted_result)"),
                 "failed proxy construction releases the owned AOT result");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_hosted_native_field_store_uses_portable_c_statements) {
    const char *src = "class Box {\n"
                      "    text: string\n"
                      "    constructor(text: string) { this.text = text }\n"
                      "}\n"
                      "fn setText(box: Box, text: string) {\n"
                      "    var target = box\n"
                      "    target.text = text\n"
                      "}\n"
                      "setText(Box(\"before\"), \"after\")\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "hosted native field-store fixture compiled");

    XiFunc *setter = NULL;
    for (uint16_t i = 0; i < ir->nchildren; i++) {
        if (ir->children[i] && ir->children[i]->name &&
            strcmp(ir->children[i]->name, "setText") == 0) {
            setter = ir->children[i];
            break;
        }
    }
    TEST_REQUIRE(setter != NULL, "hosted native field-store function lowered");
    XrCExportPlan export_plan = {
        .xray_name = "setText",
        .symbol = "xr_hosted_set_text",
        .visibility = "hidden",
        .abi = "hosted-vm-v1",
        .header = true,
    };
    setter->export_plan = &export_plan;

    bool had_error = false;
    char *code = generate_c_with_status_and_stats_for_artifact(ir, "test", &had_error, NULL,
                                                               XAOT_ARTIFACT_HOSTED_FRAGMENT);
    TEST_REQUIRE(code != NULL && !had_error, "hosted native field-store C generated");
    TEST_REQUIRE(contains(code, "_portable_native_"),
                 "native field store is hoisted to an ordinary scoped statement");
    TEST_REQUIRE(!contains(code, "({ xrt_native_"),
                 "hosted field store contains no GNU statement expression");
    TEST_REQUIRE(contains(code, "_hosted_arg_1 = xrt_str_from_slice("),
                 "hosted string arguments use AOT-owned storage that may escape the call");
    TEST_REQUIRE(!contains(code, "_hosted_string_header_1"),
                 "hosted string arguments never retain a stack-backed header");
    TEST_REQUIRE(setter->arc_borrow_sig && setter->arc_borrow_sig->valid &&
                     setter->arc_borrow_sig->nparams > 1 &&
                     setter->arc_borrow_sig->param_own[1] == XI_OWN_OWNED,
                 "native field store consumes the string parameter");
    const char *setter_c = find_static_function_definition(code, "test_setText_");
    TEST_REQUIRE(setter_c != NULL, "native field-store target emitted");
    const char *setter_end = next_static_after(setter_c);
    TEST_REQUIRE(!contains_between(setter_c, setter_end, "xrt_retain("),
                 "native field store moves the owned string without redundant ARC traffic");
    TEST_REQUIRE(!contains(code, "xrt_release(_hosted_arg_1)"),
                 "hosted adapter transfers its owned string into the consuming call");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_hosted_non_escaping_string_argument_stays_zero_copy) {
    const char *src = "fn stringLength(text: string) -> i64 { return len(text) }\n"
                      "stringLength(\"value\")\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "hosted non-escaping string fixture compiled");

    XiFunc *target = NULL;
    for (uint16_t i = 0; i < ir->nchildren; i++) {
        if (ir->children[i] && ir->children[i]->name &&
            strcmp(ir->children[i]->name, "stringLength") == 0) {
            target = ir->children[i];
            break;
        }
    }
    TEST_REQUIRE(target != NULL, "hosted non-escaping string function lowered");
    XrCExportPlan export_plan = {
        .xray_name = "stringLength",
        .symbol = "xr_hosted_string_length",
        .visibility = "hidden",
        .abi = "hosted-vm-v1",
        .header = true,
    };
    target->export_plan = &export_plan;

    bool had_error = false;
    char *code = generate_c_with_status_and_stats_for_artifact(ir, "test", &had_error, NULL,
                                                               XAOT_ARTIFACT_HOSTED_FRAGMENT);
    TEST_REQUIRE(code != NULL && !had_error, "hosted non-escaping string C generated");
    const char *stub = find_static_function_definition(code, "xr_hosted_string_length(");
    TEST_REQUIRE(stub != NULL, "hosted non-escaping string stub emitted");
    const char *stub_end = strstr(stub, "\n}\n");
    TEST_REQUIRE(stub_end != NULL, "hosted non-escaping string stub is bounded");
    TEST_REQUIRE(contains_between(stub, stub_end, "_hosted_string_header_0"),
                 "non-escaping string uses a stack borrowed header");
    TEST_REQUIRE(!contains_between(stub, stub_end, "xrt_str_from_slice("),
                 "non-escaping string avoids allocation and byte copying");
    TEST_REQUIRE(!contains_between(stub, stub_end, "xrt_release(_hosted_arg_0)"),
                 "borrowed string header requires no ARC cleanup");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_hosted_normal_class_result_call_is_not_a_constructor) {
    const char *src = "class Box {\n"
                      "    value: i64\n"
                      "    constructor(value: i64) { this.value = value }\n"
                      "}\n"
                      "fn makeBox() -> Box { return Box(7) }\n"
                      "fn forward() -> Box { return makeBox() }\n"
                      "forward()\n";
    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "hosted normal class-result call fixture compiled");

    XiFunc *forward = NULL;
    for (uint16_t i = 0; i < ir->nchildren; i++) {
        if (ir->children[i] && ir->children[i]->name &&
            strcmp(ir->children[i]->name, "forward") == 0) {
            forward = ir->children[i];
            break;
        }
    }
    TEST_REQUIRE(forward != NULL, "hosted forwarding function lowered");
    XrCExportPlan export_plan = {
        .xray_name = "forward",
        .symbol = "xr_hosted_forward_box",
        .visibility = "hidden",
        .abi = "hosted-vm-v1",
        .header = true,
    };
    forward->export_plan = &export_plan;

    bool had_error = false;
    char *code = generate_c_with_status_and_stats_for_artifact(ir, "test", &had_error, NULL,
                                                               XAOT_ARTIFACT_HOSTED_FRAGMENT);
    TEST_REQUIRE(code != NULL && !had_error, "hosted class-result forwarding C generated");
    TEST_REQUIRE(contains(code, "makeBox"),
                 "normal class-returning call remains a direct function call");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_native_class_await_uses_tagged_boundary_slot) {
    const char *src = "class Box {\n"
                      "    value: i64\n"
                      "    constructor(value: i64) { this.value = value }\n"
                      "}\n"
                      "fn worker() -> Box {\n"
                      "    Coro.yield()\n"
                      "    return Box(41)\n"
                      "}\n"
                      "fn consume() -> i64 {\n"
                      "    var task = go worker()\n"
                      "    var box = await task\n"
                      "    return box.value + 1\n"
                      "}\n"
                      "var task = go consume()\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT native-class await should generate");

    const char *consume = strstr(code, "test_consume_");
    const char *resume =
        consume ? strstr(consume, "_aot_resume(void *raw_frame, const XrAotContext *ctx) {") : NULL;
    const char *trace = resume ? strstr(resume, "test_consume_") : NULL;
    trace = trace ? strstr(trace, "_aot_trace(void *frame") : NULL;
    assert(resume != NULL && trace != NULL && "consume resume function should be emitted");

    const char *await_call = strstr(resume, "xr_aot_await_task(ctx,");
    assert(await_call != NULL && await_call < trace && "native-class await call should be emitted");
    const char *slot = strstr(await_call, "xr_slot_xvalue_ptr(&_xr_await_boundary_v");
    assert(slot != NULL && slot < trace && "native-class await must use a tagged boundary slot");
    unsigned slot_id = 0;
    assert(sscanf(slot, "xr_slot_xvalue_ptr(&_xr_await_boundary_v%u)", &slot_id) == 1 &&
           "native-class await slot id should be readable");
    char boundary_declaration[96];
    char typed_declaration[64];
    char bridge[96];
    snprintf(boundary_declaration, sizeof(boundary_declaration),
             "XrValue _xr_await_boundary_v%u = XR_NULL_VAL;", slot_id);
    snprintf(typed_declaration, sizeof(typed_declaration), "void * v%u = 0;", slot_id);
    snprintf(bridge, sizeof(bridge), "xr_aot_bridge_value_to_xrt(_xr_await_boundary_v%u)", slot_id);
    assert(contains_between(resume, await_call, boundary_declaration) &&
           "await boundary slot must have dedicated XrValue storage");
    assert(contains_between(resume, await_call, typed_declaration) &&
           "native-class await result must retain its typed pointer local");
    assert(contains_between(await_call, trace, bridge) &&
           "native-class await result should bridge from the boundary temporary exactly once");
    char tagged_clear[64];
    snprintf(tagged_clear, sizeof(tagged_clear), "v%u = XR_NULL_VAL;", slot_id);
    assert(!contains_between(await_call, trace, tagged_clear) &&
           "the native pointer local must never receive a tagged null");
    char physical_release[96];
    snprintf(physical_release, sizeof(physical_release), "xrt_release(xrt_box_obj(v%u))", slot_id);
    assert(contains_between(await_call, trace, physical_release) &&
           "the bridged native-class owner must be released through its physical pointer rep");
    char boundary_release[128];
    snprintf(boundary_release, sizeof(boundary_release), "xrt_release(_xr_await_boundary_v%u)",
             slot_id);
    assert(!contains_between(await_call, trace, boundary_release) &&
           "the tagged boundary alias must not release the transferred owner a second time");
    /* Match the promotion by its shape, not by the instance local's name: the
     * portable lowering binds the instance to a `_portable_inst_N` pointer
     * where the older statement-expression form used `_inst`. */
    const char *promotion = strstr(code, "xrt_value_set_storage(xrt_box_obj(");
    char transfer_storage[32];
    snprintf(transfer_storage, sizeof(transfer_storage), ", %u)",
             (unsigned) XR_OBJ_STORAGE_TRANSFER);
    assert(promotion && contains_between(promotion, promotion + 96, transfer_storage) &&
           "fresh native-class Task results must be allocated into TRANSFER storage by plan");
    assert(contains(code, "{ XrObjHeader hdr; int64_t f0; }") &&
           "native-class values must use the canonical header-at-value-pointer object ABI");

    printf("  Generated native-class await boundary slot %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_scalar_await_uses_tagged_slot) {
    const char *src = "fn worker() -> i64 {\n"
                      "    Coro.yield()\n"
                      "    return 41\n"
                      "}\n"
                      "fn main_plus() -> i64 {\n"
                      "    var task = go worker()\n"
                      "    var v = await task\n"
                      "    return v + 1\n"
                      "}\n"
                      "var task = go main_plus()\n"
                      "var result = await task\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT scalar await should generate");

    const char *frame = strstr(code, "typedef struct test_main_plus_");
    assert(frame != NULL && "main_plus coroutine frame should be emitted");
    const char *frame_end = strstr(frame, "} test_main_plus_");
    assert(frame_end != NULL && "main_plus coroutine frame should close");
    assert(count_between(frame, frame_end, "int64_t v") >= 1 &&
           "await Task<i64> should reserve a native scalar frame slot");

    const char *main_plus = strstr(code, "test_main_plus_");
    const char *resume =
        main_plus ? strstr(main_plus, "_aot_resume(void *raw_frame, const XrAotContext *ctx) {")
                  : NULL;
    const char *trace = resume ? strstr(resume, "test_main_plus_") : NULL;
    trace = trace ? strstr(trace, "_aot_trace(void *frame") : NULL;
    assert(resume != NULL && trace != NULL && "main_plus resume function should be emitted");
    assert(count_between(resume, trace, "xr_aot_await_task(ctx,") == 1 &&
           "initial scalar await should use the slot bridge");
    assert(count_between(resume, trace, "xr_aot_await_task_resume(ctx,") == 1 &&
           "resumed scalar await should use the slot bridge");
    assert(count_between(resume, trace, "xr_slot_aot_frame_offset") >= 2 &&
           "scalar await should pass native AOT frame slots on start and resume");
    assert(count_between(resume, trace, "xr_slot_xvalue_ptr(&") == 0 &&
           "scalar await should not pass tagged result slots");

    printf("  Generated scalar await slot %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_await_array_task_index_borrows_checked_slot) {
    const char *src = "fn worker() -> i64 {\n"
                      "    Coro.yield()\n"
                      "    return 7\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var tasks: Array<Task<i64>> = []\n"
                      "    tasks.push(go worker())\n"
                      "    var result = await tasks[0]\n"
                      "    return result + len(tasks)\n"
                      "}\n"
                      "print(run())\n";

    XiPipelineConfig cfg = xi_pipeline_aot_config();
    XiFunc *ir = compile_to_ir_with_config(src, cfg);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT await from Array<Task<i64>> should generate");
    assert(contains(code, "xr_aot_await_task(ctx,") &&
           "checked Array<Task<i64>> slot await should use a normal await while the task array "
           "stays live");
    assert(contains(code, "xr_aot_await_task_resume(ctx,") &&
           "resumed Array<Task<i64>> slot await should use the normal await resume path");
    assert(!contains(code, "xr_aot_await_deferred_task_from_array(ctx,") &&
           "task-array deferred submit requires the producer/await/clear batch shape");
    assert(contains(code, "XR_LIKELY(_idx >= 0 && _idx < _a->length) ? "
                          "((XrValue*)_a->data)[_idx]") &&
           "checked Array<Task<i64>> slot await should borrow the array element");
    assert(!contains(code, "XR_LIKELY(_idx >= 0 && _idx < _a->length) ? "
                           "xrt_value_to_owned(((XrValue*)_a->data)[_idx])") &&
           "direct await of a checked Array<Task<i64>> slot must not retain every task handle");
    assert(contains(code, "xr_slot_aot_frame_offset") &&
           "scalar await result should still use a native AOT frame slot");

    printf("  Generated checked Array<Task> await borrow path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_one_shot_await_task_array_loop_borrows_checked_slot) {
    const char *src = "fn worker(n: i64) -> i64 {\n"
                      "    Coro.yield()\n"
                      "    return n + 1\n"
                      "}\n"
                      "fn run(n: i64) -> i64 {\n"
                      "    var tasks: Array<Task<i64>> = []\n"
                      "    tasks.reserve(n)\n"
                      "    var i = 0\n"
                      "    while (i < n) {\n"
                      "        tasks.push(go worker(i))\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    var total = 0\n"
                      "    var j = 0\n"
                      "    while (j < len(tasks)) {\n"
                      "        total = total + await tasks[j]\n"
                      "        j = j + 1\n"
                      "    }\n"
                      "    tasks.clear()\n"
                      "    return total\n"
                      "}\n"
                      "print(run(4))\n";

    XiPipelineConfig cfg = xi_pipeline_aot_config();
    XiFunc *ir = compile_to_ir_with_config(src, cfg);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT sequential await over Array<Task<i64>> should generate");
    assert(contains(code, "xr_aot_await_deferred_task_from_array(ctx,") &&
           "sequential Array<Task<i64>> slot await must fuse deferred batch submit with await");
    assert(contains(code, "xr_aot_await_deferred_task_from_array_resume(ctx,") &&
           "sequential Array<Task<i64>> slot await resume must consume the array slot");
    assert(contains(code, ", false, true);") &&
           "counted task array await loop should keep the one-shot await flag");
    assert(contains(code, "((XrValue*)_a->data)[phi") &&
           !contains(code, "((XrValue*)_a->data)[XR_TO_INT(") &&
           "one-shot guarded Array<Task<i64>> loop await should borrow with its native index");
    assert(
        !contains(code, "XR_LIKELY(_idx >= 0 && _idx < _a->length) ? "
                        "xrt_value_to_owned(((XrValue*)_a->data)[_idx])") &&
        "one-shot direct await of checked Array<Task<i64>> slot must not retain every task handle");

    printf("  Generated one-shot Array<Task> await-loop borrow path %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_await_timeout_passes_deadline) {
    const char *src = "fn worker(ch: Channel<i64>) -> i64 {\n"
                      "    match (ch.recv()) {\n"
                      "        Recv.Value(value) -> { return value }\n"
                      "        _ -> { return -1 }\n"
                      "    }\n"
                      "}\n"
                      "const ch = Channel<i64>(0)\n"
                      "var task = go worker(ch)\n"
                      "var result = task.awaitTimeout(25)\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT task awaitTimeout should generate");
    const char *await_call = strstr(code, "xr_aot_task_await_result(ctx,");
    assert(await_call != NULL && "task awaitTimeout must use the AOT TaskResult bridge");
    const char *await_line_end = strchr(await_call, '\n');
    bool await_line_terminated = await_line_end != NULL;
    bool await_uses_infinite_wait =
        await_line_terminated && count_between(await_call, await_line_end, ", -1, false);") > 0;
    assert(await_line_terminated && "timeout await call should be line-terminated");
    assert(!await_uses_infinite_wait && "timeout await must not use the infinite-wait sentinel");
    (void) await_line_terminated;
    (void) await_uses_infinite_wait;
    assert(contains(code, "INT64_C(25)") && "timeout expression should be evaluated");

    printf("  Generated await timeout %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_tagged_null_equality_keeps_null_literal) {
    const char *src = "fn maybe() -> i64? {\n"
                      "    return null\n"
                      "}\n"
                      "\n"
                      "var task = go maybe()\n"
                      "var picked = task\n"
                      "var result = await picked\n"
                      "print(result == null)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT null equality should generate");
    const char *eq = strstr(code, "xrt_eq(");
    assert(eq != NULL && "tagged await result should use tagged equality");
    const char *eq_end = strchr(eq, ')');
    assert(eq_end != NULL && "tagged equality call should be closed");
    assert(count_between(eq, eq_end, "XR_NULL_VAL") == 1 &&
           "tagged null equality must compare against XR_NULL_VAL");
    assert(count_between(eq, eq_end, "XR_FROM_INT") == 0 &&
           "tagged null equality must not turn null into i64 zero");

    printf("  Generated tagged null equality %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_recv_resume_uses_wait_state_slot) {
    const char *src = "const ch = Channel<i64>(0)\n"
                      "var value = ch.recv()\n"
                      "print(value)\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "AOT channel recv should generate");
    TEST_REQUIRE(contains(code, "xr_aot_chan_recv_slot(ctx,"),
                 "initial channel recv must register a backend-neutral slot");
    TEST_REQUIRE(nonzero_state_precedes_call(code, "xr_aot_chan_recv_slot(ctx,"),
                 "channel recv must publish the AOT resume state before runtime blocking");
    TEST_REQUIRE(
        contains(code, "xr_aot_chan_recv_slot_resume(ctx, xr_slot_none(), true);"),
        "channel recv resume must recover the slot from coroutine wait state and store Recv");
    TEST_REQUIRE(!contains(code, "xr_aot_chan_recv_slot_resume(ctx, _chan_recv_slot_"),
                 "channel recv resume must not depend on a local slot variable");
    TEST_REQUIRE(
        contains(code, "xr_aot_bridge_owned_value_to_xrt(ctx,"),
        "channel recv must consume the provider-owned Recv after representation conversion");

    printf("  Generated channel recv wait-state slot %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_discarded_recv_does_not_materialize_result) {
    const char *src = "fn wait_then_return(ch: Channel<i64>) -> i64 {\n"
                      "    ch.recv()\n"
                      "    return 7\n"
                      "}\n"
                      "const ch: Channel<i64> = Channel(0)\n"
                      "var task = go wait_then_return(ch)\n"
                      "ch.send(1)\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "discarded AOT channel recv should generate");
    assert(contains(code, "xr_aot_chan_recv_slot(ctx,") &&
           "discarded channel recv must still use the blocking recv helper");
    assert(contains(code, ", -1, false);") &&
           "discarded channel recv must tell the runtime not to materialize Recv");
    assert(contains(code, "xr_aot_chan_recv_slot_resume(ctx, xr_slot_none(), false);") &&
           "discarded channel recv resume must discard the recovered result");
    assert(!contains(code, "xr_aot_chan_recv_slot_resume(ctx, xr_slot_none(), true);") &&
           "discarded channel recv must not resume through the result-producing path");

    printf("  Generated discarded channel recv %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_fused_scalar_channel_recv_uses_typed_pair_bridge) {
    const char *src = "fn recv_or(ch: Channel<i64>, fallback: i64) -> i64 {\n"
                      "    return match (ch.recv()) {\n"
                      "        Recv.Value(n) -> n\n"
                      "        _ -> fallback\n"
                      "    }\n"
                      "}\n"
                      "const ch = Channel<i64>(1)\n"
                      "ch.send(9)\n"
                      "var task = go recv_or(ch, -1)\n"
                      "var result = await task\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT fused scalar channel recv should generate");
    assert(contains(code, "xr_aot_chan_recv_pair_i64(ctx,") &&
           "fused scalar channel recv must use the typed pair bridge");
    assert(!contains(code, "xr_aot_chan_recv_pair(ctx,") &&
           "fused scalar channel recv must not call the generic pair bridge");
    assert(contains(code, "S1:;\n    f->state = 0;\n    _chan_recv_slot_") &&
           "fused channel recv resume must rebuild the frame slot after jumping to the label");

    printf("  Generated fused scalar channel recv %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_scalar_channel_recv_uses_tagged_slot) {
    const char *src = "fn recv_plus(ch: Channel<i64>) -> i64 {\n"
                      "    var v = ch.recv()\n"
                      "    return match (v) {\n"
                      "        Recv.Value(n) -> n + 1\n"
                      "        _ -> -1\n"
                      "    }\n"
                      "}\n"
                      "const ch = Channel<i64>(1)\n"
                      "ch.send(9)\n"
                      "var task = go recv_plus(ch)\n"
                      "var result = await task\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT scalar channel recv should generate");

    const char *frame = strstr(code, "typedef struct test_recv_plus_");
    assert(frame != NULL && "recv_plus coroutine frame should be emitted");
    const char *frame_end = strstr(frame, "} test_recv_plus_");
    assert(frame_end != NULL && "recv_plus coroutine frame should close");
    assert(count_between(frame, frame_end, "XrValue v") >= 1 &&
           "channel recv should reserve a tagged frame slot for Recv<T>");

    const char *slot_ref = strstr(code, "xr_slot_aot_frame_offset");
    assert(slot_ref != NULL && "channel recv must create a backend-neutral slot ref");
    assert(strstr(slot_ref, "), 3);\n    f->state = ") != NULL &&
           strstr(slot_ref, "XrAotResult _chan_recv_") != NULL &&
           "channel recv slot should use XR_REP_TAGGED before publishing resume state");

    const char *recv_call = strstr(code, "xr_aot_chan_recv_slot(ctx,");
    assert(recv_call != NULL && "channel recv must use the AOT recv slot bridge");
    assert(nonzero_state_precedes_call(code, "xr_aot_chan_recv_slot(ctx,") &&
           "scalar channel recv must publish the AOT resume state before runtime blocking");

    const char *recv_plus = strstr(code, "test_recv_plus_");
    const char *resume =
        recv_plus ? strstr(recv_plus, "_aot_resume(void *raw_frame, const XrAotContext *ctx) {")
                  : NULL;
    const char *trace = resume ? strstr(resume, "test_recv_plus_") : NULL;
    trace = trace ? strstr(trace, "_aot_trace(void *frame") : NULL;
    assert(resume != NULL && trace != NULL && "recv_plus resume function should be emitted");
    printf("  Generated scalar channel recv slot %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_channel_recv_null_check_keeps_tagged_slot) {
    const char *src = "fn read_or_stop(ch: Channel<i64>) {\n"
                      "    var v = ch.recv()\n"
                      "    match (v) {\n"
                      "        Recv.Value(n) -> { print(n) }\n"
                      "        Recv.Closed -> { print(\"closed\") }\n"
                      "        _ -> { print(\"other\") }\n"
                      "    }\n"
                      "}\n"
                      "const ch = Channel<i64>(1)\n"
                      "ch.send(0)\n"
                      "var task = go read_or_stop(ch)\n"
                      "await task\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT Recv channel recv should generate");

    const char *frame = strstr(code, "typedef struct test_read_or_stop_");
    assert(frame != NULL && "read_or_stop coroutine frame should be emitted");
    const char *frame_end = strstr(frame, "} test_read_or_stop_");
    assert(frame_end != NULL && "read_or_stop coroutine frame should close");
    assert(count_between(frame, frame_end, "XrValue v") >= 1 &&
           "Recv result should keep a tagged frame slot");

    const char *slot_ref = strstr(code, "xr_slot_aot_frame_offset");
    assert(slot_ref != NULL && "channel recv must create a backend-neutral slot ref");
    assert(strstr(slot_ref, "), 3);\n    f->state = ") != NULL &&
           strstr(slot_ref, "XrAotResult _chan_recv_") != NULL &&
           "channel recv result slot should use XR_REP_TAGGED");

    printf("  Generated Recv channel recv slot %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_descriptor_scalar_channel_try_recv_returns_recv_enum) {
    const char *src = "const ch = Channel<i64>(1)\n"
                      "var recv = ch.tryRecv()\n"
                      "print(recv)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT channel tryRecv should generate");
    assert(contains(code, "xr_aot_chan_try_recv_sync(") &&
           "descriptor-root tryRecv must use the synchronous Recv<T> enum bridge");
    assert(contains(code, "xr_aot_bridge_value_to_xrt(xr_aot_chan_try_recv_sync(") &&
           "descriptor-root tryRecv must bridge the full Recv<T> enum into AOT layout");
    assert(!contains(code, "xr_aot_poll_yield_kind(ctx)") &&
           "nonblocking tryRecv must not own a suspend/poll state");
    assert(!contains(code, "xr_aot_chan_try_recv_slot(ctx,") &&
           "tryRecv must not use the old typed slot bridge");
    assert(!contains(code, "_chan_try_ok_") && "tryRecv must not expose an ok bit");

    printf("  Generated channel tryRecv Recv enum %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_descriptor_select_try_recv_uses_ready_bit) {
    const char *src = "const ch = Channel<i64>(0)\n"
                      "select {\n"
                      "    value from ch -> { print(value) }\n"
                      "    _ -> { print(0) }\n"
                      "}\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT select tryRecv should generate");
    assert(contains(code, "xr_aot_chan_try_recv_sync(") &&
           "descriptor-root select probe must use the synchronous AOT Recv enum bridge");
    assert(contains(code, "xr_aot_bridge_value_to_xrt(xr_aot_recv_payload(_chan_try_") &&
           "descriptor-root select probe must bridge the Recv payload");
    assert(contains(code, " = xr_aot_recv_is_value(") &&
           "select readiness must use the positive Recv.Value status projection");

    printf("  Generated select tryRecv Recv.Value readiness %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_sleep_publishes_state_before_block) {
    const char *src = "import time\n"
                      "time.sleep(5)\n"
                      "print(\"awake\")\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT sleep should generate");
    assert(contains(code, "xr_aot_sleep(ctx,") && "sleep must use the AOT sleep bridge");
    assert(nonzero_state_precedes_call(code, "xr_aot_sleep(ctx,") &&
           "sleep must publish the AOT resume state before runtime blocking");

    printf("  Generated sleep state publication %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_test_yield_calls_publish_resume_states) {
    char descriptor_path[1024];
    int descriptor_len = snprintf(descriptor_path, sizeof(descriptor_path),
                                  "%s/stdlib/test_yield/test_yield.xrd", XRAY_TEST_SOURCE_DIR);
    TEST_REQUIRE(descriptor_len > 0 && (size_t) descriptor_len < sizeof(descriptor_path),
                 "test_yield descriptor path fits");
    TEST_REQUIRE(xa_xrd_load_file(descriptor_path) != NULL,
                 "test_yield native signatures loaded from XRD");

    const char *src = "import test_yield\n"
                      "import { add } from test_yield\n"
                      "Coro.yield()\n"
                      "print(test_yield.simple())\n"
                      "print(add(19, 23))\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "IR compilation succeeded");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation produced output");
    TEST_REQUIRE(!had_error, "contracted AOT test_yield calls generated successfully");
    TEST_REQUIRE(contains(code, "switch (f->state)") && contains(code, "case 1: goto S1;") &&
                     contains(code, "case 2: goto S2;") && contains(code, "case 3: goto S3;"),
                 "test_yield calls participate in the shared coroutine plan");
    TEST_REQUIRE(contains(code, "xr_aot_test_yield_simple()") &&
                     contains(code, "xr_aot_test_yield_add(INT64_C(19), INT64_C(23))"),
                 "module and selected-import calls use the AOT test provider");
    TEST_REQUIRE(contains(code, "int64_t test_yield_value_"),
                 "add arguments and result are captured before yielding");
    TEST_REQUIRE(contains(code, "f->state = 2;\n    return xr_aot_yielded();") &&
                     contains(code, "f->state = 3;\n    return xr_aot_yielded();"),
                 "each contracted yieldable call publishes one cooperative resume state");

    printf("  Generated test_yield provider states %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_rejects_mutated_frozen_coroutine_plan) {
    const char *src = "Coro.yield()\n"
                      "print(7)\n";

    XiFunc *ir = compile_to_ir(src);
    TEST_REQUIRE(ir != NULL, "coroutine mutation fixture compiled");
    TEST_REQUIRE(test_prepare_backend_ir(ir), "coroutine mutation fixture reached Backend");
    TEST_REQUIRE(ir->coro_plan != NULL && ir->coro_plan->nstates == 1 &&
                     xi_coro_plan_is_current(ir, ir->coro_plan),
                 "Backend fixture carries one current frozen state");

    XiModule *mod = ir->module;
    bool own_mod = false;
    if (!mod) {
        mod = xi_module_new("test.xr", "test", ir);
        TEST_REQUIRE(mod != NULL, "coroutine mutation fixture module allocated");
        own_mod = true;
    }
    XiModule *modules[] = {mod};
    TestAotPlan plan;
    test_aot_plan_prepare(&plan, modules, 1, 0);
    TEST_REQUIRE(xi_coro_plan_is_current(ir, ir->coro_plan),
                 "AOT evidence publication rebased the frozen coroutine plan");

    ir->coro_plan->fingerprint ^= UINT64_C(1);
    XiCgenCtx *ctx = xi_cgen_ctx_new();
    TEST_REQUIRE(ctx != NULL, "coroutine mutation fixture CGen context allocated");
    xi_cgen_ctx_set_aot_bundle(ctx, &plan.bundle);
    char *code = NULL;
    size_t code_size = 0;
    FILE *mem = xr_open_memstream(&code, &code_size);
    TEST_REQUIRE(mem != NULL, "coroutine mutation fixture output stream allocated");
    xi_cgen_program(ctx, mem, mod);
    TEST_REQUIRE(xr_close_memstream(mem, &code, &code_size) == 0,
                 "coroutine mutation fixture output stream closed");
    bool had_error = xi_cgen_has_error(ctx);
    TEST_REQUIRE(code != NULL && had_error,
                 "CGen rejects a frozen coroutine plan with a mutated fingerprint");

    printf("  Rejected mutated frozen coroutine plan before state emission\n");
    xr_free(code);
    xi_cgen_ctx_free(ctx);
    test_aot_plan_free(&plan);
    if (own_mod) {
        mod->init = NULL;
        xi_module_free(mod);
    }
    xi_func_free(ir);
}

TEST(cgen_runtime_needed_main_uses_aot_runtime) {
    const char *src = "import time\n"
                      "time.sleep(5)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT sleep should generate");
    assert(contains(code, "XrAotRuntimeConfig runtime_cfg;") &&
           "runtime-needed generated main must create an AOT runtime config");
    assert(contains(code, "XrAotRuntime *rt = xr_aot_runtime_new(&runtime_cfg);") &&
           "runtime-needed generated main must create XrAotRuntime directly");
    assert(contains(code, "runtime_cfg.file = \"test\";") &&
           "runtime-needed generated main must pass entry source path to AOT runtime");
    assert(contains(code, "xrt_global_ctx.runtime = rt;") &&
           "generated sync helpers must see the AOT runtime owner");
    assert(contains(code, "xr_aot_run_main(rt,") &&
           "generated coroutine main must call the final runtime API");
    assert(contains(code, "xr_aot_runtime_delete(rt);") &&
           "generated main must tear down XrAotRuntime directly");
    assert(!contains(code, "XrVMConfig") && "generated main must not construct VM isolate params");
    assert(!contains(code, "xray_vm_new_full(") &&
           "generated main must not construct a VM isolate");
    assert(!contains(code, "xr_multicore_init(") &&
           "generated main must not initialize scheduler through isolate");
    assert(!contains(code, "xr_aot_run_main_vm_bridge(") &&
           "generated main must not use the VM bridge entry");
    assert(!contains(code, "xray_vm_delete(") && "generated main must not tear down a VM isolate");

    printf("  Generated AOT runtime main %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_select_publishes_state_before_block) {
    const char *src = "const ch = Channel<i64>(0)\n"
                      "select {\n"
                      "    value from ch -> { print(value) }\n"
                      "}\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT select should generate");
    assert(contains(code, "xr_aot_select_block(ctx,") && "select must use the AOT bridge");
    assert(nonzero_state_precedes_call(code, "xr_aot_select_block(ctx,") &&
           "select must publish the AOT resume state before runtime blocking");

    printf("  Generated select state publication %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_channel_timeout_publishes_state_before_block) {
    const char *src = "const ch = Channel<i64>(0)\n"
                      "var sent = ch.sendTimeout(7, 10)\n"
                      "var recv = ch.recvTimeout(10)\n"
                      "print(sent)\n"
                      "print(recv)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT channel timeout ops should generate");
    assert(contains(code, "xr_aot_chan_send_timeout") &&
           "sendTimeout must use the AOT timeout bridge");
    assert(contains(code, "xr_aot_chan_recv_slot(ctx,") &&
           "recvTimeout must use the AOT recv slot bridge");
    const char *send_timeout = strstr(code, "XrAotResult _chan_send_timeout_");
    const char *recv_timeout =
        send_timeout ? strstr(send_timeout, "XrAotResult _chan_recv_timeout_") : NULL;
    assert(send_timeout != NULL && recv_timeout != NULL && "timeout send/recv blocks should exist");
    assert(count_between(send_timeout, recv_timeout, "xr_aot_bridge_value_to_xrt(") == 0 &&
           "sendTimeout result is a native no-payload SendResult enum and must not be bridged");
    assert(nonzero_state_precedes_call(code, "xr_aot_chan_send_timeout") &&
           "sendTimeout must publish the AOT resume state before runtime blocking");
    assert(nonzero_state_precedes_call(code, "xr_aot_chan_recv_slot(ctx,") &&
           "recvTimeout must publish the AOT resume state before runtime blocking");
    TEST_REQUIRE(
        contains(recv_timeout, "xr_aot_bridge_owned_value_to_xrt(ctx,"),
        "recvTimeout must consume the provider-owned Recv after representation conversion");

    printf("  Generated channel timeout state publication %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_recv_slot_is_traced_as_frame_root) {
    const char *src = "const ch = Channel<string>(0)\n"
                      "var value = ch.recv()\n"
                      "print(value)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT tagged channel recv should generate");
    assert(contains(code, "xr_aot_chan_recv_slot(ctx,") &&
           "tagged channel recv must register a frame slot");
    assert(contains(code, "xr_aot_trace_frame_value(visitor, f->v") &&
           "tagged channel recv slot must be scanned while ready or blocked");
    assert(contains(code, ".root_count = 1,") &&
           "tagged channel recv frame should report one traced root slot");

    printf("  Generated traced channel recv slot %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_await_all_uses_aggregate_bridge) {
    const char *src = "fn worker(n: i64) -> i64 {\n"
                      "    Coro.yield()\n"
                      "    return n * n\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var results = await all [go worker(2), go worker(3)]\n"
                      "    return results[0] + results[1]\n"
                      "}\n"
                      "print(run())\n";

    XiPipelineConfig cfg = xi_pipeline_aot_config();
    XiFunc *ir = compile_to_ir_with_config(src, cfg);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT await all should generate");
    assert(contains(code, "xr_aot_await_all_task_values_to_slots(ctx,") &&
           "fresh await all literals with scalar uses must write result slots directly");
    assert(contains(code, "xr_aot_await_all_task_values_to_slots(ctx,") &&
           contains(code, "XR_ELEM_I64") &&
           "await all over Task<i64> must pass the typed result element layout");
    assert(contains(code, "XR_ELEM_I64, true") &&
           "fresh await all task literals must use aggregate one-shot");
    assert(nonzero_state_precedes_call(code, "xr_aot_await_all_task_values_to_slots(ctx,") &&
           "await all must publish the AOT resume state before runtime blocking");
    assert(contains(code, "xr_aot_await_all_task_values_to_slots_resume(ctx,") &&
           "await all resume must use the aggregate AOT bridge");
    const char *await_call = strstr(code, "xr_aot_await_all_task_values_to_slots(ctx,");
    assert(await_call != NULL);
    assert(count_between(code, await_call, "xrt_value_clone_for_coro(") == 0 &&
           "inline await all literals must not clone a task array before suspension");
    assert(!contains(code, "xrt_array_new_typed(2, XR_ELEM_ANY,") &&
           "inline await all literals must not allocate an input task array");
    assert(!contains(code, "xr_aot_bridge_value_to_xrt(") &&
           "scalarized await all result arrays should not be materialized");
    assert(count_between(await_call, code + strlen(code), "xrt_value_clone_for_coro(") == 0 &&
           "scalar await all result arrays must not be cloned after bridge conversion");

    printf("  Generated await all aggregate bridge %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_await_all_named_task_array_skips_task_list_clone) {
    const char *src = "fn worker(n: i64) -> i64 {\n"
                      "    Coro.yield()\n"
                      "    return n * n\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    var t1 = go worker(2)\n"
                      "    var t2 = go worker(3)\n"
                      "    var tasks: Array<Task<i64>> = []\n"
                      "    tasks.push(t1)\n"
                      "    tasks.push(t2)\n"
                      "    var results = await all tasks\n"
                      "    return results[0] + results[1]\n"
                      "}\n"
                      "print(run())\n";

    XiPipelineConfig cfg = xi_pipeline_aot_config();
    XiFunc *ir = compile_to_ir_with_config(src, cfg);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT await all over named task array should generate");
    const char *await_call = strstr(code, "xr_aot_await_all_task_values_to_slots(ctx,");
    assert(await_call != NULL);
    assert(!contains(code, "xr_aot_await_all_tasks(ctx,") &&
           "fixed pushed named task arrays should scalarize to task value slots");
    assert(contains(code, "XR_ELEM_I64") &&
           "await all over named Array<Task<i64>> must keep typed result layout");
    assert(contains(code, "XR_ELEM_I64, true") &&
           "named task arrays whose pushed tasks have no other users may use aggregate one-shot");
    assert(contains(code, "xr_aot_spawn_deferred(ctx, &") &&
           "await-all-only task producers should use deferred batch submission");
    assert(!contains(code, "xr_aot_spawn_child") &&
           "await-all-only task producers should not yield after each go");
    assert(contains(code, ", 0, false, true, false, \"worker\"") &&
           "unique pushed task producers should be spawned as one-shot await tasks");
    assert(count_between(code, await_call, "xrt_value_clone_for_coro(") == 0 &&
           "named task arrays are frame roots and should not be deep-cloned before await all");
    assert(count_between(await_call, code + strlen(code), "xrt_value_clone_for_coro(") == 0 &&
           "scalar await all result arrays must not be cloned after bridge conversion");
    assert(!contains(code, "xrt_array_push(") &&
           "fixed pushed task arrays should not materialize input array pushes");
    assert(!contains(code, "xr_aot_bridge_value_to_xrt(") &&
           "scalarized await all should not bridge a result array back to xrt");
    assert(count_between(await_call, code + strlen(code), "xrt_index_get(") == 0 &&
           "scalarized await all result indexes should not use dynamic indexing");

    printf("  Generated await all named task array no-clone path %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_await_all_reused_push_task_array_uses_one_shot) {
    const char *src = "fn worker(n: i64) -> i64 {\n"
                      "    Coro.yield()\n"
                      "    return n * n\n"
                      "}\n"
                      "fn run(n: i64) -> i64 {\n"
                      "    var tasks: Array<Task<i64>> = []\n"
                      "    tasks.reserve(n)\n"
                      "    tasks.clear()\n"
                      "    var i = 0\n"
                      "    while (i < n) {\n"
                      "        tasks.push(go worker(i))\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    var results = await all tasks\n"
                      "    return results[0]\n"
                      "}\n"
                      "print(run(4))\n";

    XiPipelineConfig cfg = xi_pipeline_aot_config();
    XiFunc *ir = compile_to_ir_with_config(src, cfg);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT await all over reused pushed task array should generate");
    assert(contains(code, "xr_aot_await_all_tasks_wait(ctx,") &&
           "reused pushed task arrays should wait separately from typed result collection");
    assert(contains(code, "xr_aot_await_all_tasks_wait_resume(ctx,") &&
           "reused pushed task arrays should resume through the wait-only path");
    assert(contains(code, "xrt_array_new_typed(_await_count_") &&
           "reused pushed task arrays should allocate the AOT typed result array directly");
    assert(contains(code, "xr_aot_await_all_tasks_collect_into_array(ctx,") &&
           "reused pushed task arrays should fill the AOT typed result array in place");
    assert(!contains(code, "xr_aot_await_all_tasks(ctx,") &&
           "dynamic scalar await-all should not materialize a VM result array first");
    assert(!contains(code, "xr_aot_bridge_value_to_xrt(") &&
           "dynamic scalar await-all should not bridge a result array back to xrt");
    assert(contains(code, "XR_ELEM_I64, true") &&
           "reused pushed task arrays with unique go producers should use aggregate one-shot");
    assert(contains(code, "xr_aot_spawn_deferred(ctx, &") &&
           "loop-pushed await-all tasks should use deferred batch submission");
    assert(!contains(code, "xr_aot_spawn_child") &&
           "loop-pushed await-all tasks should not yield after each go");
    assert(contains(code, ", 0, false, true, false, \"worker\"") &&
           "loop-pushed go producers should be spawned as one-shot await tasks");
    assert(!contains(code, "xr_aot_poll_yield_kind(ctx)") &&
           "deferred spawn registration loops should not poll before the aggregate await submit");
    assert(count_between(code, code + strlen(code), "xrt_value_clone_for_coro(") == 0 &&
           "task array roots and scalar await-all results should not be deep-cloned");

    printf("  Generated reused pushed task array one-shot path %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_await_all_into_reuses_result_array) {
    const char *src = "fn worker(n: i64) -> i64 {\n"
                      "    Coro.yield()\n"
                      "    return n * n\n"
                      "}\n"
                      "fn run(n: i64) -> i64 {\n"
                      "    var tasks: Array<Task<i64>> = []\n"
                      "    var results: Array<i64> = []\n"
                      "    tasks.reserve(n)\n"
                      "    results.reserve(n)\n"
                      "    tasks.clear()\n"
                      "    var i = 0\n"
                      "    while (i < n) {\n"
                      "        tasks.push(go worker(i))\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    await all tasks into results\n"
                      "    var sum = 0\n"
                      "    i = 0\n"
                      "    while (i < len(results)) {\n"
                      "        sum = sum + results[i]\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    return sum\n"
                      "}\n"
                      "print(run(4))\n";

    XiPipelineConfig cfg = xi_pipeline_aot_config();
    XiFunc *ir = compile_to_ir_with_config(src, cfg);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT await all into should generate");
    assert(contains(code, "xr_aot_await_all_tasks_into_array(ctx,") &&
           "await all into should fuse wait and typed result collection");
    const char *await_into = strstr(code, "xr_aot_await_all_tasks_into_array(ctx,");
    TEST_REQUIRE(await_into != NULL, "await all into fused call missing");
    size_t await_prefix_len = (size_t) (await_into - code);
    const char *await_retain_window = await_prefix_len > 384 ? await_into - 384 : code;
    TEST_REQUIRE(count_between(await_retain_window, await_into, "xrt_retain(") == 0,
                 "await all into must borrow task/result arrays without pre-await retains");
    assert(contains(code, "xr_aot_await_all_tasks_into_array_resume(ctx,") &&
           "await all into should resume through the fused wait/result bridge");
    assert(!contains(code, "xr_aot_await_all_tasks_wait(ctx,") &&
           "await all into should not emit a separate wait helper");
    assert(!contains(code, "xr_aot_await_all_tasks_collect_into_array(ctx,") &&
           "await all into should not emit a second post-await collect helper");
    assert(contains(code, "XR_ELEM_I64, true") &&
           "await all into Array<i64> must keep typed result layout and one-shot tasks");
    assert(!contains(code, "XR_ELEM_I64, false") &&
           "await all into Array<i64> must not lose aggregate one-shot marking");
    assert(contains(code, "xr_aot_spawn_deferred(ctx, &") &&
           "await all into task producers should use deferred batch submission");
    assert(!contains(code, "xr_aot_spawn(ctx, &") &&
           "await all into task producers should not yield after each go");
    assert(contains(code, ", 0, false, true, false, \"worker\"") &&
           "await all into task producers should be spawned as one-shot await tasks");
    assert(!contains(code, "xrt_array_reserve_value(") &&
           "await all into setup should reserve typed arrays through the raw pointer helper");
    assert(!contains(code, "xrt_array_new_typed(_await_count_") &&
           "await all into must not allocate a fresh typed result array");
    assert(!contains(code, "xr_aot_await_all_tasks(ctx,") &&
           "await all into should not use the materializing aggregate await helper");
    assert(!contains(code, "xr_aot_bridge_value_to_xrt(") &&
           "await all into should not bridge a materialized result array back to xrt");

    printf("  Generated await all into result array path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_top_level_await_all_into_keeps_result_array_alive) {
    const char *src = "fn worker(n: i64) -> i64 {\n"
                      "    Coro.yield()\n"
                      "    return n * n\n"
                      "}\n"
                      "var tasks: Array<Task<i64>> = []\n"
                      "var results: Array<i64> = []\n"
                      "tasks.reserve(4)\n"
                      "results.reserve(4)\n"
                      "var i = 0\n"
                      "while (i < 4) {\n"
                      "    tasks.push(go worker(i))\n"
                      "    i = i + 1\n"
                      "}\n"
                      "await all tasks into results\n"
                      "var sum = 0\n"
                      "i = 0\n"
                      "while (i < len(results)) {\n"
                      "    sum = sum + results[i]\n"
                      "    i = i + 1\n"
                      "}\n"
                      "print(sum)\n";

    XiPipelineConfig cfg = xi_pipeline_aot_config();
    XiFunc *ir = compile_to_ir_with_config(src, cfg);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "top-level AOT await all into should generate");
    assert(contains(code, "xr_aot_spawn_deferred(ctx, &") &&
           "top-level loop-pushed await-all tasks should use deferred batch submission");
    assert(!contains(code, "xr_aot_spawn_child") &&
           "top-level deferred batch producers should not yield after each go");
    assert(contains(code, "xr_aot_await_all_tasks_into_array(ctx,") &&
           "top-level await all into should fuse wait and typed result collection");
    assert(contains(code, "xr_aot_await_all_tasks_into_array_resume(ctx,") &&
           "top-level await all into should resume through the fused bridge");
    assert(!contains(code, "xr_aot_await_all_tasks_collect_into_array(ctx,") &&
           "top-level await all into should not emit a second post-await collect helper");
    assert(contains(code, "XR_ELEM_I64, true") &&
           "top-level await all into Array<i64> should keep typed one-shot collection");
    const char *await_into = strstr(code, "xr_aot_await_all_tasks_into_array(ctx,");
    unsigned task_slot = 0;
    unsigned result_slot = 0;
    assert(await_into != NULL &&
           sscanf(await_into, "xr_aot_await_all_tasks_into_array(ctx, v%u, v%u,", &task_slot,
                  &result_slot) == 2 &&
           task_slot != result_slot && "await-all frame operand slots should be readable");
    const char *frame = strstr(code, "typedef struct test___main__");
    const char *frame_end = frame ? strstr(frame, "} test___main__") : NULL;
    assert(frame != NULL && frame_end != NULL && "top-level coroutine frame should be bounded");
    char result_field[48];
    char result_macro[64];
    char transient_local[64];
    char duplicate_trace[80];
    snprintf(result_field, sizeof(result_field), "XrValue v%u;", result_slot);
    snprintf(result_macro, sizeof(result_macro), "#define v%u (f->v%u)", result_slot, result_slot);
    snprintf(transient_local, sizeof(transient_local), "XrValue v%u = XR_NULL_VAL;", result_slot);
    snprintf(duplicate_trace, sizeof(duplicate_trace), "xr_aot_trace_frame_value(visitor, f->v%u);",
             result_slot);
    assert(contains_between(frame, frame_end, result_field) && contains(code, result_macro) &&
           "the await-all result alias must survive suspension in one stable frame slot");
    assert(!contains(code, transient_local) &&
           "the await-all result alias must not be reconstructed as a transient local");
    assert(!contains(code, duplicate_trace) &&
           "a borrowed alias of the globally rooted result array must not become a second root");

    printf("  Generated top-level await all into frame-root path %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_result_group_fire_and_forget_go_uses_deferred_batch) {
    const char *src = "import { ResultGroup } from sync\n"
                      "fn worker(group: ResultGroup, n: i64) {\n"
                      "    group.add(n)\n"
                      "    if (n == 3) {\n"
                      "        group.close()\n"
                      "    }\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    const group: ResultGroup = ResultGroup(4)\n"
                      "    var i = 0\n"
                      "    while (i < 4) {\n"
                      "        go worker(group, i)\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    return group.recv() ?? -1\n"
                      "}\n"
                      "print(run())\n";

    XiPipelineConfig cfg = xi_pipeline_aot_config();
    XiFunc *ir = compile_to_ir_with_module_graph_config(src, cfg);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT ResultGroup fire-and-forget should generate");
    /* `recv()` on a ResultGroup<int> takes the typed optional path: the payload
     * and its presence flag are separate unboxed frame slots, so nothing is
     * tagged across the suspend. */
    const char *recv_call = strstr(code, "xr_aot_result_group_recv_i64_optional(ctx,");
    assert(recv_call != NULL && "ResultGroup<i64>.recv should use the typed optional helper");
    assert(!contains(code, "xr_aot_result_group_recv_value(") &&
           "a typed ResultGroup recv must not fall back to the tagged helper");
    assert(contains(code, "xr_aot_spawn_deferred(ctx, &") &&
           "ResultGroup fire-and-forget producers should use deferred batch submission");
    assert(!contains(code, "xr_aot_spawn_child") &&
           "deferred ResultGroup producers should not yield after each go");
    assert(contains_between(code, recv_call, "xr_aot_submit_deferred_spawns(ctx);") &&
           "ResultGroup recv should submit deferred fire-and-forget producers first");
    assert(contains(code, ", 0, true, false, false, \"worker\"") &&
           "fire-and-forget ResultGroup producers should stay task-less");
    assert(!contains(code, "xr_aot_poll_yield_kind(ctx)") &&
           "deferred fire-and-forget producer loops should not poll before the blocking recv");
    assert(contains(code, "xr_aot_result_group_add_bool_sync(") &&
           "sync go ResultGroup producer should keep typed add helper dispatch");
    assert(contains(code, "xr_aot_result_group_close_void_sync(") &&
           "sync go ResultGroup producer should keep typed close helper dispatch");

    printf("  Generated ResultGroup fire-and-forget deferred path %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_result_group_reset_uses_native_helper) {
    const char *src = "import { ResultGroup } from sync\n"
                      "fn run(n: i64) -> bool {\n"
                      "    const group: ResultGroup = ResultGroup(n)\n"
                      "    group.add(1)\n"
                      "    group.add(2)\n"
                      "    group.flush()\n"
                      "    var first = group.recv() ?? -1\n"
                      "    var ok = group.reset(n)\n"
                      "    group.close()\n"
                      "    return first == 3 && ok\n"
                      "}\n"
                      "print(run(2))\n";

    XiFunc *ir = compile_to_ir_with_module_graph(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT ResultGroup reset should generate");
    assert(contains(code, "xr_aot_result_group_add_bool(ctx,") &&
           "ResultGroup.add in coroutine code should use the native bool AOT helper");
    assert(contains(code, "xr_aot_result_group_reset_bool(ctx,") &&
           "ResultGroup.reset in coroutine code should use the native bool AOT helper");
    assert(contains(code, "xr_aot_result_group_flush_void(ctx,") &&
           "ResultGroup.flush in coroutine code should use the native void AOT helper");
    assert(contains(code, "xr_aot_result_group_close_void(ctx,") &&
           "ResultGroup.close in coroutine code should use the native void AOT helper");
    assert(!contains(code, "XR_TO_INT(xr_aot_result_group_add") &&
           "ResultGroup.add should not box and immediately unbox a bool result");
    assert(!contains(code, "XR_TO_INT(xr_aot_result_group_reset") &&
           "ResultGroup.reset should not box and immediately unbox a bool result");
    assert(!contains(code, "XrValue _rg_flush_") &&
           "ResultGroup.flush should not materialize a tagged Unit result");
    assert(!contains(code, "XrValue _rg_close_") &&
           "ResultGroup.close should not materialize a tagged Unit result");

    printf("  Generated ResultGroup reset helper path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_result_group_sync_methods_elide_dead_err_checks) {
    const char *src = "import { ResultGroup } from sync\n"
                      "fn useGroup(group: ResultGroup, n: i64) -> i64 {\n"
                      "    if (!group.add(n)) { return -1 }\n"
                      "    group.flush()\n"
                      "    if (!group.reset(1)) { return -2 }\n"
                      "    group.close()\n"
                      "    return 0\n"
                      "}\n"
                      "fn run() -> i64 {\n"
                      "    const group: ResultGroup = ResultGroup(1)\n"
                      "    return useGroup(group, 1)\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir_with_module_graph(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT ResultGroup sync helpers should generate");
    const char *use_group = find_static_function_definition(code, "test_useGroup_");
    assert(use_group != NULL && "useGroup function should be generated");
    const char *use_group_end = next_static_after(use_group);
    assert(use_group_end != NULL &&
           "useGroup native body should be bounded by the next generated function");
    assert(count_between(use_group, use_group_end, "xr_aot_result_group_add_bool_sync(") == 1 &&
           "ResultGroup.add should use the native bool sync AOT helper");
    assert(count_between(use_group, use_group_end, "xr_aot_result_group_reset_bool_sync(") == 1 &&
           "ResultGroup.reset should use the native bool sync AOT helper");
    assert(count_between(use_group, use_group_end, "xr_aot_result_group_add_sync(") == 0 &&
           "ResultGroup.add should not box a bool result before local use");
    assert(count_between(use_group, use_group_end, "xr_aot_result_group_reset_sync(") == 0 &&
           "ResultGroup.reset should not box a bool result before local use");
    assert(count_between(use_group, use_group_end, "xr_aot_result_group_flush_void_sync(") == 1 &&
           "ResultGroup.flush should use the native void sync AOT helper");
    assert(count_between(use_group, use_group_end, "xr_aot_result_group_close_void_sync(") == 1 &&
           "ResultGroup.close should use the native void sync AOT helper");
    assert(count_between(use_group, use_group_end, "xr_aot_result_group_flush_sync(") == 0 &&
           "ResultGroup.flush should not materialize a tagged Unit result");
    assert(count_between(use_group, use_group_end, "xr_aot_result_group_close_sync(") == 0 &&
           "ResultGroup.close should not materialize a tagged Unit result");
    assert(count_between(use_group, use_group_end, "xrt_has_pending_error") == 0 &&
           "ResultGroup sync helpers report status by return value, not the pending-error channel");

    printf("  Generated ResultGroup sync no-error helper path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_await_any_uses_typed_aggregate_bridge) {
    const char *src = "fn delayed(ch: Channel<i64>, n: i64) -> i64 {\n"
                      "    ch.recv()\n"
                      "    return n\n"
                      "}\n"
                      "const ch1 = Channel<i64>(0)\n"
                      "const ch2 = Channel<i64>(1)\n"
                      "var t1 = go delayed(ch1, 1)\n"
                      "var t2 = go delayed(ch2, 2)\n"
                      "ch2.send(9)\n"
                      "var first = await any [t1, t2]\n"
                      "print(first)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT await any should generate");
    assert(contains(code, "xr_aot_await_any_task(ctx,") &&
           "await any must use the aggregate AOT bridge");
    assert(nonzero_state_precedes_call(code, "xr_aot_await_any_task(ctx,") &&
           "await any must publish the AOT resume state before runtime blocking");
    assert(contains(code, "xr_aot_await_any_task_resume(ctx,") &&
           "await any resume must use the aggregate AOT bridge");
    assert(contains(code, "xr_slot_aot_frame_offset") &&
           "scalar await any results should use a typed frame slot");

    printf("  Generated await any aggregate bridge %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_scope_exit_publishes_state_before_block) {
    const char *src = "fn child(ch: Channel<i64>) {\n"
                      "    ch.recv()\n"
                      "}\n"
                      "fn scoped() {\n"
                      "    const ch = Channel<i64>(0)\n"
                      "    scope {\n"
                      "        go child(ch)\n"
                      "    }\n"
                      "}\n"
                      "scoped()\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT scope exit should generate");
    assert(contains(code, "xr_aot_scope_exit(ctx,") && "scope exit must use the AOT bridge");
    assert(nonzero_state_precedes_call(code, "xr_aot_scope_exit(ctx,") &&
           "scope exit must publish the AOT resume state before runtime blocking");

    printf("  Generated scope exit state publication %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_channel_fields_use_aot_helpers) {
    const char *src = "const ch = Channel<i64>(2)\n"
                      "print(len(ch))\n"
                      "print(ch.capacity)\n"
                      "print(ch.isClosed)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT Channel field reads should generate");
    assert(contains(code, "xr_aot_chan_length(") &&
           "len(Channel) must read through the AOT channel helper");
    assert(contains(code, "xr_aot_chan_capacity(") &&
           "Channel.capacity must read through the AOT channel helper");
    assert(contains(code, "xr_aot_chan_is_closed(") &&
           "Channel.isClosed must read through the AOT channel helper");
    assert(!contains(code, "xrt_map_get((xrt_map_t*)") &&
           "AOT Channel fields must not fall back to map property dispatch");

    printf("  Generated AOT channel field helpers %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_sync_go_channel_try_methods_use_aot_helpers) {
    const char *src = "fn recv_value(r: Recv<i64>) -> i64 {\n"
                      "    return match (r) {\n"
                      "        Recv.Value(v) -> v\n"
                      "        _ -> 0\n"
                      "    }\n"
                      "}\n"
                      "fn producer(ch: Channel<i64>) -> i64 {\n"
                      "    ch.trySend(1)\n"
                      "    ch.trySend(2)\n"
                      "    return 0\n"
                      "}\n"
                      "fn consumer(ch: Channel<i64>) -> i64 {\n"
                      "    return recv_value(ch.tryRecv())\n"
                      "}\n"
                      "fn close_and_check(ch: Channel<i64>) -> bool {\n"
                      "    ch.close()\n"
                      "    return ch.isClosed()\n"
                      "}\n"
                      "const ch = Channel<i64>(2)\n"
                      "var p = go producer(ch)\n"
                      "print(await p)\n"
                      "var c = go consumer(ch)\n"
                      "print(await c)\n"
                      "var closed = go close_and_check(ch)\n"
                      "print(await closed)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT sync-go Channel nonblocking methods should generate");
    assert(contains(code, "xr_aot_chan_try_send_sync_i64(") &&
           "sync-go Channel.trySend should use the scalar sync AOT bridge");
    assert(contains(code, "xr_aot_chan_try_recv_sync(") &&
           "sync-go Channel.tryRecv should use the sync AOT bridge");
    assert(contains(code, "xr_aot_chan_close_sync(") &&
           "sync-go Channel.close should use the sync AOT bridge");
    assert(contains(code, "xr_aot_chan_is_closed_sync(") &&
           "sync-go Channel.isClosed should use the sync AOT bridge");
    assert(!contains(code, "xrt_method_0(") && !contains(code, "xrt_method_1(") &&
           "Channel nonblocking methods must not fall back to dynamic method dispatch");

    printf("  Generated sync-go channel method helpers %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_work_queue_resume_rebuilds_slot_and_traces_task) {
    const char *src = "const queue: WorkQueue<i64> = WorkQueue<i64>(1, 4)\n"
                      "fn consumer() -> i64 {\n"
                      "    var item = queue.pop(0)\n"
                      "    return item ?? 0\n"
                      "}\n"
                      "var task = go consumer()\n"
                      "assert(queue.push(7, 0))\n"
                      "print(await task)\n"
                      "queue.close()\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT WorkQueue pop should generate");
    assert(contains(code, "xr_aot_work_queue_push_bool(ctx,") &&
           "coroutine WorkQueue.push should use the native bool helper");
    assert(contains(code, "xr_aot_work_queue_close_void(ctx,") &&
           "coroutine WorkQueue.close should use the native void helper");
    /* `?? 0` consumes the optional exactly like an explicit null test does, so
     * WorkQueue<int>.pop keeps the typed has/value slot pair here too — nothing
     * about the coalescing form justifies a tagged round trip. */
    assert(contains(code, "xr_aot_work_queue_pop_i64_optional(ctx,") &&
           "WorkQueue<i64>.pop feeding ?? must still use the typed optional bridge");
    assert(!contains(code, "xr_aot_work_queue_pop_value(ctx,") &&
           "a typed WorkQueue.pop must not write its result through XrValue*");
    assert(contains(code, "S1:;\n    f->state = 1;") &&
           "native WorkQueue.pop resume must restore state after jumping to the label");
    assert(contains(code, "xr_aot_work_queue_pop_i64_optional_resume(ctx, &") &&
           "native WorkQueue.pop resume must use the rebuilt frame value slot");
    assert(!contains(code, "xr_aot_work_queue_pop(ctx,") &&
           "typed optional WorkQueue.pop should not use the generic slot-ref bridge");
    assert(contains(code, "xr_aot_trace_frame_value(visitor, f->v") &&
           "go-created Task values kept across spawn continuation must be traced");
    assert(!contains(code, "XrValue _wq_push_") &&
           "coroutine WorkQueue.push should not materialize an XrValue temp");
    assert(!contains(code, "XrValue _wq_close_") &&
           "coroutine WorkQueue.close should not materialize a tagged Unit result");

    printf("  Generated WorkQueue resume slot rebuild %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_work_queue_pop_i64_optional_uses_typed_abi) {
    const char *src = "const queue: WorkQueue<i64> = WorkQueue<i64>(1, 4)\n"
                      "fn consumer() -> i64 {\n"
                      "    var item = queue.pop(0)\n"
                      "    if (item == null) { return -1 }\n"
                      "    var value = item!\n"
                      "    return value * 2\n"
                      "}\n"
                      "var task = go consumer()\n"
                      "assert(queue.push(21, 0))\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "AOT WorkQueue i64 optional pop should generate");
    TEST_REQUIRE(contains(code, "xr_aot_work_queue_push_bool(ctx,"),
                 "coroutine WorkQueue.push should use the native bool helper");
    TEST_REQUIRE(contains(code, "xr_aot_work_queue_pop_i64_optional(ctx,"),
                 "WorkQueue<i64>.pop should use typed optional i64 AOT helper");
    TEST_REQUIRE(contains(code, "xr_aot_work_queue_pop_i64_optional_resume(ctx,"),
                 "WorkQueue<i64>.pop resume should use typed optional i64 AOT helper");
    TEST_REQUIRE(contains(code, "_opt_has") && contains(code, "_opt_value"),
                 "WorkQueue<i64>.pop should store nullable state as has/value slots");
    TEST_REQUIRE(!contains(code, "_wq_pop_slot_"),
                 "typed optional WorkQueue.pop should not materialize a generic slot ref");
    TEST_REQUIRE(!contains(code, "xr_aot_work_queue_pop_value(ctx,"),
                 "typed optional WorkQueue.pop should not write through XrValue*");
    TEST_REQUIRE(!contains(code, "xr_aot_work_queue_pop_resume(ctx,"),
                 "typed optional WorkQueue.pop should not use the generic resume bridge");
    const char *consumer_fn = strstr(code, "test_consumer_");
    const char *consumer_end = next_static_after(consumer_fn);
    TEST_REQUIRE(consumer_fn != NULL && consumer_end != NULL,
                 "consumer function should be generated for WorkQueue typed optional test");
    TEST_REQUIRE(count_between(consumer_fn, consumer_end, "xrt_eq(") == 0 &&
                     count_between(consumer_fn, consumer_end, "XR_TO_INT(") == 0,
                 "typed optional WorkQueue.pop should lower null check and unwrap to has/value");
    TEST_REQUIRE(!contains(code, "Attempted to unwrap a null value"),
                 "guarded force unwrap should stay a no-op after the null guard");
    TEST_REQUIRE(!contains(code, "XrValue _wq_push_"),
                 "coroutine WorkQueue.push should not materialize an XrValue temp");

    printf("  Generated WorkQueue typed optional i64 path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_result_group_recv_i64_optional_uses_typed_abi) {
    const char *src = "import { ResultGroup } from sync\n"
                      "fn consumer() -> i64 {\n"
                      "    const group: ResultGroup = ResultGroup(1)\n"
                      "    group.add(21)\n"
                      "    var item = group.recv()\n"
                      "    if (item == null) { return -1 }\n"
                      "    var value = item!\n"
                      "    return value * 2\n"
                      "}\n"
                      "var task = go consumer()\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir_with_module_graph(src);
    TEST_REQUIRE(ir != NULL, "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    TEST_REQUIRE(code != NULL, "C code generation failed");
    TEST_REQUIRE(!had_error, "AOT ResultGroup i64 optional recv should generate");
    TEST_REQUIRE(contains(code, "xr_aot_result_group_recv_i64_optional(ctx,"),
                 "ResultGroup.recv should use typed optional i64 AOT helper");
    TEST_REQUIRE(contains(code, "xr_aot_result_group_recv_i64_optional_resume(ctx,"),
                 "ResultGroup.recv resume should use typed optional i64 AOT helper");
    TEST_REQUIRE(contains(code, "_opt_has") && contains(code, "_opt_value"),
                 "ResultGroup.recv should store nullable state as has/value slots");
    TEST_REQUIRE(!contains(code, "_rg_recv_slot_"),
                 "typed optional ResultGroup.recv should not materialize a generic slot ref");
    TEST_REQUIRE(!contains(code, "xr_aot_result_group_recv_value(ctx,"),
                 "typed optional ResultGroup.recv should not write through XrValue*");
    TEST_REQUIRE(!contains(code, "xrt_eq("),
                 "typed optional ResultGroup.recv null check should not call xrt_eq");

    printf("  Generated ResultGroup typed optional i64 path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_work_queue_native_methods_use_aot_helpers) {
    const char *src = "const queue: WorkQueue<i64> = WorkQueue<i64>(4, 2)\n"
                      "fn use_queue() -> i64 {\n"
                      "    assert(queue.push(1, 0))\n"
                      "    assertEqual(queue.pushRange(2, 2, 0), 2)\n"
                      "    var (value, ok) = queue.tryPop(0)\n"
                      "    if (!ok) { return -1 }\n"
                      "    if (queue.isClosed) { return -2 }\n"
                      "    queue.close()\n"
                      "    return value! + len(queue) + queue.shardCount\n"
                      "}\n"
                      "print(use_queue())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT WorkQueue native methods should generate");
    assert(contains(code, "xr_aot_work_queue_push_bool_sync(") &&
           "WorkQueue.push should use the native bool sync AOT bridge");
    assert(contains(code, "xr_aot_work_queue_push_range_i64_sync(") &&
           "WorkQueue.pushRange should use the native i64 sync AOT bridge");
    assert(contains(code, "xr_aot_work_queue_close_void_sync(") &&
           "WorkQueue.close should use the native void sync AOT bridge");
    assert(contains(code, "xr_aot_work_queue_try_pop_sync(") &&
           "WorkQueue.tryPop should use the sync AOT bridge outside suspendable code");
    assert(contains(code, "xr_aot_work_queue_length(") &&
           "len(WorkQueue) should read through the AOT helper");
    assert(contains(code, "xr_aot_work_queue_shard_count(") &&
           "WorkQueue.shardCount should read through the AOT helper");
    assert(contains(code, "xr_aot_work_queue_is_closed(") &&
           "WorkQueue.isClosed should read through the AOT helper");
    assert(contains(code, "runtime_cfg.caps = ") && contains(code, "XR_AOT_CAP_WORK_QUEUE") &&
           "sync WorkQueue main must create a work-queue-capable AOT runtime");
    assert(contains(code, "xrt_global_ctx.runtime = rt;") &&
           "sync WorkQueue helpers must receive a runtime-backed global context");
    assert(!contains(code, "xray_vm_new_full(") && "sync WorkQueue main must not use a VM isolate");
    assert(!contains(code, "xrt_method_0(") && !contains(code, "xrt_method_1(") &&
           "WorkQueue native methods must not fall back to dynamic method dispatch");
    assert(!contains(code, "xr_aot_work_queue_push_sync(") &&
           "sync WorkQueue.push should not return tagged XrValue");
    assert(!contains(code, "xr_aot_work_queue_push_range_sync(") &&
           "sync WorkQueue.pushRange should not return tagged XrValue");
    assert(!contains(code, "xr_aot_work_queue_close_sync(") &&
           "sync WorkQueue.close should not return tagged XrValue");

    printf("  Generated WorkQueue native method helpers %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_task_status_uses_native_enum_status) {
    const char *src = "fn wait_for_value(ch: Channel<i64>) -> i64 {\n"
                      "    match (ch.recv()) {\n"
                      "        Recv.Value(value) -> { return value }\n"
                      "        _ -> { return -1 }\n"
                      "    }\n"
                      "}\n"
                      "fn quick_value(n: i64) -> i64 {\n"
                      "    Coro.yield()\n"
                      "    return n * 2\n"
                      "}\n"
                      "fn task_done(task: Task<i64>) -> bool {\n"
                      "    return task.done\n"
                      "}\n"
                      "fn task_status(task: Task<i64>) -> TaskStatus {\n"
                      "    return task.status\n"
                      "}\n"
                      "fn task_poll(task: ref Task<i64>) -> TaskResult<i64> {\n"
                      "    return task.poll()\n"
                      "}\n"
                      "const ch = Channel<i64>(0)\n"
                      "var blocked = go wait_for_value(ch)\n"
                      "blocked.cancel()\n"
                      "var cancelled_result = blocked.awaitResult()\n"
                      "print(blocked.done)\n"
                      "print(blocked.status)\n"
                      "print(cancelled_result)\n"
                      "var quick = go quick_value(21)\n"
                      "var quick_result = await quick\n"
                      "print(quick.done)\n"
                      "print(quick.status)\n"
                      "print(quick.poll())\n"
                      "print(quick.awaitResult())\n"
                      "print(quick_result)\n"
                      "print(task_done(quick))\n"
                      "print(task_status(quick))\n"
                      "print(task_poll(ref quick))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT Task status access should generate");
    assert(contains(code, "xr_aot_task_cancel(ctx,") && "Task.cancel must use the AOT Task bridge");
    assert(contains(code, "xr_aot_task_done(ctx,") && "Task.done must use the AOT Task bridge");
    assert(contains(code, "xr_aot_task_status(ctx,") && "Task.status must use the AOT Task bridge");
    assert(!contains(code, "xr_aot_bridge_value_to_xrt(xr_aot_task_status") &&
           "Task.status must already return AOT-native enum values");
    assert(contains(code, "xr_aot_task_poll(ctx,") && "Task.poll must use the AOT Task bridge");
    assert(contains(code, "xr_aot_task_await_result(ctx,") &&
           "Task.awaitResult must use the AOT TaskResult bridge");
    assert(contains(code, "xr_aot_task_done(&xrt_global_ctx,") &&
           "sync AOT Task.done must use the global AOT context");
    assert(contains(code, "xr_aot_task_status(&xrt_global_ctx,") &&
           "sync AOT Task.status must use the global AOT context");
    assert(!contains(code, "xr_aot_bridge_value_to_xrt(xr_aot_task_status(&xrt_global_ctx") &&
           "sync Task.status must already return AOT-native enum values");
    assert(contains(code, "xr_aot_task_poll(&xrt_global_ctx,") &&
           "sync AOT Task.poll must use the global AOT context");
    assert(!contains(code, "xr_aot_task_poll(NULL,") &&
           "sync AOT Task.poll must not lose runtime/builtin context");
    assert(!contains(code, "xrt_method_0(v") &&
           "Task.cancel must not fall back to AOT dynamic method dispatch");
    assert(!contains(code, "xrt_getprop(v") &&
           "Task fields must not fall back to AOT dynamic property dispatch");

    printf("  Generated native Task.status helper %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_structural_field_named_like_builtin_property_uses_ordinal) {
    const char *src = "type Counted = {count: i64}\n"
                      "fn read_count(data: Counted) -> i64 {\n"
                      "    return data.count\n"
                      "}\n"
                      "var data: Counted = {count: 10}\n"
                      "print(read_count(data))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "structural field access should generate");
    assert(contains(code, "xrt_object_get_field(") &&
           "structural fields must use their verified ordinal");
    assert(!contains(code, "xrt_object_get_name_owned(") &&
           "structural fields must not fall back to name lookup");
    assert(!contains(code, "xrt_getprop(v0, 234)") &&
           "a structural count field must not lower as the builtin count property");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_json_decode_loop_keeps_per_iteration_retain) {
    XrType unit_type = {
        .kind = XR_KIND_UNIT, .scalar_rep = XR_SCALAR_REP_NONE, .id = 940, .frozen = true};
    XrType bool_type = {
        .kind = XR_KIND_BOOL, .scalar_rep = XR_SCALAR_REP_NONE, .id = 941, .frozen = true};
    XrType map_type = {.kind = XR_KIND_MAP, .id = 942, .frozen = true};
    XrType object_type = {.kind = XR_KIND_STRUCT_OBJECT, .id = 943, .frozen = true};
    XiFunc *ir = xi_func_new("json_decode_loop_arc", &unit_type);
    TEST_REQUIRE(ir != NULL, "manual JSON decode ARC function allocated");
    XiBlock *entry = xi_block_new(ir);
    XiBlock *header = xi_block_new(ir);
    XiBlock *body = xi_block_new(ir);
    XiBlock *exit = xi_block_new(ir);
    TEST_REQUIRE(entry && header && body && exit, "manual JSON decode ARC blocks allocated");

    XiValue *object = xi_value_new(ir, entry, XI_MAP_NEW, &map_type, 0);
    XiValue *cond = xi_const_bool(ir, header, true, &bool_type);
    XiValue *retain = xi_value_new(ir, body, XI_RETAIN, &map_type, 1);
    XiValue *decode = xi_value_new(ir, body, XI_JSON_DECODE, &object_type, 1);
    XiValue *release = xi_value_new(ir, exit, XI_RELEASE, &map_type, 1);
    TEST_REQUIRE(object && cond && retain && decode && release,
                 "manual JSON decode ARC values allocated");
    retain->args[0] = object;
    decode->args[0] = object;
    release->args[0] = object;
    xi_block_set_jump(entry, header);
    xi_block_set_if(header, cond, body, exit);
    xi_block_set_jump(body, header);
    xi_block_set_return(exit, NULL);
    entry->sealed = header->sealed = body->sealed = exit->sealed = true;

    int eliminated = xi_arc_elim(ir);
    TEST_REQUIRE(eliminated == 0,
                 "single-consumer elimination must not remove a retain in a CFG cycle");
    bool retained = false;
    for (uint32_t i = 0; i < body->nvalues; i++) {
        if (body->values[i] == retain) {
            retained = true;
            break;
        }
    }
    TEST_REQUIRE(retained,
                 "loop-carried JSON object must retain once before every consuming decode");

    xi_func_free(ir);
}

/* ========== Main ========== */

int main(int argc, char **argv) {
    printf("=== Xi CGen Unit Tests ===\n\n");

    g_test_filter = getenv("XRAY_TEST_FILTER");
    setup();
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "native-target-leaf-emission") == 0) {
        g_native_target_leaf_c_output = argc == 3 ? argv[2] : NULL;
        run_cgen_native_target_leaf_consumes_numeric_target_authority();
        teardown();
        puts("Native target leaf CGen tests passed");
        return tests_failed > 0 ? 1 : 0;
    }
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "string-runes-emission") == 0) {
        g_string_runes_c_output = argc == 3 ? argv[2] : NULL;
        run_cgen_string_runes_consumes_immutable_emission_recipe();
        teardown();
        puts("String.runes CGen recipe tests passed");
        return tests_failed > 0 ? 1 : 0;
    }
    if (argc == 2 && strcmp(argv[1], "iterator-rune-nth-emission") == 0) {
        run_cgen_iterator_rune_nth_consumes_immutable_emission_recipe();
        teardown();
        puts("Iterator<rune>.nth CGen recipe tests passed");
        return tests_failed > 0 ? 1 : 0;
    }
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "rune-to-string-emission") == 0) {
        g_rune_to_string_c_output = argc == 3 ? argv[2] : NULL;
        run_cgen_rune_to_string_consumes_immutable_emission_recipe();
        teardown();
        puts("rune.toString CGen recipe tests passed");
        return tests_failed > 0 ? 1 : 0;
    }

    run_u64_mul_wide_returns_both_exact_halves();
    run_aot_type_fingerprint_includes_param_modes();
    run_aot_type_fingerprint_separates_error_recovery();
    run_aot_extern_registry_deduplicates_and_rejects_conflicts();
    run_cgen_extern_symbol_binding_is_portable_and_verified();
    run_aot_extern_symbol_rename_requires_typed_qualification();
    run_target_plan_scalar_ref_c_emission_from_source();
    run_aot_semantic_snapshot_survives_analyzer_pool_churn();
    run_target_plan_owned_string_lifecycle_from_source();
    run_cgen_json_codec_summary_preflight_is_exact_and_fail_closed();
    run_cgen_restricted_c90_header_is_explicit_and_minimal();
    run_cgen_simple_arith();
    run_cgen_target_layout_queries_emit_source_backed_constants();
    run_cgen_rep_identical_source_alias_shares_immutable_c_local();
    run_cgen_rep_identical_unbox_shares_immutable_c_local();
    run_cgen_fixed_array_alias_address_projection_shares_backing_c_local();
    run_cgen_scalar_alias_materializes_when_c_address_is_taken();
    run_cgen_forward_use_predeclarations_have_no_dead_initializers();
    run_cgen_trivial_span_value_clone_shares_immutable_c_local();
    run_cgen_rep_identical_span_box_shares_immutable_c_local();
    run_cgen_scalar_value_clone_remains_distinct_c_local();
    run_cgen_immediate_scalar_constant_keeps_debug_sync_without_release_local();
    run_cgen_returned_scalar_constant_emits_immediate_without_local();
    run_cgen_returned_null_constant_emits_immediate_without_local();
    run_cgen_multi_concat_string_constants_emit_immediate_without_locals();
    run_cgen_shared_string_constant_emits_immediate_without_local();
    run_cgen_unused_call_result_emits_effect_statement_without_local();
    run_cgen_unused_array_reserve_result_emits_effect_statement_without_local();
    run_cgen_dead_native_box_without_source_storage_is_elided();
    run_cgen_native_unsigned_interpolation_consumes_inner_without_box_local();
    run_cgen_panicinfo_constructor_token_emits_no_local();
    run_cgen_direct_stdlib_import_call_emits_no_function_token_local();
    run_cgen_native_target_leaf_consumes_numeric_target_authority();
    run_cgen_string_literal_runes_receiver_emits_immediate_without_local();
    run_cgen_string_runes_consumes_immutable_emission_recipe();
    run_cgen_string_slice_range_consumes_immutable_emission_recipe();
    run_cgen_iterator_rune_has_next_consumes_immutable_emission_recipe();
    run_cgen_iterator_rune_next_consumes_immutable_emission_recipe();
    run_cgen_iterator_rune_nth_consumes_immutable_emission_recipe();
    run_cgen_rune_to_uint32_consumes_immutable_emission_recipe();
    run_cgen_rune_to_string_consumes_immutable_emission_recipe();
    run_cgen_rune_is_whitespace_consumes_immutable_emission_recipe();
    run_cgen_span_passed_only_to_direct_call_omits_data_cache();
    run_cgen_span_print_materializes_scoped_portable_view();
    run_cgen_unused_shared_load_is_debug_only_when_source_bound();
    run_cgen_consumed_shared_load_stays_release_materialized();
    run_cgen_shared_store_uses_portable_owned_value_helper();
    run_cgen_immediate_scalar_constant_inlines_into_as_cast();
    run_cgen_dynamic_conversion_inlines_null_literal_without_forward_ref();
    run_cgen_immediate_scalar_constant_inlines_into_place_store();
    run_cgen_native_signed_i64_constant_emits_immediate_without_local();
    run_cgen_runtime_string_slice_constant_emits_immediate_without_local();
    run_cgen_typed_array_constants_emit_immediate_without_locals();
    run_cgen_clean_narrow_arithmetic_keeps_required_constant_local();
    run_cgen_scalar_emission_plan_owns_local_rep_and_c_spelling();
    run_cgen_struct_fixed_array_index_keeps_required_constant_local();
    run_cgen_skips_unused_process_builtin_init();
    run_cgen_initializes_used_process_builtin();
    run_cgen_initializes_file_dir_builtins_from_entry_source();
    run_cgen_runtime_file_dir_stays_runtime_owned();
    run_cgen_standalone_prelude_enum_globals_generate_static_members();
    run_cgen_cancelled_builtin_generates_false();
    run_cgen_variable_and_print();
    run_cgen_if_else();
    run_cgen_ordinary_bool_control_has_no_probability_wrapper();
    run_cgen_multi_print();
    run_cgen_while_loop();
    run_cgen_string_literal();
    run_cgen_str_concat_uses_single_allocation_helper();
    run_cgen_string_concat_cleanup_consumes_immutable_emission();
    run_cgen_function_call();
    run_cgen_canonical_generic_function_body_is_executable();
    run_cgen_plain_function_does_not_emit_public_c_abi_wrapper();
    run_cgen_multimodule_private_helpers_are_file_local_inline();
    run_cgen_multimodule_branching_dispatcher_defers_to_native_inliner();
    run_cgen_noinline_attribute_preserves_native_boundary();
    run_cgen_inline_attribute_forces_native_expansion();
    run_cgen_c_export_wrapper_keeps_default_visibility();
    run_cgen_hosted_string_array_boundary_uses_deep_value_bridge();
    run_cgen_hosted_byte_slice_boundary_uses_layout_neutral_view();
    run_cgen_hosted_runtime_capability_installs_scoped_vm_context();
    run_cgen_hosted_coroutine_export_publishes_resumable_continuation();
    run_cgen_hosted_class_boundary_uses_nominal_opaque_proxy();
    run_cgen_hosted_native_field_store_uses_portable_c_statements();
    run_cgen_hosted_non_escaping_string_argument_stays_zero_copy();
    run_cgen_hosted_normal_class_result_call_is_not_a_constructor();
    run_cgen_stats_tracks_native_abi();
    run_cgen_module_prefix_is_c_identifier();
    run_cgen_emits_source_line_directives();
    run_cgen_emits_debug_source_var_slots();
    run_cgen_emits_shadowed_debug_source_var_slots();
    run_cgen_struct_debug_source_var_slots_use_typed_pointers();
    run_cgen_local_value_struct_copy_consumes_named_aggregate_emission();
    run_cgen_struct_field_only_place_loads_are_debug_guarded();
    run_cgen_struct_raw_deref_method_receiver_skips_release_copy();
    run_cgen_struct_scalar_field_ref_skips_release_load();
    run_cgen_mem_slice_struct_pointer_owner_load_is_elided();
    run_cgen_native_bool_assert_does_not_materialize_box();
    run_cgen_assertion_calls_use_typed_adapters();
    run_cgen_span_phi_snapshot_is_debug_only();
    run_cgen_span_ref_only_value_omits_unused_data_cache();
    run_cgen_struct_value_abi_uses_canonical_layout_typedef();
    run_cgen_coro_emits_source_line_directives();
    run_cgen_coro_emits_debug_source_var_slots();
    run_cgen_coro_syncs_helper_result_debug_source_vars();
    run_cgen_recursive();
    run_cgen_for_loop();
    run_cgen_parallel_for_each_uses_runtime_executor();
    run_cgen_parallel_map_into_scalar_lanes_use_direct_storage();
    run_cgen_parallel_map_return_uses_runtime_executor();
    run_cgen_parallel_reduce_uses_runtime_executor();
    run_cgen_parallel_reduce_struct_accumulator_uses_aggregate_runtime();
    run_lower_parallel_call_plan_resolves_selective_aliases();
    run_lower_parallel_plan_methods_preserve_intrinsic_identity();
    run_cgen_parallel_for_each_allows_atomic_i64_direct_body();
    run_analyzer_parallel_for_each_rejects_throwing_body();
    run_cgen_parallel_for_body_closure_stack_allocates();
    run_cgen_typed_array_uses_raw_storage_fast_path();
    run_cgen_checked_typed_array_store_proves_nonnull_data();
    run_cgen_stringbuilder_constructor_consumes_emission_recipe();
    run_cgen_coro_stringbuilder_constructor_consumes_emission_recipe();
    run_cgen_builtin_iterator_pull_methods_preserve_error_polls();
    run_cgen_err_check_releases_live_arc_owners_on_cold_edge();
    run_cgen_typed_array_u8_uses_byte_storage_fast_path();
    run_cgen_source_class_array_push_consumes_generated_emission_recipe();
    run_cgen_string_copy_bytes_preserves_byte_storage_fast_path();
    run_cgen_typed_array_zero_fill_range_uses_memset();
    run_cgen_byte_slice_safe_methods_use_stable_owners();
    run_cgen_byte_array_copy_uses_stable_owner_adapter();
    run_cgen_byte_slice_native_load_elides_endian_box();
    run_cgen_span_window_and_mem_slice_elide_boxed_operands();
    run_cgen_borrowed_bytes_param_reserve_skips_arc();
    run_cgen_direct_call_converts_bytes_to_byte_slice_arg();
    run_cgen_boxed_adapter_converts_byte_slice_arg();
    run_cgen_array_data_ptr_unchecked_uses_raw_pointer_path();
    run_cgen_zero_byte_rawptr_copy_accepts_null_without_memcpy();
    run_cgen_cfn_local_coercion_uses_native_function_address();
    run_cgen_rawptr_copy_forwarded_constant_has_no_release_local();
    run_cgen_rawptr_parallel_for_each_capture_is_rejected();
    run_cgen_span_index_get_elides_dead_err_check();
    run_cgen_span_index_set_checks_readonly_flag();
    run_cgen_span_slice_elides_dead_err_check();
    run_cgen_byte_array_append_from_slice_elides_dead_err_check();
    run_cgen_byte_array_repeat_from_tail_elides_dead_err_check();
    run_cgen_verified_span_helper_drop_elides_pending_error_checks();
    run_cgen_mem_load_uses_pointer_helper();
    run_cgen_mem_store_uses_pointer_helper();
    run_cgen_stack_borrow_slice_allows_local_rawptr_read_chain();
    run_cgen_stack_borrow_slice_rejects_returned_rawptr();
    run_cgen_typed_array_i16_and_u32_use_raw_storage_fast_path();
    run_cgen_typed_array_float_and_bool_use_raw_storage_fast_path();
    run_cgen_inlined_struct_uses_native_field_storage();
    run_cgen_escaping_struct_uses_heap_native_storage();
    run_cgen_same_shape_structs_keep_distinct_source_field_names();
    run_cgen_escaping_struct_string_field_uses_heap_native_storage();
    run_cgen_fixed_layout_struct_omits_native_header();
    run_cgen_nested_struct_field_uses_embedded_heap_native_storage();
    run_cgen_fixed_array_struct_field_uses_embedded_heap_native_storage();
    run_cgen_fixed_array_local_uses_stack_array_ref_storage();
    run_cgen_fixed_array_local_return_clones_borrowed_stack_storage();
    run_cgen_fixed_array_index_ops_elide_boxed_operands();
    run_cgen_static_method_call_elides_class_descriptor_receiver();
    run_cgen_map_class_static_factory_is_not_constructor();
    run_cgen_shared_struct_alias_elides_tagged_hot_locals();
    run_cgen_class_method_caches_receiver_scalar_fields();
    run_cgen_local_class_direct_native_methods_omit_boxed_adapters();
    run_cgen_native_receiver_static_cleanup_borrows_without_closure_arc();
    run_cgen_class_constructor_returns_heap_native_instance();
    run_cgen_native_class_ref_field_constructor_result_uses_ptr_storage();
    run_cgen_native_class_collection_ref_fields_use_arc();
    run_cgen_class_set_length_size_sum_uses_native_arithmetic();
    run_cgen_class_set_u8_uses_typed_direct_helpers();
    run_cgen_class_map_i64_i64_uses_typed_direct_helpers();
    run_cgen_class_bool_key_map_uses_specialized_direct_helpers();
    run_cgen_class_map_bool_value_guarded_condition_uses_native();
    run_cgen_class_map_bool_value_unguarded_explicit_true_uses_tagged_compare();
    run_cgen_inherited_class_uses_native_base_layout();
    run_cgen_typed_array_slice_preserves_raw_storage_fast_path();
    run_cgen_typename_as_and_slice_use_direct_drivers();
    run_cgen_typeid_uses_stable_owner_adapter();
    run_cgen_exact_bits_use_stable_owner_adapter();
    run_cgen_bits_not_uses_stable_owner_adapter();
    run_cgen_numeric_neg_uses_stable_owner_adapter();
    run_cgen_bitwise_binary_uses_stable_owner_adapter();
    run_cgen_numeric_width_uses_stable_owner_adapter();
    run_cgen_byte_slice_scalar_uses_stable_owner_adapter();
    run_cgen_byte_slice_compare_uses_stable_owner_adapter();
    run_cgen_byte_slice_fill_uses_stable_owner_adapter();
    run_cgen_byte_slice_mutation_uses_stable_owner_adapters();
    run_cgen_byte_slice_common_prefix_uses_stable_owner_adapter();
    run_cgen_pod_slice_copy_compare_use_stable_owner_adapters();
    run_cgen_pod_slice_fill_uses_stable_owner_adapter();
    run_cgen_pod_slice_view_uses_stable_owner_adapter();
    run_cgen_raw_memory_copy_owner_registry_is_stable();
    run_cgen_enum_metadata_access_uses_stable_owner_adapter();
    run_cgen_cell_access_uses_stable_owner_adapter();
    run_cgen_null_test_uses_stable_owner_adapter();
    run_cgen_force_unwrap_checktype_uses_portable_borrowed_helper();
    run_cgen_same_type_as_lowers_away_without_arc();
    run_cgen_closure_values_and_indirect_calls_use_portable_c();
    run_cgen_cell_backed_function_upvalue_uses_boxed_indirect_call();
    run_cgen_range_uses_direct_aot_driver();
    run_cgen_typed_array_slice_loop_uses_guarded_unchecked_raw_load();
    run_cgen_typed_array_branchy_fill_loop_uses_preallocated_raw_store();
    run_cgen_typed_array_filter_preserves_raw_storage_fast_path();
    run_cgen_typed_array_map_uses_typed_result_storage_fast_path();
    run_cgen_typed_array_map_readonly_result_caches_data_pointer();
    run_cgen_typed_array_map_captured_callback_fails_closed();
    run_cgen_typed_array_direct_hof_callback_extra_use_fails_closed();
    run_cgen_typed_array_rune_uses_scalar_storage_with_rune_boxing();
    run_cgen_dynamic_uncaptured_callback_keeps_boxed_adapter();
    run_aot_closure_direct_symbol_requires_frozen_coroutine_plan();
    run_cgen_closure_cell_var_id_above_255();
    run_cgen_stack_alloc_direct_closure_uses_stack_runtime();
    run_cgen_stack_alloc_closure_preserves_cell_capture();
    run_cgen_typed_array_filter_readonly_result_caches_data_pointer();
    run_cgen_typed_array_filter_captured_callback_fails_closed();
    run_cgen_typed_array_reduce_uses_native_accumulator_fast_path();
    run_cgen_typed_array_unused_reduce_still_executes_callback_loop();
    run_cgen_typed_array_direct_hof_requires_exact_callback_abi_plan();
    run_cgen_typed_array_reduce_captured_callback_fails_closed();
    run_cgen_int_const_div_mod_uses_native_ops();
    run_cgen_shift_uses_stable_owner_adapter();
    run_cgen_unsigned_shift_uses_stable_owner_adapter();
    run_cgen_unsigned_arith_uses_native_unsigned_expr();
    run_cgen_elides_dead_err_checks_after_nothrow_scalar_helper_chain();
    run_cgen_codegen_controls_emit_provider_constructs_without_runtime_calls();
    run_cgen_uses_closed_world_effects_for_conservative_direct_call_checks();
    run_cgen_static_cleanup_isolates_existing_pending_error();
    run_cgen_err_return_stops_unreachable_tail();
    run_cgen_unsupported_coroutine_ops_fail_fast();
    run_cgen_unresolved_import_fails_fast();
    run_cgen_unknown_method_symbol_fails_fast();
    run_cgen_suspendable_function_has_no_sync_wrapper();
    run_cgen_direct_suspend_call_propagates_cps();
    run_cgen_direct_suspend_call_borrows_read_argument();
    run_cgen_direct_suspend_enum_result_consumes_owned_box();
    run_cgen_returned_suspendable_closure_uses_verified_child_frame();
    run_cgen_mixed_callable_targets_use_stable_descriptor_switch();
    run_cgen_direct_suspend_method_call_propagates_cps();
    run_cgen_shared_static_function_retain_is_elided();
    run_cgen_coro_shared_static_function_retain_is_elided();
    run_cgen_suspendable_dependency_init_fails_fast();
    run_cgen_coro_frame_params_use_typed_storage();
    run_cgen_coro_frame_skips_dead_ssa_slots();
    run_cgen_coro_loop_tail_phi_uses_shared_suspend_plan();
    run_cgen_coro_wait_driven_loop_omits_redundant_poll();
    run_cgen_countdown_latch_methods_use_native_helpers();
    run_cgen_semaphore_methods_use_native_helpers();
    run_cgen_sync_blocking_direct_methods_mark_aot_coroutines();
    run_cgen_runtime_managed_types_skip_arc();
    run_cgen_coro_frame_release_uses_aot_arc();
    run_cgen_coro_owner_forward_clears_moved_frame_root();
    run_cgen_coro_go_clones_tagged_args();
    run_cgen_coro_go_sync_function_uses_wrapper_desc();
    run_cgen_coro_go_sync_scalar_wrapper_skips_param_roots();
    run_cgen_coro_go_zero_state_sync_wrapper_has_nonempty_frame();
    run_cgen_sync_functions_without_go_emit_no_aot_wrappers();
    run_cgen_coro_sync_go_wrappers_only_for_go_targets();
    run_cgen_sync_aot_backedge_heartbeat_only_for_runtime_reachable_loops();
    run_cgen_coro_channel_send_copy_uses_transfer_helper();
    run_cgen_coro_scalar_channel_send_skips_clone();
    run_cgen_coro_unit_match_send_omits_void_phi();
    run_cgen_descriptor_scalar_channel_try_send_uses_typed_sync_bridge();
    run_cgen_descriptor_tagged_channel_try_send_normalizes_runtime_envelope();
    run_cgen_coro_builtin_no_payload_enum_fields_skip_bridge();
    run_cgen_coro_await_clones_tagged_result();
    run_cgen_coro_native_class_await_uses_tagged_boundary_slot();
    run_cgen_coro_scalar_await_uses_tagged_slot();
    run_cgen_coro_await_array_task_index_borrows_checked_slot();
    run_cgen_coro_one_shot_await_task_array_loop_borrows_checked_slot();
    run_cgen_coro_await_timeout_passes_deadline();
    run_cgen_tagged_null_equality_keeps_null_literal();
    run_cgen_coro_recv_resume_uses_wait_state_slot();
    run_cgen_coro_discarded_recv_does_not_materialize_result();
    run_cgen_coro_fused_scalar_channel_recv_uses_typed_pair_bridge();
    run_cgen_coro_scalar_channel_recv_uses_tagged_slot();
    run_cgen_coro_channel_recv_null_check_keeps_tagged_slot();
    run_cgen_descriptor_scalar_channel_try_recv_returns_recv_enum();
    run_cgen_descriptor_select_try_recv_uses_ready_bit();
    run_cgen_coro_sleep_publishes_state_before_block();
    run_cgen_test_yield_calls_publish_resume_states();
    run_cgen_rejects_mutated_frozen_coroutine_plan();
    run_cgen_runtime_needed_main_uses_aot_runtime();
    run_cgen_coro_select_publishes_state_before_block();
    run_cgen_coro_channel_timeout_publishes_state_before_block();
    run_cgen_coro_recv_slot_is_traced_as_frame_root();
    run_cgen_coro_await_all_uses_aggregate_bridge();
    run_cgen_coro_await_all_named_task_array_skips_task_list_clone();
    run_cgen_coro_await_all_reused_push_task_array_uses_one_shot();
    run_cgen_coro_await_all_into_reuses_result_array();
    run_cgen_coro_top_level_await_all_into_keeps_result_array_alive();
    run_cgen_coro_result_group_fire_and_forget_go_uses_deferred_batch();
    run_cgen_coro_result_group_reset_uses_native_helper();
    run_cgen_result_group_sync_methods_elide_dead_err_checks();
    run_cgen_coro_await_any_uses_typed_aggregate_bridge();
    run_cgen_coro_scope_exit_publishes_state_before_block();
    run_cgen_channel_fields_use_aot_helpers();
    run_cgen_sync_go_channel_try_methods_use_aot_helpers();
    run_cgen_coro_work_queue_resume_rebuilds_slot_and_traces_task();
    run_cgen_coro_work_queue_pop_i64_optional_uses_typed_abi();
    run_cgen_coro_result_group_recv_i64_optional_uses_typed_abi();
    run_cgen_work_queue_native_methods_use_aot_helpers();
    run_cgen_coro_task_status_uses_native_enum_status();
    run_cgen_structural_field_named_like_builtin_property_uses_ordinal();
    run_cgen_json_decode_loop_keeps_per_iteration_retain();

    teardown();

    printf("\n=== %d/%d Xi CGen tests passed ===\n", tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
