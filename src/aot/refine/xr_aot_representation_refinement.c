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
#include "../../ir/xi_opt.h"
#include "../../runtime/value/xtype.h"
#include <string.h>

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

static const XrSemanticOperationRecord *operation_for_value(
    const XrSemanticPlan *semantic, uint32_t value, uint32_t *out_index) {
    const XrSemanticOperationRecord *found = NULL;
    uint32_t found_index = XR_SEMANTIC_INDEX_NONE;
    uint32_t count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    for (uint32_t i = 0; i < count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->result_value != value)
            continue;
        if (found)
            return NULL;
        found = operation;
        found_index = i;
    }
    if (out_index)
        *out_index = found_index;
    return found;
}

static bool layout_for_type(const XrTargetPlan *target_plan, uint32_t type,
                            uint32_t *out_layout) {
    uint32_t count = 0;
    const XrTargetLayoutRecord *layouts =
        xr_target_plan_layouts(target_plan, &count);
    uint32_t found = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < count; i++) {
        if (layouts[i].semantic_type != type)
            continue;
        if (found != XR_SEMANTIC_INDEX_NONE)
            return false;
        found = i;
    }
    if (found == XR_SEMANTIC_INDEX_NONE)
        return false;
    *out_layout = found;
    return true;
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
    XrAotRefinementDiagnostic *diag;
} CollectContext;

static bool add_use(CollectContext *ctx, const XiFunc *function,
                    const XiValue *source, const XiValue *user,
                    uint16_t argument) {
    XiRepAdapterKind xi_kind = XI_REP_ADAPTER_NONE;
    uint16_t input_storage = XR_REP_TAGGED;
    uint16_t output_storage = XR_REP_TAGGED;
    if (!xi_opt_rep_adapter_for_use(source, user, argument, ctx->policy,
                                    &xi_kind, &input_storage, &output_storage))
        return true;

    uint32_t source_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t source_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t user_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t user_value = XR_SEMANTIC_INDEX_NONE;
    char error[256] = {0};
    if (!xr_aot_scalar_semantic_value_id(ctx->target_plan, function, source,
                                         &source_function, &source_value,
                                         error, sizeof(error)) ||
        !xr_aot_scalar_semantic_value_id(ctx->target_plan, function, user,
                                         &user_function, &user_value,
                                         error, sizeof(error)) ||
        source_function != user_function) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY,
                 ctx->record_count, source_value, user_value);
        return false;
    }
    uint32_t source_operation_index = XR_SEMANTIC_INDEX_NONE;
    uint32_t use_operation_index = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *source_operation =
        operation_for_value(ctx->semantic, source_value,
                            &source_operation_index);
    const XrSemanticOperationRecord *use_operation =
        operation_for_value(ctx->semantic, user_value, &use_operation_index);
    if (!source_operation || source_operation->opcode != source->op) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY,
                 ctx->record_count, source_value, source_operation_index);
        return false;
    }
    if (!use_operation || use_operation->opcode != user->op) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE, ctx->record_count,
                 source_value, use_operation_index);
        return false;
    }
    const XrSemanticTypeRecord *semantic_type =
        xr_semantic_plan_type(ctx->semantic, source_operation->result_type);
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
    if (!machine || !storage_matches_machine(native_storage, machine->kind)) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION,
                 ctx->record_count, source_value, use_operation_index);
        return false;
    }
    uint32_t layout = XR_SEMANTIC_INDEX_NONE;
    if (!layout_for_type(ctx->target_plan, source_operation->result_type,
                         &layout)) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_LAYOUT, ctx->record_count,
                 source_value, use_operation_index);
        return false;
    }
    XrAotRepresentationAdapterRequest request = {
        .source_value = source_value,
        .use_operation = use_operation_index,
        .use_operand = argument,
        .adapter_kind = adapter_kind(xi_kind),
        .input_rep_kind = boxes ? machine->kind : XR_MACHINE_REP_DYN_VALUE,
        .output_rep_kind = boxes ? XR_MACHINE_REP_DYN_VALUE : machine->kind,
        .layout = layout,
    };
    if (!request.adapter_kind || !xr_aot_refinement_add_representation_adapter(
            ctx->builder, &ctx->protocol, ctx->target_plan, &request,
            ctx->diag))
        return false;
    ctx->record_count++;
    return true;
}

static bool collect_function(CollectContext *ctx, const XiFunc *function) {
    if (!function || function->semantic_plan != ctx->semantic)
        return false;
    for (uint32_t b = 0; b < function->nblocks; b++) {
        const XiBlock *block = function->blocks[b];
        if (!block)
            continue;
        for (uint32_t v = 0; v < block->nvalues; v++) {
            const XiValue *user = block->values[v];
            if (!user)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] &&
                    !add_use(ctx, function, user->args[a], user, a))
                    return false;
            }
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
    bool ok = collect_function(&ctx, root) &&
              xr_aot_refinement_builder_freeze(builder, target_plan,
                                                out_plan, diag);
    xr_aot_refinement_builder_free(builder);
    return ok;
}

static bool records_equal(const XrAotTransformationRecord *left,
                          const XrAotTransformationRecord *right) {
    const XrAotRepresentationAdapterRecord *a = &left->representation_adapter;
    const XrAotRepresentationAdapterRecord *b = &right->representation_adapter;
    return left->decision == right->decision &&
           left->transform_kind == right->transform_kind &&
           left->diagnostic_issue == right->diagnostic_issue &&
           a->source_function == b->source_function &&
           a->source_value == b->source_value &&
           a->source_operation == b->source_operation &&
           a->source_type == b->source_type &&
           a->use_operation == b->use_operation &&
           a->use_operand == b->use_operand &&
           a->adapter_kind == b->adapter_kind &&
           a->input_rep_kind == b->input_rep_kind &&
           a->output_rep_kind == b->output_rep_kind && a->layout == b->layout &&
           xr_stable_id_equal(a->source_operation_id, b->source_operation_id) &&
           xr_stable_id_equal(a->source_type_id, b->source_type_id) &&
           xr_stable_id_equal(a->use_operation_id, b->use_operation_id) &&
           xr_fingerprint_equal(a->layout_fingerprint, b->layout_fingerprint) &&
           xr_fingerprint_equal(a->fingerprint, b->fingerprint) &&
           memcmp(&left->input_state, &right->input_state,
                  sizeof(left->input_state)) == 0 &&
           memcmp(&left->output_state, &right->output_state,
                  sizeof(left->output_state)) == 0;
}

bool xr_aot_representation_refinement_verify(
    const XrAotRefinementPlanView *view, const XiFunc *root,
    const XrTargetPlan *target_plan, const XiRepPolicy *policy,
    XrAotRefinementDiagnostic *diag) {
    if (!xr_aot_refinement_verify(view, target_plan, diag))
        return false;
    XrAotRefinementPlan *expected_plan = NULL;
    if (!xr_aot_representation_refinement_build(
            root, target_plan, policy, &expected_plan, diag))
        return false;
    XrAotRefinementPlanView expected =
        xr_aot_refinement_plan_view(expected_plan);
    bool valid = view->record_count == expected.record_count;
    uint32_t mismatch = 0;
    for (; valid && mismatch < view->record_count; mismatch++)
        valid = records_equal(&view->records[mismatch],
                              &expected.records[mismatch]);
    xr_aot_refinement_plan_free(expected_plan);
    if (!valid) {
        set_diag(diag, XR_AOT_REFINEMENT_STALE_EVIDENCE, mismatch, 0, 0);
        return false;
    }
    if (diag)
        memset(diag, 0, sizeof(*diag));
    return true;
}
