/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_aot_representation_refinement.c - Immutable representation adapter materialization
 */

#include "xr_aot_representation_refinement.h"
#include "xr_aot_scalar_value.h"
#include "../../base/xsha256.h"
#include "../../base/xmalloc.h"
#include "../../ir/xi_opt.h"
#include "../../runtime/value/xtype.h"
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

static void set_diag(XrAotRefinementDiagnostic *diag, uint32_t issue,
                     uint32_t record, uint32_t value, uint32_t operation) {
    if (!diag)
        return;
    memset(diag, 0, sizeof(*diag));
    diag->issue = issue;
    diag->record_index = record;
    diag->pass_id = 27902;
    diag->semantic_value = value;
    diag->semantic_operation = operation;
}

static bool source_type_matches(const XrType *live,
                                const XrSemanticTypeRecord *semantic) {
    if (!live || !semantic || (uint32_t) live->kind != semantic->kind ||
        live->scalar_rep != semantic->scalar_rep)
        return false;
    bool nullable = (semantic->flags & XR_SEM_TYPE_NULLABLE) != 0;
    return live->is_nullable == nullable;
}

static bool storage_matches_machine(XrRep storage, uint16_t machine) {
    switch (machine) {
        case XR_MACHINE_REP_I1:
        case XR_MACHINE_REP_I8:
        case XR_MACHINE_REP_U8:
        case XR_MACHINE_REP_I16:
        case XR_MACHINE_REP_U16:
        case XR_MACHINE_REP_I32:
        case XR_MACHINE_REP_U32:
        case XR_MACHINE_REP_I64:
        case XR_MACHINE_REP_U64:
        case XR_MACHINE_REP_ISIZE:
        case XR_MACHINE_REP_USIZE:
        case XR_MACHINE_REP_RUNE:
            return storage == XR_REP_I64;
        case XR_MACHINE_REP_F32:
        case XR_MACHINE_REP_F64:
            return storage == XR_REP_F64;
        case XR_MACHINE_REP_OBJECT_REF: return storage == XR_REP_PTR;
        case XR_MACHINE_REP_RAW_PTR: return storage == XR_REP_RAWPTR;
        default: return false;
    }
}

static uint16_t machine_kind_for_storage(XrRep storage) {
    switch (storage) {
        case XR_REP_TAGGED: return XR_MACHINE_REP_DYN_VALUE;
        case XR_REP_I64: return XR_MACHINE_REP_I64;
        case XR_REP_F64: return XR_MACHINE_REP_F64;
        case XR_REP_PTR: return XR_MACHINE_REP_OBJECT_REF;
        case XR_REP_RAWPTR: return XR_MACHINE_REP_RAW_PTR;
        case XR_REP_VOID: return XR_MACHINE_REP_VOID;
        default: return XR_MACHINE_REP_COUNT;
    }
}

static uint16_t adapter_kind(XiRepAdapterKind kind) {
    switch (kind) {
        case XI_REP_ADAPTER_BOX: return XR_AOT_REP_ADAPTER_BOX;
        case XI_REP_ADAPTER_UNBOX: return XR_AOT_REP_ADAPTER_UNBOX;
        case XI_REP_ADAPTER_ENUM_DESCRIPTOR_BOX:
            return XR_AOT_REP_ADAPTER_ENUM_DESCRIPTOR_BOX;
        case XI_REP_ADAPTER_ENUM_DESCRIPTOR_UNBOX:
            return XR_AOT_REP_ADAPTER_ENUM_DESCRIPTOR_UNBOX;
        default: return 0;
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
    uint32_t type_count;
    uint32_t *layout_by_type;
    XrAotRefinementDiagnostic *diag;
} CollectContext;

static void collect_indices_dispose(CollectContext *ctx);

static bool collect_scan_edge(CollectContext *ctx) {
    if (ctx->scan_count >= XR_AOT_REFINEMENT_MAX_RECORDS) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_RESOURCE_BUDGET,
                 ctx->scan_count, 0, 0);
        return false;
    }
    ctx->scan_count++;
    return true;
}

static bool collect_indices_init(CollectContext *ctx) {
    uint32_t function_count =
        (uint32_t) xr_semantic_plan_function_count(ctx->semantic);
    uint64_t value_count = 0;
    for (uint32_t i = 0; i < function_count; i++) {
        const XrSemanticFunctionRecord *function =
            xr_semantic_plan_function(ctx->semantic, i);
        if (function && (uint64_t) function->value_begin + function->value_count >
                            value_count)
            value_count = (uint64_t) function->value_begin + function->value_count;
    }
    ctx->type_count = (uint32_t) xr_semantic_plan_type_count(ctx->semantic);
    if (value_count > XR_AOT_REFINEMENT_MAX_RECORDS ||
        ctx->type_count > XR_AOT_REFINEMENT_MAX_RECORDS)
        return false;
    ctx->semantic_value_count = (uint32_t) value_count;
    ctx->operation_by_value = (uint32_t *) xr_malloc(
        (ctx->semantic_value_count ? ctx->semantic_value_count : 1u) *
        sizeof(uint32_t));
    ctx->parameter_by_value = (uint32_t *) xr_malloc(
        (ctx->semantic_value_count ? ctx->semantic_value_count : 1u) *
        sizeof(uint32_t));
    ctx->layout_by_type = (uint32_t *) xr_malloc(
        (ctx->type_count ? ctx->type_count : 1u) * sizeof(uint32_t));
    if (!ctx->operation_by_value || !ctx->parameter_by_value ||
        !ctx->layout_by_type) {
        collect_indices_dispose(ctx);
        return false;
    }
    for (uint32_t i = 0; i < ctx->semantic_value_count; i++) {
        ctx->operation_by_value[i] = XR_SEMANTIC_INDEX_NONE;
        ctx->parameter_by_value[i] = XR_SEMANTIC_INDEX_NONE;
    }
    for (uint32_t i = 0; i < ctx->type_count; i++)
        ctx->layout_by_type[i] = XR_SEMANTIC_INDEX_NONE;
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(ctx->semantic);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(ctx->semantic, i);
        if (!operation || operation->result_value >= ctx->semantic_value_count ||
            ctx->operation_by_value[operation->result_value] !=
                XR_SEMANTIC_INDEX_NONE)
            goto invalid;
        ctx->operation_by_value[operation->result_value] = i;
    }
    uint32_t parameter_count =
        (uint32_t) xr_semantic_plan_parameter_count(ctx->semantic);
    for (uint32_t i = 0; i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(ctx->semantic, i);
        if (!parameter || parameter->value >= ctx->semantic_value_count ||
            ctx->parameter_by_value[parameter->value] != XR_SEMANTIC_INDEX_NONE)
            goto invalid;
        ctx->parameter_by_value[parameter->value] = i;
    }
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts =
        xr_target_plan_layouts(ctx->target_plan, &layout_count);
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type >= ctx->type_count ||
            ctx->layout_by_type[layouts[i].semantic_type] !=
                XR_SEMANTIC_INDEX_NONE)
            goto invalid;
        ctx->layout_by_type[layouts[i].semantic_type] = i;
    }
    return true;
invalid:
    collect_indices_dispose(ctx);
    return false;
}

static void collect_indices_dispose(CollectContext *ctx) {
    xr_free(ctx->operation_by_value);
    xr_free(ctx->parameter_by_value);
    xr_free(ctx->layout_by_type);
    ctx->operation_by_value = NULL;
    ctx->parameter_by_value = NULL;
    ctx->layout_by_type = NULL;
}

static bool add_obligation(CollectContext *ctx, const XiFunc *function,
                           const XiValue *source, uint32_t use_operation_index,
                           uint32_t use_block, uint16_t use_operand,
                           uint16_t use_kind, XiRepAdapterKind xi_kind,
                           uint16_t input_storage, uint16_t output_storage) {
    uint32_t source_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t source_value = XR_SEMANTIC_INDEX_NONE;
    char error[256] = {0};
    if (!xr_aot_scalar_semantic_value_id(ctx->target_plan, function, source,
                                         &source_function, &source_value,
                                         error, sizeof(error))) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY,
                 ctx->record_count, source_value, use_operation_index);
        return false;
    }
    if (source_value >= ctx->semantic_value_count) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY,
                 ctx->record_count, source_value, use_operation_index);
        return false;
    }
    uint32_t source_operation_index = XR_SEMANTIC_INDEX_NONE;
    uint32_t source_type_index = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *source_operation = NULL;
    const XrSemanticParameterRecord *source_parameter = NULL;
    if (ctx->parameter_by_value[source_value] != XR_SEMANTIC_INDEX_NONE) {
        source_parameter = xr_semantic_plan_parameter(
            ctx->semantic, ctx->parameter_by_value[source_value]);
        source_type_index = source_parameter ? source_parameter->type
                                             : XR_SEMANTIC_INDEX_NONE;
        source_operation_index = ctx->operation_by_value[source_value];
        source_operation = source_operation_index != XR_SEMANTIC_INDEX_NONE
                               ? xr_semantic_plan_operation(
                                     ctx->semantic, source_operation_index)
                               : NULL;
        if (!source_parameter || source_parameter->function != source_function ||
            source->op != XI_PARAM || !source_operation ||
            source_operation->opcode != XI_PARAM) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY,
                     ctx->record_count, source_value, source_operation_index);
            return false;
        }
    } else {
        source_operation_index = ctx->operation_by_value[source_value];
        source_operation = source_operation_index != XR_SEMANTIC_INDEX_NONE
                               ? xr_semantic_plan_operation(
                                     ctx->semantic, source_operation_index)
                               : NULL;
        source_type_index = source_operation ? source_operation->result_type
                                             : XR_SEMANTIC_INDEX_NONE;
    }
    if (!source_operation || source_operation->opcode != source->op ||
        source_operation->auxiliary_kind != source->aux_kind ||
        source_operation->semantic_immediate != source->aux_int ||
        source_operation->flags != source->flags) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY,
                 ctx->record_count, source_value, source_operation_index);
        return false;
    }
    const XrSemanticTypeRecord *semantic_type =
        xr_semantic_plan_type(ctx->semantic, source_type_index);
    if (!source_type_matches(source->type, semantic_type)) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_TYPE,
                 ctx->record_count, source_value, source_operation_index);
        return false;
    }
    const XrTargetValueRepRecord *value_rep =
        xr_target_plan_value_rep(ctx->target_plan, source_value);
    const XrTargetMachineRepRecord *machine =
        value_rep ? xr_target_plan_machine_rep(ctx->target_plan,
                                                value_rep->register_rep)
                  : NULL;
    bool boxes = input_storage != XR_REP_TAGGED &&
                 output_storage == XR_REP_TAGGED;
    XrRep native_storage = (XrRep) (boxes ? input_storage : output_storage);
    if (machine && !storage_matches_machine(native_storage, machine->kind)) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION,
                 ctx->record_count, source_value, use_operation_index);
        return false;
    }
    uint32_t layout = source_type_index < ctx->type_count
                          ? ctx->layout_by_type[source_type_index]
                          : XR_SEMANTIC_INDEX_NONE;
    bool has_layout = layout != XR_SEMANTIC_INDEX_NONE;
    bool can_apply = machine && has_layout &&
                     xi_kind != XI_REP_ADAPTER_ENUM_DESCRIPTOR_BOX &&
                     xi_kind != XI_REP_ADAPTER_ENUM_DESCRIPTOR_UNBOX;
    uint16_t input_kind = machine_kind_for_storage((XrRep) input_storage);
    uint16_t output_kind = machine_kind_for_storage((XrRep) output_storage);
    if (can_apply) {
        input_kind = boxes ? machine->kind : XR_MACHINE_REP_DYN_VALUE;
        output_kind = boxes ? XR_MACHINE_REP_DYN_VALUE : machine->kind;
    } else {
        layout = XR_SEMANTIC_INDEX_NONE;
    }
    XrAotRepresentationAdapterRequest request = {
        .source_value = source_value,
        .use_operation = use_operation_index,
        .use_block = use_block,
        .use_operand = use_operand,
        .use_kind = use_kind,
        .adapter_kind = adapter_kind(xi_kind),
        .input_rep_kind = input_kind,
        .output_rep_kind = output_kind,
        .layout = layout,
        .policy_fingerprint = rep_policy_fingerprint(ctx->policy),
    };
    uint32_t decision = XR_AOT_REFINEMENT_REFUSED;
    if (!request.adapter_kind || input_kind >= XR_MACHINE_REP_COUNT ||
        output_kind >= XR_MACHINE_REP_COUNT ||
        !xr_aot_refinement_try_representation_adapter(
            ctx->builder, &ctx->protocol, ctx->target_plan, &request,
            &decision, ctx->diag))
        return false;
    ctx->record_count++;
    return true;
}

static bool add_use(CollectContext *ctx, const XiFunc *function,
                    const XiValue *source, const XiValue *user,
                    uint16_t argument) {
    XiRepAdapterKind xi_kind = XI_REP_ADAPTER_NONE;
    uint16_t input_storage = XR_REP_TAGGED;
    uint16_t output_storage = XR_REP_TAGGED;
    if (!xi_opt_rep_adapter_for_use(source, user, argument, ctx->policy,
                                    &xi_kind, &input_storage, &output_storage))
        return true;
    uint32_t use_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t use_value = XR_SEMANTIC_INDEX_NONE;
    char error[256] = {0};
    if (!xr_aot_scalar_semantic_value_id(ctx->target_plan, function, user,
                                         &use_function, &use_value,
                                         error, sizeof(error)) ||
        use_value >= ctx->semantic_value_count ||
        ctx->operation_by_value[use_value] == XR_SEMANTIC_INDEX_NONE) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE, ctx->record_count,
                 use_value, XR_SEMANTIC_INDEX_NONE);
        return false;
    }
    uint32_t use_operation_index = ctx->operation_by_value[use_value];
    const XrSemanticOperationRecord *use_operation =
        xr_semantic_plan_operation(ctx->semantic, use_operation_index);
    if (!use_operation || use_operation->function != use_function ||
        use_operation->opcode != user->op ||
        use_operation->auxiliary_kind != user->aux_kind ||
        use_operation->semantic_immediate != user->aux_int ||
        use_operation->flags != user->flags) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE, ctx->record_count,
                 use_value, use_operation_index);
        return false;
    }
    return add_obligation(ctx, function, source, use_operation_index,
                          use_operation->block, argument,
                          XR_AOT_REP_USE_OPERATION, xi_kind, input_storage,
                          output_storage);
}

static bool collect_function(CollectContext *ctx, const XiFunc *function) {
    if (!function || function->semantic_plan != ctx->semantic) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_BASELINE_FINGERPRINT,
                 ctx->record_count, 0, 0);
        return false;
    }
    for (uint32_t b = 0; b < function->nblocks; b++) {
        const XiBlock *block = function->blocks[b];
        if (!block) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE,
                     ctx->record_count, 0, 0);
            return false;
        }
        for (uint32_t v = 0; v < block->nvalues; v++) {
            const XiValue *user = block->values[v];
            if (!user)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] &&
                    (!collect_scan_edge(ctx) ||
                     !add_use(ctx, function, user->args[a], user, a)))
                    return false;
            }
        }
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                XiRepAdapterKind kind = XI_REP_ADAPTER_NONE;
                uint16_t input = XR_REP_TAGGED;
                uint16_t output = XR_REP_TAGGED;
                const XiValue *source = phi->value.args[a];
                if (!source)
                    continue;
                if (!collect_scan_edge(ctx))
                    return false;
                if (xi_opt_rep_adapter_for_phi(
                        source, phi, a, ctx->policy, &kind, &input, &output)) {
                    uint32_t user_function = XR_SEMANTIC_INDEX_NONE;
                    uint32_t user_value = XR_SEMANTIC_INDEX_NONE;
                    char error[256] = {0};
                    if (!xr_aot_scalar_semantic_value_id(
                            ctx->target_plan, function, &phi->value,
                            &user_function, &user_value, error,
                            sizeof(error)) ||
                        user_value >= ctx->semantic_value_count ||
                        ctx->operation_by_value[user_value] ==
                            XR_SEMANTIC_INDEX_NONE) {
                        set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE,
                                 ctx->record_count, user_value,
                                 XR_SEMANTIC_INDEX_NONE);
                        return false;
                    }
                    uint32_t use_operation =
                        ctx->operation_by_value[user_value];
                    const XrSemanticOperationRecord *use =
                        xr_semantic_plan_operation(ctx->semantic,
                                                   use_operation);
                    if (!use || use->function != user_function ||
                        use->opcode != XI_PHI ||
                        use->auxiliary_kind != phi->value.aux_kind ||
                        use->semantic_immediate != phi->value.aux_int ||
                        use->flags != phi->value.flags) {
                        set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE,
                                 ctx->record_count, user_value,
                                 use_operation);
                        return false;
                    }
                    if (!add_obligation(ctx, function, source, use_operation,
                                        use->block, a,
                                        XR_AOT_REP_USE_OPERATION, kind, input,
                                        output))
                        return false;
                }
            }
        }
        XiRepAdapterKind return_kind = XI_REP_ADAPTER_NONE;
        uint16_t return_input = XR_REP_TAGGED;
        uint16_t return_output = XR_REP_TAGGED;
        if (block->control && !collect_scan_edge(ctx))
            return false;
        if (xi_opt_rep_adapter_for_return(function, block, ctx->policy,
                                           &return_kind, &return_input,
                                           &return_output)) {
            const XrSemanticFunctionRecord *semantic_function =
                xr_semantic_plan_function(
                    ctx->semantic, function->semantic_plan_function_index);
            uint32_t use_block = semantic_function && block->id < semantic_function->block_count
                                     ? semantic_function->block_begin + block->id
                                     : XR_SEMANTIC_INDEX_NONE;
            if (use_block == XR_SEMANTIC_INDEX_NONE ||
                !add_obligation(ctx, function, block->control,
                                XR_SEMANTIC_INDEX_NONE, use_block, 0,
                                XR_AOT_REP_USE_BLOCK_CONTROL, return_kind,
                                return_input, return_output))
                return false;
        }
    }
    for (uint16_t i = 0; i < function->nchildren; i++) {
        if (function->children[i] &&
            !collect_function(ctx, function->children[i]))
            return false;
    }
    return true;
}

bool xr_aot_representation_refinement_build(
    const XiFunc *root, const XrTargetPlan *target_plan,
    const XiRepPolicy *policy, XrAotRefinementPlan **out_plan,
    XrAotRefinementDiagnostic *diag) {
    if (out_plan)
        *out_plan = NULL;
    if (diag)
        memset(diag, 0, sizeof(*diag));
    if (!root || !target_plan || !out_plan) {
        set_diag(diag, XR_AOT_REFINEMENT_INVALID_ARGUMENT, 0, 0, 0);
        return false;
    }
    const XrSemanticPlan *semantic =
        xr_target_plan_semantic_plan(target_plan);
    if (!semantic || root->semantic_plan != semantic) {
        set_diag(diag, XR_AOT_REFINEMENT_BASELINE_FINGERPRINT, 0, 0, 0);
        return false;
    }
    XrAotRefinementBuilder *builder =
        xr_aot_refinement_builder_create(target_plan, diag);
    if (!builder)
        return false;
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
    bool ok = collect_function(&ctx, root) &&
              xr_aot_refinement_builder_freeze(builder, target_plan,
                                                out_plan, diag);
    collect_indices_dispose(&ctx);
    xr_aot_refinement_builder_free(builder);
    return ok;
}

typedef struct VerifyContext {
    CollectContext index;
    const XrAotRefinementPlanView *view;
    XrFingerprint policy_fingerprint;
    uint8_t *seen;
    uint32_t visits;
} VerifyContext;

static int compare_live_key(const XrAotTransformationRecord *record,
                            uint32_t source_function, uint32_t source_value,
                            uint32_t use_operation, uint32_t use_block,
                            uint16_t use_operand, uint16_t use_kind) {
    if (record->transform_kind != XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER)
        return record->transform_kind < XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER
                   ? -1
                   : 1;
    const XrAotRepresentationAdapterRecord *adapter =
        &record->representation_adapter;
#define XR_COMPARE_LIVE(field, expected)                                                          \
    do {                                                                                           \
        if (adapter->field != (expected))                                                         \
            return adapter->field < (expected) ? -1 : 1;                                          \
    } while (0)
    XR_COMPARE_LIVE(source_function, source_function);
    XR_COMPARE_LIVE(use_kind, use_kind);
    XR_COMPARE_LIVE(use_block, use_block);
    XR_COMPARE_LIVE(use_operation, use_operation);
    XR_COMPARE_LIVE(use_operand, use_operand);
    XR_COMPARE_LIVE(source_value, source_value);
#undef XR_COMPARE_LIVE
    return 0;
}

static uint32_t find_live_record(const VerifyContext *ctx,
                                 uint32_t source_function,
                                 uint32_t source_value,
                                 uint32_t use_operation, uint32_t use_block,
                                 uint16_t use_operand, uint16_t use_kind) {
    uint32_t low = 0;
    uint32_t high = ctx->view->record_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        int order = compare_live_key(&ctx->view->records[middle],
                                     source_function, source_value,
                                     use_operation, use_block, use_operand,
                                     use_kind);
        if (order < 0)
            low = middle + 1u;
        else
            high = middle;
    }
    if (low >= ctx->view->record_count ||
        compare_live_key(&ctx->view->records[low], source_function,
                         source_value, use_operation, use_block, use_operand,
                         use_kind) != 0)
        return XR_SEMANTIC_INDEX_NONE;
    return low;
}

static bool verify_live_source(VerifyContext *ctx, const XiFunc *function,
                               const XiValue *source, uint32_t source_value,
                               const XrAotRepresentationAdapterRecord *record,
                               uint32_t record_index) {
    if (source_value >= ctx->index.semantic_value_count) {
        set_diag(ctx->index.diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY,
                 record_index, source_value, record->source_operation);
        return false;
    }
    uint32_t operation_index = ctx->index.operation_by_value[source_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->index.semantic, operation_index)
            : NULL;
    const XrSemanticParameterRecord *parameter = NULL;
    if (ctx->index.parameter_by_value[source_value] != XR_SEMANTIC_INDEX_NONE)
        parameter = xr_semantic_plan_parameter(
            ctx->index.semantic, ctx->index.parameter_by_value[source_value]);
    uint32_t type = parameter ? parameter->type
                              : operation ? operation->result_type
                                          : XR_SEMANTIC_INDEX_NONE;
    uint16_t source_kind = parameter ? XR_AOT_REP_SOURCE_PARAMETER
                                     : XR_AOT_REP_SOURCE_OPERATION;
    if (!operation || operation->function != record->source_function ||
        operation->opcode != source->op ||
        operation->auxiliary_kind != source->aux_kind ||
        operation->semantic_immediate != source->aux_int ||
        operation->flags != source->flags ||
        operation_index != record->source_operation ||
        source_kind != record->source_kind) {
        set_diag(ctx->index.diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY,
                 record_index, source_value, operation_index);
        return false;
    }
    const XrSemanticTypeRecord *semantic_type =
        xr_semantic_plan_type(ctx->index.semantic, type);
    if (type != record->source_type ||
        !source_type_matches(source->type, semantic_type)) {
        set_diag(ctx->index.diag, XR_AOT_REFINEMENT_SOURCE_TYPE,
                 record_index, source_value, operation_index);
        return false;
    }
    if (parameter &&
        (source->op != XI_PARAM || source->aux_int != parameter->ordinal ||
         parameter->function != record->source_function)) {
        set_diag(ctx->index.diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY,
                 record_index, source_value, operation_index);
        return false;
    }
    (void) function;
    return true;
}

static bool verify_obligation(VerifyContext *ctx, const XiFunc *function,
                              const XiValue *source, const XiValue *user,
                              uint32_t use_operation, uint32_t use_block,
                              uint16_t use_operand, uint16_t use_kind,
                              XiRepAdapterKind xi_kind, uint16_t input_storage,
                              uint16_t output_storage) {
    uint32_t source_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t source_value = XR_SEMANTIC_INDEX_NONE;
    char error[256] = {0};
    if (!xr_aot_scalar_semantic_value_id(ctx->index.target_plan, function,
                                         source, &source_function,
                                         &source_value, error,
                                         sizeof(error))) {
        set_diag(ctx->index.diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY,
                 ctx->visits, source_value, use_operation);
        return false;
    }
    uint32_t record_index = find_live_record(
        ctx, source_function, source_value, use_operation, use_block,
        use_operand, use_kind);
    if (record_index == XR_SEMANTIC_INDEX_NONE) {
        set_diag(ctx->index.diag, XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE,
                 ctx->visits, source_value, use_operation);
        return false;
    }
    if (ctx->seen[record_index] != 0) {
        set_diag(ctx->index.diag, XR_AOT_REFINEMENT_DUPLICATE_USE,
                 record_index, source_value, use_operation);
        return false;
    }
    ctx->seen[record_index] = 1;
    const XrAotTransformationRecord *transformation =
        &ctx->view->records[record_index];
    const XrAotRepresentationAdapterRecord *record =
        &transformation->representation_adapter;
    if (!xr_fingerprint_equal(record->policy_fingerprint,
                              ctx->policy_fingerprint)) {
        set_diag(ctx->index.diag, XR_AOT_REFINEMENT_STALE_EVIDENCE,
                 record_index, source_value, use_operation);
        return false;
    }
    if (!verify_live_source(ctx, function, source, source_value, record,
                            record_index))
        return false;
    if (use_kind == XR_AOT_REP_USE_OPERATION) {
        uint32_t user_function = XR_SEMANTIC_INDEX_NONE;
        uint32_t user_value = XR_SEMANTIC_INDEX_NONE;
        if (!user || !xr_aot_scalar_semantic_value_id(
                         ctx->index.target_plan, function, user,
                         &user_function, &user_value, error, sizeof(error)) ||
            user_function != source_function ||
            user_value >= ctx->index.semantic_value_count ||
            ctx->index.operation_by_value[user_value] != use_operation) {
            set_diag(ctx->index.diag, XR_AOT_REFINEMENT_USE_SITE,
                     record_index, source_value, use_operation);
            return false;
        }
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(ctx->index.semantic, use_operation);
        if (!operation || operation->opcode != user->op ||
            operation->auxiliary_kind != user->aux_kind ||
            operation->semantic_immediate != user->aux_int ||
            operation->flags != user->flags ||
            use_operand >= user->nargs || user->args[use_operand] != source) {
            set_diag(ctx->index.diag, XR_AOT_REFINEMENT_USE_SITE,
                     record_index, source_value, use_operation);
            return false;
        }
    } else {
        const XrSemanticBlockRecord *block =
            xr_semantic_plan_block(ctx->index.semantic, use_block);
        if (!block || block->control_value != source_value ||
            block->kind != XI_BLOCK_RETURN) {
            set_diag(ctx->index.diag, XR_AOT_REFINEMENT_USE_SITE,
                     record_index, source_value, use_operation);
            return false;
        }
    }
    uint16_t expected_adapter = adapter_kind(xi_kind);
    bool boxes = xi_kind == XI_REP_ADAPTER_BOX ||
                 xi_kind == XI_REP_ADAPTER_ENUM_DESCRIPTOR_BOX;
    uint16_t expected_input = machine_kind_for_storage((XrRep) input_storage);
    uint16_t expected_output = machine_kind_for_storage((XrRep) output_storage);
    if (transformation->decision == XR_AOT_REFINEMENT_APPLIED) {
        const XrTargetMachineRepRecord *machine =
            xr_target_plan_machine_rep(ctx->index.target_plan,
                                       record->target_register_rep);
        if (!machine) {
            set_diag(ctx->index.diag, XR_AOT_REFINEMENT_REPRESENTATION,
                     record_index, source_value, use_operation);
            return false;
        }
        expected_input = boxes ? machine->kind : XR_MACHINE_REP_DYN_VALUE;
        expected_output = boxes ? XR_MACHINE_REP_DYN_VALUE : machine->kind;
    }
    if (record->adapter_kind != expected_adapter ||
        record->input_rep_kind != expected_input ||
        record->output_rep_kind != expected_output) {
        set_diag(ctx->index.diag, XR_AOT_REFINEMENT_REPRESENTATION,
                 record_index, source_value, use_operation);
        return false;
    }
    return true;
}

static bool verify_scan_edge(VerifyContext *ctx) {
    if (ctx->visits >= XR_AOT_REFINEMENT_MAX_RECORDS) {
        set_diag(ctx->index.diag, XR_AOT_REFINEMENT_RESOURCE_BUDGET,
                 ctx->visits, 0, 0);
        return false;
    }
    ctx->visits++;
    return true;
}

static bool verify_function(VerifyContext *ctx, const XiFunc *function) {
    if (!function || function->semantic_plan != ctx->index.semantic) {
        set_diag(ctx->index.diag, XR_AOT_REFINEMENT_BASELINE_FINGERPRINT,
                 ctx->visits, 0, 0);
        return false;
    }
    for (uint32_t b = 0; b < function->nblocks; b++) {
        const XiBlock *block = function->blocks[b];
        if (!block) {
            set_diag(ctx->index.diag, XR_AOT_REFINEMENT_USE_SITE,
                     ctx->visits, 0, 0);
            return false;
        }
        for (uint32_t v = 0; v < block->nvalues; v++) {
            const XiValue *user = block->values[v];
            if (!user)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                XiRepAdapterKind kind = XI_REP_ADAPTER_NONE;
                uint16_t input = XR_REP_TAGGED;
                uint16_t output = XR_REP_TAGGED;
                const XiValue *source = user->args[a];
                if (!source)
                    continue;
                if (!verify_scan_edge(ctx))
                    return false;
                if (!xi_opt_rep_adapter_for_use(
                                   source, user, a, ctx->index.policy, &kind,
                                   &input, &output))
                    continue;
                uint32_t user_function = XR_SEMANTIC_INDEX_NONE;
                uint32_t user_value = XR_SEMANTIC_INDEX_NONE;
                char error[256] = {0};
                if (!xr_aot_scalar_semantic_value_id(
                        ctx->index.target_plan, function, user,
                        &user_function, &user_value, error, sizeof(error)) ||
                    user_value >= ctx->index.semantic_value_count ||
                    ctx->index.operation_by_value[user_value] ==
                        XR_SEMANTIC_INDEX_NONE) {
                    set_diag(ctx->index.diag, XR_AOT_REFINEMENT_USE_SITE,
                             ctx->visits, user_value,
                             XR_SEMANTIC_INDEX_NONE);
                    return false;
                }
                uint32_t operation = ctx->index.operation_by_value[user_value];
                const XrSemanticOperationRecord *use =
                    xr_semantic_plan_operation(ctx->index.semantic, operation);
                if (!use) {
                    set_diag(ctx->index.diag, XR_AOT_REFINEMENT_USE_SITE,
                             ctx->visits, user_value, operation);
                    return false;
                }
                if (!verify_obligation(ctx, function, source, user, operation,
                                       use->block, a,
                                       XR_AOT_REP_USE_OPERATION, kind, input,
                                       output))
                    return false;
            }
        }
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                XiRepAdapterKind kind = XI_REP_ADAPTER_NONE;
                uint16_t input = XR_REP_TAGGED;
                uint16_t output = XR_REP_TAGGED;
                const XiValue *source = phi->value.args[a];
                if (!source)
                    continue;
                if (!verify_scan_edge(ctx))
                    return false;
                if (!xi_opt_rep_adapter_for_phi(
                                   source, phi, a, ctx->index.policy, &kind,
                                   &input, &output))
                    continue;
                uint32_t user_function = XR_SEMANTIC_INDEX_NONE;
                uint32_t user_value = XR_SEMANTIC_INDEX_NONE;
                char error[256] = {0};
                if (!xr_aot_scalar_semantic_value_id(
                        ctx->index.target_plan, function, &phi->value,
                        &user_function, &user_value, error, sizeof(error)) ||
                    user_value >= ctx->index.semantic_value_count ||
                    ctx->index.operation_by_value[user_value] ==
                        XR_SEMANTIC_INDEX_NONE) {
                    set_diag(ctx->index.diag, XR_AOT_REFINEMENT_USE_SITE,
                             ctx->visits, user_value,
                             XR_SEMANTIC_INDEX_NONE);
                    return false;
                }
                uint32_t operation = ctx->index.operation_by_value[user_value];
                const XrSemanticOperationRecord *use =
                    xr_semantic_plan_operation(ctx->index.semantic, operation);
                if (!use || use->opcode != XI_PHI ||
                    use->auxiliary_kind != phi->value.aux_kind ||
                    use->semantic_immediate != phi->value.aux_int ||
                    use->flags != phi->value.flags) {
                    set_diag(ctx->index.diag, XR_AOT_REFINEMENT_USE_SITE,
                             ctx->visits, user_value, operation);
                    return false;
                }
                if (!verify_obligation(
                        ctx, function, source, &phi->value, operation,
                        use->block, a, XR_AOT_REP_USE_OPERATION, kind, input,
                        output))
                    return false;
            }
        }
        XiRepAdapterKind kind = XI_REP_ADAPTER_NONE;
        uint16_t input = XR_REP_TAGGED;
        uint16_t output = XR_REP_TAGGED;
        if (block->control && !verify_scan_edge(ctx))
            return false;
        if (xi_opt_rep_adapter_for_return(function, block, ctx->index.policy,
                                           &kind, &input, &output)) {
            const XrSemanticFunctionRecord *semantic_function =
                xr_semantic_plan_function(
                    ctx->index.semantic,
                    function->semantic_plan_function_index);
            uint32_t use_block = semantic_function &&
                                         block->id < semantic_function->block_count
                                     ? semantic_function->block_begin + block->id
                                     : XR_SEMANTIC_INDEX_NONE;
            if (use_block == XR_SEMANTIC_INDEX_NONE ||
                !verify_obligation(ctx, function, block->control, NULL,
                                   XR_SEMANTIC_INDEX_NONE, use_block, 0,
                                   XR_AOT_REP_USE_BLOCK_CONTROL, kind, input,
                                   output))
                return false;
        }
    }
    for (uint16_t i = 0; i < function->nchildren; i++) {
        if (function->children[i] &&
            !verify_function(ctx, function->children[i]))
            return false;
    }
    return true;
}

bool xr_aot_representation_refinement_verify(
    const XrAotRefinementPlanView *view, const XiFunc *root,
    const XrTargetPlan *target_plan, const XiRepPolicy *policy,
    XrAotRefinementDiagnostic *diag) {
    if (!root || !target_plan || root->semantic_plan !=
                                     xr_target_plan_semantic_plan(target_plan)) {
        set_diag(diag, XR_AOT_REFINEMENT_BASELINE_FINGERPRINT, 0, 0, 0);
        return false;
    }
    if (!xr_aot_refinement_verify(view, target_plan, diag))
        return false;
    XiRepPolicy default_policy = xi_rep_policy_native_boundary();
    VerifyContext ctx = {
        .index = {
            .target_plan = target_plan,
            .semantic = xr_target_plan_semantic_plan(target_plan),
            .policy = policy ? policy : &default_policy,
            .diag = diag,
        },
        .view = view,
        .policy_fingerprint =
            rep_policy_fingerprint(policy ? policy : &default_policy),
    };
    if (!collect_indices_init(&ctx.index)) {
        set_diag(diag, XR_AOT_REFINEMENT_RESOURCE_BUDGET, 0, 0, 0);
        return false;
    }
    ctx.seen = (uint8_t *) xr_calloc(view->record_count ? view->record_count : 1u,
                                     sizeof(uint8_t));
    if (!ctx.seen) {
        collect_indices_dispose(&ctx.index);
        set_diag(diag, XR_AOT_REFINEMENT_OUT_OF_MEMORY, 0, 0, 0);
        return false;
    }
    bool valid = verify_function(&ctx, root);
    if (valid) {
        for (uint32_t i = 0; i < view->record_count; i++) {
            if (view->records[i].transform_kind ==
                    XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER &&
                ctx.seen[i] == 0) {
                set_diag(diag, XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE, i,
                         view->records[i].representation_adapter.source_value,
                         view->records[i].representation_adapter.use_operation);
                valid = false;
                break;
            }
        }
    }
    xr_free(ctx.seen);
    collect_indices_dispose(&ctx.index);
    if (!valid)
        return false;
    if (diag)
        memset(diag, 0, sizeof(*diag));
    return true;
}

bool xr_aot_representation_backend_run(
    const XrAotRefinementPlanView *view, const XiFunc *root,
    const XrTargetPlan *target_plan, const XiRepPolicy *policy,
    const XrAotBackendInterface *backend, void *context,
    XrAotBackendStats *out_stats, XrAotRefinementDiagnostic *diag) {
    if (out_stats)
        memset(out_stats, 0, sizeof(*out_stats));
    if (!backend || !context || !out_stats ||
        backend->abi_version != XR_AOT_REFINEMENT_BACKEND_ABI_VERSION ||
        !backend->begin || !backend->visit || !backend->finish ||
        !backend->abort) {
        set_diag(diag, XR_AOT_REFINEMENT_BACKEND_ABI, 0, 0, 0);
        return false;
    }
    if (!xr_aot_representation_refinement_verify(
            view, root, target_plan, policy, diag))
        return false;
    for (uint32_t i = 0; i < view->record_count; i++) {
        if ((backend->supported_transforms &
             XR_AOT_TRANSFORM_BIT(view->records[i].transform_kind)) != 0)
            continue;
        set_diag(diag, XR_AOT_REFINEMENT_BACKEND_INCOMPLETE_COVERAGE, i,
                 view->records[i].representation_adapter.source_value,
                 view->records[i].representation_adapter.use_operation);
        if (diag)
            diag->pass_id = view->records[i].protocol.pass_id;
        return false;
    }
    if (!backend->begin(context, &view->baseline, view->record_count)) {
        set_diag(diag, XR_AOT_REFINEMENT_BACKEND_FAILURE, 0, 0, 0);
        return false;
    }
    for (uint32_t i = 0; i < view->record_count; i++) {
        const XrAotTransformationRecord *record = &view->records[i];
        if (!backend->visit(context, i, record)) {
            backend->abort(context);
            set_diag(diag, XR_AOT_REFINEMENT_BACKEND_FAILURE, i,
                     record->representation_adapter.source_value,
                     record->representation_adapter.use_operation);
            if (diag)
                diag->pass_id = record->protocol.pass_id;
            return false;
        }
        out_stats->visited++;
        if (record->decision == XR_AOT_REFINEMENT_APPLIED)
            out_stats->applied++;
        else
            out_stats->refused++;
    }
    if (!backend->finish(context)) {
        set_diag(diag, XR_AOT_REFINEMENT_BACKEND_FAILURE,
                 view->record_count, 0, 0);
        return false;
    }
    if (diag)
        memset(diag, 0, sizeof(*diag));
    return true;
}
