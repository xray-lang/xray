/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_function.inc.c - Actual compiler Function values and producer leases
 *
 * KEY CONCEPT:
 *   A lease belongs to one live compiler execution and actual verified producer.
 *   Common Value objects own captures; no function value escapes this execution.
 */
struct CtfeFunctionLease {
    CtfeFunctionLease *previous,*next;
    CtfeOwnedState *state;
    uint32_t function,instruction,origin_function,origin_instruction,entry,captures;
    XrXirType type;
};
static void ctfe_function_release(void *pointer) {
    CtfeFunctionLease *lease=pointer;CtfeOwnedState *state=lease->state;
    if (lease->previous) lease->previous->next=lease->next;else state->functions=lease->next;
    if (lease->next) lease->next->previous=lease->previous;
    xr_compile_resources_free(lease);
}

static XrXirValueStatus ctfe_function_cell_admit(CtfeOwnedState *state,XrXirType type) {
    CtfeTypeInfo info={0};
    XrXirCtfeStatus status=ctfe_type_info(state->context,state->artifact->module.types,
        xr_xir_cell_element(state->artifact->module.types,type),true,&info);
    return status==XR_XIR_CTFE_OK?XR_XIR_VALUE_OK:status==XR_XIR_CTFE_OUT_OF_MEMORY?XR_XIR_VALUE_OOM:
        status==XR_XIR_CTFE_MEMORY_LIMIT?XR_XIR_VALUE_MEMORY_LIMIT:
        status==XR_XIR_CTFE_OVERFLOW?XR_XIR_VALUE_OVERFLOW:
        status==XR_XIR_CTFE_FRAME_LIMIT || status==XR_XIR_CTFE_COMPLEXITY_LIMIT?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_BAD_ARGUMENT;
}
static XrXirValueStatus ctfe_function_admit(void *context,const XrXirFunctionBinding *binding,
    XrXirType type,uint64_t *runtime_work) {
    CtfeOwnedState *state=context;
    if (runtime_work || !state || !xr_xir_domain_is_compile(state->domain) ||
        !state->identity || !state->artifact || state->context!=&state->artifact->context ||
        state->identity->domain!=state->domain || !binding || binding->release!=ctfe_function_release)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    CtfeFunctionLease *lease=state->identity->functions;
    while (lease) {
        if (lease==binding->owner) break;lease=lease->next;
    }
    if (!lease || lease->state!=state->identity || state->identity->artifact!=state->artifact || !state->reachable)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirModule *module=&state->artifact->module;
    if (lease->function>=module->function_count || lease->origin_function>=module->function_count ||
        binding->entry!=lease->entry || binding->capture_count!=lease->captures || type!=lease->type ||
        lease->entry>=module->function_count || !state->reachable[lease->entry] ||
        !state->reachable[lease->function] || !state->reachable[lease->origin_function]) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirFunction *producer=&module->functions[lease->function],*origin=&module->functions[lease->origin_function];
    if (lease->instruction>=producer->instruction_count || lease->origin_instruction>=origin->instruction_count)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirInstruction *op=&producer->instructions[lease->instruction],*reference=&origin->instructions[lease->origin_instruction];
    if ((op->op!=XR_XIR_FUNCTION_REF && op->op!=XR_XIR_FUNCTION_WEAKEN) || op->type!=type ||
        reference->op!=XR_XIR_FUNCTION_REF || reference->immediate!=(int64_t)lease->entry || reference->args[1]!=lease->captures)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (type!=reference->type) {
        XrXirStatus narrowed=xr_xir_compile_callable_weakening(state->context,module->types,reference->type,type);
        if (narrowed!=XR_XIR_OK) return narrowed==XR_XIR_OUT_OF_MEMORY?XR_XIR_VALUE_OOM:
            narrowed==XR_XIR_MEMORY_LIMIT?XR_XIR_VALUE_MEMORY_LIMIT:narrowed==XR_XIR_OVERFLOW?XR_XIR_VALUE_OVERFLOW:
            narrowed==XR_XIR_BUDGET?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_BAD_ARGUMENT;
    }
    const XrXirFunction *target=&module->functions[lease->entry];
    const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,type);
    if (!signature || !xr_xir_error_bound_accepts(module->types,target->error_type,signature->element))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (binding->capture_count>target->parameter_count || (binding->capture_count && !binding->captures)) return XR_XIR_VALUE_BAD_ARGUMENT;
    for (uint32_t p=0;p<binding->capture_count;++p) {
        if (binding->captures[p].type!=(uint32_t)target->parameters[p]) return XR_XIR_VALUE_BAD_ARGUMENT;
        if (!xr_xir_type_is_cell(module->types,target->parameters[p])) continue;
        XrXirValueStatus paid=ctfe_function_cell_admit(state,target->parameters[p]);if (paid!=XR_XIR_VALUE_OK) return paid;
        if (!state->cells || xr_xir_cell_provenance_role(state->cells,lease->entry,p)!=XR_XIR_CELL_PROOF_OWNED_CAPTURE ||
             !xr_xir_cell_in_domain(&binding->captures[p],state->domain) || !xr_xir_cell_unborrowed(&binding->captures[p]) ||
             xr_xir_cell_is_projection(&binding->captures[p]) || xr_xir_cell_module_storage(&binding->captures[p]))
            return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    return XR_XIR_VALUE_OK;
}
static XrXirCtfeStatus ctfe_function_binding(CtfeOwnedState *state,const XrXirValue *value,
    const XrXirFunctionBinding **output) {

    const XrXirFunctionBinding *binding=xr_xir_function_binding(value);if (!binding) return XR_XIR_CTFE_BAD_ARTIFACT;
    XrXirCtfeStatus status=ctfe_value_status(ctfe_function_admit(state,binding,(XrXirType)value->type,NULL));
    if (status==XR_XIR_CTFE_OK) *output=binding;return status;
}
static XrXirCtfeStatus ctfe_function_step(ScalarRun *run,VmState *vm,CtfeOwnedState *state,uint32_t function) {
    const XrXirInstruction *op=&run->function->instructions[vm->instruction];
    uint32_t entry=(uint32_t)op->immediate,count=op->args[1],origin_function=function,origin_instruction=vm->instruction;
    const XrXirValue *captures=vm->arguments;XrXirValue source={0};
    XrXirCtfeStatus status=ctfe_value_type_admit(run,state,op->type);if (status!=XR_XIR_CTFE_OK) return status;
    if (op->op==XR_XIR_FUNCTION_WEAKEN) {
        source=ctfe_nominal_operand(run,op->args[0]);const XrXirFunctionBinding *binding=NULL;
        status=ctfe_function_binding(state,&source,&binding);if (status!=XR_XIR_CTFE_OK) return status;
        const CtfeFunctionLease *origin=binding->owner;entry=binding->entry;count=binding->capture_count;captures=binding->captures;
        origin_function=origin->origin_function;origin_instruction=origin->origin_instruction;
    } else {
        if (count && (!vm->arguments || count>run->layout->outgoing_count)) return XR_XIR_CTFE_BAD_ARTIFACT;
        for (uint32_t p=0;p<count;++p) vm->arguments[p]=ctfe_nominal_operand(run,run->function->operands[op->args[0]+p]);
    }
    for (uint32_t p=0;p<count;++p) { status=ctfe_value_graph_admit(run,state,&captures[p]);if (status!=XR_XIR_CTFE_OK) return status; }
    void *memory=NULL;status=ctfe_compile_status(xir_compile_resource_status(xr_compile_resources_calloc(
        state->context->resources,1,sizeof(CtfeFunctionLease),&memory)));if (status!=XR_XIR_CTFE_OK) return status;
    CtfeFunctionLease *lease=memory;CtfeOwnedState *identity=state->identity;
    *lease=(CtfeFunctionLease){NULL,identity->functions,identity,function,vm->instruction,origin_function,origin_instruction,entry,count,op->type};
    if (lease->next) lease->next->previous=lease;identity->functions=lease;
    XrXirFunctionBinding binding={lease,ctfe_function_release,entry,captures,count};XrXirValue result={0};
    XrXirValueAdmission admission=ctfe_value_admission(state);
    XrXirValueStatus created=xr_xir_function_new(state->domain,state->arena,op->type,&binding,&admission,&result);
    if (created!=XR_XIR_VALUE_OK) { ctfe_function_release(lease);return ctfe_value_status(created); }
    xr_xir_owned_slot_move(run->frame,run->layout->offsets[run->function->parameter_count+vm->instruction],&result);
    ++vm->instruction;return XR_XIR_CTFE_OK;
}
