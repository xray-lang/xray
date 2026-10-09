/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_slot_group.inc.c - Complete owned preparation and infallible publication
 */
XR_FUNC XrXirCallStatus xr_xir_instance_slot_group_init(XrXirCallView *view,uint32_t first,uint32_t count,
    const XrXirValue *compact,uint32_t payload_count) {
    XrXirInstance *instance=view_instance(view);XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if(!instance || view_is_child(instance,view) || !admission || !admission->domain || !count || (payload_count && !compact))return XR_XIR_CALL_BAD_STATE;
    const XrXirDeclarations *d=instance->program->declarations;
    if(first>d->slot_count || count>d->slot_count-first || instance->publication_count>d->slot_count ||
       count>d->slot_count-instance->publication_count)
        return XR_XIR_CALL_BAD_STATE;
    uint32_t function=xr_xir_call_current_entry(view->activation),module=d->functions[function].module;
    if(instance->current_module!=module || d->modules[module].initializer!=function)return XR_XIR_CALL_BAD_STATE;
    /* Validation + table initialization + prepare + transfer + mark + trace + worst-case cleanup are
     * prepaid. Commit and cleanup never discover an exhausted work budget. */
    uint64_t reserve=(uint64_t)count*7;
    if(reserve>admission->work)return XR_XIR_CALL_LIMIT;
    admission->work-=reserve;
    uint32_t payload=0;
    for(uint32_t i=0;i<count;++i) {
        const XrXirSlot *slot=&d->slots[first+i];
        if(slot->module!=module || instance->published[first+i] || !value_unit(instance->slots[first+i]))
            return XR_XIR_CALL_BAD_STATE;
        if(slot->type==XR_XIR_UNIT)continue;
        if(payload>=payload_count || !xr_xir_value_argument(&compact[payload++],instance->program->arena,slot->type))
            return XR_XIR_CALL_BAD_STATE;
    }
    if(payload!=payload_count)return XR_XIR_CALL_BAD_STATE;
    uint64_t bytes=(uint64_t)count*sizeof(XrXirValue);
    if(bytes>SIZE_MAX || bytes>admission->scratch_bytes)return XR_XIR_CALL_LIMIT;
    XrXirValueStatus value_status=XR_XIR_VALUE_OK;
    XrXirValue *owned=xr_xir_domain_allocate(admission->domain,(size_t)bytes,&value_status);
    if(!owned)return value_call_status(value_status);
    admission->scratch_bytes-=bytes;
    for(uint32_t i=0;i<count;++i)owned[i]=(XrXirValue){0};
    XrXirCallStatus status=XR_XIR_CALL_READY;payload=0;
    for(uint32_t i=0;i<count;++i) {
        XrXirType type=d->slots[first+i].type;
        if(type==XR_XIR_UNIT)continue;
        status=admit_instance_value(admission,&compact[payload],type);
        if(status==XR_XIR_CALL_READY)status=value_call_status(xr_xir_value_copy(&compact[payload],&owned[i]));
        if(status!=XR_XIR_CALL_READY)break;
        ++payload;
    }
    for(uint32_t i=0;status==XR_XIR_CALL_READY && i<count;++i) {
        if(!d->slots[first+i].mutable)continue;
        if(!instance_cell_work(view,1)){status=XR_XIR_CALL_LIMIT;break;}
        XrXirCellPublication publication={instance,instance->domain,first+i};
        if(xr_xir_cell_publication_prepare(&owned[i],&publication)!=XR_XIR_VALUE_OK) {
            status=XR_XIR_CALL_BAD_STATE;break;
        }
        for(uint32_t prior=0;prior<i;++prior) {
            if(!instance_cell_work(view,1)){status=XR_XIR_CALL_LIMIT;break;}
            if(d->slots[first+prior].mutable && xr_xir_cell_same_owner(&owned[prior],&owned[i])) {
                status=XR_XIR_CALL_BAD_STATE;break;
            }
        }
    }
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY) {
        for(uint32_t i=0;i<count;++i) {
            if(d->slots[first+i].mutable) {
                XrXirCellPublication publication={instance,instance->domain,first+i};
                xr_xir_cell_publication_commit(&owned[i],&publication);
            }
            instance->slots[first+i]=owned[i];owned[i]=(XrXirValue){0};
        }
        for(uint32_t i=0;i<count;++i) {
            instance->published[first+i]=1;
            instance->publication_order[instance->publication_count++]=first+i;
        }
        instance->published_counts[module]+=count;
        for(uint32_t i=0;i<count;++i)instance_trace(instance,XR_XIR_SLOT_PUBLISHED,first+i);
    }
    for(uint32_t i=0;i<count;++i)xr_xir_value_drop(&owned[i]);
    xr_xir_domain_deallocate(admission->domain,owned,(size_t)bytes);admission->scratch_bytes+=bytes;
    return status;
}
