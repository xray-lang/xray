/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_string_read.inc.c - Bounded compiler String comparison and search
 *
 * KEY CONCEPT:
 *   Ordinary Value operations determine UTF-8 semantics. Worst-case byte visits,
 *   comparison spans and search-table construction pay the original ledger first.
 */
static bool ctfe_string_read_opcode(XrXirOp op) {
    switch (op) {
    case XR_XIR_LT_STRING: case XR_XIR_LE_STRING: case XR_XIR_GT_STRING: case XR_XIR_GE_STRING:
    case XR_XIR_CALL_INTRINSIC: case XR_XIR_STRING_STARTS_WITH: case XR_XIR_STRING_ENDS_WITH:
    case XR_XIR_STRING_INDEX_OF: case XR_XIR_STRING_LAST_INDEX_OF: return true;
    default: return false;
    }
}
static XrXirCtfeStatus ctfe_string_read_work(ScalarRun *run,XrXirOp op,
    const XrXirValue *left,const XrXirValue *right,int64_t start) {
    const char *a=NULL,*b=NULL;size_t ac=0,bc=0;
    if (!xr_xir_string_view(left,&a,&ac) || !xr_xir_string_view(right,&b,&bc)) return XR_XIR_CTFE_BAD_ARTIFACT;
    uint64_t bytes=0;
    if (op>=XR_XIR_LT_STRING && op<=XR_XIR_GE_STRING) bytes=ac<bc?ac:bc;
    else if (op==XR_XIR_STRING_STARTS_WITH || op==XR_XIR_STRING_ENDS_WITH) bytes=bc<=ac?bc:0;
    else {
        if (op==XR_XIR_STRING_INDEX_OF) {
            size_t runes=0;
            if (!xr_xir_string_runes(left,&runes)) return XR_XIR_CTFE_BAD_ARTIFACT;
            if (start<0 || (uint64_t)start>runes) return XR_XIR_CTFE_NUMERIC_RANGE;
            if (!bc || (uint64_t)start==runes) return XR_XIR_CTFE_OK;
        }
        if (!bc) return XR_XIR_CTFE_OK;
        /* Index conversion may scan UTF-8 twice. Search uses at most one
         * comparison and constant loop bookkeeping per candidate byte.
         * Long-pattern skip-table lookups also pay their complete slot span. */
        if (op!=XR_XIR_CALL_INTRINSIC) {
            if (ac>UINT64_MAX/2) return XR_XIR_CTFE_BUDGET;
            bytes=2*(uint64_t)ac;
        }
        if (bc<=ac) {
            uint64_t visits=(uint64_t)ac-bc+1;
            uint64_t overhead=2+(bc>8 && op!=XR_XIR_STRING_LAST_INDEX_OF?sizeof(size_t):0);
            if (bc>UINT64_MAX-overhead || visits>UINT64_MAX/((uint64_t)bc+overhead)) return XR_XIR_CTFE_BUDGET;
            uint64_t comparisons=visits*((uint64_t)bc+overhead);
            if (comparisons>UINT64_MAX-bytes) return XR_XIR_CTFE_BUDGET;
            bytes+=comparisons;
            if (op!=XR_XIR_STRING_LAST_INDEX_OF) {
                uint64_t setup=(uint64_t)ac;
                if (bc>8) {
                    if (bc>(UINT64_MAX-256*sizeof(size_t))/(sizeof(size_t)+1)) return XR_XIR_CTFE_BUDGET;
                    setup=256*sizeof(size_t)+(uint64_t)bc*(sizeof(size_t)+1);
                }
                if (setup>UINT64_MAX-bytes) return XR_XIR_CTFE_BUDGET;
                bytes+=setup;
            }
        }
    }
    return scalar_compile_work(run,bytes)?XR_XIR_CTFE_OK:XR_XIR_CTFE_BUDGET;
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
    XrXirCtfeStatus status=ctfe_string_read_work(run,op->op,&left,&right,start);
    if (status!=XR_XIR_CTFE_OK) return status;
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
        bool valid=call?xr_xir_string_contains(&left,&right,&found):
            op->op==XR_XIR_STRING_STARTS_WITH?xr_xir_string_starts_with(&left,&right,&found):
            xr_xir_string_ends_with(&left,&right,&found);
        if (!valid) return XR_XIR_CTFE_BAD_ARTIFACT;
        result=found;
    }
    if (status==XR_XIR_CTFE_OK) {
        xr_xir_scalar_store(run->frame,run->layout->offsets[run->function->parameter_count+*cursor],result);++*cursor;
    }
    return status;
}
