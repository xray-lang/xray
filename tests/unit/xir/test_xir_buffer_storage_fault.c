/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_buffer_storage_fault.c - Actual metadata and payload allocation faults
 *
 * KEY CONCEPT:
 *   The buffer_probe_physical allocation attempt fails before the real owner can publish.
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"buffer fault %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static size_t buffer_probe_physical,buffer_probe_attempts,buffer_probe_fail_at;static bool buffer_probe_active;
static bool refuse(void){return buffer_probe_active && buffer_probe_attempts++==buffer_probe_fail_at;}
static void *observed_malloc(size_t size){if(refuse())return NULL;void *p=xr_malloc(size);if(p)++buffer_probe_physical;return p;}
static void *observed_calloc(size_t count,size_t size){if(refuse())return NULL;void *p=xr_calloc(count,size);if(p)++buffer_probe_physical;return p;}
static void *observed_realloc(void *p,size_t size){if(refuse())return NULL;bool empty=!p;void *q=xr_realloc(p,size);if(q && empty)++buffer_probe_physical;return q;}
static void observed_free(void *p){if(p){CHECK(buffer_probe_physical);--buffer_probe_physical;}xr_free(p);}
static void *observed_aligned(size_t size,size_t alignment){if(refuse())return NULL;void *p=xr_malloc_aligned(size,alignment);if(p)++buffer_probe_physical;return p;}
static void observed_free_aligned(void *p,size_t alignment){(void)alignment;if(p){CHECK(buffer_probe_physical);--buffer_probe_physical;}xr_free_aligned(p,alignment);}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#undef xr_malloc_aligned
#undef xr_free_aligned
#define xr_malloc(size) observed_malloc(size)
#define xr_calloc(count,size) observed_calloc(count,size)
#define xr_realloc(p,size) observed_realloc(p,size)
#define xr_free(p) observed_free(p)
#define xr_malloc_aligned(size,alignment) observed_aligned(size,alignment)
#define xr_free_aligned(p,alignment) observed_free_aligned(p,alignment)
#define xr_xir_buffer_storage_new buffer_probe_new
#include "xir/xxir_value.c"
#undef xr_xir_buffer_storage_new
static size_t buffer_probe_constructors;
XR_FUNC XrXirValueStatus xr_xir_buffer_storage_new(XrXirDomain *domain,int64_t bytes,int64_t alignment,bool zeroed,XrXirValue *output) {
    ++buffer_probe_constructors;return buffer_probe_new(domain,bytes,alignment,zeroed,output);
}
#include "xir/xxir_program.h"
#include "xir/xxir_program_internal.h"
extern const XrXirProgramSpec buffer_storage_program;
static void class_storage_failures(XrXirProgram *program) {
    size_t initial=buffer_probe_physical;
    XrXirDomain *home=NULL,*foreign=NULL;
    CHECK(xr_xir_domain_new(1048576,&home)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1048576,&foreign)==XR_XIR_VALUE_OK);
    XrXirValue fields[2]={{0}},replacement={0},outsider={0},text={0},box={0};
    CHECK(xr_xir_buffer_storage_new(home,13,0,true,&fields[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_buffer_storage_new(home,17,0,true,&fields[1])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_buffer_storage_new(home,27,0,true,&replacement)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_buffer_storage_new(foreign,31,0,true,&outsider)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(home,"wrong",5,&text)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={.arena=program->arena,.domain=home,.work=UINT64_MAX,.scratch_bytes=1048576};
    size_t physical_frame=buffer_probe_physical;uint64_t ledger_frame=xr_xir_domain_stats(home).live_bytes;
    buffer_probe_active=true;buffer_probe_attempts=0;buffer_probe_fail_at=0;
    CHECK(xr_xir_class_new((XrXirType)256,fields,2,&admission,&box)==XR_XIR_VALUE_OOM);
    buffer_probe_active=false;
    CHECK(buffer_probe_attempts==1 && !box.type && !box.payload && buffer_probe_physical==physical_frame);
    CHECK(xr_xir_domain_stats(home).live_bytes==ledger_frame);
    XirObject *first=object_pointer(&fields[0]),*second=object_pointer(&fields[1]);
    /* The real second retain fails after the first initialized field owns a copy. */
    atomic_store(&second->references,UINT32_MAX);
    CHECK(xr_xir_class_new((XrXirType)256,fields,2,&admission,&box)==XR_XIR_VALUE_REFCOUNT_LIMIT);
    atomic_store(&second->references,1);
    CHECK(!box.type && !box.payload && atomic_load(&first->references)==1);
    CHECK(buffer_probe_physical==physical_frame && xr_xir_domain_stats(home).live_bytes==ledger_frame);
    XrXirValue cross[]={fields[0],outsider};
    CHECK(xr_xir_class_new((XrXirType)256,cross,2,&admission,&box)==XR_XIR_VALUE_BAD_ARGUMENT && !box.type);
    CHECK(buffer_probe_physical==physical_frame && xr_xir_domain_stats(home).live_bytes==ledger_frame);
    CHECK(xr_xir_class_new((XrXirType)256,fields,2,&admission,&box)==XR_XIR_VALUE_OK);
    CHECK(atomic_load(&first->references)==2 && atomic_load(&second->references)==2);
    CHECK(xr_xir_class_set(&box,0,&replacement,&admission)==XR_XIR_VALUE_OK);
    CHECK(atomic_load(&first->references)==1 && atomic_load(&object_pointer(&replacement)->references)==2);
    physical_frame=buffer_probe_physical;ledger_frame=xr_xir_domain_stats(home).live_bytes;
    CHECK(xr_xir_class_set(&box,0,&outsider,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_class_set(&box,0,&text,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    XrXirValue forged=text;forged.type=XR_XIR_BUFFER_STORAGE;
    CHECK(xr_xir_class_set(&box,0,&forged,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(buffer_probe_physical==physical_frame && xr_xir_domain_stats(home).live_bytes==ledger_frame);
    XrXirValue received={0};CHECK(xr_xir_class_get(&box,0,&admission,&received)==XR_XIR_VALUE_OK);
    int64_t length=0;CHECK(xr_xir_buffer_storage_length(&received,&length)==XR_XIR_VALUE_OK && length==27);
    xr_xir_value_drop(&fields[0]);xr_xir_value_drop(&fields[1]);xr_xir_value_drop(&replacement);
    xr_xir_value_drop(&outsider);xr_xir_value_drop(&text);xr_xir_domain_drop(foreign);
    xr_xir_domain_drop(home);xr_xir_value_drop(&box);
    CHECK(xr_xir_buffer_storage_length(&received,&length)==XR_XIR_VALUE_OK && length==27);
    xr_xir_value_drop(&received);CHECK(buffer_probe_physical==initial);
}
int main(void) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK && buffer_probe_physical==1);
    uint64_t held_frame=xr_xir_domain_stats(domain).live_bytes;
    for(unsigned mode=0;mode<3;++mode)for(size_t ordinal=0;ordinal<2;++ordinal) {
        buffer_probe_attempts=0;buffer_probe_fail_at=ordinal;buffer_probe_active=true;XrXirValue output={0};
        XrXirValueStatus status=xr_xir_buffer_storage_new(domain,65,mode==2?64:0,mode==1,&output);buffer_probe_active=false;
        CHECK(status==XR_XIR_VALUE_OOM && !output.type && !output.reserved && !output.payload);
        CHECK(buffer_probe_attempts==ordinal+1 && buffer_probe_physical==1 && xr_xir_domain_stats(domain).live_bytes==held_frame);
        CHECK(xr_xir_buffer_storage_new(domain,65,mode==2?64:0,mode==1,&output)==XR_XIR_VALUE_OK);
        xr_xir_value_drop(&output);CHECK(buffer_probe_physical==1 && xr_xir_domain_stats(domain).live_bytes==held_frame);
    }
    xr_xir_domain_drop(domain);CHECK(!buffer_probe_physical);
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&buffer_storage_program,&program)==XR_XIR_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    size_t frame=buffer_probe_physical,constructors=buffer_probe_constructors;
    for(uint32_t f=7;f<9;++f) {
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,f,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_BAD_STATE);
        XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_BAD_STATE && !result.type && !result.reserved && !result.payload);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_BAD_STATE);
        CHECK(buffer_probe_constructors==constructors && buffer_probe_physical==frame);
    }
    class_storage_failures(program);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);CHECK(!buffer_probe_physical);
    puts("Actual byte storage OOM/recovery and aligned0 native rejection before allocation PASS");return 0;
}
