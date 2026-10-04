/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_output_status.c - Independent output channels and early ABI refusal
 *
 * KEY CONCEPT:
 *   A resource failure cannot be swallowed by a language write or panic handler.
 */
#include "xir/xxir_output.h"
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_runtime_allocations.h"
static const XrXirOutputStatus provider_statuses[]={XR_XIR_OUTPUT_OK,XR_XIR_OUTPUT_OOM,XR_XIR_OUTPUT_LIMIT,
    XR_XIR_OUTPUT_ERROR,XR_XIR_OUTPUT_BAD_ARGUMENT,XR_XIR_OUTPUT_BAD_ABI,
    (XrXirOutputStatus)-1,(XrXirOutputStatus)6,(XrXirOutputStatus)INT32_MAX};
typedef struct OutputState {
    XrXirCall *call;
    XrXirOutputStatus status;
    XrXirValue argument;
    unsigned callbacks, releases;
    bool writing, cancel;
} OutputState;
static XrXirOutputStatus group_status(void *context,const XrXirOutputGroup *group) {
    OutputState *state=context;
    CHECK(group && group->count==1 && group->stream==XR_XIR_STDOUT && !group->line);
    CHECK(group->values[0].type==XR_XIR_STRING);
    ++state->callbacks;
    if (state->cancel) CHECK(xr_xir_call_request_cancel(state->call)==XR_XIR_CALL_CANCEL_REQUESTED);
    return state->status;
}
static XrXirAction output_action(XrXirCallView *view) {
    OutputState *state=(OutputState *)view->environment;
    unsigned *phase=view->state;
    if ((*phase)++==0)
        return (XrXirAction){state->writing ? XR_XIR_ACTION_WRITE_STREAM : XR_XIR_ACTION_OUTPUT,
            XR_XIR_STDOUT,&state->argument,1,{0},{0},0};
    if (state->writing) {
        CHECK(view->inbox.status==XR_XIR_CALL_RETURNED && view->inbox.value.type==XR_XIR_BOOL);
        return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,view->inbox.value,{0},0};
    }
    return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,{XR_XIR_BOOL,0,1},{0},0};
}
static void output_release(XrXirCallView *view,XrXirCallStatus reason) {
    OutputState *state=(OutputState *)view->environment;(void)reason;++state->releases;
}
static void config_guards(void) {
    XrXirCallConfig call,before;memset(&call,0xa5,sizeof(call));before=call;
    CHECK(xr_xir_call_config_init(NULL,sizeof(call))==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_call_config_init(&call,sizeof(call)-1)==XR_XIR_CALL_BAD_ABI && !memcmp(&call,&before,sizeof(call)));
    CHECK(xr_xir_call_config_init(&call,sizeof(call)+1)==XR_XIR_CALL_BAD_ABI && !memcmp(&call,&before,sizeof(call)));
    CHECK(xr_xir_call_config_init(&call,sizeof(call))==XR_XIR_CALL_READY);
    CHECK(call.abi_version==25 && call.struct_size==sizeof(call) && !call.output.write && !call.output.abi_version);
    CHECK(call.byte_limit==16777216 && call.poll_limit==1000000 && call.depth_limit==4096);
    CHECK(!call.entries && !call.entry_count && !call.instance && !call.accounting && !call.output.reserved && !call.output.context);
    XrXirInstanceConfig instance,saved;memset(&instance,0x5a,sizeof(instance));saved=instance;
    CHECK(xr_xir_instance_config_init(NULL,sizeof(instance))==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_config_init(&instance,sizeof(instance)-1)==XR_XIR_CALL_BAD_ABI && !memcmp(&instance,&saved,sizeof(instance)));
    CHECK(xr_xir_instance_config_init(&instance,sizeof(instance)+1)==XR_XIR_CALL_BAD_ABI && !memcmp(&instance,&saved,sizeof(instance)));
    CHECK(xr_xir_instance_config_init(&instance,sizeof(instance))==XR_XIR_CALL_READY);
    CHECK(instance.abi_version==25 && instance.struct_size==sizeof(instance));
    CHECK(instance.metadata_limit==16777216 && instance.value_limit==16777216 && instance.call_limit==16777216);
    CHECK(instance.poll_limit==1000000 && instance.depth_limit==4096 && !instance.trace && !instance.trace_context);
    CHECK(!instance.output.abi_version && !instance.output.reserved && !instance.output.write && !instance.output.context);
    CHECK(!instance.time.abi_version && !instance.time.reserved && !instance.time.clock &&
        !instance.time.utc_offset && !instance.time.context);
    struct {uint32_t version,size;} prefix={20,8};XrXirCall *activation=(XrXirCall *)(uintptr_t)1;
    runtime_attempts=0;
    CHECK(xr_xir_call_new((const XrXirCallConfig *)&prefix,0,NULL,0,&activation)==XR_XIR_CALL_BAD_ABI);
    CHECK(!activation && !runtime_attempts && !runtime_live && !runtime_bytes);
    prefix.version=25;
    CHECK(xr_xir_call_new((const XrXirCallConfig *)&prefix,0,NULL,0,&activation)==XR_XIR_CALL_BAD_ABI && !runtime_attempts);
    CHECK(xr_xir_call_new(NULL,0,NULL,0,NULL)==XR_XIR_CALL_BAD_ARGUMENT);
    XrXirInstance *owned=(XrXirInstance *)(uintptr_t)1;
    prefix.version=20;
    CHECK(xr_xir_instance_new((XrXirProgram *)(uintptr_t)1,(const XrXirInstanceConfig *)&prefix,&owned)==XR_XIR_CALL_BAD_ABI);
    CHECK(!owned && !runtime_attempts);
    prefix.version=25;
    CHECK(xr_xir_instance_new((XrXirProgram *)(uintptr_t)1,(const XrXirInstanceConfig *)&prefix,&owned)==XR_XIR_CALL_BAD_ABI);
    for (unsigned mode=0;mode<4;++mode) {
        CHECK(xr_xir_instance_config_init(&instance,sizeof(instance))==XR_XIR_CALL_READY);
        if (mode==0) instance.output.abi_version=20;
        if (mode==1) instance.output.reserved=1;
        if (mode==2) instance.output.context=(void *)(uintptr_t)1;
        if (mode==3) instance.output.abi_version=25;
        CHECK(xr_xir_instance_new((XrXirProgram *)(uintptr_t)1,&instance,&owned)==XR_XIR_CALL_BAD_ABI);
        call.output=instance.output;
        CHECK(xr_xir_call_new(&call,0,NULL,0,&activation)==XR_XIR_CALL_BAD_ABI);
        CHECK(!owned && !activation && !runtime_attempts);
    }
    CHECK(xr_xir_instance_config_init(&instance,sizeof(instance))==XR_XIR_CALL_READY);
    instance.output=(XrXirOutputProvider){24,0,group_status,NULL};
    CHECK(xr_xir_instance_new((XrXirProgram *)(uintptr_t)1,&instance,&owned)==XR_XIR_CALL_BAD_ABI && !owned && !runtime_attempts);
    CHECK(xr_xir_call_config_init(&call,sizeof(call))==XR_XIR_CALL_READY);
    call.output=instance.output;
    CHECK(xr_xir_call_new(&call,0,NULL,0,&activation)==XR_XIR_CALL_BAD_ABI && !activation && !runtime_attempts);
    XrXirCallAccounting accounting={0};CHECK(xr_xir_call_config_init(&call,sizeof(call))==XR_XIR_CALL_READY);
    XrXirCallEntry old_entry={21,NULL,0,XR_XIR_BOOL,sizeof(unsigned),output_action,output_release,NULL,0,0};
    call.entries=&old_entry;call.entry_count=1;call.accounting=&accounting;
    static const uint32_t retired_versions[]={20,21,22,23,24};
    for (unsigned i=0;i<sizeof(retired_versions)/sizeof(retired_versions[0]);++i) {
        old_entry.abi_version=retired_versions[i];
        CHECK(xr_xir_call_new(&call,0,NULL,0,&activation)==XR_XIR_CALL_BAD_ABI && !activation && !runtime_attempts);
    }
}
static void call_channels(void) {
    static const XrXirCallStatus expected[]={XR_XIR_CALL_RETURNED,XR_XIR_CALL_OOM,XR_XIR_CALL_LIMIT,
        XR_XIR_CALL_OUTPUT_ERROR,XR_XIR_CALL_BAD_ARGUMENT,XR_XIR_CALL_BAD_ABI,
        XR_XIR_CALL_BAD_ARGUMENT,XR_XIR_CALL_BAD_ARGUMENT,XR_XIR_CALL_BAD_ARGUMENT};
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValue text={0};CHECK(xr_xir_string_new(domain,"A\0B",3,&text)==XR_XIR_VALUE_OK);
    size_t live=runtime_live,bytes=runtime_bytes;
    for (unsigned writing=0;writing<2;++writing) for (unsigned mode=0;mode<sizeof(provider_statuses)/sizeof(provider_statuses[0]);++mode) for (unsigned cancel=0;cancel<2;++cancel) {
        OutputState state={NULL,provider_statuses[mode],text,0,0,writing!=0,cancel!=0};
        XrXirCallEntry entry={XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_BOOL,sizeof(unsigned),output_action,output_release,&state,0,0};
        XrXirCallAccounting accounting={0};XrXirCallConfig config;
        CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.entries=&entry;config.entry_count=1;config.accounting=&accounting;
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,group_status,&state};
        CHECK(xr_xir_call_new(&config,0,NULL,0,&state.call)==XR_XIR_CALL_READY);
        XrXirCallResult result=xr_xir_call_poll_bounded(state.call, UINT64_MAX);
        XrXirCallStatus wanted=cancel ? XR_XIR_CALL_CANCELLED : writing && mode==3 ? XR_XIR_CALL_RETURNED : expected[mode];
        CHECK(result.status==wanted && state.callbacks==1 && state.releases==1);
        CHECK(!xr_xir_call_panic_status(result.status) && xr_xir_call_result_valid(&result));
        if (wanted==XR_XIR_CALL_RETURNED) CHECK(result.value.type==XR_XIR_BOOL && result.value.payload==(mode==0));
        else CHECK(result.value.type==XR_XIR_UNIT && !result.value.payload);
        CHECK(xr_xir_call_free(state.call)==XR_XIR_CALL_READY);
        CHECK(!accounting.live_bytes && accounting.allocations==accounting.frees);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    xr_xir_value_drop(&text);xr_xir_domain_drop(domain);CHECK(!runtime_live && !runtime_bytes);
}
typedef struct ByteState { unsigned calls;XrXirOutputStatus status; } ByteState;
static XrXirOutputStatus byte_status(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    ByteState *state=context;++state->calls;
    CHECK(stream==XR_XIR_STDOUT && length==4 && !memcmp(bytes,"A\0B\n",4));return state->status;
}
static void renderer_channels(void) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValue text={0};CHECK(xr_xir_string_new(domain,"A\0B",3,&text)==XR_XIR_VALUE_OK);
    XrXirOutputGroup group={XR_XIR_STDOUT,&text,1,true};ByteState bytes={0,XR_XIR_OUTPUT_OK};
    XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,byte_status,&bytes,4};
    size_t live=runtime_live,allocated=runtime_bytes;
    for (unsigned mode=0;mode<sizeof(provider_statuses)/sizeof(provider_statuses[0]);++mode) {
        bytes.status=provider_statuses[mode];
        CHECK(xr_xir_output_render(&sink,&group)==(mode<6 ? (XrXirOutputStatus)mode : XR_XIR_OUTPUT_BAD_ARGUMENT));
        CHECK(bytes.calls==mode+1 && runtime_live==live && runtime_bytes==allocated);
    }
    unsigned calls=bytes.calls;runtime_attempts=0;runtime_fail_at=0;
    CHECK(xr_xir_output_render(&sink,&group)==XR_XIR_OUTPUT_OOM);
    CHECK(bytes.calls==calls && runtime_attempts==1 && runtime_live==live && runtime_bytes==allocated);
    runtime_fail_at=SIZE_MAX;runtime_attempts=0;sink.byte_limit=3;
    CHECK(xr_xir_output_render(&sink,&group)==XR_XIR_OUTPUT_LIMIT && !runtime_attempts && bytes.calls==calls);
    sink.byte_limit=4;text.reserved=1;
    CHECK(xr_xir_output_render(&sink,&group)==XR_XIR_OUTPUT_BAD_ARGUMENT && !runtime_attempts);
    text.reserved=0;sink.reserved=1;
    CHECK(xr_xir_output_render(&sink,&group)==XR_XIR_OUTPUT_BAD_ABI && !runtime_attempts);
    sink.reserved=0;sink.abi_version=20;
    CHECK(xr_xir_output_render(&sink,&group)==XR_XIR_OUTPUT_BAD_ABI && !runtime_attempts);
    sink.abi_version=24;
    CHECK(xr_xir_output_render(&sink,&group)==XR_XIR_OUTPUT_BAD_ABI && !runtime_attempts && bytes.calls==calls);
    CHECK(xr_xir_output_render(NULL,&group)==XR_XIR_OUTPUT_BAD_ARGUMENT);
    xr_xir_value_drop(&text);xr_xir_domain_drop(domain);CHECK(!runtime_live && !runtime_bytes);
}
int main(void) {
    _Static_assert(sizeof(XrXirOutputProvider)==24 && sizeof(XrXirOutputSink)==32,"versioned output prefixes");
    _Static_assert(XR_XIR_CALL_ABI_VERSION==25 && XR_XIR_VALUE_ABI_VERSION==19 &&
        XR_XIR_PROGRAM_ABI_VERSION==28,"current execution admission versions");
    _Static_assert(sizeof(XrXirCallConfig)==136 && sizeof(XrXirInstanceConfig)==120,"exact config admission sizes");
    _Static_assert(offsetof(XrXirCallConfig,entries)==8 && offsetof(XrXirInstanceConfig,metadata_limit)==8 &&
        offsetof(XrXirOutputProvider,write)==8 && offsetof(XrXirOutputSink,write)==8,"ABI prefixes precede payload fields");
    _Static_assert(sizeof(XrXirProgramSpec)==96 && sizeof(XrXirValue)==16 && sizeof(XrXirCallEntry)==64 &&
        sizeof(XrXirAction)==88 && sizeof(XrXirCallResult)==72 && sizeof(XrXirCallView)==216,"unchanged provider payload layout");
    config_guards();renderer_channels();call_channels();
    printf("Call25 Provider24 Sink32 CallConfig136 InstanceConfig120; Value19/Program28; typed channels, cancel, physical PASS\n");
    return 0;
}
