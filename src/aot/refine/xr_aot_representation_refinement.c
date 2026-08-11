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
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"
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

XrFingerprint xr_aot_representation_policy_fingerprint(
    const XiRepPolicy *policy) {
    XiRepPolicy default_policy = xi_rep_policy_native_boundary();
    return rep_policy_fingerprint(policy ? policy : &default_policy);
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
    uint8_t flags =
        (uint8_t) ((live->is_nullable ? XR_SEM_TYPE_NULLABLE : 0u) |
                   (live->is_const ? XR_SEM_TYPE_CONST : 0u) |
                   (live->is_value_type ? XR_SEM_TYPE_VALUE : 0u) |
                   (live->is_literal ? XR_SEM_TYPE_LITERAL : 0u) |
                   (xi_own_type_is_rc(live)
                        ? XR_SEM_TYPE_REFERENCE_CAPABLE
                        : 0u) |
                   (live->kind == XR_KIND_SLICE
                        ? XR_SEM_TYPE_BORROW_VIEW
                        : 0u) |
                   (xi_own_type_is_rc(live) && live->kind != XR_KIND_SLICE
                        ? XR_SEM_TYPE_OWNERSHIP_ROOT
                        : 0u));
    return (semantic->flags & ~XR_SEM_TYPE_AGGREGATE_EXACT) == flags;
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
        if (!source_parameter || source_parameter->value != source_value ||
            source_parameter->function != source_function ||
            source->op != XI_PARAM ||
            source->aux_int != source_parameter->ordinal ||
            source->param_mode != source_parameter->mode ||
            source->transfer_mode != source_parameter->transfer_mode) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY,
                     ctx->record_count, source_value, source_operation_index);
            return false;
        }
        if (source_operation_index != XR_SEMANTIC_INDEX_NONE &&
            (!source_operation || source_operation->opcode != XI_PARAM ||
             source_operation->function != source_function ||
             source_operation->result_value != source_value ||
             source_operation->result_type != source_type_index ||
             source_operation->auxiliary_kind != source->aux_kind ||
             source_operation->semantic_immediate != source->aux_int ||
             source_operation->flags != source->flags ||
             source_operation->parameter_mode != source_parameter->mode ||
             source_operation->parameter_ownership !=
                 source_parameter->ownership ||
             source_operation->transfer_mode !=
                 source_parameter->transfer_mode)) {
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
        if (!source_operation || source_operation->function != source_function ||
            source_operation->result_value != source_value ||
            source_operation->opcode != source->op ||
            source_operation->auxiliary_kind != source->aux_kind ||
            source_operation->semantic_immediate != source->aux_int ||
            source_operation->flags != source->flags) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY,
                     ctx->record_count, source_value, source_operation_index);
            return false;
        }
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
    uint32_t *operation_by_value;
    uint32_t *parameter_by_value;
    const XiValue **live_by_value;
    const XiBlock **live_by_block;
    uint8_t *seen_function;
    uint8_t *seen_block;
    uint8_t *seen_record;
    uint64_t bytes;
    uint64_t work;
} VerifyAuthority;

static bool verify_charge_work(VerifyAuthority *ctx, uint64_t amount) {
    if (!ctx || amount > XR_AOT_REP_VERIFY_MAX_WORK - ctx->work) {
        if (ctx)
            set_diag(ctx->diag, XR_AOT_REFINEMENT_RESOURCE_BUDGET,
                     (uint32_t) ctx->work, 0, 0);
        return false;
    }
    ctx->work += amount;
    return true;
}

static bool verify_alloc(VerifyAuthority *ctx, uint32_t count, size_t width,
                         void **out) {
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
    xr_free(ctx->live_by_value);
    xr_free(ctx->live_by_block);
    xr_free(ctx->seen_function);
    xr_free(ctx->seen_block);
    xr_free(ctx->seen_record);
    ctx->operation_by_value = NULL;
    ctx->parameter_by_value = NULL;
    ctx->live_by_value = NULL;
    ctx->live_by_block = NULL;
    ctx->seen_function = NULL;
    ctx->seen_block = NULL;
    ctx->seen_record = NULL;
}

static bool verify_bounded_string_length(VerifyAuthority *ctx,
                                         const char *text,
                                         size_t *out_length) {
    if (out_length)
        *out_length = 0;
    if (!ctx || !text || !out_length)
        return false;
    size_t length = 0;
    while (length < XR_AOT_REP_VERIFY_MAX_TYPE_KEY_BYTES && text[length])
        length++;
    if (length == XR_AOT_REP_VERIFY_MAX_TYPE_KEY_BYTES) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_RESOURCE_BUDGET,
                 (uint32_t) ctx->work, 0, 0);
        return false;
    }
    if (!verify_charge_work(ctx, length + 1u))
        return false;
    *out_length = length;
    return true;
}

static bool verify_scalar_type_authority(VerifyAuthority *ctx,
                                         uint32_t type_index,
                                         const XrType *live) {
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(ctx ? ctx->semantic : NULL, type_index);
    if (!ctx || !live || !type || !type->canonical_key ||
        !source_type_matches(live, type) || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
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
        !verify_bounded_string_length(ctx, type->canonical_key,
                                      &canonical_length))
        return false;
    char prefix[256];
    int prefix_length = snprintf(
        prefix, sizeof(prefix),
        "type-v2:%u:%u:%u:%u:%u:%u:%u:%u:%u:%zu:",
        (unsigned) live->kind, live->semantic_type_id,
        live->is_nullable ? 1u : 0u, live->is_const ? 1u : 0u,
        live->is_value_type ? 1u : 0u, live->is_literal ? 1u : 0u,
        live->is_cycle_candidate ? 1u : 0u, live->ptr_is_mut ? 1u : 0u,
        (unsigned) live->scalar_rep, alias_length);
    return prefix_length > 0 && (size_t) prefix_length < sizeof(prefix) &&
           canonical_length == (size_t) prefix_length + alias_length &&
           memcmp(type->canonical_key, prefix, (size_t) prefix_length) == 0 &&
           memcmp(type->canonical_key + prefix_length, alias, alias_length) == 0;
}

static bool semantic_allocation_identity_is_canonical(
    const XrSemanticOperationRecord *operation) {
    static const char suffix[] = "/allocation";
    if (!operation || !operation->canonical_key || !operation->allocation_key)
        return false;
    size_t canonical_length = strlen(operation->canonical_key);
    size_t allocation_length = strlen(operation->allocation_key);
    if (canonical_length > SIZE_MAX - sizeof(suffix) ||
        allocation_length != canonical_length + sizeof(suffix) - 1u ||
        memcmp(operation->allocation_key, operation->canonical_key,
               canonical_length) != 0 ||
        memcmp(operation->allocation_key + canonical_length, suffix,
               sizeof(suffix)) != 0)
        return false;
    XrStableId expected;
    XrFingerprint digest;
    return xr_stable_id_from_key(operation->allocation_key, &expected, &digest) &&
           xr_stable_id_equal(expected, operation->allocation_id);
}

static bool semantic_heap_closure_is_exact(const XrSemanticPlan *semantic,
                                           const XrSemanticOperationRecord *operation) {
    if (!semantic || !operation || operation->opcode != XI_CLOSURE_NEW ||
        operation->callable_function >= xr_semantic_plan_function_count(semantic) ||
        operation->operand_count != 0 ||
        !semantic_allocation_identity_is_canonical(operation) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED)
        return false;
    const XrSemanticFunctionRecord *callee =
        xr_semantic_plan_function(semantic, operation->callable_function);
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(semantic, operation->result_type);
    uint32_t child_count = 0;
    const uint32_t *children =
        xr_semantic_plan_type_children(semantic, &child_count);
    bool typed_function = type && type->kind == XR_KIND_FUNCTION;
    bool opaque_closure = type && type->kind == XR_KIND_UNKNOWN &&
                          type->child_count == 0;
    if (!callee || !type || callee->parent != operation->function ||
        callee->capture_count != 0 || (!typed_function && !opaque_closure) ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        (type->flags & (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE |
                        XR_SEM_TYPE_BORROW_VIEW | XR_SEM_TYPE_AGGREGATE_EXACT)) != 0 ||
        (type->flags & (XR_SEM_TYPE_REFERENCE_CAPABLE |
                        XR_SEM_TYPE_OWNERSHIP_ROOT)) !=
            (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        callee->parameter_count == UINT16_MAX ||
        (typed_function &&
         type->child_count != (uint32_t) callee->parameter_count + 1u) ||
        type->child_begin > child_count ||
        type->child_count > child_count - type->child_begin ||
        callee->parameter_begin > xr_semantic_plan_parameter_count(semantic) ||
        callee->parameter_count > xr_semantic_plan_parameter_count(semantic) -
                                      callee->parameter_begin)
        return false;
    for (uint32_t i = 0; i < callee->parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(semantic, callee->parameter_begin + i);
        if (!parameter || parameter->function != operation->callable_function ||
            parameter->ordinal != i ||
            (typed_function &&
             children[type->child_begin + i] != parameter->type))
            return false;
    }
    return opaque_closure ||
           children[type->child_begin + callee->parameter_count] ==
               callee->return_type;
}

static bool semantic_string_literal_is_exact(
    const XrSemanticPlan *semantic,
    const XrSemanticOperationRecord *operation) {
    if (!semantic || !operation || operation->opcode != XI_CONST ||
        operation->operand_count != 0 || operation->allocation_key ||
        operation->constant >= xr_semantic_plan_constant_count(semantic) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        operation->return_complete != 1)
        return false;
    for (uint32_t i = 0; i < XR_STABLE_ID_BYTES; i++) {
        if (operation->allocation_id.bytes[i] != 0)
            return false;
    }
    const XrSemanticConstantRecord *constant =
        xr_semantic_plan_constant(semantic, operation->constant);
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(semantic, operation->result_type);
    uint8_t forbidden = XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE |
                        XR_SEM_TYPE_BORROW_VIEW |
                        XR_SEM_TYPE_AGGREGATE_EXACT;
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE |
                       XR_SEM_TYPE_OWNERSHIP_ROOT;
    return constant && constant->kind == XR_SEM_CONST_STRING &&
           constant->string && constant->type == operation->result_type &&
           type && type->kind == XR_KIND_STRING && type->child_count == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           (type->flags & forbidden) == 0 &&
           (type->flags & required) == required;
}

static bool verify_string_literal_type_authority(
    VerifyAuthority *ctx, const XrSemanticOperationRecord *operation,
    const XiValue *live) {
    const XrSemanticTypeRecord *type =
        ctx && operation
            ? xr_semantic_plan_type(ctx->semantic, operation->result_type)
            : NULL;
    const XrSemanticConstantRecord *constant =
        ctx && operation &&
                operation->constant <
                    xr_semantic_plan_constant_count(ctx->semantic)
            ? xr_semantic_plan_constant(ctx->semantic, operation->constant)
            : NULL;
    if (!ctx || !live || !live->type ||
        !semantic_string_literal_is_exact(ctx->semantic, operation) ||
        !type || !constant || !source_type_matches(live->type, type) ||
        live->type->kind != XR_KIND_STRING)
        return false;
    const char *live_bytes = live->aux ? (const char *) live->aux : "";
    if (strcmp(live_bytes, constant->string) != 0)
        return false;
    const char *alias = live->type->alias_name ? live->type->alias_name : "";
    size_t alias_length = 0;
    size_t canonical_length = 0;
    if (!verify_bounded_string_length(ctx, alias, &alias_length) ||
        !verify_bounded_string_length(ctx, type->canonical_key,
                                      &canonical_length))
        return false;
    char prefix[256];
    int prefix_length = snprintf(
        prefix, sizeof(prefix),
        "type-v2:%u:%u:%u:%u:%u:%u:%u:%u:%u:%zu:",
        (unsigned) live->type->kind, live->type->semantic_type_id,
        live->type->is_nullable ? 1u : 0u,
        live->type->is_const ? 1u : 0u,
        live->type->is_value_type ? 1u : 0u,
        live->type->is_literal ? 1u : 0u,
        live->type->is_cycle_candidate ? 1u : 0u,
        live->type->ptr_is_mut ? 1u : 0u,
        (unsigned) live->type->scalar_rep, alias_length);
    return prefix_length > 0 && (size_t) prefix_length < sizeof(prefix) &&
           canonical_length == (size_t) prefix_length + alias_length &&
           memcmp(type->canonical_key, prefix, (size_t) prefix_length) == 0 &&
           memcmp(type->canonical_key + prefix_length, alias, alias_length) == 0;
}

static bool verify_heap_closure_type_authority(
    VerifyAuthority *ctx, const XrSemanticOperationRecord *operation,
    const XiFunc *owner, const XiValue *live) {
    const XrSemanticTypeRecord *semantic_type =
        ctx && operation
            ? xr_semantic_plan_type(ctx->semantic, operation->result_type)
            : NULL;
    if (!ctx || !owner || !live ||
        !semantic_heap_closure_is_exact(ctx->semantic, operation) ||
        !semantic_type || !live->type || !live->aux ||
        !source_type_matches(live->type, semantic_type))
        return false;
    const XiFunc *callee = (const XiFunc *) live->aux;
    const XrSemanticFunctionRecord *semantic_callee =
        xr_semantic_plan_function(ctx->semantic, operation->callable_function);
    uint32_t pointer_matches = 0;
    uint32_t index_matches = 0;
    for (uint16_t i = 0; i < owner->nchildren; i++) {
        const XiFunc *child = owner->children ? owner->children[i] : NULL;
        pointer_matches += child == callee;
        if (child && child->semantic_plan_function_index ==
                         operation->callable_function) {
            index_matches++;
            if (child != callee)
                return false;
        }
    }
    bool typed_function = semantic_type->kind == XR_KIND_FUNCTION;
    if (!semantic_callee || callee->parent_func != owner || callee->ncaptures != 0 ||
        pointer_matches != 1 || index_matches != 1 ||
        callee->semantic_plan_function_index != operation->callable_function ||
        callee->nparams != semantic_callee->parameter_count ||
        (typed_function &&
         (live->type->function.param_count != semantic_callee->parameter_count ||
          !live->type->function.return_type ||
          !source_type_matches(
              live->type->function.return_type,
              xr_semantic_plan_type(ctx->semantic,
                                    semantic_callee->return_type)))) ||
        !callee->return_type ||
        !source_type_matches(callee->return_type,
                             xr_semantic_plan_type(ctx->semantic,
                                                   semantic_callee->return_type)))
        return false;
    for (uint32_t i = 0; i < semantic_callee->parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(ctx->semantic,
                                       semantic_callee->parameter_begin + i);
        if (!parameter || !callee->params || !callee->params[i] ||
            (typed_function &&
             (!live->type->function.params ||
              live->type->function.params[i].mode != parameter->mode ||
              !source_type_matches(
                  live->type->function.params[i].type,
                  xr_semantic_plan_type(ctx->semantic, parameter->type)))) ||
            !source_type_matches(callee->params[i]->type,
                                 xr_semantic_plan_type(ctx->semantic,
                                                       parameter->type)))
            return false;
    }
    return true;
}

static bool verify_authority_init(VerifyAuthority *ctx) {
    size_t function_count = xr_semantic_plan_function_count(ctx->semantic);
    size_t block_count = xr_semantic_plan_block_count(ctx->semantic);
    size_t operation_count = xr_semantic_plan_operation_count(ctx->semantic);
    size_t parameter_count = xr_semantic_plan_parameter_count(ctx->semantic);
    if (function_count > XR_AOT_REFINEMENT_MAX_RECORDS ||
        block_count > XR_AOT_REFINEMENT_MAX_RECORDS ||
        operation_count > XR_AOT_REFINEMENT_MAX_RECORDS ||
        parameter_count > XR_AOT_REFINEMENT_MAX_RECORDS) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_RESOURCE_BUDGET, 0, 0, 0);
        return false;
    }
    ctx->function_count = (uint32_t) function_count;
    ctx->block_count = (uint32_t) block_count;
    ctx->operation_count = (uint32_t) operation_count;
    ctx->parameter_count = (uint32_t) parameter_count;
    uint64_t values = 0;
    for (uint32_t i = 0; i < ctx->function_count; i++) {
        const XrSemanticFunctionRecord *function =
            xr_semantic_plan_function(ctx->semantic, i);
        if (!function ||
            (uint64_t) function->value_begin + function->value_count >
                UINT32_MAX) {
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
    if (!verify_charge_work(ctx, (uint64_t) ctx->function_count +
                                    ctx->value_count + ctx->operation_count +
                                    ctx->parameter_count))
        return false;
    if (!verify_alloc(ctx, ctx->value_count, sizeof(uint32_t),
                      (void **) &ctx->operation_by_value) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(uint32_t),
                      (void **) &ctx->parameter_by_value) ||
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
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(ctx->semantic, i);
        if (!operation || operation->result_value >= ctx->value_count ||
            ctx->operation_by_value[operation->result_value] !=
                XR_SEMANTIC_INDEX_NONE) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, i,
                     operation ? operation->result_value : 0, i);
            return false;
        }
        ctx->operation_by_value[operation->result_value] = i;
    }
    for (uint32_t i = 0; i < ctx->parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(ctx->semantic, i);
        if (!parameter || parameter->value >= ctx->value_count ||
            ctx->parameter_by_value[parameter->value] !=
                XR_SEMANTIC_INDEX_NONE) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, i,
                     parameter ? parameter->value : 0, 0);
            return false;
        }
        ctx->parameter_by_value[parameter->value] = i;
    }
    return true;
}

static bool verify_register_value(VerifyAuthority *ctx, const XiFunc *function,
                                  const XiValue *value) {
    uint32_t function_index = function->semantic_plan_function_index;
    const XrSemanticFunctionRecord *semantic_function =
        xr_semantic_plan_function(ctx->semantic, function_index);
    if (!value || !semantic_function || value->backend_origin != XI_BACKEND_VALUE_NONE ||
        value->id >= semantic_function->value_count ||
        semantic_function->value_begin > UINT32_MAX - value->id) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, 0,
                 value ? value->id : 0, 0);
        return false;
    }
    uint32_t semantic_value = semantic_function->value_begin + value->id;
    if (semantic_value >= ctx->value_count ||
        ctx->live_by_value[semantic_value] != NULL ||
        !value->block || value->block->func != function) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, 0,
                 semantic_value, 0);
        return false;
    }
    ctx->live_by_value[semantic_value] = value;
    return verify_charge_work(ctx, 1);
}

static bool verify_collect_live_authority(VerifyAuthority *ctx,
                                          const XiFunc *function,
                                          uint32_t expected_parent,
                                          uint32_t depth) {
    if (depth > XR_AOT_REP_VERIFY_MAX_FUNCTION_DEPTH) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_RESOURCE_BUDGET,
                 (uint32_t) ctx->work, 0, 0);
        return false;
    }
    if (!function || function->semantic_plan != ctx->semantic ||
        function->semantic_plan_function_index >= ctx->function_count) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_BASELINE_FINGERPRINT, 0, 0, 0);
        return false;
    }
    uint32_t function_index = function->semantic_plan_function_index;
    const XrSemanticFunctionRecord *semantic_function =
        xr_semantic_plan_function(ctx->semantic, function_index);
    if (!semantic_function || ctx->seen_function[function_index] != 0 ||
        semantic_function->parent != expected_parent ||
        function->nblocks != semantic_function->block_count ||
        xi_func_semantic_param_count(function) !=
            semantic_function->parameter_count ||
        function->nchildren != semantic_function->child_count ||
        function->next_value_id != semantic_function->value_count) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, function_index,
                 semantic_function ? semantic_function->value_begin : 0, 0);
        return false;
    }
    if (!verify_scalar_type_authority(ctx, semantic_function->return_type,
                                      function->return_type)) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_TYPE, function_index,
                 semantic_function->value_begin, 0);
        return false;
    }
    ctx->seen_function[function_index] = 1;
    if (!verify_charge_work(ctx, 1))
        return false;
    for (uint32_t b = 0; b < function->nblocks; b++) {
        const XiBlock *block = function->blocks[b];
        if (!block || block->func != function || block->id != b ||
            semantic_function->block_begin > UINT32_MAX - b) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE, b, 0, 0);
            return false;
        }
        uint32_t block_index = semantic_function->block_begin + b;
        if (block_index >= ctx->block_count ||
            ctx->seen_block[block_index] != 0) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE, b, 0, 0);
            return false;
        }
        ctx->seen_block[block_index] = 1;
        ctx->live_by_block[block_index] = block;
        if (!verify_charge_work(ctx, 1))
            return false;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            if (!verify_register_value(ctx, function, &phi->value))
                return false;
        }
        for (uint32_t v = 0; v < block->nvalues; v++) {
            if (!verify_register_value(ctx, function, block->values[v]))
                return false;
        }
    }
    for (uint16_t i = 0; i < function->nchildren; i++) {
        if (!verify_collect_live_authority(ctx, function->children[i],
                                           function_index, depth + 1u))
            return false;
    }
    return true;
}

static bool verify_live_value_id(const VerifyAuthority *ctx,
                                 const XiFunc *function,
                                 const XiValue *value,
                                 uint32_t *out_value) {
    if (out_value)
        *out_value = XR_SEMANTIC_INDEX_NONE;
    if (!ctx || !function || !value || !out_value ||
        function->semantic_plan_function_index >= ctx->function_count)
        return false;
    const XrSemanticFunctionRecord *semantic_function =
        xr_semantic_plan_function(ctx->semantic,
                                  function->semantic_plan_function_index);
    if (!semantic_function || value->id >= semantic_function->value_count ||
        semantic_function->value_begin > UINT32_MAX - value->id)
        return false;
    uint32_t semantic_value = semantic_function->value_begin + value->id;
    if (semantic_value >= ctx->value_count ||
        ctx->live_by_value[semantic_value] != value)
        return false;
    *out_value = semantic_value;
    return true;
}

static bool verify_operation_is_record_use(const VerifyAuthority *ctx,
                                           uint32_t operation) {
    if (!ctx || operation == XR_SEMANTIC_INDEX_NONE)
        return false;
    for (uint32_t i = 0; i < ctx->view->record_count; i++) {
        const XrAotTransformationRecord *record = &ctx->view->records[i];
        if (record->transform_kind ==
                XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER &&
            record->representation_adapter.use_kind ==
                XR_AOT_REP_USE_OPERATION &&
            record->representation_adapter.use_operation == operation)
            return true;
    }
    return false;
}

static bool verify_live_operation(VerifyAuthority *ctx,
                                  const XiFunc *function,
                                  const XiValue *value,
                                  uint32_t semantic_value) {
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    uint32_t parameter_index = ctx->parameter_by_value[semantic_value];
    const XrSemanticParameterRecord *parameter =
        parameter_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(ctx->semantic, parameter_index)
            : NULL;
    if (!parameter && !operation) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY,
                 operation_index, semantic_value, operation_index);
        return false;
    }
    uint32_t function_index = function->semantic_plan_function_index;
    bool exact = true;
    if (parameter) {
        exact = parameter->function == function_index &&
                parameter->value == semantic_value &&
                parameter->ordinal < xi_func_semantic_param_count(function) &&
                function->params &&
                function->params[parameter->ordinal] == value &&
                value->op == XI_PARAM &&
                value->aux_int == parameter->ordinal &&
                value->param_mode == parameter->mode &&
                value->transfer_mode == parameter->transfer_mode &&
                verify_scalar_type_authority(ctx, parameter->type,
                                             value->type);
    }
    if (!operation)
        exact = exact && parameter != NULL;
    else {
        uint32_t block_index = XR_SEMANTIC_INDEX_NONE;
        const XrSemanticFunctionRecord *semantic_function =
            xr_semantic_plan_function(ctx->semantic, function_index);
        if (value->block && semantic_function &&
            value->block->id < semantic_function->block_count)
            block_index = semantic_function->block_begin + value->block->id;
        bool type_authority = semantic_heap_closure_is_exact(ctx->semantic, operation)
                                  ? verify_heap_closure_type_authority(
                                        ctx, operation, function, value)
                              : semantic_string_literal_is_exact(ctx->semantic,
                                                                 operation)
                                  ? verify_string_literal_type_authority(
                                        ctx, operation, value)
                                  : verify_scalar_type_authority(
                                        ctx, operation->result_type, value->type);
        exact = exact && operation->function == function_index &&
                operation->block == block_index &&
                operation->result_value == semantic_value &&
                operation->opcode == value->op &&
                operation->operand_count == value->nargs &&
                operation->auxiliary_kind == value->aux_kind &&
                operation->semantic_immediate == value->aux_int &&
                operation->source_line == value->line &&
                operation->transfer_mode == value->transfer_mode &&
                operation->parameter_mode == value->param_mode &&
                operation->flags == value->flags &&
                operation->result_alias_operand == value->result_alias_operand &&
                type_authority;
        if (exact && parameter)
            exact = operation->parameter_mode == parameter->mode &&
                    operation->parameter_ownership == parameter->ownership &&
                    operation->transfer_mode == parameter->transfer_mode;
        uint32_t operand_count = 0;
        const XrSemanticOperandRecord *operands =
            xr_semantic_plan_operands(ctx->semantic, &operand_count);
        if ((!operands && operand_count != 0) ||
            operation->operand_begin > operand_count ||
            operation->operand_count > operand_count - operation->operand_begin)
            exact = false;
        for (uint16_t a = 0; exact && a < value->nargs; a++) {
            uint32_t source_value = XR_SEMANTIC_INDEX_NONE;
            const XrSemanticOperandRecord *operand =
                &operands[operation->operand_begin + a];
            exact = value->args && value->args[a] &&
                    verify_live_value_id(ctx, function, value->args[a],
                                         &source_value) &&
                    operand->value == source_value &&
                    operand->type < xr_semantic_plan_type_count(ctx->semantic);
            if (exact) {
                uint32_t source_operation_index =
                    ctx->operation_by_value[source_value];
                const XrSemanticOperationRecord *source_operation =
                    source_operation_index != XR_SEMANTIC_INDEX_NONE
                        ? xr_semantic_plan_operation(ctx->semantic,
                                                     source_operation_index)
                        : NULL;
                exact = source_operation &&
                                semantic_heap_closure_is_exact(
                                    ctx->semantic, source_operation)
                            ? verify_heap_closure_type_authority(
                                  ctx, source_operation, function,
                                  value->args[a]) &&
                                  operand->type == source_operation->result_type
                        : source_operation &&
                                  semantic_string_literal_is_exact(
                                      ctx->semantic, source_operation)
                            ? verify_string_literal_type_authority(
                                  ctx, source_operation, value->args[a]) &&
                                  operand->type == source_operation->result_type
                            : verify_scalar_type_authority(
                                  ctx, operand->type, value->args[a]->type);
            }
            if (!verify_charge_work(ctx, 1))
                return false;
        }
    }
    if (!exact) {
        uint32_t type_index = parameter ? parameter->type
                                        : operation ? operation->result_type
                                                    : XR_SEMANTIC_INDEX_NONE;
        bool type_exact = operation &&
                                  semantic_heap_closure_is_exact(ctx->semantic,
                                                                 operation)
                              ? verify_heap_closure_type_authority(
                                    ctx, operation, function, value)
                          : operation &&
                                    semantic_string_literal_is_exact(
                                        ctx->semantic, operation)
                              ? verify_string_literal_type_authority(
                                    ctx, operation, value)
                              : verify_scalar_type_authority(ctx, type_index,
                                                             value->type);
        set_diag(ctx->diag,
                 type_exact
                     ? (verify_operation_is_record_use(ctx, operation_index)
                            ? XR_AOT_REFINEMENT_USE_SITE
                            : XR_AOT_REFINEMENT_SOURCE_IDENTITY)
                     : XR_AOT_REFINEMENT_SOURCE_TYPE,
                 operation_index, semantic_value, operation_index);
        return false;
    }
    return true;
}

static bool verify_live_function(VerifyAuthority *ctx,
                                 const XiFunc *function) {
    uint32_t function_index = function->semantic_plan_function_index;
    const XrSemanticFunctionRecord *semantic_function =
        xr_semantic_plan_function(ctx->semantic, function_index);
    uint32_t predecessor_count = 0;
    const uint32_t *predecessors =
        xr_semantic_plan_predecessors(ctx->semantic, &predecessor_count);
    if (!semantic_function || (!predecessors && predecessor_count != 0))
        return false;
    if (!verify_charge_work(ctx, UINT64_C(1) + function->nblocks +
                                    semantic_function->value_count))
        return false;
    for (uint32_t b = 0; b < function->nblocks; b++) {
        const XiBlock *block = function->blocks[b];
        uint32_t block_index = semantic_function->block_begin + b;
        const XrSemanticBlockRecord *semantic_block =
            xr_semantic_plan_block(ctx->semantic, block_index);
        uint32_t control_value = XR_SEMANTIC_INDEX_NONE;
        if (block->control &&
            !verify_live_value_id(ctx, function, block->control,
                                  &control_value)) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE, block_index, 0, 0);
            return false;
        }
        bool exact = semantic_block && semantic_block->function == function_index &&
                     semantic_block->kind == block->kind &&
                     semantic_block->source_line == block->line &&
                     semantic_block->control_value == control_value &&
                     semantic_block->predecessor_count == block->npreds &&
                     semantic_block->predecessor_begin <= predecessor_count &&
                     semantic_block->predecessor_count <=
                         predecessor_count - semantic_block->predecessor_begin;
        for (uint16_t s = 0; exact && s < 2; s++) {
            uint32_t successor = XR_SEMANTIC_INDEX_NONE;
            if (block->succs[s]) {
                const XiBlock *live = block->succs[s];
                if (live->func != function ||
                    live->id >= semantic_function->block_count ||
                    ctx->live_by_block[semantic_function->block_begin +
                                       live->id] != live)
                    exact = false;
                else
                    successor = semantic_function->block_begin + live->id;
            }
            exact = exact && semantic_block->successors[s] == successor;
        }
        for (uint16_t p = 0; exact && p < block->npreds; p++) {
            const XiBlock *live = block->preds ? block->preds[p] : NULL;
            uint32_t predecessor =
                live && live->func == function &&
                        live->id < semantic_function->block_count
                    ? semantic_function->block_begin + live->id
                    : XR_SEMANTIC_INDEX_NONE;
            exact = predecessor != XR_SEMANTIC_INDEX_NONE &&
                    predecessors[semantic_block->predecessor_begin + p] ==
                        predecessor;
        }
        if (!exact) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE, block_index,
                     control_value, XR_SEMANTIC_INDEX_NONE);
            return false;
        }
    }
    for (uint32_t value = semantic_function->value_begin;
         value < semantic_function->value_begin + semantic_function->value_count;
         value++) {
        const XiValue *live = ctx->live_by_value[value];
        if (!live || !verify_live_operation(ctx, function, live, value))
            return false;
    }
    for (uint16_t i = 0; i < function->nchildren; i++) {
        if (!verify_live_function(ctx, function->children[i]))
            return false;
    }
    return true;
}

static bool oracle_machine_storage(const VerifyAuthority *ctx,
                                   uint32_t semantic_value,
                                   XrRep *out_storage,
                                   uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage ||
        !out_machine_kind)
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
    uint32_t type_index = parameter ? parameter->type
                                    : operation ? operation->result_type
                                                : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(ctx->semantic, type_index);
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *machine =
        binding ? xr_target_plan_machine_rep(ctx->target_plan,
                                              binding->register_rep)
                : NULL;
    if (!type || !binding || !machine ||
        binding->semantic_value != semantic_value || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        (type->flags & (XR_SEM_TYPE_NULLABLE |
                        XR_SEM_TYPE_AGGREGATE_EXACT)) != 0)
        return false;
    switch ((XrMachineRepKind) machine->kind) {
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
            *out_storage = XR_REP_I64;
            break;
        case XR_MACHINE_REP_F32:
        case XR_MACHINE_REP_F64:
            *out_storage = XR_REP_F64;
            break;
        case XR_MACHINE_REP_RAW_PTR:
            *out_storage = XR_REP_RAWPTR;
            break;
        default:
            return false;
    }
    *out_machine_kind = machine->kind;
    return true;
}

static bool oracle_dynamic_closure_storage(const VerifyAuthority *ctx,
                                           uint32_t semantic_value,
                                           XrRep *out_storage,
                                           uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage ||
        !out_machine_kind)
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
        binding ? xr_target_plan_machine_rep(ctx->target_plan,
                                              binding->register_rep)
                : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan,
                                              binding->memory_rep)
                : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots =
        xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts =
        xr_target_plan_layouts(ctx->target_plan, &layout_count);
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
        layout->root_field_count != 0 ||
        layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align ||
        slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index ||
        slot->function != operation->function ||
        slot->role != XR_TARGET_SLOT_TEMPORARY ||
        slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep ||
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_OWNED)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_dynamic_string_literal_storage(
    const VerifyAuthority *ctx, uint32_t semantic_value,
    XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage ||
        !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    if (!operation || operation->result_value != semantic_value ||
        !semantic_string_literal_is_exact(ctx->semantic, operation))
        return false;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan,
                                              binding->register_rep)
                : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan,
                                              binding->memory_rep)
                : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots =
        xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts =
        xr_target_plan_layouts(ctx->target_plan, &layout_count);
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
        layout->root_field_count != 0 ||
        layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align ||
        slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index ||
        slot->function != operation->function ||
        slot->role != XR_TARGET_SLOT_TEMPORARY ||
        slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep ||
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_OWNED)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static uint16_t oracle_representation_recipe(uint16_t adapter,
                                             uint16_t machine_kind) {
    bool box = adapter == XR_AOT_REP_ADAPTER_BOX;
    switch ((XrMachineRepKind) machine_kind) {
        case XR_MACHINE_REP_F32:
        case XR_MACHINE_REP_F64:
            return box ? XR_AOT_REP_RECIPE_BOX_FLOAT
                       : XR_AOT_REP_RECIPE_UNBOX_FLOAT;
        case XR_MACHINE_REP_RAW_PTR:
            return box ? XR_AOT_REP_RECIPE_BOX_REFERENCE
                       : XR_AOT_REP_RECIPE_UNBOX_REFERENCE;
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
            return box ? XR_AOT_REP_RECIPE_BOX_INTEGER
                       : XR_AOT_REP_RECIPE_UNBOX_INTEGER;
        default:
            return XR_AOT_REP_RECIPE_NONE;
    }
}

static bool oracle_definition_storage(const VerifyAuthority *ctx,
                                      uint32_t semantic_value,
                                      XrRep *out_storage,
                                      uint16_t *out_machine_kind) {
    if (ctx->parameter_by_value[semantic_value] != XR_SEMANTIC_INDEX_NONE)
        return oracle_machine_storage(ctx, semantic_value, out_storage,
                                      out_machine_kind);
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    if (!operation)
        return false;
    switch (operation->opcode) {
        case XI_CLOSURE_NEW:
            return oracle_dynamic_closure_storage(ctx, semantic_value,
                                                  out_storage,
                                                  out_machine_kind);
        case XI_CALL_BUILTIN:
        case XI_CHAN_RECV:
        case XI_CHAN_TRY_RECV:
        case XI_ENUM_DESCRIPTOR_BOX:
            *out_storage = XR_REP_TAGGED;
            *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
            return true;
        case XI_GET_SHARED: {
            XrRep machine_storage = XR_REP_TAGGED;
            if (!oracle_machine_storage(ctx, semantic_value,
                                        &machine_storage,
                                        out_machine_kind))
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
            return oracle_machine_storage(ctx, semantic_value, out_storage,
                                          out_machine_kind);
        case XI_BOX:
            *out_storage = XR_REP_TAGGED;
            *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
            return true;
        case XI_CONST:
            if (semantic_string_literal_is_exact(ctx->semantic, operation))
                return oracle_dynamic_string_literal_storage(
                    ctx, semantic_value, out_storage, out_machine_kind);
            return oracle_machine_storage(ctx, semantic_value, out_storage,
                                          out_machine_kind);
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
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT:
        case XI_ATOMIC_LOAD:
        case XI_ATOMIC_RMW:
        case XI_COPY:
        case XI_SOURCE_MOVE:
        case XI_OWNER_FORWARD:
        case XI_UNBOX:
        case XI_ENUM_DESCRIPTOR_UNBOX:
        case XI_CONVERT:
        case XI_NARROW_I8:
        case XI_NARROW_U8:
        case XI_NARROW_I16:
        case XI_NARROW_U16:
        case XI_NARROW_I32:
        case XI_NARROW_U32:
        case XI_WIDEN_I8:
        case XI_WIDEN_U8:
        case XI_WIDEN_I16:
        case XI_WIDEN_U16:
        case XI_WIDEN_I32:
        case XI_WIDEN_U32:
        case XI_NARROW_F32:
        case XI_WIDEN_F32:
            return oracle_machine_storage(ctx, semantic_value, out_storage,
                                          out_machine_kind);
        default:
            return false;
    }
}

static bool oracle_use_storage(const VerifyAuthority *ctx,
                               uint32_t operation_index,
                               uint16_t operand_index,
                               uint32_t source_value,
                               XrRep *out_storage) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(ctx->semantic, operation_index);
    if (!operation || operand_index >= operation->operand_count)
        return false;
    uint16_t ignored_kind = 0;
    switch (operation->opcode) {
        case XI_CALL_BUILTIN:
            *out_storage = XR_REP_TAGGED;
            return true;
        case XI_RELEASE: {
            XrRep literal_storage = XR_REP_TAGGED;
            if (!oracle_dynamic_string_literal_storage(
                    ctx, source_value, &literal_storage, &ignored_kind))
                return false;
            *out_storage = XR_REP_TAGGED;
            return true;
        }
        case XI_SET_SHARED:
        case XI_PRINT: {
            XrRep machine_storage = XR_REP_TAGGED;
            if (!oracle_machine_storage(ctx, source_value, &machine_storage,
                                        &ignored_kind) &&
                !oracle_dynamic_closure_storage(ctx, source_value,
                                                &machine_storage,
                                                &ignored_kind) &&
                !oracle_dynamic_string_literal_storage(
                    ctx, source_value, &machine_storage, &ignored_kind))
                return false;
            *out_storage = XR_REP_TAGGED;
            return true;
        }
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
        case XI_NARROW_I8:
        case XI_NARROW_U8:
        case XI_NARROW_I16:
        case XI_NARROW_U16:
        case XI_NARROW_I32:
        case XI_NARROW_U32:
        case XI_WIDEN_I8:
        case XI_WIDEN_U8:
        case XI_WIDEN_I16:
        case XI_WIDEN_U16:
        case XI_WIDEN_I32:
        case XI_WIDEN_U32:
        case XI_NARROW_F32:
        case XI_WIDEN_F32:
            return oracle_machine_storage(ctx, source_value, out_storage,
                                          &ignored_kind);
        case XI_PHI:
            if (ctx->policy->force_phi_tagged) {
                *out_storage = XR_REP_TAGGED;
                return true;
            }
            return oracle_machine_storage(ctx, operation->result_value,
                                          out_storage, &ignored_kind);
        case XI_CALL:
            if (operand_index == 0 || !ctx->policy->prefer_call_args_native) {
                *out_storage = XR_REP_TAGGED;
                return true;
            }
            return oracle_machine_storage(ctx, source_value, out_storage,
                                          &ignored_kind);
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT:
            if (operand_index == 0 || !ctx->policy->prefer_call_args_native) {
                *out_storage = XR_REP_TAGGED;
                return true;
            }
            return oracle_machine_storage(ctx, source_value, out_storage,
                                          &ignored_kind);
        case XI_COPY:
        case XI_SOURCE_MOVE:
        case XI_OWNER_FORWARD: {
            uint16_t result_kind = 0;
            return oracle_definition_storage(ctx, operation->result_value,
                                             out_storage, &result_kind);
        }
        case XI_BOX:
        case XI_ENUM_DESCRIPTOR_BOX:
            return oracle_machine_storage(ctx, source_value, out_storage,
                                          &ignored_kind);
        case XI_UNBOX:
        case XI_ENUM_DESCRIPTOR_UNBOX:
            *out_storage = XR_REP_TAGGED;
            return true;
        default:
            return false;
    }
}

static bool authority_add_obligation(CollectContext *ctx,
                                     const VerifyAuthority *oracle,
                                     uint32_t source_value,
                                     uint32_t use_operation,
                                     uint32_t use_block,
                                     uint16_t use_operand,
                                     uint16_t use_kind,
                                     XrRep input_storage,
                                     XrRep output_storage) {
    if (input_storage == output_storage)
        return true;
    XrRep native_storage = XR_REP_TAGGED;
    uint16_t machine_kind = XR_MACHINE_REP_COUNT;
    if (!oracle_machine_storage(oracle, source_value, &native_storage,
                                &machine_kind) ||
        ((input_storage != XR_REP_TAGGED) &&
         input_storage != native_storage) ||
        ((output_storage != XR_REP_TAGGED) &&
         output_storage != native_storage) ||
        (input_storage != XR_REP_TAGGED &&
         output_storage != XR_REP_TAGGED)) {
        set_diag(ctx->diag,
                 XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                 ctx->record_count, source_value, use_operation);
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
    uint32_t type_index = parameter ? parameter->type
                                    : operation ? operation->result_type
                                                : XR_SEMANTIC_INDEX_NONE;
    uint32_t layout = type_index < ctx->type_count
                          ? ctx->layout_by_type[type_index]
                          : XR_SEMANTIC_INDEX_NONE;
    if ((!parameter && !operation) || layout == XR_SEMANTIC_INDEX_NONE) {
        set_diag(ctx->diag,
                 XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                 ctx->record_count, source_value, use_operation);
        return false;
    }
    bool box = input_storage != XR_REP_TAGGED;
    XrAotRepresentationAdapterRequest request = {
        .source_value = source_value,
        .use_operation = use_operation,
        .use_block = use_block,
        .use_operand = use_operand,
        .use_kind = use_kind,
        .adapter_kind = box ? XR_AOT_REP_ADAPTER_BOX
                            : XR_AOT_REP_ADAPTER_UNBOX,
        .input_rep_kind = box ? machine_kind : XR_MACHINE_REP_DYN_VALUE,
        .output_rep_kind = box ? XR_MACHINE_REP_DYN_VALUE : machine_kind,
        .layout = layout,
        .policy_fingerprint = rep_policy_fingerprint(ctx->policy),
    };
    uint32_t decision = XR_AOT_REFINEMENT_REFUSED;
    if (!xr_aot_refinement_try_representation_adapter(
            ctx->builder, &ctx->protocol, ctx->target_plan, &request,
            &decision, ctx->diag) ||
        decision != XR_AOT_REFINEMENT_APPLIED) {
        if (decision != XR_AOT_REFINEMENT_APPLIED)
            set_diag(ctx->diag,
                     XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                     ctx->record_count, source_value, use_operation);
        return false;
    }
    ctx->record_count++;
    return true;
}

static bool authority_collect_obligations(CollectContext *ctx) {
    VerifyAuthority oracle = {
        .target_plan = ctx->target_plan,
        .semantic = ctx->semantic,
        .policy = ctx->policy,
        .value_count = ctx->semantic_value_count,
        .operation_by_value = ctx->operation_by_value,
        .parameter_by_value = ctx->parameter_by_value,
    };
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(ctx->semantic);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(ctx->semantic, i);
        if (!operation || operation->operand_begin > operand_count ||
            operation->operand_count >
                operand_count - operation->operand_begin) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE,
                     ctx->record_count, 0, i);
            return false;
        }
        for (uint16_t a = 0; a < operation->operand_count; a++) {
            uint32_t source_value =
                operands[operation->operand_begin + a].value;
            XrRep input_storage = XR_REP_TAGGED;
            XrRep output_storage = XR_REP_TAGGED;
            uint16_t ignored_machine = XR_MACHINE_REP_COUNT;
            if (source_value >= ctx->semantic_value_count) {
                set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE,
                         ctx->record_count, source_value, i);
                return false;
            }
            if (!oracle_definition_storage(&oracle, source_value,
                                           &input_storage,
                                           &ignored_machine) ||
                !oracle_use_storage(&oracle, i, a, source_value,
                                    &output_storage)) {
                set_diag(ctx->diag,
                         XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                         ctx->record_count, source_value, i);
                return false;
            }
            if (!authority_add_obligation(
                    ctx, &oracle, source_value, i, operation->block, a,
                    XR_AOT_REP_USE_OPERATION, input_storage,
                    output_storage))
                return false;
        }
    }
    uint32_t block_count =
        (uint32_t) xr_semantic_plan_block_count(ctx->semantic);
    for (uint32_t i = 0; i < block_count; i++) {
        const XrSemanticBlockRecord *block =
            xr_semantic_plan_block(ctx->semantic, i);
        if (!block || block->kind != XI_BLOCK_RETURN ||
            block->control_value == XR_SEMANTIC_INDEX_NONE)
            continue;
        uint32_t source_operation =
            block->control_value < ctx->semantic_value_count
                ? ctx->operation_by_value[block->control_value]
                : XR_SEMANTIC_INDEX_NONE;
        const XrSemanticOperationRecord *operation =
            source_operation != XR_SEMANTIC_INDEX_NONE
                ? xr_semantic_plan_operation(ctx->semantic,
                                             source_operation)
                : NULL;
        if (operation && operation->opcode == XI_ERR_RETURN)
            continue;
        XrRep input_storage = XR_REP_TAGGED;
        XrRep output_storage = XR_REP_TAGGED;
        uint16_t machine_kind = XR_MACHINE_REP_COUNT;
        if (!oracle_definition_storage(&oracle, block->control_value,
                                       &input_storage, &machine_kind)) {
            set_diag(ctx->diag,
                     XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                     ctx->record_count, block->control_value,
                     XR_SEMANTIC_INDEX_NONE);
            return false;
        }
        if (!ctx->policy->force_return_tagged &&
            !oracle_machine_storage(&oracle, block->control_value,
                                    &output_storage, &machine_kind)) {
            if (!oracle_dynamic_closure_storage(
                    &oracle, block->control_value, &output_storage,
                    &machine_kind)) {
                set_diag(ctx->diag,
                         XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                         ctx->record_count, block->control_value,
                         XR_SEMANTIC_INDEX_NONE);
                return false;
            }
        }
        if (!authority_add_obligation(
                ctx, &oracle, block->control_value,
                XR_SEMANTIC_INDEX_NONE, i, 0,
                XR_AOT_REP_USE_BLOCK_CONTROL, input_storage,
                output_storage))
            return false;
    }
    return true;
}

bool xr_aot_representation_refinement_build_from_authority(
    const XrTargetPlan *target_plan, const XiRepPolicy *policy,
    XrAotRefinementPlan **out_plan, XrAotRefinementDiagnostic *diag) {
    if (out_plan)
        *out_plan = NULL;
    if (diag)
        memset(diag, 0, sizeof(*diag));
    if (!target_plan || !out_plan) {
        set_diag(diag, XR_AOT_REFINEMENT_INVALID_ARGUMENT, 0, 0, 0);
        return false;
    }
    const XrSemanticPlan *semantic =
        xr_target_plan_semantic_plan(target_plan);
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
              xr_aot_refinement_builder_freeze(builder, target_plan,
                                                out_plan, diag);
    collect_indices_dispose(&ctx);
    xr_aot_refinement_builder_free(builder);
    return ok;
}

static void materialized_function_matches(const XiFunc *function,
                                          uint32_t index,
                                          const XiFunc **out,
                                          uint32_t *matches) {
    if (!function)
        return;
    if (function->semantic_plan_function_index == index) {
        *out = function;
        (*matches)++;
    }
    for (uint16_t i = 0; i < function->nchildren; i++)
        materialized_function_matches(function->children[i], index, out,
                                      matches);
}

static const XiFunc *materialized_function_by_index(const XiFunc *function,
                                                    uint32_t index) {
    const XiFunc *match = NULL;
    uint32_t matches = 0;
    materialized_function_matches(function, index, &match, &matches);
    return matches == 1 ? match : NULL;
}

static const XiValue *materialized_value_by_id(const XiFunc *function,
                                               uint32_t id,
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

static bool materialized_adapter_kind_matches(
    const XrAotRepresentationAdapterRecord *record,
    const XiValue *adapter) {
    if (!record || !adapter)
        return false;
    switch (record->adapter_kind) {
        case XR_AOT_REP_ADAPTER_BOX:
            return adapter->op == XI_BOX &&
                   adapter->backend_origin == XI_BACKEND_VALUE_REP_BOX;
        case XR_AOT_REP_ADAPTER_UNBOX:
            return adapter->op == XI_UNBOX &&
                   adapter->backend_origin == XI_BACKEND_VALUE_REP_UNBOX;
        default:
            return false;
    }
}

static bool materialized_operation_shape_matches(
    const XrSemanticOperationRecord *operation, const XiValue *value,
    uint32_t local_value) {
    return operation && value && value->backend_origin == XI_BACKEND_VALUE_NONE &&
           value->id == local_value && operation->opcode == value->op &&
           operation->operand_count == value->nargs &&
           operation->auxiliary_kind == value->aux_kind &&
           operation->semantic_immediate == value->aux_int &&
           operation->source_line == value->line &&
           operation->transfer_mode == value->transfer_mode &&
           operation->parameter_mode == value->param_mode &&
           operation->flags == value->flags &&
           operation->result_alias_operand == value->result_alias_operand;
}

static int compare_adapter_pointer(const void *left, const void *right) {
    uintptr_t a = (uintptr_t) *(const XiValue *const *) left;
    uintptr_t b = (uintptr_t) *(const XiValue *const *) right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static bool matched_adapter_contains(const XiValue *const *adapters,
                                     uint32_t count,
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

static bool verify_immutable_authority_coverage(
    const XrAotRefinementPlanView *view, const XrTargetPlan *target_plan,
    const XiRepPolicy *policy, XrAotRefinementDiagnostic *diag) {
    VerifyAuthority ctx = {
        .target_plan = target_plan,
        .semantic = xr_target_plan_semantic_plan(target_plan),
        .policy = policy,
        .view = view,
        .diag = diag,
        .policy_fingerprint = rep_policy_fingerprint(policy),
    };
    bool valid = verify_authority_init(&ctx) &&
                 verify_exact_semantic_coverage(&ctx);
    verify_authority_dispose(&ctx);
    return valid;
}

static bool verify_no_extra_materialized_adapters(
    const XiFunc *function, const XiValue *const *matched,
    uint32_t matched_count, XrAotRefinementDiagnostic *diag) {
    if (!function)
        return false;
    for (uint32_t b = 0; b < function->nblocks; b++) {
        const XiBlock *block = function->blocks[b];
        if (!block)
            return false;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            if (phi->value.backend_origin != XI_BACKEND_VALUE_NONE &&
                !matched_adapter_contains(matched, matched_count,
                                          &phi->value)) {
                set_diag(diag, XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE, 0,
                         phi->value.id, XR_SEMANTIC_INDEX_NONE);
                return false;
            }
        }
        for (uint32_t v = 0; v < block->nvalues; v++) {
            const XiValue *value = block->values[v];
            if (value && value->backend_origin != XI_BACKEND_VALUE_NONE &&
                !matched_adapter_contains(matched, matched_count, value)) {
                set_diag(diag, XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE, 0,
                         value->id, XR_SEMANTIC_INDEX_NONE);
                return false;
            }
        }
    }
    for (uint16_t i = 0; i < function->nchildren; i++) {
        if (!verify_no_extra_materialized_adapters(
                function->children[i], matched, matched_count, diag))
            return false;
    }
    return true;
}

bool xr_aot_representation_materialization_verify(
    const XrAotRefinementPlanView *view, const XiFunc *root,
    const XrTargetPlan *target_plan, const XiRepPolicy *policy,
    XrAotRefinementDiagnostic *diag) {
    if (diag)
        memset(diag, 0, sizeof(*diag));
    if (!view || !root || !target_plan ||
        root->semantic_plan != xr_target_plan_semantic_plan(target_plan) ||
        !xr_aot_refinement_verify(view, target_plan, diag)) {
        if (diag && diag->issue == XR_AOT_REFINEMENT_OK)
            set_diag(diag, XR_AOT_REFINEMENT_BASELINE_FINGERPRINT,
                     0, 0, 0);
        return false;
    }
    XiRepPolicy default_policy = xi_rep_policy_native_boundary();
    const XiRepPolicy *effective_policy = policy ? policy : &default_policy;
    if (!verify_immutable_authority_coverage(
            view, target_plan, effective_policy, diag))
        return false;
    XrFingerprint expected_policy =
        rep_policy_fingerprint(effective_policy);
    const XrSemanticPlan *semantic =
        xr_target_plan_semantic_plan(target_plan);
    const XiValue **matched = NULL;
    if (view->record_count) {
        matched = (const XiValue **) xr_calloc(view->record_count,
                                               sizeof(*matched));
        if (!matched) {
            set_diag(diag, XR_AOT_REFINEMENT_OUT_OF_MEMORY, 0, 0, 0);
            return false;
        }
    }
    bool valid = true;
    for (uint32_t i = 0; valid && i < view->record_count; i++) {
        const XrAotTransformationRecord *transformation =
            &view->records[i];
        const XrAotRepresentationAdapterRecord *record =
            &transformation->representation_adapter;
        if (transformation->transform_kind !=
                XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER ||
            transformation->decision != XR_AOT_REFINEMENT_APPLIED ||
            !xr_fingerprint_equal(record->policy_fingerprint,
                                  expected_policy)) {
            set_diag(diag, XR_AOT_REFINEMENT_STALE_EVIDENCE, i,
                     record->source_value, record->use_operation);
            valid = false;
            break;
        }
        const XrSemanticFunctionRecord *semantic_function =
            xr_semantic_plan_function(semantic, record->source_function);
        const XiFunc *function = materialized_function_by_index(
            root, record->source_function);
        if (!semantic_function || !function ||
            record->source_value < semantic_function->value_begin ||
            record->source_value >= semantic_function->value_begin +
                                        semantic_function->value_count) {
            set_diag(diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, i,
                     record->source_value, record->use_operation);
            valid = false;
            break;
        }
        uint32_t source_local =
            record->source_value - semantic_function->value_begin;
        uint32_t source_matches = 0;
        const XiValue *source = materialized_value_by_id(
            function, source_local, &source_matches);
        uint32_t source_function = XR_SEMANTIC_INDEX_NONE;
        uint32_t source_value = XR_SEMANTIC_INDEX_NONE;
        if (!source || source_matches != 1 ||
            !xr_aot_scalar_semantic_value_id(
                target_plan, function, source, &source_function,
                &source_value, NULL, 0) ||
            source_function != record->source_function ||
            source_value != record->source_value) {
            set_diag(diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, i,
                     record->source_value, record->use_operation);
            valid = false;
            break;
        }
        const XiValue *adapter = NULL;
        if (record->use_kind == XR_AOT_REP_USE_OPERATION) {
            const XrSemanticOperationRecord *use =
                xr_semantic_plan_operation(semantic,
                                           record->use_operation);
            const XrSemanticFunctionRecord *use_function =
                use ? xr_semantic_plan_function(semantic, use->function)
                    : NULL;
            const XiFunc *live_use_function =
                use ? materialized_function_by_index(root, use->function)
                    : NULL;
            uint32_t use_local =
                use && use_function &&
                        use->result_value >= use_function->value_begin
                    ? use->result_value - use_function->value_begin
                    : UINT32_MAX;
            uint32_t use_matches = 0;
            const XiValue *user = materialized_value_by_id(
                live_use_function, use_local, &use_matches);
            if (!use || !use_function || !live_use_function ||
                use->function != record->source_function ||
                use->block != record->use_block || use_matches != 1 ||
                !materialized_operation_shape_matches(use, user,
                                                      use_local) ||
                record->use_operand >= user->nargs || !user->args) {
                set_diag(diag, XR_AOT_REFINEMENT_USE_SITE, i,
                         record->source_value, record->use_operation);
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
                record->use_block >= semantic_function->block_begin +
                                         semantic_function->block_count) {
                set_diag(diag, XR_AOT_REFINEMENT_USE_SITE, i,
                         record->source_value, record->use_operation);
                valid = false;
                break;
            }
            uint32_t local_block =
                record->use_block - semantic_function->block_begin;
            adapter = local_block < function->nblocks &&
                              function->blocks[local_block]
                          ? function->blocks[local_block]->control
                          : NULL;
        } else {
            set_diag(diag, XR_AOT_REFINEMENT_USE_SITE, i,
                     record->source_value, record->use_operation);
            valid = false;
            break;
        }
        if (!adapter || adapter->nargs != 1 || !adapter->args ||
            adapter->args[0] != source ||
            !materialized_adapter_kind_matches(record, adapter) ||
            !xr_aot_rep_adapter_value_is_exact(
                target_plan, function, adapter, NULL, 0)) {
            set_diag(diag, XR_AOT_REFINEMENT_REPRESENTATION, i,
                     record->source_value, record->use_operation);
            valid = false;
            break;
        }
        matched[i] = adapter;
    }
    if (valid && view->record_count) {
        qsort(matched, view->record_count, sizeof(*matched),
              compare_adapter_pointer);
    }
    if (valid)
        valid = verify_no_extra_materialized_adapters(
            root, matched, view->record_count, diag);
    xr_free(matched);
    if (valid && diag)
        memset(diag, 0, sizeof(*diag));
    return valid;
}

static int compare_exact_key(const XrAotTransformationRecord *record,
                             uint32_t source_function,
                             uint32_t source_value,
                             uint32_t use_operation, uint32_t use_block,
                             uint16_t use_operand, uint16_t use_kind) {
    if (record->transform_kind != XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER)
        return record->transform_kind < XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER
                   ? -1
                   : 1;
    const XrAotRepresentationAdapterRecord *adapter =
        &record->representation_adapter;
#define XR_COMPARE_EXACT(field, expected)                                                         \
    do {                                                                                           \
        if (adapter->field != (expected))                                                         \
            return adapter->field < (expected) ? -1 : 1;                                          \
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

static uint32_t find_exact_record(const VerifyAuthority *ctx,
                                  uint32_t source_function,
                                  uint32_t source_value,
                                  uint32_t use_operation,
                                  uint32_t use_block,
                                  uint16_t use_operand,
                                  uint16_t use_kind) {
    uint32_t low = 0;
    uint32_t high = ctx->view->record_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        int order = compare_exact_key(&ctx->view->records[middle],
                                      source_function, source_value,
                                      use_operation, use_block, use_operand,
                                      use_kind);
        if (order < 0)
            low = middle + 1u;
        else
            high = middle;
    }
    return low < ctx->view->record_count &&
                   compare_exact_key(&ctx->view->records[low], source_function,
                                     source_value, use_operation, use_block,
                                     use_operand, use_kind) == 0
               ? low
               : XR_SEMANTIC_INDEX_NONE;
}

static bool verify_exact_record_authority(
    VerifyAuthority *ctx, uint32_t record_index, uint32_t source_value,
    uint32_t use_operation, uint32_t use_block, uint16_t use_kind) {
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
        set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, record_index,
                 source_value, operation_index);
        return false;
    }
    uint32_t source_function = parameter ? parameter->function
                                         : operation ? operation->function
                                                     : XR_SEMANTIC_INDEX_NONE;
    uint32_t source_type = parameter ? parameter->type
                                     : operation ? operation->result_type
                                                 : XR_SEMANTIC_INDEX_NONE;
    uint16_t source_kind = parameter ? XR_AOT_REP_SOURCE_PARAMETER
                                     : XR_AOT_REP_SOURCE_OPERATION;
    XrStableId source_id = parameter ? parameter->id : operation->id;
    uint8_t source_auxiliary = parameter ? 0 : operation->auxiliary_kind;
    uint8_t source_flags = parameter ? parameter->flags : operation->flags;
    int64_t source_immediate =
        parameter ? parameter->ordinal : operation->semantic_immediate;
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(ctx->semantic, source_type);
    if (!type || record->source_function != source_function ||
        record->source_value != source_value ||
        record->source_operation != operation_index ||
        record->source_type != source_type ||
        record->source_kind != source_kind ||
        record->source_auxiliary_kind != source_auxiliary ||
        record->source_flags != source_flags ||
        record->source_semantic_immediate != source_immediate ||
        !xr_stable_id_equal(record->source_operation_id, source_id) ||
        !xr_stable_id_equal(record->source_type_id, type->id)) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, record_index,
                 source_value, operation_index);
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
            set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE, record_index,
                     source_value, use_operation);
            return false;
        }
        use_id = use->id;
        use_auxiliary = use->auxiliary_kind;
        use_flags = use->flags;
        use_immediate = use->semantic_immediate;
    } else {
        const XrSemanticBlockRecord *block =
            xr_semantic_plan_block(ctx->semantic, use_block);
        if (!block || block->control_value != source_value) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE, record_index,
                     source_value, use_operation);
            return false;
        }
        use_id = block->id;
        use_flags = (uint8_t) block->kind;
    }
    if (record->use_auxiliary_kind != use_auxiliary ||
        record->use_flags != use_flags ||
        record->use_semantic_immediate != use_immediate ||
        !xr_stable_id_equal(record->use_operation_id, use_id)) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE, record_index,
                 source_value, use_operation);
        return false;
    }
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, source_value);
    const XrTargetMachineRepRecord *machine =
        binding ? xr_target_plan_machine_rep(ctx->target_plan,
                                              binding->register_rep)
                : NULL;
    if (!binding || !machine || binding->semantic_value != source_value ||
        record->target_register_rep != binding->register_rep ||
        record->target_memory_rep != binding->memory_rep ||
        record->target_slot != binding->slot) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION, record_index,
                 source_value, use_operation);
        return false;
    }
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts =
        xr_target_plan_layouts(ctx->target_plan, &layout_count);
    if (!layouts || record->layout >= layout_count ||
        layouts[record->layout].semantic_type != source_type) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_LAYOUT, record_index,
                 source_value, use_operation);
        return false;
    }
    return true;
}

static bool verify_exact_obligation(VerifyAuthority *ctx,
                                    uint32_t source_value,
                                    uint32_t use_operation,
                                    uint32_t use_block,
                                    uint16_t use_operand,
                                    uint16_t use_kind,
                                    XrRep output_storage) {
    XrRep input_storage = XR_REP_TAGGED;
    XrRep native_storage = XR_REP_TAGGED;
    uint16_t machine_kind = XR_MACHINE_REP_COUNT;
    if (!oracle_definition_storage(ctx, source_value, &input_storage,
                                   &machine_kind)) {
        set_diag(ctx->diag,
                 XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                 (uint32_t) ctx->work, source_value, use_operation);
        return false;
    }
    if (!verify_charge_work(ctx, 1))
        return false;
    if (input_storage == output_storage)
        return true;
    if (!oracle_machine_storage(ctx, source_value, &native_storage,
                                &machine_kind)) {
        set_diag(ctx->diag,
                 XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                 (uint32_t) ctx->work, source_value, use_operation);
        return false;
    }
    if ((input_storage != XR_REP_TAGGED && input_storage != native_storage) ||
        (output_storage != XR_REP_TAGGED &&
         output_storage != native_storage)) {
        set_diag(ctx->diag,
                 XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
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
    uint32_t source_function = parameter ? parameter->function
                                         : operation ? operation->function
                                                     : XR_SEMANTIC_INDEX_NONE;
    uint32_t record_index = find_exact_record(
        ctx, source_function, source_value, use_operation, use_block,
        use_operand, use_kind);
    if (record_index == XR_SEMANTIC_INDEX_NONE) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE,
                 (uint32_t) ctx->work, source_value, use_operation);
        return false;
    }
    if (ctx->seen_record[record_index] != 0) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_DUPLICATE_USE, record_index,
                 source_value, use_operation);
        return false;
    }
    ctx->seen_record[record_index] = 1;
    const XrAotRepresentationAdapterRecord *record =
        &ctx->view->records[record_index].representation_adapter;
    uint16_t expected_adapter = input_storage == XR_REP_TAGGED
                                    ? XR_AOT_REP_ADAPTER_UNBOX
                                    : XR_AOT_REP_ADAPTER_BOX;
    uint16_t expected_input = input_storage == XR_REP_TAGGED
                                  ? XR_MACHINE_REP_DYN_VALUE
                                  : machine_kind;
    uint16_t expected_output = output_storage == XR_REP_TAGGED
                                   ? XR_MACHINE_REP_DYN_VALUE
                                   : machine_kind;
    uint16_t expected_recipe =
        oracle_representation_recipe(expected_adapter, machine_kind);
    if (!verify_exact_record_authority(ctx, record_index, source_value,
                                       use_operation, use_block, use_kind))
        return false;
    if (!xr_fingerprint_equal(record->policy_fingerprint,
                              ctx->policy_fingerprint)) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_STALE_EVIDENCE, record_index,
                 source_value, use_operation);
        return false;
    }
    if (expected_recipe == XR_AOT_REP_RECIPE_NONE ||
        record->adapter_kind != expected_adapter ||
        record->recipe != expected_recipe ||
        record->input_rep_kind != expected_input ||
        record->output_rep_kind != expected_output) {
        set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION, record_index,
                 source_value, use_operation);
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
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(ctx->semantic, i);
        if (!operation || operation->operand_begin > operand_count ||
            operation->operand_count > operand_count - operation->operand_begin) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_USE_SITE, i, 0, i);
            return false;
        }
        for (uint16_t a = 0; a < operation->operand_count; a++) {
            uint32_t source_value =
                operands[operation->operand_begin + a].value;
            XrRep output_storage = XR_REP_TAGGED;
            if (source_value >= ctx->value_count ||
                !oracle_use_storage(ctx, i, a, source_value,
                                    &output_storage)) {
                set_diag(ctx->diag,
                         XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                         i, source_value, i);
                return false;
            }
            if (!verify_exact_obligation(
                    ctx, source_value, i, operation->block, a,
                    XR_AOT_REP_USE_OPERATION, output_storage))
                return false;
        }
    }
    for (uint32_t i = 0; i < ctx->block_count; i++) {
        const XrSemanticBlockRecord *block =
            xr_semantic_plan_block(ctx->semantic, i);
        if (!block || block->control_value == XR_SEMANTIC_INDEX_NONE)
            continue;
        if (!verify_charge_work(ctx, 1))
            return false;
        if (block->kind != XI_BLOCK_RETURN)
            continue;
        uint32_t source_operation =
            block->control_value < ctx->value_count
                ? ctx->operation_by_value[block->control_value]
                : XR_SEMANTIC_INDEX_NONE;
        const XrSemanticOperationRecord *operation =
            source_operation != XR_SEMANTIC_INDEX_NONE
                ? xr_semantic_plan_operation(ctx->semantic, source_operation)
                : NULL;
        if (operation && operation->opcode == XI_ERR_RETURN)
            continue;
        XrRep output_storage = XR_REP_TAGGED;
        if (!ctx->policy->force_return_tagged) {
            uint16_t machine_kind = 0;
            if (!oracle_machine_storage(ctx, block->control_value,
                                        &output_storage, &machine_kind) &&
                !oracle_dynamic_closure_storage(
                    ctx, block->control_value, &output_storage,
                    &machine_kind)) {
                set_diag(ctx->diag,
                         XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                         i, block->control_value, XR_SEMANTIC_INDEX_NONE);
                return false;
            }
        }
        if (!verify_exact_obligation(
                ctx, block->control_value, XR_SEMANTIC_INDEX_NONE, i, 0,
                XR_AOT_REP_USE_BLOCK_CONTROL, output_storage))
            return false;
    }
    if (!verify_charge_work(ctx, ctx->view->record_count))
        return false;
    for (uint32_t i = 0; i < ctx->view->record_count; i++) {
        if (ctx->view->records[i].transform_kind ==
                XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER &&
            ctx->seen_record[i] == 0) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE, i,
                     ctx->view->records[i]
                         .representation_adapter.source_value,
                     ctx->view->records[i]
                         .representation_adapter.use_operation);
            return false;
        }
    }
    return true;
}

bool xr_aot_representation_refinement_verify(
    const XrAotRefinementPlanView *view, const XiFunc *root,
    const XrTargetPlan *target_plan, const XiRepPolicy *policy,
    XrAotRefinementDiagnostic *diag) {
    if (!view || !root || !target_plan || root->semantic_plan !=
                                          xr_target_plan_semantic_plan(target_plan)) {
        set_diag(diag, XR_AOT_REFINEMENT_BASELINE_FINGERPRINT, 0, 0, 0);
        return false;
    }
    if (!xr_aot_refinement_verify(view, target_plan, diag))
        return false;
    XiRepPolicy default_policy = xi_rep_policy_native_boundary();
    VerifyAuthority ctx = {
        .target_plan = target_plan,
        .semantic = xr_target_plan_semantic_plan(target_plan),
        .policy = policy ? policy : &default_policy,
        .view = view,
        .diag = diag,
        .policy_fingerprint =
            rep_policy_fingerprint(policy ? policy : &default_policy),
    };
    bool valid = verify_authority_init(&ctx) &&
                 verify_collect_live_authority(
                     &ctx, root, XR_SEMANTIC_INDEX_NONE, 0);
    if (valid)
        valid = verify_charge_work(&ctx, (uint64_t) ctx.function_count +
                                             ctx.block_count);
    if (valid) {
        for (uint32_t i = 0; i < ctx.function_count; i++) {
            if (ctx.seen_function[i] == 0) {
                set_diag(diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, i, 0, 0);
                valid = false;
                break;
            }
        }
    }
    if (valid) {
        for (uint32_t i = 0; i < ctx.block_count; i++) {
            if (ctx.seen_block[i] == 0) {
                set_diag(diag, XR_AOT_REFINEMENT_USE_SITE, i, 0, 0);
                valid = false;
                break;
            }
        }
    }
    if (valid)
        valid = verify_live_function(&ctx, root) &&
                verify_exact_semantic_coverage(&ctx);
    verify_authority_dispose(&ctx);
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
