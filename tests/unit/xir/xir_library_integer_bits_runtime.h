/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_integer_bits_runtime.h - Independent signed integer outputs after producer retirement
 */
#ifndef XIR_LIBRARY_INTEGER_BITS_RUNTIME_H
#define XIR_LIBRARY_INTEGER_BITS_RUNTIME_H
static XrXirCompileContext bits_context(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();return context;
}
static XrXirValue bits_result(XrXirInstance *instance,uint32_t entry,int64_t a,int64_t b,uint32_t count) {
    const XrXirValue args[]={{XR_XIR_I64,0,a},{XR_XIR_I64,0,b}};
    CHECK(xr_xir_instance_start(instance,entry,args,count)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static XrXirValue bits_runtime_cases(XrXirProgram *program,const uint32_t selection[6]) {
    static const struct {unsigned entry;int64_t a,b,answer;uint32_t count;} cases[]={
        {0,85,15,5,2},{1,80,15,95,2},{2,85,15,90,2},{3,85,0,-86,1},
        {4,5,3,40,2},{5,-40,3,-5,2},{0,-1,0,0,2},{1,-1,0,-1,2},
        {2,-1,-1,0,2},{3,0,0,-1,1},{4,0,3,0,2},{5,-1,1,-1,2}
    };
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        XrXirValue value=bits_result(instance,selection[cases[i].entry],cases[i].a,cases[i].b,cases[i].count);
        CHECK(value.type==XR_XIR_I64 && !value.reserved && value.payload==cases[i].answer);xr_xir_value_drop(&value);
    }
    XrXirValue kept=bits_result(instance,selection[1],80,15,2);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return kept;
}
static void bits_retained(XrXirValue *kept) {
    CHECK(kept->type==XR_XIR_I64 && !kept->reserved && kept->payload==95);xr_xir_value_drop(kept);
}
#endif // XIR_LIBRARY_INTEGER_BITS_RUNTIME_H
