/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_globals_budgets.inc.c - Every insufficient global publication work prefix
 */
typedef struct GlobalWork { uint64_t baseline,required; } GlobalWork;
typedef struct GlobalWorkInput {
    const XrXirSourceView *view;
    const XrXirConstruction *construction;
    const XrXirModule *module;
    const XrXirSourceDebugView *debug;
} GlobalWorkInput;
static GlobalWork global_work_prefix(const GlobalWorkInput *input,uint64_t allowance,GlobalWork work) {
    uint64_t baseline_work=work.baseline;
    XrCompileResourceLimits limits={67108864,8388608,baseline_work ? baseline_work+allowance : 128000000};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats initial={0};CHECK(xr_compile_resources_stats(context.resources,&initial)==XR_COMPILE_RESOURCE_OK);
    XrXirModule checked=*input->module;checked.functions=NULL;checked.function_count=0;
    XrXirSourceDebugView selected={NULL,0,input->debug->globals,input->debug->global_count};
    XrXirSourceSnapshot *copy=NULL;
    CHECK(xr_xir_compile_source_snapshot_copy_v2(&context,input->view,input->construction,&copy)==XR_XIR_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    if (baseline_work) CHECK(before.work==baseline_work);
    size_t blocks=runtime_live,bytes=runtime_bytes;
    XrXirStatus status=xr_xir_compile_source_snapshot_debug_copy(copy,&selected,&checked);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.work>=before.work);
    uint64_t required=baseline_work ? work.required : after.work-before.work;
    if (!baseline_work || allowance>=required) {
        CHECK(status==XR_XIR_OK && xr_xir_compile_source_snapshot_debug(copy)->global_count==input->debug->global_count);
        CHECK(after.work==before.work+required);
    } else {
        CHECK(status==XR_XIR_BUDGET && !xr_xir_compile_source_snapshot_debug(copy));
        CHECK(after.live_bytes==before.live_bytes && blocks==runtime_live && bytes==runtime_bytes && after.work<=baseline_work+allowance);
        CHECK(after.allocated_bytes>=before.allocated_bytes);
    }
    xr_xir_compile_source_snapshot_free(copy);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==initial.live_bytes);
    xr_compile_resources_release(context.resources);return (GlobalWork){before.work,required};
}
static void global_work_budgets(const GlobalWorkInput *input) {
    GlobalWork work=global_work_prefix(input,UINT64_MAX,(GlobalWork){0});CHECK(work.baseline && work.required && work.required<16384);
    for (uint64_t allowance=0;allowance<=work.required;++allowance) global_work_prefix(input,allowance,work);
    printf("Global metadata publication work: all %llu insufficient prefixes and exact boundary PASS\n",(unsigned long long)work.required);
}
