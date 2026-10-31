/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_typed_display_values.c - Finite text conversion and real failures
 *
 * KEY CONCEPT:
 *   The provider fault is injected into actual Value allocations. Metadata
 *   and execution budgets remain independent owners with observable refunds.
 */
#include "base/xmalloc.h"
#include "xir/xxir_format.h"
#include "xir/xxir_array.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"display values %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static uint64_t observed_now=1;
static uint64_t display_clock(void) {return observed_now;}
#define xr_time_monotonic_ns display_clock
#include "base/xcompile_request.c"
#undef xr_time_monotonic_ns
static size_t allocation_attempts,fail_at=SIZE_MAX;
static void *display_malloc(size_t size) {return allocation_attempts++==fail_at?NULL:xr_malloc(size);}
static void *display_calloc(size_t count,size_t size) {return allocation_attempts++==fail_at?NULL:xr_calloc(count,size);}
static void *display_realloc(void *p,size_t size) {return allocation_attempts++==fail_at?NULL:xr_realloc(p,size);}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#define xr_malloc(size) display_malloc(size)
#define xr_calloc(count,size) display_calloc(count,size)
#define xr_realloc(p,size) display_realloc(p,size)
#include "xir/xxir_value.c"
static uint64_t held(XrCompileResources *resources) {
    XrCompileResourceStats out={0};CHECK(xr_compile_resources_stats(resources,&out)==XR_COMPILE_RESOURCE_OK);return out.held_bytes;
}
static unsigned polls,mode;
static bool interrupted(const void *context) {
    (void)context;++polls;if(polls==3 && mode==2)observed_now=200;
    if(polls==3 && mode==3)observed_now=0;
    return polls>=3 && mode==1;
}
static void type_controls(XrXirCompileContext *context) {
    XrXirTypeNode nodes[130]={0};
    for(unsigned i=0;i<130;++i){nodes[i].kind=XR_XIR_TYPE_ARRAY;nodes[i].element=i==129?XR_XIR_I64:(XrXirType)(257+i);}
    XrXirTypes types={nodes,130,NULL,NULL};uint64_t frame=held(context->resources);
    const XrXirStatus expected[]={XR_XIR_CANCELLED,XR_XIR_DEADLINE,XR_XIR_CLOCK_FAILURE};
    for(mode=1;mode<=3;++mode) {
        polls=0;observed_now=1;XrCompileRequestControl control={0};
        XrCompileRequestOptions options={interrupted,NULL,mode==1?XR_COMPILE_DEADLINE_NONE:XR_COMPILE_DEADLINE_AFTER,mode==1?0:100};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        CHECK(xir_display_type(context,&types,(XrXirType)256,&control)==expected[mode-1]);
        CHECK(held(context->resources)==frame && polls>=3);
        CHECK(xir_display_type(context,&types,(XrXirType)256,&control)==expected[mode-1]);
        observed_now=1;XrCompileRequestControl fresh={0};CHECK(xr_compile_request_control_init(&(XrCompileRequestOptions){0},&fresh)==XR_COMPILE_RESOURCE_OK);
        CHECK(xir_display_type(context,&types,(XrXirType)256,&fresh)==XR_XIR_OK && held(context->resources)==frame);
    }
    nodes[129].element=XR_XIR_BUFFER_STORAGE;
    CHECK(xir_display_type(context,&types,(XrXirType)256,NULL)==XR_XIR_BAD_TYPE && held(context->resources)==frame);
    nodes[129].element=XR_XIR_I64;uint64_t limit=context->limits.frame_bytes;context->limits.frame_bytes=1;
    CHECK(xir_display_type(context,&types,(XrXirType)256,NULL)==XR_XIR_BUDGET && held(context->resources)==frame);context->limits.frame_bytes=limit;
}
static void exact_text(const XrXirValue *value,const char *expected,size_t size) {
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(value,&bytes,&length));CHECK(length==size&&!memcmp(bytes,expected,size));
}
static XrXirValueAdmission admission(XrXirDomain *domain,XrXirTypeArena *arena) {
    return (XrXirValueAdmission){arena,domain,NULL,NULL,UINT64_MAX,1048576};
}
static void text_failures(XrXirDomain *domain,XrXirTypeArena *arena,const XrXirValue *array) {
    XrXirValueAdmission receive=admission(domain,arena);XrXirValue output={0};
    uint64_t baseline=xr_xir_domain_stats(domain).live_bytes;size_t before=allocation_attempts;
    CHECK(xr_xir_value_display_text(array,&receive,&output)==XR_XIR_VALUE_OK);size_t sites=allocation_attempts-before;
    CHECK(sites>=3);exact_text(&output,"[1, 2]",6);xr_xir_value_drop(&output);
    /* Exercise temporary-buffer failure and a later String-publication failure. */
    const size_t offsets[]={0,sites-1};
    for(unsigned i=0;i<2;++i){receive=admission(domain,arena);fail_at=allocation_attempts+offsets[i];
        CHECK(xr_xir_value_display_text(array,&receive,&output)==XR_XIR_VALUE_OOM && !output.type && !output.payload);fail_at=SIZE_MAX;
        CHECK(xr_xir_domain_stats(domain).live_bytes==baseline);receive=admission(domain,arena);
        CHECK(xr_xir_value_display_text(array,&receive,&output)==XR_XIR_VALUE_OK);exact_text(&output,"[1, 2]",6);xr_xir_value_drop(&output);}
    receive=admission(domain,arena);receive.work=1;
    CHECK(xr_xir_value_display_text(array,&receive,&output)==XR_XIR_VALUE_LIMIT && !output.type && !output.payload);
    CHECK(xr_xir_domain_stats(domain).live_bytes==baseline);
    XrXirValue sentinel={XR_XIR_I64,0,42};receive=admission(domain,arena);
    CHECK(xr_xir_value_display_text(array,&receive,&sentinel)==XR_XIR_VALUE_BAD_ARGUMENT && sentinel.type==XR_XIR_I64 && sentinel.payload==42);
    XrXirValue forged=*array;forged.type=257;receive=admission(domain,arena);
    CHECK(xr_xir_value_display_text(&forged,&receive,&output)==XR_XIR_VALUE_BAD_ARGUMENT && !output.type && !output.payload);
    CHECK(xr_xir_domain_stats(domain).live_bytes==baseline);
}
static void finite_values(XrXirDomain *domain,XrXirTypeArena *arena) {
    XrXirValueAdmission receive=admission(domain,arena);XrXirValue fill={XR_XIR_I64,0,1},large={0},output={0};
    CHECK(xr_xir_array_repeat((XrXirType)256,34,&fill,&receive,&large)==XR_XIR_VALUE_OK);
    receive=admission(domain,arena);CHECK(xr_xir_value_display_text(&large,&receive,&output)==XR_XIR_VALUE_OK);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&output,&bytes,&length));
    CHECK(length==109 && !memcmp(bytes+length-14,", ...(2 more)]",14));xr_xir_value_drop(&output);xr_xir_value_drop(&large);
    const char raw[]="A\0\xe4\xb8\xad\"";XrXirValue text={0},array={0};CHECK(xr_xir_string_new(domain,raw,sizeof(raw)-1,&text)==XR_XIR_VALUE_OK);
    receive=admission(domain,arena);CHECK(xr_xir_array_new((XrXirType)257,&text,1,&receive,&array)==XR_XIR_VALUE_OK);xr_xir_value_drop(&text);
    receive=admission(domain,arena);CHECK(xr_xir_value_display_text(&array,&receive,&output)==XR_XIR_VALUE_OK);
    const char expected[]="[\"A\0\xe4\xb8\xad\"\"]";exact_text(&output,expected,sizeof(expected)-1);xr_xir_value_drop(&output);xr_xir_value_drop(&array);
    XrXirValue deep={XR_XIR_I64,0,7};
    for(unsigned i=0;i<5;++i){XrXirValue next={0};receive=admission(domain,arena);
        CHECK(xr_xir_array_new((XrXirType)(i?257+i:256),&deep,1,&receive,&next)==XR_XIR_VALUE_OK);xr_xir_value_drop(&deep);deep=next;}
    receive=admission(domain,arena);CHECK(xr_xir_value_display_text(&deep,&receive,&output)==XR_XIR_VALUE_OK);
    exact_text(&output,"[[[[...]]]]",11);xr_xir_value_drop(&output);xr_xir_value_drop(&deep);
}
static void received_nullable(XrXirCompileContext *context) {
    uint64_t frame=held(context->resources);
    XrXirTypeNode original_nodes[]={
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64}};
    XrXirTypeNode received_nodes[]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)258},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)259},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)260}};
    XrXirTypes original={original_nodes,2,NULL,NULL},received={received_nodes,6,NULL,NULL};
    XrXirTypeArena *a=NULL,*b=NULL;
    CHECK(xr_xir_compile_type_arena_new(context,&original,&a)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(context,&received,&b)==XR_XIR_VALUE_OK);
    const XirTypeReceivingRoot roots[]={{(XrXirType)257,(XrXirType)256},{(XrXirType)256,(XrXirType)258}};
    CHECK(xir_compile_type_arena_receive(context,b,a,roots,2)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    uint64_t bytes=xr_xir_domain_stats(domain).live_bytes;XrXirValue values[]={{XR_XIR_I64,0,7},{XR_XIR_I64,0,8}},array={0},some={0},output={0};
    XrXirValueAdmission receive=admission(domain,a);
    CHECK(xr_xir_array_new((XrXirType)257,values,2,&receive,&array)==XR_XIR_VALUE_OK);
    receive=admission(domain,b);CHECK(xr_xir_nullable_new((XrXirType)257,&array,&receive,&some)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&array);receive=admission(domain,b);
    CHECK(xr_xir_value_display_text(&some,&receive,&output)==XR_XIR_VALUE_OK);exact_text(&output,"[7, 8]",6);
    xr_xir_value_drop(&output);xr_xir_value_drop(&some);
    receive=admission(domain,a);CHECK(xr_xir_nullable_new((XrXirType)256,&values[0],&receive,&some)==XR_XIR_VALUE_OK);
    for(unsigned i=0;i<3;++i){XrXirValue next={0};receive=admission(domain,b);
        CHECK(xr_xir_nullable_new((XrXirType)(259+i),&some,&receive,&next)==XR_XIR_VALUE_OK);xr_xir_value_drop(&some);some=next;}
    receive=admission(domain,b);CHECK(xr_xir_value_display_text(&some,&receive,&output)==XR_XIR_VALUE_OK);exact_text(&output,"7",1);
    xr_xir_value_drop(&output);xr_xir_value_drop(&some);CHECK(xr_xir_domain_stats(domain).live_bytes==bytes);
    xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(b);xr_xir_compile_type_arena_drop(a);CHECK(held(context->resources)==frame);
}
int main(void) {
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&(XrCompileResourceLimits){XR_COMPILE_MEMORY_ORDINARY,0,true},&context.resources)==XR_COMPILE_RESOURCE_OK);
    uint64_t frame=held(context.resources);type_controls(&context);received_nullable(&context);
    XrXirTypeNode nodes[6]={0};nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_STRING};
    for(unsigned i=2;i<6;++i)nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)(i==2?256:255+i)};
    XrXirTypes types={nodes,6,NULL,NULL};XrXirTypeArena *arena=NULL;CHECK(xr_xir_compile_type_arena_new(&context,&types,&arena)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);uint64_t bytes=xr_xir_domain_stats(domain).live_bytes;
    XrXirValueAdmission receive=admission(domain,arena);XrXirValue values[]={{XR_XIR_I64,0,1},{XR_XIR_I64,0,2}},array={0};
    CHECK(xr_xir_array_new((XrXirType)256,values,2,&receive,&array)==XR_XIR_VALUE_OK);text_failures(domain,arena,&array);xr_xir_value_drop(&array);
    finite_values(domain,arena);CHECK(xr_xir_domain_stats(domain).live_bytes==bytes);xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    CHECK(held(context.resources)==frame);xr_compile_resources_release(context.resources);
    puts("Typed display finite limits, forged type, actual OOM/work and request recovery PASS");return 0;
}
