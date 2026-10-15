/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug_write.inc.c - Prepared writes in actual paused VM callbacks
 */
XR_FUNC XrXirValueStatus xir_vm_debug_write_begin(XrXirVmDebug *debug,uint64_t epoch,uint64_t frame,
    uint32_t operand,const XrXirValue *value) {
    if (!debug_valid(debug) || !xr_xir_vm_debug_paused(debug) || debug->write.phase || !debug->snapshot ||
        !epoch || epoch!=debug->epoch || !frame || !xr_xir_value_valid(value)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(debug->domain,debug->snapshot->count+1)) return XR_XIR_VALUE_LIMIT;
    const SnapshotFrame *selected=NULL;
    for (uint32_t i=0;i<debug->snapshot->count;++i) if (debug->snapshot->frames[i].info.id==frame) selected=&debug->snapshot->frames[i];
    if (!selected || selected->info.thread!=debug->snapshot->stopped_thread || operand>=selected->info.value_count ||
        !(selected->known[operand]&2)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue owned={0};XrXirValueStatus status=xr_xir_value_copy(value,&owned);
    if (status!=XR_XIR_VALUE_OK) return status;
    debug->write=(DebugWrite){.value=owned,.epoch=epoch,.frame=frame,.operand=operand,.phase=1};return XR_XIR_VALUE_OK;
}
XR_FUNC bool xir_vm_debug_write_pending(const XrXirVmDebug *debug) { return debug_valid(debug) && debug->write.phase==1; }
XR_FUNC XrXirValueStatus xir_vm_debug_write_finish(XrXirVmDebug *debug,bool *visited,XrXirVmDebugSnapshot **output) {
    if (!debug_valid(debug) || !visited || !output || *output || !debug->write.phase) return XR_XIR_VALUE_BAD_ARGUMENT;
    *visited=debug->write.phase==2;
    XrXirValueStatus status=*visited ? debug->write.result : XR_XIR_VALUE_BAD_ARGUMENT;
    if (status==XR_XIR_VALUE_OK) { *output=debug->write.snapshot;debug->write.snapshot=NULL; }
    xr_xir_value_drop(&debug->write.value);xr_xir_vm_debug_snapshot_drop(debug->write.snapshot);debug->write=(DebugWrite){0};return status;
}
static XrXirValueStatus debug_write_status(XrXirCallStatus status) {
    return status==XR_XIR_CALL_READY ? XR_XIR_VALUE_OK : status==XR_XIR_CALL_OOM ? XR_XIR_VALUE_OOM :
        status==XR_XIR_CALL_LIMIT ? XR_XIR_VALUE_LIMIT : status==XR_XIR_CALL_UNSUPPORTED ? XR_XIR_VALUE_UNSUPPORTED : XR_XIR_VALUE_BAD_ARGUMENT;
}
/* Root Cell replacement updates every already-readable plain alias. Scoped
 * projections need their own prepared path transaction and reject here. */
static XrXirValueStatus debug_write_aliases(XrXirVmDebug *debug,XrXirVmDebugSnapshot *snapshot,const XrXirValue *cell) {
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    for (uint32_t f=0;f<snapshot->count;++f) {
        SnapshotFrame *frame=&snapshot->frames[f];
        if (!xr_xir_domain_work(debug->domain,frame->info.value_count)) return XR_XIR_VALUE_LIMIT;
        for (uint32_t v=0;v<frame->info.value_count;++v) {
            if (!(frame->known[v]&1) || !xr_xir_type_is_cell(module->types,(XrXirType)frame->values[v].type) ||
                !xr_xir_cell_same_owner(cell,&frame->values[v])) continue;
            if (xr_xir_cell_is_projection(&frame->values[v])) return XR_XIR_VALUE_UNSUPPORTED;
            if (!(frame->known[v]&2)) continue;
            XrXirValue owned={0};XrXirValueStatus status=xr_xir_value_copy(&debug->write.value,&owned);
            if (status!=XR_XIR_VALUE_OK) return status;
            xr_xir_value_drop(&frame->cell_values[v]);frame->cell_values[v]=owned;
        }
    }
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus debug_write_apply(XrXirVmDebug *debug,const XirVmDebugPoint *point) {
    const XirDebugCallFrame *current=xir_debug_calls_current(&debug->calls);
    if (!debug->snapshot || !current || current->id!=debug->snapshot->frames[0].info.id ||
        current->thread!=debug->snapshot->stopped_thread) return XR_XIR_VALUE_BAD_ARGUMENT;
    const DebugFrame *live=find_id(debug,debug->write.frame);
    const XirDebugCallFrame *call=xir_debug_calls_id(&debug->calls,debug->write.frame);
    if (!debug->snapshot || debug->epoch!=debug->write.epoch || !live || !call ||
        call->thread!=debug->snapshot->stopped_thread || debug->write.operand>=live->point.layout->slot_count ||
        !live->known[debug->write.operand]) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    const XrXirFunction *function=&module->functions[live->point.binding->function];uint32_t operand=debug->write.operand;
    XrXirType type=xr_xir_operand_type(function,operand);uint32_t offset=live->point.layout->offsets[operand];
    if (!xr_xir_type_is_cell(module->types,type) || offset==UINT32_MAX) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue cell={(uint32_t)type,0,xr_xir_scalar_load(live->point.frame,offset)};
    if (!xr_xir_value_valid(&cell) || xr_xir_cell_is_projection(&cell)) return XR_XIR_VALUE_UNSUPPORTED;
    XrXirValueAdmission *admission=xr_xir_call_admission(point->view);
    if (!admission) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status=xr_xir_value_admit(&debug->write.value,xr_xir_cell_element(module->types,type),admission);
    if (status!=XR_XIR_VALUE_OK) return status;
    XrXirVmDebugSnapshot *fresh=NULL,*reserved=NULL;
    status=capture_prepare(debug,point->view,NULL,debug->snapshot->cause,&fresh);
    if (status==XR_XIR_VALUE_OK && debug->snapshot->exception.type)
        status=xr_xir_value_copy(&debug->snapshot->exception,&fresh->exception);
    if (status==XR_XIR_VALUE_OK) status=debug_write_aliases(debug,fresh,&cell);
    if (status==XR_XIR_VALUE_OK) {
        if (!xr_xir_reference_retain(&fresh->references)) status=XR_XIR_VALUE_REFCOUNT_LIMIT;else reserved=fresh;
    }
    if (status==XR_XIR_VALUE_OK) status=debug_write_status(xr_xir_instance_cell_write(point->view,&cell,&debug->write.value));
    if (status==XR_XIR_VALUE_OK) {
        capture_publish(debug,fresh);debug->write.snapshot=reserved;return XR_XIR_VALUE_OK;
    }
    xr_xir_vm_debug_snapshot_drop(reserved);xr_xir_vm_debug_snapshot_drop(fresh);return status;
}
