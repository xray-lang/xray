/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_fill_faults.c - Actual allocation and cancellation publication controls
 *
 * KEY CONCEPT:
 *   All four backends consume the same complete proof under one finite ledger.
 */
#include "base/xmalloc.h"
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_panic.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#if defined(XR_FILL_NATIVE)
XR_DATA const XrXirProgramSpec array_fill_program;
#endif
#include "xir_instance_compile_observer.h"
#define xr_xir_instance_path_write fill_real_path_write
#include "xir_runtime_allocations.h"
#undef xr_xir_instance_path_write
#include "xir_array_fill_publication.h"
#include "xir_array_fill_pipeline.h"
static uint32_t fill_find(const XrXirModule *module, const char *name) {
    const size_t length = strlen(name); uint32_t result = UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *decl=&module->functions[f];
        if (decl->name_length==length&&!memcmp(decl->name,name,length)) { CHECK(result==UINT32_MAX);result=f; }
    }
    CHECK(result!=UINT32_MAX);return result;
}
static XrXirValue fill_run(XrXirInstance *instance, uint32_t function) {
    CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);return result;
}
#include "xir_array_fill_cases.h"
#include "xir_array_fill_rejections.h"
#include "xir_array_fill_compiler.h"
#include "xir_array_fill_initialization.h"
#include "xir_array_fill_runtime.h"
int main(int argc,char **argv) {
    if(fill_parallel_cli(argc,argv))return 0;
    if(argc==3&&!strcmp(argv[1],"--compiler")){fill_compiler((unsigned)strtoul(argv[2],NULL,10));return 0;}
    if(argc==3&&!strcmp(argv[1],"--runtime")){
        FillCompile run=fill_build(SIZE_MAX,fill_limits(),(unsigned)strtoul(argv[2],NULL,10),NULL);
        CHECK(run.status==XR_XIR_OK);fill_runtime(&run);fill_release(&run);return 0;
    }
    if(argc==2&&!strcmp(argv[1],"--rejections")){fill_rejections();return 0;}
    const unsigned mode=argc>1?(unsigned)strtoul(argv[1],NULL,10):0;
    const char *emit=argc==3?argv[2]:NULL;
    FillCompile run=fill_build(SIZE_MAX,fill_limits(),mode,emit);
    if(run.status!=XR_XIR_OK)fprintf(stderr,"fill build status%u stage%u sites%zu\n",run.status,run.stage,run.sites);
    CHECK(run.status==XR_XIR_OK);
    if(!emit){CHECK(run.program);fill_goldens(&run);fill_ranges(&run);fill_escape(&run);}
    printf("fill baseline mode%u sites%zu allocated%llu peak%llu work%llu\n",mode,run.sites,
        (unsigned long long)run.stats.allocated_bytes,(unsigned long long)run.stats.peak_bytes,(unsigned long long)run.stats.work);
    fill_release(&run);instance_compile_report();return 0;
}
