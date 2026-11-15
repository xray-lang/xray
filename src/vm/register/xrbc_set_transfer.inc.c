/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_set_transfer.inc.c - Set ownership and synchronous reference transfer
 *
 * KEY CONCEPT:
 *   Control-flow owners and actual cell or state origins authorize each use.
 */
static bool rbc_i64set_transfer(RbcStringAdmission *s,XrRbcArrayAccess a) {
    const XrRbcArrayLocation *args=a.count?s->function->array_operands+a.first:NULL;
    uint8_t source=a.count?rbc_array_state(s,args[0]):RBC_EMPTY;
    if(a.operation==XR_RBC_SET_STATE_READ || a.operation==XR_RBC_SET_STATE_WRITE) {
        uint32_t origin=args[0].spill?s->spills[args[0].index].origin:s->origin[args[0].index];
        if(source!=RBC_STATE_REFERENCE || xr_rbc_state_kind(s->module,origin)!=XRT_MODULE_SET_I64)return false;
        if(a.operation==XR_RBC_SET_STATE_WRITE)return (origin&1u) && rbc_array_source(s,args[1]);
        if(!rbc_array_destination(s,a.result))return false;
        rbc_array_define(s,a.result,RBC_STRING_OWNED,0);return true;
    }
    bool reference=a.operation==XR_RBC_ARRAY_REF_READ || a.operation==XR_RBC_ARRAY_REF_WRITE ||
        a.operation==XR_RBC_SET_REF_ADD || a.operation==XR_RBC_SET_REF_REMOVE || a.operation==XR_RBC_SET_REF_CLEAR;
    if(a.operation==XR_RBC_ARRAY_BORROW || reference) {
        uint32_t origin=args[0].spill?s->spills[args[0].index].origin:s->origin[args[0].index];
        if(a.operation==XR_RBC_ARRAY_BORROW) {
            if(s->state[a.result.index]!=RBC_EMPTY && !rbc_array_ref_state(s->state[a.result.index]))return false;
            source=rbc_cell_state(source)?RBC_ARRAY_REF_CELL:source==RBC_STATE_REFERENCE?RBC_ARRAY_REF_STATE:RBC_EMPTY;
            if(rbc_array_ref_payload(s,source,origin)!=XR_RBC_SET_I64)return false;
            rbc_array_define(s,a.result,source,origin);return true;
        }
        if(!rbc_array_ref_state(source) || rbc_array_ref_payload(s,source,origin)!=XR_RBC_SET_I64)return false;
    } else if(a.operation==XR_RBC_ARRAY_NEW) {
        if(!rbc_array_destination(s,a.result))return false;
        rbc_array_define(s,a.result,RBC_STRING_OWNED,0);return true;
    } else {
        bool cell=a.operation==XR_RBC_ARRAY_CELL_READ || a.operation==XR_RBC_ARRAY_CELL_WRITE || a.operation==XR_RBC_ARRAY_CELL_DROP ||
            a.operation==XR_RBC_SET_ADD || a.operation==XR_RBC_SET_REMOVE || a.operation==XR_RBC_SET_CLEAR;
        if(cell?!rbc_cell_state(source):!rbc_string_state(source))return false;
    }
    if(a.operation==XR_RBC_ARRAY_CELL_NEW) {
        if(!rbc_array_destination(s,a.result))return false;
        for(uint32_t r=0;r<s->function->register_count;++r)if(rbc_cell_state(s->state[r]) && s->origin[r]==a.cell+1)return false;
        rbc_array_define(s,a.result,RBC_CELL_OWNED,a.cell+1);return true;
    }
    if(a.operation==XR_RBC_ARRAY_DROP || a.operation==XR_RBC_ARRAY_CELL_DROP || a.operation==XR_RBC_ARRAY_TAKE) {
        bool cell=a.operation==XR_RBC_ARRAY_CELL_DROP;
        if(source!=(cell?RBC_CELL_OWNED:RBC_STRING_OWNED))return false;
        if(a.operation==XR_RBC_ARRAY_TAKE) {
            if(!rbc_array_destination(s,a.result))return false;
            if(!args[0].spill && rbc_register_kind(s->function,args[0].index)==XR_RBC_CALL_WINDOW && s->origin[args[0].index]!=XR_RBC_SET_I64)return false;
            rbc_array_define(s,a.result,RBC_STRING_OWNED,0);
        }
        uint32_t origin=args[0].spill?0:s->origin[args[0].index];rbc_array_define(s,args[0],RBC_EMPTY,0);
        if(cell) {
            for(uint32_t r=0;r<s->function->register_count;++r)if((rbc_cell_state(s->state[r]) || s->state[r]==RBC_ARRAY_REF_CELL) && s->origin[r]==origin)s->state[r]=RBC_EMPTY;
        } else if(!args[0].spill)rbc_string_forget_borrows(s,args[0].index);
        return true;
    }
    if(a.operation==XR_RBC_ARRAY_CELL_WRITE || a.operation==XR_RBC_ARRAY_REF_WRITE)return rbc_array_source(s,args[1]);
    if(a.count>1 && rbc_array_state(s,args[1])!=RBC_BITS)return false;
    if(a.operation==XR_RBC_SET_ADD || a.operation==XR_RBC_SET_CLEAR || a.operation==XR_RBC_SET_REF_ADD || a.operation==XR_RBC_SET_REF_CLEAR)return true;
    if(!rbc_array_destination(s,a.result))return false;
    bool bits=a.operation==XR_RBC_ARRAY_LEN || a.operation==XR_RBC_SET_CONTAINS || a.operation==XR_RBC_SET_REMOVE || a.operation==XR_RBC_SET_REF_REMOVE;
    rbc_array_define(s,a.result,bits?RBC_BITS:RBC_STRING_OWNED,0);return true;
}
