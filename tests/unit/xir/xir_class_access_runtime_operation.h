/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_access_runtime_operation.h - Complete two-instance Source operations
 */
#ifndef XIR_CLASS_ACCESS_RUNTIME_OPERATION_H
#define XIR_CLASS_ACCESS_RUNTIME_OPERATION_H
static AccessRuntimeCost access_runtime_operation(XrXirProgram *program,const CellSourceEntries *entries,
    const AccessSourceCase *fixture,unsigned mode,const XrXirInstanceConfig *limits,unsigned pass) {
    CHECK(!fixture->rejected && mode<ACCESS_MODES && (!fixture->alias || mode==ACCESS_RUN));
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned);
    size_t compiler_live=instance_compile_live,compiler_bytes=instance_compile_bytes;
    AccessRuntimeRun run={.fixture=fixture,.entries=entries,.mode=mode};
    run.retry=access_runtime_retry;
    runtime_attempts=0;runtime_fail_at=SIZE_MAX;
    cell_runtime_event_count=cell_runtime_fault_at=cell_runtime_hits=0;
    access_runtime_run=&run;cell_runtime_recording=true;
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    for (unsigned owner=0;owner<2;++owner) {
        XrXirInstanceConfig selected=limits && owner==pass?*limits:config;
        access_phase(ACCESS_CREATE,owner);
        XrXirCallStatus status=xr_xir_instance_new(program,&selected,&run.instances[owner]);
        if (access_resource_status(status)) {
            CHECK(!run.instances[owner]);access_failure(&run,owner,status);goto finish;
        }
        CHECK(status==XR_XIR_CALL_READY && run.instances[owner]);
        run.domains[owner]=run.instances[owner]->domain;CHECK(xr_xir_domain_retain(run.domains[owner]));
    }
    CHECK(run.domains[0]!=run.domains[1] && run.instances[0]->program==program && run.instances[1]->program==program);
    /* The census owns one immutable Program across actual fault traversals.
     * The separate normal Source test drops its last producer before execution. */
    if (!access_normal_pass(&run,1)) goto finish;
    if (mode==ACCESS_RUN) {if (!access_normal_pass(&run,0)) goto finish;}
    else if (!access_control(&run)) goto finish;
    run.cost.complete=true;
 finish:
    if (run.retry && !limits)
        for (unsigned owner=0;owner<2;++owner) access_runtime_rebind(&run,owner);
    for (unsigned owner=0;owner<2;++owner) access_close(&run,owner);
    access_phase(ACCESS_DROP,0);
    if (!access_value_empty(&run.projection)) {
        CHECK(xr_xir_value_valid(&run.projection));
        access_projection_released(&run);
        xr_xir_value_drop(&run.projection);
    }
    for (unsigned owner=0;owner<2;++owner) for (unsigned repeat=0;repeat<2;++repeat) {
        if (!access_value_empty(&run.results[owner][repeat])) access_number(&run.results[owner][repeat],fixture->result);
        if (!access_value_empty(&run.counters[owner][repeat])) access_number(&run.counters[owner][repeat],0);
        xr_xir_value_drop(&run.results[owner][repeat]);xr_xir_value_drop(&run.counters[owner][repeat]);
    }
    for (unsigned owner=0;owner<2;++owner) if (run.domains[owner]) {
        run.cost.budgets[owner]=xr_xir_domain_budget_stats(run.domains[owner]);
        run.cost.values[owner]=xr_xir_domain_stats(run.domains[owner]);
        CHECK(run.cost.budgets[owner].bound && run.cost.budgets[owner].work<=run.cost.budgets[owner].work_limit);
        CHECK(!run.cost.budgets[owner].call_live && !run.cost.budgets[owner].metadata_live);
        CHECK(run.cost.values[owner].live_bytes==sizeof(XrXirDomain));xr_xir_domain_drop(run.domains[owner]);
    }
    run.cost.sites=runtime_attempts;run.cost.hits=cell_runtime_hits;
    CHECK(cell_runtime_event_count==run.cost.sites);
    cell_runtime_recording=false;access_runtime_run=NULL;
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned);
    CHECK(instance_compile_live==compiler_live && instance_compile_bytes==compiler_bytes);
    if (run.cost.complete && run.cost.failure==XR_XIR_CALL_READY) {
        CHECK(run.cost.results[1]==2 && run.cost.getters[1]==(fixture->alias?2u:0u));
        if (mode==ACCESS_RUN || mode==ACCESS_CANCEL) CHECK(run.cost.results[0]==2);
        if (mode!=ACCESS_RUN) CHECK(run.cost.controlled && run.cost.loans==1);
    }
    return run.cost;
}
#endif // XIR_CLASS_ACCESS_RUNTIME_OPERATION_H
