/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_view_moves.inc.c - Scoped aggregate owners and simultaneous PHIs
 */
static uint32_t vm_view_field_operand(const ScalarRun *run,const XrXirInstruction *op,uint32_t field,uint32_t *payload) {
    if (op->op==XR_XIR_NULLABLE_SOME) return op->args[0];
    const XrXirTypeNode *tuple=xr_xir_tuple_signature(run->module->types,op->type);
    return tuple->parameters[field].type==XR_XIR_UNIT?UINT32_MAX:run->function->operands[op->args[0]+(*payload)++];
}
static XrXirRunStatus vm_view_aggregate(ScalarRun *run,VmState *state,
    const XrXirInstruction *op,uint32_t destination) {
    const XrXirTypeNode *tuple=op->op==XR_XIR_TUPLE_NEW?xr_xir_tuple_signature(run->module->types,op->type):NULL;
    uint32_t count=tuple?tuple->parameter_count:1;
    if (!run->view || count>=run->layout->outgoing_count) return XR_XIR_RUN_BAD_ARTIFACT;
    XrXirValue *fields=state->arguments;XrXirValueStatus status=XR_XIR_VALUE_OK;uint32_t payload=0;
    memset(fields,0,((size_t)count+1u)*sizeof(*fields));
    for (uint32_t i=0;i<count && status==XR_XIR_VALUE_OK;++i) {
        uint32_t id=vm_view_field_operand(run,op,i,&payload);if (id==UINT32_MAX) continue;
        XrXirValue source=vm_value_operand(run,id);
        if (xr_xir_compile_type_arena_borrowed(run->view->arena,(XrXirType)source.type)&XR_XIR_VIEW_MUTABLE) {
            if (!xr_xir_value_argument(&source,run->view->arena,(XrXirType)source.type)) {status=XR_XIR_VALUE_BAD_ARGUMENT;break;}
            fields[i]=source;xr_xir_scalar_store(run->frame,run->layout->offsets[id],0);
        } else status=xr_xir_value_copy(&source,fields+i);
    }
    if (status==XR_XIR_VALUE_OK) status=xr_xir_call_view_aggregate(run->view,op->op,op->type,fields,count,fields+count);
    payload=0;
    for (uint32_t i=0;i<count;++i) {
        uint32_t id=vm_view_field_operand(run,op,i,&payload);
        if (status!=XR_XIR_VALUE_OK && fields[i].type &&
            (xr_xir_compile_type_arena_borrowed(run->view->arena,(XrXirType)fields[i].type)&XR_XIR_VIEW_MUTABLE))
            xr_xir_owned_slot_move(run->frame,run->layout->offsets[id],fields+i);
        else xr_xir_value_drop(fields+i);
    }
    if (status==XR_XIR_VALUE_OK) xr_xir_owned_slot_move(run->frame,destination,fields+count);
    xr_xir_value_drop(fields+count);return value_run_status(status);
}
static bool vm_view_phi_target(const ScalarRun *run,uint32_t target) {
    if (!run->view) return false;
    for (uint32_t i=run->function->blocks[target].first;i<run->function->instruction_count;++i) {
        const XrXirInstruction *phi=&run->function->instructions[i];if (phi->op!=XR_XIR_PHI) break;
        if (xr_xir_compile_type_arena_borrowed(run->view->arena,phi->type)) return true;
    }
    return false;
}
static XrXirRunStatus vm_view_phi_edge(ScalarRun *run,uint32_t predecessor,uint32_t target) {
    VmState *state=run->view->state;uint32_t count=0;
    _Static_assert(sizeof(XrXirViewPhiTransfer)<=sizeof(XrXirValue),"PHI metadata fits outgoing storage");
    for (uint32_t i=run->function->blocks[target].first;i<run->function->instruction_count;++i) {
        const XrXirInstruction *phi=&run->function->instructions[i];if (phi->op!=XR_XIR_PHI) break;
        if (phi->type==XR_XIR_UNIT) continue;
        if (count>=run->layout->outgoing_count) return XR_XIR_RUN_BAD_ARTIFACT;
        uint32_t source=UINT32_MAX;
        for (uint32_t a=0;a<phi->args[1];a+=2) if (run->function->operands[phi->args[0]+a]==predecessor) {
            source=run->function->operands[phi->args[0]+a+1];break;
        }
        if (source==UINT32_MAX) return XR_XIR_RUN_BAD_ARTIFACT;
        uint32_t destination=run->layout->offsets[run->function->parameter_count+i];
        XrXirViewPhiTransfer row={phi->type,run->layout->offsets[source],destination,destination+8};
        memcpy((unsigned char *)state->arguments+(size_t)count*sizeof(row),&row,sizeof(row));++count;
    }
    XrXirViewPhiBatch batch={run->frame,run->layout->frame_bytes,(const XrXirViewPhiTransfer *)state->arguments,count};
    XrXirValueStatus status=xr_xir_call_view_phi(run->view,&batch);
    memset(state->arguments,0,(size_t)count*sizeof(XrXirViewPhiTransfer));return value_run_status(status);
}
static XrXirRunStatus vm_view_cleanup(ScalarRun *run,VmState *state,
    const XrXirInstruction *op,XrXirAction *action) {
    if (!op->args[1]) return XR_XIR_RUN_OK;
    if (!run->view || op->args[1]>run->layout->outgoing_count) return XR_XIR_RUN_BAD_ARTIFACT;
    _Static_assert(sizeof(XrXirViewEndWord)<=sizeof(XrXirValue),"cleanup metadata fits outgoing storage");
    for (uint32_t i=0;i<op->args[1];++i) {
        uint32_t value=run->function->operands[op->args[0]+i];
        XrXirViewEndWord row={xr_xir_operand_type(run->function,value),run->layout->offsets[value]};
        memcpy((unsigned char *)state->arguments+(size_t)i*sizeof(row),&row,sizeof(row));
    }
    XrXirViewEndBatch batch={run->frame,run->layout->frame_bytes,(const XrXirViewEndWord *)state->arguments,op->args[1]};
    XrXirValueStatus status=xr_xir_call_view_end_words(run->view,&batch);
    memset(state->arguments,0,(size_t)op->args[1]*sizeof(XrXirViewEndWord));
    if (status!=XR_XIR_VALUE_OK) *action=xr_xir_call_view_operation_fault(status,(XrXirFaultDetail){0});
    return XR_XIR_RUN_OK;
}
