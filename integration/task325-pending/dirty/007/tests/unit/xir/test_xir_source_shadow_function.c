/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_shadow_function.c - Authentic ordinary generic calls
 *
 * KEY CONCEPT:
 *   VM and native code independently consume identical complete sealed programs.
 */
#include "base/xmalloc.h"
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_nullable.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#if defined(XR_REMOVE_NATIVE)
XR_DATA const XrXirProgramSpec array_remove_program;
#endif
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir_source_shadow_pipeline.h"
static uint32_t remove_find(const XrXirModule *module, const char *name) {
    const size_t length = strlen(name);
    uint32_t result = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *decl = &module->functions[f];
        if (decl->name_length == length && !memcmp(decl->name, name, length)) {
            CHECK(result == UINT32_MAX); result = f;
        }
    }
    CHECK(result != UINT32_MAX); return result;
}
static XrXirValue remove_run(XrXirInstance *instance, uint32_t function) {
    CHECK(xr_xir_instance_start(instance, function, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    return value;
}
static void shadow_goldens(RemoveCompile *run) {
    for (unsigned row=0;row<3;++row) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.value_limit=1048576;
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(run->program,&config,&instance)==XR_XIR_CALL_READY);
        XrXirValue value=remove_run(instance,run->program->declarations->entry_function);
        CHECK(value.type==XR_XIR_I64 && value.payload==0);xr_xir_value_drop(&value);
        const uint32_t function=remove_find(run->module,row==0?"case0":row==1?"case1":"case2");
        runtime_attempts=0;value=remove_run(instance,function);
#if defined(XR_SHADOW_REF)
        if (row<2) CHECK(value.type==XR_XIR_I64 && value.payload==(row?8:9));
#else
        if (row==0) CHECK(value.type==XR_XIR_I64 && value.payload==16);
        if (row==1) CHECK(value.type==XR_XIR_BOOL && value.payload==1);
#endif
        if (row==2) {
            const char *text=NULL;size_t length=0;
            CHECK(xr_xir_string_view(&value,&text,&length) && length==6 && !memcmp(text,"mapped",6));
        }
        const size_t sites=runtime_attempts;
        xr_xir_value_drop(&value);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        for (size_t failure=0;failure<sites;++failure) {
            CHECK(xr_xir_instance_new(run->program,&config,&instance)==XR_XIR_CALL_READY);
            value=remove_run(instance,run->program->declarations->entry_function);xr_xir_value_drop(&value);
            runtime_attempts=0;runtime_fail_at=failure;
            XrXirCallStatus status=xr_xir_instance_start(instance,function,NULL,0);
            while (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
            CHECK(status==XR_XIR_CALL_OOM && runtime_attempts>failure);runtime_fail_at=SIZE_MAX;
            value=(XrXirValue){0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_BAD_STATE && !value.type && !value.payload);
            CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && !runtime_live && !runtime_bytes);
        }
        printf("shadow row%u actual runtimeOOM%zu physical0\n",row,sites);
        CHECK(!runtime_live && !runtime_bytes);
    }
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.value_limit=1048576;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(run->program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue initial=remove_run(instance,run->program->declarations->entry_function);xr_xir_value_drop(&initial);
    XrXirValue escaped=remove_run(instance,remove_find(run->module,"case2"));
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(run->program);run->program=NULL;
    xr_xir_compile_artifact_free(run->lowered);run->lowered=NULL;
    const char *text=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&escaped,&text,&length) && length==6 && !memcmp(text,"mapped",6));
    CHECK(runtime_live && runtime_bytes);xr_xir_value_drop(&escaped);CHECK(!runtime_live && !runtime_bytes);
    puts("ordinary generic fn Array<T> 16 true mapped once escaped after Instance/Program drop physical0");
}
#include "xir_source_shadow_compiler.h"
#include "xir_source_shadow_rejections.h"
int main(int argc,char **argv) {
    if (remove_parallel_cli(argc,argv)) return 0;
    if (argc==3 && !strcmp(argv[1],"--compiler")) {remove_compiler((unsigned)strtoul(argv[2],NULL,10));return 0;}
    if (argc==2 && !strcmp(argv[1],"--rejections")) {shadow_rejections();return 0;}
    const unsigned mode=argc>1?(unsigned)strtoul(argv[1],NULL,10):0;
    const char *emit=argc==3?argv[2]:NULL;
    RemoveCompile run=remove_build(SIZE_MAX,remove_limits(),mode,emit);
    if (run.status!=XR_XIR_OK) fprintf(stderr,"shadow function status%u stage%u sites%zu work%llu\n",
        run.status,run.stage,run.sites,(unsigned long long)run.stats.work);
    CHECK(run.status==XR_XIR_OK);
    if (!emit) shadow_goldens(&run);
    printf("shadow function mode%u sites%zu allocated%llu peak%llu work%llu\n",mode,run.sites,
        (unsigned long long)run.stats.allocated_bytes,(unsigned long long)run.stats.peak_bytes,(unsigned long long)run.stats.work);
    remove_release(&run);instance_compile_report();return 0;
}
