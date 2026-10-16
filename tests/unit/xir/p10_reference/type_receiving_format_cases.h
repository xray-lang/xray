/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * type_receiving_format_cases.h - Display actual nested carriers across arenas
 */
#ifndef TYPE_RECEIVING_FORMAT_CASES_H
#define TYPE_RECEIVING_FORMAT_CASES_H
#include "xir/xxir_format.h"
#include "xir/xxir_error.h"
#include "../xir_error_fixture.h"
typedef struct ReceivingDisplay { char bytes[64]; size_t size; } ReceivingDisplay;
static int receiving_display_write(void *context,const void *bytes,size_t size) {
    ReceivingDisplay *display=context;
    CHECK(size<=sizeof(display->bytes)-display->size);
    memcpy(display->bytes+display->size,bytes,size);display->size+=size;return 1;
}
static XrXirTypeArena *receiving_display_arena(SourceFixtureOwner *compiler,unsigned shift) {
    XrXirTypeNode nodes[4]={0};CHECK(shift<=1);
    if (shift) nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL};
    for (unsigned i=0;i<3;++i)
        nodes[shift+i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=i ? type(shift+i-1) : XR_XIR_I64};
    XrXirTypes pool={nodes,shift+3,NULL,NULL};XrXirTypeArena *result=NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pool,&result)==XR_XIR_VALUE_OK);return result;
}
static void receiving_display_check(const XrXirValue *value,const char *expected,
    SourceFixtureOwner *compiler,XrXirDomain *domain) {
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(compiler->context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirDomainStats runtime=xr_xir_domain_stats(domain);
    size_t attempts=instance_compile_attempts;
    XrXirFormatContext context={0};ReceivingDisplay display={0};
    CHECK(xr_value_format_value(xr_xir_value_format_reader(&context),(XrValueFormatNode){value,0},
        (XrValueFormatSink){&display,receiving_display_write},0));
    CHECK(display.size==strlen(expected) && !memcmp(display.bytes,expected,display.size));
    CHECK(xr_compile_resources_stats(compiler->context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes && before.work==after.work);
    XrXirDomainStats current=xr_xir_domain_stats(domain);
    CHECK(current.live_bytes==runtime.live_bytes && current.allocations==runtime.allocations &&
        current.frees==runtime.frees && current.reallocations==runtime.reallocations && instance_compile_attempts==attempts);
}
static void receiving_display_cases(SourceFixtureOwner *compiler) {
    XrXirTypeArena *source=receiving_display_arena(compiler,0),*target=receiving_display_arena(compiler,1);
    XirTypeReceivingRoot root={type(2),type(3)};
    CHECK(xir_compile_type_arena_receive(&compiler->context,target,source,&root,1)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);
    uint64_t baseline=xr_xir_domain_stats(domain).live_bytes;
    XrXirValueAdmission birth={source,domain,NULL,NULL,UINT64_C(4000000),128*1024},received=birth;
    received.arena=target;
    XrXirValue values[]={{XR_XIR_I64,0,41},{XR_XIR_I64,0,42}},old[3]={{0}},fresh[3]={{0}};
    CHECK(xr_xir_array_new(type(0),values,2,&birth,&old[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new(type(1),&old[0],1,&birth,&old[1])==XR_XIR_VALUE_OK);
    XrXirTypeArena *unrelated=receiving_display_arena(compiler,1);
    XrXirValueAdmission lookalike=received;lookalike.arena=unrelated;
    XrXirValue rejected={0};
    CHECK(xr_xir_array_new(type(3),&old[1],1,&lookalike,&rejected)==XR_XIR_VALUE_BAD_ARGUMENT &&
        !rejected.type && !rejected.payload);
    xr_xir_compile_type_arena_drop(unrelated);
    CHECK(xr_xir_array_new(type(3),&old[1],1,&received,&fresh[2])==XR_XIR_VALUE_OK);
    XrXirValue copied={0},child={0};XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_reserve(&fresh[2],32,&received,&copied)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_get(&copied,0,&received,&child,&fault)==XR_XIR_VALUE_OK &&
        child.type==(uint32_t)type(1) && xr_xir_value_arena(&child)==source);
    xr_xir_value_drop(&child);
    values[0].payload=51;values[1].payload=52;
    CHECK(xr_xir_array_new(type(1),values,2,&received,&fresh[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new(type(2),&fresh[0],1,&received,&fresh[1])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new(type(2),&fresh[1],1,&birth,&old[2])==XR_XIR_VALUE_OK);
    CHECK(old[2].type==(uint32_t)type(2) && xr_xir_value_arena(&old[2])==source);
    CHECK(fresh[2].type==(uint32_t)type(3) && xr_xir_value_arena(&fresh[2])==target);
    for (unsigned i=0;i<2;++i) {xr_xir_value_drop(&old[i]);xr_xir_value_drop(&fresh[i]);}
    xr_xir_compile_type_arena_drop(source);xr_xir_compile_type_arena_drop(target);
    receiving_display_check(&fresh[2],"[[[41, 42]]]",compiler,domain);
    receiving_display_check(&copied,"[[[41, 42]]]",compiler,domain);
    receiving_display_check(&old[2],"[[[51, 52]]]",compiler,domain);
    xr_xir_value_drop(&old[2]);xr_xir_value_drop(&fresh[2]);xr_xir_value_drop(&copied);
    CHECK(xr_xir_domain_stats(domain).live_bytes==baseline);xr_xir_domain_drop(domain);
    puts("Nested Array display keeps actual child type/arena across received indices in both directions after external arena death; no display allocation or work");
}
static void receiving_error_storage_cases(SourceFixtureOwner *compiler) {
    ErrorFixture fixture;error_fixture_init(&fixture,true);
    XrXirTypeNode nodes[]={fixture.node,{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_ERROR}};
    XrXirTypes types={nodes,2,&fixture.table,NULL};XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&types,&arena)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);
    uint64_t baseline=xr_xir_domain_stats(domain).live_bytes;
    XrXirValueAdmission admission=error_fixture_admission(domain,arena);
    XrXirValue concrete=error_fixture_code(&admission,42),error={0},borrowed={0},array={0},copy={0},read={0};
    CHECK(xr_xir_error_erase(&concrete,&admission,&error)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_from_payload(arena,XR_XIR_ERROR,error.payload,&borrowed) &&
        borrowed.type==XR_XIR_ERROR && borrowed.payload==error.payload);
    CHECK(xr_xir_array_new(type(1),&error,1,&admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_reserve(&array,32,&admission,&copy)==XR_XIR_VALUE_OK);
    XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_get(&copy,0,&admission,&read,&fault)==XR_XIR_VALUE_OK && read.type==XR_XIR_ERROR);
    XrXirValue narrowed={0};CHECK(xr_xir_error_narrow(&read,type(0),&admission,&narrowed)==XR_XIR_VALUE_OK);
    CHECK(error_fixture_is_code(&narrowed,domain,42));
    xr_xir_value_drop(&narrowed);xr_xir_value_drop(&read);xr_xir_value_drop(&copy);
    xr_xir_value_drop(&array);xr_xir_value_drop(&error);xr_xir_value_drop(&concrete);
    CHECK(xr_xir_domain_stats(domain).live_bytes==baseline);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);
    puts("Canonical erased Error remains an Error view through actual payload recovery, compact copy, unpack and release; concrete enum42 preserved");
}
static bool receiving_storage_trial(SourceFixtureOwner *compiler,size_t ordinal,size_t *sites) {
    CHECK(!runtime_live && !runtime_bytes);
    XrXirTypeArena *source=receiving_display_arena(compiler,0),*target=receiving_display_arena(compiler,1);
    XirTypeReceivingRoot root={type(2),type(3)};
    CHECK(xir_compile_type_arena_receive(&compiler->context,target,source,&root,1)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={source,domain,NULL,NULL,UINT64_C(4000000),128*1024};
    XrXirValue values[]={{XR_XIR_I64,0,41},{XR_XIR_I64,0,42}},inner={0},old={0},fresh={0},copy={0},read={0};
    CHECK(xr_xir_array_new(type(0),values,2,&admission,&inner)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new(type(1),&inner,1,&admission,&old)==XR_XIR_VALUE_OK);
    admission.arena=target;runtime_attempts=0;runtime_fail_at=ordinal;
    XrXirValueStatus status=xr_xir_array_new(type(3),&old,1,&admission,&fresh);
    if (status==XR_XIR_VALUE_OK) status=xr_xir_array_reserve(&fresh,32,&admission,&copy);
    if (status==XR_XIR_VALUE_OK) {
        XrXirFaultDetail fault={0};status=xr_xir_array_get(&copy,0,&admission,&read,&fault);
    }
    *sites=runtime_attempts;runtime_fail_at=SIZE_MAX;
    CHECK(status==XR_XIR_VALUE_OK || status==XR_XIR_VALUE_OOM);
    receiving_display_check(&old,"[[41, 42]]",compiler,domain);
    if (fresh.type) receiving_display_check(&fresh,"[[[41, 42]]]",compiler,domain);
    if (copy.type) receiving_display_check(&copy,"[[[41, 42]]]",compiler,domain);
    if (status==XR_XIR_VALUE_OK) CHECK(read.type==(uint32_t)type(1) && xr_xir_value_arena(&read)==source);
    xr_xir_value_drop(&read);xr_xir_value_drop(&copy);xr_xir_value_drop(&fresh);
    xr_xir_value_drop(&old);xr_xir_value_drop(&inner);
    xr_xir_compile_type_arena_drop(target);xr_xir_compile_type_arena_drop(source);xr_xir_domain_drop(domain);
    CHECK(!runtime_live && !runtime_bytes);return status==XR_XIR_VALUE_OK;
}
static void receiving_storage_faults(SourceFixtureOwner *compiler) {
    size_t sites=0;CHECK(receiving_storage_trial(compiler,SIZE_MAX,&sites) && sites);
    for (size_t ordinal=0;ordinal<sites;++ordinal) {
        size_t attempted=0;CHECK(!receiving_storage_trial(compiler,ordinal,&attempted) && attempted>ordinal);
    }
    size_t retry=0;CHECK(receiving_storage_trial(compiler,SIZE_MAX,&retry) && retry==sites);
    printf("Received Array compact construction/reserve/unpack runtime sites=%zu swept=%zu old41/42 preserved retry exact compiler/runtime physical0\n",sites,sites);
}
#endif // TYPE_RECEIVING_FORMAT_CASES_H
