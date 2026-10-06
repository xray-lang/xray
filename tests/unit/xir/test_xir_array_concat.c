/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_concat.c - Owned snapshots and ordered variadic concatenation
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
#if defined(XR_CONCAT_NATIVE)
XR_DATA const XrXirProgramSpec array_concat_program;
#endif
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir_array_concat_pipeline.h"
static uint32_t concat_find(const XrXirModule *module, const char *name) {
    size_t length=strlen(name);uint32_t result=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f){
        if(module->declarations->functions[f].module!=module->declarations->root_module)continue;
        const XrXirFunction *decl=&module->functions[f];
        if(decl->name_length==length&&!memcmp(decl->name,name,length)){CHECK(result==UINT32_MAX);result=f;}}
    CHECK(result!=UINT32_MAX);return result;
}
static XrXirValue concat_run(XrXirInstance *instance,uint32_t function) {
    CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static XrXirInstance *concat_open(ConcatCompile *run) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.value_limit=1048576;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(run->program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue value=concat_run(instance,run->program->declarations->entry_function);
    CHECK(value.type==XR_XIR_I64&&!value.payload);xr_xir_value_drop(&value);return instance;
}
static void concat_array(const XrXirValue *value,const int64_t *expected,int64_t count) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={xr_xir_value_arena(value),domain,NULL,NULL,100000,1048576};
    int64_t length=-1;CHECK(xr_xir_array_len(value,&admission,&length)==XR_XIR_VALUE_OK&&length==count);
    for(int64_t i=0;i<count;++i){XrXirValue item={0};XrXirFaultDetail fault={0};
        CHECK(xr_xir_array_get(value,i,&admission,&item,&fault)==XR_XIR_VALUE_OK);
        CHECK(item.type==XR_XIR_I64&&item.payload==expected[i]);xr_xir_value_drop(&item);}
    xr_xir_domain_drop(domain);
}
static void concat_goldens(ConcatCompile *run) {
    static const char *const names[]={"numbers","zero","self","vacant","readTemporary","rebound","identity"};
    static const int64_t numbers[]={1,2,3,4,5},zero[]={9,2,1,8,1,2},self[]={1,2,1,2,1,2},read[]={6,7,8},identity[]={1,7,7};
    const int64_t *const expected[]={numbers,zero,self,NULL,read,numbers,identity};
    static const int64_t counts[]={5,6,6,0,3,5,3};
    XrXirInstance *first=concat_open(run),*second=concat_open(run);
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i){
        XrXirValue value=concat_run(first,concat_find(run->module,names[i]));concat_array(&value,expected[i],counts[i]);xr_xir_value_drop(&value);}
    XrXirValue value=concat_run(first,concat_find(run->module,"currentTrace"));CHECK(value.payload==123);xr_xir_value_drop(&value);
    value=concat_run(first,concat_find(run->module,"saved"));static const int64_t changed[]={90,91};concat_array(&value,changed,2);xr_xir_value_drop(&value);
    value=concat_run(first,concat_find(run->module,"savedArgument"));static const int64_t argument[]={77,4};concat_array(&value,argument,2);xr_xir_value_drop(&value);
    value=concat_run(second,concat_find(run->module,"saved"));static const int64_t untouched[]={1,2};concat_array(&value,untouched,2);xr_xir_value_drop(&value);
    value=concat_run(first,concat_find(run->module,"caught"));CHECK(value.type==XR_XIR_I64&&value.payload==445);xr_xir_value_drop(&value);
    value=concat_run(first,concat_find(run->module,"currentTrace"));CHECK(value.payload==12);xr_xir_value_drop(&value);
    value=concat_run(first,concat_find(run->module,"nested"));
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={xr_xir_value_arena(&value),domain,NULL,NULL,100000,1048576};
    int64_t length=-1;CHECK(xr_xir_array_len(&value,&admission,&length)==XR_XIR_VALUE_OK&&length==2);
    for(int64_t i=0;i<2;++i){XrXirValue item={0};XrXirFaultDetail fault={0};
        CHECK(xr_xir_array_get(&value,i,&admission,&item,&fault)==XR_XIR_VALUE_OK);
        static const int64_t one[]={1,2},two[]={3};concat_array(&item,i?two:one,i?1:2);xr_xir_value_drop(&item);}
    xr_xir_domain_drop(domain);xr_xir_value_drop(&value);
    value=concat_run(first,concat_find(run->module,"nullable"));
    CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    admission=(XrXirValueAdmission){xr_xir_value_arena(&value),domain,NULL,NULL,100000,1048576};
    CHECK(xr_xir_array_len(&value,&admission,&length)==XR_XIR_VALUE_OK&&length==3);
    for(int64_t i=0;i<3;++i){XrXirValue item={0};XrXirFaultDetail fault={0};bool some=false;const XrXirValue *payload=NULL;
        CHECK(xr_xir_array_get(&value,i,&admission,&item,&fault)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_nullable_view(&item,&some,&payload));
        if(i==1){CHECK(some&&payload);const XrXirValue *inner=NULL;CHECK(xr_xir_nullable_view(payload,&some,&inner)&&some&&inner&&inner->payload==7);}
        else CHECK(!some&&!payload);
        xr_xir_value_drop(&item);}
    xr_xir_domain_drop(domain);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY);CHECK(xr_xir_instance_free(second)==XR_XIR_CALL_READY);
    CHECK(!runtime_live&&!runtime_bytes);puts("concat empty/zero/self/generic/order123/early12/COW/nested/nullable/class/twoInstances physical0");
}
static void concat_escape(ConcatCompile *run) {
    XrXirInstance *instance=concat_open(run);XrXirValue value=concat_run(instance,concat_find(run->module,"text"));
    xr_xir_compile_artifact_free(run->lowered);run->lowered=NULL;
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(run->program);run->program=NULL;run->module=NULL;
    XrXirValueAdmission admission={xr_xir_value_arena(&value),object_pointer(&value)->domain,NULL,NULL,100000,1048576};
    static const char *const expected[]={"a\0中","b\0c","tail"};static const size_t sizes[]={5,3,4};
    int64_t count=-1;CHECK(xr_xir_array_len(&value,&admission,&count)==XR_XIR_VALUE_OK&&count==3);
    for(int64_t i=0;i<count;++i){XrXirValue item={0};XrXirFaultDetail fault={0};const char *bytes=NULL;size_t length=0;
        CHECK(xr_xir_array_get(&value,i,&admission,&item,&fault)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_view(&item,&bytes,&length)&&length==sizes[i]&&!memcmp(bytes,expected[i],length));xr_xir_value_drop(&item);}
    xr_xir_value_drop(&value);CHECK(!runtime_live&&!runtime_bytes);
    CHECK(concat_stats(&run->context).live_bytes==run->baseline.live_bytes);
    puts("concat escaped String NUL/UTF8 exact bytes after Program/Instance death physical0");
}
#include "xir_array_concat_rejections.h"
int main(int argc,char **argv) {
    if(argc==2&&!strcmp(argv[1],"--rejections")){concat_rejections();return 0;}
    unsigned mode=argc>1?(unsigned)strtoul(argv[1],NULL,10):0;const char *emit=argc==3?argv[2]:NULL;
    ConcatCompile run=concat_build(SIZE_MAX,concat_limits(),mode,emit);
    if(run.status!=XR_XIR_OK)fprintf(stderr,"concat build status%u stage%u sites%zu\n",run.status,run.stage,run.sites);
    CHECK(run.status==XR_XIR_OK);if(!emit){concat_goldens(&run);concat_escape(&run);}
    printf("concat baseline mode%u sites%zu allocated%llu peak%llu work%llu\n",mode,run.sites,
        (unsigned long long)run.stats.allocated_bytes,(unsigned long long)run.stats.peak_bytes,(unsigned long long)run.stats.work);
    concat_release(&run);instance_compile_report();return 0;
}
