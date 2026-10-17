/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug_selector.inc.c - Match owned Source selectors without frames
 */
#include "xxir_vm_debug_selector_values.inc.c"
static XrXirValueStatus debug_selector_match(DebugSelector *run,XrXirDomain *domain,bool *matches) {
    uint32_t count=run->selector->accepted-run->selector->first;
    if (!count) { *matches=true;return XR_XIR_VALUE_OK; }
    if (!xr_xir_domain_work(domain,(uint64_t)count*2+1)) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status;uint64_t bytes=(uint64_t)count*sizeof(*run->values);
    run->values=debug_allocate(domain,bytes,&status);if (!run->values) return status;
    status=XR_XIR_VALUE_OK;
    XrXirDomainBudgetStats budget=xr_xir_domain_budget_stats(domain);
    uint64_t available=budget.bound ? budget.work_limit-budget.work : UINT64_MAX;
    run->admission.work=available<run->actual->work ? available : run->actual->work;
    uint64_t work=run->admission.work;uint32_t instruction=run->selector->first;
    run->predecessor=UINT32_MAX;
    while (instruction<run->selector->accepted) {
        const XrXirInstruction *op=&run->function->instructions[instruction];
        if (op->op==XR_XIR_JUMP || op->op==XR_XIR_BRANCH) {
            if (!xr_xir_domain_work(domain,33)) { status=XR_XIR_VALUE_LIMIT;break; }
            uint32_t edge=0;
            if (op->op==XR_XIR_BRANCH) {
                const XrXirValue *value=debug_selector_value(run,op->args[0]);
                if (!value || value->type!=XR_XIR_BOOL || value->reserved || (value->payload!=0 && value->payload!=1)) {
                    status=XR_XIR_VALUE_BAD_ARGUMENT;break;
                }
                edge=value->payload ? 0 : 1;
            }
            run->predecessor=debug_selector_block(run->function,instruction);
            uint32_t target=run->function->blocks[op->targets[edge]].first;
            if (target==run->selector->rejected) { instruction=target;break; }
            if (target<=instruction || target>run->selector->accepted) { status=XR_XIR_VALUE_BAD_ARGUMENT;break; }
            instruction=target;continue;
        }
        XrXirValue *value=&run->values[instruction-run->selector->first];
        status=debug_selector_operation(run,op,value);if (status!=XR_XIR_VALUE_OK) break;
        ++instruction;
    }
    uint64_t consumed=work-run->admission.work;
    bool paid=consumed<=run->actual->work && xr_xir_domain_work(domain,consumed);
    if (consumed<=run->actual->work) run->actual->work-=consumed;
    if (status==XR_XIR_VALUE_OK && !paid) status=XR_XIR_VALUE_LIMIT;
    for (uint32_t i=0;i<count;++i) xr_xir_value_drop(&run->values[i]);
    debug_deallocate(domain,run->values,bytes);run->values=NULL;
    if (status==XR_XIR_VALUE_OK) *matches=instruction==run->selector->accepted;
    return status;
}
static XrXirValueStatus debug_exception_local(XrXirVmDebug *debug,XrXirCallView *view,
    const SnapshotFrame *frame,uint32_t operand,XirVmDebugExceptionLocal *output) {
    uint32_t function=frame->info.function,instruction=frame->info.instruction;
    *output=(XirVmDebugExceptionLocal){XIR_VM_DEBUG_EXCEPTION_LOCAL_UNKNOWN,function,0,UINT32_MAX};
    if (!(debug_source_point(debug,function,instruction).flags&DEBUG_SOURCE_AUTHORED)) return XR_XIR_VALUE_OK;
    if (!xr_xir_call_admission(view) || view->instance!=debug->target || function>=debug->source_functions)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(debug->domain,33)) return XR_XIR_VALUE_LIMIT;
    uint32_t first=debug->source_route_offsets[function],end=debug->source_route_offsets[function+1],handler=0;
    while (first<end) {
        uint32_t middle=first+(end-first)/2;const XrXirSourceDebugRoute *site=&debug->source_routes[middle];
        if (site->instruction==instruction) { handler=site->handler;break; }
        if (site->instruction<instruction) first=middle+1;else end=middle;
    }
    XirVmDebugExceptionLocal result={XIR_VM_DEBUG_EXCEPTION_LOCAL_PROPAGATE,function,0,UINT32_MAX};
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    while (handler) {
        if (!xr_xir_domain_work(debug->domain,1)) return XR_XIR_VALUE_LIMIT;
        const DebugSourceHandler *route=&debug->source_handlers[debug->source_handler_offsets[function]+handler-1];
        for (uint32_t s=0;s<route->count;++s) {
            if (!xr_xir_domain_work(debug->domain,1)) return XR_XIR_VALUE_LIMIT;
            DebugSelector run={.module=module,.function=&module->functions[function],
                .selector=&debug->source_selectors[route->first+s],.error=&frame->values[operand],
                .observer=debug->domain,.actual=xr_xir_call_admission(view),
                .admission=*xr_xir_call_admission(view),.root=route->error};
            /* Existing Function/Cell/Class identities keep their real receiving
             * Domain and live admission. Observation owns only new scratch and
             * literal values; changing a receiver would reject legal payloads. */
            bool matches=false;XrXirValueStatus status=debug_selector_match(&run,debug->domain,&matches);
            if (status!=XR_XIR_VALUE_OK) return status;
            if (matches) {
                result=(XirVmDebugExceptionLocal){XIR_VM_DEBUG_EXCEPTION_LOCAL_MATCH,function,handler,run.selector->accepted};
                *output=result;return XR_XIR_VALUE_OK;
            }
        }
        handler=route->parent;
    }
    *output=result;return XR_XIR_VALUE_OK;
}
