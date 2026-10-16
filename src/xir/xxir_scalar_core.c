/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_scalar_core.c - Shared numeric and full arena-aware CFG execution
 *
 * KEY CONCEPT:
 *   No Source, Instance, native selection or external callback executes here.
 */
#include "xxir_scalar_core.h"
#include "xxir_float.h"
#include "xxir_math.h"
#include "xxir_types.h"
static XrXirRunStatus core_value_status(XrXirValueStatus status) {
    if (status == XR_XIR_VALUE_OK) return XR_XIR_RUN_OK;
    if (status == XR_XIR_VALUE_OOM) return XR_XIR_RUN_OUT_OF_MEMORY;
    if (status == XR_XIR_VALUE_UNSUPPORTED) return XR_XIR_RUN_UNSUPPORTED;
    if (status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT)
        return XR_XIR_RUN_FRAME_LIMIT;
    return XR_XIR_RUN_BAD_ARTIFACT;
}
static int arithmetic_operation(XrXirOp op) {
    switch (op) {
    case XR_XIR_ADD_INT: return XR_XIR_ARITH_ADD;
    case XR_XIR_SUB_INT: return XR_XIR_ARITH_SUB;
    case XR_XIR_MUL_INT: return XR_XIR_ARITH_MUL;
    case XR_XIR_DIV_INT: return XR_XIR_ARITH_DIV;
    case XR_XIR_REM_INT: return XR_XIR_ARITH_REM;
    case XR_XIR_AND_INT: return XR_XIR_ARITH_AND;
    case XR_XIR_OR_INT: return XR_XIR_ARITH_OR;
    case XR_XIR_XOR_INT: return XR_XIR_ARITH_XOR;
    case XR_XIR_SHL_INT: return XR_XIR_ARITH_SHL;
    case XR_XIR_SHR_INT: return XR_XIR_ARITH_SHR;
    default: return -1;
    }
}

XR_FUNC XrXirRunStatus xir_scalar_core_equal(XrXirType type,int64_t left,int64_t right,bool *output) {
    if (!output) return XR_XIR_RUN_BAD_ARGUMENT;
    bool equal=false;
    if (type==XR_XIR_BOOL) {
        if ((left!=0 && left!=1) || (right!=0 && right!=1)) return XR_XIR_RUN_BAD_ARGUMENT;
        equal=left==right;
    } else if (type==XR_XIR_RUNE) {
        if (!xr_xir_rune_payload_valid(left) || !xr_xir_rune_payload_valid(right)) return XR_XIR_RUN_BAD_ARGUMENT;
        equal=left==right;
    } else {
        if (!xr_xir_type_is_integer(type)) return XR_XIR_RUN_BAD_ARGUMENT;
        int ordering=0;
        XrXirRunStatus status=xr_xir_integer_compare(xr_xir_integer_format(type),left,right,&ordering);
        if (status!=XR_XIR_RUN_OK) return status;
        equal=ordering==0;
    }
    *output=equal;return XR_XIR_RUN_OK;
}

XR_FUNC XrXirRunStatus xir_scalar_core_edge(XirScalarCoreRun *run, uint32_t instruction, uint32_t target) {
    const XrXirFunction *function = run->function;
    uint32_t low = 0, high = function->block_count;
    while (low + 1 < high) {
        if (!xir_scalar_core_work(run, 1)) return XR_XIR_RUN_STEP_LIMIT;
        uint32_t middle = low + (high - low) / 2;
        if (function->blocks[middle].first <= instruction) low = middle;
        else high = middle;
    }
    uint32_t first = function->blocks[target].first, end = first;
    while (end < function->instruction_count && function->instructions[end].op == XR_XIR_PHI) {
        const XrXirInstruction *phi = &function->instructions[end];
        uint32_t source = UINT32_MAX;
        for (uint32_t a = 0; a < phi->args[1]; a += 2) {
            if (!xir_scalar_core_work(run, 1)) return XR_XIR_RUN_STEP_LIMIT;
            if (function->operands[phi->args[0] + a] == low) {
                if (!xir_scalar_core_work(run, 1)) return XR_XIR_RUN_STEP_LIMIT;
                source = function->operands[phi->args[0] + a + 1]; break;
            }
        }
        if (source == UINT32_MAX) return XR_XIR_RUN_BAD_ARTIFACT;
        if (!xir_scalar_core_work(run, 2)) return XR_XIR_RUN_STEP_LIMIT;
        uint32_t scratch = run->layout->offsets[function->parameter_count + end] + 8;
        int64_t value = xr_xir_scalar_load(run->frame, run->layout->offsets[source]);
        if (xr_xir_type_is_owned(run->module->types, phi->type)) {
            XrXirValueStatus status = xr_xir_owned_slot_copy(run->frame, scratch,
                run->arena, phi->type, value);
            if (status != XR_XIR_VALUE_OK) return core_value_status(status);
        } else xr_xir_scalar_store(run->frame, scratch, value);
        ++end;
    }
    for (uint32_t i = first; i < end; ++i) {
        if (!xir_scalar_core_work(run, 3)) return XR_XIR_RUN_STEP_LIMIT;
        const XrXirInstruction *phi = &function->instructions[i];
        uint32_t destination = run->layout->offsets[function->parameter_count + i];
        XrXirValue value = {(uint32_t) phi->type, 0, xr_xir_scalar_load(run->frame, destination + 8)};
        xr_xir_scalar_store(run->frame, destination + 8, 0);
        if (xr_xir_type_is_owned(run->module->types, phi->type)) xr_xir_owned_slot_move(run->frame, destination, &value);
        else xr_xir_scalar_store(run->frame, destination, value.payload);
    }
    return XR_XIR_RUN_OK;
}

static XrXirRunStatus core_integer_step(XirScalarCoreRun *run, uint32_t *cursor) {
    uint32_t instruction = *cursor;
    const XrXirInstruction *op = &run->function->instructions[instruction];
    uint32_t result_id = run->function->parameter_count + instruction;
    uint32_t next = instruction + 1;
    int64_t value = 0;
    switch (op->op) {
    case XR_XIR_ADD_INT: case XR_XIR_SUB_INT: case XR_XIR_MUL_INT:
    case XR_XIR_AND_INT: case XR_XIR_OR_INT: case XR_XIR_XOR_INT:
    case XR_XIR_SHL_INT: case XR_XIR_SHR_INT:
    case XR_XIR_DIV_INT: case XR_XIR_REM_INT: {
        int64_t left = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]);
        int64_t right = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]]);
        XrXirRunStatus status = xr_xir_integer_arithmetic(xr_xir_integer_format(op->type), (XrXirArithmetic) arithmetic_operation(op->op), left, right, &value);
        if (status != XR_XIR_RUN_OK) return status;
        break;
    }
    case XR_XIR_EQ_INT: case XR_XIR_NE_INT: case XR_XIR_LT_INT:
    case XR_XIR_LE_INT: case XR_XIR_GT_INT: case XR_XIR_GE_INT: {
        int ordering = 0;
        XrXirType type = xr_xir_operand_type(run->function, op->args[0]);
        XrXirRunStatus status = xr_xir_integer_compare(xr_xir_integer_format(type),
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]),
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]]), &ordering);
        if (status != XR_XIR_RUN_OK) return status;
        switch (op->op) {
        case XR_XIR_EQ_INT: value = ordering == 0; break;
        case XR_XIR_NE_INT: value = ordering != 0; break;
        case XR_XIR_LT_INT: value = ordering < 0; break;
        case XR_XIR_LE_INT: value = ordering <= 0; break;
        case XR_XIR_GT_INT: value = ordering > 0; break;
        default: value = ordering >= 0; break;
        }
        break;
    }
    case XR_XIR_RUNE_TO_INTEGER: case XR_XIR_INTEGER_TO_RUNE: {
        XrXirType from=xr_xir_operand_type(run->function,op->args[0]);
        XrXirRunStatus status=xr_xir_rune_convert(from,op->type,
            xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]]),&value);
        if (status!=XR_XIR_RUN_OK) return status;
        break;
    }
    case XR_XIR_CONVERT_NUMBER: {
        XrXirType from = xr_xir_operand_type(run->function, op->args[0]);
        XrXirRunStatus status = xr_xir_number_convert(from, op->type,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]), &value);
        if (status != XR_XIR_RUN_OK) return status;
        break;
    }
    default: return XR_XIR_RUN_BAD_ARTIFACT;
    }
    xr_xir_scalar_store(run->frame, run->layout->offsets[result_id], value);
    *cursor = next;
    return XR_XIR_RUN_OK;
}

XR_FUNC XrXirRunStatus xir_scalar_core_step(XirScalarCoreRun *run,uint32_t *cursor,XirScalarCoreAction *action) {
    if (!run || !cursor || !action || !run->module || !run->function || !run->layout || (!run->frame && run->layout->frame_bytes) ||
        *cursor>=run->function->instruction_count) return XR_XIR_RUN_BAD_ARTIFACT;
    uint32_t instruction=*cursor;
    const XrXirInstruction *op=&run->function->instructions[instruction];
    uint32_t result_id=run->function->parameter_count+instruction,next=instruction+1;
    action->kind=XIR_SCALAR_CONTINUE;
    if (arithmetic_operation(op->op)>=0 || (op->op==XR_XIR_EQ_INT || op->op==XR_XIR_NE_INT || op->op==XR_XIR_LT_INT ||
        op->op==XR_XIR_LE_INT || op->op==XR_XIR_GT_INT || op->op==XR_XIR_GE_INT) ||
        op->op==XR_XIR_CONVERT_NUMBER || op->op==XR_XIR_RUNE_TO_INTEGER || op->op==XR_XIR_INTEGER_TO_RUNE)
        return core_integer_step(run,cursor);
    if (op->op==XR_XIR_JUMP || op->op==XR_XIR_BRANCH) {
        uint32_t edge=op->op==XR_XIR_BRANCH && !xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]]) ? 1u:0u;
        XrXirRunStatus status=xir_scalar_core_edge(run,instruction,op->targets[edge]);
        *cursor=run->function->blocks[op->targets[edge]].first;return status;
    }
    if (op->op==XR_XIR_PHI) { *cursor=next;return XR_XIR_RUN_OK; }
    int64_t value=0;
    switch (op->op) {
    case XR_XIR_CONST_RUNE:case XR_XIR_CONST_BOOL:case XR_XIR_CONST_FLOAT:case XR_XIR_CONST_INT:value=op->immediate;break;
    case XR_XIR_MATH_UNARY_F64: case XR_XIR_MATH_BINARY_F64: case XR_XIR_MATH_UNARY_I64: {
        int64_t left=xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]]);
        int64_t right=op->op==XR_XIR_MATH_BINARY_F64 ?
            xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[1]]) : 0;
        XrXirRunStatus status=xr_xir_math_evaluate((uint32_t)op->immediate,left,right,&value);
        if (status!=XR_XIR_RUN_OK) return status;
        break;
    }
    case XR_XIR_EQUAL: {
        XrXirType type=xr_xir_operand_type(run->function,op->args[0]);
        if (op->type!=XR_XIR_BOOL || xr_xir_operand_type(run->function,op->args[1])!=type ||
            (op->immediate!=0 && op->immediate!=1)) return XR_XIR_RUN_BAD_ARTIFACT;
        bool equal=false;
        XrXirRunStatus status=xir_scalar_core_equal(type,
            xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]]),
            xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[1]]),&equal);
        if (status!=XR_XIR_RUN_OK) return status;
        value=op->immediate ? !equal : equal;break;
    }
    case XR_XIR_LOCAL_UNINIT:
        if (xr_xir_type_is_owned(run->module->types,op->type))
            xr_xir_owned_slot_clear(run->frame,run->layout->offsets[result_id]);
        *cursor=next;return XR_XIR_RUN_OK;
    case XR_XIR_SCALAR_LOCAL_NEW:case XR_XIR_SCALAR_LOCAL_READ:case XR_XIR_SCALAR_COPY:
        value=xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]]);break;
    case XR_XIR_SCALAR_LOCAL_WRITE:
        xr_xir_scalar_store(run->frame,run->layout->offsets[op->args[0]],
            xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[1]]));break;
    case XR_XIR_OWNED_LOCAL_WRITE: {
        XrXirType type = run->function->instructions[op->args[0] - run->function->parameter_count].type;
        XrXirValueStatus status = xr_xir_owned_slot_copy(run->frame, run->layout->offsets[op->args[0]], run->arena, type,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]]));
        *cursor = next; return core_value_status(status);
    }
    case XR_XIR_RETURN:
        action->kind=XIR_SCALAR_RETURN;action->value=(XrXirValue){(uint32_t)run->function->result,0,0};
        if (run->function->result!=XR_XIR_UNIT)
            action->value.payload=xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]]);
        return XR_XIR_RUN_OK;
    default:return XR_XIR_RUN_BAD_ARTIFACT;
    }
    if (op->type!=XR_XIR_UNIT) xr_xir_scalar_store(run->frame,run->layout->offsets[result_id],value);
    *cursor=next;return XR_XIR_RUN_OK;
}
