/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_channel_values.c - Actual queue owners and failure publication barriers
 */
#include "base/xmalloc.h"
#include "base/xcompile_resources.h"
#include "xir/xxir_channel.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_array.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)

#include "xir_channel_value_observer.h"

enum { CH_I64=256, CH_STRING, CH_ARRAY_I64, CH_ARRAY, CH_UNIT, CH_NESTED };
typedef struct ChannelOwner {
    XrXirCompileContext context;
    XrXirTypeArena *arena;
    XrXirDomain *domain;
    XrXirValueAdmission admission;
} ChannelOwner;
static XrXirValue integer(int64_t number) { return (XrXirValue){XR_XIR_I64,0,number}; }
static bool empty(const XrXirValue *value) { return !value->type && !value->reserved && !value->payload; }
static void owner_free(ChannelOwner *owner) {
    xr_xir_domain_drop(owner->domain); xr_xir_compile_type_arena_drop(owner->arena);
    xr_compile_resources_release(owner->context.resources); *owner=(ChannelOwner){0};
    CHECK(!compiler_memory.live && !compiler_memory.bytes && !runtime_memory.live && !runtime_memory.bytes);
}
static XrXirValueStatus owner_new(ChannelOwner *owner) {
    XrCompileResourceLimits limits={8388608,4194304,128000000};
    owner->context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStatus resource=xr_compile_resources_new(&limits,&owner->context.resources);
    if (resource!=XR_COMPILE_RESOURCE_OK) return resource==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_VALUE_OOM : XR_XIR_VALUE_LIMIT;
    const XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_CHANNEL,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=(XrXirType)CH_ARRAY_I64},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=XR_XIR_UNIT},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=(XrXirType)CH_STRING}};
    XrXirTypes types={nodes,6,NULL,NULL};
    XrXirValueStatus status=xr_xir_compile_type_arena_new(&owner->context,&types,&owner->arena);
    if (status!=XR_XIR_VALUE_OK) return status;
    status=xr_xir_domain_new(8388608,&owner->domain);
    if (status==XR_XIR_VALUE_OK) owner->admission=(XrXirValueAdmission){.arena=owner->arena,
        .domain=owner->domain,.work=128000000,.scratch_bytes=4194304};
    return status;
}
static void owner_ready(ChannelOwner *owner) {
    CHECK(owner_new(owner)==XR_XIR_VALUE_OK);
    XrXirFunction function={0};
    XrXirModule module={.stage=XR_XIR_BUILT,.types=xr_xir_compile_type_arena_types(owner->arena),
        .functions=&function,.function_count=1};
    XrXirConstraint sendable={.markers=XR_XIR_CONSTRAINT_SENDABLE};
    for (uint32_t type=CH_I64;type<=CH_NESTED;++type)
        CHECK(xr_xir_compile_type_constraints(&owner->context,&module,0,(XrXirType)type,sendable)==XR_XIR_OK);
}
static void send_number(ChannelOwner *owner, const XrXirValue *channel, int64_t number, XrXirChannelState expected) {
    XrXirValue value=integer(number); XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_send_owned(channel,&value,&owner->admission,&state)==XR_XIR_VALUE_OK && state==expected);
    CHECK(expected==XR_XIR_CHANNEL_SENT ? empty(&value) : value.type==XR_XIR_I64 && value.payload==number);
}
static void receive_number(ChannelOwner *owner, const XrXirValue *channel, int64_t number, XrXirChannelState expected) {
    XrXirValue value={0}; XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_receive(channel,&owner->admission,&value,&state)==XR_XIR_VALUE_OK && state==expected);
    CHECK(expected==XR_XIR_CHANNEL_VALUE ? value.type==XR_XIR_I64 && value.payload==number : empty(&value));
    xr_xir_value_drop(&value);
}
static void queue_cases(void) {
    ChannelOwner owner={0}; owner_ready(&owner);
    XrXirValue channel={0},alias={0}; XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_new((XrXirType)CH_I64,2,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&channel,&alias)==XR_XIR_VALUE_OK && alias.payload==channel.payload);
    int64_t capacity=-1; bool closed=true;
    CHECK(xr_xir_channel_info(&alias,&owner.admission,&capacity,&closed)==XR_XIR_VALUE_OK && capacity==2 && !closed);
    receive_number(&owner,&channel,0,XR_XIR_CHANNEL_EMPTY);
    for (int64_t first=0;first<64;first+=2) {
        send_number(&owner,&channel,first,XR_XIR_CHANNEL_SENT);
        send_number(&owner,&alias,first+1,XR_XIR_CHANNEL_SENT);
        send_number(&owner,&channel,99,XR_XIR_CHANNEL_FULL);
        receive_number(&owner,&alias,first,XR_XIR_CHANNEL_VALUE);
        receive_number(&owner,&channel,first+1,XR_XIR_CHANNEL_VALUE);
    }
    send_number(&owner,&channel,73,XR_XIR_CHANNEL_SENT);
    CHECK(xr_xir_channel_close(&alias,&owner.admission,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_CLOSED);
    CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_CLOSED);
    send_number(&owner,&channel,99,XR_XIR_CHANNEL_CLOSED);
    receive_number(&owner,&alias,73,XR_XIR_CHANNEL_VALUE);
    receive_number(&owner,&channel,0,XR_XIR_CHANNEL_CLOSED);
    CHECK(xr_xir_channel_info(&alias,&owner.admission,&capacity,&closed)==XR_XIR_VALUE_OK && closed);
    xr_xir_value_drop(&channel); CHECK(xr_xir_value_valid(&alias)); xr_xir_value_drop(&alias);
    CHECK(xr_xir_channel_new((XrXirType)CH_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    /* This storage component has no waiter registration: zero capacity stores nothing. */
    send_number(&owner,&channel,1,XR_XIR_CHANNEL_FULL); receive_number(&owner,&channel,0,XR_XIR_CHANNEL_EMPTY);
    xr_xir_value_drop(&channel); owner_free(&owner);
}
static void ownership_cases(void) {
    ChannelOwner owner={0}; owner_ready(&owner);
    XrXirValue channel={0},array={0},snapshot={0},result={0}; XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    XrXirValue numbers[]={integer(11),integer(22)};
    CHECK(xr_xir_channel_new((XrXirType)CH_ARRAY,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)CH_ARRAY_I64,numbers,2,&owner.admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&array,&snapshot)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_send_owned(&channel,&snapshot,&owner.admission,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_SENT && empty(&snapshot));
    XrXirValue replacement=integer(99); XrXirFaultDetail fault={0};
    XrXirValuePlace place={(XrXirType)CH_ARRAY_I64,&array.payload};
    CHECK(xr_xir_array_set(&place,0,&replacement,&owner.admission,&fault)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_receive(&channel,&owner.admission,&result,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
    CHECK(xr_xir_array_get(&result,0,&owner.admission,&snapshot,&fault)==XR_XIR_VALUE_OK && snapshot.payload==11);
    xr_xir_value_drop(&snapshot); xr_xir_value_drop(&result); xr_xir_value_drop(&array); xr_xir_value_drop(&channel);
    XrXirValue inner={0},text={0};
    CHECK(xr_xir_channel_new((XrXirType)CH_STRING,1,&owner.admission,&inner)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_new((XrXirType)CH_NESTED,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(owner.domain,"queued",6,&text)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_send_owned(&inner,&text,&owner.admission,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_SENT);
    CHECK(xr_xir_channel_send_owned(&channel,&inner,&owner.admission,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_SENT);
    CHECK(empty(&inner) && empty(&text)); xr_xir_value_drop(&channel);
    CHECK(xr_xir_channel_new((XrXirType)CH_UNIT,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_send_owned(&channel,&text,&owner.admission,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_SENT);
    CHECK(xr_xir_channel_receive(&channel,&owner.admission,&text,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE && empty(&text));
    xr_xir_value_drop(&channel); owner_free(&owner);
}
static void rejection_cases(void) {
    ChannelOwner owner={0}; owner_ready(&owner);
    XrXirValue channel={0},value=integer(8),output={0}; XrXirChannelState state=XR_XIR_CHANNEL_SENT;
    CHECK(xr_xir_channel_new((XrXirType)CH_I64,-1,&owner.admission,&channel)==XR_XIR_VALUE_BOUNDS && empty(&channel));
    CHECK(xr_xir_channel_new((XrXirType)CH_I64,INT64_MAX,&owner.admission,&channel)==XR_XIR_VALUE_BOUNDS && empty(&channel));
    CHECK(xr_xir_channel_new(XR_XIR_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_BAD_ARGUMENT && empty(&channel));
    CHECK(xr_xir_channel_new((XrXirType)CH_I64,1,&owner.admission,&value)==XR_XIR_VALUE_BAD_ARGUMENT && value.payload==8);
    CHECK(xr_xir_channel_new((XrXirType)CH_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_receive(&channel,&owner.admission,&value,&state)==XR_XIR_VALUE_BAD_ARGUMENT && value.payload==8 && state==XR_XIR_CHANNEL_SENT);
    value=(XrXirValue){XR_XIR_BOOL,0,1};
    CHECK(xr_xir_channel_send_owned(&channel,&value,&owner.admission,&state)==XR_XIR_VALUE_BAD_ARGUMENT && value.type==XR_XIR_BOOL && state==XR_XIR_CHANNEL_SENT);
    XirChannel *storage=(XirChannel *)object_pointer(&channel); CHECK(channel_lock(storage));
    value=integer(8);
    CHECK(xr_xir_channel_send_owned(&channel,&value,&owner.admission,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_RETRY && value.payload==8);
    CHECK(xr_xir_channel_receive(&channel,&owner.admission,&output,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_RETRY && empty(&output));
    CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_RETRY);
    channel_unlock(storage); send_number(&owner,&channel,8,XR_XIR_CHANNEL_SENT);
    storage->sequence=UINT64_MAX;
    state=XR_XIR_CHANNEL_SENT;
    CHECK(xr_xir_channel_receive(&channel,&owner.admission,&output,&state)==XR_XIR_VALUE_LIMIT && empty(&output) && state==XR_XIR_CHANNEL_SENT);
    CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_LIMIT && state==XR_XIR_CHANNEL_SENT);
    storage->sequence=2; receive_number(&owner,&channel,8,XR_XIR_CHANNEL_VALUE);
    xr_xir_value_drop(&channel); owner_free(&owner);
}
static void escaped_owners(void) {
    ChannelOwner owner={0}; owner_ready(&owner);
    XrXirValue channel={0},text={0},alias={0}; XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_new((XrXirType)CH_STRING,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(owner.domain,"alive",5,&text)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_send_owned(&channel,&text,&owner.admission,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_SENT);
    xr_xir_compile_type_arena_drop(owner.arena); owner.arena=NULL;
    xr_compile_resources_release(owner.context.resources); owner.context.resources=NULL;
    xr_xir_domain_drop(owner.domain); owner.domain=NULL;
    CHECK(xr_xir_value_valid(&channel) && xr_xir_value_copy(&channel,&alias)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_receive(&alias,&owner.admission,&text,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
    const char *bytes=NULL; size_t length=0;
    CHECK(xr_xir_string_view(&text,&bytes,&length) && length==5 && !memcmp(bytes,"alive",5));
    xr_xir_value_drop(&text); xr_xir_value_drop(&channel); xr_xir_value_drop(&alias); owner_free(&owner);
}
static XrXirValueStatus resource_case(unsigned mode, size_t ordinal, size_t *sites) {
    CHECK(!runtime_memory.live && !compiler_memory.live);
    ChannelObserver *observer=mode ? &runtime_memory : &compiler_memory;
    size_t begin=observer->attempts;
    if (ordinal!=SIZE_MAX) observer->fail_at=begin+ordinal;
    ChannelOwner owner={0}; XrXirValue channel={0},array={0},output={0};
    XrXirValue numbers[]={integer(3),integer(5)}; XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    XrXirValueStatus status=owner_new(&owner);
    if (status==XR_XIR_VALUE_OK) status=xr_xir_channel_new((XrXirType)CH_ARRAY,2,&owner.admission,&channel);
    if (status==XR_XIR_VALUE_OK) status=xr_xir_array_new((XrXirType)CH_ARRAY_I64,numbers,2,&owner.admission,&array);
    if (status==XR_XIR_VALUE_OK) {
        status=xr_xir_channel_send_owned(&channel,&array,&owner.admission,&state);
        if (status==XR_XIR_VALUE_OK) CHECK(state==XR_XIR_CHANNEL_SENT && empty(&array));
        else CHECK(array.type==CH_ARRAY_I64 && state==XR_XIR_CHANNEL_RETRY);
    }
    if (status==XR_XIR_VALUE_OK) {
        state=XR_XIR_CHANNEL_RETRY;
        status=xr_xir_channel_receive(&channel,&owner.admission,&output,&state);
        if (status==XR_XIR_VALUE_OK) CHECK(state==XR_XIR_CHANNEL_VALUE && output.type==CH_ARRAY_I64);
        else CHECK(state==XR_XIR_CHANNEL_RETRY && empty(&output));
    }
    *sites=observer->attempts-begin; observer->fail_at=SIZE_MAX;
    if (ordinal!=SIZE_MAX) CHECK(*sites>ordinal && status==XR_XIR_VALUE_OOM);
    xr_xir_value_drop(&array); xr_xir_value_drop(&output); xr_xir_value_drop(&channel); owner_free(&owner);
    return status;
}
static void resource_cases(void) {
    for (unsigned mode=0;mode<2;++mode) {
        size_t sites=0; CHECK(resource_case(mode,SIZE_MAX,&sites)==XR_XIR_VALUE_OK && sites && sites<20000);
        size_t total=sites;
        for (size_t ordinal=0;ordinal<total;++ordinal) {
            CHECK(resource_case(mode,ordinal,&sites)==XR_XIR_VALUE_OOM);
            CHECK(resource_case(mode,SIZE_MAX,&sites)==XR_XIR_VALUE_OK);
        }
        printf("Channel queue actual allocator=%s ordinals=%zu physical=0 recovery=PASS\n",mode ? "runtime" : "compiler",total);
    }
}
#include "xir_channel_budget_cases.h"
#include "xir_channel_thread_cases.h"
int main(int argc, char **argv) {
    bool normal=argc==2 && !strcmp(argv[1],"--normal"); CHECK(argc==1 || normal);
    queue_cases(); ownership_cases(); rejection_cases(); escaped_owners(); local_work_cases(); thread_cases();
    if (!normal) resource_cases();
    puts("Channel value candidate PASS; Source/waits/wire/backends not covered"); return 0;
}
