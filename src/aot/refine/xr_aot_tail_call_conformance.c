/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_aot_tail_call_conformance.c - Exact native tail-call conformance gate
 */

#include "xr_aot_tail_call_conformance.h"
#include "xr_aot_scalar_value.h"
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include "../../ir/xi.h"
#include "../../plan/target/xr_target_verify.h"
#include <limits.h>
#include <string.h>

typedef struct TailVerifyContext {
    const XrSemanticPlan *semantic;
    const XrTargetPlan *target;
    const XrAotRefinementPlanView *authority;
    XiFunc **live_by_function;
    uint32_t function_count;
    uint32_t indexed_function_count;
    XrAotTailCallDiagnostic *diag;
} TailVerifyContext;

static bool fail(TailVerifyContext *ctx, uint32_t issue,
                 uint32_t operation, uint32_t call, uint32_t function,
                 uint32_t value) {
    if (ctx && ctx->diag) {
        ctx->diag->issue = issue;
        ctx->diag->semantic_operation = operation;
        ctx->diag->target_call_index = call;
        ctx->diag->semantic_function = function;
        ctx->diag->semantic_value = value;
    }
    return false;
}

static void hash_u32(XrSHA256Context *ctx, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t) (value >> 24), (uint8_t) (value >> 16),
        (uint8_t) (value >> 8), (uint8_t) value,
    };
    xr_sha256_update(ctx, bytes, sizeof(bytes));
}

static bool index_function_tree(TailVerifyContext *ctx, XiFunc *function,
                                uint32_t parent) {
    if (!ctx || !function || ctx->indexed_function_count >= ctx->function_count ||
        function->semantic_plan != ctx->semantic ||
        function->semantic_plan_function_index >= ctx->function_count)
        return false;
    uint32_t index = function->semantic_plan_function_index;
    const XrSemanticFunctionRecord *frozen =
        xr_semantic_plan_function(ctx->semantic, index);
    if (!frozen || ctx->live_by_function[index] || frozen->parent != parent ||
        frozen->child_count != function->nchildren)
        return false;
    ctx->live_by_function[index] = function;
    ctx->indexed_function_count++;
    for (uint16_t i = 0; i < function->nchildren; i++) {
        if (!function->children || !function->children[i] ||
            function->children[i]->parent_func != function ||
            !index_function_tree(ctx, function->children[i], index))
            return false;
    }
    return true;
}

static XiValue *find_live_operation_value(TailVerifyContext *ctx,
                                          uint32_t operation_index,
                                          const XrSemanticOperationRecord *operation) {
    const XrSemanticFunctionRecord *frozen = operation
        ? xr_semantic_plan_function(ctx->semantic, operation->function)
        : NULL;
    XiFunc *function = operation && operation->function < ctx->function_count
        ? ctx->live_by_function[operation->function]
        : NULL;
    if (!frozen || !function || operation->result_value < frozen->value_begin ||
        operation->result_value - frozen->value_begin >= frozen->value_count)
        return NULL;
    uint32_t local_value = operation->result_value - frozen->value_begin;
    XiValue *match = NULL;
    uint32_t matches = 0;
    for (uint32_t b = 0; b < function->nblocks; b++) {
        XiBlock *block = function->blocks ? function->blocks[b] : NULL;
        if (!block || block->func != function)
            continue;
        for (uint32_t i = 0; i < block->nvalues; i++) {
            XiValue *value = block->values ? block->values[i] : NULL;
            if (!value || value->id != local_value)
                continue;
            match = value;
            matches++;
        }
    }
    if (matches != 1 || !match || !match->block ||
        match->block->func != function || match->block->id >= frozen->block_count ||
        frozen->block_begin > UINT32_MAX - match->block->id ||
        operation->block != frozen->block_begin + match->block->id)
        return NULL;
    const XrSemanticBlockRecord *block =
        xr_semantic_plan_block(ctx->semantic, operation->block);
    if (!block || block->function != operation->function ||
        block->control_value != operation->result_value ||
        match->block->kind != XI_BLOCK_RETURN || match->block->control != match)
        return NULL;
    (void) operation_index;
    return match;
}

static const XrTargetCallRecord *find_target_call(
    TailVerifyContext *ctx, uint32_t operation, uint32_t *out_index) {
    uint32_t count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(ctx->target, &count);
    const XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; i < count; i++) {
        if (calls[i].semantic_operation != operation)
            continue;
        if (match)
            return NULL;
        match = &calls[i];
        if (out_index)
            *out_index = i;
    }
    return match;
}

static const XrAotTransformationRecord *find_authority_record(
    const XrAotRefinementPlanView *view, uint32_t call_index) {
    const XrAotTransformationRecord *match = NULL;
    if (!view || !view->records)
        return NULL;
    for (uint32_t i = 0; i < view->record_count; i++) {
        const XrAotTransformationRecord *record = &view->records[i];
        if (record->transform_kind != XR_AOT_TRANSFORM_DIRECT_CALL ||
            record->direct_call.target_call_index != call_index)
            continue;
        if (match)
            return NULL;
        match = record;
    }
    return match;
}

static const XiFunc *live_callee_target(const XiFunc *caller,
                                        const XiValue *callee) {
    uint8_t depth = 0;
    while (callee && depth++ < 16) {
        if ((callee->op == XI_COPY || callee->op == XI_SOURCE_MOVE ||
             callee->op == XI_OWNER_FORWARD || callee->op == XI_BOX ||
             callee->op == XI_UNBOX) &&
            callee->nargs == 1 && callee->args && callee->args[0]) {
            callee = callee->args[0];
            continue;
        }
        if ((callee->op == XI_CLOSURE_NEW ||
             (callee->op == XI_STACK_ALLOC && callee->aux_int == XI_CLOSURE_NEW)) &&
            callee->aux)
            return (const XiFunc *) callee->aux;
        if (callee->op == XI_GET_SHARED && callee->aux_int >= 0 &&
            callee->aux_int <= UINT16_MAX) {
            uint16_t slot = (uint16_t) callee->aux_int;
            for (const XiFunc *owner = caller; owner; owner = owner->parent_func) {
                if (owner->shared_slot_funcs &&
                    slot < owner->shared_slot_func_count &&
                    owner->shared_slot_funcs[slot])
                    return owner->shared_slot_funcs[slot];
            }
        }
        return NULL;
    }
    return NULL;
}

static bool verify_tail_operation(TailVerifyContext *ctx,
                                  uint32_t operation_index,
                                  const XrSemanticOperationRecord *operation,
                                  XrSHA256Context *hash) {
    XiValue *live = find_live_operation_value(ctx, operation_index, operation);
    uint32_t call_index = XR_SEMANTIC_INDEX_NONE;
    const XrTargetCallRecord *call = find_target_call(ctx, operation_index, &call_index);
    if (!live)
        return fail(ctx, XR_AOT_TAIL_CALL_CONFORMANCE_SOURCE_IDENTITY,
                    operation_index, call_index, operation->function,
                    operation->result_value);
    if (live->op != XI_TAIL_CALL)
        return fail(ctx, XR_AOT_TAIL_CALL_CONFORMANCE_LIVE_OPCODE,
                    operation_index, call_index, operation->function,
                    operation->result_value);
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    if (!xr_aot_scalar_semantic_value_id(
            ctx->target, live->block->func, live, &semantic_function,
            &semantic_value, NULL, 0) ||
        semantic_function != operation->function ||
        semantic_value != operation->result_value ||
        live->nargs != operation->operand_count || !live->args ||
        live->aux_kind != operation->auxiliary_kind ||
        live->aux_int != operation->semantic_immediate ||
        live->flags != operation->flags ||
        live->transfer_mode != operation->transfer_mode ||
        live->param_mode != operation->parameter_mode ||
        live->xg_callsite_id != operation->evidence[0] ||
        live->xa_intrinsic_id != operation->evidence[1] ||
        live->xg_method_id != operation->evidence[2])
        return fail(ctx, XR_AOT_TAIL_CALL_CONFORMANCE_SOURCE_IDENTITY,
                    operation_index, call_index, operation->function,
                    operation->result_value);

    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(ctx->semantic, &operand_count);
    if (!operands || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin)
        return fail(ctx, XR_AOT_TAIL_CALL_CONFORMANCE_PLAN_STATE,
                    operation_index, call_index, operation->function,
                    operation->result_value);
    for (uint16_t i = 0; i < live->nargs; i++) {
        uint32_t arg_function = XR_SEMANTIC_INDEX_NONE;
        uint32_t arg_value = XR_SEMANTIC_INDEX_NONE;
        XiValue *argument = live->args[i];
        if (!argument || !argument->block || !argument->block->func ||
            !xr_aot_scalar_semantic_value_id(
                ctx->target, argument->block->func, argument, &arg_function,
                &arg_value, NULL, 0) ||
            arg_value != operands[operation->operand_begin + i].value)
            return fail(ctx, XR_AOT_TAIL_CALL_CONFORMANCE_OPERAND_MAPPING,
                        operation_index, call_index, operation->function,
                        operation->result_value);
    }

    if (!call || call->id != call_index ||
        call->caller_function != operation->function ||
        call->result_value != operation->result_value ||
        call->target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
        (call->flags & XR_TARGET_CALL_TAIL) == 0 ||
        call->callee_function >= ctx->function_count)
        return fail(ctx, XR_AOT_TAIL_CALL_CONFORMANCE_CALL_AUTHORITY,
                    operation_index, call_index, operation->function,
                    operation->result_value);
    const XrAotTransformationRecord *record =
        find_authority_record(ctx->authority, call_index);
    const XrSemanticFunctionRecord *callee =
        xr_semantic_plan_function(ctx->semantic, call->callee_function);
    if (!record || !callee ||
        record->decision != XR_AOT_REFINEMENT_APPLIED ||
        record->diagnostic_issue != XR_AOT_REFINEMENT_OK ||
        record->direct_call_binding.target_call_index != call_index ||
        record->direct_call_binding.semantic_operation != operation_index ||
        record->direct_call_binding.caller_function != operation->function ||
        record->direct_call_binding.callee_function != call->callee_function ||
        !xr_stable_id_equal(record->direct_call_binding.operation_id,
                            operation->id) ||
        !xr_stable_id_equal(record->direct_call_binding.callee_identity,
                            callee->id))
        return fail(ctx, XR_AOT_TAIL_CALL_CONFORMANCE_DIRECT_CALL_RECORD,
                    operation_index, call_index, operation->function,
                    operation->result_value);
    if (live_callee_target(live->block->func, live->args[0]) !=
        ctx->live_by_function[call->callee_function])
        return fail(ctx, XR_AOT_TAIL_CALL_CONFORMANCE_CALLEE_MAPPING,
                    operation_index, call_index, operation->function,
                    operation->result_value);

    uint32_t argument_count = 0;
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(ctx->target, &argument_count);
    if ((call->argument_count != 0 && !arguments) ||
        call->argument_count + 1u != operation->operand_count ||
        call->argument_begin > argument_count ||
        call->argument_count > argument_count - call->argument_begin)
        return fail(ctx, XR_AOT_TAIL_CALL_CONFORMANCE_CALL_AUTHORITY,
                    operation_index, call_index, operation->function,
                    operation->result_value);
    for (uint16_t i = 0; i < call->argument_count; i++) {
        const XrTargetCallArgumentRecord *argument =
            &arguments[call->argument_begin + i];
        const XrSemanticOperandRecord *operand =
            &operands[operation->operand_begin + i + 1u];
        if (argument->call != call->id || argument->ordinal != i ||
            argument->semantic_operand != operation->operand_begin + i + 1u ||
            argument->semantic_value != operand->value)
            return fail(ctx, XR_AOT_TAIL_CALL_CONFORMANCE_CALL_AUTHORITY,
                        operation_index, call_index, operation->function,
                        operation->result_value);
    }

    hash_u32(hash, operation_index);
    xr_sha256_update(hash, operation->id.bytes, sizeof(operation->id.bytes));
    xr_sha256_update(hash, call->identity.bytes, sizeof(call->identity.bytes));
    xr_sha256_update(hash, call->fingerprint.bytes,
                     sizeof(call->fingerprint.bytes));
    xr_sha256_update(hash, record->direct_call_binding.fingerprint.bytes,
                     sizeof(record->direct_call_binding.fingerprint.bytes));
    return true;
}

const char *xr_aot_tail_call_conformance_issue_name(uint32_t issue) {
    switch ((XrAotTailCallConformanceIssue) issue) {
        case XR_AOT_TAIL_CALL_CONFORMANCE_OK: return "XR_AOT_TAIL_CALL_CONFORMANCE_OK";
        case XR_AOT_TAIL_CALL_CONFORMANCE_INVALID_ARGUMENT:
            return "XR_AOT_TAIL_CALL_CONFORMANCE_INVALID_ARGUMENT";
        case XR_AOT_TAIL_CALL_CONFORMANCE_PLAN_STATE:
            return "XR_AOT_TAIL_CALL_CONFORMANCE_PLAN_STATE";
        case XR_AOT_TAIL_CALL_CONFORMANCE_SOURCE_IDENTITY:
            return "XR_AOT_TAIL_CALL_CONFORMANCE_SOURCE_IDENTITY";
        case XR_AOT_TAIL_CALL_CONFORMANCE_LIVE_OPCODE:
            return "XR_AOT_TAIL_CALL_CONFORMANCE_LIVE_OPCODE";
        case XR_AOT_TAIL_CALL_CONFORMANCE_OPERAND_MAPPING:
            return "XR_AOT_TAIL_CALL_CONFORMANCE_OPERAND_MAPPING";
        case XR_AOT_TAIL_CALL_CONFORMANCE_CALL_AUTHORITY:
            return "XR_AOT_TAIL_CALL_CONFORMANCE_CALL_AUTHORITY";
        case XR_AOT_TAIL_CALL_CONFORMANCE_DIRECT_CALL_RECORD:
            return "XR_AOT_TAIL_CALL_CONFORMANCE_DIRECT_CALL_RECORD";
        case XR_AOT_TAIL_CALL_CONFORMANCE_CALLEE_MAPPING:
            return "XR_AOT_TAIL_CALL_CONFORMANCE_CALLEE_MAPPING";
        case XR_AOT_TAIL_CALL_CONFORMANCE_INCOMPLETE_COVERAGE:
            return "XR_AOT_TAIL_CALL_CONFORMANCE_INCOMPLETE_COVERAGE";
        case XR_AOT_TAIL_CALL_CONFORMANCE_RESOURCE_BUDGET:
            return "XR_AOT_TAIL_CALL_CONFORMANCE_RESOURCE_BUDGET";
    }
    return "XR_AOT_TAIL_CALL_CONFORMANCE_UNKNOWN";
}

bool xr_aot_tail_call_conformance_verify(
    const XiFunc *root, const XrTargetPlan *target_plan,
    const XrAotRefinementPlanView *direct_call_authority,
    XrAotTailCallConformance *out_conformance,
    XrAotTailCallDiagnostic *diag) {
    if (out_conformance)
        memset(out_conformance, 0, sizeof(*out_conformance));
    if (diag)
        memset(diag, 0, sizeof(*diag));
    TailVerifyContext ctx = {.target = target_plan,
                             .authority = direct_call_authority,
                             .diag = diag};
    char error[256] = {0};
    if (!root || !target_plan || !direct_call_authority || !out_conformance)
        return fail(&ctx, XR_AOT_TAIL_CALL_CONFORMANCE_INVALID_ARGUMENT,
                    0, 0, 0, 0);
    ctx.semantic = xr_target_plan_semantic_plan(target_plan);
    if (!ctx.semantic || root->semantic_plan != ctx.semantic ||
        !xr_target_plan_is_verified(target_plan) ||
        !xr_target_plan_fingerprint_is_intact(target_plan) ||
        !xr_target_plan_verify(target_plan, error, sizeof(error)) ||
        !direct_call_authority->frozen || !direct_call_authority->verified ||
        !xr_aot_refinement_verify(direct_call_authority, target_plan, NULL))
        return fail(&ctx, XR_AOT_TAIL_CALL_CONFORMANCE_PLAN_STATE,
                    0, 0, 0, 0);
    size_t function_count = xr_semantic_plan_function_count(ctx.semantic);
    if (function_count == 0 || function_count > XR_AOT_REFINEMENT_MAX_RECORDS)
        return fail(&ctx, XR_AOT_TAIL_CALL_CONFORMANCE_RESOURCE_BUDGET,
                    0, 0, 0, 0);
    ctx.function_count = (uint32_t) function_count;
    ctx.live_by_function = (XiFunc **) xr_calloc(
        ctx.function_count, sizeof(*ctx.live_by_function));
    if (!ctx.live_by_function)
        return fail(&ctx, XR_AOT_TAIL_CALL_CONFORMANCE_RESOURCE_BUDGET,
                    0, 0, 0, 0);
    bool indexed = index_function_tree(
        &ctx, (XiFunc *) root, XR_SEMANTIC_INDEX_NONE);
    if (!indexed || ctx.indexed_function_count != ctx.function_count) {
        xr_free(ctx.live_by_function);
        return fail(&ctx, XR_AOT_TAIL_CALL_CONFORMANCE_SOURCE_IDENTITY,
                    0, 0, 0, 0);
    }

    XrSHA256Context hash;
    xr_sha256_init(&hash);
    static const uint8_t domain[] = "xray-aot-tail-call-conformance-v1";
    xr_sha256_update(&hash, domain, sizeof(domain));
    out_conformance->semantic_fingerprint =
        xr_semantic_plan_fingerprint(ctx.semantic);
    out_conformance->target_plan_fingerprint =
        xr_target_plan_fingerprint(target_plan);
    out_conformance->direct_call_authority_fingerprint =
        direct_call_authority->fingerprint;
    xr_sha256_update(&hash, out_conformance->semantic_fingerprint.bytes,
                     sizeof(out_conformance->semantic_fingerprint.bytes));
    xr_sha256_update(&hash, out_conformance->target_plan_fingerprint.bytes,
                     sizeof(out_conformance->target_plan_fingerprint.bytes));
    xr_sha256_update(&hash,
                     out_conformance->direct_call_authority_fingerprint.bytes,
                     sizeof(out_conformance->direct_call_authority_fingerprint.bytes));

    size_t operation_count = xr_semantic_plan_operation_count(ctx.semantic);
    if (operation_count > XR_AOT_REFINEMENT_MAX_RECORDS) {
        xr_free(ctx.live_by_function);
        return fail(&ctx, XR_AOT_TAIL_CALL_CONFORMANCE_RESOURCE_BUDGET,
                    0, 0, 0, 0);
    }
    for (uint32_t i = 0; i < (uint32_t) operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(ctx.semantic, i);
        if (!operation || operation->opcode != XI_TAIL_CALL)
            continue;
        if (!verify_tail_operation(&ctx, i, operation, &hash)) {
            xr_free(ctx.live_by_function);
            return false;
        }
        out_conformance->tail_call_count++;
    }
    uint32_t live_tail_count = 0;
    for (uint32_t f = 0; f < ctx.function_count; f++) {
        XiFunc *function = ctx.live_by_function[f];
        for (uint32_t b = 0; function && b < function->nblocks; b++) {
            XiBlock *block = function->blocks ? function->blocks[b] : NULL;
            for (uint32_t i = 0; block && i < block->nvalues; i++)
                live_tail_count += block->values && block->values[i] &&
                                   block->values[i]->op == XI_TAIL_CALL;
        }
    }
    xr_free(ctx.live_by_function);
    if (live_tail_count != out_conformance->tail_call_count)
        return fail(&ctx, XR_AOT_TAIL_CALL_CONFORMANCE_INCOMPLETE_COVERAGE,
                    0, 0, 0, 0);
    hash_u32(&hash, out_conformance->tail_call_count);
    xr_sha256_final(&hash, out_conformance->fingerprint.bytes);
    if (diag)
        diag->issue = XR_AOT_TAIL_CALL_CONFORMANCE_OK;
    return true;
}
