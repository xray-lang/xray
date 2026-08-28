/*
 * test_xi_stage.c - Unit tests for Xi IR stage contract
 *
 * Verifies:
 *   1. XiStage enum values and xi_stage_name()
 *   2. XiFunc.stage is set to XI_STAGE_RAW after lowering
 *   3. XiPassDesc stage contract enforcement in pipeline
 *   4. xi_verify accepts well-formed IR at each stage
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_arc.h"
#include "../../../src/ir/xi_backend.h"
#include "../../../src/ir/xi_backend_lower.h"
#include "../../../src/ir/xi_coro_lower.h"
#include "../../../src/ir/xi_effect.h"
#include "../../../src/ir/xi_evidence.h"
#include "../../../src/ir/xi_escape.h"
#include "../../../src/ir/xi_pass.h"
#include "../../../src/ir/xi_verify.h"
#include "../../../src/ir/xi_pipeline.h"
#include "../../../src/ir/xi_stage.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/frontend/analyzer/xa_intrinsic_registry.h"
#include "../../../src/ir/xi_module.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/base/xchecks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Stage contracts are mandatory in Release builds too. Several helpers use
 * assertions as consuming API calls, so NDEBUG must never erase them. */
#ifdef NDEBUG
#undef assert
#define assert(condition)                                                                          \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "stage assertion failed: %s (%s:%d)\n", #condition, __FILE__,          \
                    __LINE__);                                                                     \
            abort();                                                                               \
        }                                                                                          \
    } while (0)
#endif

/* Minimal XrType stubs */
static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_void = {.kind = XR_KIND_UNIT, .id = 2, .frozen = true};
static XrType stub_string = {.kind = XR_KIND_STRING, .id = 3, .frozen = true};
static XrType stub_array = {
    .kind = XR_KIND_ARRAY,
    .id = 4,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .container = {.element_type = &stub_int},
};
static XrType stub_map = {.kind = XR_KIND_MAP,
                          .id = 5,
                          .frozen = true,
                          .map = {.key_type = &stub_int, .value_type = &stub_int}};
static XrType stub_set = {
    .kind = XR_KIND_SET, .id = 6, .frozen = true, .container = {.element_type = &stub_int}};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 7, .frozen = true};
static XrType stub_function = {
    .kind = XR_KIND_FUNCTION, .id = 8, .frozen = true, .function = {.return_type = &stub_int}};
static XrType stub_string_builder = {
    .kind = XR_KIND_INSTANCE, .id = 9, .frozen = true, .instance = {.class_name = "StringBuilder"}};
static XrType stub_shadow_string_builder = {
    .kind = XR_KIND_INSTANCE,
    .id = 10,
    .frozen = true,
    .instance = {.class_name = "StringBuilder", .class_ref = (XrClassInfo *) (uintptr_t) 1}};

static XiCoroLoweredProgram *advance_to_coro_lowered(XiFunc *f) {
    char error[512] = {0};
    XiRawProgram *raw = xi_stage_adopt_raw(f, error, sizeof(error));
    if (!raw)
        fprintf(stderr, "raw adoption failed: %s\n", error);
    assert(raw != NULL);
    XiCanonicalProgram *canonical = xi_program_canonicalize(raw, error, sizeof(error));
    assert(canonical != NULL);
    xi_pass_close(f);
    XiClosedProgram *closed = xi_program_close(canonical, error, sizeof(error));
    assert(closed != NULL);
    xi_escape_analyze(f);
    xi_arc_insert(f);
    xi_arc_elim(f);
    XiOwnedProgram *owned = xi_program_make_owned(closed, error, sizeof(error));
    assert(owned != NULL);
    XiSemanticLoweredProgram *semantic = xi_program_lower_semantics(owned, error, sizeof(error));
    assert(semantic != NULL);
    XiCoroLoweredProgram *coro = xi_program_lower_coroutines(semantic, NULL, error, sizeof(error));
    assert(coro != NULL);
    return coro;
}

static XiReppedProgram *advance_to_repped(XiFunc *f) {
    char error[512] = {0};
    XiCoroLoweredProgram *lowered = advance_to_coro_lowered(f);
    XiOptimizedProgram *optimized = xi_program_finish_optimization(lowered, error, sizeof(error));
    assert(optimized != NULL);
    /* The SemanticPlan builder requires a lowered graph to carry a typed
     * durable module identity and synthesizes none, so this fixture names its
     * own memory-namespace identity. xi_func_free owns the module it is
     * attached to and releases it with the function. */
    if (!f->module) {
        f->module = xi_module_new("xi_stage_fixture.xr", "xi_stage_fixture", f);
        assert(f->module != NULL);
        assert(xi_module_set_identity(f->module, "memory-module-v1:id=19:xi-stage-fixture-v1"));
    }
    bool planned = xr_semantic_plan_build_and_attach(f, error, sizeof(error));
    if (!planned)
        fprintf(stderr, "semantic plan attachment failed: %s\n", error);
    assert(planned);
    XiSemanticPlannedProgram *semantic =
        xi_program_freeze_semantics(optimized, error, sizeof(error));
    assert(semantic != NULL);
    XiReppedProgram *repped = xi_program_select_reps(semantic, error, sizeof(error));
    assert(repped != NULL);
    return repped;
}

static XiBackendProgram *finish_backend(XiReppedProgram *repped, XiFunc *f) {
    char error[512] = {0};
    xi_backend_lower(f);
    XiBackendProgram *backend = xi_program_plan_backend(repped, error, sizeof(error));
    if (!backend)
        fprintf(stderr, "backend transition failed: %s\n", error);
    assert(backend != NULL);
    return backend;
}

static void assert_rewritten_builtin(XiValue *v, const char *name) {
    assert(v != NULL);
    assert(v->op == XI_CALL_BUILTIN);
    assert(v->aux != NULL);
    assert(strcmp((const char *) v->aux, name) == 0);
    assert(xi_op_is_backend_legal(v->op));
    assert((v->flags & xi_op_default_effects(XI_CALL_BUILTIN)) ==
           xi_op_default_effects(XI_CALL_BUILTIN));
}

/* ========== Test 1: XiStage enum and names ========== */

static void test_stage_enum(void) {
    printf("--- test_stage_enum ---\n");

    /* Verify ordering */
    assert(XI_STAGE_RAW == 0);
    assert(XI_STAGE_CANONICAL > XI_STAGE_RAW);
    assert(XI_STAGE_CLOSED > XI_STAGE_CANONICAL);
    assert(XI_STAGE_OWNED > XI_STAGE_CLOSED);
    assert(XI_STAGE_SEMANTIC_LOWERED > XI_STAGE_OWNED);
    assert(XI_STAGE_CORO_LOWERED > XI_STAGE_SEMANTIC_LOWERED);
    assert(XI_STAGE_OPTIMIZED > XI_STAGE_CORO_LOWERED);
    assert(XI_STAGE_SEMANTIC_PLANNED > XI_STAGE_OPTIMIZED);
    assert(XI_STAGE_REPPED > XI_STAGE_SEMANTIC_PLANNED);
    assert(XI_STAGE_BACKEND > XI_STAGE_REPPED);
    assert(XI_STAGE_COUNT == 10);

    /* Verify names */
    assert(strcmp(xi_stage_name(XI_STAGE_RAW), "Raw") == 0);
    assert(strcmp(xi_stage_name(XI_STAGE_CANONICAL), "Canonical") == 0);
    assert(strcmp(xi_stage_name(XI_STAGE_CLOSED), "Closed") == 0);
    assert(strcmp(xi_stage_name(XI_STAGE_OWNED), "Owned") == 0);
    assert(strcmp(xi_stage_name(XI_STAGE_SEMANTIC_LOWERED), "SemanticLowered") == 0);
    assert(strcmp(xi_stage_name(XI_STAGE_CORO_LOWERED), "CoroLowered") == 0);
    assert(strcmp(xi_stage_name(XI_STAGE_OPTIMIZED), "Optimized") == 0);
    assert(strcmp(xi_stage_name(XI_STAGE_SEMANTIC_PLANNED), "SemanticPlanned") == 0);
    assert(strcmp(xi_stage_name(XI_STAGE_REPPED), "Repped") == 0);
    assert(strcmp(xi_stage_name(XI_STAGE_BACKEND), "Backend") == 0);

    /* Out-of-range returns "?" */
    assert(strcmp(xi_stage_name(XI_STAGE_COUNT), "?") == 0);
    assert(strcmp(xi_stage_name((XiStage) 99), "?") == 0);

    printf("  PASS\n");
}

/* ========== Test 2: XiFunc.stage default and after creation ========== */

static void test_func_stage_default(void) {
    printf("--- test_func_stage_default ---\n");

    XiFunc *f = xi_func_new("test_fn", &stub_int);
    assert(f != NULL);

    /* xi_func_new initializes a self-consistent RAW stage contract. */
    assert(f->stage == XI_STAGE_RAW);
    assert((f->invariant_mask & xi_stage_invariants(XI_STAGE_RAW)) ==
           xi_stage_invariants(XI_STAGE_RAW));

    XiBlock *entry = xi_block_new(f);
    XiValue *value = xi_const_int(f, entry, 0, &stub_int);
    xi_block_set_return(entry, value);
    char error[256] = {0};
    XiRawProgram *raw = xi_stage_adopt_raw(f, error, sizeof(error));
    assert(raw != NULL);
    XiCanonicalProgram *canonical = xi_program_canonicalize(raw, error, sizeof(error));
    assert(canonical != NULL);
    assert(f->stage == XI_STAGE_CANONICAL);
    assert(f->stage > XI_STAGE_RAW);

    assert(xi_canonical_program_release(canonical) == f);
    xi_func_free(f);
    printf("  PASS\n");
}

/* ========== Test 3: Stage set to RAW after lowering ========== */

static void test_stage_after_lowering(void) {
    printf("--- test_stage_after_lowering ---\n");

    /* Use the pipeline to lower a simple program */
    const char *source = "var x = 42\nprint(x)\n";

    /* Parse + analyze + lower via pipeline in CHECK mode (no opt, no emit) */
    XiPipelineConfig cfg = xi_pipeline_default_config();
    cfg.run_optimize = false;
    cfg.run_emit = false;

    /* We need a full parse+analyze cycle.  Use the compiler entry point
     * indirectly by testing that lowered IR has stage == RAW.
     * Since we can't easily call the full pipeline without an isolate,
     * we verify via the compile_program path. */

    /* For now, just verify the enum and struct field exist.
     * The full lowering path is tested by test_xi_pipeline. */
    printf("  (stage-after-lowering verified by test_xi_pipeline)\n");
    printf("  PASS\n");
    (void) source;
    (void) cfg;
}

/* ========== Test 4: XiPassDesc stage fields ========== */

static void test_pass_desc_fields(void) {
    printf("--- test_pass_desc_fields ---\n");

    /* Construct a pass descriptor with stage contract */
    XiPassDesc desc = {
        .name = "test_pass",
        .fn = NULL,
        .min_level = XI_OPT_LIGHT,
        .flags = XI_PASS_NONE,
        .min_stage = XI_STAGE_RAW,
        .max_stage = XI_STAGE_CORO_LOWERED,
        .requires_evidence = XI_EVD_ALIAS,
        .produces_evidence = XI_EVD_RANGE,
    };

    assert(desc.min_stage == XI_STAGE_RAW);
    assert(desc.max_stage == XI_STAGE_CORO_LOWERED);
    assert(desc.requires_evidence == XI_EVD_ALIAS);
    assert(desc.produces_evidence == XI_EVD_RANGE);

    XiPassDesc lowered_only = {
        .name = "semantic-opt",
        .fn = NULL,
        .min_level = XI_OPT_NONE,
        .flags = XI_PASS_REQUIRED,
        .min_stage = XI_STAGE_CORO_LOWERED,
        .max_stage = XI_STAGE_CORO_LOWERED,
        .requires_inv_mask = 0,
        .produces_inv_mask = XI_INV_EVAL_ORDER_FIXED,
    };

    assert(lowered_only.min_stage == lowered_only.max_stage);
    assert(lowered_only.flags & XI_PASS_REQUIRED);
    assert(lowered_only.produces_inv_mask == XI_INV_EVAL_ORDER_FIXED);

    printf("  PASS\n");
}

/* ========== Test 5: Verify accepts IR with stage field ========== */

static void test_verify_with_stage(void) {
    printf("--- test_verify_with_stage ---\n");

    /* Build minimal IR: fn f() -> int { return 42 } */
    XiFunc *f = xi_func_new("verify_stage_fn", &stub_int);
    assert(f != NULL);
    XiBlock *entry = xi_block_new(f);
    assert(entry != NULL);

    f->nparams = 0;
    f->params = NULL;

    XiValue *c42 = xi_const_int(f, entry, 42, &stub_int);
    assert(c42 != NULL);

    xi_block_set_return(entry, c42);

    /* Verify should pass */
    char errbuf[256];
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok) {
        fprintf(stderr, "  verify error: %s\n", errbuf);
    }
    assert(ok && "well-formed IR should pass verification");

    /* Stage should still be RAW (verify doesn't change it) */
    assert(f->stage == XI_STAGE_RAW);

    xi_func_free(f);
    printf("  PASS\n");
}

static void test_backend_lower_preserves_print(void) {
    printf("--- test_backend_lower_preserves_print ---\n");

    XiFunc *f = xi_func_new("backend_print_fn", &stub_void);
    assert(f != NULL);

    XiBlock *entry = xi_block_new(f);
    assert(entry != NULL);

    XiValue *arg = xi_const_int(f, entry, 1, &stub_int);
    assert(arg != NULL);

    XiValue *print = xi_value_new(f, entry, XI_PRINT, &stub_void, 1);
    assert(print != NULL);
    print->args[0] = arg;
    print->flags = xi_op_default_effects(XI_PRINT);
    print->aux_int = 2;

    xi_block_set_return(entry, NULL);
    XiReppedProgram *repped = advance_to_repped(f);
    XiBackendProgram *backend = finish_backend(repped, f);

    assert(f->stage == XI_STAGE_BACKEND);
    assert(print->op == XI_PRINT);
    assert(xi_op_is_backend_legal(print->op));

    xi_func_free(xi_backend_program_release(backend));
    printf("  PASS\n");
}

static void test_backend_lower_preserves_json_field_ops(void) {
    printf("--- test_backend_lower_preserves_json_field_ops ---\n");

    XiFunc *f = xi_func_new("backend_json_field_fn", &stub_void);
    assert(f != NULL);

    XiBlock *entry = xi_block_new(f);
    assert(entry != NULL);

    XiValue *json = xi_value_new(f, entry, XI_OBJECT_NEW, &stub_int, 0);
    assert(json != NULL);
    json->flags = xi_op_default_effects(XI_OBJECT_NEW);

    XiValue *value = xi_const_int(f, entry, 7, &stub_int);
    assert(value != NULL);

    XiValue *init = xi_value_new(f, entry, XI_OBJECT_INIT_F, &stub_void, 2);
    assert(init != NULL);
    init->args[0] = json;
    init->args[1] = value;
    init->flags = xi_op_default_effects(XI_OBJECT_INIT_F);
    init->aux_int = 0;

    XiValue *get = xi_value_new(f, entry, XI_OBJECT_GET_F, &stub_int, 1);
    assert(get != NULL);
    get->args[0] = json;
    get->flags = xi_op_default_effects(XI_OBJECT_GET_F);
    get->aux_int = 0;

    XiValue *set = xi_value_new(f, entry, XI_OBJECT_SET_F, &stub_void, 2);
    assert(set != NULL);
    set->args[0] = json;
    set->args[1] = get;
    set->flags = xi_op_default_effects(XI_OBJECT_SET_F);
    set->aux_int = 0;

    xi_block_set_return(entry, NULL);
    XiReppedProgram *repped = advance_to_repped(f);
    XiBackendProgram *backend = finish_backend(repped, f);

    assert(f->stage == XI_STAGE_BACKEND);
    assert(init->op == XI_OBJECT_INIT_F);
    assert(get->op == XI_OBJECT_GET_F);
    assert(set->op == XI_OBJECT_SET_F);
    assert(xi_op_is_backend_legal(init->op));
    assert(xi_op_is_backend_legal(get->op));
    assert(xi_op_is_backend_legal(set->op));

    xi_func_free(xi_backend_program_release(backend));
    printf("  PASS\n");
}

static void test_backend_lower_preserves_collection_ops(void) {
    printf("--- test_backend_lower_preserves_collection_ops ---\n");

    XiFunc *f = xi_func_new("backend_collection_fn", &stub_void);
    assert(f != NULL);

    XiBlock *entry = xi_block_new(f);
    assert(entry != NULL);

    XiValue *cap = xi_const_int(f, entry, 2, &stub_int);
    assert(cap != NULL);

    XiValue *array_new = xi_value_new(f, entry, XI_ARRAY_NEW, &stub_array, 1);
    assert(array_new != NULL);
    array_new->args[0] = cap;
    array_new->flags = xi_op_default_effects(XI_ARRAY_NEW);
    array_new->array_element_storage = XR_ELEM_I64;

    XiValue *map_new = xi_value_new(f, entry, XI_MAP_NEW, &stub_map, 1);
    assert(map_new != NULL);
    map_new->args[0] = cap;
    map_new->flags = xi_op_default_effects(XI_MAP_NEW);

    XiValue *set_new = xi_value_new(f, entry, XI_SET_NEW, &stub_set, 1);
    assert(set_new != NULL);
    set_new->args[0] = cap;
    set_new->flags = xi_op_default_effects(XI_SET_NEW);

    XiValue *concat = xi_value_new(f, entry, XI_STR_CONCAT, &stub_string, 2);
    assert(concat != NULL);
    concat->args[0] = cap;
    concat->args[1] = cap;
    concat->flags = xi_op_default_effects(XI_STR_CONCAT);

    xi_block_set_return(entry, NULL);
    XiReppedProgram *repped = advance_to_repped(f);
    XiBackendProgram *backend = finish_backend(repped, f);

    assert(f->stage == XI_STAGE_BACKEND);
    assert(array_new->op == XI_ARRAY_NEW);
    assert(map_new->op == XI_MAP_NEW);
    assert(set_new->op == XI_SET_NEW);
    assert(concat->op == XI_STR_CONCAT);
    assert(xi_op_is_backend_legal(array_new->op));
    assert(xi_op_is_backend_legal(map_new->op));
    assert(xi_op_is_backend_legal(set_new->op));
    assert(xi_op_is_backend_legal(concat->op));

    xi_func_free(xi_backend_program_release(backend));
    printf("  PASS\n");
}

static void test_backend_lower_preserves_type_slice_and_range_ops(void) {
    printf("--- test_backend_lower_preserves_type_slice_and_range_ops ---\n");

    XiFunc *f = xi_func_new("backend_type_slice_fn", &stub_void);
    assert(f != NULL);

    XiBlock *entry = xi_block_new(f);
    assert(entry != NULL);

    XiValue *value = xi_const_int(f, entry, 7, &stub_int);
    assert(value != NULL);
    XiValue *start = xi_const_int(f, entry, 0, &stub_int);
    assert(start != NULL);
    XiValue *end = xi_const_int(f, entry, 1, &stub_int);
    assert(end != NULL);

    XiValue *typeof_v = xi_value_new(f, entry, XI_TYPENAME, &stub_string, 1);
    assert(typeof_v != NULL);
    typeof_v->args[0] = value;
    typeof_v->flags = xi_op_default_effects(XI_TYPENAME);

    /* XI_AS is the dynamic type-check operation; numeric conversion belongs to
     * XI_CONVERT and the verifier now rejects an XI_AS without dynamic
     * conversion evidence. Build the checked (non-nullable) form, whose safe
     * bit in aux_int must stay clear to agree with the witness. */
    XiValue *as_v = xi_value_new(f, entry, XI_AS, &stub_string, 1);
    assert(as_v != NULL);
    as_v->args[0] = value;
    as_v->flags = xi_op_default_effects(XI_AS);
    as_v->aux_int = ((int64_t) (uint32_t) 8 << 1);
    as_v->aux = (void *) "string";
    as_v->conversion.kind = XR_CONVERSION_DYNAMIC_CHECKED;

    XiValue *slice_v = xi_value_new(f, entry, XI_SLICE, &stub_array, 3);
    assert(slice_v != NULL);
    slice_v->args[0] = value;
    slice_v->args[1] = start;
    slice_v->args[2] = end;
    slice_v->flags = xi_op_default_effects(XI_SLICE);

    XiValue *range_v = xi_value_new(f, entry, XI_RANGE, &stub_array, 2);
    assert(range_v != NULL);
    range_v->args[0] = start;
    range_v->args[1] = end;
    range_v->flags = xi_op_default_effects(XI_RANGE);

    xi_block_set_return(entry, NULL);
    XiReppedProgram *repped = advance_to_repped(f);
    XiBackendProgram *backend = finish_backend(repped, f);

    assert(f->stage == XI_STAGE_BACKEND);
    assert(typeof_v->op == XI_TYPENAME);
    assert(as_v->op == XI_AS);
    assert(slice_v->op == XI_SLICE);
    assert(range_v->op == XI_RANGE);
    assert(xi_op_is_backend_legal(typeof_v->op));
    assert(xi_op_is_backend_legal(as_v->op));
    assert(xi_op_is_backend_legal(slice_v->op));
    assert(xi_op_is_backend_legal(range_v->op));

    xi_func_free(xi_backend_program_release(backend));
    printf("  PASS\n");
}

static void test_backend_policy_generated_metadata(void) {
    printf("--- test_backend_policy_generated_metadata ---\n");

    assert(xi_op_is_backend_legal(XI_ERR_SET));
    assert(xi_op_is_backend_legal(XI_ERR_RETURN));
    assert(xi_op_is_backend_legal(XI_ERR_CATCH));
    assert(!xi_op_is_backend_legal(XI_EXTRACT));
    assert(!xi_op_is_backend_legal(XI_MULTI_RET));

    assert(!xi_op_is_backend_legal(XI_ITER_NEW));
    assert(!xi_op_is_backend_legal(XI_ITER_NEXT));
    assert(!xi_op_is_backend_legal(XI_ITER_VALID));
    assert(!xi_op_is_backend_legal(XI_REGEX_COMPILE));

    assert(xi_op_backend_rewrite(XI_ITER_NEW) == XI_GEN_BACKEND_REWRITE_BUILTIN);
    assert(strcmp(xi_op_backend_rewrite_name(XI_ITER_NEW), "iter_new") == 0);
    assert(strcmp(xi_op_backend_rewrite_name(XI_ITER_NEXT), "iter_next") == 0);
    assert(strcmp(xi_op_backend_rewrite_name(XI_ITER_VALID), "iter_valid") == 0);
    assert(strcmp(xi_op_backend_rewrite_name(XI_REGEX_COMPILE), "regex_compile") == 0);

    printf("  PASS\n");
}

static void test_backend_lower_rewrites_generated_builtin_ops(void) {
    printf("--- test_backend_lower_rewrites_generated_builtin_ops ---\n");

    XiFunc *f = xi_func_new("backend_generated_rewrite_fn", &stub_void);
    assert(f != NULL);

    XiBlock *entry = xi_block_new(f);
    assert(entry != NULL);

    XiValue *source = xi_const_int(f, entry, 4, &stub_int);
    assert(source != NULL);

    XiValue *iter_new = xi_value_new(f, entry, XI_ITER_NEW, &stub_int, 1);
    assert(iter_new != NULL);
    iter_new->args[0] = source;
    iter_new->flags = xi_op_default_effects(XI_ITER_NEW);

    XiValue *iter_next = xi_value_new(f, entry, XI_ITER_NEXT, &stub_int, 1);
    assert(iter_next != NULL);
    iter_next->args[0] = iter_new;
    iter_next->flags = xi_op_default_effects(XI_ITER_NEXT);

    XiValue *iter_valid = xi_value_new(f, entry, XI_ITER_VALID, &stub_bool, 1);
    assert(iter_valid != NULL);
    iter_valid->args[0] = iter_new;
    iter_valid->flags = xi_op_default_effects(XI_ITER_VALID);

    XiValue *pattern = xi_const_str(f, entry, "x+", &stub_string);
    assert(pattern != NULL);
    XiValue *flags = xi_const_int(f, entry, 0, &stub_int);
    assert(flags != NULL);
    XiValue *regex = xi_value_new(f, entry, XI_REGEX_COMPILE, &stub_int, 2);
    assert(regex != NULL);
    regex->args[0] = pattern;
    regex->args[1] = flags;
    regex->flags = xi_op_default_effects(XI_REGEX_COMPILE);

    xi_block_set_return(entry, NULL);
    XiReppedProgram *repped = advance_to_repped(f);
    XiBackendProgram *backend = finish_backend(repped, f);

    assert(f->stage == XI_STAGE_BACKEND);
    assert_rewritten_builtin(iter_new, "iter_new");
    assert_rewritten_builtin(iter_next, "iter_next");
    assert_rewritten_builtin(iter_valid, "iter_valid");
    assert_rewritten_builtin(regex, "regex_compile");

    xi_func_free(xi_backend_program_release(backend));
    printf("  PASS\n");
}

/* ========== Test 6: Stage monotonicity ========== */

static void test_stage_monotonicity(void) {
    printf("--- test_stage_monotonicity ---\n");

    XiFunc *f = xi_func_new("mono_fn", &stub_void);
    assert(f != NULL);
    XiBlock *entry = xi_block_new(f);
    xi_block_set_return(entry, NULL);
    XiReppedProgram *repped = advance_to_repped(f);
    assert(f->stage == XI_STAGE_REPPED);
    XiBackendProgram *backend = finish_backend(repped, f);
    assert(f->stage == XI_STAGE_BACKEND);

    /* Full ordering check */
    assert(XI_STAGE_RAW < XI_STAGE_CANONICAL);
    assert(XI_STAGE_CANONICAL < XI_STAGE_CLOSED);
    assert(XI_STAGE_CLOSED < XI_STAGE_OWNED);
    assert(XI_STAGE_OWNED < XI_STAGE_SEMANTIC_LOWERED);
    assert(XI_STAGE_SEMANTIC_LOWERED < XI_STAGE_CORO_LOWERED);
    assert(XI_STAGE_CORO_LOWERED < XI_STAGE_OPTIMIZED);
    assert(XI_STAGE_OPTIMIZED < XI_STAGE_SEMANTIC_PLANNED);
    assert(XI_STAGE_SEMANTIC_PLANNED < XI_STAGE_REPPED);
    assert(XI_STAGE_REPPED < XI_STAGE_BACKEND);

    xi_func_free(xi_backend_program_release(backend));
    printf("  PASS\n");
}

static void test_consumed_handle_is_rejected(void) {
    printf("--- test_consumed_handle_is_rejected ---\n");

    XiFunc *f = xi_func_new("consume_once", &stub_void);
    XiBlock *entry = xi_block_new(f);
    xi_block_set_return(entry, NULL);
    char error[256] = {0};
    XiRawProgram *raw = xi_stage_adopt_raw(f, error, sizeof(error));
    assert(raw != NULL);
    XiCanonicalProgram *canonical = xi_program_canonicalize(raw, error, sizeof(error));
    assert(canonical != NULL);

    error[0] = '\0';
    assert(xi_program_canonicalize(raw, error, sizeof(error)) == NULL);
    assert(strstr(error, "wrong-stage") != NULL);

    xi_func_free(xi_canonical_program_release(canonical));
    printf("  PASS\n");
}

static void test_corrupt_stage_contract_is_rejected(void) {
    printf("--- test_corrupt_stage_contract_is_rejected ---\n");

    XiFunc *f = xi_func_new("corrupt_stage", &stub_void);
    XiBlock *entry = xi_block_new(f);
    xi_block_set_return(entry, NULL);
    f->invariant_mask &= ~XI_INV_SSA_WELL_FORMED;

    char error[256] = {0};
    assert(xi_stage_adopt_raw(f, error, sizeof(error)) == NULL);
    assert(error[0] != '\0');
    xi_func_free(f);
    printf("  PASS\n");
}

static void test_lowering_fact_corruption_fails_closed(void) {
    printf("--- test_lowering_fact_corruption_fails_closed ---\n");

    XiFunc *f = xi_func_new("corrupt_lowering_fact", &stub_void);
    XiBlock *entry = xi_block_new(f);
    xi_block_set_return(entry, NULL);
    XiCoroLoweredProgram *lowered = advance_to_coro_lowered(f);
    assert(f->lowering_facts.initialized);

    f->lowering_facts.semantic_ops_lowered = false;
    char error[256] = {0};
    assert(!xi_verify_stage(f, XI_STAGE_CORO_LOWERED, error, sizeof(error)));
    assert(strstr(error, "semantic lowering") != NULL);
    f->lowering_facts.semantic_ops_lowered = true;

    f->lowering_facts.coroutine_required = true;
    f->lowering_facts.coroutine_lowered = false;
    error[0] = '\0';
    assert(!xi_verify_stage(f, XI_STAGE_CORO_LOWERED, error, sizeof(error)));
    assert(strstr(error, "coroutine lowering") != NULL);
    f->lowering_facts.coroutine_lowered = true;

    xi_func_free(xi_coro_lowered_program_release(lowered));
    printf("  PASS\n");
}

static void test_semantic_stage_cannot_skip_coroutine_lowering(void) {
    printf("--- test_semantic_stage_cannot_skip_coroutine_lowering ---\n");

    XiFunc *f = xi_func_new("cannot_skip_coro", &stub_void);
    XiBlock *entry = xi_block_new(f);
    XiValue *yield = xi_value_new(f, entry, XI_YIELD, &stub_void, 0);
    assert(yield != NULL);
    xi_block_set_return(entry, NULL);
    char error[256] = {0};
    XiRawProgram *raw = xi_stage_adopt_raw(f, error, sizeof(error));
    assert(raw != NULL);
    XiCanonicalProgram *canonical = xi_program_canonicalize(raw, error, sizeof(error));
    assert(canonical != NULL);
    xi_pass_close(f);
    XiClosedProgram *closed = xi_program_close(canonical, error, sizeof(error));
    assert(closed != NULL);
    XiOwnedProgram *owned = xi_program_make_owned(closed, error, sizeof(error));
    assert(owned != NULL);
    XiSemanticLoweredProgram *semantic = xi_program_lower_semantics(owned, error, sizeof(error));
    assert(semantic != NULL);

    error[0] = '\0';
    assert(xi_program_finish_optimization((XiCoroLoweredProgram *) semantic, error,
                                          sizeof(error)) == NULL);
    assert(strstr(error, "wrong-stage") != NULL);
    assert(f->stage == XI_STAGE_SEMANTIC_LOWERED);

    uint32_t blocks_before = f->nblocks;
    XiCoroLoweredProgram *coro = xi_program_lower_coroutines(semantic, NULL, error, sizeof(error));
    assert(coro != NULL);
    assert(f->stage == XI_STAGE_CORO_LOWERED);
    assert(f->coro_plan != NULL && f->coro_plan->cfg_rewritten);
    assert(f->coro_plan->nstates == 1 && f->nblocks == blocks_before + 2);
    assert(f->coro_plan->dispatch[1].target == f->coro_plan->points[0].suspend_block);
    assert(f->coro_plan->actions_materialized);
    assert(xi_coro_plan_is_current(f, f->coro_plan));
    assert(xi_verify_stage(f, XI_STAGE_CORO_LOWERED, error, sizeof(error)));
    xi_func_free(xi_coro_lowered_program_release(coro));
    printf("  PASS\n");
}

static void test_coroutine_transition_accepts_open_callable(void) {
    printf("--- test_coroutine_transition_accepts_open_callable ---\n");

    XiFunc *f = xi_func_new("unresolved_coro_transition", &stub_int);
    XiBlock *entry = xi_block_new(f);
    XiValue *callee = xi_value_new(f, entry, XI_CONST, &stub_function, 0);
    XiValue *call = xi_value_new(f, entry, XI_CALL, &stub_int, 1);
    assert(callee && call);
    call->args[0] = callee;
    call->flags |= XI_FLAG_MAY_SUSPEND;
    xi_block_set_return(entry, call);

    char error[256] = {0};
    XiRawProgram *raw = xi_stage_adopt_raw(f, error, sizeof(error));
    assert(raw != NULL);
    XiCanonicalProgram *canonical = xi_program_canonicalize(raw, error, sizeof(error));
    assert(canonical != NULL);
    xi_pass_close(f);
    XiClosedProgram *closed = xi_program_close(canonical, error, sizeof(error));
    assert(closed != NULL);
    XiOwnedProgram *owned = xi_program_make_owned(closed, error, sizeof(error));
    assert(owned != NULL);
    XiSemanticLoweredProgram *semantic = xi_program_lower_semantics(owned, error, sizeof(error));
    assert(semantic != NULL);

    uint32_t blocks_before = f->nblocks;
    uint32_t values_before = entry->nvalues;
    uint64_t ir_revision_before = f->ir_revision;
    uint64_t cfg_revision_before = f->cfg_version;
    error[0] = '\0';
    XiCoroLoweredProgram *lowered =
        xi_program_lower_coroutines(semantic, NULL, error, sizeof(error));
    assert(lowered != NULL);
    assert(f->stage == XI_STAGE_CORO_LOWERED);
    assert(f->nblocks == blocks_before + 2 && entry->nvalues < values_before);
    assert(f->ir_revision > ir_revision_before && f->cfg_version > cfg_revision_before);
    assert(f->coro_plan != NULL && f->coro_plan->nstates == 1);
    assert(f->coro_plan->points[0].op == call);
    assert(f->coro_plan->points[0].resolved_callee == NULL);
    assert(f->coro_plan->points[0].edges[XI_CORO_EDGE_CHILD].indirect_child);
    assert(xi_coro_plan_is_current(f, f->coro_plan));

    xi_func_free(xi_coro_lowered_program_release(lowered));
    printf("  PASS\n");
}

static XiSemanticLoweredProgram *make_string_builder_call_program(XrType *receiver_type,
                                                                  bool with_yield,
                                                                  XiFunc **out_func, char *error,
                                                                  size_t error_size) {
    XiFunc *f = xi_func_new("string_builder_coro_transition", &stub_void);
    XiBlock *entry = xi_block_new(f);
    XiValue *receiver = xi_value_new(f, entry, XI_PARAM, receiver_type, 0);
    XiValue *argument = xi_value_new(f, entry, XI_CONST, &stub_string, 0);
    XiValue *append = xi_value_new(f, entry, XI_CALL_METHOD, receiver_type, 2);
    XiValue *yield = with_yield ? xi_value_new(f, entry, XI_YIELD, &stub_void, 0) : NULL;
    assert(f && entry && receiver && argument && append && (!with_yield || yield));
    append->aux = (void *) "append";
    append->args[0] = receiver;
    append->args[1] = argument;
    xi_block_set_return(entry, NULL);

    XiRawProgram *raw = xi_stage_adopt_raw(f, error, error_size);
    assert(raw != NULL);
    XiCanonicalProgram *canonical = xi_program_canonicalize(raw, error, error_size);
    assert(canonical != NULL);
    xi_pass_close(f);
    XiClosedProgram *closed = xi_program_close(canonical, error, error_size);
    assert(closed != NULL);
    XiOwnedProgram *owned = xi_program_make_owned(closed, error, error_size);
    assert(owned != NULL);
    XiSemanticLoweredProgram *semantic = xi_program_lower_semantics(owned, error, error_size);
    assert(semantic != NULL);
    *out_func = f;
    return semantic;
}

static void test_coroutine_transition_uses_builtin_string_builder_identity(void) {
    printf("--- test_coroutine_transition_uses_builtin_string_builder_identity ---\n");

    char error[256] = {0};
    XiFunc *builtin_func = NULL;
    XiSemanticLoweredProgram *builtin = make_string_builder_call_program(
        &stub_string_builder, true, &builtin_func, error, sizeof(error));
    XiCoroLoweredProgram *lowered =
        xi_program_lower_coroutines(builtin, NULL, error, sizeof(error));
    assert(lowered != NULL);
    assert(builtin_func->stage == XI_STAGE_CORO_LOWERED);
    assert(builtin_func->coro_plan != NULL && builtin_func->coro_plan->cfg_rewritten);
    xi_func_free(xi_coro_lowered_program_release(lowered));

    XiFunc *shadow_func = NULL;
    XiSemanticLoweredProgram *shadow = make_string_builder_call_program(
        &stub_shadow_string_builder, true, &shadow_func, error, sizeof(error));
    uint32_t blocks_before = shadow_func->nblocks;
    error[0] = '\0';
    assert(xi_program_lower_coroutines(shadow, NULL, error, sizeof(error)) == NULL);
    assert(strstr(error, "failed closed") != NULL);
    assert(shadow_func->stage == XI_STAGE_SEMANTIC_LOWERED);
    assert(shadow_func->nblocks == blocks_before && shadow_func->coro_plan == NULL);
    xi_func_free(xi_semantic_lowered_program_release(shadow));

    printf("  PASS\n");
}

static int unknown_call_suspendability(void *ud, const XiFunc *current, const XiValue *call) {
    (void) ud;
    (void) current;
    (void) call;
    return -1;
}

static void test_coroutine_transition_keeps_known_builtin_sync(void) {
    printf("--- test_coroutine_transition_keeps_known_builtin_sync ---\n");

    char error[256] = {0};
    XiFunc *func = NULL;
    XiSemanticLoweredProgram *semantic =
        make_string_builder_call_program(&stub_string_builder, false, &func, error, sizeof(error));
    XiCoroResolver resolver = {0};
    resolver.call_suspendability = unknown_call_suspendability;
    XiCoroLoweredProgram *lowered =
        xi_program_lower_coroutines(semantic, &resolver, error, sizeof(error));
    assert(lowered != NULL);
    assert(func->coro_plan != NULL && !func->coro_plan->is_coroutine);
    assert(func->coro_plan->nstates == 0);
    xi_func_free(xi_coro_lowered_program_release(lowered));

    printf("  PASS\n");
}

static void test_coroutine_transition_rejects_invalid_cfg_before_rewrite(void) {
    printf("--- test_coroutine_transition_rejects_invalid_cfg_before_rewrite ---\n");

    XiFunc *f = xi_func_new("invalid_cfg_coro_transition", &stub_void);
    XiBlock *entry = xi_block_new(f);
    XiValue *yield = xi_value_new(f, entry, XI_YIELD, &stub_void, 0);
    assert(yield != NULL);
    xi_block_set_return(entry, NULL);

    char error[256] = {0};
    XiRawProgram *raw = xi_stage_adopt_raw(f, error, sizeof(error));
    assert(raw != NULL);
    XiCanonicalProgram *canonical = xi_program_canonicalize(raw, error, sizeof(error));
    assert(canonical != NULL);
    xi_pass_close(f);
    XiClosedProgram *closed = xi_program_close(canonical, error, sizeof(error));
    assert(closed != NULL);
    XiOwnedProgram *owned = xi_program_make_owned(closed, error, sizeof(error));
    assert(owned != NULL);
    XiSemanticLoweredProgram *semantic = xi_program_lower_semantics(owned, error, sizeof(error));
    assert(semantic != NULL);

    entry->kind = XI_BLOCK_IF;
    uint32_t blocks_before = f->nblocks;
    uint64_t ir_revision_before = f->ir_revision;
    uint64_t cfg_revision_before = f->cfg_version;
    error[0] = '\0';
    assert(xi_program_lower_coroutines(semantic, NULL, error, sizeof(error)) == NULL);
    assert(error[0] != '\0');
    assert(f->stage == XI_STAGE_SEMANTIC_LOWERED);
    assert(f->nblocks == blocks_before);
    assert(f->ir_revision == ir_revision_before && f->cfg_version == cfg_revision_before);
    assert(f->coro_plan == NULL);

    entry->kind = XI_BLOCK_RETURN;
    xi_func_free(xi_semantic_lowered_program_release(semantic));
    printf("  PASS\n");
}

static void test_semantic_intrinsic_corruption_fails_closed(void) {
    printf("--- test_semantic_intrinsic_corruption_fails_closed ---\n");

    XiFunc *f = xi_func_new("corrupt_semantic_intrinsic", &stub_array);
    XiBlock *entry = xi_block_new(f);
    XiValue *lhs = xi_param(f, entry, 0, &stub_array);
    XiValue *rhs = xi_param(f, entry, 1, &stub_array);
    XiValue *vec = xi_value_new(f, entry, XI_VEC_WIDEN_MUL, &stub_array, 2);
    vec->args[0] = lhs;
    vec->args[1] = rhs;
    vec->aux_int = xi_vec_shape_encode(XR_NATIVE_U64, 2) | XI_VEC_SHAPE_ODD_LANES;
    vec->xa_intrinsic_id = 999999;
    xi_block_set_return(entry, vec);

    char error[512] = {0};
    XiRawProgram *raw = xi_stage_adopt_raw(f, error, sizeof(error));
    assert(raw != NULL);
    XiCanonicalProgram *canonical = xi_program_canonicalize(raw, error, sizeof(error));
    assert(canonical != NULL);
    xi_pass_close(f);
    XiClosedProgram *closed = xi_program_close(canonical, error, sizeof(error));
    assert(closed != NULL);
    XiOwnedProgram *owned = xi_program_make_owned(closed, error, sizeof(error));
    assert(owned != NULL);

    error[0] = '\0';
    assert(xi_program_lower_semantics(owned, error, sizeof(error)) == NULL);
    assert(strstr(error, "unknown canonical intrinsic") != NULL);
    assert(f->stage == XI_STAGE_OWNED);

    vec->xa_intrinsic_id = XA_INTRINSIC_SIMD_U32X4_WIDEN_MUL_ODD;
    XiSemanticLoweredProgram *lowered = xi_program_lower_semantics(owned, error, sizeof(error));
    assert(lowered != NULL);

    vec->op = XI_VEC_ADD;
    error[0] = '\0';
    assert(!xi_verify_stage(f, XI_STAGE_SEMANTIC_LOWERED, error, sizeof(error)));
    assert(strstr(error, "requires Xi op") != NULL);
    vec->op = XI_VEC_WIDEN_MUL;

    vec->xa_intrinsic_id = XA_INTRINSIC_NONE;
    error[0] = '\0';
    assert(!xi_verify_stage(f, XI_STAGE_SEMANTIC_LOWERED, error, sizeof(error)));
    assert(strstr(error, "has no intrinsic identity") != NULL);

    xi_func_free(xi_semantic_lowered_program_release(lowered));
    printf("  PASS\n");
}

static void test_pass_order_and_invariants(void) {
    printf("--- test_pass_order_and_invariants ---\n");

    assert(xi_pass_order_check());

    XiFunc *f = xi_func_new("pass_inv_fn", &stub_int);
    assert(f != NULL);

    XiBlock *entry = xi_block_new(f);
    assert(entry != NULL);

    XiValue *c42 = xi_const_int(f, entry, 42, &stub_int);
    assert(c42 != NULL);
    xi_block_set_return(entry, c42);

    XiCoroLoweredProgram *lowered = advance_to_coro_lowered(f);

    XiOptResult opt = xi_opt_run_pipeline(f, XI_OPT_FULL);
    assert(opt.ok);

    assert(xi_evidence_domain_is_current(f, XI_EVD_ALIAS));
    assert(xi_evidence_domain_is_current(f, XI_EVD_RANGE));

    char error[256] = {0};
    XiOptimizedProgram *optimized = xi_program_finish_optimization(lowered, error, sizeof(error));
    assert(optimized != NULL);
    xi_func_free(xi_optimized_program_release(optimized));
    printf("  PASS\n");
}

static void test_optimizer_invariant_failure_is_data(void) {
    printf("--- test_optimizer_invariant_failure_is_data ---\n");

    XiFunc *f = xi_func_new("invalid_pass_input", &stub_int);
    assert(f != NULL);
    XiBlock *entry = xi_block_new(f);
    assert(entry != NULL);
    XiValue *value = xi_const_int(f, entry, 1, &stub_int);
    xi_block_set_return(entry, value);
    f->stage = XI_STAGE_CLOSED;
    f->invariant_mask = 0;

    XiOptResult opt = xi_opt_run_pipeline(f, XI_OPT_FULL);
    assert(!opt.ok);
    assert(opt.detail[0] != '\0');

    xi_func_free(f);
    printf("  PASS\n");
}

static void attach_only_child(XiFunc *parent, XiFunc *child) {
    parent->children = (XiFunc **) xr_calloc(1, sizeof(XiFunc *));
    assert(parent->children != NULL);
    parent->children[0] = child;
    parent->nchildren = 1;
    parent->children_cap = 1;
    child->parent_func = parent;
}

static void set_single_source_var(XiFunc *func, const char *name, XrType *type) {
    func->source_var_names =
        (const char **) xi_func_arena_alloc(func, sizeof(*func->source_var_names));
    func->source_var_types = (XrType **) xi_func_arena_alloc(func, sizeof(*func->source_var_types));
    assert(func->source_var_names && func->source_var_types);
    func->source_var_count = 1;
    func->source_var_names[0] = name;
    func->source_var_types[0] = type;
}

static void test_close_materializes_first_class_capture_cells(void) {
    printf("--- test_close_materializes_first_class_capture_cells ---\n");

    XiFunc *root = xi_func_new("cell_root", &stub_function);
    XiFunc *middle = xi_func_new("cell_middle", &stub_int);
    XiFunc *leaf = xi_func_new("cell_leaf", &stub_int);
    assert(root && middle && leaf);
    set_single_source_var(root, "value", &stub_int);
    attach_only_child(root, middle);
    attach_only_child(middle, leaf);

    XiBlock *root_entry = xi_block_new(root);
    XiValue *first = xi_const_int(root, root_entry, 1, &stub_int);
    XiValue *second = xi_const_int(root, root_entry, 2, &stub_int);
    first->var_id = 0;
    second->var_id = 0;

    middle->captures[0] = (XiCapture) {
        .source = XI_CAPTURE_SRC_REG,
        .needs_cell = true,
        .is_mutable = true,
        .type = &stub_int,
        .value = first,
        .name = "value",
    };
    middle->ncaptures = 1;
    XiValue *middle_closure = xi_value_new(root, root_entry, XI_CLOSURE_NEW, &stub_function, 1);
    middle_closure->aux = middle;
    middle_closure->args[0] = first;
    XiValue *local_read = xi_value_new(root, root_entry, XI_COPY, &stub_int, 1);
    local_read->args[0] = second;
    local_read->aux_int = XI_COPY_KIND_CELL_READ;
    local_read->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_READS_MEM;
    xi_block_set_return(root_entry, middle_closure);

    XiBlock *middle_entry = xi_block_new(middle);
    XiValue *middle_load = xi_value_new(middle, middle_entry, XI_LOAD_UPVAL, &stub_int, 0);
    middle_load->aux_int = 0;
    XiValue *replacement = xi_const_int(middle, middle_entry, 3, &stub_int);
    XiValue *middle_store = xi_value_new(middle, middle_entry, XI_STORE_UPVAL, &stub_void, 1);
    middle_store->aux_int = 0;
    middle_store->args[0] = replacement;

    leaf->captures[0] = (XiCapture) {
        .source = XI_CAPTURE_SRC_UPVAL,
        .index = 0,
        .needs_cell = true,
        .is_mutable = true,
        .type = &stub_int,
        .name = "value",
    };
    leaf->ncaptures = 1;
    XiValue *leaf_closure = xi_value_new(middle, middle_entry, XI_CLOSURE_NEW, &stub_function, 1);
    leaf_closure->aux = leaf;
    leaf_closure->args[0] = NULL;
    xi_block_set_return(middle_entry, middle_load);

    XiBlock *leaf_entry = xi_block_new(leaf);
    XiValue *leaf_load = xi_value_new(leaf, leaf_entry, XI_LOAD_UPVAL, &stub_int, 0);
    leaf_load->aux_int = 0;
    xi_block_set_return(leaf_entry, leaf_load);

    char error[512] = {0};
    XiRawProgram *raw = xi_stage_adopt_raw(root, error, sizeof(error));
    if (!raw)
        fprintf(stderr, "first-class capture raw adoption failed: %s\n", error);
    assert(raw != NULL);
    XiCanonicalProgram *canonical = xi_program_canonicalize(raw, error, sizeof(error));
    assert(canonical != NULL);
    xi_pass_close(root);
    XiClosedProgram *closed = xi_program_close(canonical, error, sizeof(error));
    if (!closed)
        fprintf(stderr, "cell close failed: %s\n", error);
    assert(closed != NULL);

    XiValue *cell = middle_closure->args[0];
    assert(cell && cell->op == XI_CELL_NEW);
    assert(middle->captures[0].value == cell);
    assert(local_read->op == XI_CELL_GET && local_read->args[0] == cell);
    unsigned cell_news = 0;
    unsigned cell_sets = 0;
    for (uint32_t i = 0; i < root_entry->nvalues; i++) {
        cell_news += root_entry->values[i]->op == XI_CELL_NEW;
        cell_sets += root_entry->values[i]->op == XI_CELL_SET;
    }
    assert(cell_news == 1);
    assert(cell_sets == 2);

    assert(middle_load->op == XI_CELL_GET);
    assert(middle_store->op == XI_CELL_SET);
    assert(leaf->captures[0].source == XI_CAPTURE_SRC_REG);
    assert(leaf_closure->args[0] != NULL);
    assert(leaf_closure->args[0]->op == XI_LOAD_UPVAL);
    assert(leaf->captures[0].value == leaf_closure->args[0]);
    assert(leaf_load->op == XI_CELL_GET);
    assert(xi_verify_stage(root, XI_STAGE_CLOSED, error, sizeof(error)));
    assert(xi_verify_stage(middle, XI_STAGE_CLOSED, error, sizeof(error)));
    assert(xi_verify_stage(leaf, XI_STAGE_CLOSED, error, sizeof(error)));

    /* One closure captures this cell, so it is the sole consumer and takes the
     * +1 that CELL_NEW produced -- no retain, and no release by the frame that
     * gave it away. RC contract C1 makes every closure capture of a cell "an
     * ordinary consume/retain path" and forbids a cell-specific release
     * exception; demanding a retain from a sole consume would be exactly that
     * exception. The multi-consumer half of C1, where each consumer past the
     * first must retain, is covered end-to-end by
     * tests/diff/cases/semantics/closure/shared_mutable_capture_cell.xr. */
    xi_arc_insert(root);
    unsigned cell_retains = 0;
    unsigned cell_releases = 0;
    for (uint32_t i = 0; i < root_entry->nvalues; i++) {
        XiValue *value = root_entry->values[i];
        if (value->nargs == 1 && value->args[0] == cell) {
            cell_retains += value->op == XI_RETAIN;
            cell_releases += value->op == XI_RELEASE;
        }
    }
    assert(cell_retains == 0);
    assert(cell_releases == 0);

    xi_func_free(xi_closed_program_release(closed));
    printf("  PASS\n");
}

static void attach_two_children(XiFunc *parent, XiFunc *a, XiFunc *b) {
    parent->children = (XiFunc **) xr_calloc(2, sizeof(XiFunc *));
    assert(parent->children != NULL);
    parent->children[0] = a;
    parent->children[1] = b;
    parent->nchildren = 2;
    parent->children_cap = 2;
    a->parent_func = parent;
    b->parent_func = parent;
}

/* Two captures of one variable, both asking for a cell, must land on ONE cell
 * rather than one per closure. This covers the close pass only: the captures
 * here are hand-marked needs_cell, so the lowering decision that produces that
 * mark is not exercised -- that half lives in
 * tests/diff/cases/semantics/closure/shared_mutable_capture_cell.xr, whose
 * .xr.expected oracle is what catches it, since the two-cell bug printed the
 * same wrong number on both backends and the differential net stayed quiet. */
static void test_close_shares_one_cell_across_closures(void) {
    printf("--- test_close_shares_one_cell_across_closures ---\n");

    XiFunc *root = xi_func_new("shared_root", &stub_function);
    XiFunc *writer = xi_func_new("shared_writer", &stub_int);
    XiFunc *reader = xi_func_new("shared_reader", &stub_int);
    assert(root && writer && reader);
    set_single_source_var(root, "shared", &stub_int);
    attach_two_children(root, writer, reader);

    XiBlock *root_entry = xi_block_new(root);
    XiValue *initial = xi_const_int(root, root_entry, 0, &stub_int);
    initial->var_id = 0;

    const XiCapture shared = {
        .source = XI_CAPTURE_SRC_REG,
        .needs_cell = true,
        .is_mutable = true,
        .type = &stub_int,
        .value = initial,
        .name = "shared",
    };
    writer->captures[0] = shared;
    writer->ncaptures = 1;
    reader->captures[0] = shared;
    reader->ncaptures = 1;

    XiValue *writer_closure = xi_value_new(root, root_entry, XI_CLOSURE_NEW, &stub_function, 1);
    writer_closure->aux = writer;
    writer_closure->args[0] = initial;
    XiValue *reader_closure = xi_value_new(root, root_entry, XI_CLOSURE_NEW, &stub_function, 1);
    reader_closure->aux = reader;
    reader_closure->args[0] = initial;
    xi_block_set_return(root_entry, writer_closure);

    XiBlock *writer_entry = xi_block_new(writer);
    XiValue *writer_replacement = xi_const_int(writer, writer_entry, 1, &stub_int);
    XiValue *writer_store = xi_value_new(writer, writer_entry, XI_STORE_UPVAL, &stub_void, 1);
    writer_store->aux_int = 0;
    writer_store->args[0] = writer_replacement;
    xi_block_set_return(writer_entry, writer_replacement);

    XiBlock *reader_entry = xi_block_new(reader);
    XiValue *reader_load = xi_value_new(reader, reader_entry, XI_LOAD_UPVAL, &stub_int, 0);
    reader_load->aux_int = 0;
    xi_block_set_return(reader_entry, reader_load);

    char error[512] = {0};
    XiRawProgram *raw = xi_stage_adopt_raw(root, error, sizeof(error));
    assert(raw != NULL);
    XiCanonicalProgram *canonical = xi_program_canonicalize(raw, error, sizeof(error));
    assert(canonical != NULL);
    xi_pass_close(root);
    XiClosedProgram *closed = xi_program_close(canonical, error, sizeof(error));
    if (!closed)
        fprintf(stderr, "shared cell close failed: %s\n", error);
    assert(closed != NULL);

    XiValue *cell = writer_closure->args[0];
    assert(cell && cell->op == XI_CELL_NEW);
    assert(reader_closure->args[0] == cell);
    assert(writer->captures[0].value == cell);
    assert(reader->captures[0].value == cell);

    /* Exactly one cell, not one per capturing closure. */
    unsigned cell_news = 0;
    for (uint32_t i = 0; i < root_entry->nvalues; i++)
        cell_news += root_entry->values[i]->op == XI_CELL_NEW;
    assert(cell_news == 1);

    assert(writer_store->op == XI_CELL_SET);
    assert(reader_load->op == XI_CELL_GET);
    assert(xi_verify_stage(root, XI_STAGE_CLOSED, error, sizeof(error)));
    assert(xi_verify_stage(writer, XI_STAGE_CLOSED, error, sizeof(error)));
    assert(xi_verify_stage(reader, XI_STAGE_CLOSED, error, sizeof(error)));

    xi_func_free(xi_closed_program_release(closed));
    printf("  PASS\n");
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi IR Stage Contract Tests ===\n\n");

    test_stage_enum();
    test_func_stage_default();
    test_stage_after_lowering();
    test_pass_desc_fields();
    test_verify_with_stage();
    test_backend_lower_preserves_print();
    test_backend_lower_preserves_json_field_ops();
    test_backend_lower_preserves_collection_ops();
    test_backend_lower_preserves_type_slice_and_range_ops();
    test_backend_policy_generated_metadata();
    test_backend_lower_rewrites_generated_builtin_ops();
    test_stage_monotonicity();
    test_consumed_handle_is_rejected();
    test_corrupt_stage_contract_is_rejected();
    test_lowering_fact_corruption_fails_closed();
    test_semantic_stage_cannot_skip_coroutine_lowering();
    test_coroutine_transition_accepts_open_callable();
    test_coroutine_transition_uses_builtin_string_builder_identity();
    test_coroutine_transition_keeps_known_builtin_sync();
    test_coroutine_transition_rejects_invalid_cfg_before_rewrite();
    test_semantic_intrinsic_corruption_fails_closed();
    test_pass_order_and_invariants();
    test_optimizer_invariant_failure_is_data();
    test_close_materializes_first_class_capture_cells();
    test_close_shares_one_cell_across_closures();

    printf("\n=== All stage contract tests passed ===\n");
    return 0;
}
