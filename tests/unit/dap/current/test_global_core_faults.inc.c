/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_global_core_faults.inc.c - Slot writes preserve the stopped owner on failure
 */
static size_t global_fault(size_t fail,bool array) {
    GlobalFixture fixture=global_open(XR_DAP_GLOBAL_ROOT_FIXTURE);XdapAdapter *adapter=fixture.adapter;global_pause(&fixture);
    XdapCurrent *control=adapter->launch->control;XrXirVmDebugFrameInfo before=global_frame(adapter->snapshot,1);
    uint32_t slot=global_slot(adapter->launch,adapter->snapshot,1,array ? "values" : "changing");
    XrXirValuePathStep index={0};XrXirValuePath path={0};
    if (array) {
        XrXirValue value=global_array(&fixture,adapter->snapshot,1,slot,20);
        index=(XrXirValuePathStep){XR_XIR_PATH_INDEX,(XrXirType)value.type,1};path=(XrXirValuePath){&index,1};xr_xir_value_drop(&value);
    }
    XrCompileResourceStats compiler_before={0},compiler_after={0};
    CHECK(xr_compile_resources_stats(fixture.ledger,&compiler_before)==XR_COMPILE_RESOURCE_OK);
    size_t blocks=runtime_live,bytes=runtime_bytes,start=runtime_attempts;XrXirValue replacement={XR_XIR_I64,0,array ? 77 : 99};
    runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : start+fail;
    XrXirValueStatus status=xdap_current_write_slot_begin(control,before.epoch,before.id,slot,&path,&replacement);
    bool armed=status==XR_XIR_VALUE_OK,visited=false;XrXirVmDebugSnapshot *fresh=NULL;
    if (armed) {
        XdapCurrentResult result={0};CHECK(xdap_current_poll(control,1,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED);
        status=xdap_current_write_finish(control,&visited,&fresh);xdap_current_result_drop(&result);
    }
    size_t sites=runtime_attempts-start;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) CHECK(status==XR_XIR_VALUE_OK && visited && fresh);
    else {
        if (status!=XR_XIR_VALUE_OOM) fprintf(stderr,"global fault array=%u fail=%zu status=%u\n",(unsigned)array,fail,(unsigned)status);
        CHECK(status==XR_XIR_VALUE_OOM && !fresh && sites>fail && blocks==runtime_live && bytes==runtime_bytes);
        CHECK(xdap_current_stopped_epoch(control)==before.epoch);
        XrXirVmDebugSnapshot *current=NULL;CHECK(xdap_current_snapshot(control,&current)==XR_XIR_VALUE_OK && current==adapter->snapshot);
        xr_xir_vm_debug_snapshot_drop(current);XrXirVmDebugFrameInfo after=global_frame(adapter->snapshot,1);
        CHECK(after.id==before.id && after.instruction==before.instruction && after.thread==before.thread && after.epoch==before.epoch);
        CHECK(xr_compile_resources_stats(fixture.ledger,&compiler_after)==XR_COMPILE_RESOURCE_OK && compiler_after.live_bytes==compiler_before.live_bytes &&
            compiler_after.work>=compiler_before.work && compiler_after.allocated_bytes>=compiler_before.allocated_bytes);
    }
    if (array) { XrXirValue value=global_array(&fixture,adapter->snapshot,1,slot,20);xr_xir_value_drop(&value); }
    else global_number(adapter->snapshot,1,slot,11);
    global_return(&fixture,before.epoch+(status==XR_XIR_VALUE_OK),status==XR_XIR_VALUE_OK ? (array ? "208\n" : "239\n") : "151\n");
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);xr_xir_vm_debug_snapshot_drop(fresh);global_finish(&fixture);return sites;
}
static void global_faults(bool all) {
    for (unsigned array=0;array<2;++array) {
        size_t sites=global_fault(SIZE_MAX,array!=0);CHECK(sites);
        if (all) for (size_t i=0;i<sites;++i) global_fault(i,array!=0);
        else { global_fault(0,array!=0);if (sites>1) global_fault(sites-1,array!=0); }
        printf("Caller global %s write allocation sites=%zu mode=%s; rollback, original Source and physical0 PASS\n",
            array ? "Array projection" : "scalar",sites,all ? "ALL" : "FIRST_LAST_ONLY");
    }
}
