/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_scalar_program.c - PSC/CallDecision to Xi binding KAT
 */

#include "../test_framework.h"

#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "frontend/analyzer/xa_program_semantic_closure.h"
#include "frontend/analyzer/xa_typed_program.h"
#include "frontend/analyzer/xanalyzer.h"
#include "ir/xi_lower.h"
#include "ir/xi_opt.h"
#include "ir/xi_scalar_program.h"
#include "ir/xi_scalar_semantic_plan.h"
#include "module/xmodule_graph.h"
#include "module/xmodule_resolver.h"
#include "plan/format/xr_xtp_internal.h"
#include "plan/format/xr_xtp_schema.h"
#include "plan/semantic/xr_semantic_builder.h"
#include "plan/semantic/xr_semantic_plan_internal.h"
#include "plan/semantic/xr_semantic_verify.h"
#include "plan/format/xr_xsm_schema.h"
#include "plan/target/xr_target_builder.h"
#include "plan/target/xr_target_plan_internal.h"
#include "plan/target/xr_target_profile.h"
#include "plan/target/xr_target_verify.h"
#include "runtime/abi/xr_runtime_target_profile.h"
#include "toolchain/xcompiler_session.h"
#include <string.h>

static const char kScalarSource[] =
    "fn add1(value: i64) -> i64 { return value + 1 }\n"
    "fn root() -> i64 { return add1(41) }\n";

typedef struct ScalarFixture {
    XrModuleResolver *resolver;
    XrModuleGraph *graph;
    XrModuleSpec *spec;
    XaAnalyzer *analyzer;
    XaTypedProgram *typed;
} ScalarFixture;

static bool fixture_analyze(ScalarFixture *fixture,
                            XrCompilerSession *session,
                            const char *namespace_id) {
    memset(fixture, 0, sizeof(*fixture));
    XrModuleResolverConfig resolver_config = {0};
    fixture->resolver = xr_module_resolver_new(&resolver_config);
    if (!fixture->resolver)
        return false;
    fixture->graph = xr_module_graph_new(session, fixture->resolver);
    if (!fixture->graph)
        return false;
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_MEMORY,
        .namespace_id = namespace_id,
    };
    char *error = NULL;
    if (xr_module_graph_build_source(fixture->graph, &authority, kScalarSource,
                                     &error) != 0) {
        xr_free(error);
        return false;
    }
    xr_free(error);
    if (xr_module_graph_topological_sort(fixture->graph) != 0 ||
        fixture->graph->has_cycle || fixture->graph->spec_count != 1 ||
        fixture->graph->entry_index < 0)
        return false;
    fixture->spec =
        &fixture->graph->specs[fixture->graph->entry_index];
    fixture->analyzer = xa_analyzer_new(session);
    if (!fixture->analyzer)
        return false;
    xa_analyzer_set_graph(fixture->analyzer, fixture->graph);
    xa_analyzer_analyze(fixture->analyzer, "scalar-binding.xr",
                        fixture->spec->ast);
    int diagnostic_count = 0;
    for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(
             fixture->analyzer, &diagnostic_count);
         diag; diag = diag->next) {
        if (diag->severity == XR_DIAG_SEV_ERROR)
            return false;
    }
    XrHashMap *exports = NULL;
    if (!xa_analyzer_collect_export_symbols_checked(
            fixture->analyzer, fixture->spec->ast, &exports))
        return false;
    fixture->spec->status = XR_MODSPEC_ANALYZED;
    return true;
}

static bool fixture_publish(ScalarFixture *fixture) {
    XaTypedProgramPublishResult result = xa_typed_program_publish(
        fixture->analyzer, fixture->spec->ast, NULL, 1);
    fixture->typed = result.program;
    return result.reason == XA_TYPED_PROGRAM_REASON_NONE && result.program;
}

static void fixture_cleanup(ScalarFixture *fixture) {
    xa_typed_program_free(fixture->typed);
    xa_analyzer_free(fixture->analyzer);
    xr_module_graph_free(fixture->graph);
    xr_module_resolver_free(fixture->resolver);
    memset(fixture, 0, sizeof(*fixture));
}

static bool same_id(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static XiFunc *find_function(XiModule *module,
                             const XrProgramSemanticClosure *closure,
                             XrStableId identity) {
    for (uint16_t i = 0; module && i < module->nfuncs; i++) {
        XiFunc *function = module->functions[i];
        const XrProgramSemanticFunctionRecord *row =
            function ? xr_program_semantic_closure_function(
                           closure, function->psc_function_index)
                     : NULL;
        if (row && same_id(row->id, identity))
            return function;
    }
    return NULL;
}

static XiValue *find_bound_call(XiFunc *function) {
    for (uint32_t b = 0; function && b < function->nblocks; b++) {
        XiBlock *block = function->blocks[b];
        for (uint32_t i = 0; block && i < block->nvalues; i++) {
            XiValue *value = block->values[i];
            if (value && value->psc_call_index != XI_PSC_ROW_NONE)
                return value;
        }
    }
    return NULL;
}

static void dce_and_mark_tree_optimized(XiFunc *function) {
    if (!function)
        return;
    /* Match the production optimization boundary. Named-function lowering
     * creates a pure self-call sentinel that generic DCE removes when the
     * function is not recursive. */
    xi_opt_dce(function);
    function->stage = XI_STAGE_OPTIMIZED;
    for (uint16_t i = 0; i < function->nchildren; i++)
        dce_and_mark_tree_optimized(function->children[i]);
}

static bool build_authorities(
    ScalarFixture *fixture, XrTargetProfile *profile,
    XrProgramSemanticClosure **closure, XrScalarCallDecision *decision,
    char *error, size_t error_size) {
    if (!xa_typed_program_build_scalar_closure(
            fixture->typed, closure, error, error_size))
        return false;
    return xr_scalar_call_decision_build(
               *closure,
               xr_program_semantic_closure_generation_id(*closure), profile,
               decision, error, error_size) &&
           xr_scalar_call_decision_verify(decision, *closure, profile, error,
                                          error_size);
}

TEST(stable_rows_survive_mutation_and_ownership_gates) {
    XrCompilerSessionConfig session_config = {0};
    XrCompilerSession *session = xr_compiler_session_new(&session_config);
    ASSERT_NOT_NULL(session);
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, session,
                                "xi-scalar-binding-direct"));
    ASSERT_TRUE(fixture_publish(&fixture));

    char error[512] = {0};
    XrTargetProfile *profile = NULL;
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(
        &profile, error, sizeof(error)));
    XrProgramSemanticClosure *closure = NULL;
    XrScalarCallDecision decision = {0};
    ASSERT_TRUE(build_authorities(&fixture, profile, &closure, &decision,
                                  error, sizeof(error)));
    XrProgramSemanticClosure *retained =
        xr_program_semantic_closure_retain(closure);
    ASSERT_TRUE(retained == closure);

    XiScalarProgramInput input = {
        .closure = closure,
        .decision = &decision,
    };
    XiFunc *root =
        xi_lower_program(fixture.typed, NULL, false, &input);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(root->module);
    ASSERT_TRUE(xi_module_take_scalar_program(
        root->module, &closure, &decision, profile, error, sizeof(error)));
    ASSERT_NULL(closure);
    bool verified = xi_scalar_program_verify(root->module, profile, error,
                                             sizeof(error));
    if (!verified)
        fprintf(stderr, "Xi scalar verify failed: %s\n", error);
    ASSERT_TRUE(verified);

    XiFunc *caller = find_function(
        root->module, retained, decision.caller_function);
    XiFunc *callee = find_function(
        root->module, retained, decision.callee_function);
    XiValue *call = find_bound_call(caller);
    ASSERT_NOT_NULL(caller);
    ASSERT_NOT_NULL(callee);
    ASSERT_NOT_NULL(call);
    ASSERT_EQ_UINT(callee->inline_policy, XI_INLINE_PRESERVE_CALL);

    uint32_t saved_function_index = caller->psc_function_index;
    caller->psc_function_index = callee->psc_function_index;
    ASSERT_FALSE(xi_scalar_program_verify(root->module, profile, error,
                                          sizeof(error)));
    caller->psc_function_index = saved_function_index;

    uint32_t saved_locator_kind = caller->psc_declaration_locator.kind;
    caller->psc_declaration_locator.kind ^= UINT32_C(1);
    ASSERT_FALSE(xi_scalar_program_verify(root->module, profile, error,
                                          sizeof(error)));
    caller->psc_declaration_locator.kind = saved_locator_kind;

    uint32_t saved_call_index = call->psc_call_index;
    call->psc_call_index = XI_PSC_ROW_NONE;
    ASSERT_FALSE(xi_scalar_program_verify(root->module, profile, error,
                                          sizeof(error)));
    call->psc_call_index = saved_call_index;

    uint32_t saved_source_kind = call->source_kind;
    call->source_kind ^= UINT32_C(1);
    ASSERT_FALSE(xi_scalar_program_verify(root->module, profile, error,
                                          sizeof(error)));
    call->source_kind = saved_source_kind;

    root->module->scalar_call_decision->generation_id.bytes[0] ^=
        UINT8_C(0x40);
    ASSERT_FALSE(xi_scalar_program_verify(root->module, profile, error,
                                          sizeof(error)));
    root->module->scalar_call_decision->generation_id.bytes[0] ^=
        UINT8_C(0x40);
    root->module->scalar_call_decision->call_identity.bytes[0] ^=
        UINT8_C(0x20);
    ASSERT_FALSE(xi_scalar_program_verify(root->module, profile, error,
                                          sizeof(error)));
    root->module->scalar_call_decision->call_identity.bytes[0] ^=
        UINT8_C(0x20);
    ASSERT_TRUE(xi_scalar_program_verify(root->module, profile, error,
                                         sizeof(error)));

    dce_and_mark_tree_optimized(root);
    ASSERT_TRUE(xi_module_set_identity(
        root->module,
        "memory-module-v1:id=26:xi-scalar-semantic-plan-v1"));
    XrSemanticPlan *semantic = NULL;
    bool semantic_built = xr_semantic_plan_build(
        root, &semantic, error, sizeof(error));
    if (!semantic_built)
        fprintf(stderr, "Scalar SemanticPlan build failed: %s\n", error);
    ASSERT_TRUE(semantic_built);
    ASSERT_NOT_NULL(semantic);
    ASSERT_TRUE(xi_scalar_semantic_plan_verify(
        root, semantic, profile, error, sizeof(error)));
    XrTargetPlan *target_plan = NULL;
    bool target_built = xr_target_plan_build(
        semantic, profile, &target_plan, error, sizeof(error));
    if (!target_built)
        fprintf(stderr, "Scalar TargetPlan build failed: %s\n", error);
    ASSERT_TRUE(target_built);
    ASSERT_NOT_NULL(target_plan);
    ASSERT_TRUE(xr_target_plan_verify(target_plan, error, sizeof(error)));
    ASSERT_TRUE(xr_target_plan_is_frozen(target_plan));
    ASSERT_TRUE(xr_target_plan_is_verified(target_plan));
    ASSERT_TRUE(xr_target_plan_fingerprint_is_intact(target_plan));
    ASSERT_EQ_UINT(xr_target_plan_schema_version(target_plan),
                   XR_TARGET_PLAN_SCHEMA_VERSION);
    ASSERT_EQ_UINT(xr_target_plan_completed_family_mask(target_plan),
                   XR_TARGET_REQUIRED_FAMILIES);
    ASSERT_EQ_PTR(xr_target_plan_semantic_plan(target_plan), semantic);
    ASSERT_EQ_PTR(xr_target_plan_profile(target_plan), profile);
    ASSERT_TRUE(xr_fingerprint_equal(
        xr_target_plan_semantic_fingerprint(target_plan),
        xr_semantic_plan_fingerprint(semantic)));
    ASSERT_TRUE(xr_fingerprint_equal(
        xr_target_profile_fingerprint(xr_target_plan_profile(target_plan)),
        decision.target_profile_fingerprint));
    ASSERT_EQ_UINT(target_plan->functions_count, 3);
    ASSERT_EQ_UINT(target_plan->value_reps_count, 10);
    ASSERT_EQ_UINT(target_plan->slots_count, 8);
    ASSERT_EQ_UINT(target_plan->instructions_count, 7);
    ASSERT_EQ_UINT(target_plan->calls_count, 1);
    ASSERT_EQ_UINT(target_plan->call_arguments_count, 1);
    ASSERT_EQ_UINT(target_plan->capabilities_count, 2);
    ASSERT_EQ_UINT(target_plan->capabilities[0].capability,
                   XR_TARGET_CAPABILITY_ALLOCATOR);
    ASSERT_EQ_UINT(target_plan->capabilities[0].flags,
                   XR_TARGET_CAPABILITY_REQUIRED);
    ASSERT_EQ_UINT(target_plan->capabilities[1].capability,
                   XR_TARGET_CAPABILITY_PANIC);
    ASSERT_EQ_UINT(target_plan->capabilities[1].flags,
                   XR_TARGET_CAPABILITY_REQUIRED);

    const XrSemanticProgramCallBinding *program_call =
        &semantic->program_call_bindings[0];
    const XrSemanticOperationRecord *semantic_call =
        xr_semantic_plan_operation(semantic, program_call->operation);
    ASSERT_NOT_NULL(semantic_call);
    const XrTargetCallRecord *target_call = &target_plan->calls[0];
    const XrSemanticCallTargetRecord *semantic_target =
        xr_semantic_plan_call_target(semantic,
                                     target_call->semantic_call_target);
    ASSERT_NOT_NULL(semantic_target);
    ASSERT_EQ_UINT(target_call->id, 0);
    ASSERT_EQ_UINT(target_call->semantic_operation, program_call->operation);
    ASSERT_EQ_UINT(target_call->caller_function, semantic_call->function);
    ASSERT_EQ_UINT(target_call->callee_function,
                   program_call->target_function);
    ASSERT_EQ_UINT(semantic_target->operation, program_call->operation);
    ASSERT_EQ_UINT(semantic_target->function, program_call->target_function);
    ASSERT_EQ_UINT(target_plan->functions[target_call->caller_function]
                       .semantic_function,
                   semantic_call->function);
    ASSERT_EQ_UINT(target_plan->functions[target_call->callee_function]
                       .semantic_function,
                   program_call->target_function);
    ASSERT_EQ_UINT(target_call->native_abi, decision.native_abi);
    ASSERT_EQ_UINT(target_call->calling_convention,
                   XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL);
    ASSERT_EQ_UINT(target_call->target_kind,
                   XR_TARGET_CALL_TARGET_DIRECT_LOCAL);
    ASSERT_EQ_UINT(target_call->result_mode, XR_TARGET_CALL_VALUE);
    ASSERT_EQ_UINT(target_call->result_ownership, XR_TARGET_CALL_NONE);
    ASSERT_EQ_UINT(target_call->argument_count, 1);
    ASSERT_EQ_UINT(target_call->adapter_count, 0);
    ASSERT_EQ_UINT(target_call->flags, 0);
    ASSERT_EQ_UINT(target_call->error_mode,
                   XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL);
    ASSERT_EQ_UINT(target_call->error_slot, XR_SEMANTIC_INDEX_NONE);
    ASSERT_NE(target_call->result_slot, XR_SEMANTIC_INDEX_NONE);
    const XrTargetMachineRepRecord *result_register_rep =
        xr_target_plan_machine_rep(target_plan,
                                   target_call->result_register_rep);
    const XrTargetMachineRepRecord *result_memory_rep =
        xr_target_plan_machine_rep(target_plan,
                                   target_call->result_memory_rep);
    ASSERT_TRUE(result_register_rep && result_memory_rep &&
                result_register_rep->kind == XR_MACHINE_REP_I64 &&
                result_memory_rep->kind == XR_MACHINE_REP_I64 &&
                result_register_rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
                result_memory_rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL);
    const XrTargetValueRepRecord *result_value_rep =
        xr_target_plan_value_rep(target_plan, target_call->result_value);
    ASSERT_TRUE(result_value_rep &&
                result_value_rep->slot == target_call->result_slot);

    const XrTargetCallArgumentRecord *target_argument =
        &target_plan->call_arguments[0];
    ASSERT_EQ_UINT(target_argument->call, target_call->id);
    ASSERT_EQ_UINT(target_argument->ordinal, 0);
    ASSERT_EQ_UINT(target_argument->mode, XR_TARGET_CALL_VALUE);
    ASSERT_EQ_UINT(target_argument->ownership, XR_TARGET_CALL_CONSUME);
    ASSERT_EQ_UINT(target_argument->transfer_mode, XR_TRANSFER_SHARE);
    ASSERT_EQ_UINT(target_argument->flags, 0);
    ASSERT_EQ_UINT(target_argument->semantic_operand,
                   semantic_call->operand_begin + 1u);
    ASSERT_EQ_UINT(target_argument->semantic_value,
                   semantic->operands[target_argument->semantic_operand].value);
    ASSERT_EQ_UINT(target_argument->callee_parameter, 0);
    const XrTargetMachineRepRecord *argument_register_rep =
        xr_target_plan_machine_rep(target_plan, target_argument->register_rep);
    const XrTargetMachineRepRecord *argument_memory_rep =
        xr_target_plan_machine_rep(target_plan, target_argument->memory_rep);
    const XrTargetMachineRepRecord *callee_register_rep =
        xr_target_plan_machine_rep(target_plan,
                                   target_argument->callee_register_rep);
    const XrTargetMachineRepRecord *callee_memory_rep =
        xr_target_plan_machine_rep(target_plan,
                                   target_argument->callee_memory_rep);
    ASSERT_TRUE(argument_register_rep && argument_memory_rep &&
                callee_register_rep && callee_memory_rep &&
                argument_register_rep->kind == XR_MACHINE_REP_I64 &&
                argument_memory_rep->kind == XR_MACHINE_REP_I64 &&
                callee_register_rep->kind == XR_MACHINE_REP_I64 &&
                callee_memory_rep->kind == XR_MACHINE_REP_I64);

    uint32_t callee_instruction_count = 0;
    const XrTargetInstructionRecord *callee_instructions =
        xr_target_plan_function_instructions(
            target_plan, target_call->callee_function,
            &callee_instruction_count);
    ASSERT_NOT_NULL(callee_instructions);
    ASSERT_EQ_UINT(callee_instruction_count, 4);
    ASSERT_EQ_UINT(xr_target_plan_function_execution_family_mask(
                       target_plan, target_call->callee_function),
                   XR_TARGET_EXECUTION_SCALAR_I64_CLOSED);
    ASSERT_EQ_UINT(callee_instructions[0].opcode,
                   XR_TARGET_INSTRUCTION_PARAM_I64);
    ASSERT_EQ_UINT(callee_instructions[0].result_slot,
                   target_argument->callee_slot);
    ASSERT_EQ_UINT(callee_instructions[1].opcode,
                   XR_TARGET_INSTRUCTION_CONST_I64);
    ASSERT_EQ_UINT(callee_instructions[1].immediate_bits, 1);
    ASSERT_EQ_UINT(callee_instructions[2].opcode,
                   XR_TARGET_INSTRUCTION_ADD_WRAP_I64);
    ASSERT_EQ_UINT(callee_instructions[2].operand_slots[0],
                   callee_instructions[0].result_slot);
    ASSERT_EQ_UINT(callee_instructions[2].operand_slots[1],
                   callee_instructions[1].result_slot);
    ASSERT_EQ_UINT(callee_instructions[3].opcode,
                   XR_TARGET_INSTRUCTION_RETURN_I64);
    ASSERT_EQ_UINT(callee_instructions[3].operand_slots[0],
                   callee_instructions[2].result_slot);

    uint32_t caller_instruction_count = 0;
    const XrTargetInstructionRecord *caller_instructions =
        xr_target_plan_function_instructions(
            target_plan, target_call->caller_function,
            &caller_instruction_count);
    ASSERT_NOT_NULL(caller_instructions);
    ASSERT_EQ_UINT(caller_instruction_count, 3);
    ASSERT_EQ_UINT(xr_target_plan_function_execution_family_mask(
                       target_plan, target_call->caller_function),
                   XR_TARGET_EXECUTION_SCALAR_I64_CLOSED);
    ASSERT_EQ_UINT(caller_instructions[0].opcode,
                   XR_TARGET_INSTRUCTION_CONST_I64);
    ASSERT_EQ_UINT(caller_instructions[0].immediate_bits, 41);
    ASSERT_EQ_UINT(caller_instructions[0].result_slot,
                   target_argument->caller_slot);
    ASSERT_EQ_UINT(caller_instructions[1].opcode,
                   XR_TARGET_INSTRUCTION_CALL_DIRECT_I64);
    ASSERT_EQ_UINT(caller_instructions[1].immediate_bits, target_call->id);
    ASSERT_EQ_UINT(caller_instructions[1].operand_count, 0);
    ASSERT_EQ_UINT(caller_instructions[1].operand_slots[0],
                   XR_TARGET_INSTRUCTION_SLOT_NONE);
    ASSERT_EQ_UINT(caller_instructions[1].result_slot,
                   target_call->result_slot);
    ASSERT_EQ_UINT(caller_instructions[2].opcode,
                   XR_TARGET_INSTRUCTION_RETURN_I64);
    ASSERT_EQ_UINT(caller_instructions[2].operand_slots[0],
                   target_call->result_slot);

    XrFingerprint original_target_fingerprint =
        xr_target_plan_fingerprint(target_plan);
    uint16_t saved_native_abi = target_plan->calls[0].native_abi;
    target_plan->calls[0].native_abi = XR_TARGET_ABI_NONE;
    xr_target_call_compute_fingerprint(
        target_plan, 0, &target_plan->calls[0].fingerprint);
    xr_target_plan_compute_fingerprint(target_plan,
                                       &target_plan->fingerprint);
    ASSERT_FALSE(xr_target_plan_verify(target_plan, error, sizeof(error)));
    target_plan->calls[0].native_abi = saved_native_abi;
    xr_target_call_compute_fingerprint(
        target_plan, 0, &target_plan->calls[0].fingerprint);
    xr_target_plan_compute_fingerprint(target_plan,
                                       &target_plan->fingerprint);
    ASSERT_TRUE(xr_target_plan_verify(target_plan, error, sizeof(error)));
    ASSERT_TRUE(xr_fingerprint_equal(target_plan->fingerprint,
                                     original_target_fingerprint));

    XrTargetInstructionRecord *mutable_call_instruction =
        &target_plan->instructions[caller_instructions[1].id];
    uint64_t saved_call_immediate = mutable_call_instruction->immediate_bits;
    mutable_call_instruction->immediate_bits = UINT32_MAX;
    xr_target_plan_compute_fingerprint(target_plan,
                                       &target_plan->fingerprint);
    ASSERT_FALSE(xr_target_plan_verify(target_plan, error, sizeof(error)));
    mutable_call_instruction->immediate_bits = saved_call_immediate;
    xr_target_plan_compute_fingerprint(target_plan,
                                       &target_plan->fingerprint);
    ASSERT_TRUE(xr_target_plan_verify(target_plan, error, sizeof(error)));
    ASSERT_TRUE(xr_fingerprint_equal(target_plan->fingerprint,
                                     original_target_fingerprint));

    XrTargetPlan *second_target_plan = NULL;
    ASSERT_TRUE(xr_target_plan_build(semantic, profile, &second_target_plan,
                                     error, sizeof(error)));
    ASSERT_NOT_NULL(second_target_plan);
    ASSERT_TRUE(xr_fingerprint_equal(
        xr_target_plan_fingerprint(second_target_plan),
        original_target_fingerprint));
    uint8_t *target_encoded = NULL;
    size_t target_encoded_size = 0;
    uint8_t *second_target_encoded = NULL;
    size_t second_target_encoded_size = 0;
    ASSERT_TRUE(xr_xtp_encode_plan(target_plan, &target_encoded,
                                   &target_encoded_size, error,
                                   sizeof(error)));
    ASSERT_TRUE(xr_xtp_encode_plan(second_target_plan,
                                   &second_target_encoded,
                                   &second_target_encoded_size, error,
                                   sizeof(error)));
    ASSERT_EQ_UINT(target_encoded_size, second_target_encoded_size);
    ASSERT_TRUE(memcmp(target_encoded, second_target_encoded,
                       target_encoded_size) == 0);
    XrXtpCandidate *target_candidate = NULL;
    ASSERT_TRUE(xr_xtp_decode_candidate(
        target_encoded, target_encoded_size, &target_candidate, error,
        sizeof(error)));
    XrTargetPlan *roundtrip_target_plan = NULL;
    ASSERT_TRUE(xr_xtp_materialize_target_plan(
        target_candidate, semantic, profile, &roundtrip_target_plan, error,
        sizeof(error)));
    ASSERT_NOT_NULL(roundtrip_target_plan);
    ASSERT_TRUE(xr_target_plan_verify(roundtrip_target_plan, error,
                                      sizeof(error)));
    ASSERT_TRUE(xr_fingerprint_equal(
        xr_target_plan_fingerprint(roundtrip_target_plan),
        original_target_fingerprint));
    uint8_t *target_roundtrip = NULL;
    size_t target_roundtrip_size = 0;
    ASSERT_TRUE(xr_xtp_encode_plan(roundtrip_target_plan, &target_roundtrip,
                                   &target_roundtrip_size, error,
                                   sizeof(error)));
    ASSERT_EQ_UINT(target_roundtrip_size, target_encoded_size);
    ASSERT_TRUE(memcmp(target_roundtrip, target_encoded,
                       target_encoded_size) == 0);
    xr_xtp_encoded_free(target_roundtrip);
    xr_target_plan_free(roundtrip_target_plan);
    xr_xtp_candidate_release(target_candidate);
    xr_xtp_encoded_free(second_target_encoded);
    xr_xtp_encoded_free(target_encoded);
    xr_target_plan_free(second_target_plan);
    ASSERT_EQ_UINT(xr_semantic_plan_function_count(semantic), 3);
    ASSERT_EQ_UINT(xr_semantic_plan_call_target_count(semantic), 1);
    XrFingerprint retained_fingerprint =
        xr_program_semantic_closure_fingerprint(retained);
    XrGenerationClosureId retained_generation =
        xr_program_semantic_closure_generation_id(retained);
    ASSERT_EQ_UINT(semantic->program_provenance.schema,
                   XR_SEMANTIC_PROGRAM_PROVENANCE_SCHEMA_VERSION);
    ASSERT_EQ_UINT(semantic->program_provenance.program_schema,
                   xr_program_semantic_closure_schema(retained));
    ASSERT_EQ_UINT(semantic->program_function_binding_count, 2);
    ASSERT_EQ_UINT(semantic->program_call_binding_count, 1);
    ASSERT_TRUE(xr_fingerprint_equal(
        semantic->program_provenance.program_fingerprint,
        retained_fingerprint));
    ASSERT_TRUE(memcmp(semantic->program_provenance.generation_identity.bytes,
                       retained_generation.bytes,
                       sizeof(retained_generation.bytes)) == 0);

    uint32_t saved_target =
        semantic->program_call_bindings[0].target_function;
    semantic->program_call_bindings[0].target_function =
        XR_SEMANTIC_INDEX_NONE;
    ASSERT_FALSE(xi_scalar_semantic_plan_verify(
        root, semantic, profile, error, sizeof(error)));
    semantic->program_call_bindings[0].target_function = saved_target;
    semantic->program_provenance.program_fingerprint.bytes[0] ^=
        UINT8_C(0x80);
    ASSERT_FALSE(xi_scalar_semantic_plan_verify(
        root, semantic, profile, error, sizeof(error)));
    semantic->program_provenance.program_fingerprint.bytes[0] ^=
        UINT8_C(0x80);
    uint32_t saved_provenance_schema = semantic->program_provenance.schema;
    semantic->program_provenance.schema = 0;
    ASSERT_FALSE(xi_scalar_semantic_plan_verify(
        root, semantic, profile, error, sizeof(error)));
    semantic->program_provenance.schema = saved_provenance_schema;
    ASSERT_TRUE(xi_scalar_semantic_plan_verify(
        root, semantic, profile, error, sizeof(error)));
    XrFingerprint saved_plan_fingerprint = semantic->fingerprint;
    semantic->program_call_bindings[0].reserved = 1;
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    ASSERT_FALSE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    semantic->program_call_bindings[0].reserved = 0;
    semantic->fingerprint = saved_plan_fingerprint;

    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    ASSERT_TRUE(xr_xsm_encode(semantic, &encoded, &encoded_size, error,
                              sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    ASSERT_TRUE(xr_xsm_decode(encoded, encoded_size, &decoded, error,
                              sizeof(error)));
    ASSERT_TRUE(decoded != NULL &&
                decoded->program_function_binding_count == 2 &&
                decoded->program_call_binding_count == 1 &&
                xr_fingerprint_equal(
                    xr_semantic_plan_fingerprint(decoded),
                    xr_semantic_plan_fingerprint(semantic)));
    uint8_t *roundtrip = NULL;
    size_t roundtrip_size = 0;
    ASSERT_TRUE(xr_xsm_encode(decoded, &roundtrip, &roundtrip_size, error,
                              sizeof(error)));
    ASSERT_EQ_UINT(roundtrip_size, encoded_size);
    ASSERT_TRUE(memcmp(roundtrip, encoded, encoded_size) == 0);
    xr_free(roundtrip);
    xr_semantic_plan_free(decoded);

    ASSERT_TRUE(encoded_size > XR_XSM_HEADER_SIZE + 303u);
    uint8_t *hostile = (uint8_t *) xr_malloc(encoded_size);
    ASSERT_NOT_NULL(hostile);
    memcpy(hostile, encoded, encoded_size);
    semantic->program_call_bindings[0].reserved = 1;
    XrFingerprint hostile_fingerprint;
    xr_semantic_plan_compute_fingerprint(semantic, &hostile_fingerprint);
    semantic->program_call_bindings[0].reserved = 0;
    hostile[XR_XSM_HEADER_SIZE + 300u] = UINT8_C(1);
    memcpy(hostile + XR_XSM_HEADER_SIZE - XR_FINGERPRINT_BYTES,
           hostile_fingerprint.bytes, sizeof(hostile_fingerprint.bytes));
    xr_sha256(hostile + XR_XSM_HEADER_SIZE,
              encoded_size - XR_XSM_HEADER_SIZE, hostile + 24u);
    decoded = NULL;
    ASSERT_FALSE(xr_xsm_decode(hostile, encoded_size, &decoded, error,
                               sizeof(error)));
    ASSERT_NULL(decoded);
    xr_free(hostile);

    hostile = (uint8_t *) xr_malloc(encoded_size);
    ASSERT_NOT_NULL(hostile);
    memcpy(hostile, encoded, encoded_size);
    semantic->program_provenance.schema = 0;
    xr_semantic_plan_compute_fingerprint(semantic, &hostile_fingerprint);
    semantic->program_provenance.schema = saved_provenance_schema;
    memset(hostile + XR_XSM_HEADER_SIZE + 96u, 0, sizeof(uint32_t));
    memcpy(hostile + XR_XSM_HEADER_SIZE - XR_FINGERPRINT_BYTES,
           hostile_fingerprint.bytes, sizeof(hostile_fingerprint.bytes));
    xr_sha256(hostile + XR_XSM_HEADER_SIZE,
              encoded_size - XR_XSM_HEADER_SIZE, hostile + 24u);
    decoded = NULL;
    ASSERT_FALSE(xr_xsm_decode(hostile, encoded_size, &decoded, error,
                               sizeof(error)));
    ASSERT_NULL(decoded);
    xr_free(hostile);
    xr_free(encoded);

    XrProgramSemanticClosure *second = NULL;
    XrScalarCallDecision second_decision = {0};
    ASSERT_TRUE(build_authorities(&fixture, profile, &second,
                                  &second_decision, error, sizeof(error)));
    XrProgramSemanticClosure *second_owner = second;
    ASSERT_FALSE(xi_module_take_scalar_program(
        root->module, &second, &second_decision, profile, error,
        sizeof(error)));
    ASSERT_TRUE(second == second_owner);
    xr_program_semantic_closure_free(second);

    xi_func_free(root);
    ASSERT_TRUE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    xr_target_plan_free(target_plan);
    xr_semantic_plan_free(semantic);
    ASSERT_TRUE(xr_program_semantic_closure_verify(retained, error,
                                                   sizeof(error)));
    xr_program_semantic_closure_free(retained);
    xr_target_profile_free(profile);
    fixture_cleanup(&fixture);
    xr_compiler_session_delete(session);
}

TEST(retain_rejects_mutable_closure) {
    XrProgramSemanticClosureLimits limits = {
        .max_modules = 1,
        .max_dependencies = 0,
        .max_types = 0,
        .max_functions = 2,
        .max_calls = 1,
    };
    XrFingerprint policy = {{0}};
    policy.bytes[0] = 1;
    XrProgramSemanticClosure *collecting = NULL;
    char error[256] = {0};
    ASSERT_TRUE(xr_program_semantic_closure_create(
        &limits, policy, &collecting, error, sizeof(error)));
    ASSERT_NULL(xr_program_semantic_closure_retain(collecting));
    xr_program_semantic_closure_free(collecting);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("PSC/CallDecision to Xi binding");
RUN_TEST(stable_rows_survive_mutation_and_ownership_gates);
RUN_TEST(retain_rejects_mutable_closure);
TEST_MAIN_END()
