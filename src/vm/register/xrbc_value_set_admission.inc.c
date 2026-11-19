/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_set_admission.inc.c - Closed Set operations over authentic homes
 */
static bool rbc_value_set_access_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,XrRbcValueAccess a) {
    if (!xr_rbc_module_code_owner(module) || a.source || !code->homes || !code->homes->events ||
        !a.other || a.other>code->homes->event_count || a.first>code->argument_count ||
        a.count>code->argument_count-a.first || (a.count && !code->arguments)) return false;
    for (uint32_t p=0;p<a.count;++p) if (!rbc_value_id(code,code->arguments[a.first+p])) return false;
    const uint32_t *args=a.count ? code->arguments+a.first : NULL;
    const XrLirHomeEvent *event=&code->homes->events[a.other-1];
    if (event->kind!=XR_LIR_OWNERSHIP_SET_OPERATION || event->operation!=a.index ||
        event->source!=(a.index==XR_LIR_SET_NEW ? 0u : a.count ? args[0] : 0u) ||
        !event->block || !event->instruction || event->target || event->argument || event->edge ||
        event->target_block || event->transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_NONE ||
        event->hold_transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_NONE || event->callable_transport ||
        event->cell_transport || event->view_operation) return false;
    if (a.index==XR_LIR_SET_NEW) {
        return !a.count && a.window==UINT32_MAX && rbc_value_set(module,code,a.result,true);
    }
    bool mutate=a.index==XR_LIR_SET_ADD || a.index==XR_LIR_SET_REMOVE || a.index==XR_LIR_SET_CLEAR;
    if (mutate) {
        uint32_t count=a.index==XR_LIR_SET_CLEAR ? 1u : 2u;
        if (a.count!=count || rbc_value_kind(module,code,args[0])!=XRT_VALUE_SET_I64 ||
            !rbc_value_binding_valid(module,code,function,args[0]) || !code->homes->workspaces ||
            a.window>=code->homes->workspace_count ||
            (a.index==XR_LIR_SET_REMOVE ? !rbc_value_scalar(code,function,a.result,XR_RBC_BITS) : !!a.result)) return false;
        const XrLirHomeBinding *binding=rbc_value_binding(code,args[0]);
        const XrLirHomeWorkspace *workspace=&code->homes->workspaces[a.window];
        const XrtManagedFrameSpec *frame=&code->homes->frame;
        return workspace->kind==XR_LIR_HOME_WORKSPACE_VALUE && !workspace->value &&
            workspace->block==event->block && workspace->instruction==event->instruction &&
            workspace->type==binding->type && workspace->first_owner<frame->slot_count && frame->owned_offsets &&
            workspace->first_owner!=binding->first_owner && workspace->offset%_Alignof(XrtManagedHeader *)==0 &&
            workspace->offset<=frame->payload_size && sizeof(XrtManagedHeader *)<=frame->payload_size-workspace->offset &&
            frame->owned_offsets[workspace->first_owner]==workspace->offset &&
            (count==1 || rbc_value_scalar(code,function,args[1],XR_RBC_BITS));
    }
    if (a.window!=UINT32_MAX || !a.count || !rbc_value_set(module,code,args[0],false)) return false;
    if (a.index==XR_LIR_SET_DISPLAY) return a.count==1 && rbc_value_string(module,code,a.result,true);
    if (!rbc_value_scalar(code,function,a.result,XR_RBC_BITS)) return false;
    if (a.index==XR_LIR_SET_LEN) return a.count==1;
    return a.index==XR_LIR_SET_CONTAINS && a.count==2 && rbc_value_scalar(code,function,args[1],XR_RBC_BITS);
}
