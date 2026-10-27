/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_string_read.inc.c - Bounded compiler String comparison and search
 *
 * KEY CONCEPT:
 *   Ordinary Value operations determine UTF-8 comparison, Unicode indices
 *   and invalid-start behavior without compiler work prepayment.
 */
static bool ctfe_string_read_opcode(XrXirOp op) {
    switch (op) {
    case XR_XIR_LT_STRING: case XR_XIR_LE_STRING: case XR_XIR_GT_STRING: case XR_XIR_GE_STRING:
    case XR_XIR_CALL_INTRINSIC:
    case XR_XIR_STRING_INDEX_OF: case XR_XIR_STRING_LAST_INDEX_OF: return true;
    default: return false;
    }
}

static XrXirCtfeStatus ctfe_string_read_step(ScalarRun *run,uint32_t *cursor) {
    const XrXirInstruction *op=&run->function->instructions[*cursor];
    const XrNativeIntrinsicDeclaration *call = op->op == XR_XIR_CALL_INTRINSIC ?
        xr_xir_intrinsic(op->immediate) : NULL;
    if (op->op == XR_XIR_CALL_INTRINSIC && (!call || !call->ctfe)) return XR_XIR_CTFE_BAD_ARTIFACT;
    const uint32_t *args=op->op==XR_XIR_STRING_INDEX_OF || call ? run->function->operands+op->args[0]:op->args;
    XrXirValue left={XR_XIR_STRING,0,xr_xir_scalar_load(run->frame,run->layout->offsets[args[0]])};
    XrXirValue right={XR_XIR_STRING,0,xr_xir_scalar_load(run->frame,run->layout->offsets[args[1]])};
    int64_t start=op->op==XR_XIR_STRING_INDEX_OF?xr_xir_scalar_load(run->frame,run->layout->offsets[args[2]]):0;
    XrXirCtfeStatus status=XR_XIR_CTFE_OK;
    int64_t result=0;
    if (op->op==XR_XIR_STRING_INDEX_OF || op->op==XR_XIR_STRING_LAST_INDEX_OF) {
        XrXirValueStatus value=op->op==XR_XIR_STRING_INDEX_OF?
            xr_xir_string_index_of(&left,&right,start,&result):xr_xir_string_last_index_of(&left,&right,&result);
        status=value==XR_XIR_VALUE_BOUNDS?XR_XIR_CTFE_NUMERIC_RANGE:ctfe_value_status(value);
    } else if (op->op>=XR_XIR_LT_STRING && op->op<=XR_XIR_GE_STRING) {
        int order=0;
        if (!xr_xir_string_compare(&left,&right,&order)) return XR_XIR_CTFE_BAD_ARTIFACT;
        result=op->op==XR_XIR_LT_STRING?order<0:op->op==XR_XIR_LE_STRING?order<=0:op->op==XR_XIR_GT_STRING?order>0:order>=0;
    } else {
        bool found=false;
        if (!call) return XR_XIR_CTFE_BAD_ARTIFACT;
        bool valid=call->implementation==XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_CONTAINS?
            xr_xir_string_contains(&left,&right,&found):
            call->implementation==XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_STARTS_WITH?xr_xir_string_starts_with(&left,&right,&found):
            xr_xir_string_ends_with(&left,&right,&found);
        if (!valid) return XR_XIR_CTFE_BAD_ARTIFACT;
        result=found;
    }
    if (status==XR_XIR_CTFE_OK) {
        xr_xir_scalar_store(run->frame,run->layout->offsets[run->function->parameter_count+*cursor],result);++*cursor;
    }
    return status;
}
