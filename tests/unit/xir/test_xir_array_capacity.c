/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_capacity.c - Capacity bounds and one owned reserve publication
 *
 * KEY CONCEPT:
 *   Literal expectations are shared by VM, genuine C and both mixed entry tables.
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
#if defined(XR_CAPACITY_NATIVE)
XR_DATA const XrXirProgramSpec array_capacity_program;
#endif
#include "xir_instance_compile_observer.h"
#define xr_xir_instance_path_write capacity_real_path_write
#include "xir_runtime_allocations.h"
#undef xr_xir_instance_path_write
#include "xir_array_capacity_publication.h"
#include "xir_array_capacity_pipeline.h"
static uint32_t capacity_find(const XrXirModule *module, const char *name) {
    size_t length=strlen(name);uint32_t result=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f){
        if(module->declarations->functions[f].module!=module->declarations->root_module)continue;
        const XrXirFunction *decl=&module->functions[f];
        if(decl->name_length==length&&!memcmp(decl->name,name,length)){CHECK(result==UINT32_MAX);result=f;}}
    CHECK(result!=UINT32_MAX);return result;
}
static XrXirValue capacity_run(XrXirInstance *instance,uint32_t function) {
    CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_READY);
    const XrXirInstanceResult poll=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    if(poll.outcome.status!=XR_XIR_CALL_RETURNED)
        fprintf(stderr,"capacity entry%u status%u panic%lld index%lld length%lld\n",function,
            poll.outcome.status,(long long)poll.outcome.panic.detail.code,
            (long long)poll.outcome.panic.detail.index,(long long)poll.outcome.panic.detail.length);
    CHECK(poll.outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static XrXirInstance *capacity_open(CapacityCompile *run) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.value_limit=1048576;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(run->program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue value=capacity_run(instance,run->program->declarations->entry_function);
    CHECK(value.type==XR_XIR_I64&&!value.payload);xr_xir_value_drop(&value);return instance;
}
#include "xir_array_capacity_cases.h"
#include "xir_array_capacity_rejections.h"
#include "xir_array_capacity_runtime.h"
#include "xir_array_capacity_budget.h"
#include "xir_array_capacity_vm_paths.h"
#include "xir_array_capacity_compiler.h"
int main(int argc,char **argv) {
    if(capacity_parallel_cli(argc,argv))return 0;
    if(argc==3&&!strcmp(argv[1],"--compiler")){
        const unsigned mode=capacity_parallel_argument(argv[2],3);
        capacity_compiler(mode);instance_compile_report();return 0;
    }
    if(argc==2&&!strcmp(argv[1],"--rejections")){capacity_rejections();return 0;}
    if(argc==3&&!strcmp(argv[1],"--runtime-budget")){
        const unsigned mode=capacity_parallel_argument(argv[2],3);
        CapacityCompile run=capacity_build(SIZE_MAX,capacity_limits(),mode,NULL);
        CHECK(run.status==XR_XIR_OK&&run.program);capacity_runtime_budget(&run,mode);
        capacity_release(&run);instance_compile_report();return 0;
    }
    if(argc==3&&(!strcmp(argv[1],"--census")||!strcmp(argv[1],"--cancel"))){
        unsigned mode=(unsigned)strtoul(argv[2],NULL,10);
        CapacityCompile run=capacity_build(SIZE_MAX,capacity_limits(),mode,NULL);
        CHECK(run.status==XR_XIR_OK&&run.program);
        if(!strcmp(argv[1],"--census"))capacity_runtime_census(&run);
        else capacity_runtime_cancel(&run);
        capacity_release(&run);instance_compile_report();return 0;
    }
    unsigned mode=argc>1?(unsigned)strtoul(argv[1],NULL,10):0;
    const char *emit=argc==3?argv[2]:NULL;
    CapacityCompile run=capacity_build(SIZE_MAX,capacity_limits(),mode,emit);
    if(run.status!=XR_XIR_OK)fprintf(stderr,"capacity build status%u stage%u sites%zu\n",run.status,run.stage,run.sites);
    CHECK(run.status==XR_XIR_OK);
    if(!emit){capacity_goldens(&run);if(!mode)capacity_vm_paths(&run);capacity_escape(&run);}
    printf("capacity baseline mode%u sites%zu allocated%llu peak%llu work%llu\n",mode,run.sites,
        (unsigned long long)run.stats.allocated_bytes,(unsigned long long)run.stats.peak_bytes,(unsigned long long)run.stats.work);
    capacity_release(&run);instance_compile_report();return 0;
}
