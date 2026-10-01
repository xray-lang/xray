/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_callable52_constructor_artifact.c - Real Source constructor authority
 */

#include "../test_framework.h"
#include "target_profile_test_fixture.h"
#include "xray_vm.h"
#include "base/xmalloc.h"
#include "frontend/analyzer/xanalyzer.h"
#include "ir/xi.h"
#include "ir/xi_module.h"
#include "module/xmodule.h"
#include "module/xmodule_graph.h"
#include "module/xmodule_identity.h"
#include "plan/format/xr_xsm_schema.h"
#include "plan/ownership/xr_ownership_certificate_internal.h"
#include "plan/semantic/xr_semantic_constructor_callable_shape.h"
#include "plan/semantic/xr_semantic_coroutine_module_shape.h"
#include "plan/semantic/xr_semantic_plan_internal.h"
#include "plan/semantic/xr_semantic_verify.h"
#include "runtime/value/xchunk.h"
#include "runtime/xisolate_api.h"
#include "toolchain/xcompiler_session.h"

typedef struct ConstructorSourceFixture {
    XrVMRuntime *isolate;
    XrCompilerSession *session;
    XrModuleGraph *graph;
    XaAnalyzer *analyzer;
    XrCompiledModuleGraph compiled;
    XrCompilerSessionOperationScope operation_scope;
    XrProto *entry;
    const XrSemanticPlan **dependencies;
    uint32_t dependency_count;
} ConstructorSourceFixture;

static bool constructor_source_init(ConstructorSourceFixture *fixture) {
    XrVMConfig config = {0};
    fixture->isolate = xray_vm_new_full(&config);
    if (!fixture->isolate)
        return false;
    fixture->session = xr_compiler_session_current_for_isolate(fixture->isolate);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    bool ok = profile && xr_compiler_session_set_target_profile(fixture->session, profile);
    xr_target_profile_free(profile);
    ok = ok && xr_compiler_session_operation_begin(fixture->session, &fixture->operation_scope);
    XrModuleRegistry *registry = xr_isolate_get_module_registry(fixture->isolate);
    fixture->graph = xr_module_graph_new(fixture->session, xr_module_registry_get_resolver(registry));
    fixture->analyzer = xa_analyzer_new(fixture->session);
    const XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_MEMORY, .namespace_id = "constructor52-artifact-v1"};
    char *error = NULL;
    ok = ok && fixture->graph && fixture->analyzer &&
         xr_module_graph_build_source(fixture->graph, &authority,
             "import parallel\nvar plan = parallel.Plan<i64>(parallel.Options(4), (lane) -> lane)\n",
             &error) == 0;
    xr_free(error);
    if (!ok)
        return false;
    xr_module_graph_topological_sort(fixture->graph);
    if (fixture->graph->has_cycle)
        return false;
    xa_analyzer_set_graph(fixture->analyzer, fixture->graph);
    for (int t = 0; t < fixture->graph->topo_count; t++) {
        XrModuleSpec *spec = &fixture->graph->specs[fixture->graph->topo_order[t]];
        if (!spec->ast)
            continue;
        const char *file = spec->source_path ? spec->source_path : spec->canonical;
        xa_analyzer_analyze(fixture->analyzer, file, spec->ast);
        spec->export_symbols = xa_analyzer_collect_export_symbols(fixture->analyzer, spec->ast);
        if (xa_analyzer_print_errors(fixture->analyzer, file) != 0)
            return false;
        xa_analyzer_clear_diagnostics(fixture->analyzer);
    }
    xr_compiler_session_set_module_graph(fixture->session, fixture->graph);
    if (!xr_compile_module_graph_dependencies(fixture->session, fixture->analyzer,
                                               fixture->graph, &fixture->compiled))
        return false;
    XrModuleSpec *entry = &fixture->graph->specs[fixture->graph->entry_index];
    XiModule *entry_module = NULL;
    fixture->entry = xr_compile_ast_in_graph(fixture->session, fixture->analyzer, entry->ast,
        entry->canonical, fixture->graph, fixture->compiled.modules, fixture->compiled.count,
        &entry_module, &authority);
    if (!fixture->entry)
        return false;
    const XrSemanticPlan *plan = ((XiFunc *) fixture->entry->xi_func)->semantic_plan;
    fixture->dependency_count = xr_semantic_plan_dependency_count(plan);
    fixture->dependencies = xr_calloc(fixture->dependency_count, sizeof(*fixture->dependencies));
    if (fixture->dependency_count && !fixture->dependencies)
        return false;
    for (uint32_t d = 0; d < fixture->dependency_count; d++) {
        const XrSemanticDependencyRecord *required = xr_semantic_plan_dependency(plan, d);
        for (int m = 0; m < fixture->compiled.count; m++) {
            const XiModule *module = fixture->compiled.modules[m];
            const XrSemanticPlan *dependency = module && module->init ? module->init->semantic_plan : NULL;
            if (!dependency || !module->identity || strcmp(module->identity, required->module_path) != 0 ||
                !xr_fingerprint_equal(required->semantic_fingerprint, xr_semantic_plan_fingerprint(dependency)))
                continue;
            if (fixture->dependencies[d])
                return false;
            fixture->dependencies[d] = dependency;
        }
        if (!fixture->dependencies[d])
            return false;
    }
    xr_compiler_session_set_module_graph(fixture->session, NULL);
    return xr_compiler_session_operation_succeed(&fixture->operation_scope);
}

static void constructor_source_destroy(ConstructorSourceFixture *fixture) {
    xr_free(fixture->dependencies);
    xr_free_code(fixture->isolate, fixture->entry);
    xr_compiler_session_set_module_graph(fixture->session, NULL);
    xr_compiled_module_graph_dispose(&fixture->compiled);
    xa_analyzer_set_graph(fixture->analyzer, NULL);
    xa_analyzer_free(fixture->analyzer);
    xr_module_graph_free(fixture->graph);
    xray_vm_delete(fixture->isolate);
}

XR_FUNC bool probe_unchecked_xsm_encode(const XrSemanticPlan *plan, uint8_t **bytes,
                                       size_t *size, char *error, size_t error_size);

static void require_rehashed_rejection(XrSemanticPlan *plan,
                                        const ConstructorSourceFixture *fixture,
                                        const char *label, const char *expected_error) {
    XrFingerprint before = plan->fingerprint;
    XrFingerprint premise = plan->ownership->semantic_fingerprint;
    XrFingerprint certificate = plan->ownership->fingerprint;
    xr_semantic_plan_compute_fingerprint(plan, &plan->fingerprint);
    plan->ownership->semantic_fingerprint = plan->fingerprint;
    plan->ownership->fingerprint = plan->fingerprint;
    uint8_t *wire = NULL;
    size_t size = 0;
    char error[512] = {0};
    if (!probe_unchecked_xsm_encode(plan, &wire, &size, error, sizeof(error))) {
        fprintf(stderr, "unchecked encoding failed: %s\n", error);
        abort();
    }
    XrSemanticPlan *decoded = NULL;
    bool accepted = xr_xsm_decode_module_set(wire, size, fixture->dependencies,
        fixture->dependency_count, &decoded, error, sizeof(error));
    if (accepted || decoded || !strstr(error, expected_error)) {
        fprintf(stderr, "invalid rejection %s: accepted=%u %s\n", label, accepted, error);
        abort();
    }
    printf("real Source rehashed %s: %s\n", label, error);
    fflush(stdout);
    xr_free(wire);
    plan->fingerprint = before;
    plan->ownership->semantic_fingerprint = premise;
    plan->ownership->fingerprint = certificate;
}

static void constructor_key_replace_id(char *key, const char *name, XrStableId id) {
    char *field = strstr(key, name);
    if (!field)
        abort();
    char hex[33];
    xr_stable_id_hex(id, hex);
    memcpy(field + strlen(name), hex, 32);
}

static void constructor_rehashed_missing_state(XrSemanticPlan *plan,
                                                const ConstructorSourceFixture *fixture,
                                                uint32_t operation) {
    XrSemanticEntityRecord *original = plan->entities;
    uint32_t original_count = plan->entity_count;
    XrSemanticEntityRecord *entities = xr_malloc((size_t) original_count * sizeof(*entities));
    if (!entities)
        abort();
    uint32_t kept = 0, removed = 0, index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < original_count; i++) {
        if (original[i].kind == XR_SEM_ENTITY_COROUTINE_STATE && original[i].subject == operation) {
            removed++;
            index = i;
        } else {
            entities[kept++] = original[i];
        }
    }
    if (removed != 1)
        abort();
    for (uint32_t i = 0; i < kept; i++) {
        if (entities[i].parent == index)
            abort();
        if (entities[i].parent != XR_SEMANTIC_INDEX_NONE && entities[i].parent > index)
            entities[i].parent--;
    }
    plan->entities = entities;
    plan->entity_count = kept;
    require_rehashed_rejection(plan, fixture, "missing conservative constructor state",
                                "module-set coroutine state disagrees");
    plan->entities = original;
    plan->entity_count = original_count;
    xr_free(entities);
}

TEST(real_source_constructor_and_rehashed_authority_rejections) {
    ConstructorSourceFixture fixture = {0};
    ASSERT_TRUE(constructor_source_init(&fixture));
    XrSemanticPlan *plan = (XrSemanticPlan *) ((XiFunc *) fixture.entry->xi_func)->semantic_plan;
    char error[512] = {0};
    ASSERT_TRUE(xr_semantic_plan_verify_module_set(plan, fixture.dependencies,
        fixture.dependency_count, error, sizeof(error)));
    uint32_t selected = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->call_target_count; i++) {
        const XrSemanticCallTargetRecord *target = &plan->call_targets[i];
        const XrSemanticOperationRecord *operation = &plan->operations[target->operation];
        if (target->kind == XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR && operation->operand_count == 3)
            selected = i;
    }
    ASSERT_TRUE(selected != XR_SEMANTIC_INDEX_NONE);
    XrSemanticCallTargetRecord *target = &plan->call_targets[selected];
    const XrSemanticOperationRecord *construction = &plan->operations[target->operation];
    ASSERT_EQ_INT(construction->opcode, XI_CALL);
    ASSERT_EQ_INT(construction->metadata_count, 0);
    uint32_t callback_value = plan->operands[construction->operand_begin + 2u].value;
    XrSemanticOperationRecord *closure = NULL;
    for (uint32_t i = 0; i < plan->operation_count; i++)
        if (plan->operations[i].result_value == callback_value)
            closure = &plan->operations[i];
    ASSERT_NOT_NULL(closure);
    ASSERT_EQ_INT(closure->opcode, XI_CLOSURE_NEW);
    ASSERT_TRUE(xr_semantic_source_callable_pure_body_is_exact(plan, closure->callable_function));
    uint8_t *wire = NULL;
    size_t size = 0;
    ASSERT_TRUE(xr_xsm_encode(plan, &wire, &size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    ASSERT_TRUE(xr_xsm_decode_module_set(wire, size, fixture.dependencies,
        fixture.dependency_count, &decoded, error, sizeof(error)));
    xr_semantic_plan_free(decoded);
    xr_free(wire);
    const XrSemanticPlan *dependency = fixture.dependencies[target->dependency];
    XrSemanticCallTargetRecord saved = *target;
    target->callee_function = dependency->functions[0].id;
    char *key = xr_strdup(saved.canonical_key);
    ASSERT_NOT_NULL(key);
    char *field = strstr(key, ":constructor=");
    ASSERT_NOT_NULL(field);
    char id[33];
    xr_stable_id_hex(target->callee_function, id);
    memcpy(field + 13, id, 32);
    target->canonical_key = key;
    XrFingerprint digest;
    ASSERT_TRUE(xr_stable_id_from_key(key, &target->id, &digest));
    require_rehashed_rejection(plan, &fixture, "wrong constructor declaration", "XR_SEM_0019");
    *target = saved;
    xr_free(key);
    uint32_t other_export = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t e = 0; e < dependency->source_export_count; e++)
        if (e != saved.source_export &&
            dependency->source_exports[e].kind == dependency->source_exports[saved.source_export].kind) {
            other_export = e;
            break;
        }
    ASSERT_TRUE(other_export != XR_SEMANTIC_INDEX_NONE);
    target->source_export = other_export;
    target->export_identity = dependency->source_exports[other_export].id;
    key = xr_strdup(saved.canonical_key);
    ASSERT_NOT_NULL(key);
    constructor_key_replace_id(key, ":class-export=", target->export_identity);
    target->canonical_key = key;
    ASSERT_TRUE(xr_stable_id_from_key(key, &target->id, &digest));
    require_rehashed_rejection(plan, &fixture, "wrong exact exported class", "XR_SEM_0019");
    *target = saved;
    xr_free(key);
    constructor_rehashed_missing_state(plan, &fixture, target->operation);
    uint32_t callable = closure->callable_function;
    closure->callable_function = 0;
    require_rehashed_rejection(plan, &fixture, "wrong closure function source", "XR_SEM_0015");
    closure->callable_function = callable;
    XrSemanticOperandRecord *argument = &plan->operands[construction->operand_begin + 2u];
    uint8_t ownership = argument->ownership_action;
    argument->ownership_action ^= 1u;
    require_rehashed_rejection(plan, &fixture, "wrong callback ownership", "XR_OWN_3002");
    argument->ownership_action = ownership;
    uint8_t mode = argument->parameter_mode;
    argument->parameter_mode = XR_PARAM_REF;
    require_rehashed_rejection(plan, &fixture, "wrong callback mode", "XR_SEM_0019");
    argument->parameter_mode = mode;
    ASSERT_TRUE(xr_semantic_plan_verify_module_set(plan, fixture.dependencies,
        fixture.dependency_count, error, sizeof(error)));
    constructor_source_destroy(&fixture);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Source callable52 constructor artifact");
RUN_TEST(real_source_constructor_and_rehashed_authority_rejections);
TEST_MAIN_END()
