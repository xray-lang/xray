/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_float.inc.c - Deterministic compiler IEEE bit operations
 *
 * KEY CONCEPT:
 *   Existing integer-only numeric routines define results independently of
 *   the host FPU. Numeric range and division failures keep their original status.
 */
#include "xxir_float.h"
static bool ctfe_float_opcode(const ScalarRun *run,const XrXirInstruction *op) {
    if (op->op==XR_XIR_CONST_FLOAT || op->op==XR_XIR_NEG_FLOAT ||
        (op->op>=XR_XIR_EQ_FLOAT && op->op<=XR_XIR_GE_FLOAT) ||
        (op->op>=XR_XIR_ADD_FLOAT && op->op<=XR_XIR_DIV_FLOAT)) return true;
    if (op->op==XR_XIR_EQUAL) return xr_xir_float_bits(xr_xir_operand_type(run->function,op->args[0]))!=0;
    return op->op==XR_XIR_CONVERT_NUMBER && (xr_xir_float_bits(op->type) ||
        xr_xir_float_bits(xr_xir_operand_type(run->function,op->args[0])));
}
static XrXirCtfeStatus ctfe_float_step(ScalarRun *run,uint32_t *cursor) {
    const XrXirInstruction *op=&run->function->instructions[*cursor];int64_t result=0;
    /* Decode/pack, two normalizations, product carry scans and long division
     * are bounded by 64-bit formats. Reserve before entering any numeric API. */
    XrXirRunStatus status=XR_XIR_RUN_OK;
    if (op->op==XR_XIR_CONST_FLOAT) result=op->immediate;
    else {
        XrXirType type=xr_xir_operand_type(run->function,op->args[0]);
        int64_t left=xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]]);
        if (op->op==XR_XIR_CONVERT_NUMBER) status=xr_xir_number_convert(type,op->type,left,&result);
        else if (op->op==XR_XIR_NEG_FLOAT) status=xr_xir_float_negative(type,left,&result);
        else {
            int64_t right=xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[1]]);
            if (op->op>=XR_XIR_ADD_FLOAT && op->op<=XR_XIR_DIV_FLOAT)
                status=xr_xir_float_binary(type,(XrXirFloatOperation)(op->op-XR_XIR_ADD_FLOAT),left,right,&result);
            else status=xr_xir_float_relation(type,op->op==XR_XIR_EQUAL?
                (op->immediate?XR_XIR_FLOAT_NE:XR_XIR_FLOAT_EQ):(XrXirFloatRelation)(op->op-XR_XIR_EQ_FLOAT),left,right,&result);
        }
    }
    if (status!=XR_XIR_RUN_OK) return status==XR_XIR_RUN_NUMERIC_RANGE?XR_XIR_CTFE_NUMERIC_RANGE:XR_XIR_CTFE_BAD_ARTIFACT;
    xr_xir_scalar_store(run->frame,run->layout->offsets[run->function->parameter_count+*cursor],result);
    ++*cursor;return XR_XIR_CTFE_OK;
}
