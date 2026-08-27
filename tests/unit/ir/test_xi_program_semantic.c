/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_program_semantic.c - PSC-backed Xi/SemanticPlan binding KAT
 */

#include "../test_framework.h"

#include "aot/xaot_boundary.h"
#include "aot/xaot_bundle.h"
#include "aot/xi_cgen.h"
#include "aot/xr_target_aggregate_c_projection.h"
#include "aot/xr_leaf_value_product_program_emission.h"
#include "base/xmalloc.h"
#include "base/xmemstream.h"
#include "base/xsha256.h"
#include "frontend/analyzer/xa_program_semantic_closure.h"
#include "frontend/analyzer/xa_typed_program.h"
#include "frontend/analyzer/xanalyzer.h"
#include "ir/xi_arc.h"
#include "ir/xi_escape.h"
#include "ir/xi_lower.h"
#include "ir/xi_opt.h"
#include "ir/xi_own.h"
#include "ir/xi_program_semantic.h"
#include "ir/xi_program_semantic_plan.h"
#include "ir/xi_semantic_snapshot.h"
#include "module/xmodule_graph.h"
#include "module/xmodule_resolver.h"
#include "plan/format/xr_xtp_internal.h"
#include "plan/format/xr_xtp_schema.h"
#include "plan/ownership/xr_ownership_certificate_internal.h"
#include "plan/semantic/xr_semantic_builder.h"
#include "plan/semantic/xr_semantic_plan_internal.h"
#include "plan/semantic/xr_semantic_verify.h"
#include "plan/format/xr_xsm_schema.h"
#include "plan/target/xr_target_builder.h"
#include "plan/target/xr_target_instruction_verify.h"
#include "plan/target/xr_target_plan_internal.h"
#include "plan/target/xr_target_profile.h"
#include "plan/target/xr_target_verify.h"
#include "runtime/class/xclass_info.h"
#include "runtime/abi/xr_runtime_target_authority.h"
#include "runtime/abi/xr_runtime_target_profile.h"
#include "toolchain/xcompiler_session.h"
#include "vm/xr_typed_dispatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char kScalarSource[] = "fn add1(value: i64) -> i64 { return value + 1 }\n"
                                    "fn root() -> i64 { return add1(41) }\n";

static const char kLeafAggregateSource[] =
    "struct Pair { left: i64; right: i64 }\n"
    "fn swap(value: Pair) -> Pair { return Pair{left: value.right, right: value.left} }\n"
    "fn root() -> Pair { return swap(Pair{left: 1, right: 41}) }\n";

static const char kLeafAggregateSourceWithTrailingComment[] =
    "struct Pair { left: i64; right: i64 }\n"
    "fn swap(value: Pair) -> Pair { return Pair{left: value.right, right: value.left} }\n"
    "fn root() -> Pair { return swap(Pair{left: 1, right: 41}) }\n"
    "// Changes source identity without moving any declaration locator.\n";

static const char kLeafAggregateCanonicalShapeMismatchSource[] =
    "struct Pair { left: i64; right: i64 }\n"
    "fn swap(value: Pair) -> Pair { return Pair{left: value.right, right: value.left} }\n"
    "fn root() -> Pair {\n"
    "    var left = 1\n"
    "    var right = left + 40\n"
    "    return swap(Pair{left: left, right: right})\n"
    "}\n";

static const char kLeafProductXiSource[] =
    "fn scan() -> (i64, i64, u8, i64, i64, i64) { "
    "return (1, 2, 3 as u8, 4, 5, 6) }\n"
    "fn decodePath() -> (i64, i64, u8, i64, i64, i64) {\n"
    "    var value = scan()\n"
    "    return (value.0, value.1, value.2, value.3, value.4, value.5)\n"
    "}\n"
    "fn validatePath() -> (i64, i64, u8, i64, i64, i64) {\n"
    "    var value = scan()\n"
    "    return (value.0, value.1, value.2, value.3, value.4, value.5)\n"
    "}\n";

typedef struct ScalarFixture {
    XrModuleResolver *resolver;
    XrModuleGraph *graph;
    XrModuleSpec *spec;
    XaAnalyzer *analyzer;
    XaTypedProgram *typed;
} ScalarFixture;

static bool fixture_analyze(ScalarFixture *fixture, XrCompilerSession *session,
                            const char *namespace_id, const char *source) {
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
    if (xr_module_graph_build_source(fixture->graph, &authority, source, &error) != 0) {
        xr_free(error);
        return false;
    }
    xr_free(error);
    if (xr_module_graph_topological_sort(fixture->graph) != 0 || fixture->graph->has_cycle ||
        fixture->graph->spec_count != 1 || fixture->graph->entry_index < 0)
        return false;
    fixture->spec = &fixture->graph->specs[fixture->graph->entry_index];
    fixture->analyzer = xa_analyzer_new(session);
    if (!fixture->analyzer)
        return false;
    xa_analyzer_set_graph(fixture->analyzer, fixture->graph);
    xa_analyzer_analyze(fixture->analyzer, "scalar-binding.xr", fixture->spec->ast);
    int diagnostic_count = 0;
    for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(fixture->analyzer, &diagnostic_count);
         diag; diag = diag->next) {
        if (diag->severity == XR_DIAG_SEV_ERROR)
            return false;
    }
    XrHashMap *exports = NULL;
    if (!xa_analyzer_collect_export_symbols_checked(fixture->analyzer, fixture->spec->ast,
                                                    &exports))
        return false;
    fixture->spec->status = XR_MODSPEC_ANALYZED;
    return true;
}

static bool fixture_publish(ScalarFixture *fixture) {
    XaTypedProgramPublishResult result =
        xa_typed_program_publish(fixture->analyzer, fixture->spec->ast, NULL, 1);
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

static bool same_fingerprint(XrFingerprint left, XrFingerprint right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static bool same_locator(XrProgramSemanticSourceLocator left,
                         XrProgramSemanticSourceLocator right) {
    return left.kind == right.kind && left.start_line == right.start_line &&
           left.start_column == right.start_column && left.end_line == right.end_line &&
           left.end_column == right.end_column;
}

static uint32_t find_program_type_row(const XrProgramSemanticClosure *closure, uint8_t kind) {
    uint32_t match = XI_PSC_ROW_NONE;
    for (uint32_t i = 0; closure && i < xr_program_semantic_closure_type_count(closure); i++) {
        const XrProgramSemanticTypeRecord *row = xr_program_semantic_closure_type(closure, i);
        if (!row || row->kind != kind)
            continue;
        if (match != XI_PSC_ROW_NONE)
            return XI_PSC_ROW_NONE;
        match = i;
    }
    return match;
}

static XiFunc *find_function(XiModule *module, const XrProgramSemanticClosure *closure,
                             XrStableId identity) {
    for (uint16_t i = 0; module && i < module->nfuncs; i++) {
        XiFunc *function = module->functions[i];
        const XrProgramSemanticFunctionRecord *row =
            function ? xr_program_semantic_closure_function(closure, function->psc_function_index)
                     : NULL;
        if (row && same_id(row->id, identity))
            return function;
    }
    return NULL;
}

static void find_semantic_function_recursive(XiFunc *function,
                                             const XrProgramSemanticClosure *closure,
                                             const XrSemanticProgramFunctionBinding *binding,
                                             XiFunc **match, uint32_t *match_count) {
    if (!function || !match || !match_count)
        return;
    const XrProgramSemanticFunctionRecord *program_function =
        function->psc_function_index != XI_PSC_ROW_NONE
            ? xr_program_semantic_closure_function(closure, function->psc_function_index)
            : NULL;
    if (binding && function->psc_function_index == binding->program_row && program_function &&
        same_id(program_function->id, binding->program_function)) {
        *match = function;
        (*match_count)++;
    }
    for (uint16_t i = 0; i < function->nchildren; i++)
        find_semantic_function_recursive(function->children[i], closure, binding, match,
                                         match_count);
}

static XiFunc *find_semantic_function(XiModule *module, const XrSemanticPlan *semantic,
                                      uint32_t semantic_function) {
    XiFunc *match = NULL;
    uint32_t match_count = 0;
    const XrProgramSemanticClosure *closure = module ? module->program_semantic_closure : NULL;
    const XrSemanticProgramFunctionBinding *binding =
        xr_semantic_plan_program_function_for_semantic_function(semantic, semantic_function);
    const XrProgramSemanticFunctionRecord *program_function =
        binding ? xr_program_semantic_closure_function(closure, binding->program_row) : NULL;
    if (!program_function || !same_id(program_function->id, binding->program_function))
        return NULL;
    if (module->init)
        find_semantic_function_recursive(module->init, closure, binding, &match, &match_count);
    return match_count == 1 ? match : NULL;
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

static XiValue *find_bound_type_value(XiModule *module, uint32_t program_type, uint16_t opcode) {
    for (uint16_t f = 0; module && f < module->nfuncs; f++) {
        XiFunc *function = module->functions[f];
        for (uint32_t b = 0; function && b < function->nblocks; b++) {
            XiBlock *block = function->blocks[b];
            for (uint32_t i = 0; block && i < block->nvalues; i++) {
                XiValue *value = block->values[i];
                if (value && value->psc_type_index == program_type &&
                    (opcode == UINT16_MAX || value->op == opcode))
                    return value;
            }
        }
    }
    return NULL;
}

static XiValue *find_non_call_value(XiModule *module) {
    for (uint16_t f = 0; module && f < module->nfuncs; f++) {
        XiFunc *function = module->functions[f];
        for (uint32_t b = 0; function && b < function->nblocks; b++) {
            XiBlock *block = function->blocks[b];
            for (uint32_t i = 0; block && i < block->nvalues; i++) {
                XiValue *value = block->values[i];
                if (value && value->op != XI_CALL)
                    return value;
            }
        }
    }
    return NULL;
}

static void assert_foreign_declaration_type_is_rejected(XiModule *module, XrType **type_slot,
                                                        char *error, size_t error_size) {
    ASSERT_NOT_NULL(module);
    ASSERT_NOT_NULL(type_slot);
    ASSERT_NOT_NULL(*type_slot);
    ASSERT_NOT_NULL((*type_slot)->instance.class_ref);
    XrType *saved = *type_slot;
    XrClassInfo foreign_same_shape_class = *saved->instance.class_ref;
    XrType foreign_same_shape_type = *saved;
    foreign_same_shape_type.instance.class_ref = &foreign_same_shape_class;
    *type_slot = &foreign_same_shape_type;
    ASSERT_FALSE(xi_program_semantic_verify(module, NULL, error, error_size));
    *type_slot = saved;
    ASSERT_TRUE(xi_program_semantic_verify(module, NULL, error, error_size));
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

static void prepare_leaf_tree_for_semantic_plan(XiFunc *function) {
    ASSERT_NOT_NULL(function);
    xi_escape_analyze(function);
    xi_stack_alloc_rewrite(function);
    xi_arc_insert(function);
    xi_arc_elim(function);
    dce_and_mark_tree_optimized(function);
}

static bool build_authorities(ScalarFixture *fixture, XrTargetProfile *profile,
                              XrProgramSemanticClosure **closure, XrScalarCallDecision *decision,
                              char *error, size_t error_size) {
    if (!xa_typed_program_build_scalar_closure(fixture->typed, closure, error, error_size))
        return false;
    return xr_scalar_call_decision_build(*closure,
                                         xr_program_semantic_closure_generation_id(*closure),
                                         profile, decision, error, error_size) &&
           xr_scalar_call_decision_verify(decision, *closure, profile, error, error_size);
}

typedef struct LeafAggregateTargetEvidence {
    XrSemanticProgramTypeBinding *aggregate_binding;
    XrTargetLayoutRecord *layout;
    XrTargetCallRecord *call;
    XrTargetCallArgumentRecord *argument;
    uint32_t layout_index;
    uint32_t aggregate_rep;
} LeafAggregateTargetEvidence;

typedef struct LeafProductTargetEvidence {
    XrSemanticProgramTypeBinding *product_binding;
    XrTargetLayoutRecord *layout;
    XrTargetCallRecord *calls[2];
    uint32_t layout_index;
    uint32_t aggregate_rep;
    uint32_t callers[2];
    uint32_t callee;
} LeafProductTargetEvidence;

static void resign_leaf_semantic(XrSemanticPlan *semantic) {
    ASSERT_NOT_NULL(semantic);
    ASSERT_NOT_NULL(semantic->ownership);
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    semantic->ownership->semantic_fingerprint = semantic->fingerprint;
    semantic->ownership->fingerprint = semantic->fingerprint;
}

static void resign_leaf_target(XrTargetPlan *plan) {
    ASSERT_NOT_NULL(plan);
    for (uint32_t i = 0; i < plan->layouts_count; i++)
        xr_target_layout_compute_fingerprint(plan, i, &plan->layouts[i].fingerprint);
    for (uint32_t i = 0; i < plan->calls_count; i++)
        xr_target_call_compute_fingerprint(plan, i, &plan->calls[i].fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
}

static void expect_leaf_target_verify_rejection(XrTargetPlan *plan) {
    char error[512] = {0};
    resign_leaf_target(plan);
    ASSERT_FALSE(xr_target_plan_verify(plan, error, sizeof(error)));
}

static void expect_leaf_instruction_verify_rejection(XrTargetPlan *plan) {
    char error[512] = {0};
    ASSERT_FALSE(xr_target_instruction_program_verify(plan, error, sizeof(error)));
}

static void expect_leaf_target_build_rejection(XrSemanticPlan *semantic, XrTargetProfile *profile) {
    char error[512] = {0};
    XrTargetPlan *candidate = NULL;
    resign_leaf_semantic(semantic);
    ASSERT_FALSE(xr_target_plan_build(semantic, profile, &candidate, error, sizeof(error)));
    ASSERT_NULL(candidate);
}

static void assert_leaf_product_target_shape(XrSemanticPlan *semantic, XrTargetPlan *plan,
                                             LeafProductTargetEvidence *out) {
    LeafProductTargetEvidence evidence = {
        .layout_index = XR_SEMANTIC_INDEX_NONE,
        .aggregate_rep = XR_SEMANTIC_INDEX_NONE,
        .callers = {XR_SEMANTIC_INDEX_NONE, XR_SEMANTIC_INDEX_NONE},
        .callee = XR_SEMANTIC_INDEX_NONE,
    };
    ASSERT_NOT_NULL(semantic);
    ASSERT_NOT_NULL(plan);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(xr_target_plan_verify(plan, NULL, 0));
    ASSERT_TRUE(xr_target_instruction_program_verify(plan, NULL, 0));
    ASSERT_TRUE(xr_target_plan_fingerprint_is_intact(plan));
    ASSERT_EQ_UINT(plan->adapters_count, 0);
    ASSERT_EQ_UINT(plan->call_arguments_count, 0);

    for (uint32_t i = 0; i < semantic->program_type_binding_count; i++) {
        XrSemanticProgramTypeBinding *binding = &semantic->program_type_bindings[i];
        if (binding->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT)
            continue;
        ASSERT_NULL(evidence.product_binding);
        evidence.product_binding = binding;
    }
    ASSERT_NOT_NULL(evidence.product_binding);
    ASSERT_EQ_UINT(evidence.product_binding->field_count, 6);
    for (uint32_t i = 0; i < plan->layouts_count; i++) {
        if (plan->layouts[i].semantic_type != evidence.product_binding->semantic_type)
            continue;
        ASSERT_EQ_UINT(evidence.layout_index, XR_SEMANTIC_INDEX_NONE);
        evidence.layout_index = i;
        evidence.layout = &plan->layouts[i];
    }
    ASSERT_NOT_NULL(evidence.layout);
    ASSERT_EQ_UINT(evidence.layout->kind, XR_TARGET_LAYOUT_AGGREGATE);
    ASSERT_EQ_UINT(evidence.layout->fixed_prefix_size, 48);
    ASSERT_EQ_UINT(evidence.layout->align, 8);
    ASSERT_EQ_UINT(evidence.layout->field_count, 6);
    for (uint32_t ordinal = 0; ordinal < 6; ordinal++) {
        const XrTargetFieldRecord *field =
            &plan->fields[evidence.layout->field_begin + ordinal];
        ASSERT_EQ_UINT(field->layout, evidence.layout_index);
        ASSERT_EQ_UINT(field->semantic_field, ordinal);
        ASSERT_EQ_UINT(field->offset, ordinal * 8u);
        ASSERT_EQ_UINT(field->size, ordinal == 2 ? 1 : 8);
        ASSERT_EQ_UINT(field->align, ordinal == 2 ? 1 : 8);
        ASSERT_EQ_UINT(field->root_kind, XR_TARGET_ROOT_NONE);
        ASSERT_TRUE(field->memory_rep < plan->machine_reps_count);
        ASSERT_EQ_UINT(plan->machine_reps[field->memory_rep].kind,
                       ordinal == 2 ? XR_MACHINE_REP_U8 : XR_MACHINE_REP_I64);
    }
    uint32_t aggregate_rep_count = 0;
    for (uint32_t i = 0; i < plan->machine_reps_count; i++) {
        const XrTargetMachineRepRecord *rep = &plan->machine_reps[i];
        if (rep->kind != XR_MACHINE_REP_AGGREGATE || rep->detail != evidence.layout_index)
            continue;
        evidence.aggregate_rep = i;
        aggregate_rep_count++;
        ASSERT_EQ_UINT(rep->register_bits, 384);
        ASSERT_EQ_UINT(rep->memory_size, 48);
        ASSERT_EQ_UINT(rep->memory_align, 8);
        ASSERT_EQ_UINT(rep->ownership, XR_TARGET_OWNERSHIP_TRIVIAL);
        ASSERT_EQ_UINT(rep->root_kind, XR_TARGET_ROOT_NONE);
    }
    ASSERT_EQ_UINT(aggregate_rep_count, 1);

    uint32_t caller_count = 0;
    for (uint32_t i = 0; i < semantic->program_function_binding_count; i++) {
        const XrSemanticProgramFunctionBinding *binding =
            &semantic->program_function_bindings[i];
        if (binding->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) {
            ASSERT_TRUE(caller_count < 2);
            evidence.callers[caller_count++] = binding->semantic_function;
        } else {
            ASSERT_EQ_UINT(binding->flags, 0);
            ASSERT_EQ_UINT(evidence.callee, XR_SEMANTIC_INDEX_NONE);
            evidence.callee = binding->semantic_function;
        }
        ASSERT_EQ_UINT(xr_target_plan_function_execution_family_mask(
                           plan, binding->semantic_function),
                       XR_TARGET_EXECUTION_LEAF_VALUE_PRODUCT_TUPLE6);
    }
    ASSERT_EQ_UINT(caller_count, 2);
    ASSERT_NE(evidence.callee, XR_SEMANTIC_INDEX_NONE);
    ASSERT_EQ_UINT(plan->calls_count, 2);
    for (uint32_t i = 0; i < 2; i++) {
        XrTargetCallRecord *call = &plan->calls[i];
        ASSERT_EQ_UINT(call->id, i);
        ASSERT_EQ_UINT(call->callee_function, evidence.callee);
        ASSERT_TRUE(call->caller_function == evidence.callers[0] ||
                    call->caller_function == evidence.callers[1]);
        ASSERT_NULL(evidence.calls[call->caller_function == evidence.callers[0] ? 0 : 1]);
        evidence.calls[call->caller_function == evidence.callers[0] ? 0 : 1] = call;
        ASSERT_EQ_UINT(call->calling_convention, XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL);
        ASSERT_EQ_UINT(call->target_kind, XR_TARGET_CALL_TARGET_DIRECT_LOCAL);
        ASSERT_EQ_UINT(call->result_mode, XR_TARGET_CALL_CALLER_STORAGE);
        ASSERT_EQ_UINT(call->result_ownership, XR_TARGET_CALL_NONE);
        ASSERT_EQ_UINT(call->argument_count, 0);
        ASSERT_EQ_UINT(call->adapter_count, 0);
        ASSERT_EQ_UINT(call->result_register_rep, evidence.aggregate_rep);
        ASSERT_EQ_UINT(call->result_memory_rep, evidence.aggregate_rep);
        ASSERT_EQ_UINT(call->result_slot, call->caller_storage_slot);
    }
    ASSERT_NOT_NULL(evidence.calls[0]);
    ASSERT_NOT_NULL(evidence.calls[1]);

    for (uint32_t role = 0; role < 3; role++) {
        uint32_t function = role < 2 ? evidence.callers[role] : evidence.callee;
        uint32_t count = 0;
        const XrTargetInstructionRecord *rows =
            xr_target_plan_function_instructions(plan, function, &count);
        ASSERT_NOT_NULL(rows);
        ASSERT_EQ_UINT(count, role < 2 ? 15 : 14);
        uint32_t scalar_begin = role < 2 ? 1 : 0;
        uint32_t init = role < 2 ? 7 : 6;
        if (role < 2)
            ASSERT_EQ_UINT(rows[0].opcode, XR_TARGET_INSTRUCTION_CALL_DIRECT_AGGREGATE);
        for (uint32_t ordinal = 0; ordinal < 6; ordinal++) {
            ASSERT_EQ_UINT(rows[scalar_begin + ordinal].opcode,
                           role < 2
                               ? (ordinal == 2 ? XR_TARGET_INSTRUCTION_VALUE_PRODUCT_GET_U8
                                               : XR_TARGET_INSTRUCTION_AGGREGATE_GET_I64)
                               : (ordinal == 2 ? XR_TARGET_INSTRUCTION_CONST_U8
                                               : XR_TARGET_INSTRUCTION_CONST_I64));
            ASSERT_EQ_UINT(rows[init + 1u + ordinal].opcode,
                           ordinal == 2 ? XR_TARGET_INSTRUCTION_VALUE_PRODUCT_SET_U8
                                        : XR_TARGET_INSTRUCTION_VALUE_PRODUCT_SET_I64);
            ASSERT_EQ_UINT(rows[init + 1u + ordinal].immediate_bits,
                           evidence.layout->field_begin + ordinal);
        }
        ASSERT_EQ_UINT(rows[init].opcode, XR_TARGET_INSTRUCTION_VALUE_PRODUCT_INIT);
        ASSERT_EQ_UINT(rows[init].immediate_bits, evidence.layout_index);
        ASSERT_EQ_UINT(rows[count - 1u].opcode, XR_TARGET_INSTRUCTION_RETURN_AGGREGATE);
    }
    *out = evidence;
}

static void assert_leaf_product_target_mutations(XrSemanticPlan *semantic,
                                                 XrTargetProfile *profile,
                                                 XrTargetPlan *plan,
                                                 LeafProductTargetEvidence evidence) {
    uint32_t saved_u32 = evidence.layout->fixed_prefix_size;
    evidence.layout->fixed_prefix_size = 47;
    expect_leaf_target_verify_rejection(plan);
    evidence.layout->fixed_prefix_size = saved_u32;

    uint16_t saved_u16 = plan->machine_reps[evidence.aggregate_rep].register_bits;
    plan->machine_reps[evidence.aggregate_rep].register_bits = 128;
    expect_leaf_target_verify_rejection(plan);
    plan->machine_reps[evidence.aggregate_rep].register_bits = saved_u16;

    XrTargetFieldRecord *u8_field = &plan->fields[evidence.layout->field_begin + 2u];
    saved_u32 = u8_field->offset;
    u8_field->offset = 24;
    expect_leaf_target_verify_rejection(plan);
    u8_field->offset = saved_u32;

    uint8_t saved_u8 = evidence.calls[0]->result_mode;
    evidence.calls[0]->result_mode = XR_TARGET_CALL_VALUE;
    expect_leaf_target_verify_rejection(plan);
    evidence.calls[0]->result_mode = saved_u8;

    saved_u8 = evidence.calls[1]->result_ownership;
    evidence.calls[1]->result_ownership = XR_TARGET_CALL_RETURN_OWNED;
    expect_leaf_target_verify_rejection(plan);
    evidence.calls[1]->result_ownership = saved_u8;

    saved_u16 = evidence.calls[0]->adapter_count;
    XrTargetAdapterRecord fabricated_adapter = {0};
    XrTargetAdapterRecord *saved_adapters = plan->adapters;
    uint32_t saved_adapter_count = plan->adapters_count;
    plan->adapters = &fabricated_adapter;
    plan->adapters_count = 1;
    evidence.calls[0]->adapter_count = 1;
    expect_leaf_target_verify_rejection(plan);
    evidence.calls[0]->adapter_count = saved_u16;
    plan->adapters = saved_adapters;
    plan->adapters_count = saved_adapter_count;

    uint32_t caller_count = 0;
    const XrTargetInstructionRecord *caller_rows = xr_target_plan_function_instructions(
        plan, evidence.callers[0], &caller_count);
    ASSERT_NOT_NULL(caller_rows);
    ASSERT_EQ_UINT(caller_count, 15);
    XrTargetInstructionRecord *mutable_rows =
        &plan->instructions[(size_t) (caller_rows - plan->instructions)];
    uint64_t saved_u64 = mutable_rows[10].immediate_bits;
    mutable_rows[10].immediate_bits = evidence.layout->field_begin + 1u;
    expect_leaf_target_verify_rejection(plan);
    mutable_rows[10].immediate_bits = saved_u64;

    saved_u16 = mutable_rows[3].opcode;
    mutable_rows[3].opcode = XR_TARGET_INSTRUCTION_AGGREGATE_GET_I64;
    expect_leaf_target_verify_rejection(plan);
    mutable_rows[3].opcode = saved_u16;

    uint32_t callee_count = 0;
    const XrTargetInstructionRecord *callee_rows = xr_target_plan_function_instructions(
        plan, evidence.callee, &callee_count);
    ASSERT_NOT_NULL(callee_rows);
    ASSERT_EQ_UINT(callee_count, 14);
    mutable_rows = &plan->instructions[(size_t) (callee_rows - plan->instructions)];
    saved_u64 = mutable_rows[2].immediate_bits;
    mutable_rows[2].immediate_bits = 256;
    expect_leaf_target_verify_rejection(plan);
    mutable_rows[2].immediate_bits = saved_u64;

    saved_u32 = mutable_rows[0].function;
    mutable_rows[0].function = evidence.callers[0];
    expect_leaf_target_verify_rejection(plan);
    mutable_rows[0].function = saved_u32;
    resign_leaf_target(plan);
    ASSERT_TRUE(xr_target_plan_verify(plan, NULL, 0));

    XrSemanticOperationRecord *narrow = NULL;
    for (uint32_t i = 0; i < semantic->operation_count; i++)
        if (semantic->operations[i].opcode == XI_NARROW_U8)
            narrow = &semantic->operations[i];
    ASSERT_NOT_NULL(narrow);
    uint32_t saved_type = narrow->result_type;
    narrow->result_type = evidence.product_binding->semantic_type;
    expect_leaf_target_build_rejection(semantic, profile);
    narrow->result_type = saved_type;

    XrSemanticConstantRecord *u8_source = NULL;
    ASSERT_TRUE(narrow->operand_begin < semantic->operand_count);
    uint32_t source_value = semantic->operands[narrow->operand_begin].value;
    for (uint32_t i = 0; i < semantic->operation_count; i++) {
        XrSemanticOperationRecord *operation = &semantic->operations[i];
        if (operation->opcode == XI_CONST && operation->result_value == source_value &&
            operation->constant < semantic->constant_count)
            u8_source = &semantic->constants[operation->constant];
    }
    ASSERT_NOT_NULL(u8_source);
    int64_t saved_integer = u8_source->integer;
    u8_source->integer = 256;
    expect_leaf_target_build_rejection(semantic, profile);
    u8_source->integer = saved_integer;
    resign_leaf_semantic(semantic);
    ASSERT_TRUE(xr_semantic_plan_verify(semantic, NULL, 0));
}

static void assert_leaf_product_vm_execution(XrTargetPlan *plan,
                                             const LeafProductTargetEvidence *evidence) {
    ASSERT_NOT_NULL(plan);
    ASSERT_NOT_NULL(evidence);
    ASSERT_TRUE(xr_target_plan_verify(plan, NULL, 0));
    ASSERT_TRUE(xr_target_instruction_program_verify(plan, NULL, 0));
    XrFingerprint original_fingerprint = xr_target_plan_fingerprint(plan);
    static const XrTypedDispatchProvider providers[] = {
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
    };
    for (size_t provider = 0; provider < sizeof(providers) / sizeof(providers[0]); provider++) {
        for (uint32_t caller = 0; caller < 2; caller++) {
            XrTypedLeafValueProductTuple6 result = {0};
            XrTypedDispatchLeafValueProductTuple6Request request = {
                .verified_plan = plan,
                .required_plan_fingerprint = &original_fingerprint,
                .result = &result,
                .provider = providers[provider],
                .function = evidence->callers[caller],
            };
            ASSERT_EQ_UINT(xr_typed_dispatch_execute_leaf_value_product_tuple6(&request),
                           XR_TYPED_DISPATCH_OK);
            ASSERT_EQ_INT(result.field0, 1);
            ASSERT_EQ_INT(result.field1, 2);
            ASSERT_EQ_UINT(result.field2, 3);
            ASSERT_EQ_INT(result.field3, 4);
            ASSERT_EQ_INT(result.field4, 5);
            ASSERT_EQ_INT(result.field5, 6);

            XrFingerprint wrong_fingerprint = original_fingerprint;
            wrong_fingerprint.bytes[0] ^= UINT8_C(0x80);
            request.required_plan_fingerprint = &wrong_fingerprint;
            memset(&result, 0xA5, sizeof(result));
            ASSERT_EQ_UINT(xr_typed_dispatch_execute_leaf_value_product_tuple6(&request),
                           XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH);
            XrTypedLeafValueProductTuple6 zero = {0};
            ASSERT_TRUE(memcmp(&result, &zero, sizeof(result)) == 0);
        }
    }

    uint32_t caller_count = 0;
    const XrTargetInstructionRecord *caller_rows = xr_target_plan_function_instructions(
        plan, evidence->callers[0], &caller_count);
    ASSERT_NOT_NULL(caller_rows);
    ASSERT_EQ_UINT(caller_count, 15);
    XrTargetInstructionRecord *mutable_call =
        &plan->instructions[(size_t) (caller_rows - plan->instructions)];
    uint64_t saved_call_id = mutable_call->immediate_bits;
    mutable_call->immediate_bits = UINT32_MAX;
    resign_leaf_target(plan);
    ASSERT_FALSE(xr_target_plan_verify(plan, NULL, 0));
    XrFingerprint resigned_invalid_fingerprint = xr_target_plan_fingerprint(plan);
    XrTypedLeafValueProductTuple6 result = {0};
    XrTypedDispatchLeafValueProductTuple6Request request = {
        .verified_plan = plan,
        .required_plan_fingerprint = &resigned_invalid_fingerprint,
        .result = &result,
        .provider = XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        .function = evidence->callers[0],
    };
    ASSERT_EQ_UINT(xr_typed_dispatch_execute_leaf_value_product_tuple6(&request),
                   XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED);
    mutable_call->immediate_bits = saved_call_id;
    resign_leaf_target(plan);
    ASSERT_TRUE(xr_target_plan_verify(plan, NULL, 0));
    ASSERT_TRUE(xr_fingerprint_equal(xr_target_plan_fingerprint(plan), original_fingerprint));
}

static size_t substring_count(const char *text, const char *needle) {
    size_t count = 0;
    size_t length = needle ? strlen(needle) : 0;
    for (const char *match = length ? strstr(text, needle) : NULL; match;
         match = strstr(match + length, needle))
        count++;
    return count;
}

static void test_leaf_product_symbol(XrStableId identity, char out[48]) {
    static const char hex[] = "0123456789abcdef";
    memcpy(out, "xr_lp_", 6u);
    for (size_t i = 0; i < sizeof(identity.bytes); i++) {
        out[6u + i * 2u] = hex[identity.bytes[i] >> 4u];
        out[7u + i * 2u] = hex[identity.bytes[i] & UINT8_C(0x0f)];
    }
    out[38] = '\0';
}

static void assert_leaf_product_cgen_rejected(XiModule *module, XaotBundle *bundle,
                                               XaotArtifactKind artifact_kind,
                                               bool freestanding) {
    XiCgenCtx *cgen = xi_cgen_ctx_new();
    ASSERT_NOT_NULL(cgen);
    ASSERT_TRUE(xi_cgen_ctx_set_aot_bundle(cgen, bundle));
    xi_cgen_ctx_set_artifact_kind(cgen, artifact_kind);
    xi_cgen_ctx_set_freestanding_profile(cgen, freestanding);
    char *source = NULL;
    size_t source_size = 0;
    FILE *stream = xr_open_memstream(&source, &source_size);
    ASSERT_NOT_NULL(stream);
    xi_cgen_program(cgen, stream, module);
    ASSERT_EQ_INT(xr_close_memstream(stream, &source, &source_size), 0);
    ASSERT_TRUE(xi_cgen_has_error(cgen));
    ASSERT_EQ_UINT(source_size, 0);
    xr_free(source);
    xi_cgen_ctx_free(cgen);
}

static void assert_leaf_product_aot_cgen(XiModule *module, XrTargetPlan *plan,
                                         XrTargetPlan *freestanding_plan) {
    ASSERT_NOT_NULL(module);
    ASSERT_NOT_NULL(plan);
    char authority_error[512] = {0};
    ASSERT_MSG(xi_program_semantic_plan_verify_detached_leaf_authority(
                   module->init, xr_target_plan_semantic_plan(plan), authority_error,
                   sizeof(authority_error)),
               authority_error);
    XiModule *modules[] = {module};
    XaotBundle missing_plan_bundle = {0};
    ASSERT_TRUE(xaot_bundle_init(&missing_plan_bundle, modules, 1, 0));
    missing_plan_bundle.artifact_kind = XAOT_ARTIFACT_HOSTED_FRAGMENT;
    ASSERT_EQ_UINT(xi_cgen_leaf_product_program_route(module, NULL),
                   XI_CGEN_LEAF_PRODUCT_ROUTE_REJECT);
    XiCgenCtx *missing_plan_cgen = xi_cgen_ctx_new();
    ASSERT_NOT_NULL(missing_plan_cgen);
    ASSERT_TRUE(xi_cgen_ctx_set_aot_bundle(missing_plan_cgen, &missing_plan_bundle));
    xi_cgen_ctx_set_artifact_kind(missing_plan_cgen, XAOT_ARTIFACT_HOSTED_FRAGMENT);
    char *missing_plan_source = NULL;
    size_t missing_plan_source_size = 0;
    FILE *missing_plan_stream =
        xr_open_memstream(&missing_plan_source, &missing_plan_source_size);
    ASSERT_NOT_NULL(missing_plan_stream);
    xi_cgen_program(missing_plan_cgen, missing_plan_stream, module);
    ASSERT_EQ_INT(xr_close_memstream(missing_plan_stream, &missing_plan_source,
                                    &missing_plan_source_size), 0);
    ASSERT_TRUE(xi_cgen_has_error(missing_plan_cgen));
    ASSERT_EQ_UINT(missing_plan_source_size, 0);
    xr_free(missing_plan_source);
    xi_cgen_ctx_free(missing_plan_cgen);
    xaot_bundle_free(&missing_plan_bundle);

    XaotBundle bundle = {0};
    ASSERT_TRUE(xaot_bundle_init(&bundle, modules, 1, 0));
    bundle.artifact_kind = XAOT_ARTIFACT_HOSTED_FRAGMENT;
    ASSERT_MSG(xaot_bundle_set_program_target_plan(&bundle, plan),
               bundle.error_msg ? bundle.error_msg : "bundle target-plan install failed");
    ASSERT_EQ_UINT(xi_cgen_leaf_product_program_route(module, plan),
                   XI_CGEN_LEAF_PRODUCT_ROUTE_CLAIM);

    XrSemanticPlan *route_semantic =
        (XrSemanticPlan *) xr_target_plan_semantic_plan(plan);
    uint32_t saved_route_family = route_semantic->program_provenance.program_family;
    route_semantic->program_provenance.program_family =
        XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_DIRECT_CALL;
    ASSERT_EQ_UINT(xi_cgen_leaf_product_program_route(module, plan),
                   XI_CGEN_LEAF_PRODUCT_ROUTE_REJECT);
    assert_leaf_product_cgen_rejected(module, &bundle,
                                      XAOT_ARTIFACT_HOSTED_FRAGMENT, false);
    route_semantic->program_provenance.program_family = saved_route_family;

    XiModule nonproduct_module = *module;
    nonproduct_module.program_semantic_closure = NULL;
    XiModule *nonproduct_modules[] = {&nonproduct_module};
    XaotBundle product_plan_only_bundle = {0};
    ASSERT_TRUE(xaot_bundle_init(&product_plan_only_bundle, nonproduct_modules, 1, 0));
    product_plan_only_bundle.artifact_kind = XAOT_ARTIFACT_HOSTED_FRAGMENT;
    product_plan_only_bundle.program_target_plan = xr_target_plan_retain(plan);
    ASSERT_NOT_NULL(product_plan_only_bundle.program_target_plan);
    ASSERT_EQ_UINT(xi_cgen_leaf_product_program_route(&nonproduct_module, plan),
                   XI_CGEN_LEAF_PRODUCT_ROUTE_REJECT);
    assert_leaf_product_cgen_rejected(&nonproduct_module, &product_plan_only_bundle,
                                      XAOT_ARTIFACT_HOSTED_FRAGMENT, false);
    xaot_bundle_free(&product_plan_only_bundle);

    uint32_t value_count = 0;
    uint32_t rejected_aggregate_values = 0;
    const XrTargetValueRepRecord *aggregate_value = NULL;
    const XrTargetValueRepRecord *values =
        xr_target_plan_value_reps(plan, &value_count);
    ASSERT_NOT_NULL(values);
    for (uint32_t i = 0; i < value_count; i++) {
        const XrTargetMachineRepRecord *rep =
            xr_target_plan_machine_rep(plan, values[i].register_rep);
        if (!rep || rep->kind != XR_MACHINE_REP_AGGREGATE)
            continue;
        XrCAggregateProjection projection = {0};
        ASSERT_FALSE(xr_c_aggregate_projection(plan, &values[i], &projection));
        aggregate_value = &values[i];
        rejected_aggregate_values++;
    }
    ASSERT_TRUE(rejected_aggregate_values > 0u);
    ASSERT_NOT_NULL(aggregate_value);
    XrSemanticPlan *mutable_semantic =
        (XrSemanticPlan *) xr_target_plan_semantic_plan(plan);
    XrSemanticProgramTypeBinding *product_binding = NULL;
    for (uint32_t i = 0; i < mutable_semantic->program_type_binding_count; i++)
        if (mutable_semantic->program_type_bindings[i].kind ==
            XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT)
            product_binding = &mutable_semantic->program_type_bindings[i];
    ASSERT_NOT_NULL(product_binding);
    uint8_t saved_product_kind = product_binding->kind;
    product_binding->kind = XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR;
    XrCAggregateProjection corrupted_projection = {0};
    ASSERT_FALSE(xr_c_aggregate_projection(plan, aggregate_value,
                                           &corrupted_projection));
    product_binding->kind = saved_product_kind;
    XrCEmissionPlan *legacy_emission = NULL;
    ASSERT_FALSE(xr_c_emission_plan_build(
        plan, xr_target_profile_fingerprint(xr_target_plan_profile(plan)),
        &legacy_emission, authority_error, sizeof(authority_error)));
    ASSERT_NULL(legacy_emission);

    for (uint16_t i = 0; i < module->nfuncs; i++)
        ASSERT_EQ_UINT(
            xaot_boundary_leaf_aggregate_function_status(
                &bundle, module->functions[i], NULL, NULL, NULL, 0),
            XAOT_LEAF_AGGREGATE_TARGET_INVALID);
    bundle.module_emission_plans[0] =
        (const XrCEmissionPlan *) (uintptr_t) UINT32_C(1);
    ASSERT_NULL(xaot_bundle_emission_plan_for_module(&bundle, 0));
    for (uint16_t i = 0; i < module->nfuncs; i++)
        ASSERT_NULL(xaot_bundle_emission_plan_for_func(&bundle, module->functions[i]));
    bundle.module_emission_plans[0] = NULL;

    assert_leaf_product_cgen_rejected(module, &bundle,
                                      XAOT_ARTIFACT_EXECUTABLE, false);
    bundle.artifact_kind = XAOT_ARTIFACT_EXECUTABLE;
    assert_leaf_product_cgen_rejected(module, &bundle,
                                      XAOT_ARTIFACT_HOSTED_FRAGMENT, false);
    bundle.artifact_kind = XAOT_ARTIFACT_HOSTED_FRAGMENT;
    assert_leaf_product_cgen_rejected(module, &bundle,
                                      XAOT_ARTIFACT_HOSTED_FRAGMENT, true);

    XaotBundle freestanding_bundle = {0};
    ASSERT_TRUE(xaot_bundle_init(&freestanding_bundle, modules, 1, 0));
    freestanding_bundle.artifact_kind = XAOT_ARTIFACT_HOSTED_FRAGMENT;
    ASSERT_TRUE(xaot_bundle_set_program_target_plan(&freestanding_bundle,
                                                    freestanding_plan));
    char *freestanding_direct_source = NULL;
    size_t freestanding_direct_size = 0;
    ASSERT_FALSE(xr_c_leaf_value_product_program_emit(
        freestanding_plan, module, &freestanding_direct_source,
        &freestanding_direct_size, authority_error, sizeof(authority_error)));
    ASSERT_NULL(freestanding_direct_source);
    ASSERT_EQ_UINT(freestanding_direct_size, 0);
    assert_leaf_product_cgen_rejected(module, &freestanding_bundle,
                                      XAOT_ARTIFACT_HOSTED_FRAGMENT, false);
    xaot_bundle_free(&freestanding_bundle);

    XiCgenCtx *cgen = xi_cgen_ctx_new();
    ASSERT_NOT_NULL(cgen);
    ASSERT_TRUE(xi_cgen_ctx_set_aot_bundle(cgen, &bundle));
    xi_cgen_ctx_set_artifact_kind(cgen, XAOT_ARTIFACT_HOSTED_FRAGMENT);
    char *source = NULL;
    size_t source_size = 0;
    FILE *stream = xr_open_memstream(&source, &source_size);
    ASSERT_NOT_NULL(stream);
    xi_cgen_program(cgen, stream, module);
    ASSERT_EQ_INT(xr_close_memstream(stream, &source, &source_size), 0);
    ASSERT_FALSE(xi_cgen_has_error(cgen));
    ASSERT_NOT_NULL(source);
    ASSERT_TRUE(source_size > 0);
    ASSERT_NOT_NULL(strstr(source, "_Static_assert(sizeof(xr_leaf_product_tuple6) == 48"));
    ASSERT_NOT_NULL(strstr(source, "offsetof(xr_leaf_product_tuple6, field2) == 16"));
    ASSERT_NULL(strstr(source, "XrValue"));
    ASSERT_NULL(strstr(source, "XR_TAG_TUPLE"));
    ASSERT_NULL(strstr(source, "XR_TAG_PLACE"));
    ASSERT_NULL(strstr(source, "XR_TAG_AGG_REF"));
    ASSERT_NULL(strstr(source, "XI_TUPLE"));
    ASSERT_NULL(strstr(source, "xrt_tuple"));
    ASSERT_NULL(strstr(source, "xr_aggregate_ref"));
    ASSERT_NULL(strstr(source, "dynamic"));
    ASSERT_NULL(strstr(source, "boxed"));
    ASSERT_NULL(strstr(source, "xrt_"));
    ASSERT_NULL(strstr(source, "lookup"));
    ASSERT_NULL(strstr(source, "import"));
    ASSERT_NULL(strstr(source, "shared"));
    ASSERT_NULL(strstr(source, "module"));
    ASSERT_NULL(strstr(source, "int main("));
    for (uint16_t i = 0; i < module->nfuncs; i++) {
        ASSERT_NOT_NULL(module->functions[i]);
        ASSERT_NOT_NULL(module->functions[i]->name);
        ASSERT_NULL(strstr(source, module->functions[i]->name));
    }

    const XrProgramSemanticClosure *closure = module->program_semantic_closure;
    ASSERT_NOT_NULL(closure);
    char symbols[3][48] = {{0}};
    uint32_t callee = UINT32_MAX;
    const char *entry_symbols[2] = {NULL, NULL};
    uint32_t entry = 0;
    for (uint32_t i = 0; i < 3; i++) {
        const XrProgramSemanticFunctionRecord *function =
            xr_program_semantic_closure_function(closure, i);
        ASSERT_NOT_NULL(function);
        test_leaf_product_symbol(function->id, symbols[i]);
        ASSERT_NOT_NULL(strstr(source, symbols[i]));
        if (function->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY)
            entry_symbols[entry++] = symbols[i];
        else
            callee = i;
    }
    ASSERT_EQ_UINT(entry, 2);
    ASSERT_NE(callee, UINT32_MAX);
    ASSERT_EQ_UINT(substring_count(source, symbols[callee]), 4);

    const char *native_path = getenv("XR_LEAF_PRODUCT_NATIVE_C_OUTPUT");
    if (native_path && native_path[0]) {
        FILE *native = fopen(native_path, "wb");
        ASSERT_NOT_NULL(native);
        ASSERT_EQ_UINT(fwrite(source, 1, source_size, native), source_size);
        ASSERT_TRUE(fprintf(
                        native,
                        "\nstatic int xr_test_equal(xr_leaf_product_tuple6 v) {\n"
                        "    return v.field0 == 1 && v.field1 == 2 && v.field2 == 3 &&\n"
                        "           v.field3 == 4 && v.field4 == 5 && v.field5 == 6;\n"
                        "}\n"
                        "int main(void) {\n"
                        "    return xr_test_equal(%s()) && xr_test_equal(%s()) ? 0 : 1;\n"
                        "}\n",
                        entry_symbols[0], entry_symbols[1]) > 0);
        ASSERT_EQ_INT(fclose(native), 0);
    }

    xr_free(source);
    xi_cgen_ctx_free(cgen);
    xaot_bundle_free(&bundle);
}

static void assert_leaf_aggregate_vm_execution(XrTargetPlan *plan,
                                               const LeafAggregateTargetEvidence *evidence) {
    ASSERT_NOT_NULL(plan);
    ASSERT_NOT_NULL(evidence);
    ASSERT_NOT_NULL(evidence->call);
    ASSERT_TRUE(xr_target_plan_verify(plan, NULL, 0));

    XrFingerprint original_fingerprint = xr_target_plan_fingerprint(plan);
    static const XrTypedDispatchProvider providers[] = {
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
    };
    for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
        XrTypedLeafAggregateI64x2 result = {.fields = {-1, -1}};
        XrTypedDispatchLeafAggregateI64x2Request request = {
            .verified_plan = plan,
            .required_plan_fingerprint = &original_fingerprint,
            .result = &result,
            .provider = providers[i],
            .function = evidence->call->caller_function,
        };
        ASSERT_EQ_UINT(xr_typed_dispatch_execute_leaf_aggregate_i64x2(&request),
                       XR_TYPED_DISPATCH_OK);
        ASSERT_EQ_INT(result.fields[0], 41);
        ASSERT_EQ_INT(result.fields[1], 1);

        XrFingerprint wrong_fingerprint = original_fingerprint;
        wrong_fingerprint.bytes[0] ^= UINT8_C(0x80);
        request.required_plan_fingerprint = &wrong_fingerprint;
        result.fields[0] = -1;
        result.fields[1] = -1;
        ASSERT_EQ_UINT(xr_typed_dispatch_execute_leaf_aggregate_i64x2(&request),
                       XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH);
        ASSERT_EQ_INT(result.fields[0], 0);
        ASSERT_EQ_INT(result.fields[1], 0);
    }

    uint32_t saved_result_slot = evidence->call->result_slot;
    evidence->call->result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE;
    resign_leaf_target(plan);
    ASSERT_FALSE(xr_target_plan_verify(plan, NULL, 0));
    XrFingerprint resigned_invalid_fingerprint = xr_target_plan_fingerprint(plan);
    XrTypedLeafAggregateI64x2 result = {.fields = {-1, -1}};
    XrTypedDispatchLeafAggregateI64x2Request request = {
        .verified_plan = plan,
        .required_plan_fingerprint = &resigned_invalid_fingerprint,
        .result = &result,
        .provider = XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        .function = evidence->call->caller_function,
    };
    ASSERT_EQ_UINT(xr_typed_dispatch_execute_leaf_aggregate_i64x2(&request),
                   XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED);
    ASSERT_EQ_INT(result.fields[0], 0);
    ASSERT_EQ_INT(result.fields[1], 0);

    evidence->call->result_slot = saved_result_slot;
    resign_leaf_target(plan);
    ASSERT_TRUE(xr_target_plan_verify(plan, NULL, 0));
    ASSERT_TRUE(xr_fingerprint_equal(xr_target_plan_fingerprint(plan), original_fingerprint));
}

static void assert_leaf_aggregate_aot_boundary(XiModule *module, XrTargetPlan *plan,
                                               const LeafAggregateTargetEvidence *evidence) {
    ASSERT_NOT_NULL(module);
    ASSERT_NOT_NULL(plan);
    ASSERT_NOT_NULL(evidence);
    ASSERT_NOT_NULL(evidence->call);
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(plan);
    XiFunc *caller = find_semantic_function(module, semantic, evidence->call->caller_function);
    XiFunc *callee = find_semantic_function(module, semantic, evidence->call->callee_function);
    XiValue *call = find_bound_call(caller);
    ASSERT_NOT_NULL(caller);
    ASSERT_NOT_NULL(callee);
    ASSERT_NOT_NULL(call);

    XiModule *modules[] = {module};
    XaotBundle bundle = {0};
    char authority_error[512] = {0};
    ASSERT_MSG(xi_program_semantic_plan_verify_detached_leaf_authority(
                   module->init, semantic, authority_error, sizeof(authority_error)),
               authority_error);
    ASSERT_TRUE(xaot_bundle_init(&bundle, modules, 1, 0));
    ASSERT_TRUE(xaot_bundle_set_program_target_plan(&bundle, plan));
    XaotLeafAggregateTargetView view = {0};
    char error[512] = {0};
    ASSERT_EQ_UINT(
        xaot_boundary_leaf_aggregate_call_view(&bundle, caller, call, &view, error, sizeof(error)),
        XAOT_LEAF_AGGREGATE_TARGET_FOUND);
    ASSERT_EQ_PTR(view.target_plan, plan);
    ASSERT_EQ_PTR(view.call, evidence->call);
    ASSERT_EQ_PTR(view.callee, callee);
    ASSERT_EQ_PTR(view.argument_value, call->args[1]);
    ASSERT_TRUE(xr_fingerprint_equal(view.target_fingerprint, xr_target_plan_fingerprint(plan)));
    ASSERT_NULL(xaot_boundary_resolve_direct_call_target(&bundle, caller, call, NULL));

    plan->fingerprint.bytes[0] ^= UINT8_C(1);
    memset(&view, 0, sizeof(view));
    ASSERT_EQ_UINT(
        xaot_boundary_leaf_aggregate_call_view(&bundle, caller, call, &view, error, sizeof(error)),
        XAOT_LEAF_AGGREGATE_TARGET_INVALID);
    ASSERT_NULL(xaot_boundary_resolve_direct_call_target(&bundle, caller, call, NULL));
    plan->fingerprint.bytes[0] ^= UINT8_C(1);
    ASSERT_TRUE(xr_target_plan_verify(plan, error, sizeof(error)));

    xaot_bundle_free(&bundle);
}

static void assert_leaf_aggregate_target_shape(XrSemanticPlan *semantic, XrTargetPlan *plan,
                                               LeafAggregateTargetEvidence *out_evidence) {
    LeafAggregateTargetEvidence evidence = {
        .layout_index = XR_SEMANTIC_INDEX_NONE,
        .aggregate_rep = XR_SEMANTIC_INDEX_NONE,
    };
    ASSERT_NOT_NULL(out_evidence);
    ASSERT_NOT_NULL(semantic);
    ASSERT_NOT_NULL(plan);
    ASSERT_EQ_UINT(xr_target_plan_schema_version(plan), XR_TARGET_PLAN_SCHEMA_VERSION);
    ASSERT_EQ_UINT(XR_TARGET_PLAN_SCHEMA_VERSION, 51);
    ASSERT_TRUE(xr_target_plan_verify(plan, NULL, 0));
    ASSERT_TRUE(xr_target_plan_fingerprint_is_intact(plan));
    ASSERT_TRUE(xr_fingerprint_equal(xr_target_plan_semantic_fingerprint(plan),
                                     xr_semantic_plan_fingerprint(semantic)));

    for (uint32_t i = 0; i < semantic->program_type_binding_count; i++) {
        XrSemanticProgramTypeBinding *binding = &semantic->program_type_bindings[i];
        if (binding->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE)
            continue;
        ASSERT_NULL(evidence.aggregate_binding);
        evidence.aggregate_binding = binding;
    }
    ASSERT_NOT_NULL(evidence.aggregate_binding);
    ASSERT_EQ_UINT(evidence.aggregate_binding->field_count, 2);

    for (uint32_t i = 0; i < plan->layouts_count; i++) {
        if (plan->layouts[i].semantic_type != evidence.aggregate_binding->semantic_type)
            continue;
        ASSERT_EQ_UINT(evidence.layout_index, XR_SEMANTIC_INDEX_NONE);
        evidence.layout_index = i;
        evidence.layout = &plan->layouts[i];
    }
    ASSERT_NOT_NULL(evidence.layout);
    ASSERT_EQ_UINT(evidence.layout->kind, XR_TARGET_LAYOUT_AGGREGATE);
    ASSERT_EQ_UINT(evidence.layout->fixed_prefix_size, 16);
    ASSERT_EQ_UINT(evidence.layout->align, 8);
    ASSERT_EQ_UINT(evidence.layout->field_count, 2);
    ASSERT_TRUE(evidence.layout->extent < plan->extents_count);
    ASSERT_EQ_UINT(plan->extents[evidence.layout->extent].kind, XR_TARGET_EXTENT_FIXED);

    for (uint32_t ordinal = 0; ordinal < evidence.layout->field_count; ordinal++) {
        uint32_t field_index = evidence.layout->field_begin + ordinal;
        uint32_t program_field_index = evidence.aggregate_binding->field_begin + ordinal;
        ASSERT_TRUE(field_index < plan->fields_count);
        ASSERT_TRUE(program_field_index < semantic->program_type_field_binding_count);
        const XrSemanticProgramTypeFieldBinding *program_field =
            &semantic->program_type_field_bindings[program_field_index];
        const XrTargetFieldRecord *field = &plan->fields[field_index];
        ASSERT_EQ_UINT(program_field->owner_program_row, evidence.aggregate_binding->program_row);
        ASSERT_EQ_UINT(program_field->declaration_ordinal, ordinal);
        ASSERT_EQ_UINT(field->layout, evidence.layout_index);
        ASSERT_EQ_UINT(field->semantic_field, program_field->declaration_ordinal);
        ASSERT_EQ_UINT(field->semantic_name, XR_SEMANTIC_INDEX_NONE);
        ASSERT_EQ_UINT(field->offset, ordinal * 8u);
        ASSERT_EQ_UINT(field->size, 8);
        ASSERT_EQ_UINT(field->align, 8);
        ASSERT_TRUE(field->memory_rep < plan->machine_reps_count);
        ASSERT_EQ_UINT(plan->machine_reps[field->memory_rep].kind, XR_MACHINE_REP_I64);
        ASSERT_EQ_UINT(field->root_kind, XR_TARGET_ROOT_NONE);
    }

    uint32_t aggregate_rep_count = 0;
    for (uint32_t i = 0; i < plan->machine_reps_count; i++) {
        XrTargetMachineRepRecord *rep = &plan->machine_reps[i];
        if (rep->kind != XR_MACHINE_REP_AGGREGATE || rep->detail != evidence.layout_index)
            continue;
        evidence.aggregate_rep = i;
        aggregate_rep_count++;
        ASSERT_EQ_UINT(rep->register_bits, 128);
        ASSERT_EQ_UINT(rep->memory_size, 16);
        ASSERT_EQ_UINT(rep->memory_align, 8);
        ASSERT_EQ_UINT(rep->root_kind, XR_TARGET_ROOT_NONE);
        ASSERT_EQ_UINT(rep->ownership, XR_TARGET_OWNERSHIP_TRIVIAL);
        ASSERT_EQ_UINT(rep->null_encoding, XR_TARGET_NULL_NOT_NULLABLE);
    }
    ASSERT_EQ_UINT(aggregate_rep_count, 1);

    ASSERT_EQ_UINT(semantic->program_call_binding_count, 1);
    const XrSemanticProgramCallBinding *program_call = &semantic->program_call_bindings[0];
    ASSERT_EQ_UINT(plan->calls_count, 1);
    evidence.call = &plan->calls[0];
    ASSERT_EQ_UINT(evidence.call->semantic_operation, program_call->operation);
    ASSERT_EQ_UINT(evidence.call->callee_function, program_call->target_function);
    ASSERT_EQ_UINT(evidence.call->calling_convention, XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL);
    ASSERT_EQ_UINT(evidence.call->target_kind, XR_TARGET_CALL_TARGET_DIRECT_LOCAL);
    ASSERT_EQ_UINT(evidence.call->result_mode, XR_TARGET_CALL_CALLER_STORAGE);
    ASSERT_EQ_UINT(evidence.call->result_ownership, XR_TARGET_CALL_NONE);
    ASSERT_EQ_UINT(evidence.call->result_register_rep, evidence.aggregate_rep);
    ASSERT_EQ_UINT(evidence.call->result_memory_rep, evidence.aggregate_rep);
    ASSERT_EQ_UINT(evidence.call->caller_storage_slot, evidence.call->result_slot);
    ASSERT_EQ_UINT(evidence.call->argument_count, 1);
    ASSERT_EQ_UINT(evidence.call->adapter_count, 0);
    ASSERT_EQ_UINT(evidence.call->flags, 0);
    ASSERT_TRUE(evidence.call->result_slot < plan->slots_count);
    const XrTargetSlotRecord *result_slot = &plan->slots[evidence.call->result_slot];
    ASSERT_EQ_UINT(result_slot->function, evidence.call->caller_function);
    ASSERT_EQ_UINT(result_slot->semantic_value, evidence.call->result_value);
    ASSERT_EQ_UINT(result_slot->role, XR_TARGET_SLOT_TEMPORARY);
    ASSERT_EQ_UINT(result_slot->size, 16);
    ASSERT_EQ_UINT(result_slot->align, 8);
    ASSERT_EQ_UINT(result_slot->register_rep, evidence.aggregate_rep);
    ASSERT_EQ_UINT(result_slot->memory_rep, evidence.aggregate_rep);
    ASSERT_EQ_UINT(result_slot->ownership, XR_TARGET_OWNERSHIP_TRIVIAL);
    const XrTargetValueRepRecord *result_value_rep =
        xr_target_plan_value_rep(plan, evidence.call->result_value);
    ASSERT_TRUE(result_value_rep != NULL);
    ASSERT_EQ_UINT(result_value_rep->slot, evidence.call->result_slot);
    ASSERT_EQ_UINT(result_value_rep->register_rep, evidence.aggregate_rep);
    ASSERT_EQ_UINT(result_value_rep->memory_rep, evidence.aggregate_rep);

    ASSERT_EQ_UINT(plan->call_arguments_count, 1);
    evidence.argument = &plan->call_arguments[0];
    ASSERT_EQ_UINT(evidence.argument->call, evidence.call->id);
    ASSERT_EQ_UINT(evidence.argument->ordinal, 0);
    ASSERT_EQ_UINT(evidence.argument->mode, XR_TARGET_CALL_VALUE);
    ASSERT_EQ_UINT(evidence.argument->ownership, XR_TARGET_CALL_READ);
    ASSERT_EQ_UINT(evidence.argument->transfer_mode, XR_TRANSFER_SHARE);
    ASSERT_EQ_UINT(evidence.argument->flags, 0);
    ASSERT_EQ_UINT(evidence.argument->semantic_operand,
                   semantic->operations[program_call->operation].operand_begin + 1u);
    ASSERT_TRUE(evidence.argument->caller_slot < plan->slots_count);
    ASSERT_TRUE(evidence.argument->callee_slot < plan->slots_count);
    ASSERT_EQ_UINT(evidence.argument->register_rep, evidence.aggregate_rep);
    ASSERT_EQ_UINT(evidence.argument->memory_rep, evidence.aggregate_rep);
    ASSERT_EQ_UINT(evidence.argument->callee_register_rep, evidence.aggregate_rep);
    ASSERT_EQ_UINT(evidence.argument->callee_memory_rep, evidence.aggregate_rep);
    const XrTargetSlotRecord *caller_slot = &plan->slots[evidence.argument->caller_slot];
    const XrTargetSlotRecord *callee_slot = &plan->slots[evidence.argument->callee_slot];
    ASSERT_EQ_UINT(caller_slot->function, evidence.call->caller_function);
    ASSERT_EQ_UINT(caller_slot->semantic_value, evidence.argument->semantic_value);
    ASSERT_EQ_UINT(caller_slot->register_rep, evidence.aggregate_rep);
    ASSERT_EQ_UINT(caller_slot->memory_rep, evidence.aggregate_rep);
    ASSERT_EQ_UINT(callee_slot->function, evidence.call->callee_function);
    ASSERT_EQ_UINT(callee_slot->role, XR_TARGET_SLOT_PARAMETER);
    ASSERT_EQ_UINT(callee_slot->register_rep, evidence.aggregate_rep);
    ASSERT_EQ_UINT(callee_slot->memory_rep, evidence.aggregate_rep);
    ASSERT_TRUE(evidence.argument->callee_parameter < semantic->parameter_count);
    ASSERT_EQ_UINT(callee_slot->semantic_value,
                   semantic->parameters[evidence.argument->callee_parameter].value);
    const XrTargetValueRepRecord *caller_value_rep =
        xr_target_plan_value_rep(plan, evidence.argument->semantic_value);
    const XrTargetValueRepRecord *callee_value_rep = xr_target_plan_value_rep(
        plan, semantic->parameters[evidence.argument->callee_parameter].value);
    ASSERT_TRUE(caller_value_rep != NULL);
    ASSERT_EQ_UINT(caller_value_rep->slot, evidence.argument->caller_slot);
    ASSERT_EQ_UINT(caller_value_rep->register_rep, evidence.aggregate_rep);
    ASSERT_EQ_UINT(caller_value_rep->memory_rep, evidence.aggregate_rep);
    ASSERT_TRUE(callee_value_rep != NULL);
    ASSERT_EQ_UINT(callee_value_rep->slot, evidence.argument->callee_slot);
    ASSERT_EQ_UINT(callee_value_rep->register_rep, evidence.aggregate_rep);
    ASSERT_EQ_UINT(callee_value_rep->memory_rep, evidence.aggregate_rep);
    *out_evidence = evidence;
}

static void assert_leaf_aggregate_target_mutations(XrSemanticPlan *semantic, XrTargetPlan *plan,
                                                   LeafAggregateTargetEvidence evidence) {
    ASSERT_TRUE(xr_target_instruction_program_verify(plan, NULL, 0));
    ASSERT_TRUE(evidence.layout->extent < plan->extents_count);
    XrTargetExtentRecord *extent = &plan->extents[evidence.layout->extent];
    uint32_t saved_u32 = extent->alignment;
    extent->alignment = 8;
    expect_leaf_target_verify_rejection(plan);
    extent->alignment = saved_u32;

    uint16_t saved_u16 = evidence.layout->align;
    evidence.layout->align = 16;
    expect_leaf_target_verify_rejection(plan);
    evidence.layout->align = saved_u16;

    saved_u32 = evidence.layout->fixed_prefix_size;
    evidence.layout->fixed_prefix_size = 24;
    expect_leaf_target_verify_rejection(plan);
    evidence.layout->fixed_prefix_size = saved_u32;

    XrTargetFieldRecord *first_field = &plan->fields[evidence.layout->field_begin];
    saved_u32 = first_field->offset;
    first_field->offset = 8;
    expect_leaf_target_verify_rejection(plan);
    first_field->offset = saved_u32;

    uint16_t saved_rep = evidence.argument->callee_memory_rep;
    evidence.argument->callee_memory_rep = first_field->memory_rep;
    expect_leaf_target_verify_rejection(plan);
    evidence.argument->callee_memory_rep = saved_rep;

    saved_u32 = evidence.call->caller_storage_slot;
    evidence.call->caller_storage_slot = XR_SEMANTIC_INDEX_NONE;
    expect_leaf_target_verify_rejection(plan);
    evidence.call->caller_storage_slot = saved_u32;

    uint8_t saved_u8 = evidence.call->result_mode;
    evidence.call->result_mode = XR_TARGET_CALL_VALUE;
    expect_leaf_target_verify_rejection(plan);
    evidence.call->result_mode = saved_u8;

    uint32_t caller_instruction_count = 0;
    const XrTargetInstructionRecord *caller_instructions = xr_target_plan_function_instructions(
        plan, evidence.call->caller_function, &caller_instruction_count);
    ASSERT_NOT_NULL(caller_instructions);
    ASSERT_EQ_UINT(caller_instruction_count, 5);
    ASSERT_EQ_UINT(caller_instructions[3].opcode, XR_TARGET_INSTRUCTION_CALL_DIRECT_AGGREGATE);
    ASSERT_EQ_UINT(caller_instructions[3].immediate_bits, evidence.call->id);
    ASSERT_EQ_UINT(caller_instructions[4].opcode, XR_TARGET_INSTRUCTION_RETURN_AGGREGATE);
    ASSERT_EQ_UINT(caller_instructions[4].operand_slots[0], evidence.call->result_slot);
    XrTargetInstructionRecord *constant_instruction =
        &plan->instructions[(size_t) (&caller_instructions[0] - plan->instructions)];
    uint64_t saved_u64 = constant_instruction->immediate_bits;
    constant_instruction->immediate_bits ^= UINT64_C(1);
    expect_leaf_target_verify_rejection(plan);
    constant_instruction->immediate_bits = saved_u64;

    XrTargetInstructionRecord *caller_make_instruction =
        &plan->instructions[(size_t) (&caller_instructions[2] - plan->instructions)];
    saved_u64 = caller_make_instruction->immediate_bits;
    caller_make_instruction->immediate_bits = UINT32_MAX;
    expect_leaf_target_verify_rejection(plan);
    caller_make_instruction->immediate_bits = saved_u64;

    XrTargetSlotRecord *caller_constant_slot = &plan->slots[constant_instruction->result_slot];
    saved_u32 = caller_constant_slot->semantic_operation;
    caller_constant_slot->semantic_operation = XR_SEMANTIC_INDEX_NONE;
    expect_leaf_target_verify_rejection(plan);
    caller_constant_slot->semantic_operation = saved_u32;

    XrTargetInstructionRecord *call_instruction =
        &plan->instructions[(size_t) (&caller_instructions[3] - plan->instructions)];
    saved_u64 = call_instruction->immediate_bits;
    call_instruction->immediate_bits = UINT32_MAX;
    expect_leaf_target_verify_rejection(plan);
    call_instruction->immediate_bits = saved_u64;

    saved_u32 = evidence.call->semantic_operation;
    evidence.call->semantic_operation = caller_constant_slot->semantic_operation;
    expect_leaf_target_verify_rejection(plan);
    evidence.call->semantic_operation = saved_u32;

    saved_u32 = evidence.argument->semantic_operand;
    evidence.argument->semantic_operand--;
    expect_leaf_target_verify_rejection(plan);
    evidence.argument->semantic_operand = saved_u32;

    XrTargetInstructionRecord *return_instruction =
        &plan->instructions[(size_t) (&caller_instructions[4] - plan->instructions)];
    saved_u32 = return_instruction->operand_slots[0];
    return_instruction->operand_slots[0] = XR_TARGET_INSTRUCTION_SLOT_NONE;
    expect_leaf_target_verify_rejection(plan);
    return_instruction->operand_slots[0] = saved_u32;

    uint32_t callee_instruction_count = 0;
    const XrTargetInstructionRecord *callee_instructions = xr_target_plan_function_instructions(
        plan, evidence.call->callee_function, &callee_instruction_count);
    ASSERT_NOT_NULL(callee_instructions);
    ASSERT_EQ_UINT(callee_instruction_count, 5);
    ASSERT_EQ_UINT(callee_instructions[1].opcode, XR_TARGET_INSTRUCTION_AGGREGATE_GET_I64);
    ASSERT_NE(callee_instructions[1].immediate_bits, evidence.layout->field_begin);

    XrTargetInstructionRecord *saved_instructions = plan->instructions;
    uint32_t saved_instruction_count = plan->instructions_count;
    plan->instructions = NULL;
    plan->instructions_count = 0;
    expect_leaf_instruction_verify_rejection(plan);
    expect_leaf_target_verify_rejection(plan);
    plan->instructions = saved_instructions;
    plan->instructions_count = saved_instruction_count;

    XrTargetInstructionRecord only_caller[5];
    memcpy(only_caller, caller_instructions, sizeof(only_caller));
    for (uint32_t i = 0; i < 5; i++)
        only_caller[i].id = i;
    plan->instructions = only_caller;
    plan->instructions_count = 5;
    expect_leaf_instruction_verify_rejection(plan);
    expect_leaf_target_verify_rejection(plan);
    plan->instructions = saved_instructions;
    plan->instructions_count = saved_instruction_count;

    XrTargetInstructionRecord only_callee[5];
    memcpy(only_callee, callee_instructions, sizeof(only_callee));
    for (uint32_t i = 0; i < 5; i++)
        only_callee[i].id = i;
    plan->instructions = only_callee;
    plan->instructions_count = 5;
    expect_leaf_instruction_verify_rejection(plan);
    expect_leaf_target_verify_rejection(plan);
    plan->instructions = saved_instructions;
    plan->instructions_count = saved_instruction_count;
    resign_leaf_target(plan);
    ASSERT_TRUE(xr_target_plan_verify(plan, NULL, 0));

    XrTargetInstructionRecord *parameter_instruction =
        &plan->instructions[(size_t) (&callee_instructions[0] - plan->instructions)];
    saved_u64 = parameter_instruction->immediate_bits;
    parameter_instruction->immediate_bits = 1;
    expect_leaf_target_verify_rejection(plan);
    parameter_instruction->immediate_bits = saved_u64;

    XrTargetInstructionRecord *get_instruction =
        &plan->instructions[(size_t) (&callee_instructions[1] - plan->instructions)];
    saved_u64 = get_instruction->immediate_bits;
    get_instruction->immediate_bits = evidence.layout->field_begin;
    expect_leaf_target_verify_rejection(plan);
    get_instruction->immediate_bits = saved_u64;

    XrTargetInstructionRecord *callee_make_instruction =
        &plan->instructions[(size_t) (&callee_instructions[3] - plan->instructions)];
    saved_u64 = callee_make_instruction->immediate_bits;
    callee_make_instruction->immediate_bits = UINT32_MAX;
    expect_leaf_target_verify_rejection(plan);
    callee_make_instruction->immediate_bits = saved_u64;

    XrTargetSlotRecord *callee_get_slot = &plan->slots[get_instruction->result_slot];
    saved_u32 = callee_get_slot->semantic_value;
    callee_get_slot->semantic_value =
        plan->slots[callee_instructions[2].result_slot].semantic_value;
    expect_leaf_target_verify_rejection(plan);
    callee_get_slot->semantic_value = saved_u32;

    XrTargetInstructionRecord *callee_return_instruction =
        &plan->instructions[(size_t) (&callee_instructions[4] - plan->instructions)];
    saved_u32 = callee_return_instruction->operand_slots[0];
    callee_return_instruction->operand_slots[0] = callee_instructions[1].result_slot;
    expect_leaf_target_verify_rejection(plan);
    callee_return_instruction->operand_slots[0] = saved_u32;

    saved_u32 = evidence.argument->callee_slot;
    evidence.argument->callee_slot = evidence.call->result_slot;
    expect_leaf_target_verify_rejection(plan);
    evidence.argument->callee_slot = saved_u32;

    saved_u16 = evidence.call->adapter_count;
    XrTargetAdapterRecord fabricated_adapter = {0};
    XrTargetAdapterRecord *saved_adapters = plan->adapters;
    uint32_t saved_adapter_count = plan->adapters_count;
    plan->adapters = &fabricated_adapter;
    plan->adapters_count = 1;
    evidence.call->adapter_count = 1;
    expect_leaf_target_verify_rejection(plan);
    evidence.call->adapter_count = saved_u16;
    plan->adapters = saved_adapters;
    plan->adapters_count = saved_adapter_count;

    XrStableId saved_generation = semantic->program_provenance.generation_identity;
    semantic->program_provenance.generation_identity.bytes[0] ^= UINT8_C(1);
    resign_leaf_semantic(semantic);
    plan->semantic_fingerprint = semantic->fingerprint;
    expect_leaf_target_verify_rejection(plan);
    semantic->program_provenance.generation_identity = saved_generation;
    resign_leaf_semantic(semantic);
    plan->semantic_fingerprint = semantic->fingerprint;
    resign_leaf_target(plan);
    ASSERT_TRUE(xr_target_plan_verify(plan, NULL, 0));
}

static void assert_leaf_aggregate_target_input_rejections(
    XrSemanticPlan *semantic, XrTargetProfile *profile,
    const XrProgramSemanticTypeRecord *foreign_type,
    const XrProgramSemanticTypeRecord *foreign_source_type) {
    ASSERT_NOT_NULL(semantic);
    ASSERT_NOT_NULL(foreign_type);
    ASSERT_NOT_NULL(foreign_source_type);
    XrSemanticProgramTypeBinding *aggregate = NULL;
    for (uint32_t i = 0; i < semantic->program_type_binding_count; i++) {
        if (semantic->program_type_bindings[i].kind ==
            XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE)
            aggregate = &semantic->program_type_bindings[i];
    }
    ASSERT_NOT_NULL(aggregate);

    uint32_t saved_binding_count = semantic->program_type_binding_count;
    uint32_t saved_type_count = semantic->program_provenance.type_count;
    semantic->program_type_binding_count--;
    semantic->program_provenance.type_count--;
    expect_leaf_target_build_rejection(semantic, profile);
    semantic->program_type_binding_count = saved_binding_count;
    semantic->program_provenance.type_count = saved_type_count;

    uint32_t saved_row = aggregate->program_row;
    aggregate->program_row = XR_SEMANTIC_INDEX_NONE;
    expect_leaf_target_build_rejection(semantic, profile);
    aggregate->program_row = saved_row;

    XrStableId saved_program_type = aggregate->program_type;
    aggregate->program_type = foreign_type->id;
    expect_leaf_target_build_rejection(semantic, profile);
    aggregate->program_type = saved_program_type;

    XrStableId saved_source_class = aggregate->source_class_identity;
    aggregate->source_class_identity = foreign_source_type->declaration_identity;
    expect_leaf_target_build_rejection(semantic, profile);
    aggregate->source_class_identity = saved_source_class;

    resign_leaf_semantic(semantic);
    ASSERT_TRUE(xr_semantic_plan_verify(semantic, NULL, 0));
}

TEST(stable_rows_survive_mutation_and_ownership_gates) {
    XrCompilerSessionConfig session_config = {0};
    XrCompilerSession *session = xr_compiler_session_new(&session_config);
    ASSERT_NOT_NULL(session);
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, session, "xi-scalar-binding-direct", kScalarSource));
    ASSERT_TRUE(fixture_publish(&fixture));

    char error[512] = {0};
    XrTargetProfile *profile = NULL;
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&profile, error, sizeof(error)));
    XrProgramSemanticClosure *closure = NULL;
    XrScalarCallDecision decision = {0};
    ASSERT_TRUE(build_authorities(&fixture, profile, &closure, &decision, error, sizeof(error)));
    XrProgramSemanticClosure *retained = xr_program_semantic_closure_retain(closure);
    ASSERT_TRUE(retained == closure);

    XiProgramSemanticInput input = {
        .closure = closure,
        .decision = &decision,
    };
    XiFunc *root = xi_lower_program(fixture.typed, NULL, false, &input);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(root->module);
    ASSERT_TRUE(xi_module_set_identity(root->module, fixture.spec->canonical));
    ASSERT_TRUE(xi_module_take_program_semantics(root->module, &closure, &decision, profile, 0,
                                                 error, sizeof(error)));
    ASSERT_NULL(closure);
    bool verified = xi_program_semantic_verify(root->module, profile, error, sizeof(error));
    ASSERT_TRUE(verified);

    XiFunc *caller = find_function(root->module, retained, decision.caller_function);
    XiFunc *callee = find_function(root->module, retained, decision.callee_function);
    XiValue *call = find_bound_call(caller);
    ASSERT_NOT_NULL(caller);
    ASSERT_NOT_NULL(callee);
    ASSERT_NOT_NULL(call);
    ASSERT_EQ_UINT(callee->inline_policy, XI_INLINE_PRESERVE_CALL);

    uint32_t saved_function_index = caller->psc_function_index;
    caller->psc_function_index = callee->psc_function_index;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, profile, error, sizeof(error)));
    caller->psc_function_index = saved_function_index;

    uint32_t saved_locator_kind = caller->psc_declaration_locator.kind;
    caller->psc_declaration_locator.kind ^= UINT32_C(1);
    ASSERT_FALSE(xi_program_semantic_verify(root->module, profile, error, sizeof(error)));
    caller->psc_declaration_locator.kind = saved_locator_kind;

    uint32_t saved_call_index = call->psc_call_index;
    call->psc_call_index = XI_PSC_ROW_NONE;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, profile, error, sizeof(error)));
    call->psc_call_index = saved_call_index;

    uint32_t saved_source_kind = call->source_kind;
    call->source_kind ^= UINT32_C(1);
    ASSERT_FALSE(xi_program_semantic_verify(root->module, profile, error, sizeof(error)));
    call->source_kind = saved_source_kind;

    root->module->scalar_call_decision->generation_id.bytes[0] ^= UINT8_C(0x40);
    ASSERT_FALSE(xi_program_semantic_verify(root->module, profile, error, sizeof(error)));
    root->module->scalar_call_decision->generation_id.bytes[0] ^= UINT8_C(0x40);
    root->module->scalar_call_decision->call_identity.bytes[0] ^= UINT8_C(0x20);
    ASSERT_FALSE(xi_program_semantic_verify(root->module, profile, error, sizeof(error)));
    root->module->scalar_call_decision->call_identity.bytes[0] ^= UINT8_C(0x20);
    ASSERT_TRUE(xi_program_semantic_verify(root->module, profile, error, sizeof(error)));

    dce_and_mark_tree_optimized(root);
    ASSERT_TRUE(xi_module_set_identity(root->module, fixture.spec->canonical));
    XrSemanticPlan *semantic = NULL;
    bool semantic_built = xr_semantic_plan_build(root, &semantic, error, sizeof(error));
    ASSERT_TRUE(semantic_built);
    ASSERT_NOT_NULL(semantic);
    ASSERT_TRUE(xi_program_semantic_plan_verify(root, semantic, profile, error, sizeof(error)));
    XrTargetPlan *target_plan = NULL;
    bool target_built = xr_target_plan_build(semantic, profile, &target_plan, error, sizeof(error));
    ASSERT_TRUE(target_built);
    ASSERT_NOT_NULL(target_plan);
    ASSERT_TRUE(xr_target_plan_verify(target_plan, error, sizeof(error)));
    ASSERT_TRUE(xr_target_plan_is_frozen(target_plan));
    ASSERT_TRUE(xr_target_plan_is_verified(target_plan));
    ASSERT_TRUE(xr_target_plan_fingerprint_is_intact(target_plan));

    XrTargetInstructionRecord *ordinary_const = NULL;
    for (uint32_t i = 0; i < target_plan->instructions_count; i++)
        if (target_plan->instructions[i].opcode == XR_TARGET_INSTRUCTION_CONST_I64) {
            ordinary_const = &target_plan->instructions[i];
            break;
        }
    ASSERT_NOT_NULL(ordinary_const);
    uint16_t saved_ordinary_opcode = ordinary_const->opcode;
    ordinary_const->opcode = XR_TARGET_INSTRUCTION_CONST_U8;
    ASSERT_EQ_UINT(xi_cgen_leaf_product_program_route(root->module, target_plan),
                   XI_CGEN_LEAF_PRODUCT_ROUTE_ORDINARY);
    ordinary_const->opcode = saved_ordinary_opcode;
    ASSERT_TRUE(xr_target_plan_fingerprint_is_intact(target_plan));

    ASSERT_EQ_UINT(xr_target_plan_schema_version(target_plan), XR_TARGET_PLAN_SCHEMA_VERSION);
    ASSERT_EQ_UINT(xr_target_plan_completed_family_mask(target_plan), XR_TARGET_REQUIRED_FAMILIES);
    ASSERT_EQ_PTR(xr_target_plan_semantic_plan(target_plan), semantic);
    ASSERT_EQ_PTR(xr_target_plan_profile(target_plan), profile);
    ASSERT_TRUE(xr_fingerprint_equal(xr_target_plan_semantic_fingerprint(target_plan),
                                     xr_semantic_plan_fingerprint(semantic)));
    ASSERT_TRUE(
        xr_fingerprint_equal(xr_target_profile_fingerprint(xr_target_plan_profile(target_plan)),
                             decision.target_profile_fingerprint));
    ASSERT_EQ_UINT(target_plan->functions_count, 3);
    ASSERT_EQ_UINT(target_plan->value_reps_count, 10);
    ASSERT_EQ_UINT(target_plan->slots_count, 7);
    ASSERT_EQ_UINT(target_plan->instructions_count, 7);
    ASSERT_EQ_UINT(target_plan->calls_count, 1);
    ASSERT_EQ_UINT(target_plan->call_arguments_count, 1);
    ASSERT_EQ_UINT(target_plan->capabilities_count, 2);
    ASSERT_EQ_UINT(target_plan->capabilities[0].capability, XR_TARGET_CAPABILITY_ALLOCATOR);
    ASSERT_EQ_UINT(target_plan->capabilities[0].flags, XR_TARGET_CAPABILITY_REQUIRED);
    ASSERT_EQ_UINT(target_plan->capabilities[1].capability, XR_TARGET_CAPABILITY_PANIC);
    ASSERT_EQ_UINT(target_plan->capabilities[1].flags, XR_TARGET_CAPABILITY_REQUIRED);

    const XrSemanticProgramCallBinding *program_call = &semantic->program_call_bindings[0];
    const XrSemanticOperationRecord *semantic_call =
        xr_semantic_plan_operation(semantic, program_call->operation);
    ASSERT_NOT_NULL(semantic_call);
    const XrSemanticOperandRecord *semantic_callee =
        &semantic->operands[semantic_call->operand_begin];
    const XrTargetValueRepRecord *target_callee =
        xr_target_plan_value_rep(target_plan, semantic_callee->value);
    ASSERT_NOT_NULL(target_callee);
    ASSERT_EQ_UINT(target_callee->slot, XR_SEMANTIC_INDEX_NONE);
    const XrTargetCallRecord *target_call = &target_plan->calls[0];
    const XrSemanticCallTargetRecord *semantic_target =
        xr_semantic_plan_call_target(semantic, target_call->semantic_call_target);
    ASSERT_NOT_NULL(semantic_target);
    ASSERT_EQ_UINT(target_call->id, 0);
    ASSERT_EQ_UINT(target_call->semantic_operation, program_call->operation);
    ASSERT_EQ_UINT(target_call->caller_function, semantic_call->function);
    ASSERT_EQ_UINT(target_call->callee_function, program_call->target_function);
    ASSERT_EQ_UINT(semantic_target->operation, program_call->operation);
    ASSERT_EQ_UINT(semantic_target->function, program_call->target_function);
    ASSERT_EQ_UINT(target_plan->functions[target_call->caller_function].semantic_function,
                   semantic_call->function);
    ASSERT_EQ_UINT(target_plan->functions[target_call->callee_function].semantic_function,
                   program_call->target_function);
    ASSERT_EQ_UINT(target_call->native_abi, decision.native_abi);
    ASSERT_EQ_UINT(target_call->calling_convention, XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL);
    ASSERT_EQ_UINT(target_call->target_kind, XR_TARGET_CALL_TARGET_DIRECT_LOCAL);
    ASSERT_EQ_UINT(target_call->result_mode, XR_TARGET_CALL_VALUE);
    ASSERT_EQ_UINT(target_call->result_ownership, XR_TARGET_CALL_NONE);
    ASSERT_EQ_UINT(target_call->argument_count, 1);
    ASSERT_EQ_UINT(target_call->adapter_count, 0);
    ASSERT_EQ_UINT(target_call->flags, 0);
    ASSERT_EQ_UINT(target_call->error_mode, XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL);
    ASSERT_EQ_UINT(target_call->error_slot, XR_SEMANTIC_INDEX_NONE);
    ASSERT_NE(target_call->result_slot, XR_SEMANTIC_INDEX_NONE);
    const XrTargetMachineRepRecord *result_register_rep =
        xr_target_plan_machine_rep(target_plan, target_call->result_register_rep);
    const XrTargetMachineRepRecord *result_memory_rep =
        xr_target_plan_machine_rep(target_plan, target_call->result_memory_rep);
    ASSERT_TRUE(result_register_rep && result_memory_rep &&
                result_register_rep->kind == XR_MACHINE_REP_I64 &&
                result_memory_rep->kind == XR_MACHINE_REP_I64 &&
                result_register_rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
                result_memory_rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL);
    const XrTargetValueRepRecord *result_value_rep =
        xr_target_plan_value_rep(target_plan, target_call->result_value);
    ASSERT_TRUE(result_value_rep && result_value_rep->slot == target_call->result_slot);

    const XrTargetCallArgumentRecord *target_argument = &target_plan->call_arguments[0];
    ASSERT_EQ_UINT(target_argument->call, target_call->id);
    ASSERT_EQ_UINT(target_argument->ordinal, 0);
    ASSERT_EQ_UINT(target_argument->mode, XR_TARGET_CALL_VALUE);
    ASSERT_EQ_UINT(target_argument->ownership, XR_TARGET_CALL_CONSUME);
    ASSERT_EQ_UINT(target_argument->transfer_mode, XR_TRANSFER_SHARE);
    ASSERT_EQ_UINT(target_argument->flags, 0);
    ASSERT_EQ_UINT(target_argument->semantic_operand, semantic_call->operand_begin + 1u);
    ASSERT_EQ_UINT(target_argument->semantic_value,
                   semantic->operands[target_argument->semantic_operand].value);
    ASSERT_EQ_UINT(target_argument->callee_parameter, 0);
    const XrTargetMachineRepRecord *argument_register_rep =
        xr_target_plan_machine_rep(target_plan, target_argument->register_rep);
    const XrTargetMachineRepRecord *argument_memory_rep =
        xr_target_plan_machine_rep(target_plan, target_argument->memory_rep);
    const XrTargetMachineRepRecord *callee_register_rep =
        xr_target_plan_machine_rep(target_plan, target_argument->callee_register_rep);
    const XrTargetMachineRepRecord *callee_memory_rep =
        xr_target_plan_machine_rep(target_plan, target_argument->callee_memory_rep);
    ASSERT_TRUE(argument_register_rep && argument_memory_rep && callee_register_rep &&
                callee_memory_rep && argument_register_rep->kind == XR_MACHINE_REP_I64 &&
                argument_memory_rep->kind == XR_MACHINE_REP_I64 &&
                callee_register_rep->kind == XR_MACHINE_REP_I64 &&
                callee_memory_rep->kind == XR_MACHINE_REP_I64);

    uint32_t callee_instruction_count = 0;
    const XrTargetInstructionRecord *callee_instructions = xr_target_plan_function_instructions(
        target_plan, target_call->callee_function, &callee_instruction_count);
    ASSERT_NOT_NULL(callee_instructions);
    ASSERT_EQ_UINT(callee_instruction_count, 4);
    ASSERT_EQ_UINT(
        xr_target_plan_function_execution_family_mask(target_plan, target_call->callee_function),
        XR_TARGET_EXECUTION_SCALAR_I64_CLOSED);
    ASSERT_EQ_UINT(callee_instructions[0].opcode, XR_TARGET_INSTRUCTION_PARAM_I64);
    ASSERT_EQ_UINT(callee_instructions[0].result_slot, target_argument->callee_slot);
    ASSERT_EQ_UINT(callee_instructions[1].opcode, XR_TARGET_INSTRUCTION_CONST_I64);
    ASSERT_EQ_UINT(callee_instructions[1].immediate_bits, 1);
    ASSERT_EQ_UINT(callee_instructions[2].opcode, XR_TARGET_INSTRUCTION_ADD_WRAP_I64);
    ASSERT_EQ_UINT(callee_instructions[2].operand_slots[0], callee_instructions[0].result_slot);
    ASSERT_EQ_UINT(callee_instructions[2].operand_slots[1], callee_instructions[1].result_slot);
    ASSERT_EQ_UINT(callee_instructions[3].opcode, XR_TARGET_INSTRUCTION_RETURN_I64);
    ASSERT_EQ_UINT(callee_instructions[3].operand_slots[0], callee_instructions[2].result_slot);

    uint32_t caller_instruction_count = 0;
    const XrTargetInstructionRecord *caller_instructions = xr_target_plan_function_instructions(
        target_plan, target_call->caller_function, &caller_instruction_count);
    ASSERT_NOT_NULL(caller_instructions);
    ASSERT_EQ_UINT(caller_instruction_count, 3);
    ASSERT_EQ_UINT(
        xr_target_plan_function_execution_family_mask(target_plan, target_call->caller_function),
        XR_TARGET_EXECUTION_SCALAR_I64_CLOSED);
    ASSERT_EQ_UINT(caller_instructions[0].opcode, XR_TARGET_INSTRUCTION_CONST_I64);
    ASSERT_EQ_UINT(caller_instructions[0].immediate_bits, 41);
    ASSERT_EQ_UINT(caller_instructions[0].result_slot, target_argument->caller_slot);
    ASSERT_EQ_UINT(caller_instructions[1].opcode, XR_TARGET_INSTRUCTION_CALL_DIRECT_I64);
    ASSERT_EQ_UINT(caller_instructions[1].immediate_bits, target_call->id);
    ASSERT_EQ_UINT(caller_instructions[1].operand_count, 0);
    ASSERT_EQ_UINT(caller_instructions[1].operand_slots[0], XR_TARGET_INSTRUCTION_SLOT_NONE);
    ASSERT_EQ_UINT(caller_instructions[1].result_slot, target_call->result_slot);
    ASSERT_EQ_UINT(caller_instructions[2].opcode, XR_TARGET_INSTRUCTION_RETURN_I64);
    ASSERT_EQ_UINT(caller_instructions[2].operand_slots[0], target_call->result_slot);

    XrFingerprint original_target_fingerprint = xr_target_plan_fingerprint(target_plan);
    XrTargetValueRepRecord *mutable_target_callee =
        &target_plan->value_reps[(size_t) (target_callee - target_plan->value_reps)];
    mutable_target_callee->slot = target_argument->caller_slot;
    xr_target_plan_compute_fingerprint(target_plan, &target_plan->fingerprint);
    ASSERT_FALSE(xr_target_plan_verify(target_plan, error, sizeof(error)));
    mutable_target_callee->slot = XR_SEMANTIC_INDEX_NONE;
    xr_target_plan_compute_fingerprint(target_plan, &target_plan->fingerprint);
    ASSERT_TRUE(xr_target_plan_verify(target_plan, error, sizeof(error)));
    ASSERT_TRUE(xr_fingerprint_equal(target_plan->fingerprint, original_target_fingerprint));

    static const XrTypedDispatchProvider providers[] = {
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
    };
    for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
        int64_t result = -1;
        XrTypedDispatchI64Request request = {
            .verified_plan = target_plan,
            .required_plan_fingerprint = &original_target_fingerprint,
            .result = &result,
            .provider = providers[i],
            .function = target_call->caller_function,
        };
        ASSERT_EQ_UINT(xr_typed_dispatch_execute_i64(&request), XR_TYPED_DISPATCH_OK);
        ASSERT_EQ_INT(result, 42);

        XrFingerprint wrong_fingerprint = original_target_fingerprint;
        wrong_fingerprint.bytes[0] ^= UINT8_C(0x80);
        request.required_plan_fingerprint = &wrong_fingerprint;
        result = -1;
        ASSERT_EQ_UINT(xr_typed_dispatch_execute_i64(&request),
                       XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH);
        ASSERT_EQ_INT(result, 0);
    }

    uint16_t saved_native_abi = target_plan->calls[0].native_abi;
    target_plan->calls[0].native_abi = XR_TARGET_ABI_NONE;
    xr_target_call_compute_fingerprint(target_plan, 0, &target_plan->calls[0].fingerprint);
    xr_target_plan_compute_fingerprint(target_plan, &target_plan->fingerprint);
    ASSERT_FALSE(xr_target_plan_verify(target_plan, error, sizeof(error)));
    target_plan->calls[0].native_abi = saved_native_abi;
    xr_target_call_compute_fingerprint(target_plan, 0, &target_plan->calls[0].fingerprint);
    xr_target_plan_compute_fingerprint(target_plan, &target_plan->fingerprint);
    ASSERT_TRUE(xr_target_plan_verify(target_plan, error, sizeof(error)));
    ASSERT_TRUE(xr_fingerprint_equal(target_plan->fingerprint, original_target_fingerprint));

    XrTargetInstructionRecord *mutable_call_instruction =
        &target_plan->instructions[caller_instructions[1].id];
    uint64_t saved_call_immediate = mutable_call_instruction->immediate_bits;
    mutable_call_instruction->immediate_bits = UINT32_MAX;
    xr_target_plan_compute_fingerprint(target_plan, &target_plan->fingerprint);
    XrFingerprint resigned_invalid_fingerprint = target_plan->fingerprint;
    for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
        int64_t result = -1;
        XrTypedDispatchI64Request request = {
            .verified_plan = target_plan,
            .required_plan_fingerprint = &resigned_invalid_fingerprint,
            .result = &result,
            .provider = providers[i],
            .function = target_call->caller_function,
        };
        ASSERT_EQ_UINT(xr_typed_dispatch_execute_i64(&request),
                       XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED);
        ASSERT_EQ_INT(result, 0);
    }
    ASSERT_FALSE(xr_target_plan_verify(target_plan, error, sizeof(error)));
    mutable_call_instruction->immediate_bits = saved_call_immediate;
    xr_target_plan_compute_fingerprint(target_plan, &target_plan->fingerprint);
    ASSERT_TRUE(xr_target_plan_verify(target_plan, error, sizeof(error)));
    ASSERT_TRUE(xr_fingerprint_equal(target_plan->fingerprint, original_target_fingerprint));

    XrTargetPlan *second_target_plan = NULL;
    ASSERT_TRUE(xr_target_plan_build(semantic, profile, &second_target_plan, error, sizeof(error)));
    ASSERT_NOT_NULL(second_target_plan);
    ASSERT_TRUE(xr_fingerprint_equal(xr_target_plan_fingerprint(second_target_plan),
                                     original_target_fingerprint));
    uint8_t *target_encoded = NULL;
    size_t target_encoded_size = 0;
    uint8_t *second_target_encoded = NULL;
    size_t second_target_encoded_size = 0;
    ASSERT_TRUE(xr_xtp_encode_plan(target_plan, &target_encoded, &target_encoded_size, error,
                                   sizeof(error)));
    ASSERT_TRUE(xr_xtp_encode_plan(second_target_plan, &second_target_encoded,
                                   &second_target_encoded_size, error, sizeof(error)));
    ASSERT_EQ_UINT(target_encoded_size, second_target_encoded_size);
    ASSERT_TRUE(memcmp(target_encoded, second_target_encoded, target_encoded_size) == 0);
    XrXtpCandidate *target_candidate = NULL;
    ASSERT_TRUE(xr_xtp_decode_candidate(target_encoded, target_encoded_size, &target_candidate,
                                        error, sizeof(error)));
    XrTargetPlan *roundtrip_target_plan = NULL;
    ASSERT_TRUE(xr_xtp_materialize_target_plan(target_candidate, semantic, profile,
                                               &roundtrip_target_plan, error, sizeof(error)));
    ASSERT_NOT_NULL(roundtrip_target_plan);
    ASSERT_TRUE(xr_target_plan_verify(roundtrip_target_plan, error, sizeof(error)));
    ASSERT_TRUE(xr_fingerprint_equal(xr_target_plan_fingerprint(roundtrip_target_plan),
                                     original_target_fingerprint));
    uint8_t *target_roundtrip = NULL;
    size_t target_roundtrip_size = 0;
    ASSERT_TRUE(xr_xtp_encode_plan(roundtrip_target_plan, &target_roundtrip, &target_roundtrip_size,
                                   error, sizeof(error)));
    ASSERT_EQ_UINT(target_roundtrip_size, target_encoded_size);
    ASSERT_TRUE(memcmp(target_roundtrip, target_encoded, target_encoded_size) == 0);
    xr_xtp_encoded_free(target_roundtrip);
    xr_target_plan_free(roundtrip_target_plan);
    xr_xtp_candidate_release(target_candidate);
    xr_xtp_encoded_free(second_target_encoded);
    xr_xtp_encoded_free(target_encoded);
    xr_target_plan_free(second_target_plan);
    ASSERT_EQ_UINT(xr_semantic_plan_function_count(semantic), 3);
    ASSERT_EQ_UINT(xr_semantic_plan_call_target_count(semantic), 1);
    XrFingerprint retained_fingerprint = xr_program_semantic_closure_fingerprint(retained);
    XrGenerationClosureId retained_generation = xr_program_semantic_closure_generation_id(retained);
    ASSERT_EQ_UINT(semantic->program_provenance.schema,
                   XR_SEMANTIC_PROGRAM_PROVENANCE_SCHEMA_VERSION);
    ASSERT_EQ_UINT(semantic->program_provenance.program_schema,
                   xr_program_semantic_closure_schema(retained));
    ASSERT_EQ_UINT(semantic->program_function_binding_count, 2);
    ASSERT_EQ_UINT(semantic->program_call_binding_count, 1);
    ASSERT_TRUE(xr_fingerprint_equal(semantic->program_provenance.program_fingerprint,
                                     retained_fingerprint));
    ASSERT_TRUE(memcmp(semantic->program_provenance.generation_identity.bytes,
                       retained_generation.bytes, sizeof(retained_generation.bytes)) == 0);

    uint32_t saved_target = semantic->program_call_bindings[0].target_function;
    semantic->program_call_bindings[0].target_function = XR_SEMANTIC_INDEX_NONE;
    ASSERT_FALSE(xi_program_semantic_plan_verify(root, semantic, profile, error, sizeof(error)));
    semantic->program_call_bindings[0].target_function = saved_target;
    semantic->program_provenance.program_fingerprint.bytes[0] ^= UINT8_C(0x80);
    ASSERT_FALSE(xi_program_semantic_plan_verify(root, semantic, profile, error, sizeof(error)));
    semantic->program_provenance.program_fingerprint.bytes[0] ^= UINT8_C(0x80);
    uint32_t saved_provenance_schema = semantic->program_provenance.schema;
    semantic->program_provenance.schema = 0;
    ASSERT_FALSE(xi_program_semantic_plan_verify(root, semantic, profile, error, sizeof(error)));
    semantic->program_provenance.schema = saved_provenance_schema;
    ASSERT_TRUE(xi_program_semantic_plan_verify(root, semantic, profile, error, sizeof(error)));
    XrFingerprint saved_plan_fingerprint = semantic->fingerprint;
    semantic->program_call_bindings[0].reserved = 1;
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    ASSERT_FALSE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    semantic->program_call_bindings[0].reserved = 0;
    semantic->fingerprint = saved_plan_fingerprint;

    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    ASSERT_TRUE(xr_xsm_encode(semantic, &encoded, &encoded_size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    ASSERT_TRUE(xr_xsm_decode(encoded, encoded_size, &decoded, error, sizeof(error)));
    ASSERT_TRUE(decoded != NULL && decoded->program_function_binding_count == 2 &&
                decoded->program_call_binding_count == 1 &&
                xr_fingerprint_equal(xr_semantic_plan_fingerprint(decoded),
                                     xr_semantic_plan_fingerprint(semantic)));
    uint8_t *roundtrip = NULL;
    size_t roundtrip_size = 0;
    ASSERT_TRUE(xr_xsm_encode(decoded, &roundtrip, &roundtrip_size, error, sizeof(error)));
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
    memcpy(hostile + XR_XSM_HEADER_SIZE - XR_FINGERPRINT_BYTES, hostile_fingerprint.bytes,
           sizeof(hostile_fingerprint.bytes));
    xr_sha256(hostile + XR_XSM_HEADER_SIZE, encoded_size - XR_XSM_HEADER_SIZE, hostile + 24u);
    decoded = NULL;
    ASSERT_FALSE(xr_xsm_decode(hostile, encoded_size, &decoded, error, sizeof(error)));
    ASSERT_NULL(decoded);
    xr_free(hostile);

    hostile = (uint8_t *) xr_malloc(encoded_size);
    ASSERT_NOT_NULL(hostile);
    memcpy(hostile, encoded, encoded_size);
    semantic->program_provenance.schema = 0;
    xr_semantic_plan_compute_fingerprint(semantic, &hostile_fingerprint);
    semantic->program_provenance.schema = saved_provenance_schema;
    memset(hostile + XR_XSM_HEADER_SIZE + 96u, 0, sizeof(uint32_t));
    memcpy(hostile + XR_XSM_HEADER_SIZE - XR_FINGERPRINT_BYTES, hostile_fingerprint.bytes,
           sizeof(hostile_fingerprint.bytes));
    xr_sha256(hostile + XR_XSM_HEADER_SIZE, encoded_size - XR_XSM_HEADER_SIZE, hostile + 24u);
    decoded = NULL;
    ASSERT_FALSE(xr_xsm_decode(hostile, encoded_size, &decoded, error, sizeof(error)));
    ASSERT_NULL(decoded);
    xr_free(hostile);
    xr_free(encoded);

    XrProgramSemanticClosure *second = NULL;
    XrScalarCallDecision second_decision = {0};
    ASSERT_TRUE(
        build_authorities(&fixture, profile, &second, &second_decision, error, sizeof(error)));
    XrProgramSemanticClosure *second_owner = second;
    ASSERT_FALSE(xi_module_take_program_semantics(root->module, &second, &second_decision, profile,
                                                  0, error, sizeof(error)));
    ASSERT_TRUE(second == second_owner);
    xr_program_semantic_closure_free(second);

    xi_func_free(root);
    ASSERT_TRUE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    xr_target_plan_free(target_plan);
    xr_semantic_plan_free(semantic);
    ASSERT_TRUE(xr_program_semantic_closure_verify(retained, error, sizeof(error)));
    xr_program_semantic_closure_free(retained);
    xr_target_profile_free(profile);
    fixture_cleanup(&fixture);
    xr_compiler_session_delete(session);
}

TEST(leaf_aggregate_canonical_semantic_shape_mismatch_is_rejected) {
    XrCompilerSessionConfig session_config = {0};
    XrCompilerSession *session = xr_compiler_session_new(&session_config);
    ASSERT_NOT_NULL(session);
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, session, "xi-leaf-aggregate-shape-mismatch",
                                kLeafAggregateCanonicalShapeMismatchSource));
    ASSERT_TRUE(fixture_publish(&fixture));
    const XrProgramSemanticClosure *published =
        xa_typed_program_program_semantic_closure(fixture.typed);
    ASSERT_NOT_NULL(published);
    ASSERT_EQ_UINT(xr_program_semantic_closure_family(published),
                   XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL);
    XrProgramSemanticClosure *closure =
        xr_program_semantic_closure_retain((XrProgramSemanticClosure *) published);
    ASSERT_NOT_NULL(closure);
    XiProgramSemanticInput input = {
        .closure = closure,
        .decision = NULL,
    };
    XiFunc *root = xi_lower_program(fixture.typed, NULL, false, &input);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(root->module);
    ASSERT_TRUE(xi_module_set_identity(root->module, fixture.spec->canonical));
    char error[512] = {0};
    ASSERT_TRUE(
        xi_module_take_program_semantics(root->module, &closure, NULL, NULL, 0, error,
                                         sizeof(error)));
    ASSERT_NULL(closure);
    ASSERT_TRUE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));

    prepare_leaf_tree_for_semantic_plan(root);
    ASSERT_TRUE(xi_module_set_identity(root->module, fixture.spec->canonical));
    XrSemanticPlan *semantic = NULL;
    ASSERT_MSG(xr_semantic_plan_build(root, &semantic, error, sizeof(error)), error);
    ASSERT_NOT_NULL(semantic);
    ASSERT_TRUE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    const XrSemanticProgramProvenance *provenance = xr_semantic_plan_program_provenance(semantic);
    ASSERT_NOT_NULL(provenance);
    ASSERT_EQ_UINT(provenance->program_family,
                   XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL);
    const XrSemanticFunctionRecord *caller = NULL;
    for (uint32_t i = 0; i < semantic->program_function_binding_count; i++) {
        const XrSemanticProgramFunctionBinding *binding = &semantic->program_function_bindings[i];
        if ((binding->flags & XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) != 0)
            caller = xr_semantic_plan_function(semantic, binding->semantic_function);
    }
    ASSERT_NOT_NULL(caller);
    ASSERT_EQ_UINT(caller->block_count, 1);
    const XrSemanticBlockRecord *caller_block =
        xr_semantic_plan_block(semantic, caller->block_begin);
    ASSERT_NOT_NULL(caller_block);
    ASSERT_NE(caller_block->operation_count, 9);

    XrTargetProfile *profile = NULL;
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&profile, error, sizeof(error)));
    ASSERT_NOT_NULL(profile);
    expect_leaf_target_build_rejection(semantic, profile);

    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    xi_func_free(root);
    fixture_cleanup(&fixture);
    xr_compiler_session_delete(session);
}

TEST(leaf_aggregate_rows_survive_xi_semantic_and_xsm_gates) {
    XrCompilerSessionConfig session_config = {0};
    XrCompilerSession *session = xr_compiler_session_new(&session_config);
    ASSERT_NOT_NULL(session);
    XrCompilerSession *foreign_authority_session = xr_compiler_session_new(&session_config);
    ASSERT_NOT_NULL(foreign_authority_session);
    XrCompilerSession *foreign_source_session = xr_compiler_session_new(&session_config);
    ASSERT_NOT_NULL(foreign_source_session);
    ScalarFixture fixture;
    ASSERT_TRUE(
        fixture_analyze(&fixture, session, "xi-leaf-aggregate-direct", kLeafAggregateSource));
    ASSERT_TRUE(fixture_publish(&fixture));
    const XrProgramSemanticClosure *published =
        xa_typed_program_program_semantic_closure(fixture.typed);
    ASSERT_NOT_NULL(published);
    XrProgramSemanticClosure *closure =
        xr_program_semantic_closure_retain((XrProgramSemanticClosure *) published);
    ASSERT_NOT_NULL(closure);
    XiProgramSemanticInput input = {
        .closure = closure,
        .decision = NULL,
    };
    XiFunc *root = xi_lower_program(fixture.typed, NULL, false, &input);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(root->module);
    ASSERT_TRUE(xi_module_set_identity(root->module, fixture.spec->canonical));
    char error[512] = {0};

    ScalarFixture foreign_authority_fixture;
    ASSERT_TRUE(fixture_analyze(&foreign_authority_fixture, foreign_authority_session,
                                "xi-leaf-aggregate-foreign-authority", kLeafAggregateSource));
    ASSERT_TRUE(fixture_publish(&foreign_authority_fixture));
    ScalarFixture foreign_source_fixture;
    ASSERT_TRUE(fixture_analyze(&foreign_source_fixture, foreign_source_session,
                                "xi-leaf-aggregate-direct",
                                kLeafAggregateSourceWithTrailingComment));
    ASSERT_TRUE(fixture_publish(&foreign_source_fixture));

    const XrProgramSemanticClosure *foreign_authority_published =
        xa_typed_program_program_semantic_closure(foreign_authority_fixture.typed);
    const XrProgramSemanticClosure *foreign_source_published =
        xa_typed_program_program_semantic_closure(foreign_source_fixture.typed);
    ASSERT_NOT_NULL(foreign_authority_published);
    ASSERT_NOT_NULL(foreign_source_published);
    ASSERT_EQ_UINT(xr_program_semantic_closure_family(published),
                   XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL);
    uint32_t aggregate_program =
        find_program_type_row(published, XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE);
    uint32_t scalar_program =
        find_program_type_row(published, XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR);
    uint32_t foreign_authority_aggregate = find_program_type_row(
        foreign_authority_published, XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE);
    uint32_t foreign_source_aggregate = find_program_type_row(
        foreign_source_published, XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE);
    ASSERT_NE(aggregate_program, XI_PSC_ROW_NONE);
    ASSERT_NE(scalar_program, XI_PSC_ROW_NONE);
    ASSERT_NE(foreign_authority_aggregate, XI_PSC_ROW_NONE);
    ASSERT_NE(foreign_source_aggregate, XI_PSC_ROW_NONE);
    const XrProgramSemanticModuleRecord *module_row =
        xr_program_semantic_closure_module(published, 0);
    const XrProgramSemanticModuleRecord *foreign_authority_module =
        xr_program_semantic_closure_module(foreign_authority_published, 0);
    const XrProgramSemanticModuleRecord *foreign_source_module =
        xr_program_semantic_closure_module(foreign_source_published, 0);
    ASSERT_NOT_NULL(module_row);
    ASSERT_NOT_NULL(foreign_authority_module);
    ASSERT_NOT_NULL(foreign_source_module);
    ASSERT_FALSE(same_id(module_row->module_identity, foreign_authority_module->module_identity));
    ASSERT_FALSE(same_fingerprint(module_row->module_authority_fingerprint,
                                  foreign_authority_module->module_authority_fingerprint));
    ASSERT_TRUE(same_fingerprint(module_row->source_fingerprint,
                                 foreign_authority_module->source_fingerprint));
    ASSERT_TRUE(same_id(module_row->module_identity, foreign_source_module->module_identity));
    ASSERT_TRUE(same_fingerprint(module_row->module_authority_fingerprint,
                                 foreign_source_module->module_authority_fingerprint));
    ASSERT_FALSE(same_fingerprint(module_row->source_fingerprint,
                                  foreign_source_module->source_fingerprint));
    const XrProgramSemanticTypeRecord *aggregate_row =
        xr_program_semantic_closure_type(published, aggregate_program);
    const XrProgramSemanticTypeRecord *foreign_authority_aggregate_row =
        xr_program_semantic_closure_type(foreign_authority_published, foreign_authority_aggregate);
    const XrProgramSemanticTypeRecord *foreign_source_aggregate_row =
        xr_program_semantic_closure_type(foreign_source_published, foreign_source_aggregate);
    ASSERT_NOT_NULL(aggregate_row);
    ASSERT_NOT_NULL(foreign_authority_aggregate_row);
    ASSERT_NOT_NULL(foreign_source_aggregate_row);
    ASSERT_TRUE(same_locator(aggregate_row->declaration_locator,
                             foreign_authority_aggregate_row->declaration_locator));
    ASSERT_TRUE(same_locator(aggregate_row->declaration_locator,
                             foreign_source_aggregate_row->declaration_locator));
    ASSERT_TRUE(same_fingerprint(aggregate_row->shape_fingerprint,
                                 foreign_authority_aggregate_row->shape_fingerprint));
    ASSERT_TRUE(same_fingerprint(aggregate_row->shape_fingerprint,
                                 foreign_source_aggregate_row->shape_fingerprint));

    XrProgramSemanticClosure *foreign_authority = xr_program_semantic_closure_retain(
        (XrProgramSemanticClosure *) foreign_authority_published);
    ASSERT_NOT_NULL(foreign_authority);
    XrProgramSemanticClosure *foreign_authority_owner = foreign_authority;
    ASSERT_FALSE(xi_module_take_program_semantics(root->module, &foreign_authority, NULL, NULL, 0,
                                                  error, sizeof(error)));
    ASSERT_EQ_PTR(foreign_authority, foreign_authority_owner);
    ASSERT_NULL(root->module->program_semantic_closure);
    xr_program_semantic_closure_free(foreign_authority);

    XrProgramSemanticClosure *foreign_source =
        xr_program_semantic_closure_retain((XrProgramSemanticClosure *) foreign_source_published);
    ASSERT_NOT_NULL(foreign_source);
    XrProgramSemanticClosure *foreign_source_owner = foreign_source;
    ASSERT_FALSE(xi_module_take_program_semantics(root->module, &foreign_source, NULL, NULL, 0,
                                                  error, sizeof(error)));
    ASSERT_EQ_PTR(foreign_source, foreign_source_owner);
    ASSERT_NULL(root->module->program_semantic_closure);
    xr_program_semantic_closure_free(foreign_source);

    ASSERT_TRUE(
        xi_module_take_program_semantics(root->module, &closure, NULL, NULL, 0, error,
                                         sizeof(error)));
    ASSERT_NULL(closure);
    bool xi_verified = xi_program_semantic_verify(root->module, NULL, error, sizeof(error));
    ASSERT_TRUE(xi_verified);

    XiFunc *callee = NULL;
    for (uint16_t i = 0; i < root->module->nfuncs; i++) {
        XiFunc *candidate = root->module->functions[i];
        if (candidate && candidate->nparams == 1 &&
            candidate->psc_function_index != XI_PSC_ROW_NONE)
            callee = candidate;
    }
    ASSERT_NOT_NULL(callee);
    ASSERT_NOT_NULL(callee->params);
    ASSERT_NOT_NULL(callee->params[0]);
    ASSERT_EQ_UINT(callee->psc_return_type_index, aggregate_program);
    ASSERT_EQ_UINT(callee->params[0]->psc_type_index, aggregate_program);
    XiValue *aggregate_value = find_bound_type_value(root->module, aggregate_program, UINT16_MAX);
    ASSERT_NOT_NULL(aggregate_value);
    ASSERT_NOT_NULL(aggregate_value->type);
    XiValue *class_create = find_bound_type_value(root->module, aggregate_program, XI_AGG_NEW);
    ASSERT_NOT_NULL(class_create);
    XiValue *bound_call = NULL;
    for (uint16_t i = 0; i < root->module->nfuncs; i++) {
        XiValue *candidate = find_bound_call(root->module->functions[i]);
        if (!candidate)
            continue;
        ASSERT_NULL(bound_call);
        bound_call = candidate;
    }
    ASSERT_NOT_NULL(bound_call);
    ASSERT_NOT_NULL(bound_call->args);
    ASSERT_EQ_UINT(bound_call->nargs, 2);
    ASSERT_NOT_NULL(bound_call->args[1]);
    ASSERT_EQ_UINT(bound_call->psc_type_index, aggregate_program);
    ASSERT_EQ_UINT(bound_call->args[1]->psc_type_index, aggregate_program);

    /* Each semantic role must rejoin the exact local declaration. A foreign
     * class with
     * identical fields is not the Pair declaration, regardless of
     * which expression-local
     * XrType happens to carry it. */
    assert_foreign_declaration_type_is_rejected(root->module, &callee->return_type, error,
                                                sizeof(error));
    assert_foreign_declaration_type_is_rejected(root->module, &callee->params[0]->type, error,
                                                sizeof(error));
    assert_foreign_declaration_type_is_rejected(root->module, &class_create->type, error,
                                                sizeof(error));
    assert_foreign_declaration_type_is_rejected(root->module, &bound_call->type, error,
                                                sizeof(error));
    assert_foreign_declaration_type_is_rejected(root->module, &bound_call->args[1]->type, error,
                                                sizeof(error));

    ASSERT_TRUE(xi_own_value_is_psc_leaf_aggregate(callee->params[0]));
    ASSERT_FALSE(xi_own_value_is_rc(callee->params[0]));
    ASSERT_FALSE(xi_own_function_return_is_rc(callee));

    uint32_t saved_return = callee->psc_return_type_index;
    callee->psc_return_type_index = XI_PSC_ROW_NONE;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    ASSERT_FALSE(xi_own_function_return_is_rc(callee));
    callee->psc_return_type_index = saved_return;
    uint32_t saved_parameter = callee->params[0]->psc_type_index;
    callee->params[0]->psc_type_index = XI_PSC_ROW_NONE;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    ASSERT_TRUE(xi_own_value_is_psc_leaf_aggregate(callee->params[0]));
    ASSERT_FALSE(xi_own_value_is_rc(callee->params[0]));
    callee->params[0]->psc_type_index = saved_parameter;
    uint32_t saved_value = aggregate_value->psc_type_index;
    aggregate_value->psc_type_index = XI_PSC_ROW_NONE;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    aggregate_value->psc_type_index = scalar_program;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    aggregate_value->psc_type_index = saved_value;
    ASSERT_TRUE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));

    ASSERT_EQ_UINT(root->module->nclasses, 1);
    ASSERT_NOT_NULL(root->module->classes);
    XiClassData *aggregate_class = root->module->classes[0];
    ASSERT_NOT_NULL(aggregate_class);
    uint32_t saved_class_type = aggregate_class->psc_type_index;
    aggregate_class->psc_type_index = XI_PSC_ROW_NONE;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    ASSERT_FALSE(xi_own_value_is_psc_leaf_aggregate(callee->params[0]));
    ASSERT_TRUE(xi_own_value_is_rc(callee->params[0]));
    ASSERT_TRUE(xi_own_function_return_is_rc(callee));
    aggregate_class->psc_type_index = scalar_program;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    ASSERT_FALSE(xi_own_value_is_psc_leaf_aggregate(callee->params[0]));
    aggregate_class->psc_type_index = saved_class_type;
    ASSERT_TRUE(xi_own_value_is_psc_leaf_aggregate(callee->params[0]));

    XrClassInfo *saved_class_info = aggregate_class->class_info;
    ASSERT_NOT_NULL(saved_class_info);
    aggregate_class->class_info = NULL;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    aggregate_class->class_info = saved_class_info;

    XrType *saved_aggregate_value_type = aggregate_value->type;
    XrClassInfo foreign_same_shape_class = *saved_class_info;
    XrType foreign_same_shape_type = *saved_aggregate_value_type;
    foreign_same_shape_type.instance.class_ref = &foreign_same_shape_class;
    aggregate_value->type = &foreign_same_shape_type;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    aggregate_value->type = saved_aggregate_value_type;

    XrType hostile_modified_type = *saved_aggregate_value_type;
    hostile_modified_type.is_nullable = true;
    aggregate_value->type = &hostile_modified_type;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    hostile_modified_type = *saved_aggregate_value_type;
    hostile_modified_type.is_const = true;
    aggregate_value->type = &hostile_modified_type;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    XrType *hostile_type_arguments[] = {saved_aggregate_value_type};
    hostile_modified_type = *saved_aggregate_value_type;
    hostile_modified_type.instance.type_args = hostile_type_arguments;
    hostile_modified_type.instance.type_arg_count = 1;
    aggregate_value->type = &hostile_modified_type;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    aggregate_value->type = saved_aggregate_value_type;

    uint16_t saved_class_count = root->module->nclasses;
    root->module->nclasses = 0;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    root->module->nclasses = saved_class_count;

    ASSERT_EQ_UINT(root->nchildren, 2);
    uint16_t saved_child_count = root->nchildren;
    root->nchildren = 1;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    root->nchildren = saved_child_count;

    XiValue *non_call = find_non_call_value(root->module);
    ASSERT_NOT_NULL(non_call);
    ASSERT_NE(non_call->op, XI_CALL);
    ASSERT_EQ_UINT(non_call->psc_call_index, XI_PSC_ROW_NONE);
    non_call->psc_call_index = 0;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    non_call->psc_call_index = XI_PSC_ROW_NONE;

    ASSERT_TRUE(callee->nblocks > 0);
    XiBlock *phi_block = callee->blocks[0];
    ASSERT_NOT_NULL(phi_block);
    XiPhi *previous_phis = phi_block->phis;
    XiPhi *hostile_phi = xi_phi_new(callee, phi_block, callee->return_type, 0);
    ASSERT_NOT_NULL(hostile_phi);
    hostile_phi->value.psc_type_index = aggregate_program;
    ASSERT_TRUE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    hostile_phi->value.psc_call_index = 0;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    hostile_phi->value.psc_call_index = XI_PSC_ROW_NONE;
    phi_block->phis = previous_phis;
    ASSERT_TRUE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));

    /* The analyzer may materialize expression-local XrType objects for one
     * declaration. Their local cache identity and VALUE-site marker are not
     * declaration authority: the frozen class PSC row is. Keep one such clone
     * live through construction to prove all Pair sites intern to one type. */
    XrType expression_local_pair_type = *saved_aggregate_value_type;
    expression_local_pair_type.semantic_type_id = UINT32_C(0x28100001);
    expression_local_pair_type.is_value_type = !saved_aggregate_value_type->is_value_type;
    expression_local_pair_type.alias_name = "expression-local-pair";
    aggregate_value->type = &expression_local_pair_type;
    ASSERT_TRUE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));

    prepare_leaf_tree_for_semantic_plan(root);
    ASSERT_TRUE(xi_module_set_identity(root->module, fixture.spec->canonical));
    XrType *saved_callee_return_type = callee->return_type;
    XrType hostile_scalar_carrier = *saved_callee_return_type;
    hostile_scalar_carrier.scalar_rep = XR_NATIVE_I64;
    callee->return_type = &hostile_scalar_carrier;
    XrSemanticPlan *rejected_semantic = NULL;
    memset(error, 0, sizeof(error));
    ASSERT_FALSE(xr_semantic_plan_build(root, &rejected_semantic, error, sizeof(error)));
    ASSERT_NULL(rejected_semantic);
    ASSERT_NOT_NULL(strstr(error, "PSC annotation is not exact"));
    callee->return_type = saved_callee_return_type;

    XrType hostile_builtin_carrier = *saved_callee_return_type;
    hostile_builtin_carrier.instance.class_ref = NULL;
    hostile_builtin_carrier.instance.class_name = "StringBuilder";
    callee->return_type = &hostile_builtin_carrier;
    memset(error, 0, sizeof(error));
    ASSERT_FALSE(xr_semantic_plan_build(root, &rejected_semantic, error, sizeof(error)));
    ASSERT_NULL(rejected_semantic);
    ASSERT_TRUE(error[0] != '\0');
    callee->return_type = saved_callee_return_type;

    XrSemanticPlan *semantic = NULL;
    bool semantic_built = xr_semantic_plan_build(root, &semantic, error, sizeof(error));
    ASSERT_MSG(semantic_built, error);
    ASSERT_NOT_NULL(semantic);
    ASSERT_TRUE(xi_program_semantic_plan_verify(root, semantic, NULL, error, sizeof(error)));
    const XrSemanticProgramProvenance *provenance = xr_semantic_plan_program_provenance(semantic);
    ASSERT_NOT_NULL(provenance);
    ASSERT_EQ_UINT(provenance->program_family,
                   XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL);
    ASSERT_EQ_UINT(xr_semantic_plan_program_type_binding_count(semantic), 2);
    ASSERT_EQ_UINT(xr_semantic_plan_program_type_field_binding_count(semantic), 2);
    const XrSemanticProgramTypeBinding *aggregate_binding =
        xr_semantic_plan_program_type_for_row(semantic, aggregate_program);
    ASSERT_NOT_NULL(aggregate_binding);
    ASSERT_EQ_PTR(
        xr_semantic_plan_program_type_for_semantic_type(semantic, aggregate_binding->semantic_type),
        aggregate_binding);
    ASSERT_TRUE(aggregate_binding->semantic_type < semantic->type_count);
    const XrSemanticTypeRecord *aggregate_semantic_type =
        &semantic->types[aggregate_binding->semantic_type];
    ASSERT_EQ_UINT(aggregate_semantic_type->kind, XR_KIND_INSTANCE);
    ASSERT_EQ_UINT(aggregate_semantic_type->builtin_type, XR_TID_NULL);
    ASSERT_EQ_UINT(aggregate_semantic_type->scalar_rep, XR_SCALAR_REP_NONE);
    ASSERT_TRUE((aggregate_semantic_type->flags & XR_SEM_TYPE_VALUE) != 0);
    ASSERT_TRUE(same_id(aggregate_semantic_type->source_class_identity,
                        aggregate_binding->source_class_identity));
    ASSERT_TRUE(strncmp(aggregate_semantic_type->canonical_key, "type-v3:", 8) == 0);
    ASSERT_NOT_NULL(strstr(aggregate_semantic_type->canonical_key, ";program-type:"));
    uint32_t declaration_type_count = 0;
    for (uint32_t i = 0; i < semantic->type_count; i++) {
        const XrSemanticTypeRecord *type = &semantic->types[i];
        if (type->kind != XR_KIND_INSTANCE ||
            !same_id(type->source_class_identity, aggregate_binding->source_class_identity))
            continue;
        declaration_type_count++;
        ASSERT_EQ_UINT(i, aggregate_binding->semantic_type);
    }
    ASSERT_EQ_UINT(declaration_type_count, 1);

    uint32_t aggregate_function_count = 0;
    uint32_t aggregate_parameter_count = 0;
    for (uint32_t i = 0; i < semantic->program_function_binding_count; i++) {
        const XrSemanticProgramFunctionBinding *binding = &semantic->program_function_bindings[i];
        ASSERT_TRUE(binding->semantic_function < semantic->function_count);
        const XrSemanticFunctionRecord *function = &semantic->functions[binding->semantic_function];
        ASSERT_EQ_UINT(function->return_type, aggregate_binding->semantic_type);
        aggregate_function_count++;
        for (uint16_t p = 0; p < function->parameter_count; p++) {
            ASSERT_TRUE(function->parameter_begin + p < semantic->parameter_count);
            const XrSemanticParameterRecord *parameter =
                &semantic->parameters[function->parameter_begin + p];
            ASSERT_EQ_UINT(parameter->type, aggregate_binding->semantic_type);
            ASSERT_EQ_UINT(parameter->ownership, XI_OWN_NONE);
            ASSERT_EQ_UINT(parameter->transfer_mode, XR_TRANSFER_SHARE);
            aggregate_parameter_count++;
        }
    }
    ASSERT_EQ_UINT(aggregate_function_count, 2);
    ASSERT_EQ_UINT(aggregate_parameter_count, 1);
    uint32_t aggregate_parameter_operation_count = 0;
    for (uint32_t i = 0; i < semantic->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &semantic->operations[i];
        if (operation->opcode != XI_PARAM ||
            operation->result_type != aggregate_binding->semantic_type)
            continue;
        ASSERT_EQ_UINT(operation->parameter_ownership, XI_OWN_NONE);
        aggregate_parameter_operation_count++;
    }
    ASSERT_EQ_UINT(aggregate_parameter_operation_count, 1);
    ASSERT_NOT_NULL(xr_semantic_plan_program_function_for_semantic_function(
        semantic, semantic->program_function_bindings[0].semantic_function));
    const XrSemanticProgramCallBinding *aggregate_call_binding =
        xr_semantic_plan_program_call_for_operation(semantic,
                                                    semantic->program_call_bindings[0].operation);
    ASSERT_NOT_NULL(aggregate_call_binding);
    ASSERT_TRUE(aggregate_call_binding->operation < semantic->operation_count);
    const XrSemanticOperationRecord *aggregate_call =
        &semantic->operations[aggregate_call_binding->operation];
    ASSERT_EQ_UINT(aggregate_call->result_type, aggregate_binding->semantic_type);
    ASSERT_EQ_UINT(aggregate_call->result_ownership, XI_GEN_RESULT_OWNERSHIP_CALL_RESULT);
    ASSERT_EQ_UINT(aggregate_call->parameter_ownership, XI_OWN_NONE);
    ASSERT_EQ_UINT(aggregate_call->operand_count, 2);
    ASSERT_TRUE(aggregate_call->operand_begin + 1u < semantic->operand_count);
    ASSERT_EQ_UINT(semantic->operands[aggregate_call->operand_begin + 1u].type,
                   aggregate_binding->semantic_type);

    const char *saved_class_name = aggregate_class->class_name;
    ASSERT_NOT_NULL(saved_class_name);
    aggregate_class->class_name = "HostilePair";
    ASSERT_TRUE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    ASSERT_FALSE(xi_program_semantic_plan_verify(root, semantic, NULL, error, sizeof(error)));
    aggregate_class->class_name = saved_class_name;
    ASSERT_TRUE(xi_program_semantic_plan_verify(root, semantic, NULL, error, sizeof(error)));

    ASSERT_TRUE(xi_module_set_identity(root->module, "memory-module-v1:id=7:hostile"));
    ASSERT_TRUE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    ASSERT_FALSE(xi_program_semantic_plan_verify(root, semantic, NULL, error, sizeof(error)));
    ASSERT_TRUE(xi_module_set_identity(root->module, fixture.spec->canonical));
    ASSERT_TRUE(xi_program_semantic_plan_verify(root, semantic, NULL, error, sizeof(error)));

    XrFingerprint original = xr_semantic_plan_fingerprint(semantic);
    XrSemanticProgramTypeBinding *mutable_aggregate_binding = NULL;
    for (uint32_t i = 0; i < semantic->program_type_binding_count; i++) {
        XrSemanticProgramTypeBinding *candidate = &semantic->program_type_bindings[i];
        if (candidate->program_row == aggregate_program)
            mutable_aggregate_binding = candidate;
    }
    ASSERT_NOT_NULL(mutable_aggregate_binding);
    XrStableId saved_source_class_identity = mutable_aggregate_binding->source_class_identity;
    mutable_aggregate_binding->source_class_identity.bytes[0] ^= UINT8_C(1);
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    ASSERT_FALSE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    mutable_aggregate_binding->source_class_identity = saved_source_class_identity;
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    ASSERT_TRUE(xr_fingerprint_equal(semantic->fingerprint, original));
    ASSERT_TRUE(xi_program_semantic_plan_verify(root, semantic, NULL, error, sizeof(error)));

    uint8_t saved_flags = semantic->program_function_bindings[0].flags;
    semantic->program_function_bindings[0].flags ^= XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY;
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    ASSERT_FALSE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    semantic->program_function_bindings[0].flags = saved_flags;
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    ASSERT_TRUE(xr_fingerprint_equal(semantic->fingerprint, original));
    ASSERT_TRUE(xi_program_semantic_plan_verify(root, semantic, NULL, error, sizeof(error)));

    XrTargetProfile *leaf_profile = NULL;
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&leaf_profile, error, sizeof(error)));
    ASSERT_NOT_NULL(leaf_profile);
    XrTargetPlan *leaf_target = NULL;
    ASSERT_TRUE(xr_target_plan_build(semantic, leaf_profile, &leaf_target, error, sizeof(error)));
    ASSERT_NOT_NULL(leaf_target);
    LeafAggregateTargetEvidence target_evidence = {0};
    assert_leaf_aggregate_target_shape(semantic, leaf_target, &target_evidence);
    assert_leaf_aggregate_vm_execution(leaf_target, &target_evidence);
    assert_leaf_aggregate_target_mutations(semantic, leaf_target, target_evidence);
    assert_leaf_aggregate_target_input_rejections(
        semantic, leaf_profile, foreign_authority_aggregate_row, foreign_source_aggregate_row);

    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    ASSERT_TRUE(xr_xsm_encode(semantic, &encoded, &encoded_size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    ASSERT_TRUE(xr_xsm_decode(encoded, encoded_size, &decoded, error, sizeof(error)));
    ASSERT_NOT_NULL(decoded);
    ASSERT_TRUE(xr_semantic_plan_verify(decoded, error, sizeof(error)));
    ASSERT_TRUE(xi_program_semantic_plan_verify(root, decoded, NULL, error, sizeof(error)));
    ASSERT_EQ_UINT(xr_semantic_plan_program_type_binding_count(decoded), 2);
    ASSERT_EQ_UINT(xr_semantic_plan_program_type_field_binding_count(decoded), 2);
    const XrSemanticProgramTypeBinding *decoded_aggregate =
        xr_semantic_plan_program_type_for_row(decoded, aggregate_program);
    ASSERT_NOT_NULL(decoded_aggregate);
    ASSERT_TRUE(decoded_aggregate->semantic_type < decoded->type_count);
    ASSERT_TRUE(same_id(decoded->types[decoded_aggregate->semantic_type].source_class_identity,
                        decoded_aggregate->source_class_identity));
    uint8_t *roundtrip = NULL;
    size_t roundtrip_size = 0;
    ASSERT_TRUE(xr_xsm_encode(decoded, &roundtrip, &roundtrip_size, error, sizeof(error)));
    ASSERT_EQ_UINT(roundtrip_size, encoded_size);
    ASSERT_TRUE(memcmp(roundtrip, encoded, encoded_size) == 0);
    xr_free(roundtrip);
    xr_semantic_plan_free(decoded);
    xr_free(encoded);

    ASSERT_MSG(xi_semantic_snapshot_detach_ex(root, error, sizeof(error)), error);
    assert_leaf_aggregate_aot_boundary(root->module, leaf_target, &target_evidence);
    xr_target_plan_free(leaf_target);
    xr_target_profile_free(leaf_profile);

    xr_semantic_plan_free(semantic);
    xi_func_free(root);
    fixture_cleanup(&foreign_source_fixture);
    fixture_cleanup(&foreign_authority_fixture);
    fixture_cleanup(&fixture);
    xr_compiler_session_delete(foreign_source_session);
    xr_compiler_session_delete(foreign_authority_session);
    xr_compiler_session_delete(session);
}

TEST(leaf_product_uses_canonical_construct_project_joins) {
    XrCompilerSessionConfig session_config = {0};
    XrCompilerSession *session = xr_compiler_session_new(&session_config);
    ASSERT_NOT_NULL(session);
    ScalarFixture fixture;
    ASSERT_TRUE(
        fixture_analyze(&fixture, session, "xi-leaf-product-direct", kLeafProductXiSource));
    ASSERT_TRUE(fixture_publish(&fixture));
    ASSERT_NULL(xa_typed_program_program_semantic_closure(fixture.typed));

    char error[512] = {0};
    XrProgramSemanticClosure *closure = NULL;
    ASSERT_EQ_INT(xa_program_semantic_closure_publish_leaf_product(
                      fixture.analyzer, fixture.spec->ast, fixture.spec, &closure, error,
                      sizeof(error)),
                  XA_PROGRAM_SEMANTIC_CLOSURE_READY);
    ASSERT_NOT_NULL(closure);
    XiProgramSemanticInput input = {.closure = closure, .decision = NULL, .module_index = 0};
    ASSERT_TRUE(xi_program_semantic_input_is_consistent(&input, error, sizeof(error)));
    XiFunc *root = xi_lower_program(fixture.typed, NULL, false, &input);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(root->module);
    ASSERT_TRUE(xi_module_set_identity(root->module, fixture.spec->canonical));
    ASSERT_TRUE(xi_module_take_program_semantics(root->module, &closure, NULL, NULL, 0, error,
                                                 sizeof(error)));
    ASSERT_NULL(closure);
    ASSERT_TRUE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));

    const XrProgramSemanticClosure *retained = root->module->program_semantic_closure;
    uint32_t product_index =
        find_program_type_row(retained, XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT);
    uint32_t i64_index = XI_PSC_ROW_NONE;
    uint32_t u8_index = XI_PSC_ROW_NONE;
    for (uint32_t i = 0; i < xr_program_semantic_closure_type_count(retained); i++) {
        const XrProgramSemanticTypeRecord *row = xr_program_semantic_closure_type(retained, i);
        if (!row || row->kind != XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR)
            continue;
        if (row->exact_scalar == XR_EXACT_SCALAR_I64)
            i64_index = i;
        if (row->exact_scalar == XR_EXACT_SCALAR_U8)
            u8_index = i;
    }
    ASSERT_NE(product_index, XI_PSC_ROW_NONE);
    ASSERT_NE(i64_index, XI_PSC_ROW_NONE);
    ASSERT_NE(u8_index, XI_PSC_ROW_NONE);

    XiValue *calls[2] = {NULL, NULL};
    XiValue *entry_constructs[2] = {NULL, NULL};
    XiValue *entry_projects[2][6] = {{0}};
    XiValue *callee_construct = NULL;
    uint32_t entry_index = 0;
    uint32_t managed_tuple_count = 0;
    for (uint16_t f = 0; f < root->module->nfuncs; f++) {
        XiFunc *function = root->module->functions[f];
        const XrProgramSemanticFunctionRecord *function_row =
            xr_program_semantic_closure_function(retained, function->psc_function_index);
        ASSERT_NOT_NULL(function_row);
        bool entry = function_row->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY;
        uint32_t current_entry = entry_index;
        if (entry)
            ASSERT_TRUE(entry_index++ < 2);
        for (uint32_t b = 0; b < function->nblocks; b++) {
            XiBlock *block = function->blocks[b];
            for (uint32_t v = 0; v < block->nvalues; v++) {
                XiValue *value = block->values[v];
                managed_tuple_count += value->op == XI_TUPLE_NEW || value->op == XI_TUPLE_GET;
                if (value->op == XI_CALL) {
                    ASSERT_TRUE(entry);
                    ASSERT_TRUE(value->psc_call_index < 2);
                    calls[value->psc_call_index] = value;
                } else if (value->op == XI_VALUE_PRODUCT_CONSTRUCT) {
                    if (entry)
                        entry_constructs[current_entry] = value;
                    else
                        callee_construct = value;
                } else if (value->op == XI_VALUE_PRODUCT_PROJECT) {
                    ASSERT_TRUE(entry);
                    ASSERT_TRUE(value->aux_int >= 0 && value->aux_int < 6);
                    entry_projects[current_entry][value->aux_int] = value;
                }
            }
        }
    }
    ASSERT_EQ_UINT(entry_index, 2);
    ASSERT_EQ_UINT(managed_tuple_count, 0);
    ASSERT_NOT_NULL(calls[0]);
    ASSERT_NOT_NULL(calls[1]);
    ASSERT_NOT_NULL(callee_construct);
    ASSERT_FALSE(xi_own_value_is_rc(calls[0]));
    ASSERT_FALSE(xi_own_value_is_rc(calls[1]));
    ASSERT_FALSE(xi_own_value_is_rc(callee_construct));
    for (uint32_t caller = 0; caller < 2; caller++) {
        ASSERT_NOT_NULL(entry_constructs[caller]);
        ASSERT_FALSE(xi_own_value_is_rc(entry_constructs[caller]));
        for (uint32_t ordinal = 0; ordinal < 6; ordinal++) {
            ASSERT_NOT_NULL(entry_projects[caller][ordinal]);
            ASSERT_EQ_UINT(entry_projects[caller][ordinal]->psc_type_index,
                           ordinal == 2 ? u8_index : i64_index);
            ASSERT_EQ_PTR(entry_constructs[caller]->args[ordinal],
                          entry_projects[caller][ordinal]);
        }
    }

    uint16_t saved_op = entry_constructs[0]->op;
    entry_constructs[0]->op = XI_TUPLE_NEW;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    entry_constructs[0]->op = saved_op;

    saved_op = entry_projects[0][5]->op;
    entry_projects[0][5]->op = XI_COPY;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    entry_projects[0][5]->op = saved_op;

    int64_t saved_ordinal = entry_projects[0][1]->aux_int;
    entry_projects[0][1]->aux_int = 0;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    entry_projects[0][1]->aux_int = saved_ordinal;

    XiValue *saved_argument = entry_constructs[0]->args[0];
    entry_constructs[0]->args[0] = entry_constructs[0]->args[1];
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    entry_constructs[0]->args[0] = saved_argument;

    saved_ordinal = entry_projects[0][0]->aux_int;
    entry_projects[0][0]->aux_int = 6;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    entry_projects[0][0]->aux_int = saved_ordinal;

    uint32_t saved_member = entry_projects[0][2]->psc_type_index;
    entry_projects[0][2]->psc_type_index = i64_index;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    entry_projects[0][2]->psc_type_index = saved_member;

    uint32_t saved_call = calls[1]->psc_call_index;
    calls[1]->psc_call_index = calls[0]->psc_call_index;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    calls[1]->psc_call_index = saved_call;

    XiFunc *mutated_caller = calls[1]->block->func;
    uint32_t saved_caller_row = mutated_caller->psc_function_index;
    mutated_caller->psc_function_index = calls[0]->block->func->psc_function_index;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    mutated_caller->psc_function_index = saved_caller_row;

    uint8_t saved_flags = entry_constructs[0]->flags;
    entry_constructs[0]->flags ^= 1u;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    entry_constructs[0]->flags = saved_flags;

    uint8_t saved_aux_kind = entry_projects[0][0]->aux_kind;
    entry_projects[0][0]->aux_kind = XI_AUX_KIND_ASSERTION_PLAN;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    entry_projects[0][0]->aux_kind = saved_aux_kind;

    ASSERT_TRUE(root->nblocks > 0);
    ASSERT_NOT_NULL(root->blocks[0]);
    ASSERT_TRUE(root->blocks[0]->nvalues > 0);
    XiValue *init_value = root->blocks[0]->values[0];
    ASSERT_NOT_NULL(init_value);
    saved_op = init_value->op;
    init_value->op = XI_VALUE_PRODUCT_CONSTRUCT;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    init_value->op = saved_op;
    saved_call = init_value->psc_call_index;
    init_value->psc_call_index = 0;
    ASSERT_FALSE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));
    init_value->psc_call_index = saved_call;
    ASSERT_TRUE(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)));

    prepare_leaf_tree_for_semantic_plan(root);
    ASSERT_MSG(xi_program_semantic_verify(root->module, NULL, error, sizeof(error)), error);
    XrSemanticPlan *semantic = NULL;
    ASSERT_MSG(xr_semantic_plan_build(root, &semantic, error, sizeof(error)), error);
    ASSERT_NOT_NULL(semantic);
    ASSERT_TRUE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    ASSERT_TRUE(xi_program_semantic_plan_verify(root, semantic, NULL, error, sizeof(error)));

    XrTargetProfile *profile = NULL;
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&profile, error, sizeof(error)));
    ASSERT_NOT_NULL(profile);
    XrTargetPlan *target = NULL;
    ASSERT_MSG(xr_target_plan_build(semantic, profile, &target, error, sizeof(error)), error);
    ASSERT_NOT_NULL(target);
    ASSERT_TRUE(xr_target_plan_verify(target, error, sizeof(error)));
    ASSERT_TRUE(xr_target_instruction_program_verify(target, error, sizeof(error)));
    XrRuntimeTargetAuthority freestanding_authority;
    const uint64_t freestanding_providers =
        XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_ALLOCATOR) |
        XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_PANIC) |
        XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_IO);
    ASSERT_EQ_UINT(xr_runtime_target_authority_native_freestanding(
                       freestanding_providers, &freestanding_authority),
                   XR_RUNTIME_ABI_OK);
    XrTargetProfileBuildInput freestanding_input = {
        .machine = freestanding_authority.machine,
        .runtime_abi = &freestanding_authority.runtime_abi,
        .object_header_materialization =
            &freestanding_authority.object_header_materialization,
        .string_contract = &freestanding_authority.string_contract,
        .providers = freestanding_authority.providers,
        .provider_count = freestanding_authority.provider_count,
    };
    XrTargetProfile *freestanding_profile = NULL;
    ASSERT_TRUE(xr_target_profile_build(&freestanding_input, &freestanding_profile,
                                        error, sizeof(error)));
    XrTargetPlan *freestanding_target = NULL;
    ASSERT_MSG(xr_target_plan_build(semantic, freestanding_profile,
                                    &freestanding_target, error, sizeof(error)),
               error);
    ASSERT_NOT_NULL(freestanding_target);
    ASSERT_TRUE(xr_target_plan_verify(freestanding_target, error, sizeof(error)));
    LeafProductTargetEvidence target_evidence = {0};
    assert_leaf_product_target_shape(semantic, target, &target_evidence);
    assert_leaf_product_target_mutations(semantic, profile, target, target_evidence);
    assert_leaf_product_vm_execution(target, &target_evidence);
    ASSERT_MSG(xi_semantic_snapshot_detach_ex(root, error, sizeof(error)), error);
    assert_leaf_product_aot_cgen(root->module, target, freestanding_target);

    uint8_t *target_encoded = NULL;
    size_t target_encoded_size = 0;
    ASSERT_TRUE(xr_xtp_encode_plan(target, &target_encoded, &target_encoded_size, error,
                                   sizeof(error)));
    ASSERT_TRUE(target_encoded_size >= XR_XTP_HEADER_SIZE);
    uint8_t saved_target_schema[4] = {
        target_encoded[4], target_encoded[5], target_encoded[6], target_encoded[7],
    };
    target_encoded[4] = 50;
    target_encoded[5] = 0;
    target_encoded[6] = 0;
    target_encoded[7] = 0;
    XrXtpCandidate *old_target_candidate = NULL;
    ASSERT_FALSE(xr_xtp_decode_candidate(target_encoded, target_encoded_size,
                                         &old_target_candidate, error, sizeof(error)));
    ASSERT_NULL(old_target_candidate);
    memcpy(target_encoded + 4, saved_target_schema, sizeof(saved_target_schema));
    XrXtpCandidate *target_candidate = NULL;
    ASSERT_TRUE(xr_xtp_decode_candidate(target_encoded, target_encoded_size, &target_candidate,
                                        error, sizeof(error)));
    XrTargetPlan *target_roundtrip = NULL;
    ASSERT_TRUE(xr_xtp_materialize_target_plan(target_candidate, semantic, profile,
                                               &target_roundtrip, error, sizeof(error)));
    ASSERT_NOT_NULL(target_roundtrip);
    ASSERT_TRUE(xr_target_plan_verify(target_roundtrip, error, sizeof(error)));
    ASSERT_TRUE(xr_target_instruction_program_verify(target_roundtrip, error, sizeof(error)));
    ASSERT_TRUE(xr_fingerprint_equal(xr_target_plan_fingerprint(target_roundtrip),
                                     xr_target_plan_fingerprint(target)));
    assert_leaf_product_vm_execution(target_roundtrip, &target_evidence);
    assert_leaf_product_aot_cgen(root->module, target_roundtrip, freestanding_target);
    xr_target_plan_free(target_roundtrip);
    xr_xtp_candidate_release(target_candidate);
    xr_xtp_encoded_free(target_encoded);

    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    ASSERT_TRUE(xr_xsm_encode(semantic, &encoded, &encoded_size, error, sizeof(error)));
    ASSERT_TRUE(encoded_size >= XR_XSM_HEADER_SIZE);
    uint8_t saved_schema[4] = {encoded[4], encoded[5], encoded[6], encoded[7]};
    encoded[4] = 42;
    encoded[5] = 0;
    encoded[6] = 0;
    encoded[7] = 0;
    XrSemanticPlan *old_schema = NULL;
    ASSERT_FALSE(xr_xsm_decode(encoded, encoded_size, &old_schema, error, sizeof(error)));
    ASSERT_NULL(old_schema);
    memcpy(encoded + 4, saved_schema, sizeof(saved_schema));
    XrSemanticPlan *decoded = NULL;
    ASSERT_TRUE(xr_xsm_decode(encoded, encoded_size, &decoded, error, sizeof(error)));
    ASSERT_NOT_NULL(decoded);
    ASSERT_TRUE(xr_semantic_plan_verify(decoded, error, sizeof(error)));
    ASSERT_TRUE(xi_program_semantic_plan_verify(root, decoded, NULL, error, sizeof(error)));
    xr_semantic_plan_free(decoded);
    xr_free(encoded);
    xr_target_plan_free(target);
    xr_target_plan_free(freestanding_target);
    xr_target_profile_free(profile);
    xr_target_profile_free(freestanding_profile);
    xr_semantic_plan_free(semantic);

    xi_func_free(root);
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
    ASSERT_TRUE(
        xr_program_semantic_closure_create(&limits, policy, &collecting, error, sizeof(error)));
    ASSERT_NULL(xr_program_semantic_closure_retain(collecting));
    xr_program_semantic_closure_free(collecting);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("PSC-backed Xi/SemanticPlan binding");
RUN_TEST(stable_rows_survive_mutation_and_ownership_gates);
RUN_TEST(leaf_aggregate_canonical_semantic_shape_mismatch_is_rejected);
RUN_TEST(leaf_aggregate_rows_survive_xi_semantic_and_xsm_gates);
RUN_TEST(leaf_product_uses_canonical_construct_project_joins);
RUN_TEST(retain_rejects_mutable_closure);
TEST_MAIN_END()
