/*
 * test_dynamic_entry_runtime.c - Dynamic typed entry authority and cache
 */

#include "../../../include/xray_runtime_generation.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_coro_lower.h"
#include "../../../src/ir/xi_module.h"
#include "../../../src/ir/xi_stage.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/format/xr_xtp_internal.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_plan.h"
#include "../../../src/plan/target/xr_target_plan_internal.h"
#include "../../../src/plan/target/xr_target_profile_internal.h"
#include "../../../src/runtime/abi/xr_runtime_target_authority.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/runtime/xr_dynamic_entry_runtime.h"
#include "../../../src/runtime/xr_module_generation_internal.h"
#include "../../../src/vm/debug/xr_vm_profile.h"
#include "../../../src/vm/debug/xr_vm_trace.h"
#include "../../../src/vm/xr_typed_dispatch.h"
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                   \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, \
                    __LINE__, #condition);                                   \
            abort();                                                         \
        }                                                                    \
    } while (0)

typedef struct DynamicFixture {
    XrSemanticPlan *caller_semantic;
    XrSemanticPlan *dependency_semantic;
    XrTargetProfile *profile;
    XrTargetPlan *caller_plan;
    XrTargetPlan *dependency_plan;
    uint32_t caller_function;
    uint32_t dependency_function;
    uint32_t wrong_function;
    uint32_t called_export;
    uint32_t alias_export;
} DynamicFixture;

typedef enum DynamicFixtureMode {
    DYNAMIC_FIXTURE_ECHO = 0,
    DYNAMIC_FIXTURE_ALTERNATE,
    DYNAMIC_FIXTURE_DIVIDE_BY_ZERO,
    DYNAMIC_FIXTURE_STEP_LOOP,
} DynamicFixtureMode;

typedef struct ResolverFixture {
    const XiFunc *callee;
} ResolverFixture;

typedef struct RepeatPublishContext {
    XrRuntimeGenerationAuthority *authority;
    const XrTargetPlan *plan;
    XrRuntimeEntryHandle *handle;
    XrEntryCellRegistration registration;
    uint32_t source_export;
    bool success;
} RepeatPublishContext;

typedef struct TrackedDynamicEntryContext {
    XrVmDynamicEntryContext public_context;
    XrVmDynamicEntryContext delegate;
    _Atomic uint32_t acquires;
    _Atomic uint32_t releases;
} TrackedDynamicEntryContext;

typedef struct RejectCallTraceContext {
    uint32_t accepted;
    bool rejected;
} RejectCallTraceContext;

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_unit = {
    .kind = XR_KIND_UNIT,
    .id = 2,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_function = {
    .kind = XR_KIND_FUNCTION,
    .id = 3,
    .frozen = true,
    .function = {
        .return_type = &stub_int,
        .throw_effect = XR_FN_EFFECT_NO_THROW,
    },
};
static XrType stub_module_namespace = {
    .kind = XR_KIND_STRUCT_OBJECT,
    .id = 4,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static int call_suspendability(void *ud, const XiFunc *current,
                               const XiValue *call) {
    (void) ud;
    (void) current;
    return call && call->op == XI_CALL_METHOD && call->aux &&
                   strcmp((const char *) call->aux, "writeBytes") == 0
               ? 0
               : -1;
}

static const XiFunc *resolve_method(void *ud, const XiFunc *current,
                                    const XiValue *call) {
    (void) current;
    const ResolverFixture *fixture = (const ResolverFixture *) ud;
    return fixture && call && call->op == XI_CALL_METHOD && call->aux &&
                   strcmp((const char *) call->aux, "writeBytes") == 0
               ? fixture->callee
               : NULL;
}

static XrTargetProfile *build_profile(void) {
    XrRuntimeTargetAuthority native;
    REQUIRE(xr_runtime_target_authority_native_hosted(&native) ==
            XR_RUNTIME_ABI_OK);
    XrTargetProfileBuildInput input = {
        .machine = native.machine,
        .runtime_abi = &native.runtime_abi,
        .object_header_materialization =
            &native.object_header_materialization,
        .string_contract = &native.string_contract,
        .providers = native.providers,
        .provider_count = native.provider_count,
    };
    XrTargetProfile *profile = NULL;
    char diagnostic[512] = {0};
    REQUIRE(xr_target_profile_build(&input, &profile, diagnostic,
                                    sizeof(diagnostic)));
    return profile;
}

static uint64_t shifted_target_immediate(
    const XrTargetInstructionRecord *row, uint32_t inserted,
    uint32_t added) {
    if (row->opcode == XR_TARGET_INSTRUCTION_JUMP) {
        uint32_t target =
            XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(row->immediate_bits);
        if (target >= inserted)
            target += added;
        return XR_TARGET_INSTRUCTION_TARGET_PACK(target, 0);
    }
    if (row->opcode == XR_TARGET_INSTRUCTION_BRANCH_IF_NONZERO_I64 ||
        row->opcode == XR_TARGET_INSTRUCTION_BRANCH_IF_TRUE_BOOL) {
        uint32_t nonzero =
            XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(row->immediate_bits);
        uint32_t zero =
            XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(row->immediate_bits);
        if (nonzero >= inserted)
            nonzero += added;
        if (zero >= inserted)
            zero += added;
        return XR_TARGET_INSTRUCTION_TARGET_PACK(nonzero, zero);
    }
    return row->immediate_bits;
}

static XrTargetPlan *clone_with_step_loop(
    const XrTargetPlan *source, uint32_t function) {
    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *source_rows =
        xr_target_plan_instructions(source, &instruction_count);
    uint32_t inserted = XR_TARGET_INSTRUCTION_SLOT_NONE;
    uint32_t condition_slot = XR_TARGET_INSTRUCTION_SLOT_NONE;
    for (uint32_t i = 0; i < instruction_count; i++) {
        if (source_rows[i].function == function &&
            source_rows[i].opcode == XR_TARGET_INSTRUCTION_PARAM_I64)
            condition_slot = source_rows[i].result_slot;
        if (source_rows[i].function == function &&
            source_rows[i].opcode == XR_TARGET_INSTRUCTION_RETURN_I64) {
            inserted = i;
            break;
        }
    }
    REQUIRE(inserted != XR_TARGET_INSTRUCTION_SLOT_NONE &&
            condition_slot != XR_TARGET_INSTRUCTION_SLOT_NONE);
    XrTargetInstructionRecord *rows =
        (XrTargetInstructionRecord *) xr_calloc(
            instruction_count + 2u, sizeof(*rows));
    REQUIRE(rows);
    for (uint32_t i = 0, out = 0; i < instruction_count + 2u; i++) {
        if (i == inserted) {
            rows[i] = (XrTargetInstructionRecord) {
                .id = i,
                .function = function,
                .result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE,
                .operand_slots = {condition_slot,
                                  XR_TARGET_INSTRUCTION_SLOT_NONE},
                .immediate_bits =
                    XR_TARGET_INSTRUCTION_TARGET_PACK(i + 1u, i + 2u),
                .opcode = XR_TARGET_INSTRUCTION_BRANCH_IF_NONZERO_I64,
                .operand_count = 1,
            };
            continue;
        }
        if (i == inserted + 1u) {
            rows[i] = (XrTargetInstructionRecord) {
                .id = i,
                .function = function,
                .result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE,
                .operand_slots = {XR_TARGET_INSTRUCTION_SLOT_NONE,
                                  XR_TARGET_INSTRUCTION_SLOT_NONE},
                .immediate_bits =
                    XR_TARGET_INSTRUCTION_TARGET_PACK(i, 0),
                .opcode = XR_TARGET_INSTRUCTION_JUMP,
            };
            continue;
        }
        rows[i] = source_rows[out++];
        rows[i].id = i;
        rows[i].immediate_bits =
            shifted_target_immediate(&rows[i], inserted, 2u);
    }
    XrTargetPlanDraft draft = {
        .semantic_plan = source->semantic_plan,
        .semantic_dependencies =
            (const XrSemanticPlan *const *) source->semantic_dependencies,
        .semantic_dependency_count = source->semantic_dependency_count,
        .profile = source->profile,
        .completed_family_mask = source->completed_family_mask,
    };
#define COPY_TARGET_TABLE(name)                                         \
    draft.name = xr_target_plan_##name(source, &draft.name##_count)
    COPY_TARGET_TABLE(machine_reps);
    COPY_TARGET_TABLE(value_reps);
    COPY_TARGET_TABLE(extents);
    COPY_TARGET_TABLE(layouts);
    COPY_TARGET_TABLE(fields);
    COPY_TARGET_TABLE(storage);
    COPY_TARGET_TABLE(allocations);
    COPY_TARGET_TABLE(extent_operands);
    COPY_TARGET_TABLE(functions);
    COPY_TARGET_TABLE(slots);
    COPY_TARGET_TABLE(calls);
    COPY_TARGET_TABLE(call_arguments);
    COPY_TARGET_TABLE(root_maps);
    COPY_TARGET_TABLE(root_slots);
    COPY_TARGET_TABLE(cleanups);
    COPY_TARGET_TABLE(adapters);
    COPY_TARGET_TABLE(capabilities);
    COPY_TARGET_TABLE(coroutines);
    COPY_TARGET_TABLE(entry_expectations);
#undef COPY_TARGET_TABLE
    draft.instructions = rows;
    draft.instructions_count = instruction_count + 2u;
    XrTargetPlan *loop = NULL;
    char diagnostic[512] = {0};
    bool frozen = xr_target_plan_freeze(
        &draft, &loop, diagnostic, sizeof(diagnostic));
    if (!frozen)
        fprintf(stderr, "dynamic loop plan failed: %s\n", diagnostic);
    REQUIRE(frozen);
    xr_free(rows);
    return loop;
}

static void build_dynamic_fixture(DynamicFixtureMode mode,
                                  DynamicFixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    bool alternate = mode == DYNAMIC_FIXTURE_ALTERNATE;
    const char *dependency_path = alternate ? "stdlib/net/other.xr"
                                            : "stdlib/net/net.xr";
    const char *dependency_name = alternate ? "other" : "net";
    XiFunc *dependency_root = xi_func_new(
        alternate ? "other_init" : "net_init", &stub_unit);
    XiFunc *write = xi_func_new("writeBytes", &stub_int);
    XiFunc *wrong = xi_func_new("wrongBytes", &stub_int);
    REQUIRE(dependency_root && write && wrong);
    XiBlock *dependency_entry = xi_block_new(dependency_root);
    XiBlock *write_entry = xi_block_new(write);
    XiBlock *wrong_entry = xi_block_new(wrong);
    REQUIRE(dependency_entry && write_entry && wrong_entry);
    dependency_entry->sealed = write_entry->sealed = wrong_entry->sealed = true;
    dependency_root->children =
        (XiFunc **) xr_calloc(2, sizeof(*dependency_root->children));
    REQUIRE(dependency_root->children);
    dependency_root->children[0] = write;
    dependency_root->children[1] = wrong;
    dependency_root->nchildren = dependency_root->children_cap = 2;
    write->parent_func = wrong->parent_func = dependency_root;
    write->nparams = write->min_params = 1;
    wrong->nparams = wrong->min_params = 1;
    write->params = (XiValue **) xr_calloc(1, sizeof(*write->params));
    wrong->params = (XiValue **) xr_calloc(1, sizeof(*wrong->params));
    REQUIRE(write->params && wrong->params);
    write->params[0] = xi_param(write, write_entry, 0, &stub_int);
    wrong->params[0] = xi_param(wrong, wrong_entry, 0, &stub_int);
    REQUIRE(write->params[0] && wrong->params[0]);
    XiValue *closure = xi_value_new(dependency_root, dependency_entry,
                                    XI_CLOSURE_NEW, &stub_function, 0);
    XiValue *store = xi_value_new(dependency_root, dependency_entry,
                                  XI_SET_SHARED, &stub_unit, 1);
    XiValue *alias_closure = xi_value_new(
        dependency_root, dependency_entry, XI_CLOSURE_NEW,
        &stub_function, 0);
    XiValue *alias_store = xi_value_new(
        dependency_root, dependency_entry, XI_SET_SHARED, &stub_unit, 1);
    REQUIRE(closure && store && alias_closure && alias_store);
    closure->aux = write;
    store->args[0] = closure;
    store->aux_int = 0;
    alias_closure->aux = write;
    alias_store->args[0] = alias_closure;
    alias_store->aux_int = 1;
    dependency_root->nshared = 2;
    xi_block_set_return(dependency_entry, NULL);
    if (mode == DYNAMIC_FIXTURE_DIVIDE_BY_ZERO) {
        XiValue *zero = xi_const_int(write, write_entry, 0, &stub_int);
        XiValue *division = xi_binary(write, write_entry, XI_DIV,
                                      &stub_int, write->params[0], zero);
        REQUIRE(zero && division);
        xi_block_set_return(write_entry, division);
    } else {
        xi_block_set_return(write_entry, write->params[0]);
    }
    xi_block_set_return(wrong_entry, wrong->params[0]);
    dependency_root->stage = write->stage = wrong->stage =
        XI_STAGE_SEMANTIC_LOWERED;
    dependency_root->invariant_mask = write->invariant_mask =
        wrong->invariant_mask = xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    REQUIRE(xi_coro_lower(dependency_root, NULL));
    dependency_root->stage = write->stage = wrong->stage = XI_STAGE_OPTIMIZED;
    XiModule *dependency_module = xi_module_new(
        dependency_path, dependency_name, dependency_root);
    REQUIRE(dependency_module);
    dependency_root->module = dependency_module;
    dependency_module->nslots = 2;
    dependency_module->nexports = 2;
    dependency_module->exports =
        (XiModuleExport *) xr_calloc(2, sizeof(*dependency_module->exports));
    REQUIRE(dependency_module->exports);
    dependency_module->exports[0] = (XiModuleExport) {
        .name = "writeBytes",
        .shared_slot = 0,
        .function = write,
    };
    dependency_module->exports[1] = (XiModuleExport) {
        .name = "writeAlias",
        .shared_slot = 1,
        .function = write,
    };
    char diagnostic[512] = {0};
    bool dependency_built = xr_semantic_plan_build_and_attach(
        dependency_root, diagnostic, sizeof(diagnostic));
    if (!dependency_built)
        fprintf(stderr, "dynamic dependency fixture failed: %s\n",
                diagnostic);
    REQUIRE(dependency_built);
    fixture->dependency_semantic =
        xr_semantic_plan_retain(dependency_root->semantic_plan);
    REQUIRE(fixture->dependency_semantic &&
            xr_semantic_plan_source_export_count(
                fixture->dependency_semantic) == 2);

    XiFunc *caller_root = xi_func_new(
        alternate ? "other_caller_init" : "http_init", &stub_unit);
    XiFunc *caller = xi_func_new(
        alternate ? "other_call" : "serverWriteAll", &stub_int);
    REQUIRE(caller_root && caller);
    XiBlock *root_entry = xi_block_new(caller_root);
    XiBlock *caller_entry = xi_block_new(caller);
    REQUIRE(root_entry && caller_entry);
    root_entry->sealed = caller_entry->sealed = true;
    caller_root->children =
        (XiFunc **) xr_calloc(1, sizeof(*caller_root->children));
    REQUIRE(caller_root->children);
    caller_root->children[0] = caller;
    caller_root->nchildren = caller_root->children_cap = 1;
    caller->parent_func = caller_root;
    XiImportRef import_ref = {
        .module_path = dependency_path,
        .resolved_mod_index = 0,
        .resolved_shared_slot = -1,
        .resolved_export_slot = -1,
        .resolved_module = dependency_module,
    };
    XiValue *namespace_ref = xi_value_new(
        caller_root, root_entry, XI_IMPORT_REF, &stub_module_namespace, 0);
    XiValue *namespace_alias = xi_value_new(
        caller_root, root_entry, XI_COPY, &stub_module_namespace, 1);
    XiValue *namespace_store = xi_value_new(
        caller_root, root_entry, XI_SET_SHARED, &stub_unit, 1);
    REQUIRE(namespace_ref && namespace_alias && namespace_store);
    namespace_ref->aux = &import_ref;
    namespace_alias->args[0] = namespace_ref;
    namespace_alias->aux_int = XI_COPY_KIND_IDENTITY;
    namespace_store->args[0] = namespace_alias;
    namespace_store->aux_int = 0;
    caller_root->nshared = 1;
    xi_block_set_return(root_entry, NULL);
    XiValue *receiver = xi_value_new(
        caller, caller_entry, XI_GET_SHARED, &stub_module_namespace, 0);
    XiValue *receiver_alias = xi_value_new(
        caller, caller_entry, XI_COPY, &stub_module_namespace, 1);
    XiValue *argument = xi_const_int(caller, caller_entry,
                                      alternate ? 73 : 41, &stub_int);
    XiValue *method = xi_value_new(caller, caller_entry, XI_CALL_METHOD,
                                   &stub_int, 2);
    REQUIRE(receiver && receiver_alias && argument && method);
    receiver->aux_int = 0;
    receiver_alias->args[0] = receiver;
    receiver_alias->aux_int = XI_COPY_KIND_IDENTITY;
    method->args[0] = receiver_alias;
    method->args[1] = argument;
    method->aux = (void *) "writeBytes";
    method->aux_int = 0;
    xi_block_set_return(caller_entry, method);
    caller_root->stage = caller->stage = XI_STAGE_SEMANTIC_LOWERED;
    caller_root->invariant_mask = caller->invariant_mask =
        xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    ResolverFixture resolver_fixture = {.callee = write};
    XiCoroResolver resolver = {
        .resolve_method = resolve_method,
        .call_suspendability = call_suspendability,
        .ud = &resolver_fixture,
    };
    REQUIRE(xi_coro_lower(caller_root, &resolver));
    caller_root->stage = caller->stage = XI_STAGE_OPTIMIZED;
    XiModule *caller_module = xi_module_new(
        alternate ? "stdlib/other/caller.xr" : "stdlib/http/http.xr",
        alternate ? "other_caller" : "http", caller_root);
    REQUIRE(caller_module);
    caller_root->module = caller_module;
    caller_module->nslots = 1;
    XiModule *dependencies[] = {dependency_module};
    REQUIRE(xr_semantic_plan_build_and_attach_module_set(
        caller_root, dependencies, 1, diagnostic, sizeof(diagnostic)));
    fixture->caller_semantic =
        xr_semantic_plan_retain(caller_root->semantic_plan);
    fixture->profile = build_profile();
    bool target_built = xr_target_plan_build(
        fixture->dependency_semantic, fixture->profile,
        &fixture->dependency_plan, diagnostic, sizeof(diagnostic));
    if (!target_built)
        fprintf(stderr, "dynamic dependency target failed: %s\n",
                diagnostic);
    REQUIRE(target_built);
    if (mode == DYNAMIC_FIXTURE_STEP_LOOP) {
        const XrSemanticSourceExportRecord *loop_export =
            xr_semantic_plan_source_export(
                fixture->dependency_semantic, 0);
        REQUIRE(loop_export);
        XrTargetPlan *loop = clone_with_step_loop(
            fixture->dependency_plan, loop_export->function);
        xr_target_plan_free(fixture->dependency_plan);
        fixture->dependency_plan = loop;
    }
    const XrSemanticPlan *semantic_dependencies[] = {
        fixture->dependency_semantic,
    };
    REQUIRE(xr_target_plan_build_module_set(
        fixture->caller_semantic, semantic_dependencies, 1, fixture->profile,
        &fixture->caller_plan, diagnostic, sizeof(diagnostic)));
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    REQUIRE(xr_xtp_encode_plan(fixture->caller_plan, &encoded, &encoded_size,
                               diagnostic, sizeof(diagnostic)));
    XrXtpCandidate *candidate = NULL;
    REQUIRE(xr_xtp_decode_candidate(encoded, encoded_size, &candidate,
                                    diagnostic, sizeof(diagnostic)));
    XrTargetPlan *roundtrip = NULL;
    REQUIRE(xr_xtp_materialize_target_plan_module_set(
        candidate, fixture->caller_semantic, semantic_dependencies, 1,
        fixture->profile, &roundtrip, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(roundtrip),
                                 xr_target_plan_fingerprint(
                                     fixture->caller_plan)));
    uint32_t expectation_count = 0;
    REQUIRE(xr_target_plan_entry_expectations(
                roundtrip, &expectation_count) != NULL &&
            expectation_count == 1);
    xr_target_plan_free(fixture->caller_plan);
    fixture->caller_plan = roundtrip;
    xr_xtp_candidate_release(candidate);
    xr_xtp_encoded_free(encoded);
    const XrSemanticSourceExportRecord *source_export =
        xr_semantic_plan_source_export(fixture->dependency_semantic, 0);
    REQUIRE(source_export);
    fixture->dependency_function = source_export->function;
    fixture->wrong_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t function_count = 0;
    xr_target_plan_functions(fixture->dependency_plan, &function_count);
    for (uint32_t i = 0; i < function_count; i++) {
        if (i != fixture->dependency_function &&
            xr_target_plan_function_execution_family_mask(
                fixture->dependency_plan, i) ==
                XR_TARGET_EXECUTION_SCALAR_I64_CLOSED) {
            fixture->wrong_function = i;
            break;
        }
    }
    fixture->caller_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_instructions(fixture->caller_plan,
                                    &instruction_count);
    for (uint32_t i = 0; i < instruction_count; i++) {
        if (instructions[i].opcode == XR_TARGET_INSTRUCTION_CALL_ENTRY_I64) {
            fixture->caller_function = instructions[i].function;
            break;
        }
    }
    fixture->called_export = XR_SEMANTIC_INDEX_NONE;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(
        fixture->caller_plan, &call_count);
    REQUIRE(calls && call_count == 1);
    for (uint32_t i = 0; i < 2; i++) {
        const XrSemanticSourceExportRecord *candidate =
            xr_semantic_plan_source_export(fixture->dependency_semantic, i);
        if (candidate && xr_stable_id_equal(
                             candidate->id,
                             calls[0].source_export_identity)) {
            fixture->called_export = i;
            break;
        }
    }
    fixture->alias_export = fixture->called_export == 0 ? 1u : 0u;
    REQUIRE(fixture->wrong_function != XR_SEMANTIC_INDEX_NONE &&
            fixture->caller_function != XR_SEMANTIC_INDEX_NONE &&
            fixture->called_export != XR_SEMANTIC_INDEX_NONE);
    xi_func_free(caller_root);
    xi_func_free(dependency_root);
}

static void free_dynamic_fixture(DynamicFixture *fixture) {
    xr_target_plan_free(fixture->caller_plan);
    xr_target_plan_free(fixture->dependency_plan);
    xr_target_profile_free(fixture->profile);
    xr_semantic_plan_free(fixture->caller_semantic);
    xr_semantic_plan_free(fixture->dependency_semantic);
    memset(fixture, 0, sizeof(*fixture));
}

static XrRuntimeGenerationBudget make_budget(void) {
    return (XrRuntimeGenerationBudget) {
        .schema_version = XR_RUNTIME_GENERATION_SCHEMA_VERSION,
        .max_loaded_generations = 8,
        .max_total_pins = 64,
        .max_pins_per_generation = 16,
        .max_pins_by_kind = {16, 16, 16, 16, 16},
    };
}

static XrLoadedModuleGeneration *activate_generation(
    XrRuntimeGenerationAuthority *authority, const XrTargetPlan *plan) {
    char diagnostic[512] = {0};
    XrLoadedModuleGeneration *generation = NULL;
    REQUIRE(xr_module_generation_load_verified_target_plan(
        authority, plan, &generation, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_module_generation_prepare(generation, diagnostic,
                                         sizeof(diagnostic)));
    REQUIRE(xr_module_generation_activate(generation, diagnostic,
                                          sizeof(diagnostic)));
    return generation;
}

static XrRuntimeEntryHandle *bind_handle(
    XrLoadedModuleGeneration *generation, const XrTargetPlan *plan,
    uint32_t function) {
    char diagnostic[512] = {0};
    XrRuntimeEntryHandle *handle = NULL;
    REQUIRE(xr_runtime_entry_handle_create(&handle, diagnostic,
                                           sizeof(diagnostic)));
    XrEntryCellRegistration registration = {
        .generation = generation,
        .verified_plan = plan,
        .function = function,
        .executor_kind = XR_ENTRY_EXECUTOR_TYPED_VM,
    };
    bool already_bound = true;
    bool bound = xr_runtime_entry_handle_bind(
        handle, &registration, &already_bound, diagnostic,
        sizeof(diagnostic));
    if (!bound)
        fprintf(stderr, "dynamic entry bind failed: %s\n", diagnostic);
    REQUIRE(bound);
    REQUIRE(!already_bound);
    REQUIRE(xr_runtime_entry_handle_bind(
        handle, &registration, &already_bound, diagnostic,
        sizeof(diagnostic)));
    REQUIRE(already_bound);
    return handle;
}

static void *repeat_bind_publish(void *argument) {
    RepeatPublishContext *context = (RepeatPublishContext *) argument;
    char diagnostic[512] = {0};
    context->success = true;
    for (uint32_t i = 0; i < 256; i++) {
        bool already_bound = false;
        if (!xr_runtime_entry_handle_bind(
                context->handle, &context->registration, &already_bound,
                diagnostic, sizeof(diagnostic)) ||
            !already_bound ||
            !xr_runtime_entry_registry_publish(
                context->authority, context->plan, context->source_export,
                context->handle, diagnostic, sizeof(diagnostic))) {
            context->success = false;
            break;
        }
        xr_thread_yield();
    }
    return NULL;
}

static XrTypedDispatchStatus execute_dynamic_with_context(
    XrLoadedModuleGeneration *caller, uint32_t function,
    XrTypedDispatchProvider provider, bool use_cache,
    const XrVmDebugSession *debug_session,
    const XrVmDynamicEntryContext *dynamic_entries, int64_t *result) {
    XrFingerprint fingerprint = xr_target_plan_fingerprint(caller->plan);
    XrTypedDispatchI64Request request = {
        .verified_plan = caller->plan,
        .required_plan_fingerprint = &fingerprint,
        .result = result,
        .debug_session = debug_session,
        .decoded_cache = caller->decoded_cache,
        .dynamic_entries = dynamic_entries,
        .generation_identity = &caller->identity,
        .provider = provider,
        .function = function,
        .use_dynamic_entry_cache = use_cache,
    };
    return xr_typed_dispatch_execute_i64(&request);
}

static XrTypedDispatchStatus execute_dynamic(
    XrLoadedModuleGeneration *caller, uint32_t function,
    XrTypedDispatchProvider provider, bool use_cache,
    const XrVmDebugSession *debug_session, int64_t *result) {
    return execute_dynamic_with_context(
        caller, function, provider, use_cache, debug_session,
        &caller->dynamic_entries, result);
}

static XrVmDynamicEntryStatus tracked_validate(
    const XrVmDynamicEntryContext *context,
    const XrTargetPlan *caller_plan,
    const XrFingerprint *caller_fingerprint,
    const XrModuleGenerationIdentity *caller_generation_identity) {
    TrackedDynamicEntryContext *tracked =
        context ? (TrackedDynamicEntryContext *) context->owner : NULL;
    return tracked && tracked->delegate.validate
               ? tracked->delegate.validate(
                     &tracked->delegate, caller_plan, caller_fingerprint,
                     caller_generation_identity)
               : XR_VM_DYNAMIC_ENTRY_INVALID_ARGUMENT;
}

static XrVmDynamicEntryStatus tracked_acquire(
    const XrVmDynamicEntryContext *context,
    const XrTargetPlan *caller_plan,
    const XrFingerprint *caller_fingerprint,
    const XrTargetEntryExpectationRecord *expectation, bool use_cache,
    XrVmDynamicEntryResolution *resolution) {
    TrackedDynamicEntryContext *tracked =
        context ? (TrackedDynamicEntryContext *) context->owner : NULL;
    if (!tracked || !tracked->delegate.acquire)
        return XR_VM_DYNAMIC_ENTRY_INVALID_ARGUMENT;
    XrVmDynamicEntryStatus status = tracked->delegate.acquire(
        &tracked->delegate, caller_plan, caller_fingerprint, expectation,
        use_cache, resolution);
    if (status == XR_VM_DYNAMIC_ENTRY_OK)
        atomic_fetch_add_explicit(&tracked->acquires, 1u,
                                  memory_order_relaxed);
    return status;
}

static XrVmDynamicEntryStatus tracked_release(
    const XrVmDynamicEntryContext *context,
    XrVmDynamicEntryResolution *resolution) {
    TrackedDynamicEntryContext *tracked =
        context ? (TrackedDynamicEntryContext *) context->owner : NULL;
    if (!tracked || !tracked->delegate.release)
        return XR_VM_DYNAMIC_ENTRY_INVALID_ARGUMENT;
    XrVmDynamicEntryStatus status = tracked->delegate.release(
        &tracked->delegate, resolution);
    if (status == XR_VM_DYNAMIC_ENTRY_OK)
        atomic_fetch_add_explicit(&tracked->releases, 1u,
                                  memory_order_relaxed);
    return status;
}

static void tracked_context_init(XrLoadedModuleGeneration *caller,
                                 TrackedDynamicEntryContext *tracked) {
    memset(tracked, 0, sizeof(*tracked));
    tracked->delegate = caller->dynamic_entries;
    tracked->public_context = caller->dynamic_entries;
    tracked->public_context.owner = tracked;
    tracked->public_context.validate = tracked_validate;
    tracked->public_context.acquire = tracked_acquire;
    tracked->public_context.release = tracked_release;
    atomic_init(&tracked->acquires, 0u);
    atomic_init(&tracked->releases, 0u);
}

static bool reject_call_trace(void *context,
                              const XrVmTraceEvent *event) {
    RejectCallTraceContext *rejection =
        (RejectCallTraceContext *) context;
    if (!rejection || !event)
        return false;
    if (event->kind == XR_VM_TRACE_CALL_ENTER) {
        rejection->rejected = true;
        return false;
    }
    rejection->accepted++;
    return true;
}

typedef struct ConcurrentExecuteContext {
    XrLoadedModuleGeneration *caller;
    uint32_t function;
    XrTypedDispatchProvider provider;
    bool success;
} ConcurrentExecuteContext;

static void *execute_warm_calls(void *argument) {
    ConcurrentExecuteContext *context =
        (ConcurrentExecuteContext *) argument;
    context->success = true;
    for (uint32_t i = 0; i < 256; i++) {
        int64_t result = 0;
        if (execute_dynamic(context->caller, context->function,
                            context->provider, true, NULL, &result) !=
                XR_TYPED_DISPATCH_OK ||
            result != 41) {
            context->success = false;
            break;
        }
        xr_thread_yield();
    }
    return NULL;
}

static void retire_generation(XrLoadedModuleGeneration **generation) {
    char diagnostic[512] = {0};
    REQUIRE(xr_module_generation_begin_drain(*generation, diagnostic,
                                             sizeof(diagnostic)));
    REQUIRE(xr_module_generation_retire(*generation, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(xr_module_generation_unload(generation, diagnostic,
                                        sizeof(diagnostic)));
}

static void test_dynamic_entry_authority_cache_and_lifetime(void) {
    DynamicFixture fixture;
    DynamicFixture alternate;
    build_dynamic_fixture(DYNAMIC_FIXTURE_ECHO, &fixture);
    build_dynamic_fixture(DYNAMIC_FIXTURE_ALTERNATE, &alternate);
    char diagnostic[512] = {0};
    XrRuntimeGenerationBudget budget = make_budget();
    XrRuntimeGenerationAuthority *authority = NULL;
    XrRuntimeGenerationAuthority *other_authority = NULL;
    REQUIRE(xr_runtime_generation_authority_create(
        &budget, &authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_runtime_generation_authority_create(
        &budget, &other_authority, diagnostic, sizeof(diagnostic)));
    XrLoadedModuleGeneration *caller = activate_generation(
        authority, fixture.caller_plan);
    XrLoadedModuleGeneration *other_caller = activate_generation(
        authority, fixture.caller_plan);
    XrLoadedModuleGeneration *callee = activate_generation(
        authority, fixture.dependency_plan);
    XrLoadedModuleGeneration *alternate_callee = activate_generation(
        authority, alternate.dependency_plan);
    XrLoadedModuleGeneration *foreign_callee = activate_generation(
        other_authority, fixture.dependency_plan);

    XrRuntimeEntryRegistryStats before;
    XrRuntimeEntryRegistryStats after;
    XrModuleGenerationSnapshot pins_before;
    XrModuleGenerationSnapshot pins_after;
    REQUIRE(xr_runtime_entry_registry_stats(authority, &before));

    XrRuntimeEntryHandle *wrong_function = bind_handle(
        callee, fixture.dependency_plan, fixture.wrong_function);
    REQUIRE(xr_module_generation_snapshot(callee, &pins_before));
    REQUIRE(!xr_runtime_entry_registry_publish(
        authority, fixture.dependency_plan, fixture.called_export,
        wrong_function,
        diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_runtime_entry_registry_stats(authority, &after));
    REQUIRE(memcmp(&before, &after, sizeof(before)) == 0);
    REQUIRE(xr_module_generation_snapshot(callee, &pins_after));
    REQUIRE(memcmp(&pins_before, &pins_after, sizeof(pins_before)) == 0);
    REQUIRE(xr_entry_cell_clear(
        xr_runtime_entry_handle_cell(wrong_function), diagnostic,
        sizeof(diagnostic)));
    REQUIRE(xr_runtime_entry_handle_release(
        &wrong_function, diagnostic, sizeof(diagnostic)));

    XrRuntimeEntryHandle *wrong_plan = bind_handle(
        alternate_callee, alternate.dependency_plan,
        alternate.dependency_function);
    REQUIRE(!xr_runtime_entry_registry_publish(
        authority, fixture.dependency_plan, fixture.called_export, wrong_plan,
        diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_runtime_entry_registry_stats(authority, &after));
    REQUIRE(memcmp(&before, &after, sizeof(before)) == 0);
    REQUIRE(xr_entry_cell_clear(
        xr_runtime_entry_handle_cell(wrong_plan), diagnostic,
        sizeof(diagnostic)));
    REQUIRE(xr_runtime_entry_handle_release(
        &wrong_plan, diagnostic, sizeof(diagnostic)));

    XrRuntimeEntryHandle *wrong_authority = bind_handle(
        foreign_callee, fixture.dependency_plan,
        fixture.dependency_function);
    REQUIRE(!xr_runtime_entry_registry_publish(
        authority, fixture.dependency_plan, fixture.called_export,
        wrong_authority,
        diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_runtime_entry_registry_stats(authority, &after));
    REQUIRE(memcmp(&before, &after, sizeof(before)) == 0);
    REQUIRE(xr_entry_cell_clear(
        xr_runtime_entry_handle_cell(wrong_authority), diagnostic,
        sizeof(diagnostic)));
    REQUIRE(xr_runtime_entry_handle_release(
        &wrong_authority, diagnostic, sizeof(diagnostic)));

    XrRuntimeEntryHandle *first = bind_handle(
        callee, fixture.dependency_plan, fixture.dependency_function);
    REQUIRE(xr_runtime_entry_registry_publish(
        authority, fixture.dependency_plan, fixture.called_export, first,
        diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_runtime_entry_registry_publish(
        authority, fixture.dependency_plan, fixture.called_export, first,
        diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_runtime_entry_registry_publish(
        authority, fixture.dependency_plan, fixture.alias_export, first,
        diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_runtime_entry_registry_stats(authority, &after));
    REQUIRE(after.active_rows == 2 && after.mutations == 2);

    RepeatPublishContext concurrent[2];
    xr_thread_t publish_threads[2];
    for (uint32_t i = 0; i < 2; i++) {
        concurrent[i] = (RepeatPublishContext) {
            .authority = authority,
            .plan = fixture.dependency_plan,
            .handle = first,
            .registration = {
                .generation = callee,
                .verified_plan = fixture.dependency_plan,
                .function = fixture.dependency_function,
                .executor_kind = XR_ENTRY_EXECUTOR_TYPED_VM,
            },
            .source_export = fixture.called_export,
        };
        REQUIRE(xr_thread_create(&publish_threads[i], repeat_bind_publish,
                                 &concurrent[i]));
    }
    for (uint32_t i = 0; i < 2; i++) {
        REQUIRE(xr_thread_join(publish_threads[i], NULL) == 0);
        REQUIRE(concurrent[i].success);
    }

    int64_t result = 0;
    XrFingerprint caller_plan_fingerprint =
        xr_target_plan_fingerprint(caller->plan);
    XrRuntimeDynamicEntryCacheStats untouched_cache;
    XrModuleGenerationSnapshot untouched_caller;
    XrModuleGenerationSnapshot untouched_callee;
    REQUIRE(xr_runtime_dynamic_entry_cache_stats(
        caller->entry_cache, &untouched_cache));
    REQUIRE(xr_module_generation_snapshot(caller, &untouched_caller));
    REQUIRE(xr_module_generation_snapshot(callee, &untouched_callee));
    XrTypedDispatchI64Request wrong_owner_request = {
        .verified_plan = caller->plan,
        .required_plan_fingerprint = &caller_plan_fingerprint,
        .result = &result,
        .decoded_cache = caller->decoded_cache,
        .dynamic_entries = &other_caller->dynamic_entries,
        .generation_identity = &caller->identity,
        .provider = XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        .function = fixture.caller_function,
        .use_dynamic_entry_cache = true,
    };
    REQUIRE(xr_typed_dispatch_execute_i64(&wrong_owner_request) ==
            XR_TYPED_DISPATCH_ENTRY_AUTHORITY_MISMATCH);
    XrModuleGenerationIdentity wrong_identity = caller->identity;
    wrong_identity.generation_number++;
    wrong_owner_request.dynamic_entries = &caller->dynamic_entries;
    wrong_owner_request.generation_identity = &wrong_identity;
    REQUIRE(xr_typed_dispatch_execute_i64(&wrong_owner_request) ==
            XR_TYPED_DISPATCH_ENTRY_AUTHORITY_MISMATCH);
    XrRuntimeDynamicEntryCacheStats still_untouched;
    XrModuleGenerationSnapshot caller_after_rejection;
    XrModuleGenerationSnapshot callee_after_rejection;
    REQUIRE(xr_runtime_dynamic_entry_cache_stats(
        caller->entry_cache, &still_untouched));
    REQUIRE(memcmp(&untouched_cache, &still_untouched,
                   sizeof(untouched_cache)) == 0);
    REQUIRE(xr_module_generation_snapshot(caller,
                                          &caller_after_rejection));
    REQUIRE(xr_module_generation_snapshot(callee,
                                          &callee_after_rejection));
    REQUIRE(memcmp(&untouched_caller, &caller_after_rejection,
                   sizeof(untouched_caller)) == 0);
    REQUIRE(memcmp(&untouched_callee, &callee_after_rejection,
                   sizeof(untouched_callee)) == 0);
    XrTypedDispatchStatus first_status = execute_dynamic(
        caller, fixture.caller_function,
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH, true, NULL, &result);
    if (first_status != XR_TYPED_DISPATCH_OK) {
        fprintf(stderr, "first dynamic execute status: %u\n",
                (unsigned) first_status);
    }
    REQUIRE(first_status == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == 41);
    XrRuntimeDynamicEntryCacheStats cold;
    REQUIRE(xr_runtime_dynamic_entry_cache_stats(caller->entry_cache, &cold));
    REQUIRE(cold.hits == 0 && cold.misses == 1 &&
            cold.registry_scans == 1 && cold.replacements == 1);
    REQUIRE(execute_dynamic(
                caller, fixture.caller_function,
                XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE, true,
                NULL, &result) == XR_TYPED_DISPATCH_OK);
    XrRuntimeDynamicEntryCacheStats warm;
    REQUIRE(xr_runtime_dynamic_entry_cache_stats(caller->entry_cache, &warm));
    REQUIRE(warm.hits == 1 && warm.registry_scans == cold.registry_scans);
    for (uint32_t i = 0; i < 1024; i++) {
        REQUIRE(execute_dynamic(
                    caller, fixture.caller_function,
                    i & 1u
                        ? XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE
                        : XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
                    true, NULL, &result) == XR_TYPED_DISPATCH_OK);
    }
    XrRuntimeDynamicEntryCacheStats steady;
    REQUIRE(xr_runtime_dynamic_entry_cache_stats(caller->entry_cache,
                                                 &steady));
    REQUIRE(steady.hits == warm.hits + 1024 &&
            steady.registry_scans == warm.registry_scans &&
            steady.misses == warm.misses);
    XrFingerprint saved_entry_abi =
        caller->plan->entry_expectations[0].entry_abi_fingerprint;
    caller->plan->entry_expectations[0].entry_abi_fingerprint.bytes[0] ^= 1u;
    REQUIRE(execute_dynamic(
                caller, fixture.caller_function,
                XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH, true, NULL,
                &result) != XR_TYPED_DISPATCH_OK);
    caller->plan->entry_expectations[0].entry_abi_fingerprint = saved_entry_abi;
    XrRuntimeDynamicEntryCacheStats after_invalid_plan;
    REQUIRE(xr_runtime_dynamic_entry_cache_stats(
        caller->entry_cache, &after_invalid_plan));
    REQUIRE(memcmp(&steady, &after_invalid_plan, sizeof(steady)) == 0);

    XrRuntimeEntryHandle *replacement = bind_handle(
        callee, fixture.dependency_plan, fixture.dependency_function);
    ConcurrentExecuteContext execute_contexts[4];
    xr_thread_t execute_threads[4];
    for (uint32_t i = 0; i < 4; i++) {
        execute_contexts[i] = (ConcurrentExecuteContext) {
            .caller = caller,
            .function = fixture.caller_function,
            .provider = i & 1u
                            ? XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE
                            : XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        };
        REQUIRE(xr_thread_create(&execute_threads[i], execute_warm_calls,
                                 &execute_contexts[i]));
    }
    RepeatPublishContext replace_context = {
        .authority = authority,
        .plan = fixture.dependency_plan,
        .handle = replacement,
        .registration = {
            .generation = callee,
            .verified_plan = fixture.dependency_plan,
            .function = fixture.dependency_function,
            .executor_kind = XR_ENTRY_EXECUTOR_TYPED_VM,
        },
        .source_export = fixture.called_export,
    };
    xr_thread_t replace_thread;
    REQUIRE(xr_thread_create(&replace_thread, repeat_bind_publish,
                             &replace_context));
    for (uint32_t i = 0; i < 4; i++) {
        REQUIRE(xr_thread_join(execute_threads[i], NULL) == 0);
        REQUIRE(execute_contexts[i].success);
    }
    REQUIRE(xr_thread_join(replace_thread, NULL) == 0 &&
            replace_context.success);
    XrEntryCallToken alias_token;
    REQUIRE(xr_entry_cell_acquire(
        xr_runtime_entry_handle_cell(first),
        xr_runtime_entry_handle_expectation(first), &alias_token,
        diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_entry_call_release(&alias_token, diagnostic,
                                  sizeof(diagnostic)));
    REQUIRE(execute_dynamic(
                caller, fixture.caller_function,
                XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH, true, NULL,
                &result) == XR_TYPED_DISPATCH_OK);
    XrRuntimeDynamicEntryCacheStats replaced;
    REQUIRE(xr_runtime_dynamic_entry_cache_stats(
        caller->entry_cache, &replaced));
    if (!(replaced.misses >= 2 && replaced.registry_scans >= 2 &&
          replaced.replacements == 2))
        fprintf(stderr,
                "replacement stats hit=%llu miss=%llu scan=%llu replace=%llu\n",
                (unsigned long long) replaced.hits,
                (unsigned long long) replaced.misses,
                (unsigned long long) replaced.registry_scans,
                (unsigned long long) replaced.replacements);
    REQUIRE(replaced.misses >= 2 && replaced.registry_scans >= 2 &&
            replaced.replacements == 2);

    XrRuntimeEntryHandle *alias_replacement = bind_handle(
        callee, fixture.dependency_plan, fixture.dependency_function);
    REQUIRE(xr_runtime_entry_registry_publish(
        authority, fixture.dependency_plan, fixture.alias_export,
        alias_replacement,
        diagnostic, sizeof(diagnostic)));
    REQUIRE(execute_dynamic(
                caller, fixture.caller_function,
                XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE, true,
                NULL, &result) == XR_TYPED_DISPATCH_OK);
    XrRuntimeDynamicEntryCacheStats unrelated;
    REQUIRE(xr_runtime_dynamic_entry_cache_stats(
        caller->entry_cache, &unrelated));
    REQUIRE(unrelated.hits == replaced.hits + 1 &&
            unrelated.registry_scans == replaced.registry_scans);

    REQUIRE(execute_dynamic(
                caller, fixture.caller_function,
                XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH, false, NULL,
                &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(execute_dynamic(
                caller, fixture.caller_function,
                XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE, false,
                NULL, &result) == XR_TYPED_DISPATCH_OK);
    XrRuntimeDynamicEntryCacheStats no_cache;
    REQUIRE(xr_runtime_dynamic_entry_cache_stats(
        caller->entry_cache, &no_cache));
    REQUIRE(no_cache.registry_scans == unrelated.registry_scans + 2);

    XrVmTraceEvent switch_events[32];
    XrVmTraceEvent table_events[32];
    XrVmTraceBuffer switch_buffer;
    XrVmTraceBuffer table_buffer;
    XrVmProfile switch_profile;
    XrVmProfile table_profile;
    REQUIRE(xr_typed_trace_buffer_init(
        &switch_buffer, switch_events, 32));
    REQUIRE(xr_typed_trace_buffer_init(&table_buffer, table_events, 32));
    REQUIRE(xr_typed_profile_init(&switch_profile));
    REQUIRE(xr_typed_profile_init(&table_profile));
    XrVmTraceSink switch_sink =
        xr_typed_trace_buffer_sink(&switch_buffer);
    XrVmTraceSink table_sink = xr_typed_trace_buffer_sink(&table_buffer);
    XrFingerprint caller_fingerprint = xr_target_plan_fingerprint(caller->plan);
    XrVmDebugSession switch_session;
    XrVmDebugSession table_session;
    REQUIRE(xr_typed_debug_session_init(
                &caller_fingerprint, &caller->identity, &switch_sink,
                &switch_profile, &switch_session) == XR_VM_DEBUG_SESSION_OK);
    REQUIRE(xr_typed_debug_session_init(
                &caller_fingerprint, &caller->identity, &table_sink,
                &table_profile, &table_session) == XR_VM_DEBUG_SESSION_OK);
    int64_t switch_result = 0;
    int64_t table_result = 0;
    REQUIRE(execute_dynamic(
                caller, fixture.caller_function,
                XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH, true,
                &switch_session, &switch_result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(execute_dynamic(
                caller, fixture.caller_function,
                XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE, true,
                &table_session, &table_result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(switch_result == 41 && table_result == switch_result &&
            switch_buffer.count == table_buffer.count &&
            memcmp(switch_events, table_events,
                   switch_buffer.count * sizeof(*switch_events)) == 0);
    XrVmProfileSnapshot switch_snapshot;
    XrVmProfileSnapshot table_snapshot;
    REQUIRE(xr_typed_profile_snapshot(&switch_profile, &switch_snapshot));
    REQUIRE(xr_typed_profile_snapshot(&table_profile, &table_snapshot));
    REQUIRE(memcmp(&switch_snapshot, &table_snapshot,
                   sizeof(switch_snapshot)) == 0);
    bool saw_child = false;
    bool saw_return = false;
    XrFingerprint callee_fingerprint = xr_target_plan_fingerprint(callee->plan);
    for (size_t i = 0; i < switch_buffer.count; i++) {
        saw_child |= xr_fingerprint_equal(
            switch_buffer.events[i].target_plan_fingerprint,
            callee_fingerprint);
        saw_return |=
            switch_buffer.events[i].kind == XR_VM_TRACE_CALL_RETURN &&
            switch_buffer.events[i].related_function ==
                          fixture.dependency_function;
    }
    REQUIRE(saw_child && saw_return);

    XrEntryCallToken inflight;
    REQUIRE(xr_entry_cell_acquire(
        xr_runtime_entry_handle_cell(replacement),
        xr_runtime_entry_handle_expectation(replacement), &inflight,
        diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_runtime_entry_registry_unpublish(
        authority, first, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_runtime_entry_registry_unpublish(
        authority, replacement, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_runtime_entry_registry_unpublish(
        authority, alias_replacement, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_module_generation_begin_drain(callee, diagnostic,
                                             sizeof(diagnostic)));
    REQUIRE(!xr_module_generation_retire(callee, diagnostic,
                                         sizeof(diagnostic)));
    REQUIRE(xr_entry_call_release(&inflight, diagnostic,
                                  sizeof(diagnostic)));
    REQUIRE(xr_module_generation_retire(callee, diagnostic,
                                        sizeof(diagnostic)));

    retire_generation(&caller);
    retire_generation(&other_caller);
    REQUIRE(xr_module_generation_unload(&callee, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(xr_runtime_entry_handle_release(&first, diagnostic,
                                            sizeof(diagnostic)));
    REQUIRE(xr_runtime_entry_handle_release(&replacement, diagnostic,
                                            sizeof(diagnostic)));
    REQUIRE(xr_runtime_entry_handle_release(&alias_replacement, diagnostic,
                                            sizeof(diagnostic)));
    retire_generation(&alternate_callee);
    retire_generation(&foreign_callee);
    REQUIRE(xr_runtime_generation_authority_destroy(
        &authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_runtime_generation_authority_destroy(
        &other_authority, diagnostic, sizeof(diagnostic)));
    free_dynamic_fixture(&alternate);
    free_dynamic_fixture(&fixture);
}

static void publish_fixture_entry(
    XrRuntimeGenerationAuthority *authority,
    const DynamicFixture *fixture, XrLoadedModuleGeneration *callee,
    XrRuntimeEntryHandle **handle) {
    char diagnostic[512] = {0};
    *handle = bind_handle(callee, fixture->dependency_plan,
                          fixture->dependency_function);
    REQUIRE(xr_runtime_entry_registry_publish(
        authority, fixture->dependency_plan, fixture->called_export,
        *handle, diagnostic, sizeof(diagnostic)));
}

static void require_pins_equal(
    const XrModuleGenerationSnapshot *before,
    const XrModuleGenerationSnapshot *after) {
    REQUIRE(before->total_pins == after->total_pins);
    for (uint32_t i = 0; i < XR_MODULE_GENERATION_PIN_KIND_COUNT; i++)
        REQUIRE(before->pins_by_kind[i] == after->pins_by_kind[i]);
}

static void test_dynamic_entry_all_exit_release(void) {
    char diagnostic[512] = {0};
    XrRuntimeGenerationBudget budget = make_budget();

    DynamicFixture error_fixture;
    build_dynamic_fixture(DYNAMIC_FIXTURE_DIVIDE_BY_ZERO, &error_fixture);
    XrRuntimeGenerationAuthority *error_authority = NULL;
    REQUIRE(xr_runtime_generation_authority_create(
        &budget, &error_authority, diagnostic, sizeof(diagnostic)));
    XrLoadedModuleGeneration *error_caller = activate_generation(
        error_authority, error_fixture.caller_plan);
    XrLoadedModuleGeneration *error_callee = activate_generation(
        error_authority, error_fixture.dependency_plan);
    XrRuntimeEntryHandle *error_handle = NULL;
    publish_fixture_entry(error_authority, &error_fixture, error_callee,
                          &error_handle);
    TrackedDynamicEntryContext error_context;
    tracked_context_init(error_caller, &error_context);
    XrModuleGenerationSnapshot caller_before;
    XrModuleGenerationSnapshot caller_after;
    XrModuleGenerationSnapshot callee_before;
    XrModuleGenerationSnapshot callee_after;
    REQUIRE(xr_module_generation_snapshot(error_caller, &caller_before));
    REQUIRE(xr_module_generation_snapshot(error_callee, &callee_before));
    int64_t result = 91;
    REQUIRE(execute_dynamic_with_context(
                error_caller, error_fixture.caller_function,
                XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH, true, NULL,
                &error_context.public_context, &result) ==
            XR_TYPED_DISPATCH_DIVIDE_BY_ZERO);
    REQUIRE(result == 0 &&
            atomic_load_explicit(&error_context.acquires,
                                 memory_order_relaxed) == 1u &&
            atomic_load_explicit(&error_context.releases,
                                 memory_order_relaxed) == 1u);
    REQUIRE(xr_module_generation_snapshot(error_caller, &caller_after));
    REQUIRE(xr_module_generation_snapshot(error_callee, &callee_after));
    require_pins_equal(&caller_before, &caller_after);
    require_pins_equal(&callee_before, &callee_after);

    RejectCallTraceContext rejection = {0};
    XrVmTraceSink rejecting_sink = {
        .emit = reject_call_trace,
        .context = &rejection,
    };
    XrFingerprint error_fingerprint =
        xr_target_plan_fingerprint(error_caller->plan);
    XrVmDebugSession rejecting_session;
    REQUIRE(xr_typed_debug_session_init(
                &error_fingerprint, &error_caller->identity,
                &rejecting_sink, NULL, &rejecting_session) ==
            XR_VM_DEBUG_SESSION_OK);
    result = 92;
    REQUIRE(execute_dynamic_with_context(
                error_caller, error_fixture.caller_function,
                XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE, true,
                &rejecting_session, &error_context.public_context,
                &result) == XR_TYPED_DISPATCH_TRACE_REJECTED);
    REQUIRE(result == 0 && rejection.rejected && rejection.accepted > 0 &&
            atomic_load_explicit(&error_context.acquires,
                                 memory_order_relaxed) == 2u &&
            atomic_load_explicit(&error_context.releases,
                                 memory_order_relaxed) == 2u);
    REQUIRE(xr_module_generation_snapshot(error_caller, &caller_after));
    REQUIRE(xr_module_generation_snapshot(error_callee, &callee_after));
    require_pins_equal(&caller_before, &caller_after);
    require_pins_equal(&callee_before, &callee_after);

    REQUIRE(xr_runtime_entry_registry_unpublish(
        error_authority, error_handle, diagnostic, sizeof(diagnostic)));
    retire_generation(&error_caller);
    REQUIRE(xr_runtime_entry_handle_release(
        &error_handle, diagnostic, sizeof(diagnostic)));
    retire_generation(&error_callee);
    REQUIRE(xr_runtime_generation_authority_destroy(
        &error_authority, diagnostic, sizeof(diagnostic)));
    free_dynamic_fixture(&error_fixture);

    DynamicFixture loop_fixture;
    build_dynamic_fixture(DYNAMIC_FIXTURE_STEP_LOOP, &loop_fixture);
    XrRuntimeGenerationAuthority *loop_authority = NULL;
    REQUIRE(xr_runtime_generation_authority_create(
        &budget, &loop_authority, diagnostic, sizeof(diagnostic)));
    XrLoadedModuleGeneration *loop_caller = activate_generation(
        loop_authority, loop_fixture.caller_plan);
    XrLoadedModuleGeneration *loop_callee = activate_generation(
        loop_authority, loop_fixture.dependency_plan);
    XrRuntimeEntryHandle *loop_handle = NULL;
    publish_fixture_entry(loop_authority, &loop_fixture, loop_callee,
                          &loop_handle);
    TrackedDynamicEntryContext loop_context;
    tracked_context_init(loop_caller, &loop_context);
    REQUIRE(xr_module_generation_snapshot(loop_caller, &caller_before));
    REQUIRE(xr_module_generation_snapshot(loop_callee, &callee_before));
    result = 93;
    REQUIRE(execute_dynamic_with_context(
                loop_caller, loop_fixture.caller_function,
                XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE, true,
                NULL, &loop_context.public_context, &result) ==
            XR_TYPED_DISPATCH_STEP_LIMIT_EXCEEDED);
    REQUIRE(result == 0 &&
            atomic_load_explicit(&loop_context.acquires,
                                 memory_order_relaxed) == 1u &&
            atomic_load_explicit(&loop_context.releases,
                                 memory_order_relaxed) == 1u);
    REQUIRE(xr_module_generation_snapshot(loop_caller, &caller_after));
    REQUIRE(xr_module_generation_snapshot(loop_callee, &callee_after));
    require_pins_equal(&caller_before, &caller_after);
    require_pins_equal(&callee_before, &callee_after);

    REQUIRE(xr_runtime_entry_registry_unpublish(
        loop_authority, loop_handle, diagnostic, sizeof(diagnostic)));
    retire_generation(&loop_caller);
    REQUIRE(xr_runtime_entry_handle_release(
        &loop_handle, diagnostic, sizeof(diagnostic)));
    retire_generation(&loop_callee);
    REQUIRE(xr_runtime_generation_authority_destroy(
        &loop_authority, diagnostic, sizeof(diagnostic)));
    free_dynamic_fixture(&loop_fixture);
}

int main(void) {
    test_dynamic_entry_authority_cache_and_lifetime();
    test_dynamic_entry_all_exit_release();
    puts("dynamic typed entry runtime tests passed");
    return 0;
}
