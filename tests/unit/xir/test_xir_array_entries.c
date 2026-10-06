/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_entries.c - Complete owned entry sequences
 *
 * KEY CONCEPT:
 *   Independent goldens survive producer and instance destruction in every backend.
 */
#include "base/xmalloc.h"
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_tuple.h"
#include "xir/xxir_array.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#if defined(XR_ENTRIES_NATIVE)
XR_DATA const XrXirProgramSpec array_entries_program;
#endif
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir_array_entries_pipeline.h"
static uint32_t entries_find(const XrXirModule *module,const char *name) {
    size_t length=strlen(name);uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *decl=&module->functions[f];
        if(decl->name_length==length&&!memcmp(decl->name,name,length)) {CHECK(found==UINT32_MAX);found=f;}
    }
    CHECK(found!=UINT32_MAX);return found;
}
static XrXirValue entries_run(XrXirInstance *instance,uint32_t function) {
    CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
#if !defined(XR_ENTRIES_MATRIX)
static XrXirInstance *entries_open(EntriesCompile *run) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.value_limit=1048576;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(run->program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue entry=entries_run(instance,run->program->declarations->entry_function);
    CHECK(entry.type==XR_XIR_I64&&!entry.payload);xr_xir_value_drop(&entry);return instance;
}
static XrXirValue entries_get(const XrXirValue *array,int64_t index) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={xr_xir_value_arena(array),domain,NULL,NULL,100000,1048576};
    XrXirValue value={0};XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_get(array,index,&admission,&value,&fault)==XR_XIR_VALUE_OK);
    xr_xir_domain_drop(domain);return value;
}
static void entries_numbers(const XrXirValue *array,const int64_t *expected,int64_t count,bool tuple) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={xr_xir_value_arena(array),domain,NULL,NULL,100000,1048576};int64_t length=-1;
    CHECK(xr_xir_array_len(array,&admission,&length)==XR_XIR_VALUE_OK&&length==count);
    for(int64_t i=0;i<count;++i) {
        XrXirValue item=entries_get(array,i),field={0};
        if(tuple) {
            CHECK(xr_xir_tuple_get(&item,0,&field)==XR_XIR_VALUE_OK&&field.type==XR_XIR_I64&&field.payload==i);
            xr_xir_value_drop(&field);CHECK(xr_xir_tuple_get(&item,1,&field)==XR_XIR_VALUE_OK);
        } else field=item;
        CHECK(field.type==XR_XIR_I64&&field.payload==expected[i]);
        if(tuple)xr_xir_value_drop(&field);xr_xir_value_drop(&item);
    }
    xr_xir_domain_drop(domain);
}
static void entries_goldens(EntriesCompile *run) {
    XrXirInstance *instance=entries_open(run);
    XrXirValue value=entries_run(instance,entries_find(run->module,"result"));
    static const int64_t pair[]={7,9};entries_numbers(&value,pair,2,true);
    XrXirValue count=entries_run(instance,entries_find(run->module,"count"));CHECK(count.payload==1);xr_xir_value_drop(&count);
    XrXirValue empty=entries_run(instance,entries_find(run->module,"empty"));entries_numbers(&empty,pair,0,true);xr_xir_value_drop(&empty);
    XrXirValue independent=entries_run(instance,entries_find(run->module,"independent"));
    static const int64_t separate[]={99,42,7,9};entries_numbers(&independent,separate,4,false);xr_xir_value_drop(&independent);
    XrXirValue text=entries_run(instance,entries_find(run->module,"text"));
    XrXirValue callable=entries_run(instance,entries_find(run->module,"retainedCallable")),functions[2]={{0}};
    for(int64_t i=0;i<2;++i) {
        XrXirValue item={0};XrXirFaultDetail fault={0};XrXirValueAdmission admission=instance_candidate_admission(instance);
        CHECK(xr_xir_array_get(&callable,i,&admission,&item,&fault)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_tuple_get(&item,1,&functions[i])==XR_XIR_VALUE_OK);xr_xir_value_drop(&item);
    }
    xr_xir_compile_artifact_free(run->lowered);run->lowered=NULL;
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(run->program);run->program=NULL;run->module=NULL;
    CHECK(entries_stats(&run->context).live_bytes>run->baseline.live_bytes);
    entries_numbers(&value,pair,2,true);xr_xir_value_drop(&value);
    const char *expected[]={"owned中","a\0b"};const size_t lengths[]={8,3};
    for(int64_t i=0;i<2;++i) {
        XrXirValue item=entries_get(&text,i),string={0},index={0};
        CHECK(xr_xir_tuple_get(&item,0,&index)==XR_XIR_VALUE_OK&&index.payload==i);xr_xir_value_drop(&index);
        CHECK(xr_xir_tuple_get(&item,1,&string)==XR_XIR_VALUE_OK);const char *bytes=NULL;uint64_t length=0;
        CHECK(xr_xir_string_view(&string,&bytes,&length)&&length==lengths[i]&&!memcmp(bytes,expected[i],(size_t)length));
        xr_xir_value_drop(&string);xr_xir_value_drop(&item);
    }
    for(unsigned i=0;i<2;++i) {
        const XrXirFunctionBinding *binding=xr_xir_function_binding(&functions[i]);
        CHECK(binding&&binding->owner&&binding->capture_count==1&&binding->captures[0].type==XR_XIR_I64&&binding->captures[0].payload==7);
    }
    xr_xir_value_drop(&text);xr_xir_value_drop(&callable);
    CHECK(entries_stats(&run->context).live_bytes>run->baseline.live_bytes);
    xr_xir_value_drop(&functions[0]);xr_xir_value_drop(&functions[1]);
    CHECK(!runtime_live&&!runtime_bytes&&entries_stats(&run->context).live_bytes==run->baseline.live_bytes);
    puts("entries escaped tuple/String/Callable captures own metadata through final physical0");
}
#else
#if XR_ENTRIES_MATRIX == 7
#include "xir_array_entries_error_matrix.h"
#elif XR_ENTRIES_MATRIX == 6
#include "xir_array_entries_shadow.h"
#else
#include "xir_array_entries_matrix.h"
#endif
#endif
#include "xir_array_entries_compiler.h"
#include "xir_array_entries_runtime.h"
#include "xir_array_entries_rejections.h"
int main(int argc,char **argv) {
    if(entries_parallel_cli(argc,argv))return 0;
    if(argc==2&&(!strcmp(argv[1],"--rejections")||!strcmp(argv[1],"--rejection-probe"))){entries_rejections(!strcmp(argv[1],"--rejection-probe"));return 0;}
    if(argc==3&&!strcmp(argv[1],"--compiler")){entries_compiler((unsigned)strtoul(argv[2],NULL,10));return 0;}
    unsigned mode=argc>1?(unsigned)strtoul(argv[1],NULL,10):0;
    const char *emit=argc==3?argv[2]:NULL;EntriesCompile run=entries_build(SIZE_MAX,entries_limits(),mode,emit);
    if(run.status!=XR_XIR_OK)fprintf(stderr,"build status%u stage%u sites%zu work%llu\n",run.status,run.stage,run.sites,(unsigned long long)run.stats.work);
    CHECK(run.status==XR_XIR_OK);if(!emit){entries_runtime(&run);entries_goldens(&run);}
    printf("entries baseline mode%u sites%zu allocated%llu peak%llu work%llu\n",mode,run.sites,
        (unsigned long long)run.stats.allocated_bytes,(unsigned long long)run.stats.peak_bytes,(unsigned long long)run.stats.work);
    entries_release(&run);instance_compile_report();return 0;
}
