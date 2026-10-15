/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug_write.inc.c - Prepared writes in actual paused VM callbacks
 */
XR_FUNC XrXirValueStatus xir_vm_debug_write_begin(XrXirVmDebug *debug,uint64_t epoch,uint64_t frame,
    uint32_t operand,const XrXirValuePath *path,const XrXirValue *value) {
    if (!debug_valid(debug) || !xr_xir_vm_debug_paused(debug) || debug->write.phase || !debug->snapshot ||
        !epoch || epoch!=debug->epoch || !frame || !path || (path->count && !path->steps) || !xr_xir_value_valid(value)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(debug->domain,(uint64_t)debug->snapshot->count+1+path->count)) return XR_XIR_VALUE_LIMIT;
    const SnapshotFrame *selected=NULL;
    for (uint32_t i=0;i<debug->snapshot->count;++i) if (debug->snapshot->frames[i].info.id==frame) selected=&debug->snapshot->frames[i];
    if (!selected || selected->info.thread!=debug->snapshot->stopped_thread || operand>=selected->info.value_count ||
        !(selected->known[operand]&1)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    bool typed_cell=xr_xir_type_is_cell(module->types,(XrXirType)selected->values[operand].type);
    if (typed_cell ? !(selected->known[operand]&2) : !path->count) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValuePathStep *steps=NULL;XrXirValueStatus status=XR_XIR_VALUE_OK;
    if (path->count) {
        steps=debug_allocate(debug->domain,(uint64_t)path->count*sizeof(*steps),&status);
        if (!steps) return status;memcpy(steps,path->steps,(size_t)path->count*sizeof(*steps));
    }
    XrXirValue owned={0};status=xr_xir_value_copy(value,&owned);
    if (status!=XR_XIR_VALUE_OK) { debug_deallocate(debug->domain,steps,(uint64_t)path->count*sizeof(*steps));return status; }
    debug->write=(DebugWrite){.value=owned,.steps=steps,.count=path->count,.epoch=epoch,.frame=frame,.operand=operand,.phase=1};return XR_XIR_VALUE_OK;
}
XR_FUNC bool xir_vm_debug_write_pending(const XrXirVmDebug *debug) { return debug_valid(debug) && debug->write.phase==1; }
XR_FUNC XrXirValueStatus xir_vm_debug_write_finish(XrXirVmDebug *debug,bool *visited,XrXirVmDebugSnapshot **output) {
    if (!debug_valid(debug) || !visited || !output || *output || !debug->write.phase) return XR_XIR_VALUE_BAD_ARGUMENT;
    *visited=debug->write.phase==2;
    XrXirValueStatus status=debug->write.phase>=2 ? debug->write.result : XR_XIR_VALUE_BAD_ARGUMENT;
    if (status==XR_XIR_VALUE_OK) { *output=debug->write.snapshot;debug->write.snapshot=NULL; }
    xr_xir_value_drop(&debug->write.value);xr_xir_vm_debug_snapshot_drop(debug->write.snapshot);
    debug_deallocate(debug->domain,debug->write.steps,(uint64_t)debug->write.count*sizeof(*debug->write.steps));
    debug->write=(DebugWrite){0};return status;
}
static XrXirValueStatus debug_write_status(XrXirCallStatus status) {
    return status==XR_XIR_CALL_READY ? XR_XIR_VALUE_OK : status==XR_XIR_CALL_OOM ? XR_XIR_VALUE_OOM :
        status==XR_XIR_CALL_LIMIT ? XR_XIR_VALUE_LIMIT : status==XR_XIR_CALL_UNSUPPORTED ? XR_XIR_VALUE_UNSUPPORTED : XR_XIR_VALUE_BAD_ARGUMENT;
}
XR_FUNC XrXirValueStatus xir_vm_debug_write_visit(XrXirVmDebug *debug) {
    if (!debug_valid(debug) || !xr_xir_vm_debug_paused(debug) || debug->write.phase!=1 || !debug->snapshot)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirDebugCallFrame *call=xir_debug_calls_id(&debug->calls,debug->snapshot->frames[0].info.id);
    if (!call || call->event.code!=XR_XIR_CALL_CODE_VM) return XR_XIR_VALUE_BAD_ARGUMENT;
    bool entered=false;XrXirCallStatus driven=xir_instance_visit_vm(debug->target,&call->event.binding,call->event.frame,&entered);
    if (debug->write.phase==1) {
        debug->write.phase=3;debug->write.result=driven==XR_XIR_CALL_READY ? XR_XIR_VALUE_BAD_ARGUMENT : debug_write_status(driven);
        if (entered) xr_xir_vm_debug_disable(debug);
    }
    return debug_write_status(driven);
}
#include "xxir_vm_debug_aliases.inc.c"
#include "xxir_vm_debug_class_write.inc.c"
static XrXirValueStatus debug_write_replacement(XrXirVmDebug *debug,const XirVmDebugPoint *point,
    uint32_t function,const XrXirValue *cell,XrXirValue *output) {
    XrXirValueAdmission *admission=xr_xir_call_admission(point->view);
    if (!admission) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!debug->write.count) return xr_xir_value_copy(&debug->write.value,output);
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    for (uint32_t i=0;i<debug->write.count;++i) {
        const XrXirValuePathStep *step=&debug->write.steps[i];
        if (!xr_xir_domain_work(debug->domain,1)) return XR_XIR_VALUE_LIMIT;
        if (step->kind!=XR_XIR_PATH_FIELD) continue;
        const XrXirTypeNode *node=xr_xir_type_node(module->types,step->container);
        if (!node || node->kind!=XR_XIR_TYPE_NOMINAL || step->selector<0 || (uint64_t)step->selector>UINT32_MAX)
            return XR_XIR_VALUE_BAD_ARGUMENT;
        XrXirStatus allowed=xr_xir_compile_nominal_access(&debug->artifact->context,module,function,
            node->nominal.declaration,(uint32_t)step->selector,XR_XIR_NOMINAL_WRITE);
        if (allowed!=XR_XIR_OK) return allowed==XR_XIR_BUDGET ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_BAD_ARGUMENT;
    }
    XrXirValue root={0};XrXirValueStatus status=debug_write_status(xr_xir_instance_cell_read(point->view,cell,&root));
    XrXirFaultDetail fault={0};XrXirValuePath path={debug->write.steps,debug->write.count};
    if (status==XR_XIR_VALUE_OK) status=xir_value_path_replacement(&root,&path,&debug->write.value,admission,output,&fault);
    xr_xir_value_drop(&root);return status;
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
    if (offset==UINT32_MAX) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue cell={(uint32_t)type,0,xr_xir_scalar_load(live->point.frame,offset)};
    if (!xr_xir_value_valid(&cell)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission *admission=xr_xir_call_admission(point->view);
    if (!admission) return XR_XIR_VALUE_BAD_ARGUMENT;
    uint32_t boundary=UINT32_MAX;
    for (uint32_t i=0;i<debug->write.count;++i) {
        if (!xr_xir_domain_work(debug->domain,1)) return XR_XIR_VALUE_LIMIT;
        if (xr_xir_type_is_class(module->types,debug->write.steps[i].container)) boundary=i;
    }
    bool identity=boundary!=UINT32_MAX,typed_cell=xr_xir_type_is_cell(module->types,type);
    if (!identity && !typed_cell) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue replacement={0};DebugClassWrite class_write={0};XrXirValueStatus status=XR_XIR_VALUE_OK;
    if (identity) {
        XrXirValue root={0};status=typed_cell ? debug_write_status(xr_xir_instance_cell_read(point->view,&cell,&root)) : xr_xir_value_copy(&cell,&root);
        if (status==XR_XIR_VALUE_OK) status=debug_class_write_prepare(debug,point->view,live->point.binding->function,&root,boundary,&class_write);
        xr_xir_value_drop(&root);
    } else {
        status=debug_write_replacement(debug,point,live->point.binding->function,&cell,&replacement);
        if (status==XR_XIR_VALUE_OK) status=xr_xir_value_admit(&replacement,xr_xir_cell_element(module->types,type),admission);
    }
    XrXirVmDebugSnapshot *fresh=NULL,*reserved=NULL;
    if (status==XR_XIR_VALUE_OK) status=capture_prepare(debug,point->view,NULL,debug->snapshot->cause,&fresh);
    if (status==XR_XIR_VALUE_OK && debug->snapshot->exception.type)
        status=xr_xir_value_copy(&debug->snapshot->exception,&fresh->exception);
    XrXirValuePath field={&class_write.field,1};
    if (status==XR_XIR_VALUE_OK) status=identity ? debug_write_aliases(debug,fresh,&class_write.root,&field,&class_write.candidate,admission) :
        debug_write_aliases(debug,fresh,&cell,NULL,&replacement,admission);
    if (status==XR_XIR_VALUE_OK) {
        if (!xr_xir_reference_retain(&fresh->references)) status=XR_XIR_VALUE_REFCOUNT_LIMIT;else reserved=fresh;
    }
    if (status==XR_XIR_VALUE_OK) {
        if (identity) {
            XrXirFaultDetail fault={0};
            status=xr_xir_class_authorized_path_write(&class_write.root,&class_write.access.authority,&field,&class_write.candidate,admission,&fault);
        } else status=debug_write_status(xr_xir_instance_cell_write(point->view,&cell,&replacement));
    }
    debug_class_write_drop(&class_write);
    xr_xir_value_drop(&replacement);
    if (status==XR_XIR_VALUE_OK) {
        capture_publish(debug,fresh);debug->write.snapshot=reserved;return XR_XIR_VALUE_OK;
    }
    xr_xir_vm_debug_snapshot_drop(reserved);xr_xir_vm_debug_snapshot_drop(fresh);return status;
}
