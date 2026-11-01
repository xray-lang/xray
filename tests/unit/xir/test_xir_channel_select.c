/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_channel_select.c - Joint typed candidates and transactional receives
 *
 * KEY CONCEPT:
 *   Tests observe actual queue owners, failure allocation sites and lock races;
 *   random words and monotonic samples are explicit kernel inputs.
 */
#define main channel_select_reference_main
#include "test_xir_channel_results.c"
#undef main
#include "xir/xxir_channel_select_internal.h"

static void select_channels(ResultOwner *owner,XrXirValue *channels,bool mixed) {
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,2,&owner->admission,&channels[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_new((XrXirType)(mixed?RESULT_CHANNEL_STRING:RESULT_CHANNEL_I64),1,&owner->admission,&channels[1])==XR_XIR_VALUE_OK);
}
static XrXirChannelSelect *select_begin(ResultOwner *owner,XrXirValue *channels,uint32_t count,bool fallback,int64_t ms) {
    XrXirChannelSelectRequest request={.channels=channels,.count=count,.has_default=fallback,
        .has_timeout=ms>=0,.timeout_ms=ms>=0?ms:0,.now_ns=ms>0?UINT64_C(1000000000):0};
    XrXirChannelSelect *select=NULL;
    CHECK(xr_xir_channel_select_begin(&request,&owner->admission,&select)==XR_XIR_VALUE_OK&&select);return select;
}
static XrXirChannelSelectState select_probe(ResultOwner *owner,XrXirChannelSelect *select,uint64_t now,uint64_t entropy) {
    XrXirChannelSelectState state=XR_XIR_CHANNEL_SELECT_COMMITTED;
    CHECK(xr_xir_channel_select_probe(select,now,entropy,&owner->admission,&state)==XR_XIR_VALUE_OK);return state;
}
static XrXirValue select_take(ResultOwner *owner,XrXirChannelSelect *select,uint32_t expected,XrXirChannelSelectKind kind) {
    XrXirChannelSelectState state=XR_XIR_CHANNEL_SELECT_RETRY;
    CHECK(xr_xir_channel_select_commit(select,&owner->admission,&state)==XR_XIR_VALUE_OK&&state==XR_XIR_CHANNEL_SELECT_COMMITTED);
    XrXirValue value={0};uint32_t arm=0;XrXirChannelSelectKind actual=XR_XIR_CHANNEL_SELECT_TIMEOUT;
    CHECK(xr_xir_channel_select_take(select,&actual,&arm,&value)&&arm==expected&&actual==kind);
    CHECK(!xr_xir_channel_select_cancel(select));return value;
}
static void select_typed(void) {
    for(uint64_t choice=0;choice<2;++choice) {
        ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channels[2]={{0}},first=result_integer(7),second={0};
        select_channels(&owner,channels,true);CHECK(xr_xir_string_new(owner.domain,"nine",4,&second)==XR_XIR_VALUE_OK);
        int64_t string_identity=second.payload;result_send(&owner,&channels[0],&first);result_send(&owner,&channels[1],&second);
        XrXirChannelSelect *select=select_begin(&owner,channels,2,false,-1);
        CHECK(select_probe(&owner,select,0,choice)==XR_XIR_CHANNEL_SELECT_CANDIDATE);
        XrXirChannelSelectKind kind=XR_XIR_CHANNEL_SELECT_TIMEOUT;uint32_t arm=99;const XrXirValue *candidate=NULL;
        CHECK(xr_xir_channel_select_candidate(select,&kind,&arm,&candidate)&&arm==choice&&kind==XR_XIR_CHANNEL_SELECT_RECEIVE);
        CHECK(candidate->type==(uint32_t)(choice?XR_XIR_STRING:XR_XIR_I64));
        CHECK(select->arms[0].storage->buffer.count==1&&select->arms[1].storage->buffer.count==1);
        xr_xir_value_drop(&channels[0]);xr_xir_value_drop(&channels[1]);
        XrXirValue value=select_take(&owner,select,(uint32_t)choice,XR_XIR_CHANNEL_SELECT_RECEIVE);
        CHECK(value.type==(uint32_t)(choice?XR_XIR_STRING:XR_XIR_I64)&&value.payload==(choice?string_identity:7));
        CHECK(select->arms[choice].storage->buffer.count==0&&select->arms[1-choice].storage->buffer.count==1);
        xr_xir_value_drop(&value);xr_xir_channel_select_drop(select);result_owner_free(&owner);
    }
}
static void select_aliases(void) {
    for(uint64_t choice=0;choice<3;++choice) {
        ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channels[2]={{0}},a=result_integer(7),b=result_integer(9);
        select_channels(&owner,channels,false);result_send(&owner,&channels[0],&a);result_send(&owner,&channels[1],&b);
        XrXirValue aliases[]={channels[0],channels[0],channels[1]};
        XrXirChannelSelect *select=select_begin(&owner,aliases,3,false,-1);CHECK(select->unique_count==2);
        CHECK(select_probe(&owner,select,0,0)==XR_XIR_CHANNEL_SELECT_RETRY);
        CHECK(!xr_xir_channel_select_notified(select));
        CHECK(select_probe(&owner,select,0,choice+3)==XR_XIR_CHANNEL_SELECT_CANDIDATE);
        XrXirValue value=select_take(&owner,select,(uint32_t)choice,XR_XIR_CHANNEL_SELECT_RECEIVE);
        CHECK(value.payload==(choice==2?9:7));xr_xir_value_drop(&value);
        xr_xir_channel_select_drop(select);xr_xir_value_drop(&channels[0]);xr_xir_value_drop(&channels[1]);result_owner_free(&owner);
    }
}
static void select_changed(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channels[2]={{0}},a=result_integer(42),b=result_integer(9);
    select_channels(&owner,channels,false);result_send(&owner,&channels[0],&a);
    XrXirChannelSelect *select=select_begin(&owner,channels,2,false,-1);
    CHECK(select_probe(&owner,select,0,0)==XR_XIR_CHANNEL_SELECT_CANDIDATE);
    result_send(&owner,&channels[1],&b);XrXirChannelSelectState state=XR_XIR_CHANNEL_SELECT_PENDING;
    CHECK(xr_xir_channel_select_commit(select,&owner.admission,&state)==XR_XIR_VALUE_OK&&state==XR_XIR_CHANNEL_SELECT_RETRY);
    CHECK(select->arms[0].storage->buffer.count==1&&select->arms[1].storage->buffer.count==1);
    CHECK(select_probe(&owner,select,0,1)==XR_XIR_CHANNEL_SELECT_CANDIDATE);
    XrXirValue value=select_take(&owner,select,1,XR_XIR_CHANNEL_SELECT_RECEIVE);CHECK(value.payload==9);xr_xir_value_drop(&value);
    xr_xir_channel_select_drop(select);xr_xir_value_drop(&channels[0]);xr_xir_value_drop(&channels[1]);result_owner_free(&owner);
}
static ResultOwner *select_race_owner;static const XrXirValue *select_race_channel;
static void select_race_send(void) {XrXirValue message=result_integer(31);result_send(select_race_owner,select_race_channel,&message);}
static void select_notifications(bool race) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channels[2]={{0}};select_channels(&owner,channels,false);
    XrXirChannelSelect *select=select_begin(&owner,channels,2,false,-1);
    if(race) {
        select_race_owner=&owner;select_race_channel=&channels[0];channel_cas_target=&select->arms[0].storage->locked;
        channel_cas_count=0;channel_cas_pause_at=2;channel_cas_before=select_race_send;
        CHECK(select_probe(&owner,select,0,0)==XR_XIR_CHANNEL_SELECT_RETRY&&!channel_cas_before);channel_cas_target=NULL;
    } else {
        CHECK(select_probe(&owner,select,0,0)==XR_XIR_CHANNEL_SELECT_PENDING);
        CHECK(select->arms[0].storage->watch_count==1&&select->arms[1].storage->watch_count==1);
        XrXirValue message=result_integer(31);result_send(&owner,&channels[0],&message);
        CHECK(xr_xir_channel_select_notified(select));
    }
    CHECK(select_probe(&owner,select,0,0)==XR_XIR_CHANNEL_SELECT_CANDIDATE);
    XrXirValue value=select_take(&owner,select,0,XR_XIR_CHANNEL_SELECT_RECEIVE);CHECK(value.payload==31);xr_xir_value_drop(&value);
    CHECK(!select->arms[0].storage->watch_count&&!select->arms[1].storage->watch_count);
    xr_xir_channel_select_drop(select);xr_xir_value_drop(&channels[0]);xr_xir_value_drop(&channels[1]);result_owner_free(&owner);
    select_race_owner=NULL;select_race_channel=NULL;
}
static void select_default_timeout(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channels[2]={{0}};select_channels(&owner,channels,false);
    XrXirChannelSelect *select=select_begin(&owner,channels,2,true,-1);
    CHECK(select_probe(&owner,select,0,0)==XR_XIR_CHANNEL_SELECT_CANDIDATE);
    XrXirValue value=select_take(&owner,select,UINT32_MAX,XR_XIR_CHANNEL_SELECT_DEFAULT);CHECK(result_empty(&value));
    xr_xir_channel_select_drop(select);select=select_begin(&owner,channels,2,false,500);
    CHECK(select_probe(&owner,select,UINT64_C(1499000000),0)==XR_XIR_CHANNEL_SELECT_PENDING);
    XrXirChannelSelectState state=XR_XIR_CHANNEL_SELECT_COMMITTED;
    CHECK(xr_xir_channel_select_probe(select,UINT64_C(1400000000),0,&owner.admission,&state)==XR_XIR_VALUE_BAD_ARGUMENT&&state==XR_XIR_CHANNEL_SELECT_COMMITTED);
    CHECK(select_probe(&owner,select,UINT64_C(1500000000),0)==XR_XIR_CHANNEL_SELECT_CANDIDATE);
    value=select_take(&owner,select,UINT32_MAX,XR_XIR_CHANNEL_SELECT_TIMEOUT);CHECK(result_empty(&value));
    xr_xir_channel_select_drop(select);
    XrXirChannelState closed=XR_XIR_CHANNEL_RETRY;CHECK(xr_xir_channel_close(&channels[0],&owner.admission,&closed)==XR_XIR_VALUE_OK);
    select=select_begin(&owner,channels,2,false,1000);CHECK(select_probe(&owner,select,UINT64_C(1000000000),0)==XR_XIR_CHANNEL_SELECT_PENDING);
    CHECK(!select->arms[0].storage->watch_count&&select->arms[1].storage->watch_count==1);
    CHECK(xr_xir_channel_select_cancel(select)&&!xr_xir_channel_select_cancel(select)&&xr_xir_channel_select_notified(select));
    CHECK(select_probe(&owner,select,UINT64_C(1000000000),0)==XR_XIR_CHANNEL_SELECT_CANCELLED);
    xr_xir_channel_select_drop(select);xr_xir_value_drop(&channels[0]);xr_xir_value_drop(&channels[1]);result_owner_free(&owner);
}
static void select_failures(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channels[2]={{0}};select_channels(&owner,channels,false);
    size_t live=runtime_memory.live,bytes=runtime_memory.bytes;
    XrXirChannelSelectRequest request={.channels=channels,.count=2};XrXirChannelSelect *select=NULL;
    runtime_memory.fail_at=runtime_memory.attempts;
    CHECK(xr_xir_channel_select_begin(&request,&owner.admission,&select)==XR_XIR_VALUE_OOM&&!select);
    runtime_memory.fail_at=SIZE_MAX;CHECK(runtime_memory.live==live&&runtime_memory.bytes==bytes);
    select=select_begin(&owner,channels,2,false,-1);live=runtime_memory.live;bytes=runtime_memory.bytes;
    runtime_memory.fail_at=runtime_memory.attempts+1;XrXirChannelSelectState state=XR_XIR_CHANNEL_SELECT_COMMITTED;
    CHECK(xr_xir_channel_select_probe(select,0,0,&owner.admission,&state)==XR_XIR_VALUE_OOM&&state==XR_XIR_CHANNEL_SELECT_COMMITTED);
    runtime_memory.fail_at=SIZE_MAX;CHECK(runtime_memory.live==live&&runtime_memory.bytes==bytes);
    CHECK(!select->arms[0].storage->watch_count&&!select->arms[1].storage->watch_count);
    XrXirValue message=result_integer(42);result_send(&owner,&channels[0],&message);
    CHECK(select_probe(&owner,select,0,0)==XR_XIR_CHANNEL_SELECT_CANDIDATE);owner.admission.work=0;
    CHECK(xr_xir_channel_select_commit(select,&owner.admission,&state)==XR_XIR_VALUE_LIMIT&&state==XR_XIR_CHANNEL_SELECT_COMMITTED);
    CHECK(select->arms[0].storage->buffer.count==1);owner.admission.work=128000000;
    CHECK(select_probe(&owner,select,0,0)==XR_XIR_CHANNEL_SELECT_CANDIDATE);
    CHECK(xr_xir_channel_select_cancel(select));
    CHECK(xr_xir_channel_select_commit(select,&owner.admission,&state)==XR_XIR_VALUE_OK&&state==XR_XIR_CHANNEL_SELECT_CANCELLED);
    CHECK(select->arms[0].storage->buffer.count==1);xr_xir_channel_select_drop(select);select=NULL;
    request.has_default=true;request.has_timeout=true;
    CHECK(xr_xir_channel_select_begin(&request,&owner.admission,&select)==XR_XIR_VALUE_UNSUPPORTED);
    xr_xir_value_drop(&channels[0]);xr_xir_value_drop(&channels[1]);result_owner_free(&owner);
}
static void select_closed_zero(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channels[2]={{0}},message=result_integer(9);
    select_channels(&owner,channels,false);result_send(&owner,&channels[0],&message);XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_close(&channels[0],&owner.admission,&state)==XR_XIR_VALUE_OK);
    XrXirChannelSelect *select=select_begin(&owner,channels,2,false,0);
    CHECK(select_probe(&owner,select,0,0)==XR_XIR_CHANNEL_SELECT_CANDIDATE);
    XrXirValue value=select_take(&owner,select,0,XR_XIR_CHANNEL_SELECT_RECEIVE);CHECK(value.payload==9);xr_xir_value_drop(&value);
    xr_xir_channel_select_drop(select);select=select_begin(&owner,channels,2,false,0);
    CHECK(select_probe(&owner,select,0,0)==XR_XIR_CHANNEL_SELECT_CANDIDATE);
    value=select_take(&owner,select,UINT32_MAX,XR_XIR_CHANNEL_SELECT_TIMEOUT);CHECK(result_empty(&value));xr_xir_channel_select_drop(select);
    xr_xir_value_drop(&channels[1]);CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channels[1])==XR_XIR_VALUE_OK);
    XrXirChannelSelectRequest request={.channels=channels,.count=2};select=NULL;
    CHECK(xr_xir_channel_select_begin(&request,&owner.admission,&select)==XR_XIR_VALUE_UNSUPPORTED&&!select);
    xr_xir_value_drop(&channels[0]);xr_xir_value_drop(&channels[1]);result_owner_free(&owner);
}
int main(void) {
    select_typed();select_aliases();select_changed();select_notifications(false);select_notifications(true);
    select_default_timeout();select_failures();select_closed_zero();
    puts("Buffered Channel select: typed joint snapshots, arm-weighted choice, one commit, race recheck, deadlines and rollback PASS");return 0;
}
