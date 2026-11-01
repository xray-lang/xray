/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_call_select.c - One owned wait around a prepared typed selection
 *
 * KEY CONCEPT:
 *   A real callback parks on several channels, then observes only its committed
 *   tagged result. Failure and cancellation leave the buffered values owned.
 */
#include "xir/xxir_program_internal.h"
#include "xir/xxir_call_select_internal.h"
#include "base/xresource_limits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"select Call %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_runtime_allocations.h"
#include "xir_native_metadata_fixture.h"
enum { SELECT_INNER_NULLABLE=256,SELECT_CHANNEL,SELECT_NULLABLE,SELECT_STRING_CHANNEL,
       SELECT_STRING_NULLABLE,SELECT_RESULT };
typedef struct SelectWitness {
    XrXirValue channels[2];uint64_t now;uint32_t mode,samples,steps,winner;
} SelectWitness;
static XrXirTimeStatus select_clock(void *opaque,XrXirClockKind kind,int64_t *out) {
    SelectWitness *w=opaque;CHECK(kind==XR_XIR_CLOCK_MONOTONIC);++w->samples;
    if(w->mode==5)return XR_XIR_TIME_FAILED;
    if(w->mode==4&&w->samples==2)w->now+=UINT64_C(1000000);
    *out=(int64_t)w->now;return XR_XIR_TIME_OK;
}
static XrXirTimeStatus select_offset(void *opaque,int64_t seconds,int64_t *out) {
    (void)opaque;(void)seconds;*out=0;return XR_XIR_TIME_OK;
}
static XrXirAction select_root(XrXirCallView *view) {
    SelectWitness *w=(SelectWitness *)view->environment;++w->steps;
    XrXirChannelSelectCallRequest request={.channels=w->channels,.count=2,.result_type=(XrXirType)SELECT_RESULT,
        .has_default=w->mode==1,.has_timeout=w->mode==2||w->mode==3||w->mode==4||w->mode==5,
        .timeout_ms=w->mode==4?1:100};
    if(!request.has_timeout)request.timeout_ms=0;
    size_t live=runtime_live,bytes=runtime_bytes;
    if(w->mode==6||w->mode==8)runtime_fail_at=runtime_attempts+(w->mode==6?3:4);
    XrXirValue result={0};XrXirCallStatus status=xr_xir_call_select_step(view,view->state,&request,&result);
    if(w->mode==6||w->mode==8) {
        CHECK(runtime_attempts==runtime_fail_at+1&&status==XR_XIR_CALL_OOM);
        CHECK(!result.type&&!result.payload&&runtime_live==live&&runtime_bytes==bytes);
    }
    runtime_fail_at=SIZE_MAX;
    if(status==XR_XIR_CALL_BUSY)return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};
    if(status==XR_XIR_CALL_SUSPENDED)return (XrXirAction){.kind=XR_XIR_ACTION_AWAIT_CHANNEL};
    if(status!=XR_XIR_CALL_READY)return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,status}};
    XrXirValue tag={0};CHECK(xr_xir_tuple_get(&result,0,&tag)==XR_XIR_VALUE_OK&&tag.type==XR_XIR_I64);
    CHECK(tag.payload>=0&&tag.payload<=2);w->winner=(uint32_t)tag.payload;
    for(uint32_t a=0;a<2;++a) {
        XrXirValue field={0};bool some=false;const XrXirValue *payload=NULL;
        CHECK(xr_xir_tuple_get(&result,a+1,&field)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_nullable_view(&field,&some,&payload)&&some==(a==w->winner));
        if(some) {
            CHECK(payload);
            if(w->mode==9) {
                const char *text=NULL;size_t length=0;
                CHECK(xr_xir_string_view(payload,&text,&length)&&length==2&&!memcmp(text,"42",2));
            } else if(w->mode==10)CHECK(!payload->type&&!payload->reserved&&!payload->payload);
            else if(w->mode==11) {
                bool inner=false;const XrXirValue *integer=NULL;
                CHECK(payload->type==SELECT_INNER_NULLABLE&&xr_xir_nullable_view(payload,&inner,&integer)&&
                    inner&&integer&&integer->type==XR_XIR_I64&&integer->payload==42);
            } else CHECK(payload->type==XR_XIR_I64&&payload->payload==42);
        }
        xr_xir_value_drop(&field);
    }
    xr_xir_value_drop(&tag);xr_xir_value_drop(&result);
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,42}};
}
static XrXirAction select_init(XrXirCallView *view) {
    (void)view;return (XrXirAction){.kind=XR_XIR_ACTION_RETURN};
}
static void select_send(XrXirValueAdmission *admission,const XrXirValue *channel) {
    XrXirValue value={XR_XIR_I64,0,42};XrXirChannelState sent=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_send_owned(channel,&value,admission,&sent)==XR_XIR_VALUE_OK&&sent==XR_XIR_CHANNEL_SENT);
}
static void select_case(uint32_t mode) {
    NativeFixtureOwner compiler={0};CHECK(native_fixture_owner_new(&compiler)==XR_XIR_OK);
    XrCompileResourceStats baseline=native_fixture_stats(&compiler.context);SelectWitness witness={.mode=mode,.now=1000000000};
    const XrXirCallEntry entries[]={
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_UNIT,.resume=select_init},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_I64,.state_bytes=sizeof(XrXirChannelSelectCallState),
         .resume=select_root,.environment=&witness}};
    XrXirType element=mode==10?XR_XIR_UNIT:mode==11?(XrXirType)SELECT_INNER_NULLABLE:XR_XIR_I64;
    XrXirType outer=mode==10||mode==11?(XrXirType)SELECT_NULLABLE:(XrXirType)SELECT_INNER_NULLABLE;
    const XrXirCallableParameter fields[]={{XR_XIR_I64,XR_PARAM_READ},{outer,XR_PARAM_READ},
        {mode==9?(XrXirType)SELECT_STRING_NULLABLE:outer,XR_PARAM_READ}};
    const XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=element},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=mode==11?(XrXirType)SELECT_INNER_NULLABLE:XR_XIR_UNIT},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=3}};
    const XrXirTypes types={nodes,6,NULL,NULL};
    const XrXirSourceModule source={"selection",9,NULL,0,0,0,XR_XIR_MODULE_FRESH_STORAGE};
    XrXirFunctionIdentity identities[]={{0},{.exported=1}};
    const XrXirDeclarations declarations={&source,1,identities,NULL,0,NULL,0,0,1,NULL};
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
        entries,2,&declarations,{0},&types,{0}};
    XrXirArtifact *artifact=NULL;CHECK(native_metadata_fixture(&compiler.context,&spec,&artifact)==XR_XIR_OK);
    XrXirProgramInput *input=NULL;CHECK(xir_program_input_take(&compiler.context,&artifact,&input)==XR_XIR_OK&&!artifact);
    XrXirProgram *program=NULL;CHECK(xir_program_receive(&compiler.context,&spec,&input,NULL,&program)==XR_XIR_OK&&!input);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(8388608,&domain)==XR_XIR_VALUE_OK);
    const XrXirDomainBudgetControls controls={8388608,8388608,XR_RESOURCE_SANDBOX_RUNTIME_WORK,4194304,4194304};
    CHECK(xr_xir_domain_budget_bind(domain,&controls)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={program->arena,domain,NULL,NULL,XR_RESOURCE_SANDBOX_RUNTIME_WORK,4194304};
    for(unsigned a=0;a<2;++a)CHECK(xr_xir_channel_new(mode==9&&a==1?
        (XrXirType)SELECT_STRING_CHANNEL:(XrXirType)SELECT_CHANNEL,2,&admission,&witness.channels[a])==XR_XIR_VALUE_OK);
    if(mode==0||mode==4||mode==6||mode==8)select_send(&admission,&witness.channels[0]);
    if(mode>=9) {
        XrXirValue message={0};XrXirChannelState sent=XR_XIR_CHANNEL_RETRY;
        if(mode==9)CHECK(xr_xir_string_new(domain,"42",2,&message)==XR_XIR_VALUE_OK);
        if(mode==11) {
            XrXirValue integer={XR_XIR_I64,0,42};
            CHECK(xr_xir_nullable_new((XrXirType)SELECT_INNER_NULLABLE,&integer,&admission,&message)==XR_XIR_VALUE_OK);
        }
        CHECK(xr_xir_channel_send_owned(&witness.channels[mode==9?1:0],&message,&admission,&sent)==XR_XIR_VALUE_OK&&
            sent==XR_XIR_CHANNEL_SENT&&!message.type&&!message.payload);
    }
    XrXirCallAccounting accounting={0};
    XrXirCallConfig config;CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.accounting=&accounting;
    config.entries=program->entries;config.entry_count=program->entry_count;config.byte_limit=8388608;config.poll_limit=10000;
    config.admission=admission;config.time=(XrXirTimeProvider){XR_XIR_CALL_ABI_VERSION,0,select_clock,select_offset,&witness};
    XrXirCall *call=NULL;XrXirCallStatus created=xr_xir_call_new(&config,1,NULL,0,&call);
    if(created!=XR_XIR_CALL_READY)fprintf(stderr,"select constructor mode%u status%u\n",mode,created);
    CHECK(created==XR_XIR_CALL_READY);
    XrXirCallResult result=xr_xir_call_poll_bounded(call,1);
    if(mode==2||mode==3||mode==7) {
        CHECK(result.status==XR_XIR_CALL_SUSPENDED&&xr_xir_call_channel_waiting(call));
        if(mode==2)witness.now+=100000000;
        if(mode==3)select_send(&admission,&witness.channels[1]);
        if(mode==7)CHECK(xr_xir_call_request_cancel(call)==XR_XIR_CALL_CANCEL_REQUESTED);
    }
    for(unsigned steps=0;result.status==XR_XIR_CALL_READY||result.status==XR_XIR_CALL_SUSPENDED;++steps) {
        CHECK(steps<100);result=xr_xir_call_poll_bounded(call,1);
    }
    fprintf(stderr,"select mode%u status%u steps%u winner%u samples%u\n",mode,result.status,witness.steps,witness.winner,witness.samples);
    CHECK(result.status==(mode==5?XR_XIR_CALL_HOST_ERROR:mode==6||mode==8?XR_XIR_CALL_OOM:mode==7?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_RETURNED));
    if(mode==1||mode==2)CHECK(witness.winner==2);
    if(mode==3)CHECK(witness.winner==1);
    if(mode==4)CHECK(witness.steps==2);
    if(mode>=9)CHECK(witness.winner==(mode==9?1u:0u));
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);
    if(mode==6||mode==8) {
        XrXirValue untouched={0};XrXirChannelState received=XR_XIR_CHANNEL_RETRY;
        CHECK(xr_xir_channel_receive(&witness.channels[0],&admission,&untouched,&received)==XR_XIR_VALUE_OK&&
            received==XR_XIR_CHANNEL_VALUE&&untouched.type==XR_XIR_I64&&untouched.payload==42);
        xr_xir_value_drop(&untouched);
    }
    for(unsigned a=0;a<2;++a)xr_xir_value_drop(&witness.channels[a]);
    xr_xir_domain_drop(domain);xr_xir_compile_program_drop(program);
    CHECK(native_fixture_stats(&compiler.context).held_bytes==baseline.held_bytes);native_fixture_owner_free(&compiler);
    CHECK(!runtime_live&&!runtime_bytes);
}
int main(void) {for(uint32_t mode=0;mode<12;++mode)select_case(mode);return 0;}
