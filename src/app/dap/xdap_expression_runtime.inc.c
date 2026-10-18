/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_expression_runtime.inc.c - Bounded execution of compiled data expressions
 *
 * KEY CONCEPT:
 *   A fresh Source Program consumes owned observations without driving the
 *   stopped target. Every result survives the temporary execution owners.
 */
static XrXirStatus expression_call_status(XrXirCallStatus status) {
    return status==XR_XIR_CALL_READY || status==XR_XIR_CALL_RETURNED ? XR_XIR_OK :
        status==XR_XIR_CALL_OOM ? XR_XIR_OUT_OF_MEMORY :
        status==XR_XIR_CALL_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_TYPE;
}
static XrXirStatus expression_runtime(ExpressionState *state,XrXirSourceProduct *product,
    uint32_t function,const XrXirValue *arguments,uint32_t count,XrXirValue *output) {
    XrXirProgram *program=NULL;XrXirInstance *instance=NULL;XrXirValue result={0};
    XrXirStatus status=xr_xir_compile_source_product_vm_take(product,&program);
    XrXirInstanceConfig config;
    if (status==XR_XIR_OK) status=expression_call_status(xr_xir_instance_config_init(&config,sizeof(config)));
    if (status==XR_XIR_OK) {
        config.metadata_limit=config.call_limit=XR_XIR_CTFE_MAX_FRAME_BYTES;
        config.value_limit=state->request->source_limit;
        config.requested_value_limit=config.value_limit;config.requested_call_limit=config.call_limit;
        config.work_limit=config.poll_limit=XR_XIR_CTFE_MAX_STEPS;config.depth_limit=XR_XIR_CTFE_MAX_CALL_DEPTH;
        status=expression_call_status(xr_xir_instance_new(program,&config,&instance));
    }
    if (status==XR_XIR_OK) status=expression_call_status(xr_xir_instance_start(instance,function,arguments,count));
    if (status==XR_XIR_OK) {
        XrXirCallResult outcome=xr_xir_instance_poll_bounded(instance,XR_XIR_CTFE_MAX_STEPS).outcome;
        status=outcome.status==XR_XIR_CALL_RETURNED ? XR_XIR_OK :
            outcome.status==XR_XIR_CALL_READY || outcome.status==XR_XIR_CALL_SUSPENDED ? XR_XIR_BUDGET :
            expression_call_status(outcome.status);
    }
    if (status==XR_XIR_OK) status=expression_call_status(xr_xir_instance_take_result(instance,&result));
    if (instance) {
        XrXirCallStatus stopped=xr_xir_instance_stop(instance),freed=xr_xir_instance_free(instance);
        if (status==XR_XIR_OK && stopped!=XR_XIR_CALL_READY) status=expression_call_status(stopped);
        if (status==XR_XIR_OK && freed!=XR_XIR_CALL_READY) status=expression_call_status(freed);
    }
    xr_xir_compile_program_drop(program);
    if (status==XR_XIR_OK) *output=result;else xr_xir_value_drop(&result);
    return status;
}
