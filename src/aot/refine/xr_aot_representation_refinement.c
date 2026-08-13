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
#include "../../base/xglobal_indices.h"
#include "../../base/xmalloc.h"
#include "../../ir/xi_opt.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "../../plan/semantic/xr_semantic_graph.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/value/xtype_names.h"
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

static uint32_t live_builtin_type(const XrType *type) {
    static const struct {
        const char *name;
        uint32_t id;
    } builtins[] = {
        {"StringBuilder", XR_TID_STRINGBUILDER},
        {"Task", XR_TID_COROUTINE},
        {"WorkQueue", XR_TID_WORKQUEUE},
        {"ResultGroup", XR_TID_RESULTGROUP},
        {"CountdownLatch", XR_TID_COUNTDOWNLATCH},
        {"Semaphore", XR_TID_SEMAPHORE},
        {"EventCount", XR_TID_EVENTCOUNT},
    };
    for (size_t index = 0; index < sizeof(builtins) / sizeof(builtins[0]);
         index++)
        if (xr_type_is_builtin_named_class(type, builtins[index].name))
            return builtins[index].id;
    return XR_TID_NULL;
}

static bool source_type_matches(const XrType *live,
                                const XrSemanticTypeRecord *semantic) {
    if (!live || !semantic || (uint32_t) live->kind != semantic->kind ||
        live->scalar_rep != semantic->scalar_rep ||
        live_builtin_type(live) != semantic->builtin_type)
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
        case XR_MACHINE_REP_ENUM_ORDINAL:
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
    ctx->operation_by_value = NULL;
    ctx->parameter_by_value = NULL;
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
        "type-v3:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%zu:",
        (unsigned) live->kind, live->semantic_type_id, live_builtin_type(live),
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

/* Rebuilt independently of both the TargetPlan collector and its verifier: a
 * direct-local call may carry an owned String result, whose storage is the
 * outer tagged XrValue. Every other non-scalar result stays fail closed. */
static bool aot_direct_local_string_result_is_exact(
    const XrSemanticPlan *semantic, uint32_t operation_index) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(semantic, operation_index);
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
    uint8_t forbidden = XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE |
                        XR_SEM_TYPE_BORROW_VIEW | XR_SEM_TYPE_AGGREGATE_EXACT;
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    if (!semantic || !operation || !type ||
        (operation->opcode != XI_CALL && operation->opcode != XI_TAIL_CALL) ||
        operation->result_alias_operand != -1 ||
        operation->return_parameter != -1 || operation->return_complete != 1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED ||
        type->kind != XR_KIND_STRING || type->child_count != 0 ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->aggregate_extent != 0 ||
        type->aggregate_align != 0 || (type->flags & forbidden) != 0 ||
        (type->flags & required) != required)
        return false;
    size_t target_count = xr_semantic_plan_call_target_count(semantic);
    const XrSemanticFunctionRecord *callee = NULL;
    for (size_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(semantic, i);
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
           callee->return_parameter == -1 &&
           callee->return_provenance == XR_SEM_RETURN_OWNED;
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
    int written = snprintf(expected_type_key, sizeof(expected_type_key),
                           "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;named:4:JSON[0]",
                           (unsigned) XR_KIND_CLASS, (unsigned) XR_TID_NULL,
                           (unsigned) XR_SCALAR_REP_NONE);
    XrStableId zero = {{0}};
    return semantic && operation && type && written > 0 &&
           (size_t) written < sizeof(expected_type_key) &&
           operation->opcode == XI_GET_BUILTIN && operation->operand_count == 0 &&
           operation->metadata_count == 1 && operation->metadata_begin < metadata_count &&
           metadata && strcmp(metadata[operation->metadata_begin], "JSON") == 0 &&
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
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
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

static bool aot_stringbuilder_constructor_is_exact(
    const XrSemanticPlan *semantic,
    const XrSemanticOperationRecord *operation) {
    char expected_type_key[160];
    uint32_t metadata_count = 0;
    const char *const *metadata =
        xr_semantic_plan_metadata(semantic, &metadata_count);
    const XrSemanticTypeRecord *type = operation
        ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
    int written = snprintf(
        expected_type_key, sizeof(expected_type_key),
        "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;named:13:StringBuilder[0]",
        (unsigned) XR_KIND_INSTANCE, (unsigned) XR_TID_STRINGBUILDER,
        (unsigned) XR_SCALAR_REP_NONE);
    return semantic && operation && type && written > 0 &&
           (size_t) written < sizeof(expected_type_key) &&
           operation->opcode == XI_CALL_BUILTIN &&
           operation->operand_count == 0 && operation->metadata_count == 1 &&
           operation->metadata_begin < metadata_count && metadata &&
           strcmp(metadata[operation->metadata_begin], "StringBuilder") == 0 &&
           operation->auxiliary_kind == XI_AUX_KIND_NONE &&
           operation->semantic_immediate == 0 &&
           operation->constant == XR_SEMANTIC_INDEX_NONE &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
           operation->effects == xi_generated_op_effects(XI_CALL_BUILTIN) &&
           operation->flags == xi_generated_op_default_flags(XI_CALL_BUILTIN) &&
           operation->ownership_use == xi_generated_op_own_use(XI_CALL_BUILTIN) &&
           operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
           operation->transfer_mode == XR_TRANSFER_SHARE &&
           operation->parameter_mode == XR_PARAM_READ &&
           operation->parameter_ownership == XI_OWN_NONE &&
           operation->result_alias_operand == -1 &&
           operation->return_provenance == XR_SEM_RETURN_OWNED &&
           operation->return_parameter == -1 && operation->return_complete == 1 &&
           semantic_allocation_identity_is_canonical(operation) &&
           type->kind == XR_KIND_INSTANCE &&
           type->builtin_type == XR_TID_STRINGBUILDER &&
           type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->flags ==
               (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           type->canonical_key &&
           strcmp(type->canonical_key, expected_type_key) == 0;
}

static bool aot_stable_id_is_zero(XrStableId id) {
    for (uint32_t i = 0; i < XR_STABLE_ID_BYTES; i++)
        if (id.bytes[i] != 0)
            return false;
    return true;
}

static bool aot_channel_type_is_exact(const XrSemanticPlan *semantic,
                                      uint32_t type_index,
                                      uint32_t *element_type) {
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(semantic, type_index);
    uint32_t child_count = 0;
    const uint32_t *children =
        xr_semantic_plan_type_children(semantic, &child_count);
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE |
                       XR_SEM_TYPE_OWNERSHIP_ROOT;
    uint8_t allowed = required | XR_SEM_TYPE_CONST;
    if (!semantic || !type || type->kind != XR_KIND_CHANNEL ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count != 1 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        (type->flags & required) != required ||
        (type->flags & ~allowed) != 0 || type->child_begin >= child_count ||
        children[type->child_begin] >= xr_semantic_plan_type_count(semantic))
        return false;
    if (element_type)
        *element_type = children[type->child_begin];
    return true;
}

static bool aot_channel_capacity_type_is_exact(
    VerifyAuthority *ctx, uint32_t type_index) {
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(ctx->semantic, type_index);
    if (!type || type->kind != XR_KIND_INT || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->flags != 0)
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
        case XR_NATIVE_USIZE: return true;
        default: return false;
    }
}

static bool aot_channel_allocation_is_exact(
    VerifyAuthority *ctx, const XrSemanticOperationRecord *operation) {
    if (!ctx || !operation || operation->opcode != XI_CHAN_NEW ||
        operation->result_value >= ctx->value_count ||
        operation->operand_count != 1 ||
        !semantic_allocation_identity_is_canonical(operation) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != 0 ||
        operation->effects != xi_generated_op_effects(XI_CHAN_NEW) ||
        operation->flags != xi_generated_op_default_flags(XI_CHAN_NEW) ||
        operation->result_ownership !=
            xi_generated_op_result_ownership(XI_CHAN_NEW) ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_parameter != -1 || operation->return_complete != 1)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    uint32_t element_type = XR_SEMANTIC_INDEX_NONE;
    if (operation->operand_begin >= operand_count ||
        !aot_channel_type_is_exact(ctx->semantic, operation->result_type,
                                   &element_type))
        return false;
    const XrSemanticOperandRecord *capacity =
        &operands[operation->operand_begin];
    return capacity->value < ctx->value_count && capacity->parameter == -1 &&
           capacity->role == XR_SEM_OPERAND_VALUE && capacity->flags == 0 &&
           aot_channel_capacity_type_is_exact(ctx, capacity->type) &&
           element_type < xr_semantic_plan_type_count(ctx->semantic);
}

static bool aot_channel_identity_copy_is_exact(
    VerifyAuthority *ctx, const XrSemanticOperationRecord *operation) {
    if (!ctx || !operation || !ctx->exact_channel_value ||
        operation->opcode != XI_COPY || operation->operand_count != 1 ||
        operation->semantic_immediate != XI_COPY_KIND_IDENTITY ||
        operation->allocation_key ||
        !aot_stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->effects != xi_generated_op_effects(XI_COPY) ||
        operation->flags != xi_generated_op_default_flags(XI_COPY) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_alias_operand != 0 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_parameter != -1 || operation->return_complete != 1)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    if (operation->operand_begin >= operand_count)
        return false;
    const XrSemanticOperandRecord *source =
        &operands[operation->operand_begin];
    uint32_t source_element = XR_SEMANTIC_INDEX_NONE;
    uint32_t result_element = XR_SEMANTIC_INDEX_NONE;
    return source->value < ctx->value_count &&
           ctx->exact_channel_value[source->value] &&
           source->parameter == -1 && source->role == XR_SEM_OPERAND_VALUE &&
           source->flags == 0 &&
           aot_channel_type_is_exact(ctx->semantic, source->type,
                                     &source_element) &&
           aot_channel_type_is_exact(ctx->semantic, operation->result_type,
                                     &result_element) &&
           source_element == result_element;
}

static bool aot_index_channel_values(VerifyAuthority *ctx) {
    if (!verify_alloc(ctx, ctx->value_count, sizeof(uint8_t),
                      (void **) &ctx->exact_channel_value) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(uint8_t),
                      (void **) &ctx->exact_channel_allocation_value))
        return false;
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(ctx->semantic, i);
        if (!operation || operation->result_value >= ctx->value_count)
            return false;
        bool allocation = aot_channel_allocation_is_exact(ctx, operation);
        bool alias = aot_channel_identity_copy_is_exact(ctx, operation);
        if (operation->opcode == XI_CHAN_NEW && !allocation) {
            set_diag(ctx->diag, XR_AOT_REFINEMENT_SOURCE_TYPE, i,
                     operation->result_value, i);
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

static bool aot_direct_local_callee_type_is_exact(
    const XrSemanticPlan *semantic,
    const XrSemanticOperationRecord *operation,
    uint32_t target_function) {
    if (!semantic || !operation || operation->opcode != XI_GET_SHARED ||
        operation->semantic_immediate < 0 ||
        operation->semantic_immediate > UINT16_MAX ||
        operation->operand_count != 0 || operation->allocation_key ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_ownership !=
            xi_generated_op_result_ownership(XI_GET_SHARED) ||
        operation->effects != xi_generated_op_effects(XI_GET_SHARED) ||
        operation->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        operation->return_complete != 1 || operation->return_parameter != -1 ||
        target_function >= xr_semantic_plan_function_count(semantic))
        return false;
    for (uint32_t i = 0; i < XR_STABLE_ID_BYTES; i++)
        if (operation->allocation_id.bytes[i] != 0)
            return false;
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(semantic, operation->result_type);
    const XrSemanticFunctionRecord *target =
        xr_semantic_plan_function(semantic, target_function);
    uint32_t lexical_owner = target ? target->parent : XR_SEMANTIC_INDEX_NONE;
    uint32_t caller_ancestor = operation->function;
    for (uint32_t depth = 0;
         caller_ancestor != XR_SEMANTIC_INDEX_NONE &&
         caller_ancestor != lexical_owner &&
         depth < xr_semantic_plan_function_count(semantic);
         depth++) {
        const XrSemanticFunctionRecord *ancestor =
            xr_semantic_plan_function(semantic, caller_ancestor);
        caller_ancestor = ancestor ? ancestor->parent : XR_SEMANTIC_INDEX_NONE;
    }
    if (!type || !target || lexical_owner == XR_SEMANTIC_INDEX_NONE ||
        caller_ancestor != lexical_owner ||
        (type->kind != XR_KIND_FUNCTION &&
         type->kind != XR_KIND_UNKNOWN) ||
        type->scalar_rep != XR_SCALAR_REP_NONE ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->child_count != 0 ||
        target->parameter_begin > xr_semantic_plan_parameter_count(semantic) ||
        target->parameter_count > xr_semantic_plan_parameter_count(semantic) -
                                      target->parameter_begin ||
        (type->flags & (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE |
                        XR_SEM_TYPE_BORROW_VIEW |
                        XR_SEM_TYPE_AGGREGATE_EXACT)) != 0 ||
        (type->flags & (XR_SEM_TYPE_REFERENCE_CAPABLE |
                        XR_SEM_TYPE_OWNERSHIP_ROOT)) !=
            (XR_SEM_TYPE_REFERENCE_CAPABLE |
             XR_SEM_TYPE_OWNERSHIP_ROOT))
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
        !verify_alloc(ctx, ctx->value_count, sizeof(uint32_t),
                      (void **) &use_count) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(uint8_t),
                      (void **) &invalid)) {
        xr_free(target_by_operation);
        xr_free(use_count);
        xr_free(invalid);
        return false;
    }
    for (uint32_t i = 0; i < ctx->value_count; i++)
        ctx->direct_callee_target_by_value[i] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < ctx->operation_count; i++)
        target_by_operation[i] = XR_SEMANTIC_INDEX_NONE;
    uint32_t target_count =
        (uint32_t) xr_semantic_plan_call_target_count(ctx->semantic);
    for (uint32_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(ctx->semantic, i);
        if (target && target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL)
            continue;
        if (!target || target->operation >= ctx->operation_count ||
            target_by_operation[target->operation] !=
                XR_SEMANTIC_INDEX_NONE)
            goto invalid_authority;
        target_by_operation[target->operation] = i;
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *use =
            xr_semantic_plan_operation(ctx->semantic, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin)
            goto invalid_authority;
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand =
                &operands[use->operand_begin + a];
            if (operand->value >= ctx->value_count)
                goto invalid_authority;
            uint32_t source_index = ctx->operation_by_value[operand->value];
            const XrSemanticOperationRecord *source =
                source_index == XR_SEMANTIC_INDEX_NONE
                    ? NULL
                    : xr_semantic_plan_operation(ctx->semantic,
                                                 source_index);
            if (!source || source->opcode != XI_GET_SHARED)
                continue;
            uint32_t target_index = target_by_operation[i];
            const XrSemanticCallTargetRecord *target =
                target_index == XR_SEMANTIC_INDEX_NONE
                    ? NULL
                    : xr_semantic_plan_call_target(ctx->semantic,
                                                   target_index);
            uint32_t value = source->result_value;
            bool exact_use =
                a == 0 &&
                (use->opcode == XI_CALL || use->opcode == XI_TAIL_CALL) &&
                use->function == source->function && target &&
                target->operation == i &&
                target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
                operand->role == XR_SEM_OPERAND_CALLEE &&
                operand->parameter == -1 &&
                (operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0 &&
                operand->type == source->result_type &&
                value == operand->value;
            if (!exact_use ||
                (ctx->direct_callee_target_by_value[value] !=
                     XR_SEMANTIC_INDEX_NONE &&
                 ctx->direct_callee_target_by_value[value] !=
                     target->function) ||
                use_count[value] == UINT32_MAX) {
                invalid[value] = 1;
                continue;
            }
            ctx->direct_callee_target_by_value[value] = target->function;
            use_count[value]++;
        }
    }
    for (uint32_t i = 0; i < ctx->block_count; i++) {
        const XrSemanticBlockRecord *block =
            xr_semantic_plan_block(ctx->semantic, i);
        if (!block || block->control_value == XR_SEMANTIC_INDEX_NONE ||
            block->control_value >= ctx->value_count)
            continue;
        uint32_t source_index =
            ctx->operation_by_value[block->control_value];
        const XrSemanticOperationRecord *source =
            source_index == XR_SEMANTIC_INDEX_NONE
                ? NULL
                : xr_semantic_plan_operation(ctx->semantic, source_index);
        if (source && source->opcode == XI_GET_SHARED)
            invalid[block->control_value] = 1;
    }
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(ctx->semantic, i);
        if (!operation || operation->opcode != XI_GET_SHARED ||
            operation->result_value >= ctx->value_count ||
            ctx->direct_callee_target_by_value[operation->result_value] ==
                XR_SEMANTIC_INDEX_NONE)
            continue;
        uint32_t value = operation->result_value;
        if (invalid[value] || use_count[value] == 0 ||
            !aot_direct_local_callee_type_is_exact(
                ctx->semantic, operation,
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
    set_diag(ctx->diag,
             XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
             (uint32_t) ctx->work, 0, 0);
    return false;
}

static bool aot_source_namespace_operation_is_exact(
    const XrSemanticPlan *semantic,
    const XrSemanticOperationRecord *operation, uint16_t opcode) {
    const XrSemanticTypeRecord *type = operation
        ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
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
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->child_count == 0 && type->aggregate_extent == 0 &&
           type->aggregate_align == 0 &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE |
                           XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           ((opcode == XI_IMPORT_REF && operation->operand_count == 0 &&
             operation->semantic_immediate >= -1 &&
             operation->semantic_immediate <= UINT16_MAX &&
             operation->metadata_count == 2) ||
            (opcode == XI_GET_SHARED && operation->operand_count == 0 &&
             operation->semantic_immediate >= 0 &&
             operation->semantic_immediate <= UINT16_MAX &&
             operation->metadata_count == 0));
}

static bool aot_source_namespace_identity_copy_is_exact(
    const XrSemanticPlan *semantic,
    const XrSemanticOperationRecord *operation,
    const XrSemanticOperandRecord *operands, uint32_t operand_count) {
    const XrSemanticTypeRecord *type = operation
        ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
    if (!semantic || !operation || !operands || !type ||
        operation->opcode != XI_COPY || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count ||
        operation->semantic_immediate != XI_COPY_KIND_IDENTITY ||
        operation->allocation_key ||
        !aot_stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != 0 || operation->metadata_count != 0 ||
        operation->effects != xi_generated_op_effects(XI_COPY) ||
        operation->flags != xi_generated_op_default_flags(XI_COPY) ||
        operation->ownership_use != xi_generated_op_own_use(XI_COPY) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_alias_operand != 0 ||
        operation->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        operation->return_parameter != -1 || operation->return_complete != 1 ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE |
                        XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    const XrSemanticOperandRecord *source =
        &operands[operation->operand_begin];
    return source->role == XR_SEM_OPERAND_VALUE && source->parameter == -1 &&
           source->type == operation->result_type && source->flags == 0;
}

static bool aot_source_namespace_value_operation_is_exact(
    const XrSemanticPlan *semantic,
    const XrSemanticOperationRecord *operation) {
    if (!operation)
        return false;
    if (operation->opcode == XI_IMPORT_REF ||
        operation->opcode == XI_GET_SHARED)
        return aot_source_namespace_operation_is_exact(
            semantic, operation, operation->opcode);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    return aot_source_namespace_identity_copy_is_exact(
        semantic, operation, operands, operand_count);
}

/* Independent from both Target collectors: derive the complete frozen
 * IMPORT_REF -> SET_SHARED -> GET_SHARED -> SOURCE_EXPORT receiver chain. */
static bool aot_index_source_namespace_values(VerifyAuthority *ctx) {
    uint32_t *target_by_operation = NULL;
    uint32_t *expected_uses = NULL;
    uint32_t *retain_uses = NULL;
    uint32_t *consumer = NULL;
    uint32_t *visit_epoch = NULL;
    uint8_t *candidate = NULL;
    if (!verify_alloc(ctx, ctx->value_count,
                      sizeof(*ctx->exact_source_namespace_value),
                      (void **) &ctx->exact_source_namespace_value) ||
        !verify_alloc(ctx, ctx->value_count,
                      sizeof(*ctx->source_namespace_dependency_by_value),
                      (void **) &ctx->source_namespace_dependency_by_value) ||
        !verify_alloc(ctx, ctx->operation_count,
                      sizeof(*target_by_operation),
                      (void **) &target_by_operation) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*expected_uses),
                      (void **) &expected_uses) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*retain_uses),
                      (void **) &retain_uses) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*consumer),
                      (void **) &consumer) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*visit_epoch),
                      (void **) &visit_epoch) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*candidate),
                      (void **) &candidate)) {
        xr_free(target_by_operation); xr_free(expected_uses);
        xr_free(retain_uses); xr_free(consumer);
        xr_free(visit_epoch); xr_free(candidate);
        return false;
    }
    for (uint32_t i = 0; i < ctx->value_count; i++) {
        ctx->source_namespace_dependency_by_value[i] = XR_SEMANTIC_INDEX_NONE;
        consumer[i] = XR_SEMANTIC_INDEX_NONE;
    }
    for (uint32_t i = 0; i < ctx->operation_count; i++)
        target_by_operation[i] = XR_SEMANTIC_INDEX_NONE;
    uint32_t target_count =
        (uint32_t) xr_semantic_plan_call_target_count(ctx->semantic);
    for (uint32_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(ctx->semantic, i);
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
    const char *const *metadata =
        xr_semantic_plan_metadata(ctx->semantic, &metadata_count);
    uint32_t next_epoch = 1;
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        uint32_t target_index = target_by_operation[i];
        if (target_index == XR_SEMANTIC_INDEX_NONE)
            continue;
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(ctx->semantic, target_index);
        const XrSemanticOperationRecord *call =
            xr_semantic_plan_operation(ctx->semantic, i);
        if (!target || !call || call->opcode != XI_CALL_METHOD ||
            call->operand_count == 0 || call->operand_begin >= operand_count)
            goto invalid;
        const XrSemanticOperandRecord *receiver = &operands[call->operand_begin];
        if (receiver->role != XR_SEM_OPERAND_RECEIVER ||
            receiver->parameter != -1 ||
            receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
            receiver->parameter_mode != XR_PARAM_READ ||
            receiver->access != XR_CALL_ARG_PLAIN ||
            (receiver->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0)
            goto invalid;
        const XrSemanticOperationRecord *load = NULL;
        uint32_t current_value = receiver->value;
        uint32_t consumer_index = i;
        uint32_t namespace_type = receiver->type;
        uint32_t epoch = next_epoch++;
        if (epoch == 0) {
            memset(visit_epoch, 0,
                   ctx->value_count * sizeof(*visit_epoch));
            epoch = next_epoch++;
        }
        for (uint32_t depth = 0;; depth++) {
            if (depth >= ctx->operation_count ||
                current_value >= ctx->value_count ||
                visit_epoch[current_value] == epoch)
                goto invalid;
            visit_epoch[current_value] = epoch;
            uint32_t definition_index = ctx->operation_by_value[current_value];
            const XrSemanticOperationRecord *source =
                definition_index != XR_SEMANTIC_INDEX_NONE
                    ? xr_semantic_plan_operation(ctx->semantic,
                                                 definition_index)
                    : NULL;
            if (!source || source->result_value != current_value ||
                source->result_type != namespace_type ||
                source->function != call->function ||
                (candidate[current_value] &&
                 (ctx->source_namespace_dependency_by_value[current_value] !=
                      target->dependency ||
                  consumer[current_value] != consumer_index)))
                goto invalid;
            candidate[current_value] = 1;
            ctx->source_namespace_dependency_by_value[current_value] =
                target->dependency;
            consumer[current_value] = consumer_index;
            if (aot_source_namespace_operation_is_exact(
                    ctx->semantic, source, XI_GET_SHARED)) {
                load = source;
                break;
            }
            if (!aot_source_namespace_identity_copy_is_exact(
                    ctx->semantic, source, operands, operand_count))
                goto invalid;
            const XrSemanticOperandRecord *input =
                &operands[source->operand_begin];
            consumer_index = definition_index;
            current_value = input->value;
        }
        uint32_t store_index = XR_SEMANTIC_INDEX_NONE;
        for (uint32_t j = 0; j < ctx->operation_count; j++) {
            const XrSemanticOperationRecord *store =
                xr_semantic_plan_operation(ctx->semantic, j);
            if (!store || store->function != 0 || store->opcode != XI_SET_SHARED ||
                store->semantic_immediate != load->semantic_immediate)
                continue;
            if (store_index != XR_SEMANTIC_INDEX_NONE)
                goto invalid;
            store_index = j;
        }
        const XrSemanticOperationRecord *store =
            store_index != XR_SEMANTIC_INDEX_NONE
                ? xr_semantic_plan_operation(ctx->semantic, store_index) : NULL;
        if (!store || store->operand_count != 1 ||
            store->operand_begin >= operand_count)
            goto invalid;
        const XrSemanticOperandRecord *stored = &operands[store->operand_begin];
        const XrSemanticOperationRecord *import = NULL;
        current_value = stored->value;
        consumer_index = store_index;
        namespace_type = stored->type;
        epoch = next_epoch++;
        if (epoch == 0) {
            memset(visit_epoch, 0,
                   ctx->value_count * sizeof(*visit_epoch));
            epoch = next_epoch++;
        }
        for (uint32_t depth = 0;; depth++) {
            if (depth >= ctx->operation_count ||
                current_value >= ctx->value_count ||
                visit_epoch[current_value] == epoch)
                goto invalid;
            visit_epoch[current_value] = epoch;
            uint32_t definition_index = ctx->operation_by_value[current_value];
            const XrSemanticOperationRecord *source =
                definition_index != XR_SEMANTIC_INDEX_NONE
                    ? xr_semantic_plan_operation(ctx->semantic,
                                                 definition_index)
                    : NULL;
            if (!source || source->result_value != current_value ||
                source->result_type != namespace_type ||
                source->function != 0 ||
                (candidate[current_value] &&
                 (ctx->source_namespace_dependency_by_value[current_value] !=
                      target->dependency ||
                  consumer[current_value] != consumer_index)))
                goto invalid;
            candidate[current_value] = 1;
            ctx->source_namespace_dependency_by_value[current_value] =
                target->dependency;
            consumer[current_value] = consumer_index;
            if (aot_source_namespace_operation_is_exact(
                    ctx->semantic, source, XI_IMPORT_REF)) {
                import = source;
                break;
            }
            if (!aot_source_namespace_identity_copy_is_exact(
                    ctx->semantic, source, operands, operand_count))
                goto invalid;
            const XrSemanticOperandRecord *input =
                &operands[source->operand_begin];
            consumer_index = definition_index;
            current_value = input->value;
        }
        const XrSemanticDependencyRecord *dependency =
            xr_semantic_plan_dependency(ctx->semantic, target->dependency);
        if (!import || !load || receiver->type != load->result_type ||
            import->function != 0 || load->function != call->function ||
            store->function != 0 ||
            store->semantic_immediate != load->semantic_immediate ||
            stored->type != import->result_type ||
            stored->role != XR_SEM_OPERAND_VALUE || stored->parameter != -1 ||
            stored->ownership_action != XR_SEM_OPERAND_CONSUME ||
            stored->parameter_mode != XR_PARAM_READ ||
            stored->access != XR_CALL_ARG_PLAIN || stored->flags != 0 ||
            load->result_type != import->result_type || !dependency || !metadata ||
            import->metadata_begin > metadata_count ||
            import->metadata_count > metadata_count - import->metadata_begin ||
            strcmp(metadata[import->metadata_begin], dependency->module_path) != 0 ||
            metadata[import->metadata_begin + 1u][0] != '\0')
            goto invalid;
    }
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *use =
            xr_semantic_plan_operation(ctx->semantic, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin)
            goto invalid;
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand = &operands[use->operand_begin + a];
            if (operand->value >= ctx->value_count || !candidate[operand->value])
                continue;
            const XrSemanticOperationRecord *source =
                xr_semantic_plan_operation(
                    ctx->semantic, ctx->operation_by_value[operand->value]);
            bool expected = i == consumer[operand->value] && a == 0;
            if (expected && use->opcode == XI_CALL_METHOD)
                expected = target_by_operation[i] != XR_SEMANTIC_INDEX_NONE &&
                           xr_semantic_plan_call_target(
                               ctx->semantic, target_by_operation[i])->dependency ==
                               ctx->source_namespace_dependency_by_value[
                                   operand->value] &&
                           operand->role == XR_SEM_OPERAND_RECEIVER;
            else if (expected)
                expected = (use->opcode == XI_COPY ||
                            use->opcode == XI_SET_SHARED) &&
                           operand->role == XR_SEM_OPERAND_VALUE;
            if (expected) {
                if (expected_uses[operand->value] != 0)
                    goto invalid;
                expected_uses[operand->value] = 1;
                continue;
            }
            bool retain = source && source->opcode == XI_IMPORT_REF &&
                          use->opcode == XI_RETAIN && a == 0 &&
                          use->function == source->function &&
                          operand->role == XR_SEM_OPERAND_VALUE &&
                          operand->type == source->result_type &&
                          operand->parameter == -1 && operand->flags == 0;
            if (!retain || retain_uses[operand->value] != 0)
                goto invalid;
            retain_uses[operand->value] = 1;
        }
    }
    uint32_t block_count =
        (uint32_t) xr_semantic_plan_block_count(ctx->semantic);
    for (uint32_t i = 0; i < block_count; i++) {
        const XrSemanticBlockRecord *block =
            xr_semantic_plan_block(ctx->semantic, i);
        if (!block || (block->control_value != XR_SEMANTIC_INDEX_NONE &&
                       (block->control_value >= ctx->value_count ||
                        candidate[block->control_value])))
            goto invalid;
    }
    for (uint32_t i = 0; i < ctx->value_count; i++) {
        if (!candidate[i])
            continue;
        const XrSemanticOperationRecord *source = xr_semantic_plan_operation(
            ctx->semantic, ctx->operation_by_value[i]);
        if (!source || expected_uses[i] != 1)
            goto invalid;
        ctx->exact_source_namespace_value[i] = 1;
    }
    xr_free(target_by_operation); xr_free(expected_uses);
    xr_free(retain_uses); xr_free(consumer);
    xr_free(visit_epoch); xr_free(candidate);
    return true;
invalid:
    xr_free(target_by_operation); xr_free(expected_uses);
    xr_free(retain_uses); xr_free(consumer);
    xr_free(visit_epoch); xr_free(candidate);
    set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
             (uint32_t) ctx->work, 0, 0);
    return false;
}

typedef struct AotGoStoreRow {
    uint32_t function;
    uint32_t operation;
    uint16_t slot;
    uint8_t occupied;
    uint8_t ambiguous;
} AotGoStoreRow;

static AotGoStoreRow *aot_go_store_lookup(
    AotGoStoreRow *rows, uint32_t capacity, uint32_t function,
    uint16_t slot, bool insert) {
    uint32_t cursor =
        (function * UINT32_C(3266489917) ^ (uint32_t) slot) &
        (capacity - 1u);
    for (uint32_t probe = 0; probe < capacity; probe++) {
        AotGoStoreRow *row = &rows[cursor];
        if (!row->occupied) {
            if (!insert)
                return NULL;
            row->occupied = 1;
            row->function = function;
            row->slot = slot;
            row->operation = XR_SEMANTIC_INDEX_NONE;
            return row;
        }
        if (row->function == function && row->slot == slot)
            return row;
        cursor = (cursor + 1u) & (capacity - 1u);
    }
    return NULL;
}

static bool aot_go_store_is_initial_initializer(
    const XrSemanticPlan *semantic, uint32_t function_index,
    uint32_t store_index) {
    const XrSemanticFunctionRecord *function =
        xr_semantic_plan_function(semantic, function_index);
    const XrSemanticOperationRecord *store =
        xr_semantic_plan_operation(semantic, store_index);
    const XrSemanticBlockRecord *entry =
        function ? xr_semantic_plan_block(semantic, function->block_begin) : NULL;
    if (!function || !store || !entry || entry->function != function_index ||
        store->block != function->block_begin ||
        store_index < entry->operation_begin ||
        store_index >= entry->operation_begin + entry->operation_count)
        return false;
    for (uint32_t i = entry->operation_begin; i < store_index; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->opcode == XI_CALL ||
            operation->opcode == XI_TAIL_CALL ||
            operation->opcode == XI_CALL_METHOD ||
            operation->opcode == XI_CALL_METHOD_DIRECT ||
            operation->opcode == XI_CALL_BUILTIN ||
            operation->opcode == XI_GO ||
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
    uint32_t store_capacity = 1;
    while (store_capacity < ctx->operation_count * 2u)
        store_capacity <<= 1u;
    AotGoStoreRow *stores = NULL;
    uint32_t *uses = NULL;
    uint8_t *candidate = NULL;
    uint8_t *invalid = NULL;
    XrSemanticGraph graph = {0};
    bool graph_ready = false;
    if (!verify_alloc(ctx, store_capacity, sizeof(*stores),
                      (void **) &stores) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*uses),
                      (void **) &uses) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*candidate),
                      (void **) &candidate) ||
        !verify_alloc(ctx, ctx->value_count, sizeof(*invalid),
                      (void **) &invalid) ||
        !verify_alloc(ctx, ctx->value_count,
                      sizeof(*ctx->go_callee_target_by_value),
                      (void **) &ctx->go_callee_target_by_value) ||
        !verify_alloc(ctx, ctx->value_count,
                      sizeof(*ctx->exact_go_callee_value),
                      (void **) &ctx->exact_go_callee_value)) {
        xr_free(stores);
        xr_free(uses);
        xr_free(candidate);
        xr_free(invalid);
        return false;
    }
    for (uint32_t i = 0; i < ctx->value_count; i++)
        ctx->go_callee_target_by_value[i] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(ctx->semantic, i);
        if (!operation)
            goto rejected;
        if (operation->opcode != XI_SET_SHARED ||
            operation->semantic_immediate < 0 ||
            operation->semantic_immediate > UINT16_MAX)
            continue;
        AotGoStoreRow *row = aot_go_store_lookup(
            stores, store_capacity, operation->function,
            (uint16_t) operation->semantic_immediate, true);
        if (!row)
            goto rejected;
        if (row->operation != XR_SEMANTIC_INDEX_NONE)
            row->ambiguous = 1;
        else
            row->operation = i;
    }
    char graph_error[128] = {0};
    if (!xr_semantic_graph_build(ctx->semantic, &graph, graph_error,
                                 sizeof(graph_error)))
        goto rejected;
    graph_ready = true;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    for (uint32_t i = 0; i < ctx->operation_count; i++) {
        const XrSemanticOperationRecord *use =
            xr_semantic_plan_operation(ctx->semantic, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin)
            goto rejected;
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand =
                &operands[use->operand_begin + a];
            const XrSemanticOperationRecord *load =
                operand->value < ctx->value_count &&
                        ctx->operation_by_value[operand->value] <
                            ctx->operation_count
                    ? xr_semantic_plan_operation(
                          ctx->semantic,
                          ctx->operation_by_value[operand->value])
                    : NULL;
            if (!load || load->opcode != XI_GET_SHARED || use->opcode != XI_GO)
                continue;
            uint32_t value = load->result_value;
            candidate[value] = 1;
            AotGoStoreRow *row =
                load->semantic_immediate >= 0 &&
                        load->semantic_immediate <= UINT16_MAX
                    ? aot_go_store_lookup(
                          stores, store_capacity, load->function,
                          (uint16_t) load->semantic_immediate, false)
                    : NULL;
            const XrSemanticOperationRecord *store =
                row && !row->ambiguous && row->operation < ctx->operation_count
                    ? xr_semantic_plan_operation(ctx->semantic, row->operation)
                    : NULL;
            const XrSemanticOperandRecord *stored =
                store && store->operand_count == 1 &&
                        store->operand_begin < operand_count
                    ? &operands[store->operand_begin]
                    : NULL;
            const XrSemanticOperationRecord *closure =
                stored && stored->value < ctx->value_count &&
                        ctx->operation_by_value[stored->value] <
                            ctx->operation_count
                    ? xr_semantic_plan_operation(
                          ctx->semantic,
                          ctx->operation_by_value[stored->value])
                    : NULL;
            uint32_t target = closure ? closure->callable_function
                                      : XR_SEMANTIC_INDEX_NONE;
            const XrSemanticFunctionRecord *callee =
                xr_semantic_plan_function(ctx->semantic, target);
            bool initialized = store &&
                (store->block == load->block
                     ? row->operation < ctx->operation_by_value[value]
                     : xr_semantic_graph_dominates(&graph, store->block,
                                                   load->block));
            bool exact = row && !row->ambiguous && store && stored && closure &&
                callee && a == 0 && initialized &&
                aot_go_store_is_initial_initializer(
                    ctx->semantic, store->function, row->operation) &&
                store->opcode == XI_SET_SHARED &&
                store->function == load->function &&
                store->semantic_immediate == load->semantic_immediate &&
                !store->allocation_key && aot_stable_id_is_zero(store->allocation_id) &&
                store->constant == XR_SEMANTIC_INDEX_NONE &&
                store->callable_function == XR_SEMANTIC_INDEX_NONE &&
                store->effects == xi_generated_op_effects(XI_SET_SHARED) &&
                store->result_ownership ==
                    xi_generated_op_result_ownership(XI_SET_SHARED) &&
                stored->role == XR_SEM_OPERAND_VALUE && stored->parameter == -1 &&
                stored->transfer_mode == XR_TRANSFER_SHARE &&
                stored->ownership_action == XR_SEM_OPERAND_CONSUME &&
                stored->parameter_mode == XR_PARAM_READ &&
                stored->access == XR_CALL_ARG_PLAIN &&
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
                use->effects == xi_generated_op_effects(XI_GO) &&
                operand->value == value && operand->type == load->result_type &&
                operand->role == XR_SEM_OPERAND_VALUE && operand->parameter == -1 &&
                operand->transfer_mode == XR_TRANSFER_SHARE &&
                operand->ownership_action == XR_SEM_OPERAND_BORROW &&
                operand->parameter_mode == XR_PARAM_READ &&
                operand->access == XR_CALL_ARG_PLAIN &&
                operand->origin == XI_PLACE_ORIGIN_NONE &&
                operand->lifetime == XI_PLACE_LIFETIME_NONE &&
                operand->escape == XI_PLACE_ESCAPE_NONE && operand->flags == 0 &&
                aot_direct_local_callee_type_is_exact(
                    ctx->semantic, load, target);
            for (uint16_t argument = 1; exact && argument < use->operand_count;
                 argument++) {
                const XrSemanticOperandRecord *arg =
                    &operands[use->operand_begin + argument];
                const XrSemanticParameterRecord *parameter =
                    xr_semantic_plan_parameter(
                        ctx->semantic,
                        callee->parameter_begin + argument - 1u);
                exact = parameter && arg->type == parameter->type &&
                        arg->role == XR_SEM_OPERAND_VALUE && arg->parameter == -1 &&
                        arg->parameter_mode == XR_PARAM_READ &&
                        arg->access == XR_CALL_ARG_PLAIN &&
                        arg->origin == XI_PLACE_ORIGIN_NONE &&
                        arg->lifetime == XI_PLACE_LIFETIME_NONE &&
                        arg->escape == XI_PLACE_ESCAPE_NONE && arg->flags == 0;
            }
            if (!exact ||
                (ctx->go_callee_target_by_value[value] !=
                     XR_SEMANTIC_INDEX_NONE &&
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
        const XrSemanticOperationRecord *use =
            xr_semantic_plan_operation(ctx->semantic, i);
        for (uint16_t a = 0; use && a < use->operand_count; a++) {
            uint32_t value = operands[use->operand_begin + a].value;
            if (value < ctx->value_count && candidate[value] &&
                (use->opcode != XI_GO || a != 0))
                invalid[value] = 1;
        }
    }
    for (uint32_t i = 0; i < ctx->block_count; i++) {
        const XrSemanticBlockRecord *block =
            xr_semantic_plan_block(ctx->semantic, i);
        if (block && block->control_value < ctx->value_count &&
            candidate[block->control_value])
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
    set_diag(ctx->diag,
             XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
             (uint32_t) ctx->work, 0, 0);
    return false;
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
        "type-v3:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%zu:",
        (unsigned) live->type->kind, live->type->semantic_type_id,
        live_builtin_type(live->type),
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

static bool verify_static_shared_callable_type_authority(
    VerifyAuthority *ctx, const XrSemanticOperationRecord *operation,
    const XiFunc *owner, const XiValue *live, const uint8_t *exact_values,
    const uint32_t *targets) {
    if (!ctx || !operation || !owner || !live || !live->type ||
        operation->result_value >= ctx->value_count ||
        !exact_values || !targets ||
        !exact_values[operation->result_value])
        return false;
    uint32_t target_index =
        targets[operation->result_value];
    const XrSemanticTypeRecord *semantic_type =
        xr_semantic_plan_type(ctx->semantic, operation->result_type);
    const XrSemanticFunctionRecord *semantic_target =
        xr_semantic_plan_function(ctx->semantic, target_index);
    const XiFunc *shared_owner = owner;
    uint32_t shared_owner_index = operation->function;
    for (uint32_t depth = 0;
         semantic_target && shared_owner &&
         shared_owner_index != semantic_target->parent &&
         depth < ctx->function_count;
         depth++) {
        const XrSemanticFunctionRecord *semantic_owner =
            xr_semantic_plan_function(ctx->semantic, shared_owner_index);
        shared_owner_index = semantic_owner ? semantic_owner->parent
                                            : XR_SEMANTIC_INDEX_NONE;
        shared_owner = shared_owner->parent_func;
    }
    if (!aot_direct_local_callee_type_is_exact(ctx->semantic, operation,
                                                target_index) ||
        !semantic_type || !semantic_target ||
        shared_owner_index != semantic_target->parent || !shared_owner ||
        shared_owner->semantic_plan_function_index != shared_owner_index ||
        !source_type_matches(live->type, semantic_type) ||
        !shared_owner->shared_slot_funcs ||
        operation->semantic_immediate >= shared_owner->shared_slot_func_count)
        return false;
    bool typed_function = semantic_type->kind == XR_KIND_FUNCTION;
    if (typed_function &&
        (live->type->kind != XR_KIND_FUNCTION ||
         live->type->function.param_count !=
             semantic_target->parameter_count ||
         !live->type->function.return_type))
        return false;
    uint16_t slot = (uint16_t) operation->semantic_immediate;
    const XiFunc *callee = shared_owner->shared_slot_funcs[slot];
    uint32_t pointer_matches = 0;
    uint32_t index_matches = 0;
    for (uint16_t i = 0; i < shared_owner->nchildren; i++) {
        const XiFunc *child =
            shared_owner->children ? shared_owner->children[i] : NULL;
        pointer_matches += child == callee;
        if (child && child->semantic_plan_function_index == target_index) {
            index_matches++;
            if (child != callee)
                return false;
        }
    }
    if (!callee || callee->parent_func != shared_owner || pointer_matches != 1 ||
        index_matches != 1 ||
        callee->semantic_plan_function_index != target_index ||
        callee->nparams != semantic_target->parameter_count ||
        !callee->return_type ||
        !source_type_matches(
            callee->return_type,
            xr_semantic_plan_type(ctx->semantic,
                                  semantic_target->return_type)) ||
        (typed_function &&
         !source_type_matches(
             live->type->function.return_type,
             xr_semantic_plan_type(ctx->semantic,
                                   semantic_target->return_type))))
        return false;
    for (uint32_t i = 0; i < semantic_target->parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(ctx->semantic,
                                       semantic_target->parameter_begin + i);
        if (!parameter || !callee->params || !callee->params[i] ||
            (typed_function &&
             (!live->type->function.params ||
              live->type->function.params[i].mode != parameter->mode ||
              !source_type_matches(
                  live->type->function.params[i].type,
                  xr_semantic_plan_type(ctx->semantic,
                                        parameter->type)))) ||
            !source_type_matches(
                callee->params[i]->type,
                xr_semantic_plan_type(ctx->semantic, parameter->type)))
            return false;
    }
    return true;
}


static bool verify_direct_local_callee_type_authority(
    VerifyAuthority *ctx, const XrSemanticOperationRecord *operation,
    const XiFunc *owner, const XiValue *live) {
    return verify_static_shared_callable_type_authority(
        ctx, operation, owner, live, ctx->exact_direct_callee_value,
        ctx->direct_callee_target_by_value);
}

static bool verify_direct_local_go_callee_type_authority(
    VerifyAuthority *ctx, const XrSemanticOperationRecord *operation,
    const XiFunc *owner, const XiValue *live) {
    return verify_static_shared_callable_type_authority(
        ctx, operation, owner, live, ctx->exact_go_callee_value,
        ctx->go_callee_target_by_value);
}

static bool verify_source_namespace_type_authority(
    VerifyAuthority *ctx, const XrSemanticOperationRecord *operation,
    const XiValue *live) {
    if (!ctx || !operation || !live || !live->type ||
        operation->result_value >= ctx->value_count ||
        !ctx->exact_source_namespace_value ||
        !ctx->exact_source_namespace_value[operation->result_value] ||
        !aot_source_namespace_value_operation_is_exact(
            ctx->semantic, operation) ||
        !source_type_matches(
            live->type,
            xr_semantic_plan_type(ctx->semantic, operation->result_type)))
        return false;
    if (operation->opcode == XI_IMPORT_REF) {
        uint32_t metadata_count = 0;
        const char *const *metadata =
            xr_semantic_plan_metadata(ctx->semantic, &metadata_count);
        const XiImportRef *ref =
            live->aux ? (const XiImportRef *) live->aux : NULL;
        const char *member = ref ? ref->member_name : NULL;
        return live->op == XI_IMPORT_REF && ref && metadata &&
               operation->metadata_begin + 1u < metadata_count &&
               ref->module_path &&
               strcmp(ref->module_path,
                      metadata[operation->metadata_begin]) == 0 &&
               strcmp(member ? member : "",
                      metadata[operation->metadata_begin + 1u]) == 0;
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
               live->aux_int == XI_COPY_KIND_IDENTITY && live->nargs == 1 &&
               live->args && live->block && live->block->func &&
               live->block->func->semantic_plan_function_index ==
                   operation->function &&
               source_value >= function->value_begin &&
               source_value - function->value_begin < function->value_count &&
               live->args[0] == ctx->live_by_value[source_value] &&
               live->args[0] && live->args[0]->block &&
               live->args[0]->block->func == live->block->func;
    }
    return operation->opcode == XI_GET_SHARED && live->op == XI_GET_SHARED &&
           live->aux_int == operation->semantic_immediate;
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
    return aot_index_direct_local_callee_values(ctx) &&
           aot_index_direct_local_go_callee_values(ctx) &&
           aot_index_source_namespace_values(ctx) &&
           aot_index_channel_values(ctx);
}

static bool verify_channel_type_authority(
    VerifyAuthority *ctx, const XrSemanticOperationRecord *operation,
    const XiValue *live) {
    uint32_t element_type = XR_SEMANTIC_INDEX_NONE;
    if (!ctx || !operation || !live || !live->type ||
        operation->result_value >= ctx->value_count ||
        !ctx->exact_channel_value ||
        !ctx->exact_channel_value[operation->result_value] ||
        !aot_channel_type_is_exact(ctx->semantic, operation->result_type,
                                   &element_type) ||
        live->type->kind != XR_KIND_CHANNEL ||
        !source_type_matches(live->type,
                             xr_semantic_plan_type(ctx->semantic,
                                                   operation->result_type)) ||
        !live->type->container.element_type)
        return false;
    return source_type_matches(
        live->type->container.element_type,
        xr_semantic_plan_type(ctx->semantic, element_type));
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
                              : operation->result_value < ctx->value_count &&
                                        ctx->exact_direct_callee_value[
                                            operation->result_value]
                                  ? verify_direct_local_callee_type_authority(
                                        ctx, operation, function, value)
                              : operation->result_value < ctx->value_count &&
                                        ctx->exact_go_callee_value[
                                            operation->result_value]
                                  ? verify_direct_local_go_callee_type_authority(
                                        ctx, operation, function, value)
                              : operation->result_value < ctx->value_count &&
                                        ctx->exact_source_namespace_value &&
                                        ctx->exact_source_namespace_value[
                                            operation->result_value]
                                  ? verify_source_namespace_type_authority(
                                        ctx, operation, value)
                              : semantic_string_literal_is_exact(ctx->semantic,
                                                                 operation)
                                  ? verify_string_literal_type_authority(
                                        ctx, operation, value)
                              : operation->result_value < ctx->value_count &&
                                        ctx->exact_channel_value[
                                            operation->result_value]
                                  ? verify_channel_type_authority(
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
                                  source_operation->result_value <
                                      ctx->value_count &&
                                  ctx->exact_direct_callee_value[
                                      source_operation->result_value]
                            ? verify_direct_local_callee_type_authority(
                                  ctx, source_operation, function,
                                  value->args[a]) &&
                                  operand->type == source_operation->result_type
                        : source_operation &&
                                  source_operation->result_value <
                                      ctx->value_count &&
                                  ctx->exact_go_callee_value[
                                      source_operation->result_value]
                            ? verify_direct_local_go_callee_type_authority(
                                  ctx, source_operation, function,
                                  value->args[a]) &&
                                  operand->type == source_operation->result_type
                        : source_operation &&
                                  source_operation->result_value <
                                      ctx->value_count &&
                                  ctx->exact_source_namespace_value &&
                                  ctx->exact_source_namespace_value[
                                      source_operation->result_value]
                            ? verify_source_namespace_type_authority(
                                  ctx, source_operation, value->args[a]) &&
                                  operand->type == source_operation->result_type
                        : source_operation &&
                                  semantic_string_literal_is_exact(
                                      ctx->semantic, source_operation)
                            ? verify_string_literal_type_authority(
                                  ctx, source_operation, value->args[a]) &&
                                  operand->type == source_operation->result_type
                        : source_operation &&
                                  source_operation->result_value <
                                      ctx->value_count &&
                                  ctx->exact_channel_value[
                                      source_operation->result_value]
                            ? verify_channel_type_authority(
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
                                    operation->result_value < ctx->value_count &&
                                    ctx->exact_direct_callee_value[
                                        operation->result_value]
                              ? verify_direct_local_callee_type_authority(
                                    ctx, operation, function, value)
                          : operation &&
                                    operation->result_value < ctx->value_count &&
                                    ctx->exact_go_callee_value[
                                        operation->result_value]
                              ? verify_direct_local_go_callee_type_authority(
                                    ctx, operation, function, value)
                          : operation &&
                                    operation->result_value < ctx->value_count &&
                                    ctx->exact_source_namespace_value &&
                                    ctx->exact_source_namespace_value[
                                        operation->result_value]
                              ? verify_source_namespace_type_authority(
                                    ctx, operation, value)
                          : operation &&
                                    semantic_string_literal_is_exact(
                                        ctx->semantic, operation)
                              ? verify_string_literal_type_authority(
                                    ctx, operation, value)
                          : operation &&
                                    operation->result_value < ctx->value_count &&
                                    ctx->exact_channel_value[
                                        operation->result_value]
                              ? verify_channel_type_authority(
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
        case XR_MACHINE_REP_ENUM_ORDINAL:
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

static bool oracle_dynamic_direct_local_string_result_storage(
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
        !aot_direct_local_string_result_is_exact(ctx->semantic, operation_index))
        return false;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep)
                : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep)
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

/* The JSON namespace call materializes an owned dynamic value in its own
 * temporary slot.  Its storage is tagged because the runtime encoder returns a
 * boxed value; the call row proves the slot, layout, and ownership. */
static bool oracle_dynamic_json_namespace_value_storage(const VerifyAuthority *ctx,
                                                        uint32_t semantic_value,
                                                        XrRep *out_storage,
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
        call->callee_function != XR_SEMANTIC_INDEX_NONE ||
        call->result_value != semantic_value || call->result_slot != binding->slot ||
        call->argument_count != 0 || call->flags != 0 ||
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
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_dynamic_stringbuilder_storage(
    const VerifyAuthority *ctx, uint32_t semantic_value,
    XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage ||
        !out_machine_kind)
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index < ctx->operation_count
            ? xr_semantic_plan_operation(ctx->semantic, operation_index)
            : NULL;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep = binding
        ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep)
        : NULL;
    const XrTargetMachineRepRecord *memory_rep = binding
        ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep)
        : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots =
        xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls =
        xr_target_plan_calls(ctx->target_plan, &call_count);
    const XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (call)
            return false;
        call = &calls[i];
    }
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts =
        xr_target_plan_layouts(ctx->target_plan, &layout_count);
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
    xr_stable_id_hex(operation ? operation->id : (XrStableId) {{0}},
                     first_hex);
    xr_stable_id_hex(operation ? operation->allocation_id :
                                 (XrStableId) {{0}}, second_hex);
    int written = snprintf(
        key, sizeof(key),
        "xray-target-stringbuilder-constructor-v1:first=%s:second=%s:ordinal=0",
        first_hex, second_hex);
    if (!operation || !binding || !register_rep || !memory_rep || !slot ||
        !layout || !call ||
        !aot_stringbuilder_constructor_is_exact(ctx->semantic, operation) ||
        written <= 0 || (size_t) written >= sizeof(key) ||
        !xr_stable_id_from_key(key, &expected_call, &digest) ||
        !xr_stable_id_equal(call->identity, expected_call) ||
        call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        call->caller_function != operation->function ||
        call->callee_function != XR_SEMANTIC_INDEX_NONE ||
        call->result_value != semantic_value || call->result_slot != binding->slot ||
        call->argument_count != 0 || call->flags != 0 ||
        call->result_ownership != XR_TARGET_CALL_RETURN_OWNED ||
        call->calling_convention !=
            XR_TARGET_CALL_CONVENTION_STRINGBUILDER_CONSTRUCTOR ||
        call->target_kind != XR_TARGET_CALL_TARGET_STRINGBUILDER_CONSTRUCTOR ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index ||
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_static_direct_local_callee_storage(
    const VerifyAuthority *ctx, uint32_t semantic_value,
    XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage ||
        !out_machine_kind || !ctx->exact_direct_callee_value ||
        !ctx->exact_direct_callee_value[semantic_value])
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index == XR_SEMANTIC_INDEX_NONE
            ? NULL
            : xr_semantic_plan_operation(ctx->semantic, operation_index);
    uint32_t target_function =
        ctx->direct_callee_target_by_value[semantic_value];
    if (!operation || operation->result_value != semantic_value ||
        !aot_direct_local_callee_type_is_exact(
            ctx->semantic, operation, target_function))
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
        register_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        register_rep->memory_size != memory_rep->memory_size ||
        register_rep->memory_align != memory_rep->memory_align ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC ||
        layout->field_count != 0 || layout->root_field_count != 0 ||
        layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align ||
        slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index ||
        slot->function != operation->function ||
        slot->role != XR_TARGET_SLOT_TEMPORARY ||
        slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep ||
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_BORROWED)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_source_namespace_storage(
    const VerifyAuthority *ctx, uint32_t semantic_value,
    XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage ||
        !out_machine_kind || !ctx->exact_source_namespace_value ||
        !ctx->exact_source_namespace_value[semantic_value])
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index < ctx->operation_count
            ? xr_semantic_plan_operation(ctx->semantic, operation_index) : NULL;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(ctx->target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep = binding
        ? xr_target_plan_machine_rep(ctx->target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep = binding
        ? xr_target_plan_machine_rep(ctx->target_plan, binding->memory_rep) : NULL;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots =
        xr_target_plan_slots(ctx->target_plan, &slot_count);
    const XrTargetSlotRecord *slot = binding && binding->slot < slot_count
        ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts =
        xr_target_plan_layouts(ctx->target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (operation && layouts[i].semantic_type == operation->result_type) {
            if (layout)
                return false;
            layout = &layouts[i];
        }
    }
    if (!operation || !binding || !register_rep || !memory_rep || !slot ||
        !layout || !aot_source_namespace_value_operation_is_exact(
                       ctx->semantic, operation) ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
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
        slot->ownership != XR_TARGET_OWNERSHIP_BORROWED)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_static_direct_local_go_callee_storage(
    const VerifyAuthority *ctx, uint32_t semantic_value,
    XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage ||
        !out_machine_kind || !ctx->exact_go_callee_value ||
        !ctx->exact_go_callee_value[semantic_value])
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
        if (operation && layouts[i].semantic_type == operation->result_type) {
            if (layout)
                return false;
            layout = &layouts[i];
        }
    }
    if (!operation || !binding || !register_rep || !memory_rep || !slot ||
        !layout || !aot_direct_local_callee_type_is_exact(
                       ctx->semantic, operation, target) ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC || layout->field_count != 0 ||
        layout->root_field_count != 0 ||
        slot->semantic_value != semantic_value ||
        slot->semantic_operation != operation_index ||
        slot->function != operation->function ||
        slot->role != XR_TARGET_SLOT_TEMPORARY ||
        slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep ||
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_BORROWED)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_dynamic_channel_storage(
    const VerifyAuthority *ctx, uint32_t semantic_value,
    XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage ||
        !out_machine_kind || !ctx->exact_channel_value ||
        !ctx->exact_channel_value[semantic_value])
        return false;
    uint32_t operation_index = ctx->operation_by_value[semantic_value];
    const XrSemanticOperationRecord *operation =
        operation_index == XR_SEMANTIC_INDEX_NONE
            ? NULL
            : xr_semantic_plan_operation(ctx->semantic, operation_index);
    if (!operation || operation->result_value != semantic_value)
        return false;
    bool allocation = ctx->exact_channel_allocation_value &&
                      ctx->exact_channel_allocation_value[semantic_value];
    uint8_t expected_ownership = allocation ? XR_TARGET_OWNERSHIP_OWNED
                                            : XR_TARGET_OWNERSHIP_BORROWED;
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
        register_rep->ownership != expected_ownership ||
        memory_rep->ownership != expected_ownership ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
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
        slot->ownership != expected_ownership)
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool oracle_direct_local_callee_use(
    const VerifyAuthority *ctx, uint32_t operation_index,
    uint16_t operand_index, uint32_t source_value) {
    if (!ctx || source_value >= ctx->value_count || operand_index != 0 ||
        !ctx->exact_direct_callee_value[source_value])
        return false;
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(ctx->semantic, operation_index);
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls =
        xr_target_plan_calls(ctx->target_plan, &call_count);
    const XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (match)
            return false;
        match = &calls[i];
    }
    return operation &&
           (operation->opcode == XI_CALL ||
            operation->opcode == XI_TAIL_CALL) &&
           match && match->semantic_call_target != XR_SEMANTIC_INDEX_NONE &&
           match->caller_function == operation->function &&
           match->callee_function ==
               ctx->direct_callee_target_by_value[source_value] &&
           match->calling_convention ==
               XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL &&
           match->target_kind == XR_TARGET_CALL_TARGET_DIRECT_LOCAL;
}

static bool oracle_direct_local_go_callee_use(
    const VerifyAuthority *ctx, uint32_t operation_index,
    uint16_t operand_index, uint32_t source_value) {
    const XrSemanticOperationRecord *operation =
        ctx ? xr_semantic_plan_operation(ctx->semantic, operation_index) : NULL;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        ctx ? xr_semantic_plan_operands(ctx->semantic, &operand_count) : NULL;
    return ctx && operation && operand_index == 0 &&
           source_value < ctx->value_count && ctx->exact_go_callee_value &&
           ctx->exact_go_callee_value[source_value] &&
           operation->opcode == XI_GO && operation->operand_count != 0 &&
           operation->operand_begin < operand_count &&
           operands[operation->operand_begin].value == source_value &&
           ctx->go_callee_target_by_value[source_value] !=
               XR_SEMANTIC_INDEX_NONE;
}

/* Rebuild the channel receive boundary from frozen SemanticPlan and
 * TargetPlan facts.  The runtime payload is tagged; the target row owns the
 * exact scalar machine destination.  This is intentionally independent of
 * both the TargetPlan collector and the C-emission recipe verifier. */
static bool oracle_channel_receive_storage(
    const VerifyAuthority *ctx, uint32_t semantic_value,
    XrRep *out_storage, uint16_t *out_machine_kind) {
    if (!ctx || semantic_value >= ctx->value_count || !out_storage ||
        !out_machine_kind || !ctx->exact_channel_value)
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
        operation->result_value != semantic_value ||
        operation->operand_count != 1 ||
        operation->operand_begin >= operand_count || operation->allocation_key ||
        !aot_stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != 0 || operation->semantic_immediate != 0 ||
        operation->effects != xi_generated_op_effects(XI_CHAN_TRY_RECV) ||
        operation->flags != xi_generated_op_default_flags(XI_CHAN_TRY_RECV) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CHAN_TRY_RECV) ||
        operation->result_ownership !=
            xi_generated_op_result_ownership(XI_CHAN_TRY_RECV) ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_parameter != -1 || operation->return_complete != 1)
        return false;
    const XrSemanticOperandRecord *receiver =
        &operands[operation->operand_begin];
    uint32_t element_type = XR_SEMANTIC_INDEX_NONE;
    if (receiver->value >= ctx->value_count ||
        !ctx->exact_channel_value[receiver->value] ||
        receiver->role != XR_SEM_OPERAND_VALUE || receiver->parameter != -1 ||
        receiver->transfer_mode != XR_TRANSFER_SHARE ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        receiver->parameter_mode != XR_PARAM_READ ||
        receiver->access != XR_CALL_ARG_PLAIN ||
        receiver->origin != XI_PLACE_ORIGIN_NONE ||
        receiver->lifetime != XI_PLACE_LIFETIME_NONE ||
        receiver->escape != XI_PLACE_ESCAPE_NONE || receiver->flags != 0 ||
        !aot_channel_type_is_exact(ctx->semantic, receiver->type,
                                   &element_type) ||
        element_type != operation->result_type)
        return false;
    XrRep native_storage = XR_REP_TAGGED;
    uint16_t machine_kind = XR_MACHINE_REP_COUNT;
    if (!oracle_machine_storage(ctx, semantic_value, &native_storage,
                                &machine_kind) ||
        native_storage == XR_REP_TAGGED ||
        !storage_matches_machine(native_storage, machine_kind))
        return false;
    *out_storage = XR_REP_TAGGED;
    *out_machine_kind = machine_kind;
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
        case XR_MACHINE_REP_ENUM_ORDINAL:
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
    if (ctx->exact_channel_value &&
        ctx->exact_channel_value[semantic_value])
        return oracle_dynamic_channel_storage(ctx, semantic_value,
                                              out_storage,
                                              out_machine_kind);
    if (ctx->exact_source_namespace_value &&
        ctx->exact_source_namespace_value[semantic_value])
        return oracle_source_namespace_storage(
            ctx, semantic_value, out_storage, out_machine_kind);
    if (oracle_dynamic_json_namespace_value_storage(ctx, semantic_value, out_storage,
                                                    out_machine_kind))
        return true;
    if (operation->opcode == XI_COPY && operation->operand_count == 1 &&
        operation->semantic_immediate == XI_COPY_KIND_IDENTITY) {
        uint32_t operand_count = 0;
        const XrSemanticOperandRecord *operands =
            xr_semantic_plan_operands(ctx->semantic, &operand_count);
        if (operation->operand_begin >= operand_count ||
            operands[operation->operand_begin].value >= semantic_value)
            return false;
        return oracle_definition_storage(
            ctx, operands[operation->operand_begin].value, out_storage,
            out_machine_kind);
    }
    switch (operation->opcode) {
        case XI_CLOSURE_NEW:
            return oracle_dynamic_closure_storage(ctx, semantic_value,
                                                  out_storage,
                                                  out_machine_kind);
        case XI_CHAN_TRY_RECV:
            if (oracle_channel_receive_storage(
                    ctx, semantic_value, out_storage, out_machine_kind))
                return true;
            /* Non-scalar Recv<T> envelopes remain tagged. A scalar result is
             * owned exclusively by CHANNEL_RECEIVE_STORAGE and may not fall
             * back when its exact authority is missing. */
            {
                const XrTargetValueRepRecord *binding =
                    xr_target_plan_value_rep(ctx->target_plan,
                                             semantic_value);
                const XrTargetMachineRepRecord *machine = binding
                    ? xr_target_plan_machine_rep(ctx->target_plan,
                                                 binding->register_rep)
                    : NULL;
                if (machine && machine->kind >= XR_MACHINE_REP_I1 &&
                    machine->kind <= XR_MACHINE_REP_RUNE)
                    return false;
            }
            *out_storage = XR_REP_TAGGED;
            *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
            return true;
        case XI_CALL_BUILTIN:
            if (aot_stringbuilder_constructor_is_exact(ctx->semantic,
                                                       operation))
                return oracle_dynamic_stringbuilder_storage(
                    ctx, semantic_value, out_storage, out_machine_kind);
            return false;
        case XI_GET_BUILTIN:
            if (!aot_json_namespace_global_is_exact(ctx->semantic, operation))
                return false;
            *out_storage = XR_REP_TAGGED;
            *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
            return true;
        case XI_CHAN_RECV:
        case XI_ENUM_DESCRIPTOR_BOX:
            *out_storage = XR_REP_TAGGED;
            *out_machine_kind = XR_MACHINE_REP_DYN_VALUE;
            return true;
        case XI_GET_SHARED: {
            if (ctx->exact_direct_callee_value &&
                ctx->exact_direct_callee_value[semantic_value])
                return oracle_static_direct_local_callee_storage(
                    ctx, semantic_value, out_storage, out_machine_kind);
            if (ctx->exact_go_callee_value &&
                ctx->exact_go_callee_value[semantic_value])
                return oracle_static_direct_local_go_callee_storage(
                    ctx, semantic_value, out_storage, out_machine_kind);
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
            /* An owned String result is the one non-scalar direct-local shape
             * with an exact target row; everything else keeps the scalar
             * machine storage proof. */
            if (oracle_dynamic_direct_local_string_result_storage(
                    ctx, semantic_value, out_storage, out_machine_kind))
                return true;
            return oracle_machine_storage(ctx, semantic_value, out_storage,
                                          out_machine_kind);
        case XI_CHAN_RECV_STATUS:
        case XI_CHAN_IS_CLOSED:
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
        case XI_RETAIN:
        case XI_RELEASE: {
            XrRep literal_storage = XR_REP_TAGGED;
            if (!oracle_dynamic_string_literal_storage(
                    ctx, source_value, &literal_storage, &ignored_kind) &&
                !oracle_dynamic_direct_local_string_result_storage(
                    ctx, source_value, &literal_storage, &ignored_kind) &&
                !oracle_dynamic_stringbuilder_storage(
                    ctx, source_value, &literal_storage, &ignored_kind) &&
                !oracle_dynamic_json_namespace_value_storage(
                    ctx, source_value, &literal_storage, &ignored_kind) &&
                !(ctx->exact_channel_allocation_value &&
                  ctx->exact_channel_allocation_value[source_value] &&
                  oracle_dynamic_channel_storage(
                      ctx, source_value, &literal_storage, &ignored_kind)) &&
                !(ctx->exact_source_namespace_value &&
                  ctx->exact_source_namespace_value[source_value] &&
                  oracle_source_namespace_storage(
                      ctx, source_value, &literal_storage, &ignored_kind)))
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
                    ctx, source_value, &machine_storage, &ignored_kind) &&
                !oracle_dynamic_direct_local_string_result_storage(
                    ctx, source_value, &machine_storage, &ignored_kind) &&
                !oracle_dynamic_stringbuilder_storage(
                    ctx, source_value, &machine_storage, &ignored_kind) &&
                !oracle_dynamic_channel_storage(
                    ctx, source_value, &machine_storage, &ignored_kind) &&
                !oracle_dynamic_json_namespace_value_storage(
                    ctx, source_value, &machine_storage, &ignored_kind) &&
                !oracle_source_namespace_storage(
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
        case XI_GO:
            if (operand_index == 0) {
                if (!oracle_direct_local_go_callee_use(
                        ctx, operation_index, operand_index, source_value))
                    return false;
                *out_storage = XR_REP_TAGGED;
                return true;
            }
            return oracle_machine_storage(ctx, source_value, out_storage,
                                          &ignored_kind);
        case XI_CHAN_NEW:
            if (operand_index != 0 || !ctx->exact_channel_allocation_value ||
                !ctx->exact_channel_allocation_value[operation->result_value])
                return false;
            return oracle_machine_storage(ctx, source_value, out_storage,
                                          &ignored_kind);
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
            if (operand_index != 0) return false;
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
            return oracle_machine_storage(ctx, operation->result_value,
                                          out_storage, &ignored_kind);
        case XI_CALL:
            if (operand_index == 0 || !ctx->policy->prefer_call_args_native) {
                if (operand_index == 0 &&
                    ctx->exact_direct_callee_value[source_value] &&
                    !oracle_direct_local_callee_use(
                        ctx, operation_index, operand_index, source_value))
                    return false;
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

static bool authority_collect_obligations_indexed(
    CollectContext *ctx, const VerifyAuthority *oracle) {
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
            if (!oracle_definition_storage(oracle, source_value,
                                           &input_storage,
                                           &ignored_machine) ||
                !oracle_use_storage(oracle, i, a, source_value,
                                    &output_storage)) {
                set_diag(ctx->diag,
                         XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                         ctx->record_count, source_value, i);
                return false;
            }
            if (!authority_add_obligation(
                    ctx, oracle, source_value, i, operation->block, a,
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
        if (operation &&
            (operation->opcode == XI_ERR_RETURN ||
             (oracle->exact_source_namespace_value &&
              oracle->exact_source_namespace_value[block->control_value])))
            continue;
        XrRep input_storage = XR_REP_TAGGED;
        XrRep output_storage = XR_REP_TAGGED;
        uint16_t machine_kind = XR_MACHINE_REP_COUNT;
        if (!oracle_definition_storage(oracle, block->control_value,
                                       &input_storage, &machine_kind)) {
            set_diag(ctx->diag,
                     XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                     ctx->record_count, block->control_value,
                     XR_SEMANTIC_INDEX_NONE);
            return false;
        }
        if (!ctx->policy->force_return_tagged &&
            !oracle_machine_storage(oracle, block->control_value,
                                    &output_storage, &machine_kind)) {
            /* A returned owned String is tagged storage. Each oracle re-proves
             * its own exact TargetPlan row, so no unproven reference return
             * reaches this path. */
            if (!oracle_dynamic_closure_storage(
                    oracle, block->control_value, &output_storage,
                    &machine_kind) &&
                !oracle_dynamic_string_literal_storage(
                    oracle, block->control_value, &output_storage,
                    &machine_kind) &&
                !oracle_dynamic_direct_local_string_result_storage(
                    oracle, block->control_value, &output_storage,
                    &machine_kind)) {
                set_diag(ctx->diag,
                         XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
                         ctx->record_count, block->control_value,
                         XR_SEMANTIC_INDEX_NONE);
                return false;
            }
        }
        if (!authority_add_obligation(
                ctx, oracle, block->control_value,
                XR_SEMANTIC_INDEX_NONE, i, 0,
                XR_AOT_REP_USE_BLOCK_CONTROL, input_storage,
                output_storage))
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
        .function_count =
            (uint32_t) xr_semantic_plan_function_count(ctx->semantic),
        .block_count =
            (uint32_t) xr_semantic_plan_block_count(ctx->semantic),
        .value_count = ctx->semantic_value_count,
        .operation_count =
            (uint32_t) xr_semantic_plan_operation_count(ctx->semantic),
        .operation_by_value = ctx->operation_by_value,
        .parameter_by_value = ctx->parameter_by_value,
    };
    bool valid = aot_index_direct_local_callee_values(&oracle) &&
                 aot_index_direct_local_go_callee_values(&oracle) &&
                 aot_index_source_namespace_values(&oracle) &&
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
    return valid;
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

/* Representation refresh mechanically propagates and DCEs identity COPY
 * nodes.  Resolve an exact frozen namespace COPY to the nearest retained
 * materialized value so the verifier checks the published graph rather than
 * requiring an optimizer-dead Xi node to survive. */
static const XiValue *materialized_source_namespace_value(
    const VerifyAuthority *ctx, uint32_t semantic_value) {
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
            operation->operand_count != 1 ||
            operation->operand_begin >= operand_count)
            return NULL;
        semantic_value = operands[operation->operand_begin].value;
        if (semantic_value >= ctx->value_count)
            return NULL;
    }
    return NULL;
}

static bool index_materialized_callee_authority(
    VerifyAuthority *ctx, const XiFunc *function,
    const XiFunc **function_by_index, uint32_t depth) {
    if (!ctx || !function || !function_by_index ||
        depth > XR_AOT_REP_VERIFY_MAX_FUNCTION_DEPTH ||
        !verify_charge_work(ctx, 1) ||
        function->semantic_plan != ctx->semantic ||
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
            if (semantic_value >= ctx->value_count ||
                ctx->live_by_value[semantic_value])
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
            if (semantic_value >= ctx->value_count ||
                ctx->live_by_value[semantic_value])
                return false;
            ctx->live_by_value[semantic_value] = value;
        }
    }
    for (uint16_t i = 0; i < function->nchildren; i++)
        if (!index_materialized_callee_authority(
                ctx, function->children[i], function_by_index, depth + 1u))
            return false;
    return true;
}

static bool verify_exact_dynamic_storage_materialization(
    const XrAotRefinementPlanView *view, const XiFunc *root,
    const XrTargetPlan *target_plan, const XiRepPolicy *policy,
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
        valid = verify_alloc(&ctx, ctx.function_count,
                             sizeof(*function_by_index),
                             (void **) &function_by_index);
    if (valid)
        valid = index_materialized_callee_authority(
            &ctx, root, function_by_index, 0);
    for (uint32_t value = 0; valid && value < ctx.value_count; value++) {
        bool direct = ctx.exact_direct_callee_value[value] != 0;
        bool go = ctx.exact_go_callee_value[value] != 0;
        bool source_namespace = ctx.exact_source_namespace_value &&
                                ctx.exact_source_namespace_value[value] != 0;
        if (!direct && !go && !source_namespace)
            continue;
        uint32_t operation_index = ctx.operation_by_value[value];
        const XrSemanticOperationRecord *operation =
            operation_index == XR_SEMANTIC_INDEX_NONE
                ? NULL
                : xr_semantic_plan_operation(ctx.semantic, operation_index);
        const XiFunc *owner =
            operation && operation->function < ctx.function_count
                ? function_by_index[operation->function]
                : NULL;
        const XrSemanticFunctionRecord *semantic_function =
            operation ? xr_semantic_plan_function(ctx.semantic,
                                                   operation->function)
                      : NULL;
        const XiValue *live = ctx.live_by_value[value];
        bool elided_source_copy = source_namespace && operation &&
                                  operation->opcode == XI_COPY && !live;
        const XiValue *effective = elided_source_copy
            ? materialized_source_namespace_value(&ctx, value) : live;
        bool shape_ok = operation && semantic_function && owner && effective &&
                        value >= semantic_function->value_begin &&
                        (elided_source_copy ||
                         materialized_operation_shape_matches(
                             operation, live,
                             value - semantic_function->value_begin));
        bool authority_ok = shape_ok && (elided_source_copy
            ? source_type_matches(
                  effective->type,
                  xr_semantic_plan_type(ctx.semantic, operation->result_type))
            :
            (direct
                 ? verify_direct_local_callee_type_authority(
                       &ctx, operation, owner, live)
             : go
                 ? verify_direct_local_go_callee_type_authority(
                       &ctx, operation, owner, live)
                 : verify_source_namespace_type_authority(
                       &ctx, operation, live)));
        if (!operation || !semantic_function || !owner || !effective ||
            value < semantic_function->value_begin ||
            !shape_ok ||
            !authority_ok) {
            set_diag(diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY,
                     operation_index, value, operation_index);
            valid = false;
        }
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx.semantic, &operand_count);
    for (uint32_t i = 0; valid && i < ctx.operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(ctx.semantic, i);
        const XrSemanticFunctionRecord *semantic_function =
            operation ? xr_semantic_plan_function(ctx.semantic,
                                                   operation->function)
                      : NULL;
        const XiValue *user =
            operation && operation->result_value < ctx.value_count
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
            uint32_t source_value =
                operands[operation->operand_begin + a].value;
            if (source_value >= ctx.value_count ||
                (!ctx.exact_direct_callee_value[source_value] &&
                 !ctx.exact_go_callee_value[source_value] &&
                 !(ctx.exact_source_namespace_value &&
                   ctx.exact_source_namespace_value[source_value])))
                continue;
            if (elided_source_copy)
                continue;
            uint32_t local_value =
                operation->result_value - semantic_function->value_begin;
            const XiValue *materialized_source =
                materialized_source_namespace_value(&ctx, source_value);
            if (!user || !materialized_operation_shape_matches(
                             operation, user, local_value) ||
                !user->args || a >= user->nargs ||
                user->args[a] != (materialized_source
                                      ? materialized_source
                                      : ctx.live_by_value[source_value]) ||
                !(ctx.exact_direct_callee_value[source_value]
                      ? oracle_direct_local_callee_use(
                            &ctx, i, a, source_value)
                  : ctx.exact_go_callee_value[source_value]
                      ? oracle_direct_local_go_callee_use(
                            &ctx, i, a, source_value)
                      : materialized_source &&
                            user->args[a] == materialized_source)) {
                set_diag(diag, XR_AOT_REFINEMENT_USE_SITE, i,
                         source_value, i);
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
    if (!verify_exact_dynamic_storage_materialization(
            view, root, target_plan, effective_policy, diag))
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
        if (operation &&
            (operation->opcode == XI_ERR_RETURN ||
             (ctx->exact_source_namespace_value &&
              ctx->exact_source_namespace_value[block->control_value])))
            continue;
        XrRep output_storage = XR_REP_TAGGED;
        if (!ctx->policy->force_return_tagged) {
            uint16_t machine_kind = 0;
            if (!oracle_machine_storage(ctx, block->control_value,
                                        &output_storage, &machine_kind) &&
                !oracle_dynamic_closure_storage(
                    ctx, block->control_value, &output_storage,
                    &machine_kind) &&
                !oracle_dynamic_string_literal_storage(
                    ctx, block->control_value, &output_storage,
                    &machine_kind) &&
                !oracle_dynamic_direct_local_string_result_storage(
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
