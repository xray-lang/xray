/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_value_callable.inc.c - Bind callable transport to common SSA events
 */
static uint32_t rbc_lir_callable_event(RbcLirCompiler *c, uint32_t kind, uint32_t value) {
    if (!c->homes) return 0;
    for (size_t n=0;n<c->homes->event_count;++n) {
        const XrLirHomeEvent *event=&c->homes->events[n];
        if (event->kind==kind && event->block==c->diagnostic->block &&
            event->instruction==c->diagnostic->instruction && (!value || event->source==value))
            return n<UINT32_MAX ? (uint32_t)n+1 : 0;
    }
    return 0;
}
static bool rbc_lir_function_environment(RbcLirCompiler *c, const XrLirInstructionView *instruction,
    XrRbcValueAccess *access) {
    const XrLirValueHome *home=&c->homes->values[instruction->result];
    XrLirClosureTarget target={0}; XrLirFunctionView callee;
    if (xr_lir_closure_target_view(c->input,(XrLirFunction){c->input,instruction->callee},&target)!=XR_LIR_OK ||
        xr_lir_function_view(c->input,(XrLirFunction){c->input,instruction->callee},&callee)!=XR_LIR_OK)
        return rbc_lir_fail(c,XR_RBC_COMPILE_UNSUPPORTED,"callable environment has no admitted capture receiver");
    access->operation=XR_RBC_VALUE_FUNCTION_NEW;
    access->index=c->functions[instruction->callee-1]-1;
    access->source=0; access->other=rbc_lir_callable_event(c,XR_LIR_OWNERSHIP_CALLABLE_VALUE,instruction->result);
    access->count=instruction->operand_count;
    if (!access->other) return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"constructor lost its common callable event");
    if (!rbc_lir_value_arguments(c,instruction->operands,instruction->operand_count,&access->first)) return false;
    if (home->transport==XR_LIR_CALLABLE_TRANSPORT_OWNED)
        return (!target.state_capture && home->environment_type) ||
            rbc_lir_fail(c,XR_RBC_COMPILE_UNSUPPORTED,"owned constructor has no complete environment receiver");
    if (home->transport!=XR_LIR_CALLABLE_TRANSPORT_FRAME || home->environment_type)
        return rbc_lir_fail(c,XR_RBC_COMPILE_UNSUPPORTED,"constructor has no concrete callable transport");
    if (!instruction->operand_count) return true;
    size_t bytes=sizeof(XrtFunction);
    for (uint32_t p=0;p<instruction->operand_count;++p) {
        size_t size=p<target.capture_count && rbc_lir_callable_type(c->input,callee.parameter_types[p]) ? sizeof(XrtFunction) : 8;
        if (bytes>UINT32_MAX-size) return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"frame environment exceeds indexed storage");
        bytes+=size;
    }
    if (c->environment_count==UINT32_MAX || bytes>UINT32_MAX-c->environment_size)
        return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"frame environments exceed indexed storage");
    void *storage=c->environments;
    if (!rbc_lir_grow(c,&storage,&c->environment_capacity,c->environment_count+1,sizeof(*c->environments))) return false;
    c->environments=storage;
    access->window=(uint32_t)c->environment_count+1;
    c->environments[c->environment_count++]=(XrRbcEnvironment){access->index,c->environment_size,UINT32_MAX};
    c->environment_size+=(uint32_t)bytes;
    return true;
}
static bool rbc_lir_function_access(RbcLirCompiler *c, const XrLirInstructionView *instruction,
    XrRbcValueAccess *access) {
    if (instruction->op==XR_LIR_FUNCTION_REF) return rbc_lir_function_environment(c,instruction,access);
    if (instruction->op==XR_LIR_CALL_INDIRECT) {
        access->operation=XR_RBC_VALUE_FUNCTION_CALL;
        access->index=rbc_lir_callable_ordinal(c,rbc_lir_value_type(c,instruction->operands[0]))+1;
        access->other=0; access->count=instruction->operand_count-1;
        access->window=c->register_count-instruction->operand_count;
        return rbc_lir_value_arguments(c,instruction->operands+1,access->count,&access->first);
    }
    access->other=0;
    if (instruction->op==XR_LIR_FUNCTION_SCOPE_LEAVE) {
        access->operation=XR_RBC_VALUE_FUNCTION_SCOPE_EXIT;
        access->source=instruction->operand_count==2 ? instruction->operands[1] : 0;
        access->index=rbc_lir_callable_event(c,XR_LIR_OWNERSHIP_CALLABLE_SCOPE_EXIT,0);
        access->other=c->scope_home[instruction->operands[0]];
    } else {
        access->operation=XR_RBC_VALUE_FUNCTION_ALIAS;
        access->source=instruction->operands[instruction->op==XR_LIR_FUNCTION_BIND ? 1 : 0];
        access->index=rbc_lir_callable_event(c,XR_LIR_OWNERSHIP_CALLABLE_VALUE,instruction->result);
        if (instruction->op==XR_LIR_FUNCTION_BIND) access->other=c->scope_home[instruction->operands[0]];
    }
    return access->index || rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"callable operation lost its common event");
}
static bool rbc_lir_function_releases(RbcLirCompiler *c) {
    for (size_t n=0;n<c->homes->event_count;++n) {
        const XrLirHomeEvent *event=&c->homes->events[n];
        if (event->kind!=XR_LIR_OWNERSHIP_RELEASE || event->block!=c->diagnostic->block ||
            event->instruction!=c->diagnostic->instruction || !rbc_lir_callable_type(c->input,rbc_lir_value_type(c,event->source))) continue;
        if (!rbc_lir_value_append(c,(XrRbcValueAccess){.operation=XR_RBC_VALUE_DROP,.source=event->source})) return false;
    }
    return true;
}
