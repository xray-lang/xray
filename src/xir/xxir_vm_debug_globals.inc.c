/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug_globals.inc.c - Owned slots from authenticated live VM ancestors
 */
static XrXirValueStatus global_capture_status(XrXirCallStatus status) {
    return status==XR_XIR_CALL_READY ? XR_XIR_VALUE_OK : status==XR_XIR_CALL_OOM ? XR_XIR_VALUE_OOM :
        status==XR_XIR_CALL_LIMIT ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_BAD_ARGUMENT;
}
static XrXirValueStatus capture_global_values(XrXirVmDebug *debug,const XirDebugCallFrame *call,
    XrXirCallView *view,SnapshotFrame *frame) {
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    const XrXirDeclarations *declarations=module->declarations;
    if (!declarations || call->event.entry>=module->function_count) return XR_XIR_VALUE_BAD_ARGUMENT;
    uint32_t owner=declarations->functions[call->event.entry].module,count=0;
    if (!xr_xir_domain_work(debug->domain,(uint64_t)declarations->slot_count*2+1)) return XR_XIR_VALUE_LIMIT;
    for (uint32_t s=0;s<declarations->slot_count;++s) if (declarations->slots[s].module==owner) ++count;
    if (!count) return XR_XIR_VALUE_OK;
    XrXirValueStatus status;frame->global_count=count;
    frame->globals=debug_allocate(debug->domain,(uint64_t)count*sizeof(*frame->globals),&status);
    if (!frame->globals) return status;
    uint32_t at=0;
    for (uint32_t s=0;s<declarations->slot_count;++s) {
        if (declarations->slots[s].module!=owner) continue;
        SnapshotGlobal *global=&frame->globals[at++];global->slot=s;global->cell=declarations->slots[s].mutable!=0;
        XrXirCallStatus read=xir_instance_observe_slot(view,call->event.frame,s,&global->value);
        if (read==XR_XIR_CALL_BAD_ARGUMENT || read==XR_XIR_CALL_BAD_STATE) continue;
        if (read!=XR_XIR_CALL_READY) return global_capture_status(read);
        global->known=1;if (!global->cell) continue;
        read=xir_instance_observe_cell(view,&global->value,&global->cell_value);
        if (read==XR_XIR_CALL_BOUNDS) { global->known|=4;continue; }
        if (read==XR_XIR_CALL_BAD_ARGUMENT || read==XR_XIR_CALL_BAD_STATE) continue;
        if (read!=XR_XIR_CALL_READY) return global_capture_status(read);
        global->known|=2;
    }
    return XR_XIR_VALUE_OK;
}
static const SnapshotGlobal *snapshot_global(const SnapshotFrame *frame,uint32_t slot) {
    for (uint32_t g=0;g<frame->global_count;++g) if (frame->globals[g].slot==slot) return &frame->globals[g];
    return NULL;
}
XR_FUNC XrXirValueStatus xir_vm_debug_snapshot_slot(const XrXirVmDebugSnapshot *snapshot,
    uint32_t frame,uint32_t slot,XrXirValue *output) {
    if (!snapshot_valid(snapshot) || frame>=snapshot->count || !output || output->type || output->reserved || output->payload)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(snapshot->domain,(uint64_t)snapshot->frames[frame].global_count+1)) return XR_XIR_VALUE_LIMIT;
    const SnapshotGlobal *global=snapshot_global(&snapshot->frames[frame],slot);
    if (!global) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!(global->known&1) || (global->cell && !(global->known&2))) return XR_XIR_VALUE_UNSUPPORTED;
    return xr_xir_value_copy(global->cell ? &global->cell_value : &global->value,output);
}
