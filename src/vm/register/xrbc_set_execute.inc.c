/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_set_execute.inc.c - Canonical Set execution and current bindings
 *
 * KEY CONCEPT:
 *   VM operations call the same owned Set kernel used by native C.
 */
static bool xr_rbc_i64set_reference(XrRbcStack *stack,const XrRbcModule *module,uint64_t bits,uint32_t source,XrtSetRef *out) {
    if(!bits)return false;
    if((bits&3u)==2u)return false;
    if(bits&1u) {
        XrRbcRegister *cell=xr_rbc_cell(stack,module,bits>>1,XR_RBC_SET_I64);if(!cell)return false;
        *out=xrt_set_ref_local(&cell->set,source);
    } else {
        XrtModuleAccess *cache=(XrtModuleAccess *)(uintptr_t)bits;
        if(cache->kind!=XRT_MODULE_SET_I64 || !cache->is_mutable)return false;
        *out=(XrtSetRef){.state=*cache};
    }
    return true;
}
static XR_NOINLINE XrRbcStatus xr_rbc_i64set_step(const XrRbcModule *module,XrRbcStack *stack,
    const XrRbcFrame *frame,const XrRbcFunction *f,XrRbcArrayAccess a,uint32_t source) {
    const XrRbcArrayLocation *args=a.count?f->array_operands+a.first:NULL;
    XrRbcRegister *result=xr_rbc_array_location(stack,frame,a.result);
    if(a.operation==XR_RBC_ARRAY_NEW) {result->set=xrt_set_i64_new(NULL,0,source);return XR_RBC_OK;}
    XrRbcRegister *first=xr_rbc_array_location(stack,frame,args[0]);
    XrRbcRegister *second=a.count>1?xr_rbc_array_location(stack,frame,args[1]):NULL;
    if(a.operation==XR_RBC_SET_STATE_READ || a.operation==XR_RBC_SET_STATE_WRITE) {
        XrtModuleAccess *reference=first->state_ref;xr_rbc_state_ready(reference,source);
        if(a.operation==XR_RBC_SET_STATE_READ) {XrtSet *set;memcpy(&set,reference->payload,sizeof(set));result->set=xrt_set_retain(set,source);}
        else {XrXirValueStatus status=xrt_module_replace(reference->reference,&second->set);if(status!=XR_XIR_VALUE_OK)xrt_value_failure(status,source);}
        return XR_RBC_OK;
    }
    if(a.operation==XR_RBC_ARRAY_BORROW) {
        bool valid=rbc_array_location_kind(f,args[0])==XR_RBC_STATE_REF?
            xr_rbc_array_ref_encode_state(first->state_ref,&result->bits):xr_rbc_array_ref_encode_cell(first->bits,&result->bits);
        return valid?XR_RBC_OK:XR_RBC_INVALID;
    }
    bool reference=a.operation==XR_RBC_ARRAY_REF_READ || a.operation==XR_RBC_ARRAY_REF_WRITE ||
        a.operation==XR_RBC_SET_REF_ADD || a.operation==XR_RBC_SET_REF_REMOVE || a.operation==XR_RBC_SET_REF_CLEAR;
    if(reference) {
        XrtSetRef ref;if(!xr_rbc_i64set_reference(stack,module,first->bits,source,&ref))return XR_RBC_INVALID;
        if(a.operation==XR_RBC_ARRAY_REF_READ)result->set=xrt_set_ref_read(ref,source);
        else if(a.operation==XR_RBC_ARRAY_REF_WRITE)xrt_set_ref_write(ref,second->set,source);
        else if(a.operation==XR_RBC_SET_REF_ADD)xrt_set_ref_i64_add(ref,xr_rbc_signed(second->bits),source);
        else if(a.operation==XR_RBC_SET_REF_REMOVE)result->bits=xrt_set_ref_i64_remove(ref,xr_rbc_signed(second->bits),source);
        else xrt_set_ref_clear(ref,source);
        return XR_RBC_OK;
    }
    if(a.operation==XR_RBC_ARRAY_CELL_NEW) {
        XrRbcFrame *active=&stack->frames[stack->depth-1];size_t cell=active->cell_base+a.cell;
        stack->cells[cell].set=xrt_set_retain(first->set,source);
        if(active->initialized_cells<=a.cell)active->initialized_cells=a.cell+1;
        result->bits=(uint64_t)cell+1;return XR_RBC_OK;
    }
    bool cell=a.operation==XR_RBC_ARRAY_CELL_READ || a.operation==XR_RBC_ARRAY_CELL_WRITE || a.operation==XR_RBC_ARRAY_CELL_DROP ||
        a.operation==XR_RBC_SET_ADD || a.operation==XR_RBC_SET_REMOVE || a.operation==XR_RBC_SET_CLEAR;
    XrRbcRegister *binding=cell?xr_rbc_cell(stack,module,first->bits,XR_RBC_SET_I64):first;if(!binding)return XR_RBC_INVALID;
    switch(a.operation) {
    case XR_RBC_ARRAY_LEN:result->bits=(uint64_t)xrt_set_length(first->set,source);break;
    case XR_RBC_SET_CONTAINS:result->bits=xrt_set_i64_contains(first->set,xr_rbc_signed(second->bits),source);break;
    case XR_RBC_ARRAY_RETAIN:case XR_RBC_ARRAY_CELL_READ:result->set=xrt_set_retain(binding->set,source);break;
    case XR_RBC_ARRAY_TAKE:result->set=first->set;first->set=NULL;break;
    case XR_RBC_ARRAY_DROP:case XR_RBC_ARRAY_CELL_DROP:xrt_set_drop(&binding->set);if(cell)first->bits=0;break;
    case XR_RBC_ARRAY_CELL_WRITE:xrt_set_assign(&binding->set,second->set,source);break;
    case XR_RBC_SET_ADD:xrt_set_i64_add(&binding->set,xr_rbc_signed(second->bits),source);break;
    case XR_RBC_SET_REMOVE:result->bits=xrt_set_i64_remove(&binding->set,xr_rbc_signed(second->bits),source);break;
    case XR_RBC_SET_CLEAR:xrt_set_clear(&binding->set,source);break;
    default:return XR_RBC_INVALID;
    }
    return XR_RBC_OK;
}
