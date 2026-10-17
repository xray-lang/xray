/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_detached.inc.c - Detached targets retain their real driver and host services
 */
#include "xdap_current_control.h"
static XrXirOutputStatus detached_output(void *context,const XrXirOutputGroup *group) {
    XdapDetached *target=context;
    if (!target) return XR_XIR_OUTPUT_BAD_ARGUMENT;
    if (target->adapter) {
        if (target->retain_output) {
            XrXirOutputStatus status=target->output.write(target->output.context,group);
            if (status!=XR_XIR_OUTPUT_OK) return status;
        }
        return xr_xir_output_render(&target->adapter->sink,group);
    }
    return target->output.write ? target->output.write(target->output.context,group) : XR_XIR_OUTPUT_BAD_ARGUMENT;
}
static XrXirStatus detached_clock(XdapDetached *target,uint64_t *output) {
    if (xr_compile_resources_work(target->resources,1)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
    int64_t now=-1;
    if (!target->time.clock || target->time.clock(target->time.context,XR_XIR_CLOCK_MONOTONIC,&now)!=XR_XIR_TIME_OK || now<0)
        return XR_XIR_IO;
    *output=(uint64_t)now;return XR_XIR_OK;
}
static XrXirStatus detached_wait_begin(XdapDetached *target,const XdapCurrentResult *result) {
    XrXirWaitRequest request={0};
    if (xdap_current_wait_request(target->launch->control,result->instance_epoch,result->outcome.wake,&request)!=XR_XIR_CALL_READY)
        return XR_XIR_BAD_STRUCTURE;
    if (request.reserved || request.subject || request.generation || request.ticket) return XR_XIR_BAD_STRUCTURE;
    uint64_t start=0,duration=0;XrXirStatus status=XR_XIR_OK;
    if (request.kind==XR_XIR_WAIT_TIMER_MS) {
        if (!request.after_ms || request.after_ms>XR_XIR_TIMER_MAX_MS) return XR_XIR_BAD_STRUCTURE;
        status=detached_clock(target,&start);duration=request.after_ms*UINT64_C(1000000);
    } else if (request.kind!=XR_XIR_WAIT_YIELD || request.after_ms) return XR_XIR_UNSUPPORTED;
    if (status!=XR_XIR_OK) return status;
    target->wait_epoch=result->instance_epoch;target->wait_wake=result->outcome.wake;
    target->wait_start=start;target->wait_duration=duration;return XR_XIR_OK;
}
static XrXirStatus detached_wait_resume(XdapDetached *target,bool *pending) {
    *pending=false;if (!target->wait_wake) return XR_XIR_OK;
    if (target->wait_duration) {
        uint64_t now=0;XrXirStatus status=detached_clock(target,&now);if (status!=XR_XIR_OK) return status;
        if (now<target->wait_start) return XR_XIR_IO;
        if (now-target->wait_start<target->wait_duration) { *pending=true;return XR_XIR_OK; }
    }
    if (xdap_current_resume(target->launch->control,target->wait_epoch,target->wait_wake)!=XR_XIR_CALL_READY)
        return XR_XIR_BAD_STRUCTURE;
    target->wait_wake=0;return XR_XIR_OK;
}
XR_FUNC bool xdap_detached_waiting(const XdapDetached *target) {
    return target && !target->adapter && !target->ended && target->wait_wake;
}
XR_FUNC XrXirValueStatus xdap_detached_result(const XdapDetached *target,XrXirCallResult *output) {
    if (!target || target->adapter || target->driving || !output || !xr_xir_call_result_empty(output))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    return target->ended ? xr_xir_call_result_copy(&target->outcome,output) : XR_XIR_VALUE_UNSUPPORTED;
}
XR_FUNC XrXirStatus xdap_detached_poll(XdapDetached *target,uint64_t quantum,bool *ended) {
    if (!target || target->adapter || target->driving || !target->launch || !quantum || !ended) return XR_XIR_BAD_STRUCTURE;
    if (target->ended) { *ended=true;return XR_XIR_OK; }
    target->driving=true;XdapCurrentResult result={0};bool pending=false;XrXirStatus status=XR_XIR_OK;
    if (!target->started) {
        XrXirCallStatus started=xdap_current_start(target->launch->control,target->launch->entry,NULL,0);
        if (started!=XR_XIR_CALL_READY) {
            status=started==XR_XIR_CALL_OOM ? XR_XIR_OUT_OF_MEMORY : started==XR_XIR_CALL_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
            goto done;
        }
        target->started=true;
    }
    status=detached_wait_resume(target,&pending);if (status!=XR_XIR_OK || pending) goto done;
    status=dispatch_value(xdap_current_poll(target->launch->control,quantum,&result));if (status!=XR_XIR_OK) goto done;
    if (result.state==XDAP_CURRENT_STOPPED) status=XR_XIR_BAD_STRUCTURE;
    else if (result.state==XDAP_CURRENT_WAITING) status=detached_wait_begin(target,&result);
    else if (result.state==XDAP_CURRENT_ENDED) {
        status=dispatch_value(xr_xir_call_result_copy(&result.outcome,&target->outcome));
        if (status==XR_XIR_OK) target->ended=true;
    }
done:
    if (status==XR_XIR_OK) *ended=target->ended;
    xdap_current_result_drop(&result);target->driving=false;return status;
}
XR_FUNC XrXirCallStatus xdap_detached_free(XdapDetached *target) {
    if (!target) return XR_XIR_CALL_READY;
    if (target->adapter) return XR_XIR_CALL_BAD_STATE;
    if (target->driving) return XR_XIR_CALL_BUSY;
    target->driving=true;XrXirCallStatus status=xdap_launch_free(target->launch);
    if (status==XR_XIR_CALL_BUSY) { target->driving=false;return status; }
    xr_xir_call_result_drop(&target->outcome);xr_compile_resources_free(target);return status;
}
static XrXirStatus dispatch_prepare_end(XdapAdapter *s,XdapJsonWriter *event) {
    *event=xdap_json_writer(s->resources,s->config.message_limit);
    XDAP_TEXT(event,"{\"seq\":");xdap_json_unsigned(event,s->seq+1);
    XDAP_TEXT(event,",\"type\":\"event\",\"event\":\"terminated\"}");return event->status;
}
static XrXirStatus dispatch_detach(XdapAdapter *s,const XdapRequest *r) {
    if (!s->execution || !s->config.detached || *s->config.detached)
        return xdap_dispatch_response(s,r,false,NULL,"Detached execution has no independent owner");
    if (s->seq>INT32_MAX-2) return XR_XIR_BUDGET;
    XdapDetached *target=NULL;
    XdapJsonWriter response={0},event={0};
    XrXirStatus status=xdap_dispatch_prepare_response(s,r,true,NULL,NULL,&response);
    if (status!=XR_XIR_OK) goto done;
    status=dispatch_prepare_end(s,&event);if (status!=XR_XIR_OK) goto done;
    XrXirCallStatus detached=xdap_current_detach(s->launch->control,s->config.detach_commit,s->config.detach_context);
    if (detached!=XR_XIR_CALL_READY) {
        if (detached==XR_XIR_CALL_HOST_ERROR) { status=XR_XIR_IO;goto done; }
        status=xdap_dispatch_response(s,r,false,NULL,"Target or host owner cannot detach");goto done;
    }
    target=s->execution;target->driving=true;
    target->adapter=NULL;target->launch=s->launch;target->started=s->configured;
    target->wait_epoch=s->wait_epoch;target->wait_wake=s->wait_wake;
    target->wait_start=s->wait_start;target->wait_duration=s->wait_duration;
    *s->config.detached=target;s->execution=NULL;s->launch=NULL;
    xdap_dispatch_clear_stop(s);s->running=false;s->wait_wake=0;s->closed=true;
    status=xdap_dispatch_send_prepared(s,&response);
    if (status==XR_XIR_OK) status=xdap_dispatch_send_prepared(s,&event);
done:
    if (target) target->driving=false;
    xdap_json_writer_free(&response);xdap_json_writer_free(&event);return status;
}
