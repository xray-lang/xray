/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_task_result.inc.c - Task observations from sealed instruction sites
 */
static XrXirCallStatus instance_task_result_site(XrXirCallView *view,uint32_t instruction,
    const XrXirValue *task,bool waiting,XrXirInstance **owner,XrXirType *result,XrXirType *nullable) {
    if (!view || !task || !instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance=view_instance(view);
    if (!instance || instance!=instance_current(instance)) return XR_XIR_CALL_BAD_STATE;
    const XrXirProgram *program=instance->program;const XrXirProgramPermissions *permissions=program->permissions;
    uint32_t caller=xr_xir_call_current_entry(view->activation);
    if (!permissions || !permissions->input || caller>=program->entry_count ||
        permissions->function_count!=program->entry_count || view->arena!=program->arena)
        return XR_XIR_CALL_BAD_STATE;
    const XrXirModule *body=xr_xir_compile_artifact_module(permissions->input->artifact);
    if (!body || body->function_count!=program->entry_count || !body->functions) return XR_XIR_CALL_BAD_STATE;
    const XrXirFunction *function=&body->functions[caller];
    if (instruction>=function->instruction_count || !function->instructions) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirInstruction *op=&function->instructions[instruction];
    if (op->op!=(waiting?XR_XIR_TASK_AWAIT_RESULT:XR_XIR_TASK_POLL)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirType expected=xr_xir_operand_type(function,op->args[0]);
    if (!xr_xir_value_argument(task,program->arena,expected)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirType value=XR_XIR_UNIT,optional=XR_XIR_UNIT;
    if (waiting) {
        if (op->targets[0]>=function->block_count || !function->blocks) return XR_XIR_CALL_BAD_STATE;
        uint32_t first=function->blocks[op->targets[0]].first;
        if (first>=function->instruction_count || function->instructions[first].op!=XR_XIR_INVOKE_RESULT ||
            function->instructions[first].immediate!=(int64_t)instruction) return XR_XIR_CALL_BAD_STATE;
        value=function->instructions[first].type;
    } else {
        optional=op->type;
        if (!xr_xir_type_is_nullable(program->types,optional)) return XR_XIR_CALL_BAD_STATE;
        value=xr_xir_nullable_element(program->types,optional);
    }
    if (!xr_xir_task_result_matches_task(program->types,value,expected)) return XR_XIR_CALL_BAD_STATE;
    *owner=instance;*result=value;*nullable=optional;return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_instance_task_poll_at(XrXirCallView *view,
    uint32_t instruction,const XrXirValue *task,XrXirValue *output) {
    if (!output || !value_unit(*output)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance=NULL;XrXirType result=XR_XIR_UNIT,nullable=XR_XIR_UNIT;
    XrXirCallStatus status=instance_task_result_site(view,instruction,task,false,&instance,&result,&nullable);
    if (status!=XR_XIR_CALL_READY) return status;
    bool terminal=false;XrXirCallResult outcome={0};
    status=xir_task_executor_observe(instance->executor,view,task,&terminal,&outcome);
    if (status!=XR_XIR_CALL_READY) return status;
    XrXirValue value={0};XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if (terminal) status=xir_task_result_wrap(&outcome,result,admission,&value);
    if (status==XR_XIR_CALL_READY)
        status=value_call_status(xr_xir_nullable_new(nullable,terminal?&value:NULL,admission,output));
    xr_xir_value_drop(&value);xr_xir_call_result_drop(&outcome);return status;
}
XR_FUNC XrXirCallStatus xr_xir_instance_task_await_result_at(XrXirCallView *view,
    uint32_t instruction,const XrXirValue *task) {
    XrXirInstance *instance=NULL;XrXirType result=XR_XIR_UNIT,nullable=XR_XIR_UNIT;
    XrXirCallStatus status=instance_task_result_site(view,instruction,task,true,&instance,&result,&nullable);
    if (status!=XR_XIR_CALL_READY) return status;
    status=xir_task_executor_result_member(instance->executor,view,task);
    if (status!=XR_XIR_CALL_READY) return status;
    return xir_call_task_result_stage(view,task,result);
}
