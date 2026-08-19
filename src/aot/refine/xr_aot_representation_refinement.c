/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_aot_representation_refinement.c - Immutable representation adapter materialization
 */

#include "../../plan/semantic/xr_semantic_heap_literal_shape.h"
#include "xr_aot_representation_refinement.h"
#include "xr_aot_scalar_value.h"
#include "../../base/xsha256.h"
#include "../../base/xglobal_indices.h"
#include "../../base/xmalloc.h"
#include "../../frontend/analyzer/xa_intrinsic_registry.h"
#include "../../ir/xi_opt.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "../../ir/xi_op_name.h"
#include "../../ir/xi_module.h"
#include "../../ir/xi_value_query.h"
#include "../../plan/semantic/xr_semantic_range_slice_shape.h"
#include "../../plan/semantic/xr_semantic_graph.h"
#include "../../plan/semantic/xr_semantic_allocation_shape.h"
#include "../../plan/semantic/xr_semantic_class_shape.h"
#include "../../plan/semantic/xr_semantic_cleanup_shape.h"
#include "../../plan/semantic/xr_semantic_string_shape.h"
#include "../../plan/semantic/xr_semantic_task_shape.h"
#include "../../plan/semantic/xr_semantic_string_runes_shape.h"
#include "../../plan/semantic/xr_semantic_iterator_rune_has_next_shape.h"
#include "../../plan/semantic/xr_semantic_iterator_rune_next_shape.h"
#include "../../plan/semantic/xr_semantic_rune_to_uint32_shape.h"
#include "../../plan/semantic/xr_semantic_rune_is_whitespace_shape.h"
#include "../../plan/semantic/xr_semantic_string_slice_shape.h"
#include "../../plan/semantic/xr_semantic_native_module_shape.h"
#include "../../plan/semantic/xr_semantic_value_aggregate_shape.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/value/xtype_names.h"
#include "../../stdlib/xstdlib_metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static XrFingerprint rep_policy_fingerprint(const XiRepPolicy *policy) {
    static const uint8_t domain[] = "xray-aot-representation-policy-v1\0";
    uint8_t facts[3] = {
        policy && policy->force_phi_tagged ? 1u : 0u,
        policy && policy->force_return_tagged ? 1u : 0u,
        policy && policy->prefer_call_args_native ? 1u : 0u,
    };
    XrFingerprint fingerprint = {{0}};
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
    xr_sha256_update(&ctx, facts, sizeof(facts));
    xr_sha256_final(&ctx, fingerprint.bytes);
    return fingerprint;
}

XrFingerprint xr_aot_representation_policy_fingerprint(const XiRepPolicy *policy) {
    XiRepPolicy default_policy = xi_rep_policy_native_boundary();
    return rep_policy_fingerprint(policy ? policy : &default_policy);
}

static void set_diag(XrAotRefinementDiagnostic *diag, uint32_t issue, uint32_t record,
                     uint32_t value, uint32_t operation) {
    if (!diag)
        return;
    memset(diag, 0, sizeof(*diag));
    diag->issue = issue;
    diag->record_index = record;
    diag->pass_id = 27902;
    diag->semantic_value = value;
    diag->semantic_operation = operation;
}

static uint32_t live_builtin_type(const XrType *type) {
    static const struct {
        const char *name;
        uint32_t id;
    } builtins[] = {
        {"StringBuilder", XR_TID_STRINGBUILDER},   {"Task", XR_TID_COROUTINE},
        {"WorkQueue", XR_TID_WORKQUEUE},           {"ResultGroup", XR_TID_RESULTGROUP},
        {"CountdownLatch", XR_TID_COUNTDOWNLATCH}, {"Semaphore", XR_TID_SEMAPHORE},
        {"EventCount", XR_TID_EVENTCOUNT},
    };
    for (size_t index = 0; index < sizeof(builtins) / sizeof(builtins[0]); index++)
        if (xr_type_is_builtin_named_class(type, builtins[index].name))
            return builtins[index].id;
    return XR_TID_NULL;
}

static bool source_type_matches(const XrType *live, const XrSemanticTypeRecord *semantic) {
    if (!live || !semantic || (uint32_t) live->kind != semantic->kind ||
        live->scalar_rep != semantic->scalar_rep ||
        live_builtin_type(live) != semantic->builtin_type)
        return false;
    uint8_t flags = (uint8_t) ((live->is_nullable ? XR_SEM_TYPE_NULLABLE : 0u) |
                               (live->is_const ? XR_SEM_TYPE_CONST : 0u) |
                               (live->is_value_type ? XR_SEM_TYPE_VALUE : 0u) |
                               (live->is_literal ? XR_SEM_TYPE_LITERAL : 0u) |
                               (xi_own_type_is_rc(live) ? XR_SEM_TYPE_REFERENCE_CAPABLE : 0u) |
                               (live->kind == XR_KIND_SLICE ? XR_SEM_TYPE_BORROW_VIEW : 0u) |
                               (xi_own_type_is_rc(live) && live->kind != XR_KIND_SLICE
                                    ? XR_SEM_TYPE_OWNERSHIP_ROOT
                                    : 0u));
    return (semantic->flags & ~XR_SEM_TYPE_AGGREGATE_EXACT) == flags;
}

/* The single place a machine representation names the storage class that holds
 * it.  Every judgement about such a pair is this one judgement, so no second
 * derivation can fall behind it: while a second copy existed the two disagreed
 * about the void representation, and a channel receive carrying unit was
 * refused for that disagreement rather than for anything about the receive.  A
 * kind this switch does not name has no storage class at all, which every
 * caller must take as a refusal. */
static bool machine_storage_class(uint16_t machine, XrRep *out_storage) {
    switch ((XrMachineRepKind) machine) {
        case XR_MACHINE_REP_I1:
        case XR_MACHINE_REP_I8:
        case XR_MACHINE_REP_U8:
        case XR_MACHINE_REP_I16:
        case XR_MACHINE_REP_U16:
        case XR_MACHINE_REP_I32:
        case XR_MACHINE_REP_U32:
        case XR_MACHINE_REP_I64:
        case XR_MACHINE_REP_ENUM_ORDINAL:
        case XR_MACHINE_REP_U64:
        case XR_MACHINE_REP_ISIZE:
        case XR_MACHINE_REP_USIZE:
        case XR_MACHINE_REP_RUNE:
            *out_storage = XR_REP_I64;
            return true;
        case XR_MACHINE_REP_F32:
        case XR_MACHINE_REP_F64:
            *out_storage = XR_REP_F64;
            return true;
        case XR_MACHINE_REP_RAW_PTR:
            *out_storage = XR_REP_RAWPTR;
            return true;
        case XR_MACHINE_REP_VOID:
            /* A value the TargetPlan bound to the void representation carries
             * nothing: unit is the whole of what it says.  That is a named
             * storage, not a missing one -- the plan binds the rep and
             * deliberately gives it no slot -- so leaving it out of this switch
             * made every use of a unit result look like a value whose storage
             * no oracle knows, which is a different verdict entirely. */
            *out_storage = XR_REP_VOID;
            return true;
        default:
            return false;
    }
}

typedef struct CollectContext {
    const XrTargetPlan *target_plan;
    const XrSemanticPlan *semantic;
    const XiRepPolicy *policy;
    XrAotRefinementBuilder *builder;
    XrAotPassProtocol protocol;
    uint32_t record_count;
    uint32_t scan_count;
    uint32_t semantic_value_count;
    uint32_t *operation_by_value;
    uint32_t *parameter_by_value;
    uint32_t *use_count_by_value;
    uint32_t operation_count;
    uint32_t *call_by_operation;
    uint32_t type_count;
    uint32_t *layout_by_type;
    XrAotRefinementDiagnostic *diag;
} CollectContext;

static void collect_indices_dispose(CollectContext *ctx);

static bool collect_indices_init(CollectContext *ctx) {
    uint32_t function_count = (uint32_t) xr_semantic_plan_function_count(ctx->semantic);
    uint64_t value_count = 0;
    for (uint32_t i = 0; i < function_count; i++) {
        const XrSemanticFunctionRecord *function = xr_semantic_plan_function(ctx->semantic, i);
        if (function && (uint64_t) function->value_begin + function->value_count > value_count)
            value_count = (uint64_t) function->value_begin + function->value_count;
    }
    size_t operation_count = xr_semantic_plan_operation_count(ctx->semantic);
    if (operation_count > XR_AOT_REFINEMENT_MAX_RECORDS)
        return false;
    ctx->operation_count = (uint32_t) operation_count;
    ctx->type_count = (uint32_t) xr_semantic_plan_type_count(ctx->semantic);
    if (value_count > XR_AOT_REFINEMENT_MAX_RECORDS ||
        ctx->type_count > XR_AOT_REFINEMENT_MAX_RECORDS)
        return false;
    ctx->semantic_value_count = (uint32_t) value_count;
    ctx->operation_by_value = (uint32_t *) xr_malloc(
        (ctx->semantic_value_count ? ctx->semantic_value_count : 1u) * sizeof(uint32_t));
    ctx->parameter_by_value = (uint32_t *) xr_malloc(
        (ctx->semantic_value_count ? ctx->semantic_value_count : 1u) * sizeof(uint32_t));
    ctx->use_count_by_value = (uint32_t *) xr_calloc(
        ctx->semantic_value_count ? ctx->semantic_value_count : 1u, sizeof(uint32_t));
    ctx->call_by_operation = (uint32_t *) xr_malloc(
        (ctx->operation_count ? ctx->operation_count : 1u) * sizeof(uint32_t));
    ctx->layout_by_type =
        (uint32_t *) xr_malloc((ctx->type_count ? ctx->type_count : 1u) * sizeof(uint32_t));
    if (!ctx->operation_by_value || !ctx->parameter_by_value || !ctx->use_count_by_value ||
        !ctx->call_by_operation || !ctx->layout_by_type) {
        collect_indices_dispose(ctx);
        return false;
    }
    for (uint32_t i = 0; i < ctx->semantic_value_count; i++) {
        ctx->operation_by_value[i] = XR_SEMANTIC_INDEX_NONE;
        ctx->parameter_by_value[i] = XR_SEMANTIC_INDEX_NONE;
    }
    for (uint32_t i = 0; i < ctx->type_count; i++)
        ctx->layout_by_type[i] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < ctx->operation_count; i++)
        ctx->call_by_operation[i] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(ctx->semantic, i);
        if (!operation || operation->result_value >= ctx->semantic_value_count ||
            ctx->operation_by_value[operation->result_value] != XR_SEMANTIC_INDEX_NONE)
            goto invalid;
        ctx->operation_by_value[operation->result_value] = i;
    }
    uint32_t parameter_count = (uint32_t) xr_semantic_plan_parameter_count(ctx->semantic);
    for (uint32_t i = 0; i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(ctx->semantic, i);
        if (!parameter || parameter->value >= ctx->semantic_value_count ||
            ctx->parameter_by_value[parameter->value] != XR_SEMANTIC_INDEX_NONE)
            goto invalid;
        ctx->parameter_by_value[parameter->value] = i;
    }
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type >= ctx->type_count ||
            ctx->layout_by_type[layouts[i].semantic_type] != XR_SEMANTIC_INDEX_NONE)
            goto invalid;
        ctx->layout_by_type[layouts[i].semantic_type] = i;
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    for (uint32_t i = 0; i < operand_count; i++) {
        if (!operands || operands[i].value >= ctx->semantic_value_count ||
            ctx->use_count_by_value[operands[i].value] == UINT32_MAX)
            goto invalid;
        ctx->use_count_by_value[operands[i].value]++;
    }
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target_plan, &call_count);
    for (uint32_t i = 0; i < call_count; i++) {
        if (!calls || calls[i].id != i || calls[i].semantic_operation >= ctx->operation_count ||
            ctx->call_by_operation[calls[i].semantic_operation] != XR_SEMANTIC_INDEX_NONE)
            goto invalid;
        ctx->call_by_operation[calls[i].semantic_operation] = i;
    }
    return true;
invalid:
    collect_indices_dispose(ctx);
    return false;
}

static void collect_indices_dispose(CollectContext *ctx) {
    xr_free(ctx->operation_by_value);
    xr_free(ctx->parameter_by_value);
    xr_free(ctx->use_count_by_value);
    xr_free(ctx->call_by_operation);
    xr_free(ctx->layout_by_type);
    ctx->operation_by_value = NULL;
    ctx->parameter_by_value = NULL;
    ctx->use_count_by_value = NULL;
    ctx->call_by_operation = NULL;
    ctx->layout_by_type = NULL;
}

#define XR_AOT_REP_VERIFY_MAX_BYTES (UINT64_C(128) * 1024u * 1024u)
#define XR_AOT_REP_VERIFY_MAX_WORK (UINT64_C(32) * XR_AOT_REFINEMENT_MAX_RECORDS)
#define XR_AOT_REP_VERIFY_MAX_TYPE_KEY_BYTES UINT32_C(1048576)
#define XR_AOT_REP_VERIFY_MAX_FUNCTION_DEPTH UINT32_C(1024)

typedef struct VerifyAuthority {
    const XrTargetPlan *target_plan;
    const XrSemanticPlan *semantic;
    const XiRepPolicy *policy;
    const XrAotRefinementPlanView *view;
    XrAotRefinementDiagnostic *diag;
    XrFingerprint policy_fingerprint;
    uint32_t function_count;
    uint32_t block_count;
    uint32_t value_count;
    uint32_t operation_count;
    uint32_t parameter_count;
    uint32_t type_count;
    uint32_t *operation_by_value;
    uint32_t *parameter_by_value;
    uint32_t *use_count_by_value;
    uint32_t *call_by_operation;
    uint32_t *layout_by_type;
    uint32_t *direct_callee_target_by_value;
    uint32_t *go_callee_target_by_value;
    const XiValue **live_by_value;
    const XiBlock **live_by_block;
    uint8_t *seen_function;
    uint8_t *seen_block;
    uint8_t *seen_record;
    uint8_t *exact_direct_callee_value;
    uint8_t *exact_go_callee_value;
    uint8_t *exact_channel_value;
    uint8_t *exact_channel_allocation_value;
    uint8_t *exact_source_namespace_value;
    uint32_t *source_namespace_dependency_by_value;
    uint8_t *exact_native_module_namespace_value;
    uint64_t bytes;
    uint64_t work;
} VerifyAuthority;

/* Setting XRAY_AOT_REFINE_TRACE prints, on stderr, why this pass refused a
 * program. Unset, the variable is never read and nothing is printed: it is
 * consulted only on the path that is already failing the build, so a passing
 * compile pays nothing for it.
 *
 * It exists because XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE, on its
 * own, cannot be read. `value=N operation=M` names two independent things: N is
 * the value being consumed and M is the operation consuming it. The operation
 * that DEFINED N is a third index, and it is not M -- reading M as "where N
 * comes from" has misdirected more than one investigation. The trace prints all
 * three under their own names, plus the storage each side asked for, so the
 * refusal can be placed without adding temporary instrumentation. */
static bool rep_trace_enabled(void) {
    return getenv("XRAY_AOT_REFINE_TRACE") != NULL;
}

/* Setting XRAY_COLLECT_ALL_REFUSALS makes this pass keep walking after a
 * refusal instead of stopping at the first one, printing each on stderr under
 * [refusal-survey]. The build still fails: an operand this pass refused got no
 * obligation, so the plan cannot freeze whatever the operands after it answer.
 * What changes is that one compile says how many separate oracle branches a
 * module is missing rather than only the lowest-numbered one, which is what
 * separates "this program needs a fix" from "this program needs five".
 *
 * The same variable drives the same walk in the TargetPlan pass, so one run
 * surveys whichever pass a program reaches. What the count is not is a proof of
 * independence: the walk continues over a builder missing the obligation the
 * refused operand would have carried.
 *
 * Unset, the variable is never read: every call sits on a path that has already
 * refused, so a compile that succeeds pays nothing. */
static bool rep_survey_enabled(void) {
    return getenv("XRAY_COLLECT_ALL_REFUSALS") != NULL;
}

/* What a reader needs to place a refusal in the oracle: which side declined,
 * the opcode of the operation that DEFINED the value (which is the branch a
 * definition-side fix extends) and the opcode consuming it (the branch a
 * use-side fix extends). The two are different operations, and naming only one
 * of them is what makes the bare diagnostic unreadable.
 *
 * The opcode pair alone cannot say whether two refusals are two obligations or
 * one obligation spelled twice, and counting rows by that pair overstates the
 * work by the number of spellings. The representation the TargetPlan already
 * froze for the value, and the shape of its semantic type, are what an
 * obligation actually turns on, so both are printed beside the opcodes: rows
 * that agree on those and differ only in opcode are one rule, and a row whose
 * value carries no frozen representation at all is a gap in the pass upstream
 * of this one rather than a missing branch here. */
static void rep_survey_row(const CollectContext *ctx, const char *side, uint32_t source_value,
                           uint32_t use_operation, uint16_t operand) {
    uint32_t definer = ctx && ctx->operation_by_value && source_value < ctx->semantic_value_count
                           ? ctx->operation_by_value[source_value]
                           : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *def = definer != XR_SEMANTIC_INDEX_NONE
                                               ? xr_semantic_plan_operation(ctx->semantic, definer)
                                               : NULL;
    const XrSemanticOperationRecord *use = xr_semantic_plan_operation(ctx->semantic, use_operation);
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(ctx->semantic, &metadata_count);
    const char *selector = use && use->metadata_count == 1 && use->metadata_begin < metadata_count
                               ? metadata[use->metadata_begin]
                               : "";
    const XrTargetValueRepRecord *binding =
        ctx ? xr_target_plan_value_rep(ctx->target_plan, source_value) : NULL;
    const XrTargetMachineRepRecord *machine =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    uint32_t parameter_index =
        ctx && ctx->parameter_by_value && source_value < ctx->semantic_value_count
            ? ctx->parameter_by_value[source_value]
            : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticParameterRecord *parameter =
        parameter_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(ctx->semantic, parameter_index)
            : NULL;
    uint32_t type_index = parameter ? parameter->type
                          : def     ? def->result_type
                                    : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(ctx->semantic, type_index);
    /* XR_MACHINE_REP_COUNT means the plan bound no representation for the
     * value; one past it means it bound one this pass could not resolve. */
    fprintf(stderr,
            "[refusal-survey] family=refinement_%s_oracle definer-opcode=%u use-opcode=%u "
            "selector=%s operand=%u value-machine=%u value-shape=%u value-flags=%u\n",
            side, def ? def->opcode : 9999u, use ? use->opcode : 9999u, selector, operand,
            machine   ? (unsigned) machine->kind
            : binding ? (unsigned) XR_MACHINE_REP_COUNT + 1u
                      : (unsigned) XR_MACHINE_REP_COUNT,
            type ? (unsigned) (type->child_count != 0 ? 1u : 0u) |
                       (unsigned) (type->aggregate_extent != 0 ? 2u : 0u)
                 : 4u,
            type ? (unsigned) type->flags : 0u);
}

/* XR_REP_COUNT stands for "this side never named a storage". */
static const char *rep_trace_storage_name(XrRep storage) {
    switch (storage) {
        case XR_REP_I64:
            return "i64";
        case XR_REP_F64:
            return "f64";
        case XR_REP_PTR:
            return "ptr";
        case XR_REP_TAGGED:
            return "tagged";
        case XR_REP_VOID:
            return "void";
        case XR_REP_STR:
            return "str";
        case XR_REP_RAWPTR:
            return "rawptr";
        default:
            return "unnamed";
    }
}

/* Where a value came from: the parameter that bound it on entry, or the
 * operation that produced it. This is the index a reader of the diagnostic
 * tends to assume `operation=` already holds. */
static void rep_trace_origin(const VerifyAuthority *ctx, uint32_t value) {
    if (!ctx || value == XR_SEMANTIC_INDEX_NONE || value >= ctx->value_count) {
        fprintf(stderr, "[aot-refine]     defined by      = <value out of range>\n");
        return;
    }
    uint32_t parameter_index =
        ctx->parameter_by_value ? ctx->parameter_by_value[value] : XR_SEMANTIC_INDEX_NONE;
    if (parameter_index != XR_SEMANTIC_INDEX_NONE) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(ctx->semantic, parameter_index);
        fprintf(stderr,
                "[aot-refine]     defined by      = parameter %u (ordinal %u) of function %u, "
                "semantic type %u\n",
                parameter_index, parameter ? (unsigned) parameter->ordinal : 0u,
                parameter ? parameter->function : 0u, parameter ? parameter->type : 0u);
        return;
    }
    uint32_t operation_index =
        ctx->operation_by_value ? ctx->operation_by_value[value] : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    if (!operation) {
        fprintf(stderr,
                "[aot-refine]     defined by      = nothing this pass indexed (no parameter, no "
                "operation)\n");
        return;
    }
    fprintf(stderr,
            "[aot-refine]     defined by      = operation %u %s in function %u, block %u, "
            "semantic type %u\n",
            operation_index, xi_op_name(operation->opcode), operation->function, operation->block,
            operation->result_type);
    if (operation->source_file)
        fprintf(stderr, "[aot-refine]                       %s:%u\n", operation->source_file,
                operation->source_line);
}

/* One refusal, printed whole. `stage` names the judgement that said no,
 * `reason` says what it wanted. Either storage may be XR_REP_COUNT when that
 * side of the question never got as far as naming one. */
static void rep_trace_refusal(const VerifyAuthority *ctx, const char *stage, const char *reason,
                              uint32_t source_value, uint32_t use_operation, uint16_t use_operand,
                              XrRep definition_storage, XrRep use_storage) {
    if (!rep_trace_enabled())
        return;
    fprintf(stderr, "[aot-refine] refused in %s: %s\n", stage, reason);
    fprintf(stderr, "[aot-refine]   value=%u          the value being consumed (the source)\n",
            source_value);
    rep_trace_origin(ctx, source_value);
    fprintf(stderr, "[aot-refine]     definition rep  = %s\n",
            rep_trace_storage_name(definition_storage));
    if (use_operation == XR_SEMANTIC_INDEX_NONE) {
        fprintf(stderr,
                "[aot-refine]   operation=<none>  the use is a block return, not an operand\n");
    } else {
        const XrSemanticOperationRecord *operation =
            ctx ? xr_semantic_plan_operation(ctx->semantic, use_operation) : NULL;
        fprintf(stderr,
                "[aot-refine]   operation=%u      the USE SITE consuming that value -- not where "
                "it is defined\n",
                use_operation);
        if (operation) {
            fprintf(stderr,
                    "[aot-refine]     opcode          = %s, operand %u, function %u, block %u\n",
                    xi_op_name(operation->opcode), (unsigned) use_operand, operation->function,
                    operation->block);
            if (operation->source_file)
                fprintf(stderr, "[aot-refine]                       %s:%u\n",
                        operation->source_file, operation->source_line);
        }
    }
    fprintf(stderr, "[aot-refine]     required rep    = %s\n", rep_trace_storage_name(use_storage));
    fprintf(stderr,
            "[aot-refine]   read it as: operation=%u consumes value=%u; the operation that "
            "defines value=%u is the one printed under \"defined by\" above, and the two indexes "
            "are unrelated.\n",
            use_operation, source_value, source_value);
}

static bool oracle_dynamic_array_intrinsic_storage(const VerifyAuthority *ctx,
                                                   uint32_t semantic_value, XrRep *out_storage,
                                                   uint16_t *out_machine_kind);
static bool oracle_dynamic_array_fill_scalar_storage(const VerifyAuthority *ctx,
                                                     uint32_t semantic_value, XrRep *out_storage,
                                                     uint16_t *out_machine_kind);
static bool oracle_array_hof_result_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                            XrRep *out_storage, uint16_t *out_machine_kind);
static bool oracle_array_produced_tagged_carrier_storage(const VerifyAuthority *ctx,
                                                         uint32_t semantic_value,
                                                         XrRep *out_storage,
                                                         uint16_t *out_machine_kind);
static bool oracle_array_borrowed_tagged_carrier_storage(const VerifyAuthority *ctx,
                                                         uint32_t semantic_value,
                                                         XrRep *out_storage,
                                                         uint16_t *out_machine_kind);
static bool oracle_array_tagged_carrier_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                                XrRep *out_storage, uint16_t *out_machine_kind);
static bool oracle_string_tagged_carrier_storage(const VerifyAuthority *ctx,
                                                 uint32_t semantic_value, XrRep *out_storage,
                                                 uint16_t *out_machine_kind);
static bool oracle_tagged_reference_carrier_storage(const VerifyAuthority *ctx,
                                                    uint32_t semantic_value, XrRep *out_storage,
                                                    uint16_t *out_machine_kind);
static bool oracle_direct_local_array_value_parameter_storage(const VerifyAuthority *ctx,
                                                              uint32_t semantic_value,
                                                              XrRep *out_storage,
                                                              uint16_t *out_machine_kind);
static bool oracle_dynamic_direct_local_array_result_storage(const VerifyAuthority *ctx,
                                                             uint32_t semantic_value,
                                                             XrRep *out_storage,
                                                             uint16_t *out_machine_kind);
static bool oracle_resolve_identity_rename(const VerifyAuthority *ctx, uint32_t semantic_value,
                                           uint32_t *out_value);

static bool verify_charge_work(VerifyAuthority *ctx, uint64_t amount) {
    if (!ctx || amount > XR_AOT_REP_VERIFY_MAX_WORK - ctx->work) {
        if (ctx)
            set_diag(ctx->diag, XR_AOT_REFINEMENT_RESOURCE_BUDGET, (uint32_t) ctx->work, 0, 0);
        return false;
    }
    ctx->work += amount;
    return true;
}

static bool verify_alloc(VerifyAuthority *ctx, uint32_t count, size_t width, void **out) {
    if (out)
        *out = NULL;
    uint64_t rows = count ? count : 1u;
    if (!ctx || !out || width == 0 || rows > UINT64_MAX / width ||
        rows * width > XR_AOT_REP_VERIFY_MAX_BYTES - ctx->bytes) {
        if (ctx)
            set_diag(ctx->diag, XR_AOT_REFINEMENT_RESOURCE_BUDGET, 0, 0, 0);
        return false;
    }
    void *memory = xr_calloc((size_t) rows, width);
    if (!memory) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_OUT_OF_MEMORY, 0, 0, 0);
        return false;
    }
    ctx->bytes += rows * width;
    *out = memory;
    return true;
}

static void verify_authority_dispose(VerifyAuthority *ctx) {
    if (!ctx)
        return;
    xr_free(ctx->operation_by_value);
    xr_free(ctx->parameter_by_value);
    xr_free(ctx->use_count_by_value);
    xr_free(ctx->call_by_operation);
    xr_free(ctx->layout_by_type);
    xr_free(ctx->direct_callee_target_by_value);
    xr_free(ctx->go_callee_target_by_value);
    xr_free(ctx->live_by_value);
    xr_free(ctx->live_by_block);
    xr_free(ctx->seen_function);
    xr_free(ctx->seen_block);
    xr_free(ctx->seen_record);
    xr_free(ctx->exact_direct_callee_value);
    xr_free(ctx->exact_go_callee_value);
    xr_free(ctx->exact_channel_value);
    xr_free(ctx->exact_channel_allocation_value);
    xr_free(ctx->exact_source_namespace_value);
    xr_free(ctx->source_namespace_dependency_by_value);
    xr_free(ctx->exact_native_module_namespace_value);
    ctx->operation_by_value = NULL;
    ctx->parameter_by_value = NULL;
    ctx->use_count_by_value = NULL;
    ctx->call_by_operation = NULL;
    ctx->layout_by_type = NULL;
    ctx->direct_callee_target_by_value = NULL;
    ctx->go_callee_target_by_value = NULL;
    ctx->live_by_value = NULL;
    ctx->live_by_block = NULL;
    ctx->seen_function = NULL;
    ctx->seen_block = NULL;
    ctx->seen_record = NULL;
    ctx->exact_direct_callee_value = NULL;
    ctx->exact_go_callee_value = NULL;
    ctx->exact_channel_value = NULL;
    ctx->exact_channel_allocation_value = NULL;
    ctx->exact_source_namespace_value = NULL;
    ctx->source_namespace_dependency_by_value = NULL;
    ctx->exact_native_module_namespace_value = NULL;
}

static bool verify_bounded_string_length(VerifyAuthority *ctx, const char *text,
                                         size_t *out_length) {
    if (out_length)
        *out_length = 0;
    if (!ctx || !text || !out_length)
        return false;
    size_t length = 0;
    while (length < XR_AOT_REP_VERIFY_MAX_TYPE_KEY_BYTES && text[length])
        length++;
    if (length == XR_AOT_REP_VERIFY_MAX_TYPE_KEY_BYTES) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_RESOURCE_BUDGET, (uint32_t) ctx->work, 0, 0);
        return false;
    }
    if (!verify_charge_work(ctx, length + 1u))
        return false;
    *out_length = length;
    return true;
}

static bool verify_scalar_type_authority(VerifyAuthority *ctx, uint32_t type_index,
                                         const XrType *live) {
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(ctx ? ctx->semantic : NULL, type_index);
    if (!ctx || !live || !type || !type->canonical_key || !source_type_matches(live, type) ||
        type->child_count != 0 || type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        (type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) != 0)
        return false;
    switch (live->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_NULL:
        case XR_KIND_NEVER:
        case XR_KIND_UNIT:
        case XR_KIND_POINTER:
        case XR_KIND_RUNE:
            break;
        default:
            return false;
    }
    const char *alias = live->alias_name ? live->alias_name : "";
    size_t alias_length = 0;
    size_t canonical_length = 0;
    if (!verify_bounded_string_length(ctx, alias, &alias_length) ||
        !verify_bounded_string_length(ctx, type->canonical_key, &canonical_length))
        return false;
    char prefix[256];
    int prefix_length = snprintf(
        prefix, sizeof(prefix), "type-v3:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%zu:", (unsigned) live->kind,
        live->semantic_type_id, live_builtin_type(live), live->is_nullable ? 1u : 0u,
        live->is_const ? 1u : 0u, live->is_value_type ? 1u : 0u, live->is_literal ? 1u : 0u,
        live->is_cycle_candidate ? 1u : 0u, live->ptr_is_mut ? 1u : 0u, (unsigned) live->scalar_rep,
        alias_length);
    return prefix_length > 0 && (size_t) prefix_length < sizeof(prefix) &&
           canonical_length == (size_t) prefix_length + alias_length &&
           memcmp(type->canonical_key, prefix, (size_t) prefix_length) == 0 &&
           memcmp(type->canonical_key + prefix_length, alias, alias_length) == 0;
}

static bool semantic_heap_closure_is_exact(const XrSemanticPlan *semantic,
                                           const XrSemanticOperationRecord *operation) {
    if (!semantic || !operation || operation->opcode != XI_CLOSURE_NEW ||
        operation->callable_function >= xr_semantic_plan_function_count(semantic) ||
        operation->operand_count != 0 || !xr_semantic_allocation_identity_is_canonical(operation) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED)
        return false;
    const XrSemanticFunctionRecord *callee =
        xr_semantic_plan_function(semantic, operation->callable_function);
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, operation->result_type);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    bool typed_function = type && type->kind == XR_KIND_FUNCTION;
    bool opaque_closure = type && type->kind == XR_KIND_UNKNOWN && type->child_count == 0;
    if (!callee || !type || callee->parent != operation->function || callee->capture_count != 0 ||
        (!typed_function && !opaque_closure) || type->aggregate_extent != 0 ||
        type->aggregate_align != 0 ||
        (type->flags & (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE | XR_SEM_TYPE_BORROW_VIEW |
                        XR_SEM_TYPE_AGGREGATE_EXACT)) != 0 ||
        (type->flags & (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT)) !=
            (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        callee->parameter_count == UINT16_MAX ||
        (typed_function && type->child_count != (uint32_t) callee->parameter_count + 1u) ||
        type->child_begin > child_count || type->child_count > child_count - type->child_begin ||
        callee->parameter_begin > xr_semantic_plan_parameter_count(semantic) ||
        callee->parameter_count >
            xr_semantic_plan_parameter_count(semantic) - callee->parameter_begin)
        return false;
    for (uint32_t i = 0; i < callee->parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(semantic, callee->parameter_begin + i);
        if (!parameter || parameter->function != operation->callable_function ||
            parameter->ordinal != i ||
            (typed_function && children[type->child_begin + i] != parameter->type))
            return false;
    }
    return opaque_closure ||
           children[type->child_begin + callee->parameter_count] == callee->return_type;
}

static bool semantic_panic_catch_is_exact(const XrSemanticPlan *semantic,
                                          const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
    if (!semantic || !operation || operation->opcode != XI_CATCH ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->function >= xr_semantic_plan_function_count(semantic) ||
        operation->operand_count != 0 || operation->metadata_count != 0 ||
        operation->allocation_key != NULL || !xr_stable_id_equal(operation->allocation_id, zero) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->semantic_immediate != 0 || operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->effects != xi_generated_op_effects(XI_CATCH) ||
        operation->flags != xi_generated_op_default_flags(XI_CATCH) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CATCH) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE || operation->result_alias_operand != -1 ||
        operation->return_parameter != -1 || operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_complete != 1 || operation->view_source_value != XR_SEMANTIC_INDEX_NONE ||
        operation->view_element_type != XR_SEMANTIC_INDEX_NONE ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        operation->view_origin != XI_VIEW_ORIGIN_NONE || operation->view_capability != 0 ||
        operation->view_lifetime != 0 || operation->view_complete != 0)
        return false;
    for (uint32_t i = 0; i < 8; i++)
        if (operation->evidence[i] != (i == 7 ? XR_SEMANTIC_INDEX_NONE : 0u))
            return false;
    return type && type->kind == XR_KIND_UNKNOWN && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE && type->source_enum_key == NULL &&
           xr_stable_id_equal(type->source_enum_identity, zero) &&
           xr_stable_id_equal(type->source_class_identity, zero) && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 && type->enum_layout_id == 0 &&
           type->enum_member_count == 0 && type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           type->enum_flags == 0 && type->reserved_enum == 0;
}

/* The one integer spelling this container authority admits. Every narrower or
 * unsigned spelling carries a second element representation the array storage
 * fact does not state, so it stays outside the shape. */
static bool semantic_exact_i64_type(const XrSemanticTypeRecord *type) {
    return type && type->kind == XR_KIND_INT && type->scalar_rep == XR_NATIVE_I64 &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->flags == 0 && type->builtin_type == XR_TID_NULL;
}

/* Rebuilt independently of both the TargetPlan collector and its verifier: a
 * freshly allocated `Array<T>` is an owned heap object whose only storage fact
 * is the outer tagged XrValue, the same fact a heap closure allocation carries.
 * The element entry must be an exact signed 64-bit integer, so no element store
 * leaves a reference-count obligation behind and the element carries exactly one
 * native representation. Every other container shape stays fail closed. */
static bool aot_array_intrinsic_storage_is_exact(const XrSemanticTypeRecord *element,
                                                 uint8_t semantic_storage, uint8_t *target_storage);
static bool aot_array_type_is_exact(const XrSemanticPlan *semantic, uint32_t type_index,
                                    bool indexes_elements, uint8_t *storage);

static bool aot_array_allocation_is_exact(const XrSemanticPlan *semantic,
                                          const XrSemanticOperationRecord *operation,
                                          uint8_t *out_storage) {
    char expected_type_key[96];
    int written = snprintf(expected_type_key, sizeof(expected_type_key),
                           "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;element:", (unsigned) XR_KIND_ARRAY,
                           (unsigned) XR_TID_NULL, (unsigned) XR_SCALAR_REP_NONE);
    uint32_t operand_count = 0, child_count = 0;
    const XrSemanticOperandRecord *operands =
        semantic ? xr_semantic_plan_operands(semantic, &operand_count) : NULL;
    const uint32_t *children =
        semantic ? xr_semantic_plan_type_children(semantic, &child_count) : NULL;
    XrStableId zero = {{0}};
    if (!semantic || !operation || !operands || !children || written <= 0 ||
        (size_t) written >= sizeof(expected_type_key) || operation->opcode != XI_ARRAY_NEW ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE || operation->metadata_count != 0 ||
        operation->semantic_immediate != 0 || operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->effects != xi_generated_op_effects(XI_ARRAY_NEW) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_complete != 1 ||
        operation->return_parameter != -1 || operation->result_alias_operand != -1 ||
        !xr_semantic_allocation_identity_is_canonical(operation))
        return false;
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, operation->result_type);
    if (!type || type->kind != XR_KIND_ARRAY || type->builtin_type != XR_TID_NULL ||
        type->child_count != 1 || type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->scalar_rep != XR_SCALAR_REP_NONE ||
        type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        type->source_class != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(type->source_class_identity, zero) || !type->canonical_key ||
        strncmp(type->canonical_key, expected_type_key, (size_t) written) != 0 ||
        type->child_begin >= child_count)
        return false;
    const XrSemanticTypeRecord *element =
        xr_semantic_plan_type(semantic, children[type->child_begin]);
    const XrSemanticOperandRecord *capacity = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *capacity_type = xr_semantic_plan_type(semantic, capacity->type);
    const XrSemanticFunctionRecord *function =
        xr_semantic_plan_function(semantic, operation->function);
    uint8_t semantic_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    uint8_t type_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    bool exact = element && capacity_type && function &&
                 (element->flags & XR_SEM_TYPE_REFERENCE_CAPABLE) == 0 &&
                 aot_array_intrinsic_storage_is_exact(element, operation->array_element_storage,
                                                      &semantic_storage) &&
                 aot_array_type_is_exact(semantic, operation->result_type, true, &type_storage) &&
                 semantic_storage == type_storage && semantic_exact_i64_type(capacity_type) &&
                 capacity->role == XR_SEM_OPERAND_VALUE && capacity->parameter == -1 &&
                 capacity->flags == 0 && capacity->ownership_action == XR_SEM_OPERAND_CONSUME &&
                 operation->result_value >= function->value_begin &&
                 operation->result_value < function->value_begin + function->value_count;
    if (exact && out_storage)
        *out_storage = semantic_storage;
    return exact;
}

/* A bare object literal's type. Unlike a tuple or a value class it is not an
 * aggregate slot at all: the shared aggregate judgement answers 0 for it,
 * because it is reference capable and roots its own ownership, so the target
 * plan names no representation for it and the value lives entirely in the
 * tagged carrier its runtime allocation hands back.
 *
 * A struct object carrying the value flag is the separate exact-aggregate
 * spelling the plan does place, and it must not be claimed here. */
static bool aot_struct_object_type_is_exact(const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    return type && type->kind == XR_KIND_STRUCT_OBJECT &&
           type->flags == (uint8_t) (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->builtin_type == XR_TID_NULL &&
           type->child_count != 0 && type->aggregate_extent == type->child_count &&
           type->aggregate_align == 0 && type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) && !type->source_enum_key &&
           type->enum_layout_id == 0 && type->enum_member_count == 0 && type->enum_flags == 0 &&
           type->reserved_enum == 0 && type->canonical_key != NULL;
}

/* Rebuilt independently of both the TargetPlan collector and its verifier: a
 * bare object literal is an owned heap allocation whose fields are named by the
 * operation's own metadata and counted by its immediate, and whose only storage
 * fact is the outer tagged value. The lanes are written by the field-init
 * operations that follow, so the allocation itself takes no operand. */
static bool aot_struct_object_allocation_is_exact(const XrSemanticPlan *semantic,
                                                  const XrSemanticOperationRecord *operation) {
    uint32_t metadata_count = 0;
    const char *const *metadata =
        semantic ? xr_semantic_plan_metadata(semantic, &metadata_count) : NULL;
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
    if (!semantic || !operation || !metadata || operation->opcode != XI_OBJECT_NEW ||
        operation->operand_count != 0 || operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        !aot_struct_object_type_is_exact(type) ||
        operation->semantic_immediate != (int64_t) type->child_count ||
        operation->metadata_count != type->child_count ||
        operation->metadata_begin > metadata_count ||
        operation->metadata_count > metadata_count - operation->metadata_begin ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->effects != xi_generated_op_effects(XI_OBJECT_NEW) ||
        operation->flags != xi_generated_op_default_flags(XI_OBJECT_NEW) ||
        operation->ownership_use != xi_generated_op_own_use(XI_OBJECT_NEW) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_OBJECT_NEW) ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1 || operation->view_complete != 0 ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        !xr_semantic_allocation_identity_is_canonical(operation))
        return false;
    for (uint16_t i = 0; i < type->child_count; i++)
        if (!metadata[operation->metadata_begin + i] || !metadata[operation->metadata_begin + i][0])
            return false;
    return true;
}

/* The borrowed read of a bare object literal held in a shared cell. A local
 * variable is a shared cell, so every mention of the object after the
 * assignment is a load whose result is the same allocation the construction
 * produced; demanding the allocation instruction itself would refuse every
 * object literal that outlives its own defining instruction, which is every
 * one bound to a variable. */
static bool aot_struct_object_shared_read_is_exact(const XrSemanticPlan *semantic,
                                                   const XrSemanticOperationRecord *operation) {
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
    XrStableId zero = {{0}};
    return semantic && operation && operation->opcode == XI_GET_SHARED &&
           operation->operand_count == 0 && operation->metadata_count == 0 &&
           operation->result_value != XR_SEMANTIC_INDEX_NONE &&
           aot_struct_object_type_is_exact(type) && operation->semantic_immediate >= 0 &&
           operation->semantic_immediate <= UINT16_MAX && !operation->allocation_key &&
           xr_stable_id_equal(operation->allocation_id, zero) &&
           operation->constant == XR_SEMANTIC_INDEX_NONE &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->auxiliary_kind == XI_AUX_KIND_NONE &&
           operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
           operation->intrinsic_kind == XR_SEM_INTRINSIC_NONE &&
           operation->effects == xi_generated_op_effects(XI_GET_SHARED) &&
           operation->flags == xi_generated_op_default_flags(XI_GET_SHARED) &&
           operation->ownership_use == xi_generated_op_own_use(XI_GET_SHARED) &&
           operation->result_ownership == xi_generated_op_result_ownership(XI_GET_SHARED) &&
           operation->result_alias_operand == -1 &&
           operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
           operation->return_parameter == -1 && operation->return_complete == 1 &&
           operation->view_complete == 0 && operation->view_source_operand == -1 &&
           operation->view_source_parameter == -1;
}

/* Rebuilt independently of both the TargetPlan collector and its verifier:
 * `T?` is `T | null`, and the language surface requires the nullable primitives
 * to carry `null` in the tagged representation so a null renders as "null" and
 * not as the payload's zero, with the interpreter and the native backend
 * agreeing. The payload admitted here is exactly one machine scalar that cannot
 * hold a reference, so the tagged carrier owes no reference count. A nullable
 * String, object, or container is reference capable and stays refused. */
static bool aot_nullable_scalar_type_is_exact(const XrSemanticTypeRecord *type) {
    const uint8_t allowed = (uint8_t) (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_CONST);
    XrStableId zero = {{0}};
    if (!type || (type->flags & (uint8_t) ~allowed) != 0 || type->builtin_type != XR_TID_NULL ||
        type->child_count != 0 || type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->source_class != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(type->source_class_identity, zero) || type->source_enum_key ||
        type->enum_layout_id != 0 || type->enum_member_count != 0 || type->enum_flags != 0 ||
        type->reserved_enum != 0)
        return false;
    /* The type of the `null` spelling itself is the degenerate member of this
     * shape: its carrier holds the null tag and nothing else, which is the same
     * storage the payload-carrying members use and owes no reference count
     * either. It is what a nullable scalar is initialized from and compared
     * against, so leaving it out would refuse every program that names null. */
    if (type->kind == XR_KIND_NULL)
        return type->scalar_rep == XR_SCALAR_REP_NONE;
    if ((type->flags & XR_SEM_TYPE_NULLABLE) == 0)
        return false;
    /* The authority stops at the three payload spellings whose whole path is
     * proved, `int`, `float` and `bool`. A narrower or unsigned integer, a
     * single-precision float, and a rune each imply a boxing recipe this
     * authority does not state, so they stay refused along with every other
     * payload. */
    switch ((XrTypeKind) type->kind) {
        case XR_KIND_INT:
            return type->scalar_rep == XR_NATIVE_I64;
        case XR_KIND_FLOAT:
            return type->scalar_rep == XR_NATIVE_F64;
        case XR_KIND_BOOL:
            return type->scalar_rep == XR_SCALAR_REP_NONE;
        default:
            return false;
    }
}

/* Rebuilt independently of both the TargetPlan collector and its verifier: a
 * direct-local call may carry an owned String result, whose storage is the
 * outer tagged XrValue. Every other non-scalar result stays fail closed. */
static bool aot_direct_local_string_result_is_exact(const XrSemanticPlan *semantic,
                                                    uint32_t operation_index) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(semantic, operation_index);
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
    uint8_t forbidden = XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE | XR_SEM_TYPE_BORROW_VIEW |
                        XR_SEM_TYPE_AGGREGATE_EXACT;
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    if (!semantic || !operation || !type ||
        (operation->opcode != XI_CALL && operation->opcode != XI_TAIL_CALL) ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->return_complete != 1 || operation->return_provenance != XR_SEM_RETURN_OWNED ||
        type->kind != XR_KIND_STRING || type->child_count != 0 ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->aggregate_extent != 0 ||
        type->aggregate_align != 0 || (type->flags & forbidden) != 0 ||
        (type->flags & required) != required)
        return false;
    size_t target_count = xr_semantic_plan_call_target_count(semantic);
    const XrSemanticFunctionRecord *callee = NULL;
    for (size_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(semantic, i);
        if (!target || target->operation != operation_index ||
            target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL)
            continue;
        if (callee)
            return false;
        callee = xr_semantic_plan_function(semantic, target->function);
        if (!callee)
            return false;
    }
    return callee && callee->return_type == operation->result_type &&
           callee->return_parameter == -1 && callee->return_provenance == XR_SEM_RETURN_OWNED;
}

/* The reserved JSON class global is a compiler-owned namespace handle: it is
 * loaded from the runtime global table and never adapted to a native
 * representation, so its definition storage stays tagged.  Every other builtin
 * global remains without representation authority. */
static bool aot_json_namespace_global_is_exact(const XrSemanticPlan *semantic,
                                               const XrSemanticOperationRecord *operation) {
    char expected_type_key[160];
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
    int written =
        snprintf(expected_type_key, sizeof(expected_type_key),
                 "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;named:4:JSON[0]", (unsigned) XR_KIND_CLASS,
                 (unsigned) XR_TID_NULL, (unsigned) XR_SCALAR_REP_NONE);
    XrStableId zero = {{0}};
    return semantic && operation && type && written > 0 &&
           (size_t) written < sizeof(expected_type_key) && operation->opcode == XI_GET_BUILTIN &&
           operation->operand_count == 0 && operation->metadata_count == 1 &&
           operation->metadata_begin < metadata_count && metadata &&
           strcmp(metadata[operation->metadata_begin], "JSON") == 0 &&
           operation->auxiliary_kind == XI_AUX_KIND_NONE &&
           operation->semantic_immediate == XR_GLOBAL_VAR_JSON &&
           operation->constant == XR_SEMANTIC_INDEX_NONE &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
           operation->effects == xi_generated_op_effects(XI_GET_BUILTIN) &&
           operation->flags == xi_generated_op_default_flags(XI_GET_BUILTIN) &&
           operation->result_alias_operand == -1 && type->kind == XR_KIND_CLASS &&
           type->builtin_type == XR_TID_NULL && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) && type->canonical_key &&
           strcmp(type->canonical_key, expected_type_key) == 0;
}

static bool aot_json_namespace_value_is_exact(const XrSemanticPlan *semantic,
                                              const XrSemanticOperationRecord *operation,
                                              uint32_t *argument_value) {
    uint32_t operand_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    if (!semantic || !operation ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_JSON_NAMESPACE_VALUE ||
        operation->opcode != XI_CALL_METHOD || operation->operand_count != 2 ||
        operation->operand_begin + 1u >= operand_count || operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count || !metadata ||
        strcmp(metadata[operation->metadata_begin], "value") != 0 ||
        operation->result_alias_operand != -1 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *argument = receiver + 1;
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(semantic, operation->result_type);
    uint32_t receiver_definition = XR_SEMANTIC_INDEX_NONE;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->result_value != receiver->value)
            continue;
        if (receiver_definition != XR_SEMANTIC_INDEX_NONE)
            return false;
        receiver_definition = i;
    }
    if (!result_type || result_type->kind != XR_KIND_JSON ||
        result_type->builtin_type != XR_TID_NULL || result_type->child_count != 0 ||
        result_type->scalar_rep != XR_SCALAR_REP_NONE ||
        receiver->role != XR_SEM_OPERAND_RECEIVER ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        argument->role != XR_SEM_OPERAND_ARGUMENT ||
        argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver_definition == XR_SEMANTIC_INDEX_NONE ||
        !aot_json_namespace_global_is_exact(
            semantic, xr_semantic_plan_operation(semantic, receiver_definition)))
        return false;
    if (argument_value)
        *argument_value = argument->value;
    return true;
}

static bool aot_stringbuilder_constructor_is_exact(const XrSemanticPlan *semantic,
                                                   const XrSemanticOperationRecord *operation) {
    char expected_type_key[160];
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
    int written = snprintf(expected_type_key, sizeof(expected_type_key),
                           "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;named:13:StringBuilder[0]",
                           (unsigned) XR_KIND_INSTANCE, (unsigned) XR_TID_STRINGBUILDER,
                           (unsigned) XR_SCALAR_REP_NONE);
    return semantic && operation && type && written > 0 &&
           (size_t) written < sizeof(expected_type_key) && operation->opcode == XI_CALL_BUILTIN &&
           operation->operand_count == 0 && operation->metadata_count == 1 &&
           operation->metadata_begin < metadata_count && metadata &&
           strcmp(metadata[operation->metadata_begin], "StringBuilder") == 0 &&
           operation->auxiliary_kind == XI_AUX_KIND_NONE && operation->semantic_immediate == 0 &&
           operation->constant == XR_SEMANTIC_INDEX_NONE &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
           operation->effects == xi_generated_op_effects(XI_CALL_BUILTIN) &&
           operation->flags == xi_generated_op_default_flags(XI_CALL_BUILTIN) &&
           operation->ownership_use == xi_generated_op_own_use(XI_CALL_BUILTIN) &&
           operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
           operation->transfer_mode == XR_TRANSFER_SHARE &&
           operation->parameter_mode == XR_PARAM_READ &&
           operation->parameter_ownership == XI_OWN_NONE && operation->result_alias_operand == -1 &&
           operation->return_provenance == XR_SEM_RETURN_OWNED &&
           operation->return_parameter == -1 && operation->return_complete == 1 &&
           xr_semantic_allocation_identity_is_canonical(operation) &&
           type->kind == XR_KIND_INSTANCE && type->builtin_type == XR_TID_STRINGBUILDER &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           type->canonical_key && strcmp(type->canonical_key, expected_type_key) == 0;
}

static bool aot_array_intrinsic_storage_is_exact(const XrSemanticTypeRecord *element,
                                                 uint8_t semantic_storage,
                                                 uint8_t *target_storage) {
    uint8_t expected = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!element || !target_storage || element->child_count != 0 ||
        element->aggregate_extent != 0 || element->aggregate_align != 0 || element->flags != 0)
        return false;
    switch (element->kind) {
        case XR_KIND_INT:
            switch (element->scalar_rep) {
                case XR_NATIVE_I8:
                    expected = XR_TARGET_ARRAY_STORAGE_I8;
                    break;
                case XR_NATIVE_U8:
                    expected = XR_TARGET_ARRAY_STORAGE_U8;
                    break;
                case XR_NATIVE_I16:
                    expected = XR_TARGET_ARRAY_STORAGE_I16;
                    break;
                case XR_NATIVE_U16:
                    expected = XR_TARGET_ARRAY_STORAGE_U16;
                    break;
                case XR_NATIVE_I32:
                    expected = XR_TARGET_ARRAY_STORAGE_I32;
                    break;
                case XR_NATIVE_U32:
                    expected = XR_TARGET_ARRAY_STORAGE_U32;
                    break;
                case XR_NATIVE_I64:
                    expected = XR_TARGET_ARRAY_STORAGE_I64;
                    break;
                case XR_NATIVE_U64:
                    expected = XR_TARGET_ARRAY_STORAGE_U64;
                    break;
                default:
                    return false;
            }
            break;
        case XR_KIND_FLOAT:
            if (element->scalar_rep == XR_NATIVE_F32)
                expected = XR_TARGET_ARRAY_STORAGE_F32;
            else if (element->scalar_rep == XR_NATIVE_F64)
                expected = XR_TARGET_ARRAY_STORAGE_F64;
            else
                return false;
            break;
        case XR_KIND_BOOL:
            if (element->scalar_rep != XR_SCALAR_REP_NONE)
                return false;
            expected = XR_TARGET_ARRAY_STORAGE_BOOL;
            break;
        case XR_KIND_RUNE:
            if (element->scalar_rep != XR_SCALAR_REP_NONE)
                return false;
            expected = XR_TARGET_ARRAY_STORAGE_RUNE;
            break;
        default:
            return false;
    }
    static const uint8_t semantic_by_target[XR_TARGET_ARRAY_STORAGE_COUNT] = {
        [XR_TARGET_ARRAY_STORAGE_NONE] = XR_ELEM_ANY,
        [XR_TARGET_ARRAY_STORAGE_I8] = XR_ELEM_I8,
        [XR_TARGET_ARRAY_STORAGE_U8] = XR_ELEM_U8,
        [XR_TARGET_ARRAY_STORAGE_I16] = XR_ELEM_I16,
        [XR_TARGET_ARRAY_STORAGE_U16] = XR_ELEM_U16,
        [XR_TARGET_ARRAY_STORAGE_I32] = XR_ELEM_I32,
        [XR_TARGET_ARRAY_STORAGE_U32] = XR_ELEM_U32,
        [XR_TARGET_ARRAY_STORAGE_I64] = XR_ELEM_I64,
        [XR_TARGET_ARRAY_STORAGE_U64] = XR_ELEM_U64,
        [XR_TARGET_ARRAY_STORAGE_F32] = XR_ELEM_F32,
        [XR_TARGET_ARRAY_STORAGE_F64] = XR_ELEM_F64,
        [XR_TARGET_ARRAY_STORAGE_BOOL] = XR_ELEM_BOOL,
        [XR_TARGET_ARRAY_STORAGE_RUNE] = XR_ELEM_RUNE,
    };
    if (semantic_storage != semantic_by_target[expected])
        return false;
    *target_storage = expected;
    return true;
}

static bool aot_array_intrinsic_fill_type_is_exact(const XrSemanticTypeRecord *type,
                                                   uint8_t element_storage) {
    if (!type || type->builtin_type != XR_TID_NULL || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 || type->flags != 0)
        return false;
    if (element_storage == XR_TARGET_ARRAY_STORAGE_RUNE)
        return type->kind == XR_KIND_RUNE && type->scalar_rep == XR_SCALAR_REP_NONE;
    if (element_storage <= XR_TARGET_ARRAY_STORAGE_NONE ||
        element_storage >= XR_TARGET_ARRAY_STORAGE_RUNE)
        return false;
    if (type->kind == XR_KIND_BOOL)
        return type->scalar_rep == XR_SCALAR_REP_NONE;
    if (type->kind != XR_KIND_INT && type->kind != XR_KIND_FLOAT)
        return false;
    switch (type->scalar_rep) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
        case XR_NATIVE_I64:
        case XR_NATIVE_U64:
        case XR_NATIVE_F32:
        case XR_NATIVE_F64:
            return true;
        default:
            return false;
    }
}

/* The one scalar storage identity an Array element has, when it has one. A
 * reference-capable element has none: a store into it would leave an element
 * ownership and drop obligation no row here states. */
static bool aot_array_element_storage_is_exact(const XrSemanticTypeRecord *element,
                                               uint8_t *target_storage) {
    if (!element || !target_storage || element->builtin_type != XR_TID_NULL ||
        element->child_count != 0 || element->aggregate_extent != 0 ||
        element->aggregate_align != 0 || element->flags != 0)
        return false;
    switch (element->kind) {
        case XR_KIND_INT:
            switch (element->scalar_rep) {
                case XR_NATIVE_I8:
                    *target_storage = XR_TARGET_ARRAY_STORAGE_I8;
                    return true;
                case XR_NATIVE_U8:
                    *target_storage = XR_TARGET_ARRAY_STORAGE_U8;
                    return true;
                case XR_NATIVE_I16:
                    *target_storage = XR_TARGET_ARRAY_STORAGE_I16;
                    return true;
                case XR_NATIVE_U16:
                    *target_storage = XR_TARGET_ARRAY_STORAGE_U16;
                    return true;
                case XR_NATIVE_I32:
                    *target_storage = XR_TARGET_ARRAY_STORAGE_I32;
                    return true;
                case XR_NATIVE_U32:
                    *target_storage = XR_TARGET_ARRAY_STORAGE_U32;
                    return true;
                case XR_NATIVE_I64:
                    *target_storage = XR_TARGET_ARRAY_STORAGE_I64;
                    return true;
                case XR_NATIVE_U64:
                    *target_storage = XR_TARGET_ARRAY_STORAGE_U64;
                    return true;
                default:
                    return false;
            }
        case XR_KIND_FLOAT:
            if (element->scalar_rep == XR_NATIVE_F32) {
                *target_storage = XR_TARGET_ARRAY_STORAGE_F32;
                return true;
            }
            if (element->scalar_rep == XR_NATIVE_F64) {
                *target_storage = XR_TARGET_ARRAY_STORAGE_F64;
                return true;
            }
            return false;
        case XR_KIND_BOOL:
            if (element->scalar_rep != XR_SCALAR_REP_NONE)
                return false;
            *target_storage = XR_TARGET_ARRAY_STORAGE_BOOL;
            return true;
        case XR_KIND_RUNE:
            if (element->scalar_rep != XR_SCALAR_REP_NONE)
                return false;
            *target_storage = XR_TARGET_ARRAY_STORAGE_RUNE;
            return true;
        default:
            return false;
    }
}

/* An exact `Array<T>`, and the element storage the asking carrier is entitled
 * to know. A carrier that holds the tagged outer value never reaches an
 * element, so `Array<String>` is as carryable as `Array<i64>`; a carrier that
 * indexes or rewrites elements needs one scalar element storage identity.
 * `indexes_elements` is which of the two is asking, and `storage` is the
 * element's scalar storage when the element has one and NONE otherwise. */
static bool aot_array_type_is_exact(const XrSemanticPlan *semantic, uint32_t type_index,
                                    bool indexes_elements, uint8_t *target_storage) {
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    const XrSemanticTypeRecord *array = xr_semantic_plan_type(semantic, type_index);
    uint8_t element = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!children || !array || array->kind != XR_KIND_ARRAY || array->builtin_type != XR_TID_NULL ||
        array->child_count != 1 || array->child_begin >= child_count ||
        array->scalar_rep != XR_SCALAR_REP_NONE || array->aggregate_extent != 0 ||
        array->aggregate_align != 0 ||
        array->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    if (!aot_array_element_storage_is_exact(
            xr_semantic_plan_type(semantic, children[array->child_begin]), &element)) {
        if (indexes_elements)
            return false;
        element = XR_TARGET_ARRAY_STORAGE_NONE;
    }
    if (target_storage)
        *target_storage = element;
    return true;
}

/* A borrowed `Array<T>` parameter in whichever passing mode was declared. Both
 * modes borrow the caller's allocation and release nothing; they differ only in
 * whether the callee is handed a pointer to the caller's cell (ref, so it may
 * rebind) or the tagged value itself. One judgement with the mode as its
 * parameter, so the two cannot drift apart. */
static bool aot_array_parameter_is_exact(const XrSemanticPlan *semantic,
                                         const XrSemanticParameterRecord *parameter, uint8_t mode,
                                         uint8_t *storage) {
    return parameter && parameter->function < xr_semantic_plan_function_count(semantic) &&
           parameter->value != XR_SEMANTIC_INDEX_NONE && parameter->mode == mode &&
           parameter->ownership == XI_OWN_BORROWED &&
           parameter->transfer_mode == XR_TRANSFER_SHARE &&
           (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) == 0 && parameter->reserved == 0 &&
           aot_array_type_is_exact(semantic, parameter->type, mode == XR_PARAM_REF, storage);
}

static bool aot_array_ref_parameter_is_exact(const XrSemanticPlan *semantic,
                                             const XrSemanticParameterRecord *parameter,
                                             uint8_t *storage) {
    return aot_array_parameter_is_exact(semantic, parameter, XR_PARAM_REF, storage);
}

static bool aot_array_value_parameter_is_exact(const XrSemanticPlan *semantic,
                                               const XrSemanticParameterRecord *parameter,
                                               uint8_t *storage) {
    return aot_array_parameter_is_exact(semantic, parameter, XR_PARAM_READ, storage);
}

/* A direct-local call that hands back a freshly owned `Array<T>`. The container
 * is a dynamic value, so the result is a transfer of the outer tagged value,
 * exactly as an owned String result is. */
static bool aot_direct_local_array_result_is_exact(const XrSemanticPlan *semantic,
                                                   uint32_t operation_index) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(semantic, operation_index);
    if (!semantic || !operation ||
        (operation->opcode != XI_CALL && operation->opcode != XI_TAIL_CALL) ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->return_complete != 1 || operation->return_provenance != XR_SEM_RETURN_OWNED ||
        !aot_array_type_is_exact(semantic, operation->result_type, false, NULL))
        return false;
    size_t target_count = xr_semantic_plan_call_target_count(semantic);
    const XrSemanticFunctionRecord *callee = NULL;
    for (size_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(semantic, i);
        if (!target || target->operation != operation_index ||
            target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL)
            continue;
        if (callee)
            return false;
        callee = xr_semantic_plan_function(semantic, target->function);
        if (!callee)
            return false;
    }
    return callee && callee->return_type == operation->result_type &&
           callee->return_parameter == -1 && callee->return_provenance == XR_SEM_RETURN_OWNED;
}

/* The borrowed read of an Array held in a shared cell. The read hands over the
 * tagged outer value and reaches no element, so it demands nothing of the
 * element type; `storage` is still reported, because the layout row this read
 * is checked against records the element storage when the element has one. */
static bool aot_array_shared_value_is_exact(const XrSemanticPlan *semantic,
                                            const XrSemanticOperationRecord *operation,
                                            uint8_t *storage) {
    return xr_semantic_shared_read_operation_is_exact(operation) &&
           aot_array_type_is_exact(semantic, operation->result_type, false, storage);
}

/* Independent reconstruction for ARRAY_INTRINSIC.  Only frozen semantic and
 * target rows participate; Xi selector spellings, result-type guesses and the
 * legacy packed immediate are deliberately unavailable here. */
static bool aot_array_intrinsic_is_exact(const XrSemanticPlan *semantic,
                                         const XrSemanticOperationRecord *operation,
                                         uint8_t *target_kind, uint8_t *target_storage) {
    uint32_t operand_count = 0, child_count = 0;
    const XrSemanticOperandRecord *operands =
        semantic ? xr_semantic_plan_operands(semantic, &operand_count) : NULL;
    const uint32_t *children =
        semantic ? xr_semantic_plan_type_children(semantic, &child_count) : NULL;
    bool with_capacity =
        operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_WITH_CAPACITY;
    bool filled = operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_FILLED_NEW;
    uint16_t expected_operands = with_capacity ? 1u : 2u;
    if (!semantic || !operation || (!with_capacity && !filled) || !operands || !children ||
        operation->opcode != XI_CALL_BUILTIN || operation->operand_count != expected_operands ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 0 || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->semantic_immediate != 0 || operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_BUILTIN) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_BUILTIN) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_BUILTIN) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1 || !xr_semantic_allocation_identity_is_canonical(operation))
        return false;
    const XrSemanticTypeRecord *array = xr_semantic_plan_type(semantic, operation->result_type);
    if (!array || array->kind != XR_KIND_ARRAY || array->builtin_type != XR_TID_NULL ||
        array->child_count != 1 || array->child_begin >= child_count ||
        array->scalar_rep != XR_SCALAR_REP_NONE || array->aggregate_extent != 0 ||
        array->aggregate_align != 0 ||
        array->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    const XrSemanticTypeRecord *element =
        xr_semantic_plan_type(semantic, children[array->child_begin]);
    const XrSemanticOperandRecord *count = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *count_type = xr_semantic_plan_type(semantic, count->type);
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!count_type || !semantic_exact_i64_type(count_type) ||
        count->role != XR_SEM_OPERAND_ARGUMENT || count->parameter != 0 ||
        count->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        count->ownership_action != XR_SEM_OPERAND_CONSUME ||
        !aot_array_intrinsic_storage_is_exact(element, operation->array_element_storage, &storage))
        return false;
    if (filled) {
        const XrSemanticOperandRecord *fill = count + 1;
        const XrSemanticTypeRecord *fill_type = xr_semantic_plan_type(semantic, fill->type);
        if (!aot_array_intrinsic_fill_type_is_exact(fill_type, storage) ||
            fill->role != XR_SEM_OPERAND_ARGUMENT || fill->parameter != 1 ||
            fill->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
            fill->ownership_action != XR_SEM_OPERAND_CONSUME)
            return false;
    }
    if (target_kind)
        *target_kind = with_capacity ? XR_TARGET_ARRAY_INTRINSIC_WITH_CAPACITY
                                     : XR_TARGET_ARRAY_INTRINSIC_FILLED_NEW;
    if (target_storage)
        *target_storage = storage;
    return true;
}

/* Dedicated Array.fill identity reconstructed solely from frozen SemanticPlan
 * rows. The metadata payload is intentionally not read. */
static bool aot_array_fill_scalar_is_exact(const XrSemanticPlan *semantic,
                                           const XrSemanticOperationRecord *operation,
                                           uint32_t *receiver_value, uint32_t *fill_value,
                                           uint8_t *target_storage) {
    uint32_t operand_count = 0, child_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands =
        semantic ? xr_semantic_plan_operands(semantic, &operand_count) : NULL;
    const uint32_t *children =
        semantic ? xr_semantic_plan_type_children(semantic, &child_count) : NULL;
    if (semantic)
        (void) xr_semantic_plan_metadata(semantic, &metadata_count);
    if (!semantic || !operation || !operands || !children ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_ARRAY_FILL_SCALAR ||
        operation->opcode != XI_CALL_METHOD || operation->operand_count != 2 ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        operation->semantic_immediate != 0 || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->result_alias_operand != 0 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *fill = receiver + 1;
    const XrSemanticTypeRecord *array = xr_semantic_plan_type(semantic, receiver->type);
    if (!array || array->kind != XR_KIND_ARRAY || array->builtin_type != XR_TID_NULL ||
        array->child_count != 1 || array->child_begin >= child_count ||
        array->scalar_rep != XR_SCALAR_REP_NONE || array->aggregate_extent != 0 ||
        array->aggregate_align != 0 ||
        array->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    uint32_t element_index = children[array->child_begin];
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(semantic, element_index);
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!element || operation->result_type != receiver->type || fill->type != element_index ||
        !aot_array_intrinsic_storage_is_exact(element, operation->array_element_storage,
                                              &storage) ||
        receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        fill->role != XR_SEM_OPERAND_ARGUMENT || fill->parameter != 0 ||
        fill->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        fill->ownership_action != XR_SEM_OPERAND_CONSUME)
        return false;
    if (receiver_value)
        *receiver_value = receiver->value;
    if (fill_value)
        *fill_value = fill->value;
    if (target_storage)
        *target_storage = storage;
    return true;
}

static bool aot_pair_identity(const char *domain, XrStableId first, XrStableId second,
                              uint32_t ordinal, XrStableId *out) {
    char first_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char second_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    XrFingerprint digest;
    xr_stable_id_hex(first, first_hex);
    xr_stable_id_hex(second, second_hex);
    int written = snprintf(key, sizeof(key), "%s:first=%s:second=%s:ordinal=%u", domain, first_hex,
                           second_hex, ordinal);
    return out && written > 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

static bool aot_stable_id_is_zero(XrStableId id) {
    for (uint32_t i = 0; i < XR_STABLE_ID_BYTES; i++)
        if (id.bytes[i] != 0)
            return false;
    return true;
}

typedef struct AotArrayHofAuthority {
    uint32_t operation;
    uint32_t receiver;
    uint32_t callback;
    uint32_t initial;
    uint8_t kind;
    uint8_t source_storage;
    uint8_t result_storage;
} AotArrayHofAuthority;

static bool aot_array_hof_array_type_is_exact(const XrSemanticPlan *semantic, uint32_t type_index,
                                              uint8_t frozen_storage, uint32_t *out_element,
                                              uint8_t *out_target_storage) {
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    const XrSemanticTypeRecord *array = xr_semantic_plan_type(semantic, type_index);
    if (!children || !array || array->kind != XR_KIND_ARRAY || array->builtin_type != XR_TID_NULL ||
        array->child_count != 1 || array->child_begin >= child_count ||
        array->scalar_rep != XR_SCALAR_REP_NONE || array->aggregate_extent != 0 ||
        array->aggregate_align != 0 ||
        array->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    uint32_t element_index = children[array->child_begin];
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(semantic, element_index);
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!aot_array_intrinsic_storage_is_exact(element, frozen_storage, &storage))
        return false;
    if (out_element)
        *out_element = element_index;
    if (out_target_storage)
        *out_target_storage = storage;
    return true;
}

/* Reconstruct the closed Array HOF family from frozen plan rows.  The
 * operation/value/use indexes make callback ownership and identity constant
 * time; neither selector metadata nor live Xi arity participates. */
static bool aot_array_hof_semantic_is_exact(const VerifyAuthority *ctx, uint32_t operation_index,
                                            AotArrayHofAuthority *out) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(ctx ? ctx->semantic : NULL, operation_index);
    uint32_t operand_count = 0, child_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx ? ctx->semantic : NULL, &operand_count);
    const uint32_t *children =
        xr_semantic_plan_type_children(ctx ? ctx->semantic : NULL, &child_count);
    if (ctx)
        (void) xr_semantic_plan_metadata(ctx->semantic, &metadata_count);
    uint8_t kind = XR_TARGET_ARRAY_HOF_NONE;
    if (operation) {
        switch (operation->array_hof_kind) {
            case XR_SEM_ARRAY_HOF_MAP:
                kind = XR_TARGET_ARRAY_HOF_MAP;
                break;
            case XR_SEM_ARRAY_HOF_FILTER:
                kind = XR_TARGET_ARRAY_HOF_FILTER;
                break;
            case XR_SEM_ARRAY_HOF_REDUCE:
                kind = XR_TARGET_ARRAY_HOF_REDUCE;
                break;
            default:
                break;
        }
    }
    uint16_t expected_operands = kind == XR_TARGET_ARRAY_HOF_REDUCE ? 3u : 2u;
    if (!ctx || !operation || !operands || !children ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_ARRAY_HOF ||
        kind == XR_TARGET_ARRAY_HOF_NONE || operation->opcode != XI_CALL_METHOD ||
        operation->operand_count != expected_operands || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        operation->semantic_immediate != 0 || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function >= ctx->function_count ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1)
        return false;
    const XrSemanticOperandRecord *rows = &operands[operation->operand_begin];
    uint32_t source_element = XR_SEMANTIC_INDEX_NONE;
    uint8_t source_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!aot_array_hof_array_type_is_exact(ctx->semantic, rows[0].type,
                                           operation->array_element_storage, &source_element,
                                           &source_storage))
        return false;
    uint32_t result_element = operation->result_type;
    uint8_t result_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (kind != XR_TARGET_ARRAY_HOF_REDUCE) {
        if (!aot_array_hof_array_type_is_exact(ctx->semantic, operation->result_type,
                                               operation->array_result_element_storage,
                                               &result_element, &result_storage) ||
            (kind == XR_TARGET_ARRAY_HOF_FILTER &&
             (operation->result_type != rows[0].type || result_element != source_element)))
            return false;
    } else {
        const XrSemanticTypeRecord *result_type =
            xr_semantic_plan_type(ctx->semantic, result_element);
        if (rows[2].type != operation->result_type ||
            !aot_array_intrinsic_storage_is_exact(
                result_type, operation->array_result_element_storage, &result_storage))
            return false;
    }
    const XrSemanticFunctionRecord *callee =
        xr_semantic_plan_function(ctx->semantic, operation->callable_function);
    uint16_t expected_parameters = kind == XR_TARGET_ARRAY_HOF_REDUCE ? 2u : 1u;
    if (!callee || callee->parent != operation->function || callee->capture_count != 0 ||
        callee->parameter_count != expected_parameters ||
        (callee->semantic_effects & (XI_EFFECT_SIDE_EFFECT | XI_EFFECT_MEMORY_WRITE |
                                     XI_EFFECT_MAY_THROW | XI_EFFECT_MAY_SUSPEND)) != 0 ||
        callee->parameter_begin > ctx->parameter_count ||
        callee->parameter_count > ctx->parameter_count - callee->parameter_begin)
        return false;
    const XrSemanticParameterRecord *first =
        xr_semantic_plan_parameter(ctx->semantic, callee->parameter_begin);
    const XrSemanticParameterRecord *second =
        expected_parameters == 2u
            ? xr_semantic_plan_parameter(ctx->semantic, callee->parameter_begin + 1u)
            : NULL;
    if (!first || first->function != operation->callable_function || first->ordinal != 0 ||
        first->type != (kind == XR_TARGET_ARRAY_HOF_REDUCE ? result_element : source_element) ||
        (second && (second->function != operation->callable_function || second->ordinal != 1 ||
                    second->type != source_element)))
        return false;
    if (kind == XR_TARGET_ARRAY_HOF_FILTER) {
        const XrSemanticTypeRecord *return_type =
            xr_semantic_plan_type(ctx->semantic, callee->return_type);
        if (!return_type || return_type->kind != XR_KIND_BOOL ||
            return_type->builtin_type != XR_TID_NULL ||
            return_type->scalar_rep != XR_SCALAR_REP_NONE || return_type->child_count != 0 ||
            return_type->aggregate_extent != 0 || return_type->aggregate_align != 0 ||
            return_type->flags != 0)
            return false;
    } else if (callee->return_type != result_element) {
        return false;
    }
    const XrSemanticTypeRecord *callback_type = xr_semantic_plan_type(ctx->semantic, rows[1].type);
    if (!callback_type || callback_type->kind != XR_KIND_FUNCTION ||
        callback_type->child_count != (uint32_t) expected_parameters + 1u ||
        callback_type->child_begin > child_count ||
        callback_type->child_count > child_count - callback_type->child_begin)
        return false;
    for (uint16_t i = 0; i < expected_parameters; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(ctx->semantic, callee->parameter_begin + i);
        if (!parameter || children[callback_type->child_begin + i] != parameter->type)
            return false;
    }
    if (children[callback_type->child_begin + expected_parameters] != callee->return_type ||
        rows[0].role != XR_SEM_OPERAND_RECEIVER || rows[0].parameter != -1 ||
        rows[0].flags != XR_SEM_OPERAND_CALL_CONTRACT || rows[1].role != XR_SEM_OPERAND_ARGUMENT ||
        rows[1].parameter != 0 || rows[1].flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        (kind == XR_TARGET_ARRAY_HOF_REDUCE &&
         (rows[2].role != XR_SEM_OPERAND_ARGUMENT || rows[2].parameter != 1 ||
          rows[2].flags != XR_SEM_OPERAND_CALL_CONTRACT)))
        return false;
    bool result_exact = kind == XR_TARGET_ARRAY_HOF_REDUCE
                            ? operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT &&
                                  operation->return_provenance == XR_SEM_RETURN_NONE &&
                                  operation->return_complete == 0
                            : operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
                                  operation->return_provenance == XR_SEM_RETURN_OWNED &&
                                  operation->return_complete == 1;
    uint32_t producer_index = rows[1].value < ctx->value_count
                                  ? ctx->operation_by_value[rows[1].value]
                                  : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *producer =
        producer_index < ctx->operation_count
            ? xr_semantic_plan_operation(ctx->semantic, producer_index)
            : NULL;
    if (!result_exact || !producer || producer_index >= operation_index ||
        ctx->use_count_by_value[rows[1].value] != 1 ||
        !semantic_heap_closure_is_exact(ctx->semantic, producer) ||
        producer->callable_function != operation->callable_function ||
        producer->result_type != rows[1].type)
        return false;
    if (out) {
        *out = (AotArrayHofAuthority) {
            .operation = operation_index,
            .receiver = rows[0].value,
            .callback = rows[1].value,
            .initial = kind == XR_TARGET_ARRAY_HOF_REDUCE ? rows[2].value : XR_SEMANTIC_INDEX_NONE,
            .kind = kind,
            .source_storage = source_storage,
            .result_storage = result_storage,
        };
    }
    return true;
}

/* Reconstruct the Array.reserve operand contract from immutable plan rows.
 * The intrinsic evidence selects the semantic operation and the unique Target
 * call binds its receiver and capacity identities. No live selector, type, or
 * arity participates in representation refinement. */
static bool aot_array_reserve_use_is_exact(const VerifyAuthority *ctx, uint32_t operation_index,
                                           uint32_t *receiver_value, uint32_t *capacity_value) {
    const XrSemanticOperationRecord *operation =
        ctx ? xr_semantic_plan_operation(ctx->semantic, operation_index) : NULL;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        ctx ? xr_semantic_plan_operands(ctx->semantic, &operand_count) : NULL;
    if (!ctx || !operation || !operands ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_ARRAY_MEMBER_SCALAR ||
        operation->evidence[1] != XA_INTRINSIC_ARRAY_RESERVE ||
        operation->opcode != XI_CALL_BUILTIN || operation->operand_count != 2 ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 0 || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->semantic_immediate != 0 ||
        operation->effects != xi_generated_op_effects(XI_CALL_BUILTIN))
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *capacity = receiver + 1;
    const XrSemanticTypeRecord *receiver_type =
        xr_semantic_plan_type(ctx->semantic, receiver->type);
    const XrSemanticTypeRecord *capacity_type =
        xr_semantic_plan_type(ctx->semantic, capacity->type);
    if (!receiver_type || receiver_type->kind != XR_KIND_ARRAY ||
        receiver_type->builtin_type != XR_TID_NULL || receiver_type->child_count != 1 ||
        receiver_type->scalar_rep != XR_SCALAR_REP_NONE || receiver_type->aggregate_extent != 0 ||
        receiver_type->aggregate_align != 0 ||
        receiver_type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        !semantic_exact_i64_type(capacity_type) || operation->result_type != receiver->type ||
        operation->result_alias_operand != 0 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1 || receiver->role != XR_SEM_OPERAND_ARGUMENT ||
        receiver->parameter != 0 || receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        capacity->role != XR_SEM_OPERAND_ARGUMENT || capacity->parameter != 1 ||
        capacity->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        capacity->ownership_action != XR_SEM_OPERAND_CONSUME)
        return false;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target_plan, &call_count);
    const XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (call)
            return false;
        call = &calls[i];
    }
    XrStableId expected_call;
    if (!call ||
        !aot_pair_identity("xray-target-array-member-scalar-v1", operation->id, receiver_type->id,
                           capacity->value, &expected_call) ||
        !xr_stable_id_equal(call->identity, expected_call) ||
        call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        call->caller_function != operation->function ||
        call->callee_function != XR_SEMANTIC_INDEX_NONE ||
        call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        call->source_export != XR_SEMANTIC_INDEX_NONE ||
        !aot_stable_id_is_zero(call->source_export_identity) ||
        !aot_stable_id_is_zero(call->source_callee_identity) ||
        call->result_value != operation->result_value || call->argument_count != 0 ||
        call->adapter_count != 0 || call->flags != 0 ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_ARRAY_MEMBER_SCALAR ||
        call->target_kind != XR_TARGET_CALL_TARGET_ARRAY_MEMBER_SCALAR ||
        call->result_mode != XR_TARGET_CALL_VALUE || call->result_ownership != XR_TARGET_CALL_NONE)
        return false;
    if (receiver_value)
        *receiver_value = receiver->value;
    if (capacity_value)
        *capacity_value = capacity->value;
    return true;
}

static bool aot_channel_type_is_exact(const XrSemanticPlan *semantic, uint32_t type_index,
                                      uint32_t *element_type) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, type_index);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    uint8_t allowed = required | XR_SEM_TYPE_CONST;
    if (!semantic || !type || type->kind != XR_KIND_CHANNEL ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count != 1 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        (type->flags & required) != required || (type->flags & ~allowed) != 0 ||
        type->child_begin >= child_count ||
        children[type->child_begin] >= xr_semantic_plan_type_count(semantic))
        return false;
    if (element_type)
        *element_type = children[type->child_begin];
    return true;
}

static bool aot_channel_capacity_type_is_exact(VerifyAuthority *ctx, uint32_t type_index) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(ctx->semantic, type_index);
    if (!type || type->kind != XR_KIND_INT || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 || type->flags != 0)
        return false;
    switch (type->scalar_rep) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
        case XR_NATIVE_I64:
        case XR_NATIVE_U64:
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
            return true;
        default:
            return false;
    }
}

static bool aot_channel_allocation_is_exact(VerifyAuthority *ctx,
                                            const XrSemanticOperationRecord *operation) {
    if (!ctx || !operation || operation->opcode != XI_CHAN_NEW ||
        operation->result_value >= ctx->value_count || operation->operand_count != 1 ||
        !xr_semantic_allocation_identity_is_canonical(operation) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE || operation->auxiliary_kind != 0 ||
        operation->effects != xi_generated_op_effects(XI_CHAN_NEW) ||
        operation->flags != xi_generated_op_default_flags(XI_CHAN_NEW) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_CHAN_NEW) ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    uint32_t element_type = XR_SEMANTIC_INDEX_NONE;
    if (operation->operand_begin >= operand_count ||
        !aot_channel_type_is_exact(ctx->semantic, operation->result_type, &element_type))
        return false;
    const XrSemanticOperandRecord *capacity = &operands[operation->operand_begin];
    return capacity->value < ctx->value_count && capacity->parameter == -1 &&
           capacity->role == XR_SEM_OPERAND_VALUE && capacity->flags == 0 &&
           aot_channel_capacity_type_is_exact(ctx, capacity->type) &&
           element_type < xr_semantic_plan_type_count(ctx->semantic);
}

static bool aot_channel_identity_copy_is_exact(VerifyAuthority *ctx,
                                               const XrSemanticOperationRecord *operation) {
    if (!ctx || !operation || !ctx->exact_channel_value || operation->opcode != XI_COPY ||
        operation->operand_count != 1 || operation->semantic_immediate != XI_COPY_KIND_IDENTITY ||
        operation->allocation_key || !aot_stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->effects != xi_generated_op_effects(XI_COPY) ||
        operation->flags != xi_generated_op_default_flags(XI_COPY) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_alias_operand != 0 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    if (operation->operand_begin >= operand_count)
        return false;
    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    uint32_t source_element = XR_SEMANTIC_INDEX_NONE;
    uint32_t result_element = XR_SEMANTIC_INDEX_NONE;
    return source->value < ctx->value_count && ctx->exact_channel_value[source->value] &&
           source->parameter == -1 && source->role == XR_SEM_OPERAND_VALUE && source->flags == 0 &&
           aot_channel_type_is_exact(ctx->semantic, source->type, &source_element) &&
           aot_channel_type_is_exact(ctx->semantic, operation->result_type, &result_element) &&
           source_element == result_element;
}

static bool aot_index_channel_values(VerifyAuthority *ctx) {
    if (!verify_alloc(ctx, ctx->value_count, sizeof(uint8_t),
                      (void **) &ctx->exact_channel_value) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(uint8_t),
                      (void **) &ctx->exact_channel_allocation_value))
        return false;
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(ctx->semantic, i);
        if (!operation || operation->result_value >= ctx->value_count)
            return false;
        bool allocation = aot_channel_allocation_is_exact(ctx, operation);
        bool alias = aot_channel_identity_copy_is_exact(ctx, operation);
        if (operation->opcode == XI_CHAN_NEW && !allocation) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_TYPE, i, operation->result_value, i);
            return false;
        }
        if (allocation) {
            ctx->exact_channel_value[operation->result_value] = 1;
            ctx->exact_channel_allocation_value[operation->result_value] = 1;
        } else if (alias) {
            ctx->exact_channel_value[operation->result_value] = 1;
        }
    }
    return true;
}

static bool aot_direct_local_callee_type_is_exact(const XrSemanticPlan *semantic,
                                                  const XrSemanticOperationRecord *operation,
                                                  uint32_t target_function) {
    if (!semantic || !operation || operation->opcode != XI_GET_SHARED ||
        operation->semantic_immediate < 0 || operation->semantic_immediate > UINT16_MAX ||
        operation->operand_count != 0 || operation->allocation_key ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_GET_SHARED) ||
        operation->effects != xi_generated_op_effects(XI_GET_SHARED) ||
        operation->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        operation->return_complete != 1 || operation->return_parameter != -1 ||
        target_function >= xr_semantic_plan_function_count(semantic))
        return false;
    for (uint32_t i = 0; i < XR_STABLE_ID_BYTES; i++)
        if (operation->allocation_id.bytes[i] != 0)
            return false;
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, operation->result_type);
    const XrSemanticFunctionRecord *target = xr_semantic_plan_function(semantic, target_function);
    uint32_t lexical_owner = target ? target->parent : XR_SEMANTIC_INDEX_NONE;
    uint32_t caller_ancestor = operation->function;
    for (uint32_t depth = 0;
         caller_ancestor != XR_SEMANTIC_INDEX_NONE && caller_ancestor != lexical_owner &&
         depth < xr_semantic_plan_function_count(semantic);
         depth++) {
        const XrSemanticFunctionRecord *ancestor =
            xr_semantic_plan_function(semantic, caller_ancestor);
        caller_ancestor = ancestor ? ancestor->parent : XR_SEMANTIC_INDEX_NONE;
    }
    if (!type || !target || lexical_owner == XR_SEMANTIC_INDEX_NONE ||
        caller_ancestor != lexical_owner ||
        (type->kind != XR_KIND_FUNCTION && type->kind != XR_KIND_UNKNOWN) ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->aggregate_extent != 0 ||
        type->aggregate_align != 0 || type->child_count != 0 ||
        target->parameter_begin > xr_semantic_plan_parameter_count(semantic) ||
        target->parameter_count >
            xr_semantic_plan_parameter_count(semantic) - target->parameter_begin ||
        (type->flags & (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE | XR_SEM_TYPE_BORROW_VIEW |
                        XR_SEM_TYPE_AGGREGATE_EXACT)) != 0 ||
        (type->flags & (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT)) !=
            (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    return true;
}

static bool aot_index_direct_local_callee_values(VerifyAuthority *ctx) {
    uint32_t *target_by_operation = NULL;
    uint32_t *use_count = NULL;
    uint8_t *invalid = NULL;
    if (!verify_alloc(ctx, ctx->value_count, sizeof(uint32_t),
                      (void **) &ctx->direct_callee_target_by_value) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(uint8_t),
                      (void **) &ctx->exact_direct_callee_value) ||
        !verify_alloc(ctx, ctx->operation_count, sizeof(uint32_t),
                      (void **) &target_by_operation) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(uint32_t), (void **) &use_count) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(uint8_t), (void **) &invalid)) {
        xr_free(target_by_operation);
        xr_free(use_count);
        xr_free(invalid);
        return false;
    }
    for (uint32_t i = 0; i < ctx->value_count; i++)
        ctx->direct_callee_target_by_value[i] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < ctx->operation_count; i++)
        target_by_operation[i] = XR_SEMANTIC_INDEX_NONE;
    uint32_t target_count = (uint32_t) xr_semantic_plan_call_target_count(ctx->semantic);
    for (uint32_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(ctx->semantic, i);
        if (target && target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL)
            continue;
        if (!target || target->operation >= ctx->operation_count ||
            target_by_operation[target->operation] != XR_SEMANTIC_INDEX_NONE)
            goto invalid_authority;
        target_by_operation[target->operation] = i;
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *use = xr_semantic_plan_operation(ctx->semantic, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin)
            goto invalid_authority;
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand = &operands[use->operand_begin + a];
            if (operand->value >= ctx->value_count)
                goto invalid_authority;
            uint32_t source_index = ctx->operation_by_value[operand->value];
            const XrSemanticOperationRecord *source =
                source_index == XR_SEMANTIC_INDEX_NONE
                    ? NULL
                    : xr_semantic_plan_operation(ctx->semantic, source_index);
            if (!source || source->opcode != XI_GET_SHARED)
                continue;
            uint32_t target_index = target_by_operation[i];
            const XrSemanticCallTargetRecord *target =
                target_index == XR_SEMANTIC_INDEX_NONE
                    ? NULL
                    : xr_semantic_plan_call_target(ctx->semantic, target_index);
            uint32_t value = source->result_value;
            bool exact_use = a == 0 && (use->opcode == XI_CALL || use->opcode == XI_TAIL_CALL) &&
                             use->function == source->function && target &&
                             target->operation == i &&
                             target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
                             operand->role == XR_SEM_OPERAND_CALLEE && operand->parameter == -1 &&
                             (operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0 &&
                             operand->type == source->result_type && value == operand->value;
            if (!exact_use ||
                (ctx->direct_callee_target_by_value[value] != XR_SEMANTIC_INDEX_NONE &&
                 ctx->direct_callee_target_by_value[value] != target->function) ||
                use_count[value] == UINT32_MAX) {
                invalid[value] = 1;
                continue;
            }
            ctx->direct_callee_target_by_value[value] = target->function;
            use_count[value]++;
        }
    }
    for (uint32_t i = 0; i < ctx->block_count; i++) {
        const XrSemanticBlockRecord *block = xr_semantic_plan_block(ctx->semantic, i);
        if (!block || block->control_value == XR_SEMANTIC_INDEX_NONE ||
            block->control_value >= ctx->value_count)
            continue;
        uint32_t source_index = ctx->operation_by_value[block->control_value];
        const XrSemanticOperationRecord *source =
            source_index == XR_SEMANTIC_INDEX_NONE
                ? NULL
                : xr_semantic_plan_operation(ctx->semantic, source_index);
        if (source && source->opcode == XI_GET_SHARED)
            invalid[block->control_value] = 1;
    }
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(ctx->semantic, i);
        if (!operation || operation->opcode != XI_GET_SHARED ||
            operation->result_value >= ctx->value_count ||
            ctx->direct_callee_target_by_value[operation->result_value] == XR_SEMANTIC_INDEX_NONE)
            continue;
        uint32_t value = operation->result_value;
        if (invalid[value] || use_count[value] == 0 ||
            !aot_direct_local_callee_type_is_exact(ctx->semantic, operation,
                                                   ctx->direct_callee_target_by_value[value]))
            goto invalid_authority;
        ctx->exact_direct_callee_value[value] = 1;
    }
    xr_free(target_by_operation);
    xr_free(use_count);
    xr_free(invalid);
    return true;

invalid_authority:
    xr_free(target_by_operation);
    xr_free(use_count);
    xr_free(invalid);
    set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE, (uint32_t) ctx->work,
             0, 0);
    return false;
}

/* Rebuilt here a third time, from the frozen rows alone. A native stdlib
 * namespace receiver is the module-init import reference published into a
 * shared slot: its frozen import classification is resolved against the native
 * definition registry, and its metadata pair names the module path with an
 * empty member. */
static bool aot_native_module_import_is_exact(const XrSemanticPlan *semantic,
                                              const XrSemanticOperationRecord *record,
                                              const char **out_module_path) {
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    const XrSemanticTypeRecord *type =
        record ? xr_semantic_plan_type(semantic, record->result_type) : NULL;
    if (!record || !type || !metadata || record->opcode != XI_IMPORT_REF || record->function != 0 ||
        record->operand_count != 0 || record->metadata_count != 2 ||
        record->metadata_begin + 1u >= metadata_count ||
        record->import_resolution != XR_SEM_IMPORT_RESOLUTION_NATIVE_STDLIB ||
        record->semantic_immediate < -1 || record->semantic_immediate > UINT16_MAX ||
        record->allocation_key || !aot_stable_id_is_zero(record->allocation_id) ||
        record->constant != XR_SEMANTIC_INDEX_NONE ||
        record->callable_function != XR_SEMANTIC_INDEX_NONE || record->auxiliary_kind != 0 ||
        record->effects != xi_generated_op_effects(XI_IMPORT_REF) ||
        record->flags != xi_generated_op_default_flags(XI_IMPORT_REF) ||
        record->ownership_use != xi_generated_op_own_use(XI_IMPORT_REF) ||
        record->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        record->result_alias_operand != -1 ||
        record->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        record->return_parameter != -1 || record->return_complete != 1 ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    const char *module_path = metadata[record->metadata_begin];
    const char *member = metadata[record->metadata_begin + 1u];
    if (!module_path || !member || member[0] != '\0' ||
        !xr_stdlib_metadata_module_known(module_path))
        return false;
    if (out_module_path)
        *out_module_path = module_path;
    return true;
}

static bool aot_native_module_load_is_exact(const XrSemanticPlan *semantic,
                                            const XrSemanticOperationRecord *record) {
    const XrSemanticTypeRecord *type =
        record ? xr_semantic_plan_type(semantic, record->result_type) : NULL;
    return record && type && record->opcode == XI_GET_SHARED && record->operand_count == 0 &&
           record->metadata_count == 0 && record->semantic_immediate >= 0 &&
           record->semantic_immediate <= UINT16_MAX && !record->allocation_key &&
           aot_stable_id_is_zero(record->allocation_id) &&
           record->constant == XR_SEMANTIC_INDEX_NONE &&
           record->callable_function == XR_SEMANTIC_INDEX_NONE && record->auxiliary_kind == 0 &&
           record->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
           record->effects == xi_generated_op_effects(XI_GET_SHARED) &&
           record->flags == xi_generated_op_default_flags(XI_GET_SHARED) &&
           record->ownership_use == xi_generated_op_own_use(XI_GET_SHARED) &&
           record->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
           record->result_alias_operand == -1 &&
           record->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
           record->return_parameter == -1 && record->return_complete == 1 &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT);
}

static const char *aot_native_module_namespace_path(const XrSemanticPlan *semantic,
                                                    uint32_t receiver_value) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    const XrSemanticOperationRecord *load = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->result_value != receiver_value)
            continue;
        if (load)
            return NULL;
        load = candidate;
    }
    if (!aot_native_module_load_is_exact(semantic, load))
        return NULL;
    const XrSemanticOperationRecord *store = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->opcode != XI_SET_SHARED || candidate->function != 0 ||
            candidate->semantic_immediate != load->semantic_immediate)
            continue;
        if (store)
            return NULL;
        store = candidate;
    }
    if (!store || store->operand_count != 1 || store->operand_begin >= operand_count)
        return NULL;
    const XrSemanticOperandRecord *stored = &operands[store->operand_begin];
    if (stored->role != XR_SEM_OPERAND_VALUE || stored->parameter != -1 ||
        stored->ownership_action != XR_SEM_OPERAND_CONSUME || stored->flags != 0 ||
        stored->type != load->result_type)
        return NULL;
    const XrSemanticOperationRecord *import = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->result_value != stored->value)
            continue;
        if (import)
            return NULL;
        import = candidate;
    }
    const char *module_path = NULL;
    return import && import->result_type == load->result_type &&
                   aot_native_module_import_is_exact(semantic, import, &module_path)
               ? module_path
               : NULL;
}

static bool aot_native_module_call_shape_is_exact(const XrSemanticPlan *semantic,
                                                  const XrSemanticOperationRecord *operation,
                                                  const char **out_selector,
                                                  uint32_t *out_receiver_value,
                                                  uint32_t *out_arity) {
    uint32_t operands_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operands_count);
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    if (!semantic || !operation ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NATIVE_MODULE_SCALAR_CALL ||
        operation->opcode != XI_CALL_METHOD || operation->semantic_immediate <= 0 ||
        (operation->semantic_immediate & 1) != 0 || operation->operand_count == 0 ||
        operation->operand_begin >= operands_count ||
        operation->operand_count > operands_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        !metadata || (operation->flags & XI_FLAG_MAY_SUSPEND) != 0 ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        operation->result_alias_operand != -1 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        !xr_semantic_native_module_boundary_type_is_exact(
            xr_semantic_plan_type(semantic, operation->result_type), true))
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    if (receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW)
        return false;
    for (uint16_t i = 1; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *argument = receiver + i;
        if (argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != (int16_t) (i - 1) ||
            argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
            !xr_semantic_native_module_boundary_type_is_exact(
                xr_semantic_plan_type(semantic, argument->type), false))
            return false;
    }
    if (out_selector)
        *out_selector = metadata[operation->metadata_begin];
    if (out_receiver_value)
        *out_receiver_value = receiver->value;
    if (out_arity)
        *out_arity = (uint32_t) (operation->operand_count - 1u);
    return true;
}

/* The refinement oracle independently proves that a yieldable member use owns
 * a frozen SemanticPlan target. Registry lookup is only a reconstruction of
 * that target's tuple; it never substitutes for the stable target identity. */
static bool aot_native_module_yieldable_call_is_exact(const XrSemanticPlan *semantic,
                                                      uint32_t operation_index,
                                                      const XrSemanticOperationRecord *operation,
                                                      const char *module_path,
                                                      uint32_t receiver_value,
                                                      uint32_t receiver_type) {
    uint32_t operand_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    if (!semantic || !operation || !module_path || !module_path[0] || !operands || !metadata ||
        operation->opcode != XI_CALL_METHOD || (operation->semantic_immediate & 1) != 0 ||
        operation->operand_count == 0 || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD))
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    if (receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW || receiver->value != receiver_value ||
        receiver->type != receiver_type)
        return false;
    for (uint16_t i = 1; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *argument = receiver + i;
        if (argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != (int16_t) (i - 1) ||
            argument->flags != XR_SEM_OPERAND_CALL_CONTRACT)
            return false;
    }
    const char *selector = metadata[operation->metadata_begin];
    const XrStdlibDefEntry *binding =
        selector ? xr_stdlib_metadata_unique_func(module_path, selector) : NULL;
    if (!binding || !binding->signature || !binding->vm || !binding->vm_binding ||
        strcmp(binding->vm_binding, "yieldable") != 0 ||
        operation->operand_count != (uint16_t) (binding->argc + 1u))
        return false;
    const XrSemanticCallTargetRecord *target = NULL;
    uint32_t target_count = (uint32_t) xr_semantic_plan_call_target_count(semantic);
    for (uint32_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *candidate = xr_semantic_plan_call_target(semantic, i);
        if (!candidate || candidate->operation != operation_index)
            continue;
        if (target)
            return false;
        target = candidate;
    }
    XrStableId zero = {{0}};
    if (!target || target->kind != XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE ||
        target->function != XR_SEMANTIC_INDEX_NONE ||
        target->dependency != XR_SEMANTIC_INDEX_NONE ||
        target->source_export != XR_SEMANTIC_INDEX_NONE ||
        target->callable_type != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(target->export_identity, zero) ||
        !xr_stable_id_equal(target->callee_function, zero))
        return false;
    char operation_id[XR_STABLE_ID_BYTES * 2 + 1];
    char key[320];
    xr_stable_id_hex(operation->id, operation_id);
    int length = snprintf(
        key, sizeof(key), "call-target-v5:schema=%u:operation=%s:native-namespace=%s.%s:kind=%u",
        XR_SEMANTIC_SCHEMA_VERSION, operation_id, module_path, selector, (unsigned) target->kind);
    XrStableId expected_id;
    XrFingerprint digest;
    return length > 0 && (size_t) length < sizeof(key) && target->canonical_key &&
           strcmp(target->canonical_key, key) == 0 &&
           xr_stable_id_from_key(key, &expected_id, &digest) &&
           xr_stable_id_equal(target->id, expected_id);
}

static bool aot_native_module_namespace_value_is_exact(const XrSemanticPlan *semantic,
                                                       const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    if (!operation)
        return false;
    if (operation->opcode == XI_IMPORT_REF) {
        if (!aot_native_module_import_is_exact(semantic, operation, NULL))
            return false;
    } else if (operation->opcode == XI_GET_SHARED) {
        if (!aot_native_module_load_is_exact(semantic, operation) ||
            !aot_native_module_namespace_path(semantic, operation->result_value))
            return false;
    } else {
        return false;
    }
    bool consumed = false;
    const char *module_path = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *use = xr_semantic_plan_operation(semantic, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin)
            return false;
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand = &operands[use->operand_begin + a];
            if (operand->value != operation->result_value)
                continue;
            if ((use->opcode == XI_RETAIN || use->opcode == XI_RELEASE ||
                 use->opcode == XI_SET_SHARED) &&
                a == 0)
                continue;
            const char *selector = NULL;
            uint32_t receiver_value = XR_SEMANTIC_INDEX_NONE;
            uint32_t arity = 0;
            if (use->opcode != XI_CALL_METHOD || a != 0)
                return false;
            if (!module_path)
                module_path = aot_native_module_namespace_path(semantic, operation->result_value);
            bool scalar = aot_native_module_call_shape_is_exact(semantic, use, &selector,
                                                                &receiver_value, &arity) &&
                          receiver_value == operation->result_value && module_path &&
                          xr_stdlib_metadata_exact_native_direct_member(module_path, selector,
                                                                        (uint16_t) arity);
            bool yieldable = aot_native_module_yieldable_call_is_exact(
                semantic, i, use, module_path, operation->result_value, operation->result_type);
            if (!scalar && !yieldable)
                return false;
            consumed = true;
        }
    }
    return operation->opcode == XI_IMPORT_REF || consumed;
}

static bool aot_index_native_module_namespace_values(VerifyAuthority *ctx) {
    if (!verify_alloc(ctx, ctx->value_count, sizeof(*ctx->exact_native_module_namespace_value),
                      (void **) &ctx->exact_native_module_namespace_value))
        return false;
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(ctx->semantic, i);
        if (!operation ||
            (operation->opcode != XI_IMPORT_REF && operation->opcode != XI_GET_SHARED) ||
            operation->result_value >= ctx->value_count ||
            !aot_native_module_namespace_value_is_exact(ctx->semantic, operation))
            continue;
        ctx->exact_native_module_namespace_value[operation->result_value] = 1;
    }
    return true;
}

static bool aot_source_namespace_operation_is_exact(const XrSemanticPlan *semantic,
                                                    const XrSemanticOperationRecord *operation,
                                                    uint16_t opcode) {
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
    return semantic && operation && type && operation->opcode == opcode &&
           !operation->allocation_key && aot_stable_id_is_zero(operation->allocation_id) &&
           operation->constant == XR_SEMANTIC_INDEX_NONE &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->auxiliary_kind == 0 &&
           operation->effects == xi_generated_op_effects(opcode) &&
           operation->flags == xi_generated_op_default_flags(opcode) &&
           operation->ownership_use == xi_generated_op_own_use(opcode) &&
           operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
           operation->result_alias_operand == -1 &&
           operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
           operation->return_parameter == -1 && operation->return_complete == 1 &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           ((opcode == XI_IMPORT_REF && operation->operand_count == 0 &&
             operation->semantic_immediate >= -1 && operation->semantic_immediate <= UINT16_MAX &&
             operation->metadata_count == 2) ||
            (opcode == XI_GET_SHARED && operation->operand_count == 0 &&
             operation->semantic_immediate >= 0 && operation->semantic_immediate <= UINT16_MAX &&
             operation->metadata_count == 0));
}

static bool aot_source_import_dependency_is_exact(const XrSemanticPlan *semantic,
                                                  const XrSemanticOperationRecord *operation,
                                                  const char *const *metadata,
                                                  uint32_t metadata_count, bool named_export,
                                                  uint32_t *out_dependency) {
    if (!semantic || !operation || !metadata || !out_dependency ||
        !aot_source_namespace_operation_is_exact(semantic, operation, XI_IMPORT_REF) ||
        operation->function != 0 ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_SOURCE_MODULE ||
        operation->metadata_begin + 1u >= metadata_count)
        return false;
    const char *module_path = metadata[operation->metadata_begin];
    const char *member = metadata[operation->metadata_begin + 1u];
    if (!module_path || !module_path[0] || !member || ((member[0] != '\0') != named_export))
        return false;
    uint32_t match = XR_SEMANTIC_INDEX_NONE;
    uint32_t dependency_count = (uint32_t) xr_semantic_plan_dependency_count(semantic);
    for (uint32_t i = 0; i < dependency_count; i++) {
        const XrSemanticDependencyRecord *dependency = xr_semantic_plan_dependency(semantic, i);
        if (!dependency || !dependency->module_path ||
            strcmp(dependency->module_path, module_path) != 0)
            continue;
        if (match != XR_SEMANTIC_INDEX_NONE)
            return false;
        match = i;
    }
    if (match == XR_SEMANTIC_INDEX_NONE)
        return false;
    *out_dependency = match;
    return true;
}

static bool aot_source_namespace_identity_copy_is_exact(const XrSemanticPlan *semantic,
                                                        const XrSemanticOperationRecord *operation,
                                                        const XrSemanticOperandRecord *operands,
                                                        uint32_t operand_count) {
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
    if (!semantic || !operation || !operands || !type || operation->opcode != XI_COPY ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->semantic_immediate != XI_COPY_KIND_IDENTITY || operation->allocation_key ||
        !aot_stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE || operation->auxiliary_kind != 0 ||
        operation->metadata_count != 0 || operation->effects != xi_generated_op_effects(XI_COPY) ||
        operation->flags != xi_generated_op_default_flags(XI_COPY) ||
        operation->ownership_use != xi_generated_op_own_use(XI_COPY) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_alias_operand != 0 ||
        operation->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        operation->return_parameter != -1 || operation->return_complete != 1 ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    return source->role == XR_SEM_OPERAND_VALUE && source->parameter == -1 &&
           source->type == operation->result_type && source->flags == 0;
}

static bool
aot_source_namespace_value_operation_is_exact(const XrSemanticPlan *semantic,
                                              const XrSemanticOperationRecord *operation) {
    if (!operation)
        return false;
    if (operation->opcode == XI_IMPORT_REF || operation->opcode == XI_GET_SHARED)
        return aot_source_namespace_operation_is_exact(semantic, operation, operation->opcode);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    return aot_source_namespace_identity_copy_is_exact(semantic, operation, operands,
                                                       operand_count);
}

/* Independent from both Target collectors: derive either the complete frozen
 * IMPORT_REF -> SET_SHARED -> GET_SHARED -> SOURCE_EXPORT receiver chain or an
 * exact IMPORT_REF -> RETAIN + SET_SHARED lifecycle whose slot is never read. */
static bool aot_index_source_namespace_values(VerifyAuthority *ctx) {
    uint32_t *target_by_operation = NULL;
    uint32_t *expected_uses = NULL;
    uint32_t *retain_uses = NULL;
    uint32_t *consumer = NULL;
    uint32_t *visit_epoch = NULL;
    uint8_t *candidate = NULL;
    uint8_t *standalone_import = NULL;
    if (!verify_alloc(ctx, ctx->value_count, sizeof(*ctx->exact_source_namespace_value),
                      (void **) &ctx->exact_source_namespace_value) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*ctx->source_namespace_dependency_by_value),
                      (void **) &ctx->source_namespace_dependency_by_value) ||
        !verify_alloc(ctx, ctx->operation_count, sizeof(*target_by_operation),
                      (void **) &target_by_operation) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*expected_uses), (void **) &expected_uses) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*retain_uses), (void **) &retain_uses) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*consumer), (void **) &consumer) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*visit_epoch), (void **) &visit_epoch) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*candidate), (void **) &candidate) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*standalone_import),
                      (void **) &standalone_import)) {
        xr_free(target_by_operation);
        xr_free(expected_uses);
        xr_free(retain_uses);
        xr_free(consumer);
        xr_free(visit_epoch);
        xr_free(candidate);
        xr_free(standalone_import);
        return false;
    }
    for (uint32_t i = 0; i < ctx->value_count; i++) {
        ctx->source_namespace_dependency_by_value[i] = XR_SEMANTIC_INDEX_NONE;
        consumer[i] = XR_SEMANTIC_INDEX_NONE;
    }
    for (uint32_t i = 0; i < ctx->operation_count; i++)
        target_by_operation[i] = XR_SEMANTIC_INDEX_NONE;
    uint32_t target_count = (uint32_t) xr_semantic_plan_call_target_count(ctx->semantic);
    for (uint32_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(ctx->semantic, i);
        if (!target || target->kind != XR_SEM_CALL_TARGET_SOURCE_EXPORT)
            continue;
        if (target->operation >= ctx->operation_count ||
            target->dependency >= xr_semantic_plan_dependency_count(ctx->semantic) ||
            target_by_operation[target->operation] != XR_SEMANTIC_INDEX_NONE)
            goto invalid;
        target_by_operation[target->operation] = i;
    }
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(ctx->semantic, &metadata_count);
    uint32_t next_epoch = 1;
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        uint32_t target_index = target_by_operation[i];
        if (target_index == XR_SEMANTIC_INDEX_NONE)
            continue;
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(ctx->semantic, target_index);
        const XrSemanticOperationRecord *call = xr_semantic_plan_operation(ctx->semantic, i);
        bool named_export = call && call->opcode == XI_CALL;
        if (!target || !call || (call->opcode != XI_CALL && call->opcode != XI_CALL_METHOD) ||
            call->operand_count == 0 || call->operand_begin >= operand_count)
            goto invalid;
        const XrSemanticOperandRecord *receiver = &operands[call->operand_begin];
        if (receiver->role != (named_export ? XR_SEM_OPERAND_CALLEE : XR_SEM_OPERAND_RECEIVER) ||
            receiver->parameter != -1 || receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
            receiver->parameter_mode != XR_PARAM_READ || receiver->access != XR_CALL_ARG_PLAIN ||
            receiver->flags != (named_export ? 0u : XR_SEM_OPERAND_CALL_CONTRACT))
            goto invalid;
        const XrSemanticOperationRecord *load = NULL;
        uint32_t current_value = receiver->value;
        uint32_t consumer_index = i;
        uint32_t namespace_type = receiver->type;
        uint32_t epoch = next_epoch++;
        if (epoch == 0) {
            memset(visit_epoch, 0, ctx->value_count * sizeof(*visit_epoch));
            epoch = next_epoch++;
        }
        for (uint32_t depth = 0;; depth++) {
            if (depth >= ctx->operation_count || current_value >= ctx->value_count ||
                visit_epoch[current_value] == epoch)
                goto invalid;
            visit_epoch[current_value] = epoch;
            uint32_t definition_index = ctx->operation_by_value[current_value];
            const XrSemanticOperationRecord *source =
                definition_index != XR_SEMANTIC_INDEX_NONE
                    ? xr_semantic_plan_operation(ctx->semantic, definition_index)
                    : NULL;
            if (!source || source->result_value != current_value ||
                source->result_type != namespace_type || source->function != call->function ||
                (candidate[current_value] &&
                 (ctx->source_namespace_dependency_by_value[current_value] != target->dependency ||
                  consumer[current_value] != consumer_index)))
                goto invalid;
            candidate[current_value] = 1;
            ctx->source_namespace_dependency_by_value[current_value] = target->dependency;
            consumer[current_value] = consumer_index;
            if (aot_source_namespace_operation_is_exact(ctx->semantic, source, XI_GET_SHARED)) {
                load = source;
                break;
            }
            if (!aot_source_namespace_identity_copy_is_exact(ctx->semantic, source, operands,
                                                             operand_count))
                goto invalid;
            const XrSemanticOperandRecord *input = &operands[source->operand_begin];
            consumer_index = definition_index;
            current_value = input->value;
        }
        uint32_t store_index = XR_SEMANTIC_INDEX_NONE;
        for (uint32_t j = 0; j < ctx->operation_count; j++) {
            const XrSemanticOperationRecord *store = xr_semantic_plan_operation(ctx->semantic, j);
            if (!store || store->function != 0 || store->opcode != XI_SET_SHARED ||
                store->semantic_immediate != load->semantic_immediate)
                continue;
            if (store_index != XR_SEMANTIC_INDEX_NONE)
                goto invalid;
            store_index = j;
        }
        const XrSemanticOperationRecord *store =
            store_index != XR_SEMANTIC_INDEX_NONE
                ? xr_semantic_plan_operation(ctx->semantic, store_index)
                : NULL;
        if (!store || store->operand_count != 1 || store->operand_begin >= operand_count)
            goto invalid;
        const XrSemanticOperandRecord *stored = &operands[store->operand_begin];
        const XrSemanticOperationRecord *import = NULL;
        current_value = stored->value;
        consumer_index = store_index;
        namespace_type = stored->type;
        epoch = next_epoch++;
        if (epoch == 0) {
            memset(visit_epoch, 0, ctx->value_count * sizeof(*visit_epoch));
            epoch = next_epoch++;
        }
        for (uint32_t depth = 0;; depth++) {
            if (depth >= ctx->operation_count || current_value >= ctx->value_count ||
                visit_epoch[current_value] == epoch)
                goto invalid;
            visit_epoch[current_value] = epoch;
            uint32_t definition_index = ctx->operation_by_value[current_value];
            const XrSemanticOperationRecord *source =
                definition_index != XR_SEMANTIC_INDEX_NONE
                    ? xr_semantic_plan_operation(ctx->semantic, definition_index)
                    : NULL;
            if (!source || source->result_value != current_value ||
                source->result_type != namespace_type || source->function != 0 ||
                (candidate[current_value] &&
                 (ctx->source_namespace_dependency_by_value[current_value] != target->dependency ||
                  consumer[current_value] != consumer_index)))
                goto invalid;
            candidate[current_value] = 1;
            ctx->source_namespace_dependency_by_value[current_value] = target->dependency;
            consumer[current_value] = consumer_index;
            if (aot_source_namespace_operation_is_exact(ctx->semantic, source, XI_IMPORT_REF)) {
                import = source;
                break;
            }
            if (!aot_source_namespace_identity_copy_is_exact(ctx->semantic, source, operands,
                                                             operand_count))
                goto invalid;
            const XrSemanticOperandRecord *input = &operands[source->operand_begin];
            consumer_index = definition_index;
            current_value = input->value;
        }
        uint32_t import_dependency = XR_SEMANTIC_INDEX_NONE;
        if (!import || !load || receiver->type != load->result_type || import->function != 0 ||
            load->function != call->function || store->function != 0 ||
            store->semantic_immediate != load->semantic_immediate ||
            stored->type != import->result_type || stored->role != XR_SEM_OPERAND_VALUE ||
            stored->parameter != -1 || stored->ownership_action != XR_SEM_OPERAND_CONSUME ||
            stored->parameter_mode != XR_PARAM_READ || stored->access != XR_CALL_ARG_PLAIN ||
            stored->flags != 0 || load->result_type != import->result_type ||
            !aot_source_import_dependency_is_exact(ctx->semantic, import, metadata, metadata_count,
                                                   named_export, &import_dependency) ||
            import_dependency != target->dependency)
            goto invalid;
    }
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *import = xr_semantic_plan_operation(ctx->semantic, i);
        if (!import || import->opcode != XI_IMPORT_REF ||
            import->import_resolution != XR_SEM_IMPORT_RESOLUTION_SOURCE_MODULE ||
            import->metadata_count != 2 || import->metadata_begin + 1u >= metadata_count ||
            !metadata || !metadata[import->metadata_begin + 1u] ||
            metadata[import->metadata_begin + 1u][0] != '\0')
            continue;
        if (import->result_value >= ctx->value_count)
            goto invalid;
        if (candidate[import->result_value])
            continue;
        uint32_t import_dependency = XR_SEMANTIC_INDEX_NONE;
        if (!aot_source_import_dependency_is_exact(ctx->semantic, import, metadata, metadata_count,
                                                   false, &import_dependency))
            goto invalid;
        uint32_t store_index = XR_SEMANTIC_INDEX_NONE;
        int64_t shared_slot = -1;
        for (uint32_t j = 0; j < ctx->operation_count; j++) {
            const XrSemanticOperationRecord *operation =
                xr_semantic_plan_operation(ctx->semantic, j);
            if (!operation || operation->opcode != XI_SET_SHARED || operation->function != 0 ||
                operation->operand_count != 1 || operation->operand_begin >= operand_count)
                continue;
            const XrSemanticOperandRecord *stored = &operands[operation->operand_begin];
            if (stored->value != import->result_value)
                continue;
            if (store_index != XR_SEMANTIC_INDEX_NONE || operation->semantic_immediate < 0 ||
                operation->semantic_immediate > UINT16_MAX || stored->type != import->result_type ||
                stored->role != XR_SEM_OPERAND_VALUE || stored->parameter != -1 ||
                stored->ownership_action != XR_SEM_OPERAND_CONSUME ||
                stored->parameter_mode != XR_PARAM_READ || stored->access != XR_CALL_ARG_PLAIN ||
                stored->flags != 0)
                goto invalid;
            store_index = j;
            shared_slot = operation->semantic_immediate;
        }
        if (store_index == XR_SEMANTIC_INDEX_NONE)
            goto invalid;
        for (uint32_t j = 0; j < ctx->operation_count; j++) {
            const XrSemanticOperationRecord *operation =
                xr_semantic_plan_operation(ctx->semantic, j);
            if (!operation ||
                ((operation->opcode == XI_GET_SHARED || operation->opcode == XI_SET_SHARED) &&
                 operation->semantic_immediate == shared_slot && j != store_index))
                goto invalid;
        }
        candidate[import->result_value] = 1;
        standalone_import[import->result_value] = 1;
        ctx->source_namespace_dependency_by_value[import->result_value] = import_dependency;
        consumer[import->result_value] = store_index;
    }
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *use = xr_semantic_plan_operation(ctx->semantic, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin)
            goto invalid;
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand = &operands[use->operand_begin + a];
            if (operand->value >= ctx->value_count || !candidate[operand->value])
                continue;
            const XrSemanticOperationRecord *source =
                xr_semantic_plan_operation(ctx->semantic, ctx->operation_by_value[operand->value]);
            bool expected = i == consumer[operand->value] && a == 0;
            if (expected && (use->opcode == XI_CALL || use->opcode == XI_CALL_METHOD))
                expected = target_by_operation[i] != XR_SEMANTIC_INDEX_NONE &&
                           xr_semantic_plan_call_target(ctx->semantic, target_by_operation[i])
                                   ->dependency ==
                               ctx->source_namespace_dependency_by_value[operand->value] &&
                           operand->role == (use->opcode == XI_CALL ? XR_SEM_OPERAND_CALLEE
                                                                    : XR_SEM_OPERAND_RECEIVER);
            else if (expected)
                expected = (use->opcode == XI_COPY || use->opcode == XI_SET_SHARED) &&
                           operand->role == XR_SEM_OPERAND_VALUE;
            if (expected) {
                if (expected_uses[operand->value] != 0)
                    goto invalid;
                expected_uses[operand->value] = 1;
                continue;
            }
            bool retain = source && source->opcode == XI_IMPORT_REF && use->opcode == XI_RETAIN &&
                          a == 0 && use->function == source->function &&
                          operand->role == XR_SEM_OPERAND_VALUE &&
                          operand->type == source->result_type && operand->parameter == -1 &&
                          operand->flags == 0;
            if (!retain || retain_uses[operand->value] != 0)
                goto invalid;
            retain_uses[operand->value] = 1;
        }
    }
    uint32_t block_count = (uint32_t) xr_semantic_plan_block_count(ctx->semantic);
    for (uint32_t i = 0; i < block_count; i++) {
        const XrSemanticBlockRecord *block = xr_semantic_plan_block(ctx->semantic, i);
        if (!block ||
            (block->control_value != XR_SEMANTIC_INDEX_NONE &&
             (block->control_value >= ctx->value_count || candidate[block->control_value])))
            goto invalid;
    }
    for (uint32_t i = 0; i < ctx->value_count; i++) {
        if (!candidate[i])
            continue;
        const XrSemanticOperationRecord *source =
            xr_semantic_plan_operation(ctx->semantic, ctx->operation_by_value[i]);
        if (!source || expected_uses[i] != 1 || (standalone_import[i] && retain_uses[i] != 1))
            goto invalid;
        ctx->exact_source_namespace_value[i] = 1;
    }
    xr_free(target_by_operation);
    xr_free(expected_uses);
    xr_free(retain_uses);
    xr_free(consumer);
    xr_free(visit_epoch);
    xr_free(candidate);
    xr_free(standalone_import);
    return true;
invalid:
    xr_free(target_by_operation);
    xr_free(expected_uses);
    xr_free(retain_uses);
    xr_free(consumer);
    xr_free(visit_epoch);
    xr_free(candidate);
    xr_free(standalone_import);
    set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE, (uint32_t) ctx->work,
             0, 0);
    return false;
}

/* One module-wide table of shared slots is owned by the module root, so a slot
 * index names the same slot in every function that reads or writes it. These
 * rows are indexed by that index alone; the storing function is recorded so the
 * owner judgement can require it to be the callee's lexical parent. */
typedef struct AotGoStoreRow {
    uint32_t function;
    uint32_t operation;
    uint8_t ambiguous;
} AotGoStoreRow;

static AotGoStoreRow *aot_go_store_lookup(AotGoStoreRow *rows, uint32_t slot_count, int64_t slot) {
    if (!rows || slot < 0 || slot >= (int64_t) slot_count)
        return NULL;
    return &rows[slot];
}

static bool aot_go_store_is_initial_initializer(const XrSemanticPlan *semantic,
                                                uint32_t function_index, uint32_t store_index) {
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(semantic, function_index);
    const XrSemanticOperationRecord *store = xr_semantic_plan_operation(semantic, store_index);
    const XrSemanticBlockRecord *entry =
        function ? xr_semantic_plan_block(semantic, function->block_begin) : NULL;
    if (!function || !store || !entry || entry->function != function_index ||
        store->block != function->block_begin || store_index < entry->operation_begin ||
        store_index >= entry->operation_begin + entry->operation_count)
        return false;
    for (uint32_t i = entry->operation_begin; i < store_index; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->opcode == XI_CALL || operation->opcode == XI_TAIL_CALL ||
            operation->opcode == XI_CALL_METHOD || operation->opcode == XI_CALL_METHOD_DIRECT ||
            operation->opcode == XI_CALL_BUILTIN || operation->opcode == XI_GO ||
            operation->opcode == XI_THREAD_SPAWN)
            return false;
    }
    return true;
}

/* AOT rebuild is intentionally independent from both Target builder and
 * Target verifier implementations. */
static bool aot_index_direct_local_go_callee_values(VerifyAuthority *ctx) {
    if (!ctx || ctx->operation_count > (1u << 24))
        return false;
    uint32_t slot_count = 0;
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(ctx->semantic, i);
        if (!operation ||
            (operation->opcode != XI_SET_SHARED && operation->opcode != XI_GET_SHARED))
            continue;
        if (operation->semantic_immediate < 0 || operation->semantic_immediate > UINT16_MAX)
            continue;
        uint32_t slot = (uint32_t) operation->semantic_immediate;
        if (slot + 1u > slot_count)
            slot_count = slot + 1u;
    }
    AotGoStoreRow *stores = NULL;
    uint32_t *uses = NULL;
    uint8_t *candidate = NULL;
    uint8_t *invalid = NULL;
    XrSemanticGraph graph = {0};
    bool graph_ready = false;
    if (!verify_alloc(ctx, slot_count, sizeof(*stores), (void **) &stores) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*uses), (void **) &uses) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*candidate), (void **) &candidate) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*invalid), (void **) &invalid) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*ctx->go_callee_target_by_value),
                      (void **) &ctx->go_callee_target_by_value) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*ctx->exact_go_callee_value),
                      (void **) &ctx->exact_go_callee_value)) {
        xr_free(stores);
        xr_free(uses);
        xr_free(candidate);
        xr_free(invalid);
        return false;
    }
    for (uint32_t i = 0; i < ctx->value_count; i++)
        ctx->go_callee_target_by_value[i] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < slot_count; i++)
        stores[i].operation = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(ctx->semantic, i);
        if (!operation)
            goto rejected;
        if (operation->opcode != XI_SET_SHARED || operation->semantic_immediate < 0 ||
            operation->semantic_immediate > UINT16_MAX)
            continue;
        AotGoStoreRow *row = aot_go_store_lookup(stores, slot_count, operation->semantic_immediate);
        if (!row)
            goto rejected;
        if (row->operation != XR_SEMANTIC_INDEX_NONE)
            row->ambiguous = 1;
        else {
            row->operation = i;
            row->function = operation->function;
        }
    }
    char graph_error[128] = {0};
    if (!xr_semantic_graph_build(ctx->semantic, &graph, graph_error, sizeof(graph_error)))
        goto rejected;
    graph_ready = true;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *use = xr_semantic_plan_operation(ctx->semantic, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin)
            goto rejected;
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand = &operands[use->operand_begin + a];
            const XrSemanticOperationRecord *load =
                operand->value < ctx->value_count &&
                        ctx->operation_by_value[operand->value] < ctx->operation_count
                    ? xr_semantic_plan_operation(ctx->semantic,
                                                 ctx->operation_by_value[operand->value])
                    : NULL;
            if (!load || load->opcode != XI_GET_SHARED || use->opcode != XI_GO)
                continue;
            uint32_t value = load->result_value;
            candidate[value] = 1;
            AotGoStoreRow *row = aot_go_store_lookup(stores, slot_count, load->semantic_immediate);
            const XrSemanticOperationRecord *store =
                row && !row->ambiguous && row->operation < ctx->operation_count
                    ? xr_semantic_plan_operation(ctx->semantic, row->operation)
                    : NULL;
            const XrSemanticOperandRecord *stored =
                store && store->operand_count == 1 && store->operand_begin < operand_count
                    ? &operands[store->operand_begin]
                    : NULL;
            const XrSemanticOperationRecord *closure =
                stored && stored->value < ctx->value_count &&
                        ctx->operation_by_value[stored->value] < ctx->operation_count
                    ? xr_semantic_plan_operation(ctx->semantic,
                                                 ctx->operation_by_value[stored->value])
                    : NULL;
            uint32_t target = closure ? closure->callable_function : XR_SEMANTIC_INDEX_NONE;
            const XrSemanticFunctionRecord *callee =
                xr_semantic_plan_function(ctx->semantic, target);
            /* Dominance is a relation only a store in the loading function
             * itself can carry. A store in the slot's owning scope answers the
             * same question through the owner and entry-prefix judgements: it
             * is the callee's lexical parent, and it ran before any operation
             * that could activate the loading function. */
            bool initialized =
                store && (store->function != load->function ||
                          (store->block == load->block
                               ? row->operation < ctx->operation_by_value[value]
                               : xr_semantic_graph_dominates(&graph, store->block, load->block)));
            bool exact =
                row && !row->ambiguous && store && stored && closure && callee && a == 0 &&
                initialized &&
                aot_go_store_is_initial_initializer(ctx->semantic, store->function,
                                                    row->operation) &&
                store->opcode == XI_SET_SHARED && callee->parent == store->function &&
                store->semantic_immediate == load->semantic_immediate && !store->allocation_key &&
                aot_stable_id_is_zero(store->allocation_id) &&
                store->constant == XR_SEMANTIC_INDEX_NONE &&
                store->callable_function == XR_SEMANTIC_INDEX_NONE &&
                store->effects == xi_generated_op_effects(XI_SET_SHARED) &&
                store->result_ownership == xi_generated_op_result_ownership(XI_SET_SHARED) &&
                stored->role == XR_SEM_OPERAND_VALUE && stored->parameter == -1 &&
                stored->transfer_mode == XR_TRANSFER_SHARE &&
                stored->ownership_action == XR_SEM_OPERAND_CONSUME &&
                stored->parameter_mode == XR_PARAM_READ && stored->access == XR_CALL_ARG_PLAIN &&
                stored->origin == XI_PLACE_ORIGIN_NONE &&
                stored->lifetime == XI_PLACE_LIFETIME_NONE &&
                stored->escape == XI_PLACE_ESCAPE_NONE && stored->flags == 0 &&
                closure->function == store->function &&
                semantic_heap_closure_is_exact(ctx->semantic, closure) &&
                use->function == load->function &&
                use->operand_count == (uint16_t) (callee->parameter_count + 1u) &&
                !use->allocation_key && aot_stable_id_is_zero(use->allocation_id) &&
                use->constant == XR_SEMANTIC_INDEX_NONE &&
                use->callable_function == XR_SEMANTIC_INDEX_NONE &&
                use->effects == xi_generated_op_effects(XI_GO) && operand->value == value &&
                operand->type == load->result_type && operand->role == XR_SEM_OPERAND_VALUE &&
                operand->parameter == -1 && operand->transfer_mode == XR_TRANSFER_SHARE &&
                operand->ownership_action == XR_SEM_OPERAND_BORROW &&
                operand->parameter_mode == XR_PARAM_READ && operand->access == XR_CALL_ARG_PLAIN &&
                operand->origin == XI_PLACE_ORIGIN_NONE &&
                operand->lifetime == XI_PLACE_LIFETIME_NONE &&
                operand->escape == XI_PLACE_ESCAPE_NONE && operand->flags == 0 &&
                aot_direct_local_callee_type_is_exact(ctx->semantic, load, target);
            for (uint16_t argument = 1; exact && argument < use->operand_count; argument++) {
                const XrSemanticOperandRecord *arg = &operands[use->operand_begin + argument];
                const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(
                    ctx->semantic, callee->parameter_begin + argument - 1u);
                exact = parameter && arg->type == parameter->type &&
                        arg->role == XR_SEM_OPERAND_VALUE && arg->parameter == -1 &&
                        arg->parameter_mode == XR_PARAM_READ && arg->access == XR_CALL_ARG_PLAIN &&
                        arg->origin == XI_PLACE_ORIGIN_NONE &&
                        arg->lifetime == XI_PLACE_LIFETIME_NONE &&
                        arg->escape == XI_PLACE_ESCAPE_NONE && arg->flags == 0;
            }
            if (!exact ||
                (ctx->go_callee_target_by_value[value] != XR_SEMANTIC_INDEX_NONE &&
                 ctx->go_callee_target_by_value[value] != target) ||
                uses[value] == UINT32_MAX) {
                invalid[value] = 1;
                continue;
            }
            ctx->go_callee_target_by_value[value] = target;
            uses[value]++;
        }
    }
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *use = xr_semantic_plan_operation(ctx->semantic, i);
        for (uint16_t a = 0; use && a < use->operand_count; a++) {
            uint32_t value = operands[use->operand_begin + a].value;
            if (value < ctx->value_count && candidate[value] && (use->opcode != XI_GO || a != 0))
                invalid[value] = 1;
        }
    }
    for (uint32_t i = 0; i < ctx->block_count; i++) {
        const XrSemanticBlockRecord *block = xr_semantic_plan_block(ctx->semantic, i);
        if (block && block->control_value < ctx->value_count && candidate[block->control_value])
            invalid[block->control_value] = 1;
    }
    xr_semantic_graph_dispose(&graph);
    graph_ready = false;
    for (uint32_t i = 0; i < ctx->value_count; i++) {
        if (!candidate[i])
            continue;
        if (invalid[i] || uses[i] == 0 ||
            ctx->go_callee_target_by_value[i] == XR_SEMANTIC_INDEX_NONE)
            goto rejected;
        ctx->exact_go_callee_value[i] = 1;
    }
    xr_free(stores);
    xr_free(uses);
    xr_free(candidate);
    xr_free(invalid);
    return true;

rejected:
    if (graph_ready)
        xr_semantic_graph_dispose(&graph);
    xr_free(stores);
    xr_free(uses);
    xr_free(candidate);
    xr_free(invalid);
    set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE, (uint32_t) ctx->work,
             0, 0);
    return false;
}

static bool verify_static_shared_callable_type_authority(VerifyAuthority *ctx,
                                                         const XrSemanticOperationRecord *operation,
                                                         const XiFunc *owner, const XiValue *live,
                                                         const uint8_t *exact_values,
                                                         const uint32_t *targets) {
    if (!ctx || !operation || !owner || !live || !live->type ||
        operation->result_value >= ctx->value_count || !exact_values || !targets ||
        !exact_values[operation->result_value])
        return false;
    uint32_t target_index = targets[operation->result_value];
    const XrSemanticTypeRecord *semantic_type =
        xr_semantic_plan_type(ctx->semantic, operation->result_type);
    const XrSemanticFunctionRecord *semantic_target =
        xr_semantic_plan_function(ctx->semantic, target_index);
    const XiFunc *shared_owner = owner;
    uint32_t shared_owner_index = operation->function;
    for (uint32_t depth = 0;
         semantic_target && shared_owner && shared_owner_index != semantic_target->parent &&
         depth < ctx->function_count;
         depth++) {
        const XrSemanticFunctionRecord *semantic_owner =
            xr_semantic_plan_function(ctx->semantic, shared_owner_index);
        shared_owner_index = semantic_owner ? semantic_owner->parent : XR_SEMANTIC_INDEX_NONE;
        shared_owner = shared_owner->parent_func;
    }
    if (!aot_direct_local_callee_type_is_exact(ctx->semantic, operation, target_index) ||
        !semantic_type || !semantic_target || shared_owner_index != semantic_target->parent ||
        !shared_owner || shared_owner->semantic_plan_function_index != shared_owner_index ||
        !source_type_matches(live->type, semantic_type) || !shared_owner->shared_slot_funcs ||
        operation->semantic_immediate >= shared_owner->shared_slot_func_count)
        return false;
    bool typed_function = semantic_type->kind == XR_KIND_FUNCTION;
    if (typed_function && (live->type->kind != XR_KIND_FUNCTION ||
                           live->type->function.param_count != semantic_target->parameter_count ||
                           !live->type->function.return_type))
        return false;
    uint16_t slot = (uint16_t) operation->semantic_immediate;
    const XiFunc *callee = shared_owner->shared_slot_funcs[slot];
    uint32_t pointer_matches = 0;
    uint32_t index_matches = 0;
    for (uint16_t i = 0; i < shared_owner->nchildren; i++) {
        const XiFunc *child = shared_owner->children ? shared_owner->children[i] : NULL;
        pointer_matches += child == callee;
        if (child && child->semantic_plan_function_index == target_index) {
            index_matches++;
            if (child != callee)
                return false;
        }
    }
    if (!callee || callee->parent_func != shared_owner || pointer_matches != 1 ||
        index_matches != 1 || callee->semantic_plan_function_index != target_index ||
        callee->nparams != semantic_target->parameter_count || !callee->return_type ||
        !source_type_matches(callee->return_type,
                             xr_semantic_plan_type(ctx->semantic, semantic_target->return_type)) ||
        (typed_function &&
         !source_type_matches(live->type->function.return_type,
                              xr_semantic_plan_type(ctx->semantic, semantic_target->return_type))))
        return false;
    for (uint32_t i = 0; i < semantic_target->parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(ctx->semantic, semantic_target->parameter_begin + i);
        if (!parameter || !callee->params || !callee->params[i] ||
            (typed_function &&
             (!live->type->function.params ||
              live->type->function.params[i].mode != parameter->mode ||
              !source_type_matches(live->type->function.params[i].type,
                                   xr_semantic_plan_type(ctx->semantic, parameter->type)))) ||
            !source_type_matches(callee->params[i]->type,
                                 xr_semantic_plan_type(ctx->semantic, parameter->type)))
            return false;
    }
    return true;
}

static bool verify_direct_local_callee_type_authority(VerifyAuthority *ctx,
                                                      const XrSemanticOperationRecord *operation,
                                                      const XiFunc *owner, const XiValue *live) {
    return verify_static_shared_callable_type_authority(ctx, operation, owner, live,
                                                        ctx->exact_direct_callee_value,
                                                        ctx->direct_callee_target_by_value);
}

static bool verify_direct_local_go_callee_type_authority(VerifyAuthority *ctx,
                                                         const XrSemanticOperationRecord *operation,
                                                         const XiFunc *owner, const XiValue *live) {
    return verify_static_shared_callable_type_authority(
        ctx, operation, owner, live, ctx->exact_go_callee_value, ctx->go_callee_target_by_value);
}

static bool canonical_source_path_matches(const char *frozen, const char *live) {
    if (!frozen || !live)
        return false;
    while (live[0] == '.' && (live[1] == '/' || live[1] == '\\'))
        live += 2;
    if (!live[0])
        return false;
    for (;;) {
        char left = *frozen++;
        char right = *live++;
        if (left == '\\')
            left = '/';
        if (right == '\\')
            right = '/';
        if (left != right)
            return false;
        if (!left)
            return true;
    }
}

static bool verify_source_namespace_type_authority(VerifyAuthority *ctx,
                                                   const XrSemanticOperationRecord *operation,
                                                   const XiValue *live) {
    if (!ctx || !operation || !live || !live->type || operation->result_value >= ctx->value_count ||
        !ctx->exact_source_namespace_value ||
        !ctx->exact_source_namespace_value[operation->result_value] ||
        !aot_source_namespace_value_operation_is_exact(ctx->semantic, operation) ||
        !source_type_matches(live->type,
                             xr_semantic_plan_type(ctx->semantic, operation->result_type)))
        return false;
    if (operation->opcode == XI_IMPORT_REF) {
        uint32_t metadata_count = 0;
        const char *const *metadata = xr_semantic_plan_metadata(ctx->semantic, &metadata_count);
        const XiImportRef *ref = live->aux ? (const XiImportRef *) live->aux : NULL;
        const char *member = ref ? ref->member_name : NULL;
        /* module_path is the source spelling (for example a package name).
         * The resolver-bound module path is the durable identity frozen by
         * SemanticPlan, so independently normalize and compare that path. */
        const char *resolved_path =
            ref && xi_import_ref_is_source_module(ref) && ref->resolved_module
                ? ref->resolved_module->path
                : NULL;
        return live->op == XI_IMPORT_REF && ref && metadata && resolved_path &&
               operation->metadata_begin + 1u < metadata_count &&
               canonical_source_path_matches(metadata[operation->metadata_begin], resolved_path) &&
               strcmp(member ? member : "", metadata[operation->metadata_begin + 1u]) == 0;
    }
    if (operation->opcode == XI_COPY) {
        uint32_t operand_count = 0;
        const XrSemanticOperandRecord *operands =
            xr_semantic_plan_operands(ctx->semantic, &operand_count);
        const XrSemanticFunctionRecord *function =
            xr_semantic_plan_function(ctx->semantic, operation->function);
        if (!operands || !function || operation->operand_begin >= operand_count)
            return false;
        uint32_t source_value = operands[operation->operand_begin].value;
        return source_value < ctx->value_count && live->op == XI_COPY &&
               live->aux_int == XI_COPY_KIND_IDENTITY && live->nargs == 1 && live->args &&
               live->block && live->block->func &&
               live->block->func->semantic_plan_function_index == operation->function &&
               source_value >= function->value_begin &&
               source_value - function->value_begin < function->value_count &&
               live->args[0] == ctx->live_by_value[source_value] && live->args[0] &&
               live->args[0]->block && live->args[0]->block->func == live->block->func;
    }
    return operation->opcode == XI_GET_SHARED && live->op == XI_GET_SHARED &&
           live->aux_int == operation->semantic_immediate;
}

static bool verify_authority_init(VerifyAuthority *ctx) {
    size_t function_count = xr_semantic_plan_function_count(ctx->semantic);
    size_t block_count = xr_semantic_plan_block_count(ctx->semantic);
    size_t operation_count = xr_semantic_plan_operation_count(ctx->semantic);
    size_t parameter_count = xr_semantic_plan_parameter_count(ctx->semantic);
    size_t type_count = xr_semantic_plan_type_count(ctx->semantic);
    uint32_t operand_count = 0, call_count = 0, layout_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target_plan, &call_count);
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    if (function_count > XR_AOT_REFINEMENT_MAX_RECORDS ||
        block_count > XR_AOT_REFINEMENT_MAX_RECORDS ||
        operation_count > XR_AOT_REFINEMENT_MAX_RECORDS ||
        parameter_count > XR_AOT_REFINEMENT_MAX_RECORDS ||
        type_count > XR_AOT_REFINEMENT_MAX_RECORDS ||
        operand_count > XR_AOT_REFINEMENT_MAX_RECORDS ||
        call_count > XR_AOT_REFINEMENT_MAX_RECORDS ||
        layout_count > XR_AOT_REFINEMENT_MAX_RECORDS) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_RESOURCE_BUDGET, 0, 0, 0);
        return false;
    }
    ctx->function_count = (uint32_t) function_count;
    ctx->block_count = (uint32_t) block_count;
    ctx->operation_count = (uint32_t) operation_count;
    ctx->parameter_count = (uint32_t) parameter_count;
    ctx->type_count = (uint32_t) type_count;
    uint64_t values = 0;
    for (uint32_t i = 0; i < ctx->function_count; i++) {
        const XrSemanticFunctionRecord *function = xr_semantic_plan_function(ctx->semantic, i);
        if (!function || (uint64_t) function->value_begin + function->value_count > UINT32_MAX) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, 0, 0, 0);
            return false;
        }
        uint64_t end = (uint64_t) function->value_begin + function->value_count;
        if (end > values)
            values = end;
    }
    if (values > XR_AOT_REFINEMENT_MAX_RECORDS) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_RESOURCE_BUDGET, 0, 0, 0);
        return false;
    }
    ctx->value_count = (uint32_t) values;
    if (!verify_charge_work(ctx, (uint64_t) ctx->function_count + ctx->value_count +
                                     ctx->operation_count + ctx->parameter_count + ctx->type_count +
                                     operand_count + call_count + layout_count))
        return false;
    if (!verify_alloc(ctx, ctx->value_count, sizeof(uint32_t),
                      (void **) &ctx->operation_by_value) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(uint32_t),
                      (void **) &ctx->parameter_by_value) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(uint32_t),
                      (void **) &ctx->use_count_by_value) ||
        !verify_alloc(ctx, ctx->operation_count, sizeof(uint32_t),
                      (void **) &ctx->call_by_operation) ||
        !verify_alloc(ctx, ctx->type_count, sizeof(uint32_t), (void **) &ctx->layout_by_type) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*ctx->live_by_value),
                      (void **) &ctx->live_by_value) ||
        !verify_alloc(ctx, ctx->block_count, sizeof(*ctx->live_by_block),
                      (void **) &ctx->live_by_block) ||
        !verify_alloc(ctx, ctx->function_count, sizeof(*ctx->seen_function),
                      (void **) &ctx->seen_function) ||
        !verify_alloc(ctx, ctx->block_count, sizeof(*ctx->seen_block),
                      (void **) &ctx->seen_block) ||
        !verify_alloc(ctx, ctx->view->record_count, sizeof(*ctx->seen_record),
                      (void **) &ctx->seen_record))
        return false;
    for (uint32_t i = 0; i < ctx->value_count; i++) {
        ctx->operation_by_value[i] = XR_SEMANTIC_INDEX_NONE;
        ctx->parameter_by_value[i] = XR_SEMANTIC_INDEX_NONE;
    }
    for (uint32_t i = 0; i < ctx->operation_count; i++)
        ctx->call_by_operation[i] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < ctx->type_count; i++)
        ctx->layout_by_type[i] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(ctx->semantic, i);
        if (!operation || operation->result_value >= ctx->value_count ||
            ctx->operation_by_value[operation->result_value] != XR_SEMANTIC_INDEX_NONE) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, i,
                     operation ? operation->result_value : 0, i);
            return false;
        }
        ctx->operation_by_value[operation->result_value] = i;
    }
    for (uint32_t i = 0; i < ctx->parameter_count; i++) {
        const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(ctx->semantic, i);
        if (!parameter || parameter->value >= ctx->value_count ||
            ctx->parameter_by_value[parameter->value] != XR_SEMANTIC_INDEX_NONE) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, i,
                     parameter ? parameter->value : 0, 0);
            return false;
        }
        ctx->parameter_by_value[parameter->value] = i;
    }
    for (uint32_t i = 0; i < operand_count; i++) {
        if (!operands || operands[i].value >= ctx->value_count ||
            ctx->use_count_by_value[operands[i].value] == UINT32_MAX) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, i,
                     operands ? operands[i].value : 0, 0);
            return false;
        }
        ctx->use_count_by_value[operands[i].value]++;
    }
    for (uint32_t i = 0; i < call_count; i++) {
        if (!calls || calls[i].id != i || calls[i].semantic_operation >= ctx->operation_count ||
            ctx->call_by_operation[calls[i].semantic_operation] != XR_SEMANTIC_INDEX_NONE) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, i, 0,
                     calls ? calls[i].semantic_operation : 0);
            return false;
        }
        ctx->call_by_operation[calls[i].semantic_operation] = i;
    }
    for (uint32_t i = 0; i < layout_count; i++) {
        if (!layouts || layouts[i].id != i || layouts[i].semantic_type >= ctx->type_count ||
            ctx->layout_by_type[layouts[i].semantic_type] != XR_SEMANTIC_INDEX_NONE) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_LAYOUT, i, 0, 0);
            return false;
        }
        ctx->layout_by_type[layouts[i].semantic_type] = i;
    }
    return aot_index_direct_local_callee_values(ctx) &&
           aot_index_direct_local_go_callee_values(ctx) && aot_index_source_namespace_values(ctx) &&
           aot_index_native_module_namespace_values(ctx) && aot_index_channel_values(ctx);
}

static bool oracle_machine_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                   XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    uint32_t parameter_index = ctx->parameter_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    const XrSemanticParameterRecord *parameter =
        parameter_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(ctx->semantic, parameter_index)
            : NULL;
    uint32_t type_index = parameter   ? parameter->type
                          : operation ? operation->result_type
                                      : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(ctx->semantic, type_index);
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *machine =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    if (!type || !binding || !machine || binding->semantic_value != semantic_value ||
        type->child_count != 0 || type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        (type->flags & (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_AGGREGATE_EXACT)) != 0)
        return false;
    if (!machine_storage_class(machine->kind, out_storage))
        return false;
    *out_machine_kind = machine->kind;
    return true;
}

/* Value aggregates do not participate in the scalar adapter protocol: the C
 * backend materializes their frozen TargetPlan layout directly. TAGGED is the
 * legacy Xi "no scalar adapter" marker here, not a request to box the value.
 * Requiring the complete aggregate binding keeps that marker from becoming a
 * type-shaped fallback for an unplanned aggregate family. */
static bool oracle_value_aggregate_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                           XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    uint32_t parameter_index = ctx->parameter_by_value[semantic_value];
    if ((operation_index == XR_SEMANTIC_INDEX_NONE) == (parameter_index == XR_SEMANTIC_INDEX_NONE))
        return false;
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    const XrSemanticParameterRecord *parameter =
        parameter_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(ctx->semantic, parameter_index)
            : NULL;
    uint32_t semantic_type = parameter   ? parameter->type
                             : operation ? operation->result_type
                                         : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(ctx->semantic, semantic_type);
    XrSemanticValueAggregateShape aggregate_shape = {0};
    /* The shape judgement is the one the target plan itself applied when it
     * gave this value an aggregate representation, so refinement cannot admit a
     * narrower aggregate family than the plan it re-proves. A tuple and a named
     * value class occupy an aggregate slot the same way; only the named class
     * carries the extra demand that its field identity be complete, which is
     * what the plan builder and the plan verifier require of it and of nothing
     * else. */
    bool exact_shape =
        type && xr_semantic_aggregate_type_kind(type) == 1 &&
        (type->kind != XR_KIND_INSTANCE || xr_semantic_value_aggregate_shape_for_type(
                                               ctx->semantic, semantic_type, &aggregate_shape));
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t layout_count = 0;
    uint32_t field_count = 0;
    uint32_t slot_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetFieldRecord *fields = xr_target_plan_fields(ctx->target_plan, &field_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetLayoutRecord *layout =
        register_rep && layouts && register_rep->detail < layout_count
            ? &layouts[register_rep->detail]
            : NULL;
    const XrTargetSlotRecord *slot =
        binding && slots && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t semantic_function = parameter   ? parameter->function
                                 : operation ? operation->function
                                             : XR_SEMANTIC_INDEX_NONE;
    if (!exact_shape || !binding || !register_rep || !memory_rep || !layout || !slot ||
        register_rep->kind != XR_MACHINE_REP_AGGREGATE ||
        memory_rep->kind != XR_MACHINE_REP_AGGREGATE ||
        register_rep->detail != memory_rep->detail ||
        register_rep->root_kind != XR_TARGET_ROOT_NONE ||
        memory_rep->root_kind != XR_TARGET_ROOT_NONE ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        layout->id != register_rep->detail || layout->semantic_type != semantic_type ||
        layout->kind != XR_TARGET_LAYOUT_AGGREGATE || layout->field_count == 0 || !fields ||
        layout->field_begin > field_count ||
        layout->field_count > field_count - layout->field_begin ||
        slot->semantic_value != semantic_value || slot->semantic_operation != operation_index ||
        slot->function != semantic_function || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->root_kind != XR_TARGET_ROOT_NONE ||
        slot->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_AGGREGATE;
    return true;
}

/* An object nests, so the two judgements below call each other: a field read
 * whose own result is an object is the receiver of the next read. The bound is
 * on that nesting, and a deeper one stays unproven rather than searched. */
#define XR_AOT_STRUCT_OBJECT_MAX_NESTING 8u

static bool struct_object_field_access_is_exact_depth(const VerifyAuthority *ctx,
                                                      uint32_t operation_index, unsigned depth);

/* A bare object literal, whichever instruction the value reaches its use from:
 * the construction that allocates it, the shared-cell read that hands the same
 * allocation back, or the field read that hands out a nested one. The value is
 * a tagged carrier and nothing else.
 *
 * The target plan states no representation for this family -- the shared
 * aggregate judgement answers 0, and the plan's own type classification places
 * no machine kind -- so a binding here would be a row neither the plan builder
 * nor the plan verifier can account for. Demanding its absence is what keeps
 * this authority from admitting a value some other family already placed. */
static bool struct_object_carrier_storage_depth(const VerifyAuthority *ctx, uint32_t semantic_value,
                                                unsigned depth, XrRep *out_storage,
                                                uint16_t *out_machine_kind) {
    if (!ctx || depth > XR_AOT_STRUCT_OBJECT_MAX_NESTING || semantic_value >= ctx->value_count ||
        !out_storage || !out_machine_kind ||
        xr_target_plan_value_rep(ctx->target_plan, semantic_value) != NULL)
        return false;
    /* A second name for the same object is the same allocation, so the family
     * question is asked of the value the name resolves to. The absence of a
     * plan binding is demanded of the name as well as of the allocation: this
     * family exists only where the plan states nothing, and a rename the plan
     * did bind is not one of its members however its source was built. */
    if (!oracle_resolve_identity_rename(ctx, semantic_value, &semantic_value) ||
        xr_target_plan_value_rep(ctx->target_plan, semantic_value) != NULL)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    if (!operation || operation->result_value != semantic_value ||
        ctx->parameter_by_value[semantic_value] != XR_SEMANTIC_INDEX_NONE)
        return false;
    bool carrier = aot_struct_object_allocation_is_exact(ctx->semantic, operation) ||
                   aot_struct_object_shared_read_is_exact(ctx->semantic, operation) ||
                   (operation->opcode == XI_OBJECT_GET_F &&
                    aot_struct_object_type_is_exact(
                        xr_semantic_plan_type(ctx->semantic, operation->result_type)) &&
                    struct_object_field_access_is_exact_depth(ctx, operation_index, depth + 1u));
    if (!carrier)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_dynamic_struct_object_storage(const VerifyAuthority *ctx,
                                                 uint32_t semantic_value, XrRep *out_storage,
                                                 uint16_t *out_machine_kind) {
    return struct_object_carrier_storage_depth(ctx, semantic_value, 0, out_storage,
                                               out_machine_kind);
}

/* A field of a bare object literal, read or written. The receiver is the
 * object the judgement above proves, the field ordinal is the operation's own
 * immediate, and the field type is the one the object type names at that
 * ordinal -- the same three facts an aggregate field access rests on, over a
 * runtime object shape rather than a frozen layout.
 *
 * Every lane of an object is stored as a full tagged value, so the operand
 * written and the result read both travel in the carrier. */
static bool struct_object_field_access_is_exact_depth(const VerifyAuthority *ctx,
                                                      uint32_t operation_index, unsigned depth) {
    const XrSemanticOperationRecord *operation =
        ctx && depth <= XR_AOT_STRUCT_OBJECT_MAX_NESTING
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    uint32_t operand_count = 0;
    uint32_t child_count = 0;
    const XrSemanticOperandRecord *operands =
        ctx ? xr_semantic_plan_operands(ctx->semantic, &operand_count) : NULL;
    const uint32_t *children =
        ctx ? xr_semantic_plan_type_children(ctx->semantic, &child_count) : NULL;
    bool field_read = operation && operation->opcode == XI_OBJECT_GET_F;
    bool field_write = operation && (operation->opcode == XI_OBJECT_INIT_F ||
                                     operation->opcode == XI_OBJECT_SET_F);
    uint16_t expected_operands = field_read ? 1u : field_write ? 2u : 0u;
    if (!operation || !operands || !children || expected_operands == 0 ||
        operation->operand_count != expected_operands || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 0 || operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->effects != xi_generated_op_effects(operation->opcode) ||
        operation->flags != xi_generated_op_default_flags(operation->opcode) ||
        operation->ownership_use != xi_generated_op_own_use(operation->opcode) ||
        operation->result_ownership != xi_generated_op_result_ownership(operation->opcode) ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->view_complete != 0 || operation->view_source_operand != -1 ||
        operation->view_source_parameter != -1)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    XrRep receiver_storage = XR_REP_TAGGED;
    uint16_t receiver_kind = XR_MACHINE_REP_COUNT;
    if (receiver->role != XR_SEM_OPERAND_VALUE || receiver->parameter != -1 ||
        receiver->flags != 0 || receiver->value >= ctx->value_count ||
        !struct_object_carrier_storage_depth(ctx, receiver->value, depth + 1u, &receiver_storage,
                                             &receiver_kind))
        return false;
    const XrSemanticOperationRecord *receiver_definition =
        xr_semantic_plan_operation(ctx->semantic, ctx->operation_by_value[receiver->value]);
    const XrSemanticTypeRecord *receiver_type =
        xr_semantic_plan_type(ctx->semantic, receiver->type);
    if (!receiver_definition || receiver->type != receiver_definition->result_type ||
        !aot_struct_object_type_is_exact(receiver_type) ||
        receiver_type->child_begin > child_count ||
        receiver_type->child_count > child_count - receiver_type->child_begin ||
        operation->semantic_immediate < 0 ||
        operation->semantic_immediate >= (int64_t) receiver_type->child_count)
        return false;
    uint32_t field_type =
        children[receiver_type->child_begin + (uint32_t) operation->semantic_immediate];
    if (field_read)
        return operation->result_type == field_type;
    const XrSemanticOperandRecord *stored = receiver + 1;
    return stored->role == XR_SEM_OPERAND_VALUE && stored->parameter == -1 && stored->flags == 0 &&
           stored->value < ctx->value_count && stored->type == field_type;
}

static bool oracle_struct_object_field_access_is_exact(const VerifyAuthority *ctx,
                                                       uint32_t operation_index) {
    return struct_object_field_access_is_exact_depth(ctx, operation_index, 0);
}

/* The value a proved field read hands back, whatever the field's static type
 * is. A field holds a full tagged value, so the read produces the carrier and
 * every consumer either keeps it or adapts from it; the object-typed subset is
 * what the carrier judgement above admits as a receiver in its own right. */
static bool oracle_struct_object_field_read_storage(const VerifyAuthority *ctx,
                                                    uint32_t semantic_value, XrRep *out_storage,
                                                    uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    if (!operation || operation->opcode != XI_OBJECT_GET_F ||
        operation->result_value != semantic_value ||
        !oracle_struct_object_field_access_is_exact(ctx, operation_index))
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool fixed_array_machine_native_type(uint16_t machine_kind, uint8_t *out_native) {
    if (!out_native)
        return false;
    switch ((XrMachineRepKind) machine_kind) {
        case XR_MACHINE_REP_I1:
            *out_native = XR_NATIVE_BOOL;
            return true;
        case XR_MACHINE_REP_I8:
            *out_native = XR_NATIVE_I8;
            return true;
        case XR_MACHINE_REP_U8:
            *out_native = XR_NATIVE_U8;
            return true;
        case XR_MACHINE_REP_I16:
            *out_native = XR_NATIVE_I16;
            return true;
        case XR_MACHINE_REP_U16:
            *out_native = XR_NATIVE_U16;
            return true;
        case XR_MACHINE_REP_I32:
            *out_native = XR_NATIVE_I32;
            return true;
        case XR_MACHINE_REP_U32:
            *out_native = XR_NATIVE_U32;
            return true;
        case XR_MACHINE_REP_I64:
            *out_native = XR_NATIVE_I64;
            return true;
        case XR_MACHINE_REP_U64:
            *out_native = XR_NATIVE_U64;
            return true;
        case XR_MACHINE_REP_ISIZE:
            *out_native = XR_NATIVE_ISIZE;
            return true;
        case XR_MACHINE_REP_USIZE:
            *out_native = XR_NATIVE_USIZE;
            return true;
        case XR_MACHINE_REP_F32:
            *out_native = XR_NATIVE_F32;
            return true;
        case XR_MACHINE_REP_F64:
            *out_native = XR_NATIVE_F64;
            return true;
        default:
            return false;
    }
}

/* Fixed arrays are value aggregates, but their C identity is the address of
 * the producer's native lane backing, not a named-struct object.  Rebuild that
 * family from the frozen Semantic/Target records so refinement never obtains
 * authority from a live Xi type or from the named-aggregate oracle. */
static bool oracle_fixed_array_backing_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                               XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index < ctx->operation_count
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(ctx->semantic, operation->result_type) : NULL;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t layout_count = 0, field_count = 0, slot_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetFieldRecord *fields = xr_target_plan_fields(ctx->target_plan, &field_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetLayoutRecord *layout =
        register_rep && layouts && register_rep->detail < layout_count
            ? &layouts[register_rep->detail]
            : NULL;
    const XrTargetSlotRecord *slot =
        binding && slots && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    if (!operation || !type || !binding || !register_rep || !memory_rep || !layout || !slot ||
        operation->opcode != XI_FIXED_ARRAY_NEW || operation->result_value != semantic_value ||
        type->kind != XR_KIND_FIXED_ARRAY || type->child_count != 1 ||
        type->aggregate_extent == 0 || type->aggregate_extent > UINT16_MAX ||
        type->scalar_rep != XR_SCALAR_REP_NONE || (type->flags & XR_SEM_TYPE_NULLABLE) != 0 ||
        operation->operand_count != 0 || operation->metadata_count != 0 ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->effects != xi_generated_op_effects(XI_FIXED_ARRAY_NEW) ||
        operation->flags != xi_generated_op_default_flags(XI_FIXED_ARRAY_NEW) ||
        operation->ownership_use != xi_generated_op_own_use(XI_FIXED_ARRAY_NEW) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_complete != 1 ||
        !xr_semantic_allocation_identity_is_canonical(operation) ||
        register_rep->kind != XR_MACHINE_REP_AGGREGATE ||
        memory_rep->kind != XR_MACHINE_REP_AGGREGATE ||
        register_rep->detail != memory_rep->detail ||
        register_rep->root_kind != XR_TARGET_ROOT_NONE ||
        memory_rep->root_kind != XR_TARGET_ROOT_NONE ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        layout->id != register_rep->detail || layout->semantic_type != operation->result_type ||
        layout->kind != XR_TARGET_LAYOUT_AGGREGATE ||
        layout->field_count != type->aggregate_extent || !fields ||
        layout->field_begin > field_count ||
        layout->field_count > field_count - layout->field_begin ||
        slot->semantic_value != semantic_value || slot->semantic_operation != operation_index ||
        slot->function != operation->function || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->root_kind != XR_TARGET_ROOT_NONE ||
        slot->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)
        return false;
    uint16_t lane_kind = XR_MACHINE_REP_COUNT;
    uint8_t lane_native = 0;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        const XrTargetFieldRecord *field = &fields[layout->field_begin + i];
        const XrTargetMachineRepRecord *field_rep =
            xr_target_plan_machine_rep(ctx->target_plan, field->memory_rep);
        uint8_t field_native = 0;
        if (!field_rep || !fixed_array_machine_native_type(field_rep->kind, &field_native) ||
            field->layout != layout->id || field->semantic_field != i ||
            field->semantic_name != XR_SEMANTIC_INDEX_NONE ||
            field->root_kind != XR_TARGET_ROOT_NONE || field->flags != 0 || field->reserved != 0 ||
            (i && (field_rep->kind != lane_kind || field_native != lane_native)))
            return false;
        if (!i) {
            lane_kind = field_rep->kind;
            lane_native = field_native;
        }
    }
    if (operation->semantic_immediate != lane_native)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_AGGREGATE;
    return true;
}

/* The fixed array an element access reads or writes, whichever instruction
 * defined the receiver. The backing oracle above proves a construction, so it
 * answers only for the allocation instruction itself; a fixed array stored into
 * a local cell and read back out is the same aggregate reaching the same
 * element access through a different definition, and demanding the allocation
 * would refuse every fixed array that outlives its own defining instruction.
 * The aggregate judgement is the one the target plan applied when it gave this
 * value an aggregate slot, narrowed to the fixed-array family whose single
 * element type an index names. */
static bool oracle_fixed_array_value_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                             XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    uint32_t parameter_index = ctx->parameter_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    const XrSemanticParameterRecord *parameter =
        parameter_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(ctx->semantic, parameter_index)
            : NULL;
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(ctx->semantic, parameter   ? parameter->type
                                             : operation ? operation->result_type
                                                         : XR_SEMANTIC_INDEX_NONE);
    if (!type || type->kind != XR_KIND_FIXED_ARRAY || type->child_count != 1 ||
        type->aggregate_extent == 0 || type->aggregate_extent > UINT16_MAX)
        return false;
    return oracle_value_aggregate_storage(ctx, semantic_value, out_storage, out_machine_kind);
}

static bool oracle_fixed_array_element_access_is_exact(const VerifyAuthority *ctx,
                                                       uint32_t operation_index) {
    const XrSemanticOperationRecord *operation =
        ctx ? xr_semantic_plan_operation(ctx->semantic, operation_index) : NULL;
    uint32_t operand_count = 0, child_count = 0;
    const XrSemanticOperandRecord *operands =
        ctx ? xr_semantic_plan_operands(ctx->semantic, &operand_count) : NULL;
    const uint32_t *children =
        ctx ? xr_semantic_plan_type_children(ctx->semantic, &child_count) : NULL;
    uint16_t expected = operation && operation->opcode == XI_INDEX_GET   ? 2u
                        : operation && operation->opcode == XI_INDEX_SET ? 3u
                                                                         : 0u;
    if (!operation || !operands || !children || !expected || operation->operand_count != expected ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->effects != xi_generated_op_effects(operation->opcode) ||
        operation->flags != xi_generated_op_default_flags(operation->opcode) ||
        operation->ownership_use != xi_generated_op_own_use(operation->opcode) ||
        operation->result_ownership != xi_generated_op_result_ownership(operation->opcode) ||
        operation->metadata_count != 0 || operation->semantic_immediate != 0 ||
        operation->allocation_key != NULL || operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *index = receiver + 1;
    XrRep storage = XR_REP_TAGGED;
    uint16_t kind = XR_MACHINE_REP_COUNT;
    if (receiver->role != XR_SEM_OPERAND_VALUE || receiver->parameter != -1 ||
        receiver->flags != 0 || receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        index->role != XR_SEM_OPERAND_VALUE || index->parameter != -1 || index->flags != 0 ||
        !semantic_exact_i64_type(xr_semantic_plan_type(ctx->semantic, index->type)) ||
        receiver->value >= ctx->value_count ||
        !oracle_fixed_array_value_storage(ctx, receiver->value, &storage, &kind) ||
        storage != XR_REP_TAGGED || kind != XR_MACHINE_REP_AGGREGATE)
        return false;
    uint32_t definition_index = ctx->operation_by_value[receiver->value];
    const XrSemanticOperationRecord *definition =
        definition_index < ctx->operation_count
            ? xr_semantic_plan_operation(ctx->semantic, definition_index)
            : NULL;
    const XrSemanticTypeRecord *receiver_type =
        xr_semantic_plan_type(ctx->semantic, receiver->type);
    if (!definition || receiver->type != definition->result_type || !receiver_type ||
        receiver_type->kind != XR_KIND_FIXED_ARRAY || receiver_type->child_count != 1 ||
        receiver_type->child_begin >= child_count)
        return false;
    uint32_t element_type = children[receiver_type->child_begin];
    if (operation->opcode == XI_INDEX_GET)
        return operation->result_type == element_type;
    const XrSemanticOperandRecord *element = index + 1;
    return element->role == XR_SEM_OPERAND_VALUE && element->parameter == -1 &&
           element->flags == 0 && element->type == element_type &&
           element->ownership_action == XR_SEM_OPERAND_CONSUME;
}

static bool oracle_aggregate_field_access_is_exact(const VerifyAuthority *ctx,
                                                   uint32_t operation_index) {
    const XrSemanticOperationRecord *operation =
        ctx ? xr_semantic_plan_operation(ctx->semantic, operation_index) : NULL;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        ctx ? xr_semantic_plan_operands(ctx->semantic, &operand_count) : NULL;
    XrStableId zero = {{0}};
    /* A tuple element read is the same field access an aggregate getter is:
     * one aggregate receiver, one frozen field ordinal, one field type. Only
     * the Xi spelling differs, so both admit the same proof here. */
    bool field_read =
        operation && (operation->opcode == XI_AGG_GET || operation->opcode == XI_TUPLE_GET);
    uint16_t expected_operands = field_read                                     ? 1u
                                 : operation && operation->opcode == XI_AGG_SET ? 2u
                                                                                : 0u;
    if (!operation || expected_operands == 0 || operation->operand_count != expected_operands ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 0 || operation->semantic_immediate < 0 ||
        operation->allocation_key || !xr_stable_id_equal(operation->allocation_id, zero) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->effects != xi_generated_op_effects(operation->opcode) ||
        operation->flags != xi_generated_op_default_flags(operation->opcode) ||
        operation->ownership_use != xi_generated_op_own_use(operation->opcode) ||
        operation->result_ownership != xi_generated_op_result_ownership(operation->opcode) ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->view_complete != 0 || operation->view_source_operand != -1 ||
        operation->view_source_parameter != -1)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    XrRep receiver_storage = XR_REP_TAGGED;
    uint16_t receiver_kind = XR_MACHINE_REP_COUNT;
    if (receiver->role != XR_SEM_OPERAND_VALUE || receiver->parameter != -1 ||
        receiver->flags != 0 || receiver->value >= ctx->value_count ||
        !oracle_value_aggregate_storage(ctx, receiver->value, &receiver_storage, &receiver_kind) ||
        receiver_storage != XR_REP_TAGGED || receiver_kind != XR_MACHINE_REP_AGGREGATE)
        return false;
    const XrTargetValueRepRecord *receiver_binding =
        xr_target_plan_value_rep(ctx->target_plan, receiver->value);
    const XrTargetMachineRepRecord *receiver_rep =
        receiver_binding
            ? xr_target_plan_machine_rep(ctx->target_plan, receiver_binding->register_rep)
            : NULL;
    uint32_t layout_count = 0;
    uint32_t field_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetFieldRecord *fields = xr_target_plan_fields(ctx->target_plan, &field_count);
    const XrTargetLayoutRecord *layout =
        receiver_rep && layouts && receiver_rep->detail < layout_count
            ? &layouts[receiver_rep->detail]
            : NULL;
    uint32_t field_index = (uint32_t) operation->semantic_immediate;
    if (!layout || !fields || field_index >= layout->field_count ||
        layout->field_begin > field_count ||
        layout->field_count > field_count - layout->field_begin ||
        fields[layout->field_begin + field_index].layout != layout->id ||
        fields[layout->field_begin + field_index].semantic_field != field_index)
        return false;
    const XrSemanticTypeRecord *receiver_type =
        xr_semantic_plan_type(ctx->semantic, receiver->type);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(ctx->semantic, &child_count);
    uint32_t child_ordinal =
        receiver_type && receiver_type->kind == XR_KIND_FIXED_ARRAY ? 0u : field_index;
    if (!receiver_type || !children || receiver_type->child_begin > child_count ||
        receiver_type->child_count > child_count - receiver_type->child_begin ||
        child_ordinal >= receiver_type->child_count)
        return false;
    uint32_t field_type = children[receiver_type->child_begin + child_ordinal];
    if (field_read)
        return operation->result_type == field_type;
    const XrSemanticOperandRecord *stored = receiver + 1;
    return stored->role == XR_SEM_OPERAND_VALUE && stored->parameter == -1 && stored->flags == 0 &&
           stored->type == field_type && stored->value < ctx->value_count;
}

static bool aot_array_hof_target_is_exact(const VerifyAuthority *ctx, uint32_t operation_index,
                                          AotArrayHofAuthority *out) {
    AotArrayHofAuthority shape = {0};
    if (!aot_array_hof_semantic_is_exact(ctx, operation_index, &shape) ||
        operation_index >= ctx->operation_count)
        return false;
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(ctx->semantic, operation_index);
    const XrSemanticFunctionRecord *callee =
        operation ? xr_semantic_plan_function(ctx->semantic, operation->callable_function) : NULL;
    uint32_t call_index = ctx->call_by_operation[operation_index];
    uint32_t call_count = 0, argument_count = 0, slot_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target_plan, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(ctx->target_plan, &argument_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetCallRecord *call = calls && call_index < call_count ? &calls[call_index] : NULL;
    const XrTargetValueRepRecord *result =
        operation ? xr_target_plan_value_rep(ctx->target_plan, operation->result_value) : NULL;
    const XrTargetMachineRepRecord *result_register =
        result ? xr_target_plan_machine_rep(ctx->target_plan, result->register_rep) : NULL;
    const XrTargetMachineRepRecord *result_memory =
        result ? xr_target_plan_machine_rep(ctx->target_plan, result->memory_rep) : NULL;
    const XrTargetSlotRecord *result_slot =
        result && slots && result->slot < slot_count ? &slots[result->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    uint32_t layout_index = operation && operation->result_type < ctx->type_count
                                ? ctx->layout_by_type[operation->result_type]
                                : XR_SEMANTIC_INDEX_NONE;
    const XrTargetLayoutRecord *layout =
        layouts && layout_index < layout_count ? &layouts[layout_index] : NULL;
    XrStableId expected_call;
    uint32_t discriminator = ((uint32_t) shape.kind << 16) |
                             ((uint32_t) shape.source_storage << 8) | shape.result_storage;
    if (!operation || !callee || !call || !result || !result_register || !result_memory ||
        !result_slot || !layout || !arguments ||
        !aot_pair_identity("xray-target-array-hof-v1", operation->id, callee->id, discriminator,
                           &expected_call) ||
        !xr_stable_id_equal(call->identity, expected_call) || call->id != call_index ||
        call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        call->semantic_operation != operation_index ||
        call->caller_function != operation->function ||
        call->callee_function != operation->callable_function ||
        call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        call->source_export != XR_SEMANTIC_INDEX_NONE ||
        !aot_stable_id_is_zero(call->source_export_identity) ||
        !aot_stable_id_is_zero(call->source_callee_identity) ||
        call->result_value != operation->result_value || call->result_slot != result->slot ||
        call->result_register_rep != result->register_rep ||
        call->result_memory_rep != result->memory_rep ||
        call->caller_storage_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE ||
        call->argument_count != operation->operand_count || call->argument_begin > argument_count ||
        call->argument_count > argument_count - call->argument_begin || call->adapter_count != 0 ||
        call->flags != 0 || call->calling_convention != XR_TARGET_CALL_CONVENTION_ARRAY_HOF ||
        call->target_kind != XR_TARGET_CALL_TARGET_ARRAY_HOF ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != (shape.kind == XR_TARGET_ARRAY_HOF_REDUCE
                                       ? XR_TARGET_CALL_NONE
                                       : XR_TARGET_CALL_RETURN_OWNED) ||
        call->array_intrinsic_kind != XR_TARGET_ARRAY_INTRINSIC_NONE ||
        call->array_element_storage != shape.source_storage || call->array_hof_kind != shape.kind ||
        call->array_result_element_storage != shape.result_storage || call->reserved8[0] != 0 ||
        call->reserved8[1] != 0 || call->reserved8[2] != 0 ||
        result->semantic_value != operation->result_value || result_slot->id != result->slot ||
        result_slot->function != operation->function ||
        result_slot->semantic_value != operation->result_value ||
        result_slot->semantic_operation != operation_index ||
        result_slot->role != XR_TARGET_SLOT_TEMPORARY ||
        result_slot->register_rep != result->register_rep ||
        result_slot->memory_rep != result->memory_rep || layout->id != layout_index ||
        layout->semantic_type != operation->result_type || layout->field_count != 0 ||
        layout->root_field_count != 0 || layout->fixed_prefix_size != result_memory->memory_size ||
        layout->align != result_memory->memory_align)
        return false;
    bool dynamic_result = shape.kind != XR_TARGET_ARRAY_HOF_REDUCE;
    if (dynamic_result) {
        if (result_register->kind != XR_MACHINE_REP_DYN_VALUE ||
            result_memory->kind != XR_MACHINE_REP_DYN_VALUE ||
            result_register->root_kind != XR_TARGET_ROOT_DYNAMIC ||
            result_memory->root_kind != XR_TARGET_ROOT_DYNAMIC ||
            result_register->ownership != XR_TARGET_OWNERSHIP_OWNED ||
            result_memory->ownership != XR_TARGET_OWNERSHIP_OWNED ||
            result_register->null_encoding != XR_TARGET_NULL_TAGGED ||
            result_memory->null_encoding != XR_TARGET_NULL_TAGGED ||
            result_slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
            result_slot->ownership != XR_TARGET_OWNERSHIP_OWNED ||
            layout->kind != XR_TARGET_LAYOUT_DYNAMIC ||
            layout->array_element_storage != shape.result_storage)
            return false;
    } else {
        XrRep scalar_storage = XR_REP_VOID;
        uint16_t scalar_kind = XR_MACHINE_REP_COUNT;
        if (!oracle_machine_storage(ctx, operation->result_value, &scalar_storage, &scalar_kind) ||
            scalar_storage == XR_REP_TAGGED || scalar_kind != result_register->kind ||
            result_slot->root_kind != XR_TARGET_ROOT_NONE ||
            result_slot->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
            layout->kind != XR_TARGET_LAYOUT_SCALAR ||
            layout->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE)
            return false;
    }
    uint32_t semantic_operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &semantic_operand_count);
    for (uint16_t ordinal = 0; ordinal < call->argument_count; ordinal++) {
        uint32_t semantic_operand = operation->operand_begin + ordinal;
        if (!operands || semantic_operand >= semantic_operand_count)
            return false;
        const XrSemanticOperandRecord *operand = &operands[semantic_operand];
        const XrSemanticTypeRecord *type = xr_semantic_plan_type(ctx->semantic, operand->type);
        const XrTargetValueRepRecord *caller =
            xr_target_plan_value_rep(ctx->target_plan, operand->value);
        const XrTargetCallArgumentRecord *argument = &arguments[call->argument_begin + ordinal];
        uint32_t expected_value = ordinal == 0   ? shape.receiver
                                  : ordinal == 1 ? shape.callback
                                                 : shape.initial;
        XrStableId expected_argument;
        if (!type || !caller || operand->value != expected_value ||
            !aot_pair_identity("xray-target-array-hof-argument-v1", operation->id, type->id,
                               ordinal, &expected_argument) ||
            !xr_stable_id_equal(argument->identity, expected_argument) ||
            argument->call != call_index || argument->semantic_operand != semantic_operand ||
            argument->semantic_value != expected_value ||
            argument->callee_parameter != XR_SEMANTIC_INDEX_NONE ||
            argument->caller_slot != caller->slot ||
            argument->callee_slot != XR_SEMANTIC_INDEX_NONE ||
            argument->register_rep != caller->register_rep ||
            argument->memory_rep != caller->memory_rep ||
            argument->callee_register_rep != caller->register_rep ||
            argument->callee_memory_rep != caller->memory_rep || argument->ordinal != ordinal ||
            argument->mode != XR_TARGET_CALL_VALUE ||
            argument->ownership !=
                (ordinal == 0 ? XR_TARGET_CALL_BORROW : XR_TARGET_CALL_CONSUME) ||
            argument->transfer_mode != operand->transfer_mode || argument->flags != 0 ||
            argument->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
            argument->reserved8[0] != 0 || argument->reserved8[1] != 0 ||
            argument->reserved8[2] != 0)
            return false;
    }
    if (out)
        *out = shape;
    return true;
}

static bool oracle_array_hof_result_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                            XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    AotArrayHofAuthority shape = {0};
    if (operation_index >= ctx->operation_count ||
        !aot_array_hof_target_is_exact(ctx, operation_index, &shape))
        return false;
    if (shape.kind == XR_TARGET_ARRAY_HOF_REDUCE)
        return oracle_machine_storage(ctx, semantic_value, out_storage, out_machine_kind);
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_dynamic_closure_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                           XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    if (!operation || operation->result_value != semantic_value ||
        !semantic_heap_closure_is_exact(ctx->semantic, operation))
        return false;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    uint32_t layout_index = operation->result_type < ctx->type_count
                                ? ctx->layout_by_type[operation->result_type]
                                : XR_SEMANTIC_INDEX_NONE;
    const XrTargetLayoutRecord *layout =
        layouts && layout_index < layout_count ? &layouts[layout_index] : NULL;
    if (!binding || !register_rep || !memory_rep || !slot || !layout ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        register_rep->memory_size != memory_rep->memory_size ||
        register_rep->memory_align != memory_rep->memory_align ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || layout->field_count != 0 ||
        layout->root_field_count != 0 || layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align || slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index || slot->function != operation->function ||
        slot->role != XR_TARGET_SLOT_TEMPORARY || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_OWNED)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_dynamic_array_fill_scalar_storage(const VerifyAuthority *ctx,
                                                     uint32_t semantic_value, XrRep *out_storage,
                                                     uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index < ctx->operation_count
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    uint32_t receiver_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t fill_value = XR_SEMANTIC_INDEX_NONE;
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!aot_array_fill_scalar_is_exact(ctx->semantic, operation, &receiver_value, &fill_value,
                                        &storage))
        return false;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target_plan, &call_count);
    const XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (call)
            return false;
        call = &calls[i];
    }
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; operation && i < layout_count; i++) {
        if (layouts[i].semantic_type != operation->result_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    uint32_t operand_count = 0, argument_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(ctx->target_plan, &argument_count);
    const XrSemanticTypeRecord *receiver_type =
        operation ? xr_semantic_plan_type(ctx->semantic, operation->result_type) : NULL;
    XrStableId expected_call;
    bool expected_call_valid = receiver_type && operation &&
                               aot_pair_identity("xray-target-array-fill-scalar-v1", operation->id,
                                                 receiver_type->id, storage, &expected_call);
    if (!operation || !binding || !register_rep || !memory_rep || !slot || !layout || !call ||
        !operands || !arguments || !expected_call_valid ||
        !xr_stable_id_equal(call->identity, expected_call) ||
        call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        call->caller_function != operation->function ||
        call->callee_function != XR_SEMANTIC_INDEX_NONE ||
        call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        call->source_export != XR_SEMANTIC_INDEX_NONE ||
        !aot_stable_id_is_zero(call->source_export_identity) ||
        !aot_stable_id_is_zero(call->source_callee_identity) ||
        call->result_value != semantic_value || call->result_slot != binding->slot ||
        call->result_register_rep != binding->register_rep ||
        call->result_memory_rep != binding->memory_rep ||
        call->caller_storage_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE || call->argument_count != 2 ||
        call->argument_begin > argument_count ||
        call->argument_count > argument_count - call->argument_begin || call->adapter_count != 0 ||
        call->flags != 0 ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_ARRAY_FILL_SCALAR ||
        call->target_kind != XR_TARGET_CALL_TARGET_ARRAY_FILL_SCALAR ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        call->array_intrinsic_kind != XR_TARGET_ARRAY_INTRINSIC_NONE ||
        call->array_element_storage != storage || call->reserved8[0] != 0 ||
        call->reserved8[1] != 0 || call->reserved8[2] != 0 ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        slot->semantic_value != semantic_value || slot->semantic_operation != operation_index ||
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC || slot->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || layout->array_element_storage != storage)
        return false;
    for (uint16_t ordinal = 0; ordinal < 2; ordinal++) {
        uint32_t semantic_operand = operation->operand_begin + ordinal;
        const XrSemanticOperandRecord *operand = &operands[semantic_operand];
        const XrSemanticTypeRecord *type = xr_semantic_plan_type(ctx->semantic, operand->type);
        const XrTargetValueRepRecord *caller =
            xr_target_plan_value_rep(ctx->target_plan, operand->value);
        const XrTargetCallArgumentRecord *argument = &arguments[call->argument_begin + ordinal];
        XrStableId expected_argument;
        if (!type || !caller || operand->value != (ordinal == 0 ? receiver_value : fill_value) ||
            !aot_pair_identity("xray-target-array-fill-scalar-argument-v1", operation->id, type->id,
                               ordinal, &expected_argument) ||
            !xr_stable_id_equal(argument->identity, expected_argument) ||
            argument->call != call->id || argument->semantic_operand != semantic_operand ||
            argument->semantic_value != operand->value ||
            argument->callee_parameter != XR_SEMANTIC_INDEX_NONE ||
            argument->caller_slot != caller->slot ||
            argument->callee_slot != XR_SEMANTIC_INDEX_NONE ||
            argument->register_rep != caller->register_rep ||
            argument->memory_rep != caller->memory_rep ||
            argument->callee_register_rep != caller->register_rep ||
            argument->callee_memory_rep != caller->memory_rep || argument->ordinal != ordinal ||
            argument->mode != XR_TARGET_CALL_VALUE ||
            argument->ownership !=
                (ordinal == 0 ? XR_TARGET_CALL_BORROW : XR_TARGET_CALL_CONSUME) ||
            argument->transfer_mode != operand->transfer_mode || argument->flags != 0 ||
            argument->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
            argument->reserved8[0] != 0 || argument->reserved8[1] != 0 ||
            argument->reserved8[2] != 0)
            return false;
    }
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_dynamic_panic_catch_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                               XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    if (!operation || operation->result_value != semantic_value ||
        !semantic_panic_catch_is_exact(ctx->semantic, operation))
        return false;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != operation->result_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    if (!binding || !register_rep || !memory_rep || !slot || !layout ||
        binding->semantic_value != semantic_value ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        register_rep->memory_size != memory_rep->memory_size ||
        register_rep->memory_align != memory_rep->memory_align ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || layout->field_count != 0 ||
        layout->root_field_count != 0 || layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align || slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index || slot->function != operation->function ||
        slot->role != XR_TARGET_SLOT_TEMPORARY || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_OWNED)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_dynamic_heap_literal_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                                XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    /* A String literal and a BigInt literal reach this oracle with the same
     * answer -- both are heap constants the target plan bound to the dynamic
     * carrier -- so the storage is read once and each literal is recognised by
     * its own exact predicate. */
    if (!operation || operation->result_value != semantic_value ||
        (!xr_semantic_string_literal_is_exact(ctx->semantic, operation) &&
         !xr_semantic_bigint_value_is_exact(ctx->semantic, operation)))
        return false;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != operation->result_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    if (!binding || !register_rep || !memory_rep || !slot || !layout ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        register_rep->memory_size != memory_rep->memory_size ||
        register_rep->memory_align != memory_rep->memory_align ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || layout->field_count != 0 ||
        layout->root_field_count != 0 || layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align || slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index || slot->function != operation->function ||
        slot->role != XR_TARGET_SLOT_TEMPORARY || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_OWNED)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* String.runes returns a freshly owned iterator in the dynamic carrier. The
 * shared SemanticPlan judgement names the member; this authority independently
 * rebuilds its exact TargetPlan call identity and complete result storage. */
static bool oracle_dynamic_string_runes_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                                XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index < ctx->operation_count
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_string_runes_is_exact(ctx->semantic, operation, &receiver) ||
        operation->result_value != semantic_value)
        return false;
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(ctx->semantic, operation->result_type);
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target_plan, &call_count);
    const XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (call)
            return false;
        call = &calls[i];
    }
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != operation->result_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    uint32_t extent_count = 0;
    const XrTargetExtentRecord *extents = xr_target_plan_extents(ctx->target_plan, &extent_count);
    const XrTargetExtentRecord *extent =
        layout && layout->extent < extent_count ? &extents[layout->extent] : NULL;
    char first_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char second_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    XrStableId expected_call;
    XrFingerprint digest;
    xr_stable_id_hex(operation->id, first_hex);
    xr_stable_id_hex(result_type ? result_type->id : (XrStableId) {{0}}, second_hex);
    int written =
        snprintf(key, sizeof(key), "xray-target-string-runes-v1:first=%s:second=%s:ordinal=%u",
                 first_hex, second_hex, receiver);
    if (!result_type || !binding || !register_rep || !memory_rep || !slot || !layout || !extent ||
        !call || written <= 0 || (size_t) written >= sizeof(key) ||
        !xr_stable_id_from_key(key, &expected_call, &digest) ||
        !xr_stable_id_equal(call->identity, expected_call) ||
        binding->semantic_value != semantic_value ||
        call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        call->caller_function != operation->function ||
        call->callee_function != XR_SEMANTIC_INDEX_NONE ||
        call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        call->source_export != XR_SEMANTIC_INDEX_NONE ||
        !aot_stable_id_is_zero(call->source_export_identity) ||
        !aot_stable_id_is_zero(call->source_callee_identity) ||
        call->result_value != semantic_value || call->result_slot != binding->slot ||
        call->caller_storage_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE || call->argument_count != 0 ||
        call->adapter_count != 0 || call->flags != 0 ||
        call->result_register_rep != binding->register_rep ||
        call->result_memory_rep != binding->memory_rep ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_RETURN_OWNED ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_STRING_RUNES ||
        call->target_kind != XR_TARGET_CALL_TARGET_STRING_RUNES ||
        call->error_mode != XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL || call->reserved8[0] != 0 ||
        call->reserved8[1] != 0 || call->reserved8[2] != 0 ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        register_rep->memory_size != memory_rep->memory_size ||
        register_rep->memory_align != memory_rep->memory_align ||
        slot->semantic_value != semantic_value || slot->semantic_operation != operation_index ||
        slot->function != operation->function || slot->logical_slot != XR_SEMANTIC_INDEX_NONE ||
        slot->role != XR_TARGET_SLOT_TEMPORARY || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_OWNED || slot->reserved != 0 ||
        slot->debug_variable != XR_SEMANTIC_INDEX_NONE ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC ||
        layout->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE || layout->field_count != 0 ||
        layout->root_field_count != 0 || layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align || extent->id != layout->extent ||
        extent->kind != XR_TARGET_EXTENT_FIXED || extent->operand_count != 0 ||
        extent->alignment != 0 || extent->element_layout != XR_SEMANTIC_INDEX_NONE ||
        extent->stride != 0 || extent->provider != 0 || extent->flags != 0)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* The exact range slice has three independently frozen inputs. Its receiver
 * stays tagged, both bounds stay native i64, and the unique sealed call owns
 * the freshly allocated tagged String result. */
static bool oracle_string_slice_parameter_receiver_storage(const VerifyAuthority *ctx,
                                                           uint32_t semantic_value,
                                                           XrRep *out_storage,
                                                           uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t parameter_index = ctx->parameter_by_value[semantic_value];
    const XrSemanticParameterRecord *parameter =
        parameter_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(ctx->semantic, parameter_index)
            : NULL;
    const XrSemanticTypeRecord *type =
        parameter ? xr_semantic_plan_type(ctx->semantic, parameter->type) : NULL;
    if (!parameter || parameter->value != semantic_value ||
        !xr_semantic_string_slice_string_type_is_exact(type) || parameter->mode != XR_PARAM_READ ||
        (parameter->ownership != XI_OWN_OWNED && parameter->ownership != XI_OWN_BORROWED) ||
        parameter->transfer_mode != XR_TRANSFER_SHARE ||
        parameter->flags != XR_SEM_PARAMETER_REQUIRED || parameter->reserved != 0)
        return false;
    uint8_t ownership = parameter->ownership == XI_OWN_OWNED ? XR_TARGET_OWNERSHIP_OWNED
                                                             : XR_TARGET_OWNERSHIP_BORROWED;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != parameter->type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    if (!binding || !register_rep || !memory_rep || !slot || !layout ||
        binding->semantic_value != semantic_value ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC || register_rep->ownership != ownership ||
        memory_rep->ownership != ownership ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        register_rep->memory_size != memory_rep->memory_size ||
        register_rep->memory_align != memory_rep->memory_align ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || layout->field_count != 0 ||
        layout->root_field_count != 0 || layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align || slot->semantic_value != semantic_value ||
        slot->semantic_operation != XR_SEMANTIC_INDEX_NONE ||
        slot->function != parameter->function || slot->role != XR_TARGET_SLOT_PARAMETER ||
        slot->register_rep != binding->register_rep || slot->memory_rep != binding->memory_rep ||
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC || slot->ownership != ownership)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_string_slice_receiver_storage(const VerifyAuthority *ctx,
                                                 uint32_t semantic_value, XrRep *out_storage,
                                                 uint16_t *out_machine_kind) {
    return oracle_string_slice_parameter_receiver_storage(ctx, semantic_value, out_storage,
                                                          out_machine_kind) ||
           oracle_dynamic_heap_literal_storage(ctx, semantic_value, out_storage, out_machine_kind);
}

static bool oracle_dynamic_string_slice_range_storage(const VerifyAuthority *ctx,
                                                      uint32_t semantic_value, XrRep *out_storage,
                                                      uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index < ctx->operation_count
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    uint32_t start = XR_SEMANTIC_INDEX_NONE;
    uint32_t end = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_string_slice_range_is_exact(ctx->semantic, operation, &receiver, &start,
                                                 &end) ||
        operation->result_value != semantic_value)
        return false;
    XrRep receiver_storage = XR_REP_VOID;
    XrRep start_storage = XR_REP_VOID;
    XrRep end_storage = XR_REP_VOID;
    uint16_t receiver_kind = XR_MACHINE_REP_COUNT;
    uint16_t start_kind = XR_MACHINE_REP_COUNT;
    uint16_t end_kind = XR_MACHINE_REP_COUNT;
    if (!oracle_string_slice_receiver_storage(ctx, receiver, &receiver_storage, &receiver_kind) ||
        receiver_storage != XR_REP_TAGGED || receiver_kind != XR_MACHINE_REP_DYN_VALUE ||
        !oracle_machine_storage(ctx, start, &start_storage, &start_kind) ||
        start_storage != XR_REP_I64 || start_kind != XR_MACHINE_REP_I64 ||
        !oracle_machine_storage(ctx, end, &end_storage, &end_kind) || end_storage != XR_REP_I64 ||
        end_kind != XR_MACHINE_REP_I64)
        return false;
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(ctx->semantic, operation->result_type);
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target_plan, &call_count);
    const XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (call)
            return false;
        call = &calls[i];
    }
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != operation->result_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    uint32_t extent_count = 0;
    const XrTargetExtentRecord *extents = xr_target_plan_extents(ctx->target_plan, &extent_count);
    const XrTargetExtentRecord *extent =
        layout && layout->extent < extent_count ? &extents[layout->extent] : NULL;
    char first_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char second_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char key[208];
    XrStableId expected_call;
    XrFingerprint digest;
    xr_stable_id_hex(operation->id, first_hex);
    xr_stable_id_hex(result_type ? result_type->id : (XrStableId) {{0}}, second_hex);
    int written = snprintf(key, sizeof(key),
                           "xray-target-string-slice-range-v1:first=%s:second=%s:ordinal=%u",
                           first_hex, second_hex, receiver);
    if (!result_type || !binding || !register_rep || !memory_rep || !slot || !layout || !extent ||
        !call || written <= 0 || (size_t) written >= sizeof(key) ||
        !xr_stable_id_from_key(key, &expected_call, &digest) ||
        !xr_stable_id_equal(call->identity, expected_call) ||
        binding->semantic_value != semantic_value ||
        call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        call->caller_function != operation->function ||
        call->callee_function != XR_SEMANTIC_INDEX_NONE ||
        call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        call->source_export != XR_SEMANTIC_INDEX_NONE ||
        !aot_stable_id_is_zero(call->source_export_identity) ||
        !aot_stable_id_is_zero(call->source_callee_identity) ||
        call->result_value != semantic_value || call->result_slot != binding->slot ||
        call->caller_storage_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE || call->argument_count != 0 ||
        call->adapter_count != 0 || call->flags != XR_TARGET_CALL_TAIL ||
        call->result_register_rep != binding->register_rep ||
        call->result_memory_rep != binding->memory_rep ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_RETURN_OWNED ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_STRING_SLICE_RANGE ||
        call->target_kind != XR_TARGET_CALL_TARGET_STRING_SLICE_RANGE ||
        call->error_mode != XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL || call->reserved8[0] != 0 ||
        call->reserved8[1] != 0 || call->reserved8[2] != 0 ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        register_rep->memory_size != memory_rep->memory_size ||
        register_rep->memory_align != memory_rep->memory_align ||
        slot->semantic_value != semantic_value || slot->semantic_operation != operation_index ||
        slot->function != operation->function || slot->logical_slot != XR_SEMANTIC_INDEX_NONE ||
        slot->role != XR_TARGET_SLOT_TEMPORARY || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_OWNED || slot->reserved != 0 ||
        slot->debug_variable != XR_SEMANTIC_INDEX_NONE ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC ||
        layout->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE || layout->field_count != 0 ||
        layout->root_field_count != 0 || layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align || extent->id != layout->extent ||
        extent->kind != XR_TARGET_EXTENT_FIXED || extent->operand_count != 0 ||
        extent->alignment != 0 || extent->element_layout != XR_SEMANTIC_INDEX_NONE ||
        extent->stride != 0 || extent->provider != 0 || extent->flags != 0)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* Iterator<rune>.hasNext is admitted only when its receiver is the exact
 * String.runes result this plan already proved and its scalar result is bound
 * by the unique sealed TargetPlan call row. */
static bool oracle_iterator_rune_has_next_call(const VerifyAuthority *ctx, uint32_t semantic_value,
                                               XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index < ctx->operation_count
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_iterator_rune_has_next_is_exact(ctx->semantic, operation, &receiver) ||
        operation->result_value != semantic_value)
        return false;
    XrRep receiver_storage = XR_REP_TAGGED;
    uint16_t receiver_kind = XR_MACHINE_REP_COUNT;
    if (!oracle_dynamic_string_runes_storage(ctx, receiver, &receiver_storage, &receiver_kind) ||
        receiver_storage != XR_REP_TAGGED || receiver_kind != XR_MACHINE_REP_DYN_VALUE)
        return false;
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(ctx->semantic, operation->result_type);
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target_plan, &call_count);
    const XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (call)
            return false;
        call = &calls[i];
    }
    char first_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char second_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char key[208];
    XrStableId expected_call;
    XrFingerprint digest;
    xr_stable_id_hex(operation->id, first_hex);
    xr_stable_id_hex(result_type ? result_type->id : (XrStableId) {{0}}, second_hex);
    int written = snprintf(key, sizeof(key),
                           "xray-target-iterator-rune-has-next-v1:first=%s:second=%s:ordinal=%u",
                           first_hex, second_hex, receiver);
    if (!result_type || !binding || !call || written <= 0 || (size_t) written >= sizeof(key) ||
        !xr_stable_id_from_key(key, &expected_call, &digest) ||
        !xr_stable_id_equal(call->identity, expected_call) ||
        call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        call->caller_function != operation->function ||
        call->callee_function != XR_SEMANTIC_INDEX_NONE ||
        call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        call->source_export != XR_SEMANTIC_INDEX_NONE ||
        !aot_stable_id_is_zero(call->source_export_identity) ||
        !aot_stable_id_is_zero(call->source_callee_identity) ||
        call->result_value != semantic_value || call->result_slot != binding->slot ||
        call->caller_storage_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE || call->argument_count != 0 ||
        call->adapter_count != 0 || call->flags != 0 ||
        call->result_register_rep != binding->register_rep ||
        call->result_memory_rep != binding->memory_rep ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_HAS_NEXT ||
        call->target_kind != XR_TARGET_CALL_TARGET_ITERATOR_RUNE_HAS_NEXT ||
        call->error_mode != XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL || call->reserved8[0] != 0 ||
        call->reserved8[1] != 0 || call->reserved8[2] != 0)
        return false;
    return oracle_machine_storage(ctx, semantic_value, out_storage, out_machine_kind) &&
           *out_machine_kind == XR_MACHINE_REP_I1;
}

/* Iterator<rune>.next is a native rune only for the exact receiver created by
 * the frozen String.runes operation and the unique sealed TargetPlan call. */
static bool oracle_iterator_rune_next_call(const VerifyAuthority *ctx, uint32_t semantic_value,
                                           XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index < ctx->operation_count
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_iterator_rune_next_is_exact(ctx->semantic, operation, &receiver) ||
        operation->result_value != semantic_value)
        return false;
    XrRep receiver_storage = XR_REP_TAGGED;
    uint16_t receiver_kind = XR_MACHINE_REP_COUNT;
    if (!oracle_dynamic_string_runes_storage(ctx, receiver, &receiver_storage, &receiver_kind) ||
        receiver_storage != XR_REP_TAGGED || receiver_kind != XR_MACHINE_REP_DYN_VALUE)
        return false;
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(ctx->semantic, operation->result_type);
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target_plan, &call_count);
    const XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (call)
            return false;
        call = &calls[i];
    }
    char first_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char second_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char key[208];
    XrStableId expected_call;
    XrFingerprint digest;
    xr_stable_id_hex(operation->id, first_hex);
    xr_stable_id_hex(result_type ? result_type->id : (XrStableId) {{0}}, second_hex);
    int written = snprintf(key, sizeof(key),
                           "xray-target-iterator-rune-next-v1:first=%s:second=%s:ordinal=%u",
                           first_hex, second_hex, receiver);
    if (!result_type || !binding || !call || written <= 0 || (size_t) written >= sizeof(key) ||
        !xr_stable_id_from_key(key, &expected_call, &digest) ||
        !xr_stable_id_equal(call->identity, expected_call) ||
        call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        call->caller_function != operation->function ||
        call->callee_function != XR_SEMANTIC_INDEX_NONE ||
        call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        call->source_export != XR_SEMANTIC_INDEX_NONE ||
        !aot_stable_id_is_zero(call->source_export_identity) ||
        !aot_stable_id_is_zero(call->source_callee_identity) ||
        call->result_value != semantic_value || call->result_slot != binding->slot ||
        call->caller_storage_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE || call->argument_count != 0 ||
        call->adapter_count != 0 || call->flags != 0 ||
        call->result_register_rep != binding->register_rep ||
        call->result_memory_rep != binding->memory_rep ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_NEXT ||
        call->target_kind != XR_TARGET_CALL_TARGET_ITERATOR_RUNE_NEXT ||
        call->error_mode != XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL || call->reserved8[0] != 0 ||
        call->reserved8[1] != 0 || call->reserved8[2] != 0)
        return false;
    return oracle_machine_storage(ctx, semantic_value, out_storage, out_machine_kind) &&
           *out_machine_kind == XR_MACHINE_REP_RUNE;
}

/* Exact rune-to-u32 conversion preserves the native integer lane and is
 * admitted only for the Rune produced by the frozen Iterator<rune>.next row. */
static bool oracle_rune_to_uint32_call(const VerifyAuthority *ctx, uint32_t semantic_value,
                                       XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index < ctx->operation_count
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_rune_to_uint32_is_exact(ctx->semantic, operation, &receiver) ||
        operation->result_value != semantic_value)
        return false;
    XrRep receiver_storage = XR_REP_TAGGED;
    uint16_t receiver_kind = XR_MACHINE_REP_COUNT;
    if (!oracle_iterator_rune_next_call(ctx, receiver, &receiver_storage, &receiver_kind) ||
        receiver_storage != XR_REP_I64 || receiver_kind != XR_MACHINE_REP_RUNE)
        return false;
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(ctx->semantic, operation->result_type);
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target_plan, &call_count);
    const XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (call)
            return false;
        call = &calls[i];
    }
    char first_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char second_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char key[208];
    XrStableId expected_call;
    XrFingerprint digest;
    xr_stable_id_hex(operation->id, first_hex);
    xr_stable_id_hex(result_type ? result_type->id : (XrStableId) {{0}}, second_hex);
    int written =
        snprintf(key, sizeof(key), "xray-target-rune-to-uint32-v1:first=%s:second=%s:ordinal=%u",
                 first_hex, second_hex, receiver);
    if (!result_type || !binding || !call || written <= 0 || (size_t) written >= sizeof(key) ||
        !xr_stable_id_from_key(key, &expected_call, &digest) ||
        !xr_stable_id_equal(call->identity, expected_call) ||
        call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        call->caller_function != operation->function ||
        call->callee_function != XR_SEMANTIC_INDEX_NONE ||
        call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        call->source_export != XR_SEMANTIC_INDEX_NONE ||
        !aot_stable_id_is_zero(call->source_export_identity) ||
        !aot_stable_id_is_zero(call->source_callee_identity) ||
        call->result_value != semantic_value || call->result_slot != binding->slot ||
        call->caller_storage_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE || call->argument_count != 0 ||
        call->adapter_count != 0 || call->flags != 0 ||
        call->result_register_rep != binding->register_rep ||
        call->result_memory_rep != binding->memory_rep ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_RUNE_TO_UINT32 ||
        call->target_kind != XR_TARGET_CALL_TARGET_RUNE_TO_UINT32 ||
        call->error_mode != XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL || call->reserved8[0] != 0 ||
        call->reserved8[1] != 0 || call->reserved8[2] != 0)
        return false;
    return oracle_machine_storage(ctx, semantic_value, out_storage, out_machine_kind) &&
           *out_machine_kind == XR_MACHINE_REP_U32;
}

/* Exact rune whitespace classification produces a native boolean only for
 * the Rune produced by the frozen Iterator<rune>.next row. */
static bool oracle_rune_is_whitespace_call(const VerifyAuthority *ctx, uint32_t semantic_value,
                                           XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index < ctx->operation_count
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_rune_is_whitespace_is_exact(ctx->semantic, operation, &receiver) ||
        operation->result_value != semantic_value)
        return false;
    XrRep receiver_storage = XR_REP_TAGGED;
    uint16_t receiver_kind = XR_MACHINE_REP_COUNT;
    if (!oracle_iterator_rune_next_call(ctx, receiver, &receiver_storage, &receiver_kind) ||
        receiver_storage != XR_REP_I64 || receiver_kind != XR_MACHINE_REP_RUNE)
        return false;
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(ctx->semantic, operation->result_type);
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target_plan, &call_count);
    const XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (call)
            return false;
        call = &calls[i];
    }
    char first_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char second_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char key[208];
    XrStableId expected_call;
    XrFingerprint digest;
    xr_stable_id_hex(operation->id, first_hex);
    xr_stable_id_hex(result_type ? result_type->id : (XrStableId) {{0}}, second_hex);
    int written = snprintf(key, sizeof(key),
                           "xray-target-rune-is-whitespace-v1:first=%s:second=%s:ordinal=%u",
                           first_hex, second_hex, receiver);
    if (!result_type || !binding || !call || written <= 0 || (size_t) written >= sizeof(key) ||
        !xr_stable_id_from_key(key, &expected_call, &digest) ||
        !xr_stable_id_equal(call->identity, expected_call) ||
        call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        call->caller_function != operation->function ||
        call->callee_function != XR_SEMANTIC_INDEX_NONE ||
        call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        call->source_export != XR_SEMANTIC_INDEX_NONE ||
        !aot_stable_id_is_zero(call->source_export_identity) ||
        !aot_stable_id_is_zero(call->source_callee_identity) ||
        call->result_value != semantic_value || call->result_slot != binding->slot ||
        call->caller_storage_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE || call->argument_count != 0 ||
        call->adapter_count != 0 || call->flags != 0 ||
        call->result_register_rep != binding->register_rep ||
        call->result_memory_rep != binding->memory_rep ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_RUNE_IS_WHITESPACE ||
        call->target_kind != XR_TARGET_CALL_TARGET_RUNE_IS_WHITESPACE ||
        call->error_mode != XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL || call->reserved8[0] != 0 ||
        call->reserved8[1] != 0 || call->reserved8[2] != 0)
        return false;
    return oracle_machine_storage(ctx, semantic_value, out_storage, out_machine_kind) &&
           *out_machine_kind == XR_MACHINE_REP_I1;
}

/* The array allocation materializes an owned dynamic value in its own temporary
 * slot. Its storage is tagged because the runtime allocator returns a boxed
 * object; the frozen slot, layout, and ownership rows are rebuilt here rather
 * than read back from the collector. */
static bool oracle_dynamic_array_allocation_storage(const VerifyAuthority *ctx,
                                                    uint32_t semantic_value, XrRep *out_storage,
                                                    uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    uint8_t array_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!operation || operation->result_value != semantic_value ||
        !aot_array_allocation_is_exact(ctx->semantic, operation, &array_storage))
        return false;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    uint32_t layout_index = operation->result_type < ctx->type_count
                                ? ctx->layout_by_type[operation->result_type]
                                : XR_SEMANTIC_INDEX_NONE;
    const XrTargetLayoutRecord *layout =
        layouts && layout_index < layout_count ? &layouts[layout_index] : NULL;
    if (!binding || !register_rep || !memory_rep || !slot || !layout ||
        binding->semantic_value != semantic_value ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        register_rep->memory_size != memory_rep->memory_size ||
        register_rep->memory_align != memory_rep->memory_align ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC ||
        layout->array_element_storage != array_storage || layout->field_count != 0 ||
        layout->root_field_count != 0 || layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align || slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index || slot->function != operation->function ||
        slot->role != XR_TARGET_SLOT_TEMPORARY || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_OWNED)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* The module-level class object. This oracle re-proves the TargetPlan row from
 * the same shared judgement the plan builder and the plan verifier use, so no
 * layer can admit a class object the others would refuse. The storage answer is
 * the outer tagged value, which is the representation Xi selects for the erased
 * reference the allocation produces. */
static bool oracle_dynamic_source_class_object_storage(const VerifyAuthority *ctx,
                                                       uint32_t semantic_value, XrRep *out_storage,
                                                       uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    if (!operation || operation->result_value != semantic_value ||
        !xr_semantic_class_object_is_exact(ctx->semantic, operation))
        return false;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != operation->result_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    if (!binding || !register_rep || !memory_rep || !slot || !layout ||
        binding->semantic_value != semantic_value ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        register_rep->memory_size != memory_rep->memory_size ||
        register_rep->memory_align != memory_rep->memory_align ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || layout->field_count != 0 ||
        layout->root_field_count != 0 || layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align || slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index || slot->function != operation->function ||
        slot->role != XR_TARGET_SLOT_TEMPORARY || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_OWNED)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* The same proof for the three values a source-class construction produces. The
 * ownership is not fixed by the family: the construction owns its instance and
 * both the class object read and the instance read borrow, so the expected
 * ownership is re-derived from the operation's own result ownership rather than
 * read back from the row it is checking. */
static bool oracle_dynamic_source_class_instance_storage(const VerifyAuthority *ctx,
                                                         uint32_t semantic_value,
                                                         XrRep *out_storage,
                                                         uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    if (!operation || operation->result_value != semantic_value ||
        !xr_semantic_class_instance_value_is_exact(ctx->semantic, operation, NULL))
        return false;
    uint8_t ownership = operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED
                            ? XR_TARGET_OWNERSHIP_OWNED
                            : XR_TARGET_OWNERSHIP_BORROWED;
    if (operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED &&
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED)
        return false;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != operation->result_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    if (!binding || !register_rep || !memory_rep || !slot || !layout ||
        binding->semantic_value != semantic_value ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC || register_rep->ownership != ownership ||
        memory_rep->ownership != ownership ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        register_rep->memory_size != memory_rep->memory_size ||
        register_rep->memory_align != memory_rep->memory_align ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || layout->field_count != 0 ||
        layout->root_field_count != 0 || layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align || slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index || slot->function != operation->function ||
        slot->role != XR_TARGET_SLOT_TEMPORARY || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != ownership)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* The same proof for a class instance that crosses a parameter boundary: the
 * receiver a constructor builds, the receiver an instance method borrows, and an
 * ordinary parameter declared with the class as its type. It re-proves the
 * TargetPlan row from the same shared judgement the plan builder and the plan
 * verifier use, so no layer can admit a binding the others would refuse. The
 * value is bound on entry rather than computed, so the row it carries is a
 * parameter slot and its ownership is the parameter's own recorded ownership
 * rather than a property of the family. */
static bool oracle_dynamic_source_class_parameter_storage(const VerifyAuthority *ctx,
                                                          uint32_t semantic_value,
                                                          XrRep *out_storage,
                                                          uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t parameter_index = ctx->parameter_by_value[semantic_value];
    const XrSemanticParameterRecord *parameter =
        parameter_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(ctx->semantic, parameter_index)
            : NULL;
    if (!parameter || parameter->value != semantic_value ||
        xr_semantic_class_instance_parameter_source_class(ctx->semantic, parameter_index) ==
            XR_SEMANTIC_INDEX_NONE)
        return false;
    uint8_t ownership = parameter->ownership == XI_OWN_OWNED ? XR_TARGET_OWNERSHIP_OWNED
                                                             : XR_TARGET_OWNERSHIP_BORROWED;
    if (parameter->ownership != XI_OWN_OWNED && parameter->ownership != XI_OWN_BORROWED)
        return false;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != parameter->type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    if (!binding || !register_rep || !memory_rep || !slot || !layout ||
        binding->semantic_value != semantic_value ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC || register_rep->ownership != ownership ||
        memory_rep->ownership != ownership ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        register_rep->memory_size != memory_rep->memory_size ||
        register_rep->memory_align != memory_rep->memory_align ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || layout->field_count != 0 ||
        layout->root_field_count != 0 || layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align || slot->semantic_value != semantic_value ||
        slot->semantic_operation != XR_SEMANTIC_INDEX_NONE ||
        slot->function != parameter->function || slot->role != XR_TARGET_SLOT_PARAMETER ||
        slot->register_rep != binding->register_rep || slot->memory_rep != binding->memory_rep ||
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC || slot->ownership != ownership)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool aot_u8_slice_type_is_exact(const XrSemanticPlan *semantic, uint32_t type_index,
                                       uint32_t *out_element_type) {
    const XrSemanticTypeRecord *type =
        semantic ? xr_semantic_plan_type(semantic, type_index) : NULL;
    uint32_t child_count = 0;
    const uint32_t *children =
        semantic ? xr_semantic_plan_type_children(semantic, &child_count) : NULL;
    const uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW;
    const uint8_t allowed = required | XR_SEM_TYPE_CONST;
    if (!type || !children || type->kind != XR_KIND_SLICE || type->builtin_type != XR_TID_NULL ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->aggregate_extent != 0 ||
        type->aggregate_align != 0 || type->child_count != 1 || type->child_begin >= child_count ||
        (type->flags & required) != required || (type->flags & ~allowed) != 0)
        return false;
    uint32_t element_type = children[type->child_begin];
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(semantic, element_type);
    if (!element || element->kind != XR_KIND_INT || element->builtin_type != XR_TID_NULL ||
        element->scalar_rep != XR_NATIVE_U8 || element->flags != 0 || element->child_count != 0 ||
        element->aggregate_extent != 0 || element->aggregate_align != 0)
        return false;
    if (out_element_type)
        *out_element_type = element_type;
    return true;
}

/* A borrowed Slice<byte> parameter is the one view value whose identity is
 * fixed before the backend starts: its SemanticPlan parameter row names the
 * function, value, and type, and the TargetPlan binds that exact subject to a
 * VIEW rep, parameter slot, and unique view layout.  Re-prove every storage
 * fact needed by Xi here; a live type or an unbound pointer is never authority. */
static bool oracle_u8_slice_parameter_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                              XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind ||
        (xr_target_plan_completed_family_mask(ctx->target_plan) &
         XR_TARGET_FAMILY_STRING_BYTE_SLICE_VIEW_STORAGE) == 0)
        return false;
    uint32_t parameter_index = ctx->parameter_by_value[semantic_value];
    const XrSemanticParameterRecord *parameter =
        parameter_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(ctx->semantic, parameter_index)
            : NULL;
    if (!parameter || parameter->value != semantic_value ||
        parameter->function >= xr_semantic_plan_function_count(ctx->semantic) ||
        parameter->mode != XR_PARAM_READ || parameter->ownership != XI_OWN_BORROWED ||
        parameter->transfer_mode != XR_TRANSFER_SHARE ||
        (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) != 0 || parameter->reserved != 0 ||
        !aot_u8_slice_type_is_exact(ctx->semantic, parameter->type, NULL))
        return false;

    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != parameter->type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    if (!binding || !register_rep || !memory_rep || !slot || !layout ||
        binding->semantic_value != semantic_value || register_rep->id != binding->register_rep ||
        memory_rep->id != binding->memory_rep || register_rep->kind != XR_MACHINE_REP_VIEW ||
        memory_rep->kind != XR_MACHINE_REP_VIEW || register_rep->register_bits != 128 ||
        memory_rep->register_bits != 128 || register_rep->memory_size != 16 ||
        memory_rep->memory_size != 16 || register_rep->memory_align != 8 ||
        memory_rep->memory_align != 8 || register_rep->signedness != XR_TARGET_SIGN_NONE ||
        memory_rep->signedness != XR_TARGET_SIGN_NONE ||
        register_rep->root_kind != XR_TARGET_ROOT_VIEW_OWNER ||
        memory_rep->root_kind != XR_TARGET_ROOT_VIEW_OWNER ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        register_rep->null_encoding != XR_TARGET_NULL_NOT_NULLABLE ||
        memory_rep->null_encoding != XR_TARGET_NULL_NOT_NULLABLE ||
        register_rep->detail != parameter->type || memory_rep->detail != parameter->type ||
        register_rep->lane_count != 0 || memory_rep->lane_count != 0 ||
        register_rep->reserved != 0 || memory_rep->reserved != 0 ||
        layout->kind != XR_TARGET_LAYOUT_VIEW ||
        layout->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE || layout->align != 8 ||
        layout->fixed_prefix_size != 16 || layout->field_count != 0 ||
        layout->root_field_count != 0 || slot->semantic_value != semantic_value ||
        slot->semantic_operation != XR_SEMANTIC_INDEX_NONE ||
        slot->function != parameter->function || slot->logical_slot != XR_SEMANTIC_INDEX_NONE ||
        slot->size != 16 || slot->align != 8 || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->role != XR_TARGET_SLOT_PARAMETER ||
        slot->root_kind != XR_TARGET_ROOT_VIEW_OWNER ||
        slot->ownership != XR_TARGET_OWNERSHIP_BORROWED || slot->reserved != 0 ||
        slot->debug_variable != XR_SEMANTIC_INDEX_NONE)
        return false;
    for (uint32_t i = 0; i < 4; i++)
        if (register_rep->legal_conversion_mask[i] != 0 ||
            memory_rep->legal_conversion_mask[i] != 0)
            return false;
    *out_storage = XR_REP_PTR;
    *out_machine_kind = XR_MACHINE_REP_VIEW;
    return true;
}

static bool oracle_u8_slice_element_read_is_exact(const VerifyAuthority *ctx,
                                                  uint32_t operation_index) {
    const XrSemanticOperationRecord *operation =
        ctx ? xr_semantic_plan_operation(ctx->semantic, operation_index) : NULL;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        ctx ? xr_semantic_plan_operands(ctx->semantic, &operand_count) : NULL;
    if (!ctx || !operation || !operands || operation->opcode != XI_INDEX_GET ||
        operation->operand_count != 2 || operand_count < 2 ||
        operation->operand_begin > operand_count - 2 || operation->auxiliary_kind != 0 ||
        operation->metadata_count != 0 || operation->semantic_immediate != 0 ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE || operation->allocation_key != NULL ||
        operation->effects != xi_generated_op_effects(XI_INDEX_GET) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_INDEX_GET) ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1)
        return false;
    const XrSemanticOperandRecord *view = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *index = view + 1;
    uint32_t parameter_index = view->value < ctx->value_count ? ctx->parameter_by_value[view->value]
                                                              : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticParameterRecord *parameter =
        parameter_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(ctx->semantic, parameter_index)
            : NULL;
    uint32_t element_type = XR_SEMANTIC_INDEX_NONE;
    XrRep view_storage = XR_REP_TAGGED;
    uint16_t view_kind = XR_MACHINE_REP_COUNT;
    if (!parameter || parameter->function != operation->function || parameter->type != view->type ||
        !aot_u8_slice_type_is_exact(ctx->semantic, view->type, &element_type) ||
        operation->result_type != element_type || view->role != XR_SEM_OPERAND_VALUE ||
        view->parameter != -1 || view->transfer_mode != XR_TRANSFER_SHARE ||
        view->ownership_action != XR_SEM_OPERAND_BORROW || view->parameter_mode != XR_PARAM_READ ||
        view->access != XR_CALL_ARG_PLAIN || view->origin != XI_PLACE_ORIGIN_NONE ||
        view->lifetime != XI_PLACE_LIFETIME_NONE || view->escape != XI_PLACE_ESCAPE_NONE ||
        view->flags != 0 || index->role != XR_SEM_OPERAND_VALUE || index->parameter != -1 ||
        index->transfer_mode != XR_TRANSFER_SHARE ||
        index->ownership_action != XR_SEM_OPERAND_BORROW ||
        index->parameter_mode != XR_PARAM_READ || index->access != XR_CALL_ARG_PLAIN ||
        index->origin != XI_PLACE_ORIGIN_NONE || index->lifetime != XI_PLACE_LIFETIME_NONE ||
        index->escape != XI_PLACE_ESCAPE_NONE || index->flags != 0 ||
        !semantic_exact_i64_type(xr_semantic_plan_type(ctx->semantic, index->type)) ||
        !oracle_u8_slice_parameter_storage(ctx, view->value, &view_storage, &view_kind))
        return false;
    return view_storage == XR_REP_PTR && view_kind == XR_MACHINE_REP_VIEW;
}

static bool oracle_dynamic_array_ref_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                             XrRep *out_storage, uint16_t *out_machine_kind);

static bool oracle_direct_local_array_ref_parameter_place_storage(const VerifyAuthority *ctx,
                                                                  uint32_t semantic_value,
                                                                  XrRep *out_storage,
                                                                  uint16_t *out_machine_kind);

/* An element read or write is admitted only against a container this authority
 * already proved to be an exact array allocation. The index is an exact signed
 * 64-bit integer, the stored element matches the container's own element entry,
 * and the container is borrowed for the duration of the access, so the access
 * moves no ownership. */
static bool oracle_array_element_access_is_exact(const VerifyAuthority *ctx,
                                                 uint32_t operation_index) {
    const XrSemanticOperationRecord *operation =
        ctx ? xr_semantic_plan_operation(ctx->semantic, operation_index) : NULL;
    uint32_t operand_count = 0, child_count = 0;
    const XrSemanticOperandRecord *operands =
        ctx ? xr_semantic_plan_operands(ctx->semantic, &operand_count) : NULL;
    const uint32_t *children =
        ctx ? xr_semantic_plan_type_children(ctx->semantic, &child_count) : NULL;
    if (!ctx || !operation || !operands || !children ||
        (operation->opcode != XI_INDEX_GET && operation->opcode != XI_INDEX_SET))
        return false;
    uint16_t expected_operands = operation->opcode == XI_INDEX_GET ? 2u : 3u;
    if (operation->operand_count != expected_operands || operand_count < expected_operands ||
        operation->operand_begin > operand_count - expected_operands ||
        operation->auxiliary_kind != 0 || operation->metadata_count != 0 ||
        /* Array.get/set's canonical lowering records the unchecked member
         * contract in bit zero; an explicit unsafe index uses the independent
         * XI_ACCESS_UNCHECKED bit.  Neither bit selects this storage family:
         * the exact Array type, operands, Target value row and ownership do. */
        (operation->semantic_immediate & ~((int64_t) XI_ACCESS_UNCHECKED | INT64_C(1))) != 0 ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE || operation->allocation_key != NULL ||
        operation->effects != xi_generated_op_effects(operation->opcode) ||
        operation->result_ownership != xi_generated_op_result_ownership(operation->opcode) ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1)
        return false;
    const XrSemanticOperandRecord *container = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *index = container + 1;
    XrRep container_storage = XR_REP_TAGGED;
    uint16_t container_kind = XR_MACHINE_REP_COUNT;
    /* An owned receiver is one an operation in this function produced, so the
     * operation that produced it is the allocation the checks below compare
     * against. A borrowed receiver names an allocation someone else owns, plus
     * the cell a ref parameter points at; none of them has a producing
     * operation here, so the type is proved from the receiver's own row. The
     * two halves are the shared carrier list split along that one question,
     * not a receiver list of this site's own. */
    bool owned_array = oracle_array_produced_tagged_carrier_storage(
        ctx, container->value, &container_storage, &container_kind);
    bool borrowed_array =
        !owned_array && (oracle_direct_local_array_ref_parameter_place_storage(
                             ctx, container->value, &container_storage, &container_kind) ||
                         oracle_array_borrowed_tagged_carrier_storage(
                             ctx, container->value, &container_storage, &container_kind));
    if (container->role != XR_SEM_OPERAND_VALUE || container->parameter != -1 ||
        container->flags != 0 || container->ownership_action != XR_SEM_OPERAND_BORROW ||
        index->role != XR_SEM_OPERAND_VALUE || index->parameter != -1 || index->flags != 0 ||
        !semantic_exact_i64_type(xr_semantic_plan_type(ctx->semantic, index->type)) ||
        container->value >= ctx->value_count || (!owned_array && !borrowed_array))
        return false;
    const XrSemanticTypeRecord *container_type =
        xr_semantic_plan_type(ctx->semantic, container->type);
    const XrSemanticOperationRecord *allocation =
        owned_array
            ? xr_semantic_plan_operation(ctx->semantic, ctx->operation_by_value[container->value])
            : NULL;
    uint8_t borrowed_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!container_type ||
        (owned_array && (!allocation || container->type != allocation->result_type)) ||
        (borrowed_array &&
         !aot_array_type_is_exact(ctx->semantic, container->type, true, &borrowed_storage)) ||
        container_type->child_begin >= child_count)
        return false;
    uint32_t element_type = children[container_type->child_begin];
    if (operation->opcode == XI_INDEX_GET)
        return operation->result_type == element_type;
    const XrSemanticOperandRecord *element = index + 1;
    return element->role == XR_SEM_OPERAND_VALUE && element->parameter == -1 &&
           element->flags == 0 && element->type == element_type &&
           element->ownership_action == XR_SEM_OPERAND_CONSUME;
}

/* A nullable scalar lives in the tagged carrier at every point of its life: the
 * definition, every use, and the return boundary all name the same storage, so
 * this value never asks for a representation adapter. The frozen rep, slot, and
 * layout rows are rebuilt here rather than read back from the collector. The
 * value may be a parameter or an operation result; both carry the same fact. */
static bool oracle_nullable_scalar_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                           XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    uint32_t parameter_index = ctx->parameter_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    const XrSemanticParameterRecord *parameter =
        parameter_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(ctx->semantic, parameter_index)
            : NULL;
    if ((!parameter && !operation) || (!parameter && operation->result_value != semantic_value))
        return false;
    uint32_t type_index = parameter ? parameter->type : operation->result_type;
    if (!aot_nullable_scalar_type_is_exact(xr_semantic_plan_type(ctx->semantic, type_index)))
        return false;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != type_index)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    if (!binding || !register_rep || !memory_rep || !slot || !layout ||
        binding->semantic_value != semantic_value ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        register_rep->memory_size != memory_rep->memory_size ||
        register_rep->memory_align != memory_rep->memory_align ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || layout->field_count != 0 ||
        layout->root_field_count != 0 || layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align || slot->semantic_value != semantic_value ||
        slot->semantic_operation != (parameter ? XR_SEMANTIC_INDEX_NONE : operation_index) ||
        slot->function != (parameter ? parameter->function : operation->function) ||
        slot->role != (parameter ? XR_TARGET_SLOT_PARAMETER
                                 : (operation->opcode == XI_PHI ? XR_TARGET_SLOT_PHI
                                                                : XR_TARGET_SLOT_TEMPORARY)) ||
        slot->register_rep != binding->register_rep || slot->memory_rep != binding->memory_rep ||
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_BORROWED)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* The frozen rows a String held in its own temporary slot must carry: the
 * tagged carrier in both the register and the memory position, a dynamic layout
 * of exactly that geometry, and a dynamic temporary slot naming this value and
 * this operation. A String has one carrier however it was produced, so its
 * producers -- a concatenation, a direct-local result, and a read of the shared
 * cell it was bound to -- differ only in whether that carrier owns its
 * allocation, and the rows they must agree with are stated once here rather
 * than once per producer. */
static bool tagged_value_temporary_rows_are_exact(const VerifyAuthority *ctx,
                                                  uint32_t semantic_value,
                                                  const XrSemanticOperationRecord *operation,
                                                  uint32_t operation_index, uint8_t ownership) {
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != operation->result_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    return binding && register_rep && memory_rep && slot && layout &&
           register_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
           memory_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
           register_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
           memory_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
           register_rep->ownership == ownership && memory_rep->ownership == ownership &&
           register_rep->null_encoding == XR_TARGET_NULL_TAGGED &&
           memory_rep->null_encoding == XR_TARGET_NULL_TAGGED &&
           register_rep->memory_size == memory_rep->memory_size &&
           register_rep->memory_align == memory_rep->memory_align &&
           layout->kind == XR_TARGET_LAYOUT_DYNAMIC && layout->field_count == 0 &&
           layout->root_field_count == 0 && layout->fixed_prefix_size == memory_rep->memory_size &&
           layout->align == memory_rep->memory_align && slot->semantic_value == semantic_value &&
           slot->semantic_operation == operation_index && slot->function == operation->function &&
           slot->role == XR_TARGET_SLOT_TEMPORARY && slot->register_rep == binding->register_rep &&
           slot->memory_rep == binding->memory_rep && slot->root_kind == XR_TARGET_ROOT_DYNAMIC &&
           slot->ownership == ownership;
}

static bool oracle_dynamic_direct_local_string_result_storage(const VerifyAuthority *ctx,
                                                              uint32_t semantic_value,
                                                              XrRep *out_storage,
                                                              uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    if (!operation || operation->result_value != semantic_value ||
        !aot_direct_local_string_result_is_exact(ctx->semantic, operation_index) ||
        !tagged_value_temporary_rows_are_exact(ctx, semantic_value, operation, operation_index,
                                               XR_TARGET_OWNERSHIP_OWNED))
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* A String read back out of the shared cell it was bound to. The read borrows
 * the cell's allocation, so it lands in the one tagged carrier a String has,
 * holding it borrowed rather than owned. */
static bool oracle_dynamic_string_shared_read_storage(const VerifyAuthority *ctx,
                                                      uint32_t semantic_value, XrRep *out_storage,
                                                      uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    if (!operation || operation->result_value != semantic_value ||
        !xr_semantic_tagged_string_shared_read_is_exact(ctx->semantic, operation) ||
        !tagged_value_temporary_rows_are_exact(ctx, semantic_value, operation, operation_index,
                                               XR_TARGET_OWNERSHIP_BORROWED))
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* The String a concatenation allocates materializes an owned dynamic value in
 * its own temporary slot. Its storage is tagged because the runtime joiner
 * returns a boxed object; the frozen slot, layout and ownership rows are
 * rebuilt here rather than read back from the collector. */
static bool oracle_dynamic_string_concat_result_storage(const VerifyAuthority *ctx,
                                                        uint32_t semantic_value, XrRep *out_storage,
                                                        uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    if (!operation || operation->result_value != semantic_value ||
        !xr_semantic_string_concat_is_exact(ctx->semantic, operation) ||
        !tagged_value_temporary_rows_are_exact(ctx, semantic_value, operation, operation_index,
                                               XR_TARGET_OWNERSHIP_OWNED))
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* The String a `string(x)` conversion allocates materializes an owned dynamic
 * value in its own temporary slot, the same rows a concatenation result carries
 * and for the same reason: the runtime display helper returns a boxed object.
 * The scalar it converts keeps whatever native storage its own definition
 * named; C emission adapts it at the call. */
static bool oracle_dynamic_string_convert_result_storage(const VerifyAuthority *ctx,
                                                         uint32_t semantic_value,
                                                         XrRep *out_storage,
                                                         uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    if (!operation || operation->result_value != semantic_value ||
        !xr_semantic_string_convert_is_exact(ctx->semantic, operation) ||
        !tagged_value_temporary_rows_are_exact(ctx, semantic_value, operation, operation_index,
                                               XR_TARGET_OWNERSHIP_OWNED))
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* A length read borrows one container and yields the plain machine integer the
 * language types it. The receiver keeps the single tagged storage fact its own
 * family already proved: the owned array allocation, the string literal, or the
 * owned String a direct-local call returned. A container without such a row, a
 * consumed receiver, or a result that is not the exact signed 64-bit integer
 * leaves the read without authority rather than falling back to a guess. */
static bool oracle_length_read_is_exact(const VerifyAuthority *ctx, uint32_t operation_index) {
    const XrSemanticOperationRecord *operation =
        ctx ? xr_semantic_plan_operation(ctx->semantic, operation_index) : NULL;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        ctx ? xr_semantic_plan_operands(ctx->semantic, &operand_count) : NULL;
    if (!ctx || !operation || !operands || operation->opcode != XI_LEN ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->auxiliary_kind != 0 || operation->metadata_count != 0 ||
        operation->semantic_immediate != 0 || operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE || operation->allocation_key != NULL ||
        operation->effects != xi_generated_op_effects(XI_LEN) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_LEN) ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        !semantic_exact_i64_type(xr_semantic_plan_type(ctx->semantic, operation->result_type)))
        return false;
    const XrSemanticOperandRecord *container = &operands[operation->operand_begin];
    XrRep container_storage = XR_REP_TAGGED;
    uint16_t container_kind = XR_MACHINE_REP_COUNT;
    return container->role == XR_SEM_OPERAND_VALUE && container->parameter == -1 &&
           container->flags == 0 && container->ownership_action == XR_SEM_OPERAND_BORROW &&
           container->value < ctx->value_count &&
           (oracle_array_tagged_carrier_storage(ctx, container->value, &container_storage,
                                                &container_kind) ||
            oracle_string_tagged_carrier_storage(ctx, container->value, &container_storage,
                                                 &container_kind));
}

/* One equality over two proved String values. String is immutable and shared,
 * so its single storage fact is the outer tagged value and both sides of the
 * comparison stay in that carrier; the truth value the comparison writes keeps
 * the machine storage its own definition already names. Both sides must be a
 * String this authority already proved, both must be borrowed, and the result
 * must be the plain boolean the language types a comparison. Ordering
 * relations are not admitted here: only equality and inequality carry a
 * storage fact this authority states for a reference operand. */
static bool oracle_string_equality_is_exact(const VerifyAuthority *ctx, uint32_t operation_index) {
    const XrSemanticOperationRecord *operation =
        ctx ? xr_semantic_plan_operation(ctx->semantic, operation_index) : NULL;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        ctx ? xr_semantic_plan_operands(ctx->semantic, &operand_count) : NULL;
    const XrSemanticTypeRecord *result_type =
        operation ? xr_semantic_plan_type(ctx->semantic, operation->result_type) : NULL;
    if (!ctx || !operation || !operands || !result_type ||
        (operation->opcode != XI_EQ && operation->opcode != XI_NE) ||
        operation->operand_count != 2 || operand_count < 2u ||
        operation->operand_begin > operand_count - 2u || operation->auxiliary_kind != 0 ||
        operation->metadata_count != 0 || operation->semantic_immediate != 0 ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE || operation->allocation_key != NULL ||
        operation->effects != xi_generated_op_effects(operation->opcode) ||
        operation->result_ownership != xi_generated_op_result_ownership(operation->opcode) ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE || result_type->kind != XR_KIND_BOOL ||
        result_type->builtin_type != XR_TID_NULL || result_type->child_count != 0 ||
        result_type->aggregate_extent != 0 || result_type->aggregate_align != 0 ||
        result_type->scalar_rep != XR_SCALAR_REP_NONE || result_type->flags != 0)
        return false;
    for (uint16_t i = 0; i < 2u; i++) {
        const XrSemanticOperandRecord *side = &operands[operation->operand_begin + i];
        XrRep side_storage = XR_REP_TAGGED;
        uint16_t side_kind = XR_MACHINE_REP_COUNT;
        if (side->role != XR_SEM_OPERAND_VALUE || side->parameter != -1 || side->flags != 0 ||
            side->ownership_action != XR_SEM_OPERAND_BORROW || side->value >= ctx->value_count ||
            !oracle_string_tagged_carrier_storage(ctx, side->value, &side_storage, &side_kind))
            return false;
    }
    return true;
}

/* The JSON namespace call materializes an owned dynamic value in its own
 * temporary slot.  Its storage is tagged because the runtime encoder returns a
 * boxed value; the call row proves the slot, layout, and ownership. */
static bool oracle_dynamic_json_namespace_value_storage(const VerifyAuthority *ctx,
                                                        uint32_t semantic_value, XrRep *out_storage,
                                                        uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index < ctx->operation_count
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    uint32_t argument_value = XR_SEMANTIC_INDEX_NONE;
    if (!aot_json_namespace_value_is_exact(ctx->semantic, operation, &argument_value))
        return false;
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(ctx->semantic, operation->result_type);
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target_plan, &call_count);
    const XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (call)
            return false;
        call = &calls[i];
    }
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != operation->result_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    char first_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char second_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    XrStableId expected_call;
    XrFingerprint digest;
    xr_stable_id_hex(operation->id, first_hex);
    xr_stable_id_hex(result_type ? result_type->id : (XrStableId) {{0}}, second_hex);
    int written = snprintf(key, sizeof(key),
                           "xray-target-json-namespace-value-v1:first=%s:second=%s:ordinal=%u",
                           first_hex, second_hex, argument_value);
    if (!result_type || !binding || !register_rep || !memory_rep || !slot || !layout || !call ||
        written <= 0 || (size_t) written >= sizeof(key) ||
        !xr_stable_id_from_key(key, &expected_call, &digest) ||
        !xr_stable_id_equal(call->identity, expected_call) ||
        call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        call->caller_function != operation->function ||
        call->callee_function != XR_SEMANTIC_INDEX_NONE || call->result_value != semantic_value ||
        call->result_slot != binding->slot || call->argument_count != 0 || call->flags != 0 ||
        call->result_ownership != XR_TARGET_CALL_RETURN_OWNED ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_JSON_NAMESPACE_VALUE ||
        call->target_kind != XR_TARGET_CALL_TARGET_JSON_NAMESPACE_VALUE ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        slot->semantic_value != semantic_value || slot->semantic_operation != operation_index ||
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC || slot->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC ||
        layout->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_dynamic_stringbuilder_storage(const VerifyAuthority *ctx,
                                                 uint32_t semantic_value, XrRep *out_storage,
                                                 uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index < ctx->operation_count
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target_plan, &call_count);
    const XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (call)
            return false;
        call = &calls[i];
    }
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; operation && i < layout_count; i++) {
        if (layouts[i].semantic_type != operation->result_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    char first_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char second_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    XrStableId expected_call;
    XrFingerprint digest;
    xr_stable_id_hex(operation ? operation->id : (XrStableId) {{0}}, first_hex);
    xr_stable_id_hex(operation ? operation->allocation_id : (XrStableId) {{0}}, second_hex);
    int written = snprintf(key, sizeof(key),
                           "xray-target-stringbuilder-constructor-v1:first=%s:second=%s:ordinal=0",
                           first_hex, second_hex);
    if (!operation || !binding || !register_rep || !memory_rep || !slot || !layout || !call ||
        !aot_stringbuilder_constructor_is_exact(ctx->semantic, operation) || written <= 0 ||
        (size_t) written >= sizeof(key) || !xr_stable_id_from_key(key, &expected_call, &digest) ||
        !xr_stable_id_equal(call->identity, expected_call) ||
        call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        call->caller_function != operation->function ||
        call->callee_function != XR_SEMANTIC_INDEX_NONE || call->result_value != semantic_value ||
        call->result_slot != binding->slot || call->argument_count != 0 || call->flags != 0 ||
        call->result_ownership != XR_TARGET_CALL_RETURN_OWNED ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_STRINGBUILDER_CONSTRUCTOR ||
        call->target_kind != XR_TARGET_CALL_TARGET_STRINGBUILDER_CONSTRUCTOR ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        slot->semantic_value != semantic_value || slot->semantic_operation != operation_index ||
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC || slot->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC ||
        layout->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_dynamic_array_intrinsic_storage(const VerifyAuthority *ctx,
                                                   uint32_t semantic_value, XrRep *out_storage,
                                                   uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index < ctx->operation_count
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    uint8_t kind = XR_TARGET_ARRAY_INTRINSIC_NONE;
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!aot_array_intrinsic_is_exact(ctx->semantic, operation, &kind, &storage))
        return false;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target_plan, &call_count);
    const XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (call)
            return false;
        call = &calls[i];
    }
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; operation && i < layout_count; i++) {
        if (layouts[i].semantic_type != operation->result_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    uint32_t operand_count = 0, argument_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(ctx->target_plan, &argument_count);
    XrStableId expected_call;
    uint32_t discriminator = ((uint32_t) kind << 8) | storage;
    bool expected_call_valid =
        aot_pair_identity("xray-target-array-intrinsic-v1", operation->id, operation->allocation_id,
                          discriminator, &expected_call);
    if (!operation || !binding || !register_rep || !memory_rep || !slot || !layout || !call ||
        !operands || !arguments || !expected_call_valid ||
        !xr_stable_id_equal(call->identity, expected_call) ||
        call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        call->caller_function != operation->function ||
        call->callee_function != XR_SEMANTIC_INDEX_NONE ||
        call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        call->source_export != XR_SEMANTIC_INDEX_NONE ||
        !aot_stable_id_is_zero(call->source_export_identity) ||
        !aot_stable_id_is_zero(call->source_callee_identity) ||
        call->result_value != semantic_value || call->result_slot != binding->slot ||
        call->result_register_rep != binding->register_rep ||
        call->result_memory_rep != binding->memory_rep ||
        call->caller_storage_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE ||
        call->argument_count != operation->operand_count || call->argument_begin > argument_count ||
        call->argument_count > argument_count - call->argument_begin || call->adapter_count != 0 ||
        call->flags != 0 || call->calling_convention != XR_TARGET_CALL_CONVENTION_ARRAY_INTRINSIC ||
        call->target_kind != XR_TARGET_CALL_TARGET_ARRAY_INTRINSIC ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_RETURN_OWNED ||
        call->array_intrinsic_kind != kind || call->array_element_storage != storage ||
        call->reserved8[0] != 0 || call->reserved8[1] != 0 || call->reserved8[2] != 0 ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        slot->semantic_value != semantic_value || slot->semantic_operation != operation_index ||
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC || slot->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || layout->array_element_storage != storage)
        return false;
    for (uint16_t ordinal = 0; ordinal < call->argument_count; ordinal++) {
        uint32_t semantic_operand = operation->operand_begin + ordinal;
        const XrSemanticOperandRecord *operand = &operands[semantic_operand];
        const XrSemanticTypeRecord *type = xr_semantic_plan_type(ctx->semantic, operand->type);
        const XrTargetValueRepRecord *caller =
            xr_target_plan_value_rep(ctx->target_plan, operand->value);
        const XrTargetCallArgumentRecord *argument = &arguments[call->argument_begin + ordinal];
        XrStableId expected_argument;
        if (!type || !caller ||
            !aot_pair_identity("xray-target-array-intrinsic-argument-v1", operation->id, type->id,
                               ordinal, &expected_argument) ||
            !xr_stable_id_equal(argument->identity, expected_argument) ||
            argument->call != call->id || argument->semantic_operand != semantic_operand ||
            argument->semantic_value != operand->value ||
            argument->callee_parameter != XR_SEMANTIC_INDEX_NONE ||
            argument->caller_slot != caller->slot ||
            argument->callee_slot != XR_SEMANTIC_INDEX_NONE ||
            argument->register_rep != caller->register_rep ||
            argument->memory_rep != caller->memory_rep ||
            argument->callee_register_rep != caller->register_rep ||
            argument->callee_memory_rep != caller->memory_rep || argument->ordinal != ordinal ||
            argument->mode != XR_TARGET_CALL_VALUE ||
            argument->ownership != XR_TARGET_CALL_CONSUME ||
            argument->transfer_mode != operand->transfer_mode || argument->flags != 0)
            return false;
    }
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_direct_local_array_ref_place_storage(const VerifyAuthority *ctx,
                                                        uint32_t semantic_value, XrRep *out_storage,
                                                        uint16_t *out_machine_kind);

static bool oracle_direct_local_array_ref_parameter_place_storage(const VerifyAuthority *ctx,
                                                                  uint32_t semantic_value,
                                                                  XrRep *out_storage,
                                                                  uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t parameter_index = ctx->parameter_by_value[semantic_value];
    const XrSemanticParameterRecord *parameter =
        parameter_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(ctx->semantic, parameter_index)
            : NULL;
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    if (!parameter || !binding || !register_rep || !memory_rep || !slot ||
        !aot_array_ref_parameter_is_exact(ctx->semantic, parameter, &storage) ||
        parameter->value != semantic_value || register_rep->kind != XR_MACHINE_REP_RAW_PTR ||
        memory_rep->kind != XR_MACHINE_REP_RAW_PTR ||
        register_rep->root_kind != XR_TARGET_ROOT_NONE ||
        memory_rep->root_kind != XR_TARGET_ROOT_NONE ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        slot->semantic_value != semantic_value ||
        slot->semantic_operation != XR_SEMANTIC_INDEX_NONE ||
        slot->function != parameter->function || slot->role != XR_TARGET_SLOT_PARAMETER ||
        slot->register_rep != binding->register_rep || slot->memory_rep != binding->memory_rep ||
        slot->root_kind != XR_TARGET_ROOT_NONE || slot->ownership != XR_TARGET_OWNERSHIP_BORROWED)
        return false;
    *out_storage = XR_REP_RAWPTR;
    *out_machine_kind = XR_MACHINE_REP_RAW_PTR;
    return true;
}

/* The frozen rows a container parameter taken by value must carry: the tagged
 * carrier in both the register and the memory position, and a dynamic parameter
 * slot that names this value and no operation. An Array and a String differ
 * only in which declaration shape they accept, so the rows they must agree with
 * are stated once here rather than twice, and `callee_ownership` is the one
 * thing that varies: an Array by value is always borrowed, while a String
 * parameter carries whichever ownership its declaration proved. */
static bool tagged_value_parameter_rows_are_exact(const VerifyAuthority *ctx,
                                                  uint32_t semantic_value,
                                                  const XrSemanticParameterRecord *parameter,
                                                  uint8_t callee_ownership) {
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    return parameter && binding && register_rep && memory_rep && slot &&
           parameter->value == semantic_value && register_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
           memory_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
           register_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
           memory_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
           register_rep->ownership == callee_ownership &&
           memory_rep->ownership == callee_ownership &&
           register_rep->null_encoding == XR_TARGET_NULL_TAGGED &&
           memory_rep->null_encoding == XR_TARGET_NULL_TAGGED &&
           slot->semantic_value == semantic_value &&
           slot->semantic_operation == XR_SEMANTIC_INDEX_NONE &&
           slot->function == parameter->function && slot->role == XR_TARGET_SLOT_PARAMETER &&
           slot->register_rep == binding->register_rep && slot->memory_rep == binding->memory_rep &&
           slot->root_kind == XR_TARGET_ROOT_DYNAMIC && slot->ownership == callee_ownership;
}

/* An `Array<T>` parameter handed over by value. It borrows the caller's
 * allocation for the extent of the call, so it is bound to the same borrowed
 * tagged carrier a shared read of that array gets rather than to the pointer a
 * ref parameter needs. */
static bool oracle_direct_local_array_value_parameter_storage(const VerifyAuthority *ctx,
                                                              uint32_t semantic_value,
                                                              XrRep *out_storage,
                                                              uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t parameter_index = ctx->parameter_by_value[semantic_value];
    const XrSemanticParameterRecord *parameter =
        parameter_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(ctx->semantic, parameter_index)
            : NULL;
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!aot_array_value_parameter_is_exact(ctx->semantic, parameter, &storage) ||
        !tagged_value_parameter_rows_are_exact(ctx, semantic_value, parameter,
                                               XR_TARGET_OWNERSHIP_BORROWED))
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* A String parameter handed over by value. A String has no carrier other than
 * the outer tagged value, so it binds exactly the rows an Array by value binds,
 * under whichever ownership the declaration proved. */
static bool oracle_direct_local_string_value_parameter_storage(const VerifyAuthority *ctx,
                                                               uint32_t semantic_value,
                                                               XrRep *out_storage,
                                                               uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t parameter_index = ctx->parameter_by_value[semantic_value];
    const XrSemanticParameterRecord *parameter =
        parameter_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(ctx->semantic, parameter_index)
            : NULL;
    bool callee_owns = false;
    if (!xr_semantic_direct_local_string_value_parameter_is_exact(ctx->semantic, parameter,
                                                                  &callee_owns) ||
        !tagged_value_parameter_rows_are_exact(ctx, semantic_value, parameter,
                                               callee_owns ? XR_TARGET_OWNERSHIP_OWNED
                                                           : XR_TARGET_OWNERSHIP_BORROWED))
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* The `Array<T>` a direct-local call hands back. The transfer lands in a
 * temporary the caller owns, so the row it is rebuilt against is the owned
 * dynamic one an owned String result gets. */
static bool oracle_dynamic_direct_local_array_result_storage(const VerifyAuthority *ctx,
                                                             uint32_t semantic_value,
                                                             XrRep *out_storage,
                                                             uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    if (!operation || operation->result_value != semantic_value ||
        !aot_direct_local_array_result_is_exact(ctx->semantic, operation_index))
        return false;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != operation->result_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    if (!binding || !register_rep || !memory_rep || !slot || !layout ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        register_rep->memory_size != memory_rep->memory_size ||
        register_rep->memory_align != memory_rep->memory_align ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index || slot->function != operation->function ||
        slot->role != XR_TARGET_SLOT_TEMPORARY || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_OWNED)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_dynamic_array_ref_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                             XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    bool shared_value = aot_array_shared_value_is_exact(ctx->semantic, operation, &storage) &&
                        operation->result_value == semantic_value;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    const XrSemanticOperandRecord *place = operation && operands && operation->operand_count == 1 &&
                                                   operation->operand_begin < operand_count
                                               ? &operands[operation->operand_begin]
                                               : NULL;
    XrRep place_storage = XR_REP_TAGGED;
    uint16_t place_kind = XR_MACHINE_REP_COUNT;
    bool place_load_value =
        !shared_value && operation && place && operation->opcode == XI_PLACE_LOAD &&
        operation->result_value == semantic_value &&
        operation->effects == xi_generated_op_effects(XI_PLACE_LOAD) &&
        operation->flags == xi_generated_op_default_flags(XI_PLACE_LOAD) &&
        operation->ownership_use == xi_generated_op_own_use(XI_PLACE_LOAD) &&
        operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
        operation->result_alias_operand == -1 &&
        operation->intrinsic_kind == XR_SEM_INTRINSIC_NONE && operation->metadata_count == 0 &&
        operation->auxiliary_kind == XI_AUX_KIND_NONE && operation->semantic_immediate == 0 &&
        operation->constant == XR_SEMANTIC_INDEX_NONE &&
        operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
        operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
        place->type == operation->result_type && place->role == XR_SEM_OPERAND_VALUE &&
        place->parameter == -1 && place->parameter_mode == XR_PARAM_READ &&
        place->access == XR_CALL_ARG_PLAIN && place->origin == XI_PLACE_ORIGIN_NONE &&
        place->lifetime == XI_PLACE_LIFETIME_NONE && place->escape == XI_PLACE_ESCAPE_NONE &&
        place->flags == 0 && place->ownership_action == XR_SEM_OPERAND_BORROW &&
        aot_array_type_is_exact(ctx->semantic, operation->result_type, true, &storage) &&
        (oracle_direct_local_array_ref_place_storage(ctx, place->value, &place_storage,
                                                     &place_kind) ||
         oracle_direct_local_array_ref_parameter_place_storage(ctx, place->value, &place_storage,
                                                               &place_kind)) &&
        place_storage == XR_REP_RAWPTR && place_kind == XR_MACHINE_REP_RAW_PTR;
    if (!shared_value && !place_load_value)
        return false;
    uint32_t semantic_type = operation->result_type;
    uint32_t semantic_function = operation->function;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != semantic_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    if (!binding || !register_rep || !memory_rep || !slot || !layout ||
        binding->semantic_value != semantic_value ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        register_rep->memory_size != memory_rep->memory_size ||
        register_rep->memory_align != memory_rep->memory_align ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || layout->array_element_storage != storage ||
        layout->field_count != 0 || layout->root_field_count != 0 ||
        layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align || slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index || slot->function != semantic_function ||
        slot->role != XR_TARGET_SLOT_TEMPORARY || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_BORROWED)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_static_direct_local_callee_storage(const VerifyAuthority *ctx,
                                                      uint32_t semantic_value, XrRep *out_storage,
                                                      uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind ||
        !ctx->exact_direct_callee_value || !ctx->exact_direct_callee_value[semantic_value])
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index == XR_SEMANTIC_INDEX_NONE
            ? NULL
            : xr_semantic_plan_operation(ctx->semantic, operation_index);
    uint32_t target_function = ctx->direct_callee_target_by_value[semantic_value];
    if (!operation || operation->result_value != semantic_value ||
        !aot_direct_local_callee_type_is_exact(ctx->semantic, operation, target_function))
        return false;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != operation->result_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    if (!binding || !register_rep || !memory_rep || !slot || !layout ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        register_rep->memory_size != memory_rep->memory_size ||
        register_rep->memory_align != memory_rep->memory_align ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || layout->field_count != 0 ||
        layout->root_field_count != 0 || layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align || slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index || slot->function != operation->function ||
        slot->role != XR_TARGET_SLOT_TEMPORARY || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_BORROWED)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* The native stdlib namespace handle is a borrowed compiler-owned reference:
 * it is loaded from a module shared slot and never adapted to a native
 * representation, so its storage stays the borrowed tagged value. */
static bool oracle_native_module_namespace_storage(const VerifyAuthority *ctx,
                                                   uint32_t semantic_value, XrRep *out_storage,
                                                   uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind ||
        !ctx->exact_native_module_namespace_value ||
        !ctx->exact_native_module_namespace_value[semantic_value])
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index < ctx->operation_count
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (!operation || layouts[i].semantic_type != operation->result_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    if (!operation || !binding || !register_rep || !memory_rep || !slot || !layout ||
        !aot_native_module_namespace_value_is_exact(ctx->semantic, operation) ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || layout->field_count != 0 ||
        layout->root_field_count != 0 || layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align || slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index || slot->function != operation->function ||
        slot->role != XR_TARGET_SLOT_TEMPORARY || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_BORROWED)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_source_namespace_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                            XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind ||
        !ctx->exact_source_namespace_value || !ctx->exact_source_namespace_value[semantic_value])
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index < ctx->operation_count
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (operation && layouts[i].semantic_type == operation->result_type) {
            if (layout)
                return false;
            layout = &layouts[i];
        }
    }
    if (!operation || !binding || !register_rep || !memory_rep || !slot || !layout ||
        !aot_source_namespace_value_operation_is_exact(ctx->semantic, operation) ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || layout->field_count != 0 ||
        layout->root_field_count != 0 || layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align || slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index || slot->function != operation->function ||
        slot->role != XR_TARGET_SLOT_TEMPORARY || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_BORROWED)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_static_direct_local_go_callee_storage(const VerifyAuthority *ctx,
                                                         uint32_t semantic_value,
                                                         XrRep *out_storage,
                                                         uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind ||
        !ctx->exact_go_callee_value || !ctx->exact_go_callee_value[semantic_value])
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index < ctx->operation_count
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    uint32_t target = ctx->go_callee_target_by_value[semantic_value];
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (operation && layouts[i].semantic_type == operation->result_type) {
            if (layout)
                return false;
            layout = &layouts[i];
        }
    }
    if (!operation || !binding || !register_rep || !memory_rep || !slot || !layout ||
        !aot_direct_local_callee_type_is_exact(ctx->semantic, operation, target) ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || layout->field_count != 0 ||
        layout->root_field_count != 0 || slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index || slot->function != operation->function ||
        slot->role != XR_TARGET_SLOT_TEMPORARY || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_BORROWED)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_dynamic_channel_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                           XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind ||
        !ctx->exact_channel_value || !ctx->exact_channel_value[semantic_value])
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index == XR_SEMANTIC_INDEX_NONE
            ? NULL
            : xr_semantic_plan_operation(ctx->semantic, operation_index);
    if (!operation || operation->result_value != semantic_value)
        return false;
    bool allocation =
        ctx->exact_channel_allocation_value && ctx->exact_channel_allocation_value[semantic_value];
    uint8_t expected_ownership =
        allocation ? XR_TARGET_OWNERSHIP_OWNED : XR_TARGET_OWNERSHIP_BORROWED;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != operation->result_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    if (!binding || !register_rep || !memory_rep || !slot || !layout ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != expected_ownership ||
        memory_rep->ownership != expected_ownership ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || layout->field_count != 0 ||
        layout->root_field_count != 0 || layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align || slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index || slot->function != operation->function ||
        slot->role != XR_TARGET_SLOT_TEMPORARY || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != expected_ownership)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_dynamic_direct_local_go_task_storage(const VerifyAuthority *ctx,
                                                        uint32_t semantic_value, XrRep *out_storage,
                                                        uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind ||
        !ctx->exact_go_callee_value)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index == XR_SEMANTIC_INDEX_NONE
            ? NULL
            : xr_semantic_plan_operation(ctx->semantic, operation_index);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    uint32_t callee_value = XR_SEMANTIC_INDEX_NONE;
    if (!operation || !operands || operation->opcode != XI_GO ||
        operation->result_value != semantic_value || operation->operand_count == 0 ||
        operation->operand_begin >= operand_count)
        return false;
    callee_value = operands[operation->operand_begin].value;
    if (callee_value >= ctx->value_count || !ctx->exact_go_callee_value[callee_value] ||
        !xr_semantic_direct_local_go_task_result_is_exact(ctx->semantic, operation, true, NULL))
        return false;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != operation->result_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    if (!binding || !register_rep || !memory_rep || !slot || !layout ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || layout->field_count != 0 ||
        layout->root_field_count != 0 || layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align || slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index || slot->function != operation->function ||
        slot->role != XR_TARGET_SLOT_TEMPORARY || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_BORROWED)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_direct_local_callee_use(const VerifyAuthority *ctx, uint32_t operation_index,
                                           uint16_t operand_index, uint32_t source_value) {
    if (!ctx || source_value >= ctx->value_count || operand_index != 0 ||
        !ctx->exact_direct_callee_value[source_value])
        return false;
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(ctx->semantic, operation_index);
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target_plan, &call_count);
    const XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (match)
            return false;
        match = &calls[i];
    }
    return operation && (operation->opcode == XI_CALL || operation->opcode == XI_TAIL_CALL) &&
           match && match->semantic_call_target != XR_SEMANTIC_INDEX_NONE &&
           match->caller_function == operation->function &&
           match->callee_function == ctx->direct_callee_target_by_value[source_value] &&
           match->calling_convention == XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL &&
           match->target_kind == XR_TARGET_CALL_TARGET_DIRECT_LOCAL;
}

static bool oracle_direct_local_go_callee_use(const VerifyAuthority *ctx, uint32_t operation_index,
                                              uint16_t operand_index, uint32_t source_value) {
    const XrSemanticOperationRecord *operation =
        ctx ? xr_semantic_plan_operation(ctx->semantic, operation_index) : NULL;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        ctx ? xr_semantic_plan_operands(ctx->semantic, &operand_count) : NULL;
    return ctx && operation && operand_index == 0 && source_value < ctx->value_count &&
           ctx->exact_go_callee_value && ctx->exact_go_callee_value[source_value] &&
           operation->opcode == XI_GO && operation->operand_count != 0 &&
           operation->operand_begin < operand_count &&
           operands[operation->operand_begin].value == source_value &&
           ctx->go_callee_target_by_value[source_value] != XR_SEMANTIC_INDEX_NONE;
}

/* Rebuild the channel receive boundary from frozen SemanticPlan and
 * TargetPlan facts.  The runtime payload is tagged; the target row owns the
 * exact scalar machine destination.  This is intentionally independent of
 * both the TargetPlan collector and the C-emission recipe verifier. */
static bool oracle_channel_receive_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                           XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind ||
        !ctx->exact_channel_value)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index == XR_SEMANTIC_INDEX_NONE
            ? NULL
            : xr_semantic_plan_operation(ctx->semantic, operation_index);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    if (!operation || operation->opcode != XI_CHAN_TRY_RECV ||
        operation->result_value != semantic_value || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count || operation->allocation_key ||
        !aot_stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE || operation->auxiliary_kind != 0 ||
        operation->semantic_immediate != 0 ||
        operation->effects != xi_generated_op_effects(XI_CHAN_TRY_RECV) ||
        operation->flags != xi_generated_op_default_flags(XI_CHAN_TRY_RECV) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CHAN_TRY_RECV) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_CHAN_TRY_RECV) ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    uint32_t element_type = XR_SEMANTIC_INDEX_NONE;
    if (receiver->value >= ctx->value_count || !ctx->exact_channel_value[receiver->value] ||
        receiver->role != XR_SEM_OPERAND_VALUE || receiver->parameter != -1 ||
        receiver->transfer_mode != XR_TRANSFER_SHARE ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        receiver->parameter_mode != XR_PARAM_READ || receiver->access != XR_CALL_ARG_PLAIN ||
        receiver->origin != XI_PLACE_ORIGIN_NONE || receiver->lifetime != XI_PLACE_LIFETIME_NONE ||
        receiver->escape != XI_PLACE_ESCAPE_NONE || receiver->flags != 0 ||
        !aot_channel_type_is_exact(ctx->semantic, receiver->type, &element_type) ||
        element_type != operation->result_type)
        return false;
    /* The machine oracle refuses a kind that names no storage class and reports
     * the class every named kind resolves to, so asking a second time could
     * only restate that answer -- or, once the two derivations drifted,
     * contradict it.  One judgement settles the destination; the receive then
     * reports the tagged runtime payload over it. */
    XrRep native_storage = XR_REP_TAGGED;
    uint16_t machine_kind = XR_MACHINE_REP_COUNT;
    if (!oracle_machine_storage(ctx, semantic_value, &native_storage, &machine_kind))
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = machine_kind;
    return true;
}

static uint16_t oracle_representation_recipe(uint16_t adapter, uint16_t machine_kind) {
    bool box = adapter == XR_AOT_REP_ADAPTER_BOX;
    switch ((XrMachineRepKind) machine_kind) {
        case XR_MACHINE_REP_F32:
        case XR_MACHINE_REP_F64:
            return box ? XR_AOT_REP_RECIPE_BOX_FLOAT : XR_AOT_REP_RECIPE_UNBOX_FLOAT;
        case XR_MACHINE_REP_RAW_PTR:
            return box ? XR_AOT_REP_RECIPE_BOX_REFERENCE : XR_AOT_REP_RECIPE_UNBOX_REFERENCE;
        case XR_MACHINE_REP_I1:
        case XR_MACHINE_REP_I8:
        case XR_MACHINE_REP_U8:
        case XR_MACHINE_REP_I16:
        case XR_MACHINE_REP_U16:
        case XR_MACHINE_REP_I32:
        case XR_MACHINE_REP_U32:
        case XR_MACHINE_REP_I64:
        case XR_MACHINE_REP_ENUM_ORDINAL:
        case XR_MACHINE_REP_U64:
        case XR_MACHINE_REP_ISIZE:
        case XR_MACHINE_REP_USIZE:
        case XR_MACHINE_REP_RUNE:
            return box ? XR_AOT_REP_RECIPE_BOX_INTEGER : XR_AOT_REP_RECIPE_UNBOX_INTEGER;
        default:
            return XR_AOT_REP_RECIPE_NONE;
    }
}

/* A ref parameter is not a raw pointer value that the refinement oracle may
 * rediscover from its Xi type.  It is a call-bound place whose pointee storage
 * is frozen by the parameter row and by that parameter's TargetPlan slot.  A
 * PLACE_LOAD/PLACE_STORE can use the raw-pointer representation only after all
 * three rows name the same function, value, type and machine representation.
 * This deliberately admits only the scalar raw-pointer family needed here;
 * every other ref-place family remains unavailable until it owns an equally
 * exact representation contract. */
static bool oracle_raw_pointer_local_addr(const VerifyAuthority *ctx, uint32_t operation_index);

static bool oracle_raw_pointer_ref_place(const VerifyAuthority *ctx, uint32_t operation_index,
                                         const XrSemanticOperandRecord **out_place,
                                         const XrSemanticOperandRecord **out_stored) {
    if (out_place)
        *out_place = NULL;
    if (out_stored)
        *out_stored = NULL;
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(ctx ? ctx->semantic : NULL, operation_index);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx ? ctx->semantic : NULL, &operand_count);
    if (!ctx || !operation || !operands ||
        (operation->opcode != XI_PLACE_LOAD && operation->opcode != XI_PLACE_STORE) ||
        operation->operand_count != (operation->opcode == XI_PLACE_LOAD ? 1u : 2u) ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 0 || operation->function >= ctx->function_count)
        return false;
    const XrSemanticOperandRecord *place = &operands[operation->operand_begin];
    if (place->value >= ctx->value_count || place->role != XR_SEM_OPERAND_VALUE ||
        place->parameter != -1 || place->transfer_mode != XR_TRANSFER_SHARE ||
        place->ownership_action != XR_SEM_OPERAND_BORROW ||
        place->parameter_mode != XR_PARAM_READ || place->access != XR_CALL_ARG_PLAIN ||
        place->origin != XI_PLACE_ORIGIN_NONE || place->lifetime != XI_PLACE_LIFETIME_NONE ||
        place->escape != XI_PLACE_ESCAPE_NONE || place->flags != 0)
        return false;
    uint32_t parameter_index = ctx->parameter_by_value[place->value];
    const XrSemanticParameterRecord *parameter =
        parameter_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(ctx->semantic, parameter_index)
            : NULL;
    uint32_t place_operation_index = ctx->operation_by_value[place->value];
    bool parameter_place =
        parameter && parameter->function == operation->function &&
        parameter->value == place->value && parameter->type == place->type &&
        parameter->mode == XR_PARAM_REF && parameter->ownership == XI_OWN_NONE &&
        parameter->transfer_mode == XR_TRANSFER_SHARE &&
        (parameter->flags & ~(XR_SEM_PARAMETER_REQUIRED | XR_SEM_PARAMETER_VARIADIC)) == 0;
    bool local_place = !parameter && place_operation_index < ctx->operation_count &&
                       oracle_raw_pointer_local_addr(ctx, place_operation_index);
    if (!parameter_place && !local_place)
        return false;
    XrRep storage = XR_REP_TAGGED;
    uint16_t machine_kind = XR_MACHINE_REP_COUNT;
    if (!oracle_machine_storage(ctx, place->value, &storage, &machine_kind) ||
        storage != XR_REP_RAWPTR || machine_kind != XR_MACHINE_REP_RAW_PTR)
        return false;
    if (parameter_place) {
        const XrTargetValueRepRecord *binding =
            xr_target_plan_value_rep(ctx->target_plan, place->value);
        uint32_t slot_count = 0;
        const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
        const XrTargetSlotRecord *slot =
            binding && slots && binding->slot < slot_count ? &slots[binding->slot] : NULL;
        if (!binding || !slot || slot->id != binding->slot ||
            slot->function != parameter->function || slot->semantic_value != parameter->value ||
            slot->semantic_operation != XR_SEMANTIC_INDEX_NONE ||
            slot->role != XR_TARGET_SLOT_PARAMETER || slot->register_rep != binding->register_rep ||
            slot->memory_rep != binding->memory_rep)
            return false;
    }
    if (operation->opcode == XI_PLACE_LOAD) {
        if (operation->result_type != place->type)
            return false;
        XrRep result_storage = XR_REP_TAGGED;
        uint16_t result_kind = XR_MACHINE_REP_COUNT;
        if (!oracle_machine_storage(ctx, operation->result_value, &result_storage, &result_kind) ||
            result_storage != XR_REP_RAWPTR || result_kind != XR_MACHINE_REP_RAW_PTR)
            return false;
    } else {
        const XrSemanticOperandRecord *stored = place + 1;
        if (stored->value >= ctx->value_count || stored->type != place->type ||
            stored->role != XR_SEM_OPERAND_VALUE || stored->parameter != -1 ||
            stored->transfer_mode != XR_TRANSFER_SHARE ||
            stored->ownership_action != XR_SEM_OPERAND_CONSUME ||
            stored->parameter_mode != XR_PARAM_READ || stored->access != XR_CALL_ARG_PLAIN ||
            stored->origin != XI_PLACE_ORIGIN_NONE || stored->lifetime != XI_PLACE_LIFETIME_NONE ||
            stored->escape != XI_PLACE_ESCAPE_NONE || stored->flags != 0)
            return false;
        XrRep stored_storage = XR_REP_TAGGED;
        uint16_t stored_kind = XR_MACHINE_REP_COUNT;
        if (!oracle_machine_storage(ctx, stored->value, &stored_storage, &stored_kind) ||
            stored_storage != XR_REP_RAWPTR || stored_kind != XR_MACHINE_REP_RAW_PTR)
            return false;
        if (out_stored)
            *out_stored = stored;
    }
    if (out_place)
        *out_place = place;
    return true;
}

static bool oracle_definition_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                      XrRep *out_storage, uint16_t *out_machine_kind);
static bool oracle_range_slice_result_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                              XrRep *out_storage, uint16_t *out_machine_kind);

/* Resolve the value a name ultimately stands for. An identity COPY is a rename:
 * it gives a value that already exists a second name and produces no storage of
 * its own, so every judgement that asks what a value IS -- which storage family
 * it belongs to, which carrier it already holds -- has to be asked about the
 * value the name resolves to. Asking it about the rename instead grants a
 * carrier to a program that says `s` and refuses the same carrier to the same
 * program written `var t = s`.
 *
 * Xi numbers a COPY result above the operand it renames, so following the chain
 * strictly decreases the index and terminates. A plan that breaks that
 * numbering is refused rather than walked, which leaves the caller without a
 * family: the fail-closed answer.
 *
 * A value that is not a rename resolves to itself, so a caller asks
 * unconditionally and never has to test for the shape first. */
static bool oracle_resolve_identity_rename(const VerifyAuthority *ctx, uint32_t semantic_value,
                                           uint32_t *out_value) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx ? ctx->semantic : NULL, &operand_count);
    if (!ctx || !out_value || !ctx->operation_by_value || semantic_value >= ctx->value_count)
        return false;
    for (;;) {
        uint32_t operation_index = ctx->operation_by_value[semantic_value];
        const XrSemanticOperationRecord *operation =
            operation_index != XR_SEMANTIC_INDEX_NONE
                ? xr_semantic_plan_operation(ctx->semantic, operation_index)
                : NULL;
        if (!operation || operation->opcode != XI_COPY || operation->operand_count != 1 ||
            operation->semantic_immediate != XI_COPY_KIND_IDENTITY)
            break;
        if (!operands || operation->operand_begin >= operand_count ||
            operands[operation->operand_begin].value >= semantic_value)
            return false;
        semantic_value = operands[operation->operand_begin].value;
    }
    *out_value = semantic_value;
    return true;
}

/* The caller-side half of the same ref contract.  LOCAL_ADDR is admitted only
 * when it is the exact address view of one frozen raw-pointer local and both
 * the source and result have independent TargetPlan RAW_PTR bindings.  The
 * pointer type alone is deliberately insufficient: the semantic operation,
 * its sole borrow operand and the two target slots must agree. */
static bool oracle_raw_pointer_local_addr(const VerifyAuthority *ctx, uint32_t operation_index) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(ctx ? ctx->semantic : NULL, operation_index);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx ? ctx->semantic : NULL, &operand_count);
    if (!ctx || !operation || !operands || operation->opcode != XI_LOCAL_ADDR ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->metadata_count != 0 || operation->semantic_immediate != 0 ||
        operation->result_value >= ctx->value_count || operation->function >= ctx->function_count)
        return false;
    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    if (source->value >= ctx->value_count || source->type != operation->result_type ||
        source->role != XR_SEM_OPERAND_VALUE || source->parameter != -1 ||
        source->transfer_mode != XR_TRANSFER_SHARE ||
        source->ownership_action != XR_SEM_OPERAND_BORROW ||
        source->parameter_mode != XR_PARAM_READ || source->access != XR_CALL_ARG_PLAIN ||
        source->origin != XI_PLACE_ORIGIN_NONE || source->lifetime != XI_PLACE_LIFETIME_NONE ||
        source->escape != XI_PLACE_ESCAPE_NONE || source->flags != 0)
        return false;
    XrRep source_storage = XR_REP_TAGGED;
    XrRep result_storage = XR_REP_TAGGED;
    uint16_t source_kind = XR_MACHINE_REP_COUNT;
    uint16_t result_kind = XR_MACHINE_REP_COUNT;
    if (!oracle_machine_storage(ctx, source->value, &source_storage, &source_kind) ||
        !oracle_machine_storage(ctx, operation->result_value, &result_storage, &result_kind) ||
        source_storage != XR_REP_RAWPTR || result_storage != XR_REP_RAWPTR ||
        source_kind != XR_MACHINE_REP_RAW_PTR || result_kind != XR_MACHINE_REP_RAW_PTR)
        return false;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, operation->result_value);
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && slots && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    return binding && slot && slot->id == binding->slot && slot->function == operation->function &&
           slot->semantic_value == operation->result_value &&
           slot->semantic_operation == operation_index && slot->role == XR_TARGET_SLOT_TEMPORARY &&
           slot->register_rep == binding->register_rep && slot->memory_rep == binding->memory_rep;
}

static bool oracle_definition_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                      XrRep *out_storage, uint16_t *out_machine_kind) {
    /* A nullable scalar names the tagged carrier whatever produced it, so the
     * question is settled by its type before any opcode is consulted. */
    if (oracle_nullable_scalar_storage(ctx, semantic_value, out_storage, out_machine_kind))
        return true;
    if (oracle_fixed_array_backing_storage(ctx, semantic_value, out_storage, out_machine_kind))
        return true;
    if (oracle_value_aggregate_storage(ctx, semantic_value, out_storage, out_machine_kind))
        return true;
    if (ctx->parameter_by_value[semantic_value] != XR_SEMANTIC_INDEX_NONE) {
        /* A class instance bound on entry is a tagged instance, which has no
         * scalar machine storage to report; every other parameter keeps its
         * own. */
        if (oracle_direct_local_array_ref_parameter_place_storage(ctx, semantic_value, out_storage,
                                                                  out_machine_kind))
            return true;
        if (oracle_direct_local_array_value_parameter_storage(ctx, semantic_value, out_storage,
                                                              out_machine_kind))
            return true;
        if (oracle_direct_local_string_value_parameter_storage(ctx, semantic_value, out_storage,
                                                               out_machine_kind))
            return true;
        if (oracle_dynamic_source_class_parameter_storage(ctx, semantic_value, out_storage,
                                                          out_machine_kind))
            return true;
        if (oracle_u8_slice_parameter_storage(ctx, semantic_value, out_storage, out_machine_kind))
            return true;
        return oracle_machine_storage(ctx, semantic_value, out_storage, out_machine_kind);
    }
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    if (!operation)
        return false;
    if (ctx->exact_channel_value && ctx->exact_channel_value[semantic_value])
        return oracle_dynamic_channel_storage(ctx, semantic_value, out_storage, out_machine_kind);
    if (ctx->exact_source_namespace_value && ctx->exact_source_namespace_value[semantic_value])
        return oracle_source_namespace_storage(ctx, semantic_value, out_storage, out_machine_kind);
    if (ctx->exact_native_module_namespace_value &&
        ctx->exact_native_module_namespace_value[semantic_value])
        return oracle_native_module_namespace_storage(ctx, semantic_value, out_storage,
                                                      out_machine_kind);
    if (oracle_dynamic_json_namespace_value_storage(ctx, semantic_value, out_storage,
                                                    out_machine_kind))
        return true;
    uint32_t named_value = semantic_value;
    if (!oracle_resolve_identity_rename(ctx, semantic_value, &named_value))
        return false;
    if (named_value != semantic_value)
        return oracle_definition_storage(ctx, named_value, out_storage, out_machine_kind);
    switch (operation->opcode) {
        case XI_CATCH:
            if (oracle_dynamic_panic_catch_storage(ctx, semantic_value, out_storage,
                                                   out_machine_kind))
                return true;
            break;
        case XI_CLOSURE_NEW:
            if (oracle_dynamic_closure_storage(ctx, semantic_value, out_storage, out_machine_kind))
                return true;
            break;
        case XI_CHAN_TRY_RECV:
            if (oracle_channel_receive_storage(ctx, semantic_value, out_storage, out_machine_kind))
                return true;
            /* Non-scalar Recv<T> envelopes remain tagged. A scalar result is
             * owned exclusively by CHANNEL_RECEIVE_STORAGE and may not fall
             * back when its exact authority is missing. */
            {
                const XrTargetValueRepRecord *binding =
                    xr_target_plan_value_rep(ctx->target_plan, semantic_value);
                const XrTargetMachineRepRecord *machine =
                    binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep)
                            : NULL;
                if (machine && machine->kind >= XR_MACHINE_REP_I1 &&
                    machine->kind <= XR_MACHINE_REP_RUNE)
                    return false;
            }
            *out_storage = XR_REP_TAGGED;
            *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
            return true;
        case XI_CALL_BUILTIN:
            if (aot_stringbuilder_constructor_is_exact(ctx->semantic, operation))
                return oracle_dynamic_stringbuilder_storage(ctx, semantic_value, out_storage,
                                                            out_machine_kind);
            if (aot_array_intrinsic_is_exact(ctx->semantic, operation, NULL, NULL))
                return oracle_dynamic_array_intrinsic_storage(ctx, semantic_value, out_storage,
                                                              out_machine_kind);
            break;
        case XI_GET_BUILTIN:
            if (aot_json_namespace_global_is_exact(ctx->semantic, operation)) {
                *out_storage = XR_REP_TAGGED;
                *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
                return true;
            }
            break;
        case XI_ARRAY_NEW:
            if (oracle_dynamic_array_allocation_storage(ctx, semantic_value, out_storage,
                                                        out_machine_kind))
                return true;
            break;
        case XI_SLICE:
        case XI_SLICE_WINDOW:
        case XI_SLICE_REINTERPRET:
        case XI_SLICE_COPY:
        case XI_SLICE_AS_BYTES:
        case XI_BYTE_SLICE_COPY:
            if (oracle_range_slice_result_storage(ctx, semantic_value, out_storage,
                                                  out_machine_kind))
                return true;
            break;
        case XI_LOCAL_ADDR:
            if (oracle_direct_local_array_ref_place_storage(ctx, semantic_value, out_storage,
                                                            out_machine_kind))
                return true;
            break;
        case XI_PLACE_LOAD:
            if (oracle_dynamic_array_ref_storage(ctx, semantic_value, out_storage,
                                                 out_machine_kind))
                return true;
            break;
        case XI_STR_CONCAT:
            if (oracle_dynamic_string_concat_result_storage(ctx, semantic_value, out_storage,
                                                            out_machine_kind))
                return true;
            break;
        case XI_CONVERT:
            if (oracle_dynamic_string_convert_result_storage(ctx, semantic_value, out_storage,
                                                             out_machine_kind))
                return true;
            break;
        case XI_CLASS_CREATE:
            if (oracle_dynamic_source_class_object_storage(ctx, semantic_value, out_storage,
                                                           out_machine_kind))
                return true;
            break;
        case XI_TUPLE_GET:
            /* A tuple lane is read back as the full tagged value it was stored
             * as. Unlike a named value class, whose fields the backend reads in
             * their own native storage, the element read hands out the carrier
             * and every native consumer adapts from it. */
            if (oracle_aggregate_field_access_is_exact(ctx, operation_index)) {
                *out_storage = XR_REP_TAGGED;
                *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
                return true;
            }
            break;
        case XI_OBJECT_NEW:
            if (oracle_dynamic_struct_object_storage(ctx, semantic_value, out_storage,
                                                     out_machine_kind))
                return true;
            break;
        case XI_OBJECT_GET_F:
            /* An object field is stored as a full tagged value, so the read
             * hands out that carrier and each native consumer adapts from it,
             * exactly as a tuple lane read does. */
            if (oracle_struct_object_field_read_storage(ctx, semantic_value, out_storage,
                                                        out_machine_kind))
                return true;
            break;
        case XI_CHAN_RECV:
        case XI_ENUM_DESCRIPTOR_BOX:
            *out_storage = XR_REP_TAGGED;
            *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
            return true;
        case XI_GO:
            if (oracle_dynamic_direct_local_go_task_storage(ctx, semantic_value, out_storage,
                                                            out_machine_kind))
                return true;
            break;
        case XI_GET_SHARED: {
            if (oracle_dynamic_array_ref_storage(ctx, semantic_value, out_storage,
                                                 out_machine_kind))
                return true;
            /* A String read out of a shared cell is the same borrow an Array
             * read of that cell is: a reference-capable container whose only
             * storage fact is the tagged value the cell holds. */
            if (oracle_dynamic_string_shared_read_storage(ctx, semantic_value, out_storage,
                                                          out_machine_kind))
                return true;
            if (oracle_dynamic_struct_object_storage(ctx, semantic_value, out_storage,
                                                     out_machine_kind))
                return true;
            if (ctx->exact_direct_callee_value && ctx->exact_direct_callee_value[semantic_value])
                return oracle_static_direct_local_callee_storage(ctx, semantic_value, out_storage,
                                                                 out_machine_kind);
            if (ctx->exact_go_callee_value && ctx->exact_go_callee_value[semantic_value])
                return oracle_static_direct_local_go_callee_storage(ctx, semantic_value,
                                                                    out_storage, out_machine_kind);
            if (oracle_dynamic_source_class_instance_storage(ctx, semantic_value, out_storage,
                                                             out_machine_kind))
                return true;
            XrRep machine_storage = XR_REP_TAGGED;
            if (!oracle_machine_storage(ctx, semantic_value, &machine_storage, out_machine_kind))
                return false;
            /* Shared storage is tagged, while the exact target value row owns
             * the only legal native adapter recipe for this scalar result. */
            *out_storage = XR_REP_TAGGED;
            return true;
        }
        case XI_PHI:
            if (ctx->policy->force_phi_tagged) {
                *out_storage = XR_REP_TAGGED;
                *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
                return true;
            }
            break;
        case XI_BOX:
            *out_storage = XR_REP_TAGGED;
            *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
            return true;
        case XI_CONST:
            if (xr_semantic_string_literal_is_exact(ctx->semantic, operation) ||
                xr_semantic_bigint_value_is_exact(ctx->semantic, operation))
                return oracle_dynamic_heap_literal_storage(ctx, semantic_value, out_storage,
                                                           out_machine_kind);
            break;
        case XI_CALL_METHOD:
            if (operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_HOF)
                return oracle_array_hof_result_storage(ctx, semantic_value, out_storage,
                                                       out_machine_kind);
            if (aot_array_fill_scalar_is_exact(ctx->semantic, operation, NULL, NULL, NULL))
                return oracle_dynamic_array_fill_scalar_storage(ctx, semantic_value, out_storage,
                                                                out_machine_kind);
            if (xr_semantic_string_slice_range_is_exact(ctx->semantic, operation, NULL, NULL, NULL))
                return oracle_dynamic_string_slice_range_storage(ctx, semantic_value, out_storage,
                                                                 out_machine_kind);
            if (xr_semantic_iterator_rune_has_next_is_exact(ctx->semantic, operation, NULL))
                return oracle_iterator_rune_has_next_call(ctx, semantic_value, out_storage,
                                                          out_machine_kind);
            if (xr_semantic_iterator_rune_next_is_exact(ctx->semantic, operation, NULL))
                return oracle_iterator_rune_next_call(ctx, semantic_value, out_storage,
                                                      out_machine_kind);
            if (xr_semantic_rune_to_uint32_is_exact(ctx->semantic, operation, NULL))
                return oracle_rune_to_uint32_call(ctx, semantic_value, out_storage,
                                                  out_machine_kind);
            if (xr_semantic_rune_is_whitespace_is_exact(ctx->semantic, operation, NULL))
                return oracle_rune_is_whitespace_call(ctx, semantic_value, out_storage,
                                                      out_machine_kind);
            if (oracle_dynamic_string_runes_storage(ctx, semantic_value, out_storage,
                                                    out_machine_kind))
                return true;
            break;
        case XI_ADD:
        case XI_SUB:
        case XI_MUL:
        case XI_DIV:
        case XI_MOD:
        case XI_NEG:
        case XI_BAND:
        case XI_BOR:
        case XI_BXOR:
        case XI_BNOT:
        case XI_SHL:
        case XI_SHR:
        case XI_BIT_ROTL:
        case XI_BIT_ROTR:
        case XI_BIT_BSWAP:
        case XI_BIT_POPCOUNT:
        case XI_BIT_CLZ:
        case XI_BIT_MUL_HIGH:
        case XI_BIT_CTZ:
        case XI_EQ:
        case XI_NE:
        case XI_LT:
        case XI_LE:
        case XI_GT:
        case XI_GE:
        case XI_NOT:
        case XI_ISNULL:
        case XI_IS:
        case XI_LEN:
        case XI_CALL:
            /* An owned String result is the one non-scalar direct-local shape
             * with an exact target row; everything else keeps the scalar
             * machine storage proof. The owned instance a source-class
             * construction returns is the other, and it names its own family. */
            if (oracle_dynamic_direct_local_string_result_storage(ctx, semantic_value, out_storage,
                                                                  out_machine_kind))
                return true;
            if (oracle_dynamic_direct_local_array_result_storage(ctx, semantic_value, out_storage,
                                                                 out_machine_kind))
                return true;
            if (oracle_dynamic_source_class_instance_storage(ctx, semantic_value, out_storage,
                                                             out_machine_kind))
                return true;
            /* An arbitrary-precision result is a heap value however it was
             * computed: negating or adding BigInts yields another BigInt, not
             * a machine scalar. The class is read from the frozen builtin id
             * the plan already stores, not from the type's spelling. */
            if (oracle_dynamic_heap_literal_storage(ctx, semantic_value, out_storage,
                                                    out_machine_kind))
                return true;
            break;
        default:
            break;
    }
    /* No family named a carrier of its own, so the value is in whatever storage
     * the TargetPlan already froze for it.
     *
     * The TargetPlan binds a scalar representation from a value's semantic
     * type, uniformly for every operation result and every parameter; it never
     * consults the opcode to do it. Asking again here, once per defining
     * opcode, therefore restated a fact the frozen plan already carried -- the
     * same judgement was written out across sixteen opcodes, and any opcode
     * nobody had written a branch for yet refused a value whose storage the
     * plan names exactly.
     *
     * This stays fail-closed on the fact rather than on the spelling: a value
     * the plan bound nothing for, or bound to a representation that names no
     * storage class -- every object, code, dynamic, aggregate, vector and view
     * kind, and every nullable or aggregate type -- is refused here exactly as
     * before. The families that do carry those name their own storage above,
     * and keep their priority. */
    return oracle_machine_storage(ctx, semantic_value, out_storage, out_machine_kind);
}

static bool oracle_direct_local_array_ref_place_use(const VerifyAuthority *ctx,
                                                    uint32_t operation_index,
                                                    uint32_t source_value) {
    const XrSemanticOperationRecord *place =
        xr_semantic_plan_operation(ctx->semantic, operation_index);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!place || !operands || place->opcode != XI_LOCAL_ADDR || place->operand_count != 1 ||
        place->operand_begin >= operand_count ||
        operands[place->operand_begin].value != source_value ||
        operands[place->operand_begin].type != place->result_type ||
        operands[place->operand_begin].role != XR_SEM_OPERAND_VALUE ||
        operands[place->operand_begin].parameter != -1 ||
        operands[place->operand_begin].parameter_mode != XR_PARAM_READ ||
        operands[place->operand_begin].access != XR_CALL_ARG_PLAIN ||
        operands[place->operand_begin].origin != XI_PLACE_ORIGIN_NONE ||
        operands[place->operand_begin].lifetime != XI_PLACE_LIFETIME_NONE ||
        operands[place->operand_begin].escape != XI_PLACE_ESCAPE_NONE ||
        operands[place->operand_begin].flags != 0 ||
        operands[place->operand_begin].ownership_action != XR_SEM_OPERAND_BORROW ||
        place->effects != xi_generated_op_effects(XI_LOCAL_ADDR) ||
        place->flags != xi_generated_op_default_flags(XI_LOCAL_ADDR) ||
        place->ownership_use != xi_generated_op_own_use(XI_LOCAL_ADDR) ||
        place->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        place->result_alias_operand != -1 || place->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        place->metadata_count != 0 || place->auxiliary_kind != XI_AUX_KIND_NONE ||
        place->semantic_immediate != 0 || place->constant != XR_SEMANTIC_INDEX_NONE ||
        place->callable_function != XR_SEMANTIC_INDEX_NONE ||
        place->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        !aot_array_type_is_exact(ctx->semantic, place->result_type, true, &storage))
        return false;
    const XrTargetValueRepRecord *caller = xr_target_plan_value_rep(ctx->target_plan, source_value);
    uint32_t argument_count = 0;
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(ctx->target_plan, &argument_count);
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target_plan, &call_count);
    const XrTargetCallArgumentRecord *argument = NULL;
    for (uint32_t i = 0; arguments && i < argument_count; i++) {
        if (arguments[i].semantic_value != place->result_value)
            continue;
        if (argument)
            return false;
        argument = &arguments[i];
    }
    const XrTargetCallRecord *call =
        argument && argument->call < call_count ? &calls[argument->call] : NULL;
    const XrSemanticCallTargetRecord *target =
        call && call->semantic_call_target != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_call_target(ctx->semantic, call->semantic_call_target)
            : NULL;
    const XrSemanticParameterRecord *parameter =
        argument && argument->callee_parameter != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(ctx->semantic, argument->callee_parameter)
            : NULL;
    const XrSemanticOperationRecord *call_operation =
        call ? xr_semantic_plan_operation(ctx->semantic, call->semantic_operation) : NULL;
    const XrSemanticOperandRecord *call_operand =
        argument && argument->semantic_operand < operand_count
            ? &operands[argument->semantic_operand]
            : NULL;
    const XrTargetValueRepRecord *callee =
        parameter ? xr_target_plan_value_rep(ctx->target_plan, parameter->value) : NULL;
    XrStableId expected_identity;
    if (!caller || !argument || !call || !target || !parameter || !call_operation ||
        !call_operand || !callee ||
        !aot_array_ref_parameter_is_exact(ctx->semantic, parameter, &storage) ||
        target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL ||
        target->operation != call->semantic_operation ||
        target->function != call->callee_function ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
        call->target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
        argument->semantic_operand < call_operation->operand_begin + 1u ||
        argument->semantic_operand >=
            call_operation->operand_begin + call_operation->operand_count ||
        call_operand->value != place->result_value || call_operand->type != parameter->type ||
        call_operand->role != XR_SEM_OPERAND_ARGUMENT ||
        call_operand->parameter != (int16_t) argument->ordinal ||
        call_operand->parameter_mode != XR_PARAM_REF || call_operand->access != XR_CALL_ARG_REF ||
        call_operand->origin == XI_PLACE_ORIGIN_NONE ||
        call_operand->lifetime != XI_PLACE_LIFETIME_CALL_BOUND ||
        call_operand->escape != XI_PLACE_ESCAPE_NONE ||
        call_operand->ownership_action != XR_SEM_OPERAND_BORROW ||
        call_operand->transfer_mode != XR_TRANSFER_SHARE ||
        call_operand->flags != (XR_SEM_OPERAND_CALL_CONTRACT | XR_SEM_OPERAND_ADDRESSABLE) ||
        !aot_pair_identity("xray-target-direct-array-ref-argument-v1", target->id, parameter->id,
                           argument->ordinal, &expected_identity) ||
        !xr_stable_id_equal(argument->identity, expected_identity) ||
        argument->caller_slot != caller->slot || argument->callee_slot != callee->slot ||
        argument->register_rep != caller->register_rep ||
        argument->memory_rep != caller->memory_rep ||
        argument->callee_register_rep != callee->register_rep ||
        argument->callee_memory_rep != callee->memory_rep ||
        argument->mode != XR_TARGET_CALL_REFERENCE ||
        argument->ownership != XR_TARGET_CALL_BORROW ||
        argument->transfer_mode != XR_TRANSFER_SHARE ||
        argument->flags != XR_TARGET_CALL_ARGUMENT_ADDRESSABLE ||
        argument->array_element_storage != storage || argument->reserved8[0] != 0 ||
        argument->reserved8[1] != 0 || argument->reserved8[2] != 0)
        return false;
    return true;
}

static bool oracle_direct_local_array_ref_place_storage(const VerifyAuthority *ctx,
                                                        uint32_t semantic_value, XrRep *out_storage,
                                                        uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    if (!operation || !operands || operation->opcode != XI_LOCAL_ADDR ||
        operation->result_value != semantic_value || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count ||
        !oracle_direct_local_array_ref_place_use(ctx, operation_index,
                                                 operands[operation->operand_begin].value))
        return false;
    *out_storage = XR_REP_RAWPTR;
    *out_machine_kind = XR_MACHINE_REP_RAW_PTR;
    return true;
}

static bool oracle_array_hof_use_storage(const VerifyAuthority *ctx, uint32_t operation_index,
                                         uint16_t operand_index, uint32_t source_value,
                                         XrRep *out_storage) {
    AotArrayHofAuthority shape = {0};
    uint16_t machine_kind = XR_MACHINE_REP_COUNT;
    XrRep definition_storage = XR_REP_VOID;
    if (!out_storage || !aot_array_hof_target_is_exact(ctx, operation_index, &shape))
        return false;
    if (operand_index == 0 && source_value == shape.receiver) {
        if (!oracle_definition_storage(ctx, source_value, &definition_storage, &machine_kind) ||
            definition_storage != XR_REP_TAGGED || machine_kind != XR_MACHINE_REP_DYN_VALUE)
            return false;
        *out_storage = XR_REP_TAGGED;
        return true;
    }
    if (operand_index == 1 && source_value == shape.callback) {
        if (!oracle_dynamic_closure_storage(ctx, source_value, &definition_storage,
                                            &machine_kind) ||
            definition_storage != XR_REP_TAGGED || machine_kind != XR_MACHINE_REP_DYN_VALUE)
            return false;
        *out_storage = XR_REP_TAGGED;
        return true;
    }
    return shape.kind == XR_TARGET_ARRAY_HOF_REDUCE && operand_index == 2 &&
           source_value == shape.initial &&
           oracle_machine_storage(ctx, source_value, out_storage, &machine_kind);
}

/* The three containers whose elements this authority can name, each answered in
 * the storage its own family bound: a byte view in the frozen VIEW its slice
 * parameter carries, a fixed array in its aggregate lane, and a dynamic Array
 * in its tagged allocation. A read that borrows a container without asking it
 * to change storage -- an element access, a data-pointer projection -- asks
 * this one judgement, so a receiver one such read admits can never be one
 * another refuses. The order is the containers' own: a value belongs to exactly
 * one of the three families, and each judgement re-proves its own membership.
 *
 * Being a container is the whole requirement. These reads impose no carrier, so
 * there is nothing else for them to check, and a value none of the three names
 * has no proved element storage to point into. */
/* The view a range slice produces. It borrows part of a container it did not
 * allocate, so it owns nothing and its storage is the same VIEW pair a borrowed
 * Slice parameter carries: a two-word pointer/length pair, never a tagged
 * carrier. The result type must say the same thing -- a borrow view over an
 * exact element -- and the TargetPlan must already have bound that exact
 * subject, because this pass re-proves storage rather than choosing it. */
static bool oracle_range_slice_result_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                              XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage || !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    if (!operation || operation->result_value != semantic_value ||
        !xr_semantic_range_slice_is_exact(ctx->semantic, operation, NULL))
        return false;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    if (!binding || !register_rep || !memory_rep || binding->semantic_value != semantic_value ||
        register_rep->id != binding->register_rep || memory_rep->id != binding->memory_rep ||
        register_rep->kind != XR_MACHINE_REP_VIEW || memory_rep->kind != XR_MACHINE_REP_VIEW)
        return false;
    *out_storage = XR_REP_PTR;
    *out_machine_kind = XR_MACHINE_REP_VIEW;
    return true;
}

/* An element read through a view a slice produced. The existing arms of the
 * index branch each name where their receiver came from -- a borrowed parameter,
 * a fixed array, a dynamic Array -- and a view taken by `container[start:end]`
 * is a fourth origin none of them covers. The receiver keeps the VIEW pair its
 * own family bound and the index keeps its native scalar storage, exactly as the
 * borrowed-parameter arm already reads its own view. */
static bool oracle_range_slice_element_read_is_exact(const VerifyAuthority *ctx,
                                                     uint32_t operation_index) {
    const XrSemanticOperationRecord *operation =
        ctx ? xr_semantic_plan_operation(ctx->semantic, operation_index) : NULL;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        ctx ? xr_semantic_plan_operands(ctx->semantic, &operand_count) : NULL;
    bool write = operation && operation->opcode == XI_INDEX_SET;
    uint16_t arity = write ? 3u : 2u;
    if (!ctx || !operation || !operands ||
        (operation->opcode != XI_INDEX_GET && operation->opcode != XI_INDEX_SET) ||
        operation->operand_count != arity || operand_count < arity ||
        operation->operand_begin > operand_count - arity || operation->auxiliary_kind != 0 ||
        operation->metadata_count != 0 || operation->semantic_immediate != 0 ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE || operation->allocation_key != NULL ||
        operation->effects != xi_generated_op_effects(operation->opcode) ||
        operation->result_ownership != xi_generated_op_result_ownership(operation->opcode) ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1)
        return false;
    const XrSemanticOperandRecord *view = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *index = view + 1;
    const XrSemanticTypeRecord *view_type = xr_semantic_plan_type(ctx->semantic, view->type);
    /* A write needs a view that is not a read-only borrow, and stores the
     * element the view's own type names. A read hands that element back. */
    if (write && (!view_type || (view_type->flags & XR_SEM_TYPE_CONST) != 0))
        return false;
    XrRep view_storage = XR_REP_TAGGED;
    uint16_t view_kind = XR_MACHINE_REP_COUNT;
    uint32_t element_type = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticTypeRecord *index_type = xr_semantic_plan_type(ctx->semantic, index->type);
    if (view->role != XR_SEM_OPERAND_VALUE || view->parameter != -1 ||
        view->ownership_action != XR_SEM_OPERAND_BORROW ||
        !xr_semantic_slice_view_type_is_exact(ctx->semantic, view->type, &element_type) ||
        (!write && operation->result_type != element_type) ||
        !oracle_range_slice_result_storage(ctx, view->value, &view_storage, &view_kind) ||
        view_storage != XR_REP_PTR || view_kind != XR_MACHINE_REP_VIEW ||
        index->role != XR_SEM_OPERAND_VALUE || index->parameter != -1 ||
        index->ownership_action != XR_SEM_OPERAND_BORROW || !index_type ||
        index_type->kind != XR_KIND_INT || index_type->scalar_rep != XR_NATIVE_I64 ||
        index_type->flags != 0 || index_type->child_count != 0 ||
        index_type->builtin_type != XR_TID_NULL)
        return false;
    if (write) {
        const XrSemanticOperandRecord *stored = view + 2;
        if (stored->role != XR_SEM_OPERAND_VALUE || stored->parameter != -1 ||
            stored->type != element_type)
            return false;
    }
    return true;
}

static bool oracle_array_like_receiver_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                               XrRep *out_storage, uint16_t *out_machine_kind) {
    return oracle_u8_slice_parameter_storage(ctx, semantic_value, out_storage, out_machine_kind) ||
           oracle_fixed_array_value_storage(ctx, semantic_value, out_storage, out_machine_kind) ||
           oracle_array_tagged_carrier_storage(ctx, semantic_value, out_storage, out_machine_kind);
}

static bool oracle_use_storage(const VerifyAuthority *ctx, uint32_t operation_index,
                               uint16_t operand_index, uint32_t source_value, XrRep *out_storage) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(ctx->semantic, operation_index);
    if (!operation || operand_index >= operation->operand_count)
        return false;
    uint16_t ignored_kind = 0;
    /* A nullable scalar operand stays in the tagged carrier its definition
     * already named, so no use of it can request an adapter. */
    if (oracle_nullable_scalar_storage(ctx, source_value, out_storage, &ignored_kind))
        return true;
    /* The view a range slice produced stays in the VIEW pair its definition
     * named, for the same reason and on the same terms: a borrowed pointer and
     * length has no tagged form to be boxed into, so every reader of it -- a
     * length read, a byte load or store, a window, a comparison, a
     * reinterpretation, a further slice -- takes exactly the pair that already
     * exists. Stating that once here is what keeps the twelve consuming opcodes
     * from each having to name the same storage and drift apart later. */
    if (oracle_range_slice_result_storage(ctx, source_value, out_storage, &ignored_kind))
        return true;
    switch (operation->opcode) {
        case XI_PLACE_LOAD:
            if (operand_index != 0 || source_value >= ctx->value_count)
                return false;
            if (oracle_direct_local_array_ref_place_storage(ctx, source_value, out_storage,
                                                            &ignored_kind) ||
                oracle_direct_local_array_ref_parameter_place_storage(ctx, source_value,
                                                                      out_storage, &ignored_kind)) {
                *out_storage = XR_REP_RAWPTR;
                return true;
            }
            if (!oracle_raw_pointer_ref_place(ctx, operation_index, NULL, NULL))
                return false;
            return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
        case XI_PLACE_STORE: {
            const XrSemanticOperandRecord *place = NULL;
            const XrSemanticOperandRecord *stored = NULL;
            if (operand_index > 1 ||
                !oracle_raw_pointer_ref_place(ctx, operation_index, &place, &stored) ||
                source_value != (operand_index == 0 ? place->value : stored->value))
                return false;
            return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
        }
        case XI_LOCAL_ADDR:
            if (operand_index != 0)
                return false;
            if (oracle_direct_local_array_ref_place_use(ctx, operation_index, source_value)) {
                *out_storage = XR_REP_TAGGED;
                return true;
            }
            if (!oracle_raw_pointer_local_addr(ctx, operation_index))
                return false;
            return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
        case XI_CALL_BUILTIN: {
            uint32_t reserve_receiver = XR_SEMANTIC_INDEX_NONE;
            uint32_t reserve_capacity = XR_SEMANTIC_INDEX_NONE;
            if (aot_array_reserve_use_is_exact(ctx, operation_index, &reserve_receiver,
                                               &reserve_capacity)) {
                if (operand_index == 0 && source_value == reserve_receiver) {
                    *out_storage = XR_REP_TAGGED;
                    return true;
                }
                if (operand_index == 1 && source_value == reserve_capacity)
                    return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
                return false;
            }
            if (aot_array_intrinsic_is_exact(ctx->semantic, operation, NULL, NULL)) {
                XrRep result_storage = XR_REP_TAGGED;
                if (!oracle_dynamic_array_intrinsic_storage(ctx, operation->result_value,
                                                            &result_storage, &ignored_kind))
                    return false;
                return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
            }
            *out_storage = XR_REP_TAGGED;
            return true;
        }
        case XI_RETAIN:
        case XI_RELEASE:
        case XI_THROW: {
            /* A refcount adjustment names no family of its own: it acts on any
             * reference this authority can name, and each stays in the tagged
             * carrier its own family bound. A native scalar has no refcount and
             * is not admitted here.
             *
             * A throw consumes its operand on the same terms. The exception
             * carrier the runtime raises is one tagged value, and every panic
             * reaches it as a reference the front end already built: a
             * constructed PanicInfo, a message String, or the value a catch
             * handed back for a cleanup path to re-raise. Each of those is a
             * family this authority names, and each is already in the tagged
             * carrier the throw wants, so the site records no adapter. A native
             * scalar stays refused because no throw produces one. */
            XrRep carrier_storage = XR_REP_TAGGED;
            if (!oracle_tagged_reference_carrier_storage(ctx, source_value, &carrier_storage,
                                                         &ignored_kind))
                return false;
            *out_storage = XR_REP_TAGGED;
            return true;
        }
        case XI_SET_SHARED:
        case XI_PRINT: {
            /* A store into a shared cell and a print both consume their operand
             * as a tagged value, whatever it is: any reference this authority
             * can name reaches them in its own carrier, and a native scalar
             * reaches them through the box adapter its machine storage owes. */
            XrRep carrier_storage = XR_REP_TAGGED;
            if (!oracle_tagged_reference_carrier_storage(ctx, source_value, &carrier_storage,
                                                         &ignored_kind) &&
                !oracle_machine_storage(ctx, source_value, &carrier_storage, &ignored_kind))
                return false;
            *out_storage = XR_REP_TAGGED;
            return true;
        }
        case XI_GO:
            if (operand_index == 0) {
                if (!oracle_direct_local_go_callee_use(ctx, operation_index, operand_index,
                                                       source_value))
                    return false;
                *out_storage = XR_REP_TAGGED;
                return true;
            }
            return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
        case XI_AWAIT:
            if (!xr_semantic_await_task_operand_is_exact(ctx->semantic, operation, operand_index,
                                                         source_value) ||
                !oracle_dynamic_direct_local_go_task_storage(ctx, source_value, out_storage,
                                                             &ignored_kind))
                return false;
            *out_storage = XR_REP_TAGGED;
            return true;
        case XI_CHAN_NEW:
            if (operand_index != 0 || !ctx->exact_channel_allocation_value ||
                !ctx->exact_channel_allocation_value[operation->result_value])
                return false;
            return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
        case XI_CHAN_SEND:
        case XI_CHAN_TRY_SEND:
            if (operand_index == 0) {
                if (!ctx->exact_channel_value[source_value])
                    return false;
                *out_storage = XR_REP_TAGGED;
                return true;
            }
            if (operand_index == 1) {
                *out_storage = XR_REP_TAGGED;
                return true;
            }
            return false;
        case XI_CHAN_RECV:
        case XI_CHAN_TRY_RECV:
        case XI_CHAN_IS_CLOSED:
        case XI_CHAN_TIMER_DISPOSE:
            if (operand_index != 0 || !ctx->exact_channel_value[source_value])
                return false;
            *out_storage = XR_REP_TAGGED;
            return true;
        case XI_CHAN_RECV_STATUS:
            if (operand_index != 0)
                return false;
            *out_storage = XR_REP_TAGGED;
            return true;
        case XI_SELECT_BLOCK:
            if (!ctx->exact_channel_value[source_value])
                return false;
            *out_storage = XR_REP_TAGGED;
            return true;
        case XI_PHI:
            if (ctx->policy->force_phi_tagged) {
                *out_storage = XR_REP_TAGGED;
                return true;
            }
            return oracle_machine_storage(ctx, operation->result_value, out_storage, &ignored_kind);
        case XI_SELECT:
            /* The condition is the bool the structure contract already pinned,
             * and it keeps the native scalar storage its own definition named:
             * the generated conditional reads it through the same truthiness
             * adapter a branch on it would use, so no operand adapter is owed.
             * The two arms are the incoming edges of the merge and must reach
             * it in the merge's own storage, which is what a phi demands of
             * its edges. */
            if (operand_index == 0)
                return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
            if (operand_index > 2)
                return false;
            return oracle_machine_storage(ctx, operation->result_value, out_storage, &ignored_kind);
        case XI_CALL:
            if (operand_index != 0 && oracle_direct_local_array_ref_place_storage(
                                          ctx, source_value, out_storage, &ignored_kind))
                return true;
            if (operand_index == 0 || !ctx->policy->prefer_call_args_native) {
                if (operand_index == 0 && ctx->exact_direct_callee_value[source_value] &&
                    !oracle_direct_local_callee_use(ctx, operation_index, operand_index,
                                                    source_value))
                    return false;
                *out_storage = XR_REP_TAGGED;
                return true;
            }
            /* A class instance argument has no native scalar storage a policy
             * could prefer: it is the tagged carrier its own storage family
             * bound, whether the caller computed it or received it. An Array or
             * a String handed over by value is the same case for the same
             * reason -- each is a reference-capable container whose only
             * storage fact is that tagged value. */
            if (oracle_dynamic_source_class_instance_storage(ctx, source_value, out_storage,
                                                             &ignored_kind) ||
                oracle_dynamic_source_class_parameter_storage(ctx, source_value, out_storage,
                                                              &ignored_kind) ||
                oracle_array_tagged_carrier_storage(ctx, source_value, out_storage,
                                                    &ignored_kind) ||
                oracle_string_tagged_carrier_storage(ctx, source_value, out_storage, &ignored_kind))
                return true;
            return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT:
            if (operation->opcode == XI_CALL_METHOD &&
                operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_HOF)
                return oracle_array_hof_use_storage(ctx, operation_index, operand_index,
                                                    source_value, out_storage);
            if (operation->opcode == XI_CALL_METHOD &&
                xr_semantic_string_slice_range_is_exact(ctx->semantic, operation, NULL, NULL,
                                                        NULL)) {
                XrRep result_storage = XR_REP_VOID;
                XrRep source_storage = XR_REP_VOID;
                uint16_t source_kind = XR_MACHINE_REP_COUNT;
                if (operand_index >= 3 ||
                    !oracle_dynamic_string_slice_range_storage(ctx, operation->result_value,
                                                               &result_storage, &ignored_kind) ||
                    !oracle_definition_storage(ctx, source_value, &source_storage, &source_kind))
                    return false;
                if (operand_index == 0) {
                    if (source_storage != XR_REP_TAGGED || source_kind != XR_MACHINE_REP_DYN_VALUE)
                        return false;
                    *out_storage = XR_REP_TAGGED;
                    return true;
                }
                if (source_storage != XR_REP_I64 || source_kind != XR_MACHINE_REP_I64)
                    return false;
                /* The helper takes both bounds as int64_t, and the emission
                 * recipe reads their semantic sources directly rather than any
                 * adapter node. Asking for a tagged carrier here would have
                 * demanded a box the recipe then discards -- and Xi never built
                 * one, so the demand could not be met at all. The bound is
                 * native on both sides, which leaves nothing to adapt. */
                *out_storage = XR_REP_I64;
                return true;
            }
            if (operation->opcode == XI_CALL_METHOD &&
                aot_array_fill_scalar_is_exact(ctx->semantic, operation, NULL, NULL, NULL)) {
                XrRep result_storage = XR_REP_TAGGED;
                if (!oracle_dynamic_array_fill_scalar_storage(ctx, operation->result_value,
                                                              &result_storage, &ignored_kind))
                    return false;
                if (operand_index == 0) {
                    if (!oracle_definition_storage(ctx, source_value, out_storage, &ignored_kind))
                        return false;
                    *out_storage = XR_REP_TAGGED;
                    return true;
                }
                return operand_index == 1 &&
                       oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
            }
            if (operation->opcode == XI_CALL_METHOD &&
                xr_semantic_iterator_rune_has_next_is_exact(ctx->semantic, operation, NULL)) {
                XrRep result_storage = XR_REP_TAGGED;
                if (operand_index != 0 ||
                    !oracle_iterator_rune_has_next_call(ctx, operation->result_value,
                                                        &result_storage, &ignored_kind) ||
                    !oracle_dynamic_string_runes_storage(ctx, source_value, out_storage,
                                                         &ignored_kind))
                    return false;
                *out_storage = XR_REP_TAGGED;
                return true;
            }
            if (operation->opcode == XI_CALL_METHOD &&
                xr_semantic_iterator_rune_next_is_exact(ctx->semantic, operation, NULL)) {
                XrRep result_storage = XR_REP_TAGGED;
                if (operand_index != 0 ||
                    !oracle_iterator_rune_next_call(ctx, operation->result_value, &result_storage,
                                                    &ignored_kind) ||
                    !oracle_dynamic_string_runes_storage(ctx, source_value, out_storage,
                                                         &ignored_kind))
                    return false;
                *out_storage = XR_REP_TAGGED;
                return true;
            }
            if (operation->opcode == XI_CALL_METHOD &&
                xr_semantic_rune_to_uint32_is_exact(ctx->semantic, operation, NULL)) {
                XrRep result_storage = XR_REP_TAGGED;
                if (operand_index != 0 ||
                    !oracle_rune_to_uint32_call(ctx, operation->result_value, &result_storage,
                                                &ignored_kind) ||
                    !oracle_iterator_rune_next_call(ctx, source_value, out_storage, &ignored_kind))
                    return false;
                *out_storage = XR_REP_I64;
                return true;
            }
            if (operation->opcode == XI_CALL_METHOD &&
                xr_semantic_rune_is_whitespace_is_exact(ctx->semantic, operation, NULL)) {
                XrRep result_storage = XR_REP_TAGGED;
                if (operand_index != 0 ||
                    !oracle_rune_is_whitespace_call(ctx, operation->result_value, &result_storage,
                                                    &ignored_kind) ||
                    !oracle_iterator_rune_next_call(ctx, source_value, out_storage, &ignored_kind))
                    return false;
                *out_storage = XR_REP_I64;
                return true;
            }
            if (operation->opcode == XI_CALL_METHOD &&
                xr_semantic_string_runes_is_exact(ctx->semantic, operation, NULL)) {
                XrRep result_storage = XR_REP_TAGGED;
                if (operand_index != 0 ||
                    !oracle_dynamic_string_runes_storage(ctx, operation->result_value,
                                                         &result_storage, &ignored_kind))
                    return false;
                *out_storage = XR_REP_TAGGED;
                return true;
            }
            if (operand_index == 0 || !ctx->policy->prefer_call_args_native) {
                *out_storage = XR_REP_TAGGED;
                return true;
            }
            return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
        case XI_COPY:
        case XI_SOURCE_MOVE:
        case XI_OWNER_FORWARD: {
            uint16_t result_kind = 0;
            return oracle_definition_storage(ctx, operation->result_value, out_storage,
                                             &result_kind);
        }
        case XI_ARRAY_NEW: {
            /* The capacity is the sole operand and stays in its own native
             * scalar storage; the allocation it feeds must be exact. */
            XrRep ignored_storage = XR_REP_TAGGED;
            if (operand_index != 0 ||
                !oracle_dynamic_array_allocation_storage(ctx, operation->result_value,
                                                         &ignored_storage, &ignored_kind))
                return false;
            return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
        }
        case XI_TUPLE_NEW: {
            /* A tuple lane is stored as a full tagged value, so every operand
             * reaches the construction in the tagged carrier whatever native
             * scalar storage its own definition named. The aggregate being
             * built must itself be exact before any lane is claimed. */
            XrRep aggregate_storage = XR_REP_TAGGED;
            if (!oracle_value_aggregate_storage(ctx, operation->result_value, &aggregate_storage,
                                                &ignored_kind))
                return false;
            *out_storage = XR_REP_TAGGED;
            return true;
        }
        case XI_TUPLE_GET:
            /* The receiver of a proved element read stays in its own aggregate
             * binding, and the read has no other operand. */
            if (operand_index != 0 || !oracle_aggregate_field_access_is_exact(ctx, operation_index))
                return false;
            return oracle_value_aggregate_storage(ctx, source_value, out_storage, &ignored_kind);
        case XI_OBJECT_GET_F:
            /* The receiver of a proved field read stays the tagged carrier it
             * is, and the read has no other operand. */
            if (operand_index != 0 ||
                !oracle_struct_object_field_access_is_exact(ctx, operation_index))
                return false;
            return oracle_dynamic_struct_object_storage(ctx, source_value, out_storage,
                                                        &ignored_kind);
        case XI_OBJECT_INIT_F:
        case XI_OBJECT_SET_F:
            /* Every field of an object is stored as a full tagged value, so the
             * written operand reaches the store in the carrier whatever native
             * scalar storage its own definition named, and the receiver stays
             * the carrier it is. */
            if (!oracle_struct_object_field_access_is_exact(ctx, operation_index))
                return false;
            if (operand_index == 0)
                return oracle_dynamic_struct_object_storage(ctx, source_value, out_storage,
                                                            &ignored_kind);
            if (operand_index != 1)
                return false;
            *out_storage = XR_REP_TAGGED;
            return true;
        case XI_AGG_NEW:
            if (operand_index != 0 || !oracle_value_aggregate_storage(ctx, operation->result_value,
                                                                      out_storage, &ignored_kind))
                return false;
            return oracle_dynamic_source_class_object_storage(ctx, source_value, out_storage,
                                                              &ignored_kind) ||
                   oracle_dynamic_source_class_instance_storage(ctx, source_value, out_storage,
                                                                &ignored_kind);
        case XI_AGG_GET:
            if (operand_index != 0 || !oracle_aggregate_field_access_is_exact(ctx, operation_index))
                return false;
            return oracle_value_aggregate_storage(ctx, source_value, out_storage, &ignored_kind);
        case XI_AGG_SET:
            if (!oracle_aggregate_field_access_is_exact(ctx, operation_index))
                return false;
            if (operand_index == 0)
                return oracle_value_aggregate_storage(ctx, source_value, out_storage,
                                                      &ignored_kind);
            if (operand_index != 1)
                return false;
            if (oracle_value_aggregate_storage(ctx, source_value, out_storage, &ignored_kind))
                return true;
            return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
        case XI_EQ:
        case XI_NE:
            /* A comparison over machine scalars keeps their native storage. The
             * one reference shape admitted here is a proved String on both
             * sides, which stays in the tagged carrier its own family named. */
            if (oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind))
                return true;
            if (!oracle_string_equality_is_exact(ctx, operation_index))
                return false;
            *out_storage = XR_REP_TAGGED;
            return true;
        case XI_LEN:
            /* The container of a proved length read stays in the tagged carrier
             * its own storage family named; the integer the read produces keeps
             * its native scalar storage at its own definition. */
            if (operand_index != 0 || !oracle_length_read_is_exact(ctx, operation_index))
                return false;
            *out_storage = XR_REP_TAGGED;
            return true;
        case XI_STR_CONCAT: {
            /* Every piece a proved concatenation joins is an owned String, and
             * each one stays in the tagged carrier its own family named. A join
             * this authority has not proved leaves its pieces unclaimed. */
            XrRep piece_storage = XR_REP_TAGGED;
            if (!oracle_dynamic_string_concat_result_storage(ctx, operation->result_value,
                                                             &piece_storage, &ignored_kind))
                return false;
            *out_storage = XR_REP_TAGGED;
            return true;
        }
        case XI_ARRAY_DATA_PTR: {
            /* A data-pointer read borrows the container it is given and hands
             * back a raw pointer into that container's own elements. It asks
             * nothing of the container's storage: C emission reads a byte view
             * through its span, a fixed array through its aggregate lane, and a
             * dynamic Array through the allocation its tagged carrier holds,
             * each in the storage that family already bound.
             *
             * So the site names no carrier of its own, and the storage it
             * consumes is the one the definition already stated -- which is
             * what makes it adapter-free by construction rather than by two
             * judgements happening to agree. What it does require is that the
             * operand be one of the three containers; a value none of them
             * names has no proved elements to point into, and the read stays
             * refused. */
            XrRep container_storage = XR_REP_TAGGED;
            if (operand_index != 0 || !oracle_array_like_receiver_storage(
                                          ctx, source_value, &container_storage, &ignored_kind))
                return false;
            return oracle_definition_storage(ctx, source_value, out_storage, &ignored_kind);
        }
        case XI_SLICE: {
            /* A range slice borrows the container it is given and hands back a
             * view over part of the same elements. Like a data-pointer read it
             * asks nothing of the container's storage -- a byte view, a fixed
             * array and a dynamic Array each stay in the carrier their own
             * family bound -- so the site names no carrier of its own and is
             * adapter-free by construction rather than by two judgements
             * happening to agree. The operand must still be one of those three
             * containers: a value none of them names has no proved elements to
             * take a window of. The two bounds are ordinary native integers and
             * keep the scalar storage bound for their own subject. */
            if (operand_index == 0) {
                XrRep container_storage = XR_REP_TAGGED;
                if (!oracle_array_like_receiver_storage(ctx, source_value, &container_storage,
                                                        &ignored_kind))
                    return false;
                return oracle_definition_storage(ctx, source_value, out_storage, &ignored_kind);
            }
            if (operand_index > 2u)
                return false;
            return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
        }
        case XI_INDEX_SET:
        case XI_INDEX_GET:
            /* A byte-view receiver stays in its frozen VIEW storage. Arrays
             * remain owned tagged allocations, and all indices/elements keep
             * the native scalar storage bound for their own subject. */
            if (oracle_u8_slice_element_read_is_exact(ctx, operation_index)) {
                if (operation->opcode != XI_INDEX_GET)
                    return false;
                if (operand_index == 0)
                    return oracle_u8_slice_parameter_storage(ctx, source_value, out_storage,
                                                             &ignored_kind);
                return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
            }
            if (oracle_fixed_array_element_access_is_exact(ctx, operation_index)) {
                if (operand_index == 0)
                    return oracle_fixed_array_value_storage(ctx, source_value, out_storage,
                                                            &ignored_kind);
                return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
            }
            if (oracle_range_slice_element_read_is_exact(ctx, operation_index)) {
                if (operand_index == 0)
                    return oracle_range_slice_result_storage(ctx, source_value, out_storage,
                                                             &ignored_kind);
                return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
            }
            if (!oracle_array_element_access_is_exact(ctx, operation_index))
                return false;
            if (operand_index == 0)
                return oracle_array_tagged_carrier_storage(ctx, source_value, out_storage,
                                                           &ignored_kind) ||
                       oracle_direct_local_array_ref_parameter_place_storage(
                           ctx, source_value, out_storage, &ignored_kind);
            return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
        case XI_LOAD_FIELD:
            /* The receiver is the tagged instance the construction family
             * proved, or the one a parameter bound on entry; the read has no
             * other operand. Which of the two it is decides which oracle
             * re-proves the row, and neither may answer for the other. */
            if (operand_index != 0 || xr_semantic_class_field_read_source_class(
                                          ctx->semantic, operation) == XR_SEMANTIC_INDEX_NONE)
                return false;
            if (ctx->parameter_by_value[source_value] != XR_SEMANTIC_INDEX_NONE)
                return oracle_dynamic_source_class_parameter_storage(ctx, source_value, out_storage,
                                                                     &ignored_kind);
            return oracle_dynamic_source_class_instance_storage(ctx, source_value, out_storage,
                                                                &ignored_kind);
        case XI_STORE_FIELD:
            /* The receiver is the tagged instance a parameter bound on entry;
             * the stored value keeps its own native scalar storage. A field
             * whose type has no storage row stays without authority rather than
             * falling back to a tagged guess, so a class that stores a field
             * this authority cannot represent is refused here. */
            if (operand_index > 1 || xr_semantic_class_field_store_source_class(
                                         ctx->semantic, operation) == XR_SEMANTIC_INDEX_NONE)
                return false;
            if (operand_index == 0)
                return oracle_dynamic_source_class_parameter_storage(ctx, source_value, out_storage,
                                                                     &ignored_kind);
            return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
        case XI_UNBOX:
        case XI_ENUM_DESCRIPTOR_UNBOX:
            *out_storage = XR_REP_TAGGED;
            return true;
        default:
            break;
    }
    /* This use site names no carrier of its own, so it consumes the operand in
     * the storage the operand already occupies: no adapter, because there are
     * not two storages to adapt between.
     *
     * A carrier a use site does require -- a boxed argument, a tagged channel
     * payload, a tuple lane -- is stated by that site's own branch above and
     * keeps its priority. What was left over was the opposite case, an operand
     * a native operation takes exactly as the TargetPlan already froze it, and
     * that judgement had been written out once per consuming opcode: thirty-odd
     * spellings of one rule, with every opcode nobody had reached yet refusing
     * a native operand for want of a branch naming it.
     *
     * Fail-closed is kept on the storage rather than on the opcode. An operand
     * whose representation the plan never froze, or froze as a class this pass
     * names no storage for, is refused here exactly as before; the reference
     * families that carry those answer above. */
    if (oracle_dynamic_heap_literal_storage(ctx, source_value, out_storage, &ignored_kind))
        return true;
    return oracle_machine_storage(ctx, source_value, out_storage, &ignored_kind);
}

static bool authority_add_obligation(CollectContext *ctx, const VerifyAuthority *oracle,
                                     uint32_t source_value, uint32_t use_operation,
                                     uint32_t use_block, uint16_t use_operand, uint16_t use_kind,
                                     XrRep input_storage, XrRep output_storage) {
    if (input_storage == output_storage)
        return true;
    XrRep native_storage = XR_REP_TAGGED;
    uint16_t machine_kind = XR_MACHINE_REP_COUNT;
    if (!oracle_machine_storage(oracle, source_value, &native_storage, &machine_kind) ||
        ((input_storage != XR_REP_TAGGED) && input_storage != native_storage) ||
        ((output_storage != XR_REP_TAGGED) && output_storage != native_storage) ||
        (input_storage != XR_REP_TAGGED && output_storage != XR_REP_TAGGED)) {
        rep_trace_refusal(oracle, "the adapter contract",
                          "definition and use disagree, and the pair is not one the box/unbox "
                          "adapter can bridge: exactly one side must be tagged and the other must "
                          "be the value's own native storage",
                          source_value, use_operation, use_operand, input_storage, output_storage);
        set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE, ctx->record_count,
                 source_value, use_operation);
        return false;
    }
    uint32_t operation_index = ctx->operation_by_value[source_value];
    uint32_t parameter_index = ctx->parameter_by_value[source_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    const XrSemanticParameterRecord *parameter =
        parameter_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(ctx->semantic, parameter_index)
            : NULL;
    uint32_t type_index = parameter   ? parameter->type
                          : operation ? operation->result_type
                                      : XR_SEMANTIC_INDEX_NONE;
    uint32_t layout =
        type_index < ctx->type_count ? ctx->layout_by_type[type_index] : XR_SEMANTIC_INDEX_NONE;
    if ((!parameter && !operation) || layout == XR_SEMANTIC_INDEX_NONE) {
        rep_trace_refusal(oracle, "the adapter layout lookup",
                          (!parameter && !operation)
                              ? "the value has neither a parameter nor an operation to take a "
                                "type from"
                              : "the TargetPlan binds no layout for the value's semantic type, so "
                                "no adapter can be built for it",
                          source_value, use_operation, use_operand, input_storage, output_storage);
        set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE, ctx->record_count,
                 source_value, use_operation);
        return false;
    }
    bool box = input_storage != XR_REP_TAGGED;
    XrAotRepresentationAdapterRequest request = {
        .source_value = source_value,
        .use_operation = use_operation,
        .use_block = use_block,
        .use_operand = use_operand,
        .use_kind = use_kind,
        .adapter_kind = box ? XR_AOT_REP_ADAPTER_BOX : XR_AOT_REP_ADAPTER_UNBOX,
        .input_rep_kind = box ? machine_kind : XR_MACHINE_REP_DYN_VALUE,
        .output_rep_kind = box ? XR_MACHINE_REP_DYN_VALUE : machine_kind,
        .layout = layout,
        .policy_fingerprint = rep_policy_fingerprint(ctx->policy),
    };
    uint32_t decision = XR_AOT_REFINEMENT_REFUSED;
    if (!xr_aot_refinement_try_representation_adapter(
            ctx->builder, &ctx->protocol, ctx->target_plan, &request, &decision, ctx->diag) ||
        decision != XR_AOT_REFINEMENT_APPLIED) {
        if (decision != XR_AOT_REFINEMENT_APPLIED) {
            rep_trace_refusal(oracle, "the representation adapter",
                              box ? "the protocol has no BOX adapter for this machine rep and "
                                    "layout"
                                  : "the protocol has no UNBOX adapter for this machine rep and "
                                    "layout",
                              source_value, use_operation, use_operand, input_storage,
                              output_storage);
            set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                     ctx->record_count, source_value, use_operation);
        }
        return false;
    }
    ctx->record_count++;
    return true;
}

/* An `Array<T>` an operation in this function produced, so the operation that
 * produced it is the allocation a receiver check can compare against: freshly
 * built, handed back by an intrinsic, filled from a scalar, returned by a
 * higher-order call, or returned by a direct-local call. */
static bool oracle_array_produced_tagged_carrier_storage(const VerifyAuthority *ctx,
                                                         uint32_t semantic_value,
                                                         XrRep *out_storage,
                                                         uint16_t *out_machine_kind) {
    if (!oracle_resolve_identity_rename(ctx, semantic_value, &semantic_value))
        return false;
    return oracle_dynamic_array_allocation_storage(ctx, semantic_value, out_storage,
                                                   out_machine_kind) ||
           oracle_dynamic_array_intrinsic_storage(ctx, semantic_value, out_storage,
                                                  out_machine_kind) ||
           oracle_dynamic_array_fill_scalar_storage(ctx, semantic_value, out_storage,
                                                    out_machine_kind) ||
           oracle_array_hof_result_storage(ctx, semantic_value, out_storage, out_machine_kind) ||
           oracle_dynamic_direct_local_array_result_storage(ctx, semantic_value, out_storage,
                                                            out_machine_kind);
}

/* An `Array<T>` someone else owns, reaching this function in the tagged carrier:
 * read back out of the shared cell it was bound to, or borrowed as a by-value
 * parameter. Neither has a producing operation here, so a receiver check that
 * needs one proves the type from the value's own row instead.
 *
 * The ref-place carrier is deliberately absent: it is a pointer, not a tagged
 * value, and the sites that accept it name it separately. */
static bool oracle_array_borrowed_tagged_carrier_storage(const VerifyAuthority *ctx,
                                                         uint32_t semantic_value,
                                                         XrRep *out_storage,
                                                         uint16_t *out_machine_kind) {
    if (!oracle_resolve_identity_rename(ctx, semantic_value, &semantic_value))
        return false;
    return oracle_dynamic_array_ref_storage(ctx, semantic_value, out_storage, out_machine_kind) ||
           oracle_direct_local_array_value_parameter_storage(ctx, semantic_value, out_storage,
                                                             out_machine_kind);
}

/* Every way an `Array<T>` can reach a use site already holding its tagged
 * carrier. Each use that has to recognise an Array asks this one judgement
 * instead of respelling the list, so a carrier one use site admits can never be
 * one another refuses; a use that also cares whether the Array was produced
 * here asks the two halves separately rather than writing its own list.
 *
 * Each half answers about the value a name resolves to, so an Array reached
 * under a second name keeps the side its allocation came from. */
static bool oracle_array_tagged_carrier_storage(const VerifyAuthority *ctx, uint32_t semantic_value,
                                                XrRep *out_storage, uint16_t *out_machine_kind) {
    return oracle_array_produced_tagged_carrier_storage(ctx, semantic_value, out_storage,
                                                        out_machine_kind) ||
           oracle_array_borrowed_tagged_carrier_storage(ctx, semantic_value, out_storage,
                                                        out_machine_kind);
}

/* Every way a String can reach a use site already holding its tagged carrier:
 * a literal, a concatenation the caller just built, a String a direct-local
 * call handed back, one read back out of the shared cell it was bound to, or
 * one borrowed as a by-value parameter. A String has no other carrier, so every
 * site that accepts one in that carrier asks this one list, and a String the
 * equality test admits can never be one a call argument refuses.
 *
 * The list answers about the value a name resolves to, so a String reached
 * under a second name is the String it renames and not a family of its own. */
static bool oracle_string_tagged_carrier_storage(const VerifyAuthority *ctx,
                                                 uint32_t semantic_value, XrRep *out_storage,
                                                 uint16_t *out_machine_kind) {
    if (!oracle_resolve_identity_rename(ctx, semantic_value, &semantic_value))
        return false;
    return oracle_dynamic_heap_literal_storage(ctx, semantic_value, out_storage,
                                               out_machine_kind) ||
           oracle_dynamic_string_concat_result_storage(ctx, semantic_value, out_storage,
                                                       out_machine_kind) ||
           oracle_dynamic_string_convert_result_storage(ctx, semantic_value, out_storage,
                                                        out_machine_kind) ||
           oracle_dynamic_direct_local_string_result_storage(ctx, semantic_value, out_storage,
                                                             out_machine_kind) ||
           oracle_dynamic_string_shared_read_storage(ctx, semantic_value, out_storage,
                                                     out_machine_kind) ||
           oracle_direct_local_string_value_parameter_storage(ctx, semantic_value, out_storage,
                                                              out_machine_kind);
}

/* Every reference family this authority can name whose single storage fact is
 * the tagged carrier. A use site that consumes a reference without caring which
 * family it belongs to -- a refcount adjustment, a store into a shared cell, a
 * print -- asks this one judgement instead of respelling the families, so a
 * reference one such site admits can never be one another refuses.
 *
 * Native scalars are deliberately absent: a scalar has its own machine storage
 * and the sites that also accept one name `oracle_machine_storage` themselves.
 * The array ref-place carrier is absent for the same reason its own list omits
 * it -- it is a pointer, not a tagged value.
 *
 * The channel and namespace families gate on the indexes their own oracles
 * re-check internally, so no caller repeats the guard.
 *
 * Like the per-container lists it is built from, this one answers about the
 * value a name resolves to: a reference reached under a second name belongs to
 * the family of the value it renames. */
static bool oracle_tagged_reference_carrier_storage(const VerifyAuthority *ctx,
                                                    uint32_t semantic_value, XrRep *out_storage,
                                                    uint16_t *out_machine_kind) {
    if (!oracle_resolve_identity_rename(ctx, semantic_value, &semantic_value))
        return false;
    return oracle_string_tagged_carrier_storage(ctx, semantic_value, out_storage,
                                                out_machine_kind) ||
           oracle_array_tagged_carrier_storage(ctx, semantic_value, out_storage,
                                               out_machine_kind) ||
           oracle_dynamic_closure_storage(ctx, semantic_value, out_storage, out_machine_kind) ||
           oracle_dynamic_panic_catch_storage(ctx, semantic_value, out_storage, out_machine_kind) ||
           oracle_dynamic_stringbuilder_storage(ctx, semantic_value, out_storage,
                                                out_machine_kind) ||
           oracle_dynamic_string_runes_storage(ctx, semantic_value, out_storage,
                                               out_machine_kind) ||
           oracle_dynamic_string_slice_range_storage(ctx, semantic_value, out_storage,
                                                     out_machine_kind) ||
           oracle_fixed_array_backing_storage(ctx, semantic_value, out_storage, out_machine_kind) ||
           oracle_value_aggregate_storage(ctx, semantic_value, out_storage, out_machine_kind) ||
           oracle_dynamic_struct_object_storage(ctx, semantic_value, out_storage,
                                                out_machine_kind) ||
           oracle_struct_object_field_read_storage(ctx, semantic_value, out_storage,
                                                   out_machine_kind) ||
           oracle_dynamic_source_class_object_storage(ctx, semantic_value, out_storage,
                                                      out_machine_kind) ||
           oracle_dynamic_source_class_instance_storage(ctx, semantic_value, out_storage,
                                                        out_machine_kind) ||
           oracle_dynamic_json_namespace_value_storage(ctx, semantic_value, out_storage,
                                                       out_machine_kind) ||
           oracle_dynamic_direct_local_go_task_storage(ctx, semantic_value, out_storage,
                                                       out_machine_kind) ||
           oracle_dynamic_channel_storage(ctx, semantic_value, out_storage, out_machine_kind) ||
           oracle_source_namespace_storage(ctx, semantic_value, out_storage, out_machine_kind) ||
           oracle_native_module_namespace_storage(ctx, semantic_value, out_storage,
                                                  out_machine_kind);
}

/* The storage a returned value carries. A scalar return keeps its own native
 * storage, and the reference returns this authority can name are tagged: a
 * closure, a string literal, the receiver a constructor hands back, and a
 * direct local string result. The collecting pass and the verifying pass ask
 * this one judgement rather than each spelling the same chain, so a return one
 * admits can never be a return the other refuses. */
static bool oracle_return_storage(const VerifyAuthority *ctx, uint32_t value, XrRep *out_storage,
                                  uint16_t *out_machine_kind) {
    return oracle_machine_storage(ctx, value, out_storage, out_machine_kind) ||
           oracle_u8_slice_parameter_storage(ctx, value, out_storage, out_machine_kind) ||
           oracle_nullable_scalar_storage(ctx, value, out_storage, out_machine_kind) ||
           oracle_dynamic_closure_storage(ctx, value, out_storage, out_machine_kind) ||
           oracle_dynamic_panic_catch_storage(ctx, value, out_storage, out_machine_kind) ||
           oracle_dynamic_heap_literal_storage(ctx, value, out_storage, out_machine_kind) ||
           oracle_dynamic_string_runes_storage(ctx, value, out_storage, out_machine_kind) ||
           oracle_dynamic_string_slice_range_storage(ctx, value, out_storage, out_machine_kind) ||
           oracle_dynamic_string_concat_result_storage(ctx, value, out_storage, out_machine_kind) ||
           oracle_dynamic_string_convert_result_storage(ctx, value, out_storage,
                                                        out_machine_kind) ||
           oracle_array_hof_result_storage(ctx, value, out_storage, out_machine_kind) ||
           oracle_dynamic_source_class_parameter_storage(ctx, value, out_storage,
                                                         out_machine_kind) ||
           oracle_dynamic_source_class_instance_storage(ctx, value, out_storage,
                                                        out_machine_kind) ||
           oracle_dynamic_direct_local_string_result_storage(ctx, value, out_storage,
                                                             out_machine_kind) ||
           oracle_array_tagged_carrier_storage(ctx, value, out_storage, out_machine_kind);
}

static bool authority_collect_obligations_indexed(CollectContext *ctx,
                                                  const VerifyAuthority *oracle) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(ctx->semantic);
    bool survey = rep_survey_enabled();
    uint32_t refused_operands = 0;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(ctx->semantic, i);
        if (!operation || operation->operand_begin > operand_count ||
            operation->operand_count > operand_count - operation->operand_begin) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE, ctx->record_count, 0, i);
            return false;
        }
        for (uint16_t a = 0; a < operation->operand_count; a++) {
            uint32_t source_value = operands[operation->operand_begin + a].value;
            XrRep input_storage = XR_REP_TAGGED;
            XrRep output_storage = XR_REP_TAGGED;
            uint16_t ignored_machine = XR_MACHINE_REP_COUNT;
            if (source_value >= ctx->semantic_value_count) {
                set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE, ctx->record_count, source_value, i);
                return false;
            }
            bool has_definition =
                oracle_definition_storage(oracle, source_value, &input_storage, &ignored_machine);
            bool has_use =
                has_definition && oracle_use_storage(oracle, i, a, source_value, &output_storage);
            if (!has_definition || !has_use) {
                rep_trace_refusal(
                    oracle, has_definition ? "the use-site oracle" : "the definition oracle",
                    has_definition
                        ? "the use site admits no storage for this operand: its opcode branch in "
                          "oracle_use_storage names no family that covers the value"
                        : "no definition oracle names this value, so the pass never learns what "
                          "storage it is already in",
                    source_value, i, a, has_definition ? input_storage : XR_REP_COUNT,
                    XR_REP_COUNT);
                /* A definition refusal has two very different causes and the
                 * sentence above cannot tell them apart. Either the TargetPlan
                 * bound this value nothing at all -- the gap is a missing
                 * storage family one layer up, and no oracle here can close it
                 * -- or it bound a row this pass declines to read as a scalar
                 * carrier, which is a gap in this pass. Naming which one turns
                 * a refusal into a work item for the right layer. */
                if (!has_definition && rep_trace_enabled()) {
                    const XrTargetValueRepRecord *bound =
                        xr_target_plan_value_rep(ctx->target_plan, source_value);
                    const XrTargetMachineRepRecord *bound_rep =
                        bound ? xr_target_plan_machine_rep(ctx->target_plan, bound->register_rep)
                              : NULL;
                    uint32_t definition = ctx->operation_by_value[source_value];
                    const XrSemanticOperationRecord *producer =
                        definition != XR_SEMANTIC_INDEX_NONE
                            ? xr_semantic_plan_operation(ctx->semantic, definition)
                            : NULL;
                    const XrSemanticTypeRecord *value_type = xr_semantic_plan_type(
                        ctx->semantic, producer ? producer->result_type : XR_SEMANTIC_INDEX_NONE);
                    fprintf(stderr,
                            "[aot-refine]   definition gap: bound=%s rep_kind=%u type_kind=%u "
                            "children=%u aggregate=%u flags=%u\n",
                            bound ? "target-plan" : "NONE", bound_rep ? bound_rep->kind : 9999u,
                            value_type ? value_type->kind : 9999u,
                            value_type ? value_type->child_count : 9999u,
                            value_type ? value_type->aggregate_extent : 9999u,
                            value_type ? value_type->flags : 9999u);
                }
                set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                         ctx->record_count, source_value, i);
                refused_operands++;
                if (!survey)
                    return false;
                rep_survey_row(ctx, has_definition ? "use_site" : "definition", source_value, i, a);
                continue;
            }
            if (!authority_add_obligation(ctx, oracle, source_value, i, operation->block, a,
                                          XR_AOT_REP_USE_OPERATION, input_storage, output_storage))
                return false;
        }
    }
    uint32_t block_count = (uint32_t) xr_semantic_plan_block_count(ctx->semantic);
    for (uint32_t i = 0; i < block_count; i++) {
        const XrSemanticBlockRecord *block = xr_semantic_plan_block(ctx->semantic, i);
        if (!block || block->kind != XI_BLOCK_RETURN ||
            block->control_value == XR_SEMANTIC_INDEX_NONE)
            continue;
        uint32_t source_operation = block->control_value < ctx->semantic_value_count
                                        ? ctx->operation_by_value[block->control_value]
                                        : XR_SEMANTIC_INDEX_NONE;
        const XrSemanticOperationRecord *operation =
            source_operation != XR_SEMANTIC_INDEX_NONE
                ? xr_semantic_plan_operation(ctx->semantic, source_operation)
                : NULL;
        if (operation && (operation->opcode == XI_ERR_RETURN ||
                          (oracle->exact_source_namespace_value &&
                           oracle->exact_source_namespace_value[block->control_value])))
            continue;
        XrRep input_storage = XR_REP_TAGGED;
        XrRep output_storage = XR_REP_TAGGED;
        uint16_t machine_kind = XR_MACHINE_REP_COUNT;
        if (!oracle_definition_storage(oracle, block->control_value, &input_storage,
                                       &machine_kind)) {
            rep_trace_refusal(oracle, "the definition oracle",
                              "no definition oracle names the value this block returns",
                              block->control_value, XR_SEMANTIC_INDEX_NONE, 0, XR_REP_COUNT,
                              XR_REP_COUNT);
            set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                     ctx->record_count, block->control_value, XR_SEMANTIC_INDEX_NONE);
            refused_operands++;
            if (!survey)
                return false;
            rep_survey_row(ctx, "definition_at_return", block->control_value,
                           XR_SEMANTIC_INDEX_NONE, 0);
            continue;
        }
        /* A returned owned String is tagged storage. Each oracle re-proves its
         * own exact TargetPlan row, so no unproven reference return reaches
         * this path. */
        if (!ctx->policy->force_return_tagged &&
            !oracle_return_storage(oracle, block->control_value, &output_storage, &machine_kind)) {
            rep_trace_refusal(oracle, "the return-storage oracle",
                              "oracle_return_storage names no carrier a return may hand this "
                              "value back in",
                              block->control_value, XR_SEMANTIC_INDEX_NONE, 0, input_storage,
                              XR_REP_COUNT);
            set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                     ctx->record_count, block->control_value, XR_SEMANTIC_INDEX_NONE);
            refused_operands++;
            if (!survey)
                return false;
            rep_survey_row(ctx, "return_storage", block->control_value, XR_SEMANTIC_INDEX_NONE, 0);
            continue;
        }
        if (!authority_add_obligation(ctx, oracle, block->control_value, XR_SEMANTIC_INDEX_NONE, i,
                                      0, XR_AOT_REP_USE_BLOCK_CONTROL, input_storage,
                                      output_storage))
            return false;
    }
    if (refused_operands) {
        fprintf(stderr, "[refusal-survey] refinement operands refused: %u\n", refused_operands);
        return false;
    }
    return true;
}

static bool authority_collect_obligations(CollectContext *ctx) {
    VerifyAuthority oracle = {
        .target_plan = ctx->target_plan,
        .semantic = ctx->semantic,
        .policy = ctx->policy,
        .diag = ctx->diag,
        .function_count = (uint32_t) xr_semantic_plan_function_count(ctx->semantic),
        .block_count = (uint32_t) xr_semantic_plan_block_count(ctx->semantic),
        .value_count = ctx->semantic_value_count,
        .operation_count = (uint32_t) xr_semantic_plan_operation_count(ctx->semantic),
        .parameter_count = (uint32_t) xr_semantic_plan_parameter_count(ctx->semantic),
        .type_count = ctx->type_count,
        .operation_by_value = ctx->operation_by_value,
        .parameter_by_value = ctx->parameter_by_value,
        .use_count_by_value = ctx->use_count_by_value,
        .call_by_operation = ctx->call_by_operation,
        .layout_by_type = ctx->layout_by_type,
    };
    bool valid = aot_index_direct_local_callee_values(&oracle) &&
                 aot_index_direct_local_go_callee_values(&oracle) &&
                 aot_index_source_namespace_values(&oracle) &&
                 aot_index_native_module_namespace_values(&oracle) &&
                 aot_index_channel_values(&oracle) &&
                 authority_collect_obligations_indexed(ctx, &oracle);
    xr_free(oracle.direct_callee_target_by_value);
    xr_free(oracle.exact_direct_callee_value);
    xr_free(oracle.go_callee_target_by_value);
    xr_free(oracle.exact_go_callee_value);
    xr_free(oracle.exact_channel_value);
    xr_free(oracle.exact_channel_allocation_value);
    xr_free(oracle.exact_source_namespace_value);
    xr_free(oracle.source_namespace_dependency_by_value);
    xr_free(oracle.exact_native_module_namespace_value);
    return valid;
}

bool xr_aot_representation_refinement_build_from_authority(const XrTargetPlan *target_plan,
                                                           const XiRepPolicy *policy,
                                                           XrAotRefinementPlan **out_plan,
                                                           XrAotRefinementDiagnostic *diag) {
    if (out_plan)
        *out_plan = NULL;
    if (diag)
        memset(diag, 0, sizeof(*diag));
    if (!target_plan || !out_plan) {
        set_diag(diag, XR_AOT_REFINEMENT_INVALID_ARGUMENT, 0, 0, 0);
        return false;
    }
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    XrAotRefinementBuilder *builder =
        semantic ? xr_aot_refinement_builder_create(target_plan, diag) : NULL;
    if (!builder) {
        if (diag && diag->issue == XR_AOT_REFINEMENT_OK)
            set_diag(diag, XR_AOT_REFINEMENT_PLAN_STATE, 0, 0, 0);
        return false;
    }
    XiRepPolicy default_policy = xi_rep_policy_native_boundary();
    CollectContext ctx = {
        .target_plan = target_plan,
        .semantic = semantic,
        .policy = policy ? policy : &default_policy,
        .builder = builder,
        .protocol = xr_aot_refinement_representation_protocol(27902),
        .diag = diag,
    };
    if (!collect_indices_init(&ctx)) {
        set_diag(diag, XR_AOT_REFINEMENT_RESOURCE_BUDGET, 0, 0, 0);
        xr_aot_refinement_builder_free(builder);
        return false;
    }
    bool ok = authority_collect_obligations(&ctx) &&
              xr_aot_refinement_builder_freeze(builder, target_plan, out_plan, diag);
    collect_indices_dispose(&ctx);
    xr_aot_refinement_builder_free(builder);
    return ok;
}

static void materialized_function_matches(const XiFunc *function, uint32_t index,
                                          const XiFunc **out, uint32_t *matches) {
    if (!function)
        return;
    if (function->semantic_plan_function_index == index) {
        *out = function;
        (*matches)++;
    }
    for (uint16_t i = 0; i < function->nchildren; i++)
        materialized_function_matches(function->children[i], index, out, matches);
}

static const XiFunc *materialized_function_by_index(const XiFunc *function, uint32_t index) {
    const XiFunc *match = NULL;
    uint32_t matches = 0;
    materialized_function_matches(function, index, &match, &matches);
    return matches == 1 ? match : NULL;
}

static const XiValue *materialized_value_by_id(const XiFunc *function, uint32_t id,
                                               uint32_t *out_matches) {
    uint32_t matches = 0;
    const XiValue *found = NULL;
    if (!function)
        return NULL;
    for (uint32_t b = 0; b < function->nblocks; b++) {
        const XiBlock *block = function->blocks[b];
        if (!block)
            continue;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            if (phi->value.id == id) {
                found = &phi->value;
                matches++;
            }
        }
        for (uint32_t v = 0; v < block->nvalues; v++) {
            if (block->values[v] && block->values[v]->id == id) {
                found = block->values[v];
                matches++;
            }
        }
    }
    if (out_matches)
        *out_matches = matches;
    return matches == 1 ? found : NULL;
}

static bool materialized_adapter_kind_matches(const XrAotRepresentationAdapterRecord *record,
                                              const XiValue *adapter) {
    if (!record || !adapter)
        return false;
    switch (record->adapter_kind) {
        case XR_AOT_REP_ADAPTER_BOX:
            return adapter->op == XI_BOX && adapter->backend_origin == XI_BACKEND_VALUE_REP_BOX;
        case XR_AOT_REP_ADAPTER_UNBOX:
            return adapter->op == XI_UNBOX && adapter->backend_origin == XI_BACKEND_VALUE_REP_UNBOX;
        default:
            return false;
    }
}

static bool materialized_operation_shape_matches(const XrSemanticOperationRecord *operation,
                                                 const XiValue *value, uint32_t local_value) {
    return operation && value && value->backend_origin == XI_BACKEND_VALUE_NONE &&
           value->id == local_value && operation->opcode == value->op &&
           operation->operand_count == value->nargs &&
           operation->auxiliary_kind == value->aux_kind &&
           operation->semantic_immediate == value->aux_int &&
           operation->source_line == value->line &&
           operation->transfer_mode == value->transfer_mode &&
           operation->parameter_mode == value->param_mode && operation->flags == value->flags &&
           operation->result_alias_operand == value->result_alias_operand;
}

/* Representation refresh mechanically propagates and DCEs identity COPY
 * nodes.  Resolve an exact frozen namespace COPY to the nearest retained
 * materialized value so the verifier checks the published graph rather than
 * requiring an optimizer-dead Xi node to survive. */
static const XiValue *materialized_source_namespace_value(const VerifyAuthority *ctx,
                                                          uint32_t semantic_value) {
    if (!ctx || semantic_value >= ctx->value_count)
        return NULL;
    for (uint32_t depth = 0; depth < ctx->operation_count; depth++) {
        const XiValue *live = ctx->live_by_value[semantic_value];
        if (live)
            return live;
        if (!ctx->exact_source_namespace_value ||
            !ctx->exact_source_namespace_value[semantic_value])
            return NULL;
        uint32_t operation_index = ctx->operation_by_value[semantic_value];
        const XrSemanticOperationRecord *operation =
            operation_index != XR_SEMANTIC_INDEX_NONE
                ? xr_semantic_plan_operation(ctx->semantic, operation_index)
                : NULL;
        uint32_t operand_count = 0;
        const XrSemanticOperandRecord *operands =
            xr_semantic_plan_operands(ctx->semantic, &operand_count);
        if (!operation || operation->opcode != XI_COPY || !operands ||
            operation->operand_count != 1 || operation->operand_begin >= operand_count)
            return NULL;
        semantic_value = operands[operation->operand_begin].value;
        if (semantic_value >= ctx->value_count)
            return NULL;
    }
    return NULL;
}

static bool index_materialized_callee_authority(VerifyAuthority *ctx, const XiFunc *function,
                                                const XiFunc **function_by_index, uint32_t depth) {
    if (!ctx || !function || !function_by_index || depth > XR_AOT_REP_VERIFY_MAX_FUNCTION_DEPTH ||
        !verify_charge_work(ctx, 1) || function->semantic_plan != ctx->semantic ||
        function->semantic_plan_function_index >= ctx->function_count)
        return false;
    uint32_t function_index = function->semantic_plan_function_index;
    const XrSemanticFunctionRecord *semantic_function =
        xr_semantic_plan_function(ctx->semantic, function_index);
    if (!semantic_function || function_by_index[function_index] ||
        ctx->seen_function[function_index])
        return false;
    function_by_index[function_index] = function;
    ctx->seen_function[function_index] = 1;
    for (uint32_t b = 0; b < function->nblocks; b++) {
        const XiBlock *block = function->blocks[b];
        if (!block || block->func != function)
            return false;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            const XiValue *value = &phi->value;
            if (value->backend_origin != XI_BACKEND_VALUE_NONE)
                continue;
            if (value->id >= semantic_function->value_count ||
                semantic_function->value_begin > UINT32_MAX - value->id)
                return false;
            uint32_t semantic_value = semantic_function->value_begin + value->id;
            if (semantic_value >= ctx->value_count || ctx->live_by_value[semantic_value])
                return false;
            ctx->live_by_value[semantic_value] = value;
        }
        for (uint32_t v = 0; v < block->nvalues; v++) {
            const XiValue *value = block->values[v];
            if (!value)
                return false;
            if (value->backend_origin != XI_BACKEND_VALUE_NONE)
                continue;
            if (value->id >= semantic_function->value_count ||
                semantic_function->value_begin > UINT32_MAX - value->id)
                return false;
            uint32_t semantic_value = semantic_function->value_begin + value->id;
            if (semantic_value >= ctx->value_count || ctx->live_by_value[semantic_value])
                return false;
            ctx->live_by_value[semantic_value] = value;
        }
    }
    for (uint16_t i = 0; i < function->nchildren; i++)
        if (!index_materialized_callee_authority(ctx, function->children[i], function_by_index,
                                                 depth + 1u))
            return false;
    return true;
}

static bool verify_exact_dynamic_storage_materialization(const XrAotRefinementPlanView *view,
                                                         const XiFunc *root,
                                                         const XrTargetPlan *target_plan,
                                                         const XiRepPolicy *policy,
                                                         XrAotRefinementDiagnostic *diag) {
    VerifyAuthority ctx = {
        .target_plan = target_plan,
        .semantic = xr_target_plan_semantic_plan(target_plan),
        .policy = policy,
        .view = view,
        .diag = diag,
        .policy_fingerprint = rep_policy_fingerprint(policy),
    };
    const XiFunc **function_by_index = NULL;
    bool valid = verify_authority_init(&ctx);
    if (valid && ctx.function_count)
        valid = verify_alloc(&ctx, ctx.function_count, sizeof(*function_by_index),
                             (void **) &function_by_index);
    if (valid)
        valid = index_materialized_callee_authority(&ctx, root, function_by_index, 0);
    for (uint32_t value = 0; valid && value < ctx.value_count; value++) {
        bool direct = ctx.exact_direct_callee_value[value] != 0;
        bool go = ctx.exact_go_callee_value[value] != 0;
        bool source_namespace =
            ctx.exact_source_namespace_value && ctx.exact_source_namespace_value[value] != 0;
        if (!direct && !go && !source_namespace)
            continue;
        uint32_t operation_index = ctx.operation_by_value[value];
        const XrSemanticOperationRecord *operation =
            operation_index == XR_SEMANTIC_INDEX_NONE
                ? NULL
                : xr_semantic_plan_operation(ctx.semantic, operation_index);
        const XiFunc *owner = operation && operation->function < ctx.function_count
                                  ? function_by_index[operation->function]
                                  : NULL;
        const XrSemanticFunctionRecord *semantic_function =
            operation ? xr_semantic_plan_function(ctx.semantic, operation->function) : NULL;
        const XiValue *live = ctx.live_by_value[value];
        bool elided_source_copy =
            source_namespace && operation && operation->opcode == XI_COPY && !live;
        const XiValue *effective =
            elided_source_copy ? materialized_source_namespace_value(&ctx, value) : live;
        bool shape_ok =
            operation && semantic_function && owner && effective &&
            value >= semantic_function->value_begin &&
            (elided_source_copy || materialized_operation_shape_matches(
                                       operation, live, value - semantic_function->value_begin));
        bool authority_ok =
            shape_ok &&
            (elided_source_copy
                 ? source_type_matches(effective->type,
                                       xr_semantic_plan_type(ctx.semantic, operation->result_type))
                 : (direct ? verify_direct_local_callee_type_authority(&ctx, operation, owner, live)
                    : go
                        ? verify_direct_local_go_callee_type_authority(&ctx, operation, owner, live)
                        : verify_source_namespace_type_authority(&ctx, operation, live)));
        if (!operation || !semantic_function || !owner || !effective ||
            value < semantic_function->value_begin || !shape_ok || !authority_ok) {
            set_diag(diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, operation_index, value,
                     operation_index);
            valid = false;
        }
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx.semantic, &operand_count);
    for (uint32_t i = 0; valid && i < ctx.operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(ctx.semantic, i);
        const XrSemanticFunctionRecord *semantic_function =
            operation ? xr_semantic_plan_function(ctx.semantic, operation->function) : NULL;
        const XiValue *user = operation && operation->result_value < ctx.value_count
                                  ? ctx.live_by_value[operation->result_value]
                                  : NULL;
        bool elided_source_copy = operation && operation->opcode == XI_COPY &&
                                  operation->result_value < ctx.value_count && !user &&
                                  ctx.exact_source_namespace_value &&
                                  ctx.exact_source_namespace_value[operation->result_value];
        if (!operation || !semantic_function ||
            operation->result_value < semantic_function->value_begin ||
            operation->operand_begin > operand_count ||
            operation->operand_count > operand_count - operation->operand_begin) {
            valid = false;
            break;
        }
        for (uint16_t a = 0; valid && a < operation->operand_count; a++) {
            uint32_t source_value = operands[operation->operand_begin + a].value;
            if (source_value >= ctx.value_count ||
                (!ctx.exact_direct_callee_value[source_value] &&
                 !ctx.exact_go_callee_value[source_value] &&
                 !(ctx.exact_source_namespace_value &&
                   ctx.exact_source_namespace_value[source_value])))
                continue;
            if (elided_source_copy)
                continue;
            uint32_t local_value = operation->result_value - semantic_function->value_begin;
            const XiValue *materialized_source =
                materialized_source_namespace_value(&ctx, source_value);
            if (!user || !materialized_operation_shape_matches(operation, user, local_value) ||
                !user->args || a >= user->nargs ||
                user->args[a] !=
                    (materialized_source ? materialized_source : ctx.live_by_value[source_value]) ||
                !(ctx.exact_direct_callee_value[source_value]
                      ? oracle_direct_local_callee_use(&ctx, i, a, source_value)
                  : ctx.exact_go_callee_value[source_value]
                      ? oracle_direct_local_go_callee_use(&ctx, i, a, source_value)
                      : materialized_source && user->args[a] == materialized_source)) {
                set_diag(diag, XR_AOT_REFINEMENT_USE_SITE, i, source_value, i);
                valid = false;
            }
        }
    }
    if (!valid && diag && diag->issue == XR_AOT_REFINEMENT_OK)
        set_diag(diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, 0, 0, 0);
    xr_free(function_by_index);
    verify_authority_dispose(&ctx);
    return valid;
}

static bool verify_exact_cleanup_materialization(const XiFunc *root,
                                                 const XrTargetPlan *target_plan,
                                                 XrAotRefinementDiagnostic *diag) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    uint32_t cleanup_count = 0;
    uint32_t slot_count = 0;
    const XrTargetCleanupRecord *cleanups = xr_target_plan_cleanups(target_plan, &cleanup_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(target_plan, &slot_count);
    for (uint32_t i = 0; i < cleanup_count; i++) {
        const XrTargetCleanupRecord *cleanup = &cleanups[i];
        XrSemanticStringConcatReleaseShape shape = {0};
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, cleanup->semantic_operation);
        const XrSemanticFunctionRecord *semantic_function =
            xr_semantic_plan_function(semantic, cleanup->function);
        const XiFunc *function = materialized_function_by_index(root, cleanup->function);
        const XrTargetSlotRecord *slot = cleanup->slot < slot_count ? &slots[cleanup->slot] : NULL;
        uint32_t local_value = XR_SEMANTIC_INDEX_NONE;
        uint32_t matches = 0;
        const XiValue *release = NULL;
        if (operation && semantic_function &&
            operation->result_value >= semantic_function->value_begin &&
            operation->result_value - semantic_function->value_begin <
                semantic_function->value_count) {
            local_value = operation->result_value - semantic_function->value_begin;
            release = materialized_value_by_id(function, local_value, &matches);
        }
        uint32_t argument_function = XR_SEMANTIC_INDEX_NONE;
        uint32_t argument_value = XR_SEMANTIC_INDEX_NONE;
        char error[512] = {0};
        bool exact =
            cleanups && slots && cleanup->id == i && cleanup->action == XR_TARGET_CLEANUP_RELEASE &&
            cleanup->flags == 0 && cleanup->provider == 0 &&
            xr_semantic_string_concat_release_is_exact(semantic, cleanup->semantic_operation,
                                                       &shape) &&
            shape.function == cleanup->function && slot && slot->function == cleanup->function &&
            slot->semantic_value == shape.released_value && operation && semantic_function &&
            function && release && matches == 1 &&
            materialized_operation_shape_matches(operation, release, local_value) &&
            release->nargs == 1 && release->args && release->args[0] &&
            xr_aot_scalar_semantic_value_id(target_plan, function, release->args[0],
                                            &argument_function, &argument_value, error,
                                            sizeof(error)) &&
            argument_function == cleanup->function && argument_value == shape.released_value;
        if (!exact) {
            set_diag(diag, XR_AOT_REFINEMENT_USE_SITE, i, shape.released_value,
                     cleanup->semantic_operation);
            return false;
        }
    }
    return cleanup_count == 0 || (cleanups && slots);
}

static int compare_adapter_pointer(const void *left, const void *right) {
    uintptr_t a = (uintptr_t) *(const XiValue *const *) left;
    uintptr_t b = (uintptr_t) *(const XiValue *const *) right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static bool matched_adapter_contains(const XiValue *const *adapters, uint32_t count,
                                     const XiValue *value) {
    uint32_t low = 0;
    uint32_t high = count;
    uintptr_t needle = (uintptr_t) value;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        uintptr_t current = (uintptr_t) adapters[middle];
        if (current < needle)
            low = middle + 1u;
        else
            high = middle;
    }
    return low < count && adapters[low] == value;
}

static bool verify_exact_semantic_coverage(VerifyAuthority *ctx);

static bool verify_immutable_authority_coverage(const XrAotRefinementPlanView *view,
                                                const XrTargetPlan *target_plan,
                                                const XiRepPolicy *policy,
                                                XrAotRefinementDiagnostic *diag) {
    VerifyAuthority ctx = {
        .target_plan = target_plan,
        .semantic = xr_target_plan_semantic_plan(target_plan),
        .policy = policy,
        .view = view,
        .diag = diag,
        .policy_fingerprint = rep_policy_fingerprint(policy),
    };
    bool valid = verify_authority_init(&ctx) && verify_exact_semantic_coverage(&ctx);
    verify_authority_dispose(&ctx);
    return valid;
}

static bool verify_no_extra_materialized_adapters(const XiFunc *function,
                                                  const XiValue *const *matched,
                                                  uint32_t matched_count,
                                                  XrAotRefinementDiagnostic *diag) {
    if (!function)
        return false;
    for (uint32_t b = 0; b < function->nblocks; b++) {
        const XiBlock *block = function->blocks[b];
        if (!block)
            return false;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            if (phi->value.backend_origin != XI_BACKEND_VALUE_NONE &&
                !matched_adapter_contains(matched, matched_count, &phi->value)) {
                set_diag(diag, XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE, 0, phi->value.id,
                         XR_SEMANTIC_INDEX_NONE);
                return false;
            }
        }
        for (uint32_t v = 0; v < block->nvalues; v++) {
            const XiValue *value = block->values[v];
            if (value && value->backend_origin != XI_BACKEND_VALUE_NONE &&
                !matched_adapter_contains(matched, matched_count, value)) {
                set_diag(diag, XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE, 0, value->id,
                         XR_SEMANTIC_INDEX_NONE);
                return false;
            }
        }
    }
    for (uint16_t i = 0; i < function->nchildren; i++) {
        if (!verify_no_extra_materialized_adapters(function->children[i], matched, matched_count,
                                                   diag))
            return false;
    }
    return true;
}

bool xr_aot_representation_materialization_verify(const XrAotRefinementPlanView *view,
                                                  const XiFunc *root,
                                                  const XrTargetPlan *target_plan,
                                                  const XiRepPolicy *policy,
                                                  XrAotRefinementDiagnostic *diag) {
    if (diag)
        memset(diag, 0, sizeof(*diag));
    if (!view || !root || !target_plan ||
        root->semantic_plan != xr_target_plan_semantic_plan(target_plan) ||
        !xr_aot_refinement_verify(view, target_plan, diag)) {
        if (diag && diag->issue == XR_AOT_REFINEMENT_OK)
            set_diag(diag, XR_AOT_REFINEMENT_BASELINE_FINGERPRINT, 0, 0, 0);
        return false;
    }
    XiRepPolicy default_policy = xi_rep_policy_native_boundary();
    const XiRepPolicy *effective_policy = policy ? policy : &default_policy;
    if (!verify_immutable_authority_coverage(view, target_plan, effective_policy, diag))
        return false;
    if (!verify_exact_dynamic_storage_materialization(view, root, target_plan, effective_policy,
                                                      diag))
        return false;
    if (!verify_exact_cleanup_materialization(root, target_plan, diag))
        return false;
    XrFingerprint expected_policy = rep_policy_fingerprint(effective_policy);
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XiValue **matched = NULL;
    if (view->record_count) {
        matched = (const XiValue **) xr_calloc(view->record_count, sizeof(*matched));
        if (!matched) {
            set_diag(diag, XR_AOT_REFINEMENT_OUT_OF_MEMORY, 0, 0, 0);
            return false;
        }
    }
    bool valid = true;
    for (uint32_t i = 0; valid && i < view->record_count; i++) {
        const XrAotTransformationRecord *transformation = &view->records[i];
        const XrAotRepresentationAdapterRecord *record = &transformation->representation_adapter;
        if (transformation->transform_kind != XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER ||
            transformation->decision != XR_AOT_REFINEMENT_APPLIED ||
            !xr_fingerprint_equal(record->policy_fingerprint, expected_policy)) {
            set_diag(diag, XR_AOT_REFINEMENT_STALE_EVIDENCE, i, record->source_value,
                     record->use_operation);
            valid = false;
            break;
        }
        const XrSemanticFunctionRecord *semantic_function =
            xr_semantic_plan_function(semantic, record->source_function);
        const XiFunc *function = materialized_function_by_index(root, record->source_function);
        if (!semantic_function || !function ||
            record->source_value < semantic_function->value_begin ||
            record->source_value >=
                semantic_function->value_begin + semantic_function->value_count) {
            set_diag(diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, i, record->source_value,
                     record->use_operation);
            valid = false;
            break;
        }
        uint32_t source_local = record->source_value - semantic_function->value_begin;
        uint32_t source_matches = 0;
        const XiValue *source = materialized_value_by_id(function, source_local, &source_matches);
        uint32_t source_function = XR_SEMANTIC_INDEX_NONE;
        uint32_t source_value = XR_SEMANTIC_INDEX_NONE;
        if (!source || source_matches != 1 ||
            !xr_aot_scalar_semantic_value_id(target_plan, function, source, &source_function,
                                             &source_value, NULL, 0) ||
            source_function != record->source_function || source_value != record->source_value) {
            set_diag(diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, i, record->source_value,
                     record->use_operation);
            valid = false;
            break;
        }
        const XiValue *adapter = NULL;
        if (record->use_kind == XR_AOT_REP_USE_OPERATION) {
            const XrSemanticOperationRecord *use =
                xr_semantic_plan_operation(semantic, record->use_operation);
            const XrSemanticFunctionRecord *use_function =
                use ? xr_semantic_plan_function(semantic, use->function) : NULL;
            const XiFunc *live_use_function =
                use ? materialized_function_by_index(root, use->function) : NULL;
            uint32_t use_local =
                use && use_function && use->result_value >= use_function->value_begin
                    ? use->result_value - use_function->value_begin
                    : UINT32_MAX;
            uint32_t use_matches = 0;
            const XiValue *user =
                materialized_value_by_id(live_use_function, use_local, &use_matches);
            if (!use || !use_function || !live_use_function ||
                use->function != record->source_function || use->block != record->use_block ||
                use_matches != 1 || !materialized_operation_shape_matches(use, user, use_local) ||
                record->use_operand >= user->nargs || !user->args) {
                set_diag(diag, XR_AOT_REFINEMENT_USE_SITE, i, record->source_value,
                         record->use_operation);
                valid = false;
                break;
            }
            adapter = user->args[record->use_operand];
        } else if (record->use_kind == XR_AOT_REP_USE_BLOCK_CONTROL) {
            const XrSemanticBlockRecord *block =
                xr_semantic_plan_block(semantic, record->use_block);
            if (!block || block->function != record->source_function ||
                record->use_operation != XR_SEMANTIC_INDEX_NONE ||
                block->control_value != record->source_value ||
                block->function >= xr_semantic_plan_function_count(semantic) ||
                record->use_block < semantic_function->block_begin ||
                record->use_block >=
                    semantic_function->block_begin + semantic_function->block_count) {
                set_diag(diag, XR_AOT_REFINEMENT_USE_SITE, i, record->source_value,
                         record->use_operation);
                valid = false;
                break;
            }
            uint32_t local_block = record->use_block - semantic_function->block_begin;
            adapter = local_block < function->nblocks && function->blocks[local_block]
                          ? function->blocks[local_block]->control
                          : NULL;
        } else {
            set_diag(diag, XR_AOT_REFINEMENT_USE_SITE, i, record->source_value,
                     record->use_operation);
            valid = false;
            break;
        }
        if (!adapter || adapter->nargs != 1 || !adapter->args || adapter->args[0] != source ||
            !materialized_adapter_kind_matches(record, adapter) ||
            !xr_aot_rep_adapter_value_is_exact(target_plan, function, adapter, NULL, 0)) {
            set_diag(diag, XR_AOT_REFINEMENT_REPRESENTATION, i, record->source_value,
                     record->use_operation);
            valid = false;
            break;
        }
        matched[i] = adapter;
    }
    if (valid && view->record_count) {
        qsort(matched, view->record_count, sizeof(*matched), compare_adapter_pointer);
    }
    if (valid)
        valid = verify_no_extra_materialized_adapters(root, matched, view->record_count, diag);
    xr_free(matched);
    if (valid && diag)
        memset(diag, 0, sizeof(*diag));
    return valid;
}

static int compare_exact_key(const XrAotTransformationRecord *record, uint32_t source_function,
                             uint32_t source_value, uint32_t use_operation, uint32_t use_block,
                             uint16_t use_operand, uint16_t use_kind) {
    if (record->transform_kind != XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER)
        return record->transform_kind < XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER ? -1 : 1;
    const XrAotRepresentationAdapterRecord *adapter = &record->representation_adapter;
#define XR_COMPARE_EXACT(field, expected)                                                          \
    do {                                                                                           \
        if (adapter->field != (expected))                                                          \
            return adapter->field < (expected) ? -1 : 1;                                           \
    } while (0)
    XR_COMPARE_EXACT(source_function, source_function);
    XR_COMPARE_EXACT(use_kind, use_kind);
    XR_COMPARE_EXACT(use_block, use_block);
    XR_COMPARE_EXACT(use_operation, use_operation);
    XR_COMPARE_EXACT(use_operand, use_operand);
    XR_COMPARE_EXACT(source_value, source_value);
#undef XR_COMPARE_EXACT
    return 0;
}

static uint32_t find_exact_record(const VerifyAuthority *ctx, uint32_t source_function,
                                  uint32_t source_value, uint32_t use_operation, uint32_t use_block,
                                  uint16_t use_operand, uint16_t use_kind) {
    uint32_t low = 0;
    uint32_t high = ctx->view->record_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        int order = compare_exact_key(&ctx->view->records[middle], source_function, source_value,
                                      use_operation, use_block, use_operand, use_kind);
        if (order < 0)
            low = middle + 1u;
        else
            high = middle;
    }
    return low < ctx->view->record_count &&
                   compare_exact_key(&ctx->view->records[low], source_function, source_value,
                                     use_operation, use_block, use_operand, use_kind) == 0
               ? low
               : XR_SEMANTIC_INDEX_NONE;
}

static bool verify_exact_record_authority(VerifyAuthority *ctx, uint32_t record_index,
                                          uint32_t source_value, uint32_t use_operation,
                                          uint32_t use_block, uint16_t use_kind) {
    const XrAotRepresentationAdapterRecord *record =
        &ctx->view->records[record_index].representation_adapter;
    uint32_t operation_index = ctx->operation_by_value[source_value];
    uint32_t parameter_index = ctx->parameter_by_value[source_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    const XrSemanticParameterRecord *parameter =
        parameter_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(ctx->semantic, parameter_index)
            : NULL;
    if (!parameter && !operation) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, record_index, source_value,
                 operation_index);
        return false;
    }
    uint32_t source_function = parameter   ? parameter->function
                               : operation ? operation->function
                                           : XR_SEMANTIC_INDEX_NONE;
    uint32_t source_type = parameter   ? parameter->type
                           : operation ? operation->result_type
                                       : XR_SEMANTIC_INDEX_NONE;
    uint16_t source_kind = parameter ? XR_AOT_REP_SOURCE_PARAMETER : XR_AOT_REP_SOURCE_OPERATION;
    XrStableId source_id = parameter ? parameter->id : operation->id;
    uint8_t source_auxiliary = parameter ? 0 : operation->auxiliary_kind;
    uint8_t source_flags = parameter ? parameter->flags : operation->flags;
    int64_t source_immediate = parameter ? parameter->ordinal : operation->semantic_immediate;
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(ctx->semantic, source_type);
    if (!type || record->source_function != source_function ||
        record->source_value != source_value || record->source_operation != operation_index ||
        record->source_type != source_type || record->source_kind != source_kind ||
        record->source_auxiliary_kind != source_auxiliary || record->source_flags != source_flags ||
        record->source_semantic_immediate != source_immediate ||
        !xr_stable_id_equal(record->source_operation_id, source_id) ||
        !xr_stable_id_equal(record->source_type_id, type->id)) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, record_index, source_value,
                 operation_index);
        return false;
    }
    XrStableId use_id = {{0}};
    uint8_t use_auxiliary = 0;
    uint8_t use_flags = 0;
    int64_t use_immediate = 0;
    if (use_kind == XR_AOT_REP_USE_OPERATION) {
        const XrSemanticOperationRecord *use =
            xr_semantic_plan_operation(ctx->semantic, use_operation);
        if (!use || use->block != use_block) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE, record_index, source_value,
                     use_operation);
            return false;
        }
        use_id = use->id;
        use_auxiliary = use->auxiliary_kind;
        use_flags = use->flags;
        use_immediate = use->semantic_immediate;
    } else {
        const XrSemanticBlockRecord *block = xr_semantic_plan_block(ctx->semantic, use_block);
        if (!block || block->control_value != source_value) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE, record_index, source_value,
                     use_operation);
            return false;
        }
        use_id = block->id;
        use_flags = (uint8_t) block->kind;
    }
    if (record->use_auxiliary_kind != use_auxiliary || record->use_flags != use_flags ||
        record->use_semantic_immediate != use_immediate ||
        !xr_stable_id_equal(record->use_operation_id, use_id)) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE, record_index, source_value, use_operation);
        return false;
    }
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, source_value);
    const XrTargetMachineRepRecord *machine =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    if (!binding || !machine || binding->semantic_value != source_value ||
        record->target_register_rep != binding->register_rep ||
        record->target_memory_rep != binding->memory_rep || record->target_slot != binding->slot) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION, record_index, source_value,
                 use_operation);
        return false;
    }
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(ctx->target_plan, &layout_count);
    if (!layouts || record->layout >= layout_count ||
        layouts[record->layout].semantic_type != source_type) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_LAYOUT, record_index, source_value, use_operation);
        return false;
    }
    return true;
}

static bool verify_exact_obligation(VerifyAuthority *ctx, uint32_t source_value,
                                    uint32_t use_operation, uint32_t use_block,
                                    uint16_t use_operand, uint16_t use_kind, XrRep output_storage) {
    XrRep input_storage = XR_REP_TAGGED;
    XrRep native_storage = XR_REP_TAGGED;
    uint16_t machine_kind = XR_MACHINE_REP_COUNT;
    if (!oracle_definition_storage(ctx, source_value, &input_storage, &machine_kind)) {
        rep_trace_refusal(ctx, "the definition oracle, replayed by the verifier",
                          "no definition oracle names this value", source_value, use_operation,
                          use_operand, XR_REP_COUNT, output_storage);
        set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                 (uint32_t) ctx->work, source_value, use_operation);
        return false;
    }
    if (!verify_charge_work(ctx, 1))
        return false;
    if (input_storage == output_storage)
        return true;
    if (!oracle_machine_storage(ctx, source_value, &native_storage, &machine_kind)) {
        rep_trace_refusal(ctx, "the verifier's machine-storage lookup",
                          "definition and use disagree but the value has no native storage row to "
                          "adapt through",
                          source_value, use_operation, use_operand, input_storage, output_storage);
        set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                 (uint32_t) ctx->work, source_value, use_operation);
        return false;
    }
    if ((input_storage != XR_REP_TAGGED && input_storage != native_storage) ||
        (output_storage != XR_REP_TAGGED && output_storage != native_storage)) {
        rep_trace_refusal(ctx, "the verifier's adapter contract",
                          "a non-tagged side does not match the value's own native storage, so "
                          "the pair is not one box/unbox can bridge",
                          source_value, use_operation, use_operand, input_storage, output_storage);
        set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                 (uint32_t) ctx->work, source_value, use_operation);
        return false;
    }
    if (input_storage != XR_REP_TAGGED && output_storage != XR_REP_TAGGED)
        return true;
    uint32_t source_operation = ctx->operation_by_value[source_value];
    uint32_t source_parameter = ctx->parameter_by_value[source_value];
    const XrSemanticOperationRecord *operation =
        source_operation != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, source_operation)
            : NULL;
    const XrSemanticParameterRecord *parameter =
        source_parameter != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(ctx->semantic, source_parameter)
            : NULL;
    uint32_t source_function = parameter   ? parameter->function
                               : operation ? operation->function
                                           : XR_SEMANTIC_INDEX_NONE;
    uint32_t record_index = find_exact_record(ctx, source_function, source_value, use_operation,
                                              use_block, use_operand, use_kind);
    if (record_index == XR_SEMANTIC_INDEX_NONE) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE, (uint32_t) ctx->work,
                 source_value, use_operation);
        return false;
    }
    if (ctx->seen_record[record_index] != 0) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_DUPLICATE_USE, record_index, source_value,
                 use_operation);
        return false;
    }
    ctx->seen_record[record_index] = 1;
    const XrAotRepresentationAdapterRecord *record =
        &ctx->view->records[record_index].representation_adapter;
    uint16_t expected_adapter =
        input_storage == XR_REP_TAGGED ? XR_AOT_REP_ADAPTER_UNBOX : XR_AOT_REP_ADAPTER_BOX;
    uint16_t expected_input =
        input_storage == XR_REP_TAGGED ? XR_MACHINE_REP_DYN_VALUE : machine_kind;
    uint16_t expected_output =
        output_storage == XR_REP_TAGGED ? XR_MACHINE_REP_DYN_VALUE : machine_kind;
    uint16_t expected_recipe = oracle_representation_recipe(expected_adapter, machine_kind);
    if (!verify_exact_record_authority(ctx, record_index, source_value, use_operation, use_block,
                                       use_kind))
        return false;
    if (!xr_fingerprint_equal(record->policy_fingerprint, ctx->policy_fingerprint)) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_STALE_EVIDENCE, record_index, source_value,
                 use_operation);
        return false;
    }
    if (expected_recipe == XR_AOT_REP_RECIPE_NONE || record->adapter_kind != expected_adapter ||
        record->recipe != expected_recipe || record->input_rep_kind != expected_input ||
        record->output_rep_kind != expected_output) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION, record_index, source_value,
                 use_operation);
        return false;
    }
    return true;
}

static bool verify_exact_semantic_coverage(VerifyAuthority *ctx) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    if (!verify_charge_work(ctx, ctx->operation_count))
        return false;
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(ctx->semantic, i);
        if (!operation || operation->operand_begin > operand_count ||
            operation->operand_count > operand_count - operation->operand_begin) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE, i, 0, i);
            return false;
        }
        for (uint16_t a = 0; a < operation->operand_count; a++) {
            uint32_t source_value = operands[operation->operand_begin + a].value;
            XrRep output_storage = XR_REP_TAGGED;
            if (source_value >= ctx->value_count ||
                !oracle_use_storage(ctx, i, a, source_value, &output_storage)) {
                rep_trace_refusal(ctx, "the use-site oracle, replayed by the verifier",
                                  "the use site admits no storage for this operand", source_value,
                                  i, a, XR_REP_COUNT, XR_REP_COUNT);
                set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE, i,
                         source_value, i);
                return false;
            }
            if (!verify_exact_obligation(ctx, source_value, i, operation->block, a,
                                         XR_AOT_REP_USE_OPERATION, output_storage))
                return false;
        }
    }
    for (uint32_t i = 0; i < ctx->block_count; i++) {
        const XrSemanticBlockRecord *block = xr_semantic_plan_block(ctx->semantic, i);
        if (!block || block->control_value == XR_SEMANTIC_INDEX_NONE)
            continue;
        if (!verify_charge_work(ctx, 1))
            return false;
        if (block->kind != XI_BLOCK_RETURN)
            continue;
        uint32_t source_operation = block->control_value < ctx->value_count
                                        ? ctx->operation_by_value[block->control_value]
                                        : XR_SEMANTIC_INDEX_NONE;
        const XrSemanticOperationRecord *operation =
            source_operation != XR_SEMANTIC_INDEX_NONE
                ? xr_semantic_plan_operation(ctx->semantic, source_operation)
                : NULL;
        if (operation && (operation->opcode == XI_ERR_RETURN ||
                          (ctx->exact_source_namespace_value &&
                           ctx->exact_source_namespace_value[block->control_value])))
            continue;
        XrRep output_storage = XR_REP_TAGGED;
        if (!ctx->policy->force_return_tagged) {
            uint16_t machine_kind = 0;
            if (!oracle_return_storage(ctx, block->control_value, &output_storage, &machine_kind)) {
                rep_trace_refusal(ctx, "the return-storage oracle, replayed by the verifier",
                                  "oracle_return_storage names no carrier a return may hand this "
                                  "value back in",
                                  block->control_value, XR_SEMANTIC_INDEX_NONE, 0, XR_REP_COUNT,
                                  XR_REP_COUNT);
                set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE, i,
                         block->control_value, XR_SEMANTIC_INDEX_NONE);
                return false;
            }
        }
        if (!verify_exact_obligation(ctx, block->control_value, XR_SEMANTIC_INDEX_NONE, i, 0,
                                     XR_AOT_REP_USE_BLOCK_CONTROL, output_storage))
            return false;
    }
    if (!verify_charge_work(ctx, ctx->view->record_count))
        return false;
    for (uint32_t i = 0; i < ctx->view->record_count; i++) {
        if (ctx->view->records[i].transform_kind == XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER &&
            ctx->seen_record[i] == 0) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE, i,
                     ctx->view->records[i].representation_adapter.source_value,
                     ctx->view->records[i].representation_adapter.use_operation);
            return false;
        }
    }
    return true;
}
