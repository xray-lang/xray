/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * route_write_fixture.h - Preserve exception route proof across an actual write
 */
#ifndef DAP_ROUTE_WRITE_FIXTURE_H
#define DAP_ROUTE_WRITE_FIXTURE_H
static void route_unauthenticated(void) {
    XirVmDebugExceptionRoute output={.frame=123};
    CHECK(xir_vm_debug_snapshot_exception_route(NULL,&output)==XR_XIR_VALUE_BAD_ARGUMENT && output.frame==123);
}
static const RouteCase route_write_expected={
    "enum E{Bad}\nfn fail()->i64{var note:i64=5;if(note==5){throw E.Bad};return 0}\n"
    "fn run()->i64{try{return fail()}catch(E.Bad){return 109} // ACCEPT write\n}\nprint(run())\n",
    "109\n",{"ACCEPT write"},{""},{1},1,XR_XIR_CALL_RETURNED,false
};
static uint32_t route_write_operand(const HandlerFixture *fixture) {
    const XrXirSourceView *source=xr_xir_compile_source_product_view(fixture->launch->source);
    const XrXirSourceDebugView *debug=xr_xir_compile_source_product_debug(fixture->launch->source);CHECK(source && debug);
    uint32_t operand=UINT32_MAX;
    for (uint32_t f=0;f<debug->function_count;++f) for (uint32_t b=0;b<debug->functions[f].binding_count;++b) {
        const XrXirSourceDebugBinding *binding=&debug->functions[f].bindings[b];
        CHECK(binding->declaration && binding->declaration<=source->declaration_count);
        if (strcmp(source->declarations[binding->declaration-1].name,"note")) continue;
        CHECK(operand==UINT32_MAX && binding->flags&XR_XIR_SOURCE_DEBUG_CELL);operand=binding->value;
    }
    CHECK(operand!=UINT32_MAX);return operand;
}
static void route_write_value(const XrXirVmDebugSnapshot *snapshot,uint32_t operand,uint64_t expected) {
    XrXirValue value={0};CHECK(xr_xir_vm_debug_snapshot_cell_value(snapshot,0,operand,&value)==XR_XIR_VALUE_OK);
    CHECK(value.type==XR_XIR_I64 && (uint64_t)value.payload==expected);xr_xir_value_drop(&value);
}
static size_t route_write_fault(size_t fail) {
    HandlerFixture fixture={0};handler_open(&fixture);CHECK(handler_build(&fixture,route_write_expected.text)==XR_XIR_OK);
    RoutePoint points[4];uint32_t count=route_points(&fixture,points,4),operand=route_write_operand(&fixture);
    CHECK(xdap_current_break_on_throw(fixture.launch->control,true)==XR_XIR_VALUE_OK);
    xr_xir_compile_source_product_free(fixture.launch->source);fixture.launch->source=NULL;
    xr_source_compiler_free(fixture.compiler);fixture.compiler=NULL;
    CHECK(xdap_current_start(fixture.launch->control,fixture.launch->entry,NULL,0)==XR_XIR_CALL_READY);
    XdapCurrentResult result={0};CHECK(xdap_current_poll(fixture.launch->control,1000,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED);
    uint64_t epoch=result.debug_epoch;xdap_current_result_drop(&result);
    XrXirVmDebugSnapshot *old=NULL;CHECK(xdap_current_snapshot(fixture.launch->control,&old)==XR_XIR_VALUE_OK);
    XirVmDebugExceptionRoute selected=route_snapshot(old,&route_write_expected,0,points,count);route_write_value(old,operand,5);
    XrXirVmDebugFrameInfo frame={0};XrXirValue name={0};
    CHECK(xr_xir_vm_debug_snapshot_frame(old,0,&frame,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
    XrXirValue replacement={XR_XIR_I64,0,7};XrXirValuePath whole={0};
    CHECK(xdap_current_write_begin(fixture.launch->control,epoch,frame.id,operand,&whole,&replacement)==XR_XIR_VALUE_OK);
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(fixture.domain);
    size_t begin=runtime_attempts,live=runtime_live,bytes=runtime_bytes;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    CHECK(xdap_current_poll(fixture.launch->control,1,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;bool visited=false;XrXirVmDebugSnapshot *fresh=NULL;
    XrXirValueStatus status=xdap_current_write_finish(fixture.launch->control,&visited,&fresh);CHECK(visited && !fixture.length);
    if (fail==SIZE_MAX) {
        CHECK(status==XR_XIR_VALUE_OK && fresh && result.debug_epoch==epoch+1);route_write_value(fresh,operand,7);
        XirVmDebugExceptionRoute after=route_snapshot(fresh,&route_write_expected,0,points,count);
        CHECK(after.frame==selected.frame && after.function==selected.function && after.handler==selected.handler && after.accepted==selected.accepted);
        XrXirVmDebugFrameInfo changed={0};CHECK(xr_xir_vm_debug_snapshot_frame(fresh,0,&changed,&name)==XR_XIR_VALUE_OK);
        xr_xir_value_drop(&name);CHECK(changed.id==frame.id && changed.instruction==frame.instruction && changed.epoch==epoch+1);
    } else {
        CHECK(status==XR_XIR_VALUE_OOM && sites>fail && !fresh && result.debug_epoch==epoch);
        CHECK(runtime_live==live && runtime_bytes==bytes);
        XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(fixture.domain);
        CHECK(after.metadata_live==before.metadata_live && after.requested_bytes>=before.requested_bytes && after.work>before.work);
    }
    route_write_value(old,operand,5);
    CHECK(xdap_current_continue(fixture.launch->control,result.debug_epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);xdap_current_result_drop(&result);
    CHECK(xdap_current_poll(fixture.launch->control,1000,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(fixture.length==4 && !memcmp(fixture.output,"109\n",4));xdap_current_result_drop(&result);
    CHECK(xdap_launch_free(fixture.launch)==XR_XIR_CALL_READY);fixture.launch=NULL;
    if (fresh) route_write_value(fresh,operand,7);
    xr_xir_vm_debug_snapshot_drop(old);xr_xir_vm_debug_snapshot_drop(fresh);handler_close(&fixture);return sites;
}
#endif // DAP_ROUTE_WRITE_FIXTURE_H
