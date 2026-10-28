/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * exception_policy_controls.h - Missing proof, independent stops and physical faults
 */
#ifndef DAP_EXCEPTION_POLICY_CONTROLS_H
#define DAP_EXCEPTION_POLICY_CONTROLS_H
static XrXirInstanceConfig policy_instance(HandlerFixture *fixture) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.metadata_limit=config.value_limit=config.call_limit=1048576;
    config.requested_value_limit=config.requested_call_limit=1048576;
    config.work_limit=128000000;config.poll_limit=10000;config.depth_limit=64;
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&fixture->sink};return config;
}
static void policy_omitted_route(void) {
    HandlerFixture fixture={0};handler_open(&fixture);CHECK(handler_build(&fixture,route_cases[0].text)==XR_XIR_OK);
    const XrXirSourceDebugView *original=xr_xir_compile_source_product_debug(fixture.launch->source);
    const XrXirCompileContext *context=xr_source_compiler_context(fixture.compiler);
    XrXirSourceProductPacketView packet={0};XrXirArtifact *closed=NULL,*lowered=NULL;XrXirProgram *program=NULL;XrXirVmDebug *observer=NULL;
    CHECK(xr_xir_compile_source_product_packet(fixture.launch->source,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&closed,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);CHECK(xr_xir_vm_debug_new(fixture.domain,64,&observer)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_vm_debug_program_take(&lowered,observer,&program)==XR_XIR_OK && !lowered);
    CHECK(xr_xir_vm_debug_break_on_throw(observer,XR_XIR_VM_DEBUG_EXCEPTION_CAUGHT)==XR_XIR_VALUE_UNSUPPORTED);
    CHECK(xr_xir_vm_debug_break_on_throw(observer,XR_XIR_VM_DEBUG_EXCEPTION_ALL)==XR_XIR_VALUE_OK);
    XrXirSourceDebugFunction *functions=NULL;size_t bytes=(size_t)original->function_count*sizeof(*functions);
    CHECK(xr_compile_resources_alloc(fixture.resources,bytes,(void **)&functions)==XR_COMPILE_RESOURCE_OK);memcpy(functions,original->functions,bytes);
    uint32_t selected=UINT32_MAX;
    for (uint32_t f=0;f<original->function_count;++f) if (functions[f].route_count) { selected=f;break; }
    CHECK(selected!=UINT32_MAX);functions[selected].route_count=0;functions[selected].routes=NULL;
    XrXirSourceDebugView missing=*original;missing.functions=functions;CHECK(xir_vm_debug_source_map(observer,context,&missing)==XR_XIR_OK);
    CHECK(xr_xir_vm_debug_break_on_throw(observer,XR_XIR_VM_DEBUG_EXCEPTION_CAUGHT)==XR_XIR_VALUE_UNSUPPORTED);
    CHECK(xr_xir_vm_debug_break_on_throw(observer,XR_XIR_VM_DEBUG_EXCEPTION_UNCAUGHT)==XR_XIR_VALUE_UNSUPPORTED);
    memset(functions,0xff,bytes);xr_compile_resources_free(functions);
    XdapCurrent *control=NULL;XrXirInstanceConfig config=policy_instance(&fixture);
    CHECK(xdap_current_new(fixture.domain,&observer,program,&config,&control)==XR_XIR_VALUE_OK && !observer);
    xr_xir_compile_source_product_free(fixture.launch->source);fixture.launch->source=NULL;
    xr_source_compiler_free(fixture.compiler);fixture.compiler=NULL;
    CHECK(xdap_current_start(control,fixture.launch->entry,NULL,0)==XR_XIR_CALL_READY);
    XdapCurrentResult result={0};CHECK(xdap_current_poll(control,1000,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED && !fixture.length);
    XrXirVmDebugSnapshot *snapshot=NULL;CHECK(xdap_current_snapshot(control,&snapshot)==XR_XIR_VALUE_OK);
    XirVmDebugExceptionRoute route={0};uint32_t filter=0;
    CHECK(xir_vm_debug_snapshot_exception_route(snapshot,&route)==XR_XIR_VALUE_OK && route.kind==XIR_VM_DEBUG_EXCEPTION_ROUTE_UNKNOWN);
    CHECK(xir_vm_debug_snapshot_exception_filter(snapshot,&filter)==XR_XIR_VALUE_OK && filter==XR_XIR_VM_DEBUG_EXCEPTION_ALL);
    xr_xir_vm_debug_snapshot_drop(snapshot);
    CHECK(xdap_current_continue(control,result.debug_epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);xdap_current_result_drop(&result);
    CHECK(xdap_current_poll(control,1000,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(fixture.length==3 && !memcmp(fixture.output,"71\n",3));xdap_current_result_drop(&result);
    CHECK(xdap_current_free(control)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);handler_close(&fixture);
}
static void policy_independent_breakpoint(void) {
    HandlerFixture fixture={0};handler_open(&fixture);CHECK(handler_build(&fixture,route_cases[6].text)==XR_XIR_OK);
    const XrXirSourceDebugView *source=xr_xir_compile_source_product_debug(fixture.launch->source);XrXirVmDebugLocation location={UINT32_MAX,UINT32_MAX};
    for (uint32_t f=0;f<source->function_count;++f) if (source->functions[f].throw_count) {
        location=(XrXirVmDebugLocation){f,source->functions[f].throw_sites[0].instruction};break;
    }
    CHECK(location.function!=UINT32_MAX);
    CHECK(xdap_current_break_on_throw(fixture.launch->control,XR_XIR_VM_DEBUG_EXCEPTION_CAUGHT)==XR_XIR_VALUE_OK);
    CHECK(xdap_current_breakpoints_replace(fixture.launch->control,&location,1)==XR_XIR_VALUE_OK);
    CHECK(xdap_current_start(fixture.launch->control,fixture.launch->entry,NULL,0)==XR_XIR_CALL_READY);
    XdapCurrentResult result={0};CHECK(xdap_current_poll(fixture.launch->control,1000,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED);
    XrXirVmDebugSnapshot *snapshot=NULL;CHECK(xdap_current_snapshot(fixture.launch->control,&snapshot)==XR_XIR_VALUE_OK);
    XrXirValue value={0};CHECK(xr_xir_vm_debug_snapshot_exception(snapshot,&value)==XR_XIR_VALUE_UNSUPPORTED);
    uint32_t filter=123;CHECK(xir_vm_debug_snapshot_exception_filter(snapshot,&filter)==XR_XIR_VALUE_UNSUPPORTED && filter==123);
    XrXirVmDebugFrameInfo frame={0};XrXirValue name={0};CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,0,&frame,&name)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&name);CHECK(frame.function==location.function && frame.instruction==location.instruction);
    CHECK(xdap_current_continue(fixture.launch->control,result.debug_epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);xdap_current_result_drop(&result);
    CHECK(xdap_current_poll(fixture.launch->control,1000,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_THROWN);
    xdap_current_result_drop(&result);xr_xir_vm_debug_snapshot_drop(snapshot);handler_close(&fixture);
}
static size_t policy_capture_fault(uint32_t mode,size_t fail) {
    HandlerFixture fixture={0};handler_open(&fixture);const RouteCase *example=&route_cases[mode==1 ? 6 : 1];
    CHECK(handler_build(&fixture,example->text)==XR_XIR_OK);
    uint32_t filter=mode ? XR_XIR_VM_DEBUG_EXCEPTION_UNCAUGHT : XR_XIR_VM_DEBUG_EXCEPTION_CAUGHT;
    CHECK(xdap_current_break_on_throw(fixture.launch->control,filter)==XR_XIR_VALUE_OK);
    xr_xir_compile_source_product_free(fixture.launch->source);fixture.launch->source=NULL;
    xr_source_compiler_free(fixture.compiler);fixture.compiler=NULL;
    size_t begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirCallStatus start=xdap_current_start(fixture.launch->control,fixture.launch->entry,NULL,0);XdapCurrentResult result={0};
    if (start==XR_XIR_CALL_READY) {
        CHECK(xdap_current_poll(fixture.launch->control,1000,&result)==XR_XIR_VALUE_OK);
        if (fail==SIZE_MAX) CHECK(mode==2 ? result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_RETURNED : result.state==XDAP_CURRENT_STOPPED);
        else CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_OOM);
    } else CHECK(fail!=SIZE_MAX && start==XR_XIR_CALL_OOM);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;CHECK(fail==SIZE_MAX || sites>fail);
    CHECK(fixture.length<=strlen(example->output) && !memcmp(fixture.output,example->output,fixture.length));
    xdap_current_result_drop(&result);XrXirCallStatus freed=xdap_launch_free(fixture.launch);fixture.launch=NULL;
    CHECK(freed==XR_XIR_CALL_READY || (fail!=SIZE_MAX && freed==XR_XIR_CALL_OOM));handler_close(&fixture);return sites;
}
#endif // DAP_EXCEPTION_POLICY_CONTROLS_H
