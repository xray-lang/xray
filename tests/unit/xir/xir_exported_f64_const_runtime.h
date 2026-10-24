/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_exported_f64_const_runtime.h - Scalar and managed callable owner lifetime
 */
#ifndef XIR_EXPORTED_F64_CONST_RUNTIME_H
#define XIR_EXPORTED_F64_CONST_RUNTIME_H
static XrXirCompileContext ci_context(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();return context;
}
static XrXirValue ci_take(XrXirInstance *instance,uint32_t entry) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static void ci_number(XrXirValue *value,XrXirType type,uint64_t expected) {
    CHECK(value->type==(uint32_t)type && !value->reserved && (uint64_t)value->payload==expected);xr_xir_value_drop(value);
}
static XrXirValue ci_runtime_cases(XrXirProgram *program,const uint32_t selection[6]) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *first=NULL,*other=NULL;
    CHECK(xr_xir_instance_new(program,&config,&first)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&other)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    const uint64_t expected[]={UINT64_C(0x400921fb54442d18),UINT64_C(0x4020000000000000),1,UINT64_C(0x8000000000000000),UINT64_C(0x4030000000000000)};
    for(unsigned repeat=0;repeat<2;++repeat) for(unsigned i=0;i<5;++i) {
        XrXirValue a=ci_take(first,selection[i]),b=ci_take(other,selection[i]);
        XrXirType type=i==2?XR_XIR_I64:XR_XIR_F64;ci_number(&a,type,expected[i]);ci_number(&b,type,expected[i]);
    }
    XrXirValue function=ci_take(first,selection[5]),copy={0};
    CHECK(xr_xir_instance_start_function(first,&function,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(first,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue answer={0};CHECK(xr_xir_instance_take_result(first,&answer)==XR_XIR_CALL_RETURNED);ci_number(&answer,XR_XIR_F64,UINT64_C(0x400921fb54442d18));
    XrXirValue kept=ci_take(first,selection[3]);
    CHECK(xr_xir_instance_stop(first)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start_function(first,&function,NULL,0)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY);
    CHECK(xr_xir_value_copy(&function,&copy)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_instance_start_function(other,&function,NULL,0)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_free(other)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&function);xr_xir_value_drop(&copy);return kept;
}
static void ci_retained(XrXirValue *kept) {ci_number(kept,XR_XIR_F64,UINT64_C(0x8000000000000000));}
#endif // XIR_EXPORTED_F64_CONST_RUNTIME_H
