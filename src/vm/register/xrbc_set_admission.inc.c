/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_set_admission.inc.c - Exact Set operand and result admission
 *
 * KEY CONCEPT:
 *   Map, Array and raw scalar handles cannot enter the closed Set family.
 */
static bool rbc_i64set_access_valid(const XrRbcFunction *f,XrRbcArrayAccess a) {
    if(a.kind!=XR_RBC_SET_I64 || a.first>f->array_operand_count || a.count>f->array_operand_count-a.first)return false;
    uint32_t count=1;XrRbcRegisterKind result=XR_RBC_SET_I64,receiver=XR_RBC_SET_I64;bool output=true;
    switch(a.operation) {
    case XR_RBC_ARRAY_NEW:count=0;break;
    case XR_RBC_ARRAY_LEN:result=XR_RBC_BITS;break;
    case XR_RBC_SET_CONTAINS:count=2;result=XR_RBC_BITS;break;
    case XR_RBC_SET_ADD:count=2;receiver=XR_RBC_SET_I64_CELL;output=false;break;
    case XR_RBC_SET_REMOVE:count=2;receiver=XR_RBC_SET_I64_CELL;result=XR_RBC_BITS;break;
    case XR_RBC_SET_CLEAR:receiver=XR_RBC_SET_I64_CELL;output=false;break;
    case XR_RBC_ARRAY_RETAIN:case XR_RBC_ARRAY_TAKE:break;
    case XR_RBC_ARRAY_DROP:output=false;break;
    case XR_RBC_ARRAY_CELL_NEW:result=XR_RBC_SET_I64_CELL;break;
    case XR_RBC_ARRAY_CELL_READ:receiver=XR_RBC_SET_I64_CELL;break;
    case XR_RBC_ARRAY_CELL_WRITE:count=2;receiver=XR_RBC_SET_I64_CELL;output=false;break;
    case XR_RBC_ARRAY_CELL_DROP:receiver=XR_RBC_SET_I64_CELL;output=false;break;
    case XR_RBC_ARRAY_BORROW:result=XR_RBC_SET_I64_REF;break;
    case XR_RBC_ARRAY_REF_READ:receiver=XR_RBC_SET_I64_REF;break;
    case XR_RBC_ARRAY_REF_WRITE:count=2;receiver=XR_RBC_SET_I64_REF;output=false;break;
    case XR_RBC_SET_REF_ADD:count=2;receiver=XR_RBC_SET_I64_REF;output=false;break;
    case XR_RBC_SET_REF_REMOVE:count=2;receiver=XR_RBC_SET_I64_REF;result=XR_RBC_BITS;break;
    case XR_RBC_SET_REF_CLEAR:receiver=XR_RBC_SET_I64_REF;output=false;break;
    case XR_RBC_SET_STATE_READ:receiver=XR_RBC_STATE_REF;break;
    case XR_RBC_SET_STATE_WRITE:count=2;receiver=XR_RBC_STATE_REF;output=false;break;
    default:return false;
    }
    if(a.count!=count)return false;
    if(output) {
        if(!rbc_array_location_valid(f,a.result) || rbc_array_location_kind(f,a.result)!=result ||
            (result==XR_RBC_SET_I64_REF && a.result.spill))return false;
    } else if(a.result.index || a.result.spill)return false;
    if(a.operation==XR_RBC_ARRAY_CELL_NEW) {
        if(!f->cell_kinds || a.cell>=f->cell_count || f->cell_kinds[a.cell]!=XR_RBC_SET_I64)return false;
    } else if(a.cell)return false;
    for(uint32_t p=0;p<a.count;++p) {
        XrRbcArrayLocation location=f->array_operands[a.first+p];
        if(!rbc_array_location_valid(f,location))return false;
        XrRbcRegisterKind kind=rbc_array_location_kind(f,location),expected=receiver;
        if(p)expected=a.operation==XR_RBC_ARRAY_CELL_WRITE || a.operation==XR_RBC_ARRAY_REF_WRITE || a.operation==XR_RBC_SET_STATE_WRITE?XR_RBC_SET_I64:XR_RBC_BITS;
        else if(a.operation==XR_RBC_ARRAY_BORROW) {
            if(location.spill || (kind!=XR_RBC_SET_I64_CELL && kind!=XR_RBC_STATE_REF))return false;
            continue;
        }
        if(kind!=expected && !(a.operation==XR_RBC_ARRAY_TAKE && kind==XR_RBC_CALL_WINDOW))return false;
        if(a.operation==XR_RBC_ARRAY_TAKE && location.index==a.result.index && location.spill==a.result.spill)return false;
    }
    return true;
}
