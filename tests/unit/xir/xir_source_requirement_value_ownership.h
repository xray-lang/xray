/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_requirement_value_ownership.h - Ownership past producer destruction
 *
 * KEY CONCEPT:
 *   Two fresh instances retain owned results beyond source and Program lifetime.
 */
#ifndef XIR_SOURCE_REQUIREMENT_VALUE_OWNERSHIP_H
#define XIR_SOURCE_REQUIREMENT_VALUE_OWNERSHIP_H
#include "xir_requirement_value_fixture.h"
static void source_requirement_value_ownership(const XrXirSourceRequest *request) {
    char path[8192]; source_fixture_path(request,"witness_promises.xr",path);
    XrXirSourceRequest local=*request; local.entry_path=path;
    source_manifest_raw(&local,"");
    witness_promise_file(&local,"witness_promises.xr",requirement_value_owned_source);
    /* This helper destroys its compiler session, source snapshot and first
     * Checked artifact, and overwrites packet storage before returning a copy. */
    XrXirArtifact *checked=witness_promise_check(&local,true,"receiver-owned escaped callable",NULL);
    witness_promise_file(&local,"witness_promises.xr","const overwritten=0\n");
    XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_specialize(checked,NULL,&closed,NULL)==XR_XIR_OK);
    xr_xir_artifact_free(checked);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_artifact_free(closed);
    const XrXirModule *module=xr_xir_artifact_module(lowered);
    uint32_t entries[2]={UINT32_MAX,UINT32_MAX};
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        if (function->name_length==6 && !memcmp(function->name,"answer",6)) entries[0]=f;
        if (function->name_length==5 && !memcmp(function->name,"count",5)) entries[1]=f;
    }
    CHECK(entries[0]!=UINT32_MAX && entries[1]!=UINT32_MAX);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
    XrXirValue retained[2]={{0},{0}};
    for (uint32_t run=0;run<2;++run) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); XrXirInstance *instance=NULL;
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        for (uint32_t call=0;call<3;++call) {
            uint32_t entry=call==1 ? entries[0] : entries[1];
            CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
            XrXirValue value={0};
            CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
            if (call==1) retained[run]=value;
            else { CHECK(value.type==XR_XIR_I64 && value.payload==(call==0 ? 0u : 1u)); xr_xir_value_drop(&value); }
        }
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_program_drop(program);
    for (uint32_t run=0;run<2;++run) {
        const char *bytes=NULL; size_t length=0;
        CHECK(retained[run].type==XR_XIR_STRING && xr_xir_string_view(&retained[run],&bytes,&length));
        CHECK(length==6 && !memcmp(bytes,"mapped",6)); xr_xir_value_drop(&retained[run]);
    }
    CHECK(xr_test_unlink(path)==0);
}
#endif // XIR_SOURCE_REQUIREMENT_VALUE_OWNERSHIP_H
