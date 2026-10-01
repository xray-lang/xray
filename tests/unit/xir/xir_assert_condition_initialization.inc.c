/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_condition_initialization.inc.c - Sticky owned initializer messages
 */
static void assert_initialization(const char *directory,const char *path) {
    XrXirSourceResult source=assert_source(directory,path,"assert(false,\"init\")\n",true);
    XrXirArtifact *lowered=assert_lower(&source);
    uint32_t entry=xr_xir_artifact_module(lowered)->declarations->entry_function;
    XrXirProgram *program=NULL;
    CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){16777216,64000000},&program)==XR_XIR_OK && !lowered);
    XrXirCallResult owned[2]={{0}};
    for (uint32_t i=0;i<2;++i) {
        XrXirInstance *instance=NULL;XrXirInstanceConfig config=xr_xir_instance_defaults();
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult result=xr_xir_instance_poll(instance);
        CHECK(result.outcome.status==XR_XIR_CALL_ASSERTION && result.outcome.panic.detail.code==445);
        CHECK(result.outcome.value.type==XR_XIR_UNIT && !result.outcome.value.payload && !result.outcome.wake);
        CHECK(xr_xir_instance_state(instance)==XR_XIR_INSTANCE_FAILED);
        CHECK(xr_xir_instance_copy_failure(instance,&owned[i])==XR_XIR_CALL_ASSERTION);
        XrXirValue empty={0};CHECK(xr_xir_instance_take_result(instance,&empty)==XR_XIR_CALL_BAD_STATE);
        CHECK(!empty.type && !empty.reserved && !empty.payload);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_program_drop(program);
    for (uint32_t i=0;i<2;++i) {
        const char *bytes=NULL;size_t length=0;
        CHECK(xr_xir_string_view(&owned[i].panic.message,&bytes,&length) && length==4 && !memcmp(bytes,"init",4));
        xr_xir_call_result_drop(&owned[i]);
    }
    CHECK(!source_live && !source_bytes && !runtime_live && !runtime_bytes);
    puts("Two initializer failures retain exact typed messages beyond Instance/Program destruction PASS");
}
