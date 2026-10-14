/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_dispatch.c - Metered DAP requests and current Instance state transitions
 */
#include "xdap_dispatch_internal.h"
#include "../../xir/xxir_value_internal.h"
#include <string.h>
static XrXirStatus dispatch_value(XrXirValueStatus status) {
    return status==XR_XIR_VALUE_OK ? XR_XIR_OK : status==XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY :
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
}
XR_FUNC XrXirStatus xdap_dispatch_send_prepared(XdapAdapter *s,XdapJsonWriter *message) {
    if (message->status!=XR_XIR_OK) return message->status;
    if (s->seq==INT32_MAX) return XR_XIR_BUDGET;
    XrXirStatus status=s->config.write(s->config.context,message->bytes,message->length);
    if (status==XR_XIR_OK) ++s->seq;
    return status;
}
XR_FUNC XrXirStatus xdap_dispatch_prepare_response(XdapAdapter *s,const XdapRequest *request,bool success,
    const XdapJsonWriter *body,const char *error,XdapJsonWriter *output) {
    if (!output || output->resources || output->bytes) return XR_XIR_BAD_STRUCTURE;
    if (body && body->status!=XR_XIR_OK) return body->status;
    XdapJsonWriter message=xdap_json_writer(s->resources,s->config.message_limit);
    XDAP_TEXT(&message,"{\"seq\":");xdap_json_unsigned(&message,s->seq);
    XDAP_TEXT(&message,",\"type\":\"response\",\"request_seq\":");xdap_json_unsigned(&message,request->seq);
    XDAP_TEXT(&message,",\"command\":");
    const XdapJsonNode *command=&request->document.nodes[request->command];
    xdap_json_quote(&message,command->text,command->length);
    XDAP_TEXT(&message,",\"success\":");xdap_json_text(&message,success ? "true" : "false",success ? 4 : 5);
    if (body && body->length) { XDAP_TEXT(&message,",\"body\":");xdap_json_text(&message,body->bytes,body->length); }
    if (error) { XDAP_TEXT(&message,",\"message\":");xdap_json_quote(&message,error,strlen(error)); }
    XDAP_TEXT(&message,"}");
    if (message.status!=XR_XIR_OK) { XrXirStatus status=message.status;xdap_json_writer_free(&message);return status; }
    *output=message;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xdap_dispatch_response(XdapAdapter *s,const XdapRequest *request,bool success,
    const XdapJsonWriter *body,const char *error) {
    XdapJsonWriter message={0};
    XrXirStatus status=xdap_dispatch_prepare_response(s,request,success,body,error,&message);
    if (status==XR_XIR_OK) status=xdap_dispatch_send_prepared(s,&message);
    xdap_json_writer_free(&message);return status;
}
XR_FUNC XrXirStatus xdap_dispatch_event(XdapAdapter *s,const char *event,const XdapJsonWriter *body) {
    if (body && body->status!=XR_XIR_OK) return body->status;
    XdapJsonWriter message=xdap_json_writer(s->resources,s->config.message_limit);
    XDAP_TEXT(&message,"{\"seq\":");xdap_json_unsigned(&message,s->seq);
    XDAP_TEXT(&message,",\"type\":\"event\",\"event\":");xdap_json_quote(&message,event,strlen(event));
    if (body && body->length) { XDAP_TEXT(&message,",\"body\":");xdap_json_text(&message,body->bytes,body->length); }
    XDAP_TEXT(&message,"}");XrXirStatus status=xdap_dispatch_send_prepared(s,&message);xdap_json_writer_free(&message);return status;
}
XR_FUNC XrXirStatus xdap_dispatch_argument(const XdapRequest *r,const char *key,uint32_t *output) {
    if (!r->arguments) { *output=0;return XR_XIR_OK; }
    return xdap_json_member(&r->document,r->arguments,key,output);
}
XR_FUNC XrXirStatus xdap_dispatch_number(const XdapRequest *r,const char *key,uint64_t fallback,uint64_t *output) {
    uint32_t index=0;XrXirStatus status=xdap_dispatch_argument(r,key,&index);
    if (status!=XR_XIR_OK) return status;
    if (!index) { *output=fallback;return XR_XIR_OK; }
    return xdap_json_u64(&r->document,index,output);
}
XR_FUNC void xdap_dispatch_clear_stop(XdapAdapter *s) {
    for (uint32_t i=0;i<s->value_count;++i) xr_xir_value_drop(&s->values[i].value);
    memset(s->values,0,sizeof(s->values));s->value_count=0;
    xr_xir_vm_debug_snapshot_drop(s->snapshot);s->snapshot=NULL;s->depth=0;s->stop_epoch=0;
    memset(s->frames,0,sizeof(s->frames));
}
static XrXirOutputStatus dispatch_output(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    XdapAdapter *s=context;XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);
    XDAP_TEXT(&body,"{\"category\":");const char *category=stream==XR_XIR_STDERR ? "stderr" : "stdout";
    xdap_json_quote(&body,category,strlen(category));XDAP_TEXT(&body,",\"output\":");
    xdap_json_quote(&body,bytes,length);XDAP_TEXT(&body,"}");
    XrXirStatus status=xdap_dispatch_event(s,"output",&body);xdap_json_writer_free(&body);
    return status==XR_XIR_OK ? XR_XIR_OUTPUT_OK : status==XR_XIR_OUT_OF_MEMORY ? XR_XIR_OUTPUT_OOM :
        status==XR_XIR_BUDGET ? XR_XIR_OUTPUT_LIMIT : XR_XIR_OUTPUT_ERROR;
}
XR_FUNC XrXirStatus xdap_adapter_new(XrSourceCompiler **compiler,const XdapAdapterConfig *config,XdapAdapter **output) {
    if (!compiler || !*compiler || !config || !output || *output || !config->write ||
        !config->message_limit || config->message_limit>1048576 || !config->instance.depth_limit ||
        config->instance.depth_limit>64 || !xr_xir_domain_current(config->observer_metadata)) return XR_XIR_BAD_STRUCTURE;
    const XrXirCompileContext *context=xr_source_compiler_context(*compiler);
    if (!context || !xr_xir_domain_retain(config->observer_metadata)) return XR_XIR_BUDGET;
    XdapAdapter *s=NULL;XrCompileResourceStatus allocated=xr_compile_resources_calloc(context->resources,1,sizeof(*s),(void **)&s);
    if (allocated!=XR_COMPILE_RESOURCE_OK) {
        xr_xir_domain_drop(config->observer_metadata);
        return allocated==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    }
    s->compiler=*compiler;s->resources=context->resources;s->config=*config;s->seq=1;s->next_handle=1;
    /* The instance owns this sink context only while the adapter stays alive. */
    s->sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,dispatch_output,s,s->config.message_limit};
    s->config.instance.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&s->sink};
    *compiler=NULL;*output=s;return XR_XIR_OK;
}
XR_FUNC bool xdap_adapter_running(const XdapAdapter *s) { return s && s->running && !s->closed; }
XR_FUNC bool xdap_adapter_waiting(const XdapAdapter *s) { return s && s->wait_wake && s->running && !s->closed; }
XR_FUNC bool xdap_adapter_closed(const XdapAdapter *s) { return !s || s->closed; }
XR_FUNC XrXirCallStatus xdap_adapter_free(XdapAdapter *s) {
    if (!s) return XR_XIR_CALL_READY;
    if (s->busy) return XR_XIR_CALL_BUSY;
    xdap_dispatch_clear_stop(s);XrXirCallStatus status=xdap_launch_free(s->launch);
    if (status==XR_XIR_CALL_BUSY) return status;
    xr_xir_call_result_drop(&s->outcome);
    for (uint32_t i=0;i<s->function_breakpoint_count;++i) xr_compile_resources_free(s->function_breakpoints[i]);
    xr_source_compiler_free(s->compiler);
    xr_xir_domain_drop(s->config.observer_metadata);xr_compile_resources_free(s);return status;
}
static XrXirStatus dispatch_request_parse(XdapAdapter *s,const char *bytes,size_t length,XdapRequest *r) {
    if (!length || length>s->config.message_limit) return XR_XIR_BUDGET;
    XrXirStatus status=xdap_json_parse(s->resources,bytes,length,8192,64,&r->document);
    if (status!=XR_XIR_OK) return status;
    uint32_t type=0,seq=0;bool request=false;uint64_t number=0;
    status=xdap_json_member(&r->document,r->document.root,"type",&type);
    if (status==XR_XIR_OK) status=xdap_json_equal(&r->document,type,"request",&request);
    if (status==XR_XIR_OK && !request) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) status=xdap_json_member(&r->document,r->document.root,"seq",&seq);
    if (status==XR_XIR_OK) status=xdap_json_u64(&r->document,seq,&number);
    if (status==XR_XIR_OK && (!number || number>INT32_MAX)) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) status=xdap_json_member(&r->document,r->document.root,"command",&r->command);
    if (status==XR_XIR_OK && (!r->command || r->document.nodes[r->command].kind!=XDAP_JSON_STRING)) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) status=xdap_json_member(&r->document,r->document.root,"arguments",&r->arguments);
    if (status==XR_XIR_OK && r->arguments && r->document.nodes[r->arguments].kind!=XDAP_JSON_OBJECT) status=XR_XIR_BAD_STRUCTURE;
    r->seq=(uint32_t)number;return status;
}
static XrXirStatus dispatch_initialize(XdapAdapter *s,const XdapRequest *r) {
    if (s->initialized) return xdap_dispatch_response(s,r,false,NULL,"Already initialized");
    uint32_t types=0,paging=0;
    XrXirStatus settings=xdap_dispatch_argument(r,"supportsVariableType",&types);
    if (settings==XR_XIR_OK) settings=xdap_dispatch_argument(r,"supportsVariablePaging",&paging);
    if (settings!=XR_XIR_OK) return settings;
    if ((types && r->document.nodes[types].kind!=XDAP_JSON_BOOL) ||
        (paging && r->document.nodes[paging].kind!=XDAP_JSON_BOOL))
        return xdap_dispatch_response(s,r,false,NULL,"Invalid client capabilities");
    s->variable_types=types && r->document.nodes[types].boolean;
    s->variable_paging=paging && r->document.nodes[paging].boolean;
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);
    XDAP_TEXT(&body,"{\"supportsConfigurationDoneRequest\":true,\"supportsFunctionBreakpoints\":true,"
        "\"supportsTerminateRequest\":true,\"supportsRestartRequest\":true,\"supportsSteppingGranularity\":true,\"supportsDisassembleRequest\":true,"
        "\"supportsValueFormattingOptions\":false}");
    XrXirStatus status=xdap_dispatch_response(s,r,true,&body,NULL);xdap_json_writer_free(&body);
    if (status==XR_XIR_OK) { s->initialized=true;status=xdap_dispatch_event(s,"initialized",NULL); }
    return status;
}
static XrXirStatus dispatch_launch(XdapAdapter *s,const XdapRequest *r) {
    if (!s->initialized || s->launch) return xdap_dispatch_response(s,r,false,NULL,"Launch is not valid in this state");
    uint32_t file=0,stop=0;XrXirStatus status=xdap_dispatch_argument(r,"program",&file);
    if (status!=XR_XIR_OK) return status;
    if (!file || r->document.nodes[file].kind!=XDAP_JSON_STRING || !r->document.nodes[file].length)
        return xdap_dispatch_response(s,r,false,NULL,"An absolute program path is required");
    status=xdap_dispatch_argument(r,"stopOnEntry",&stop);if (status!=XR_XIR_OK) return status;
    if (stop && r->document.nodes[stop].kind!=XDAP_JSON_BOOL) return xdap_dispatch_response(s,r,false,NULL,"Invalid stopOnEntry");
    XdapLaunchRequest input={.compiler=s->compiler,.absolute_file=r->document.nodes[file].text,
        .observer_metadata=s->config.observer_metadata,.instance=s->config.instance};
    XdapLaunch *launch=NULL;XrSourceCompileDiagnostic diagnostic={0};
    status=xdap_launch_build(&input,&launch,&diagnostic);
    if (status!=XR_XIR_OK) {
        char message[256];xr_source_compile_diagnostic_format(&diagnostic,message,sizeof(message));
        xr_source_compile_diagnostic_free(&diagnostic);
        return xdap_dispatch_response(s,r,false,NULL,message);
    }
    xr_source_compile_diagnostic_free(&diagnostic);
    status=xdap_dispatch_response(s,r,true,NULL,NULL);
    if (status!=XR_XIR_OK) { xdap_launch_free(launch);return status; }
    s->launch=launch;s->stop_entry=stop && r->document.nodes[stop].boolean;
    return XR_XIR_OK;
}
static XrXirStatus dispatch_restart(XdapAdapter *s,const XdapRequest *r) {
    if (!s->initialized || !s->launch || !s->launch->absolute_file)
        return xdap_dispatch_response(s,r,false,NULL,"No restartable source launch");
    XdapLaunchRequest input={.compiler=s->compiler,.absolute_file=s->launch->absolute_file,
        .observer_metadata=s->config.observer_metadata,.instance=s->config.instance};
    XdapLaunch *candidate=NULL;XrSourceCompileDiagnostic diagnostic={0};
    XrXirStatus status=xdap_launch_build(&input,&candidate,&diagnostic);
    if (status!=XR_XIR_OK) {
        char message[256];xr_source_compile_diagnostic_format(&diagnostic,message,sizeof(message));
        xr_source_compile_diagnostic_free(&diagnostic);return xdap_dispatch_response(s,r,false,NULL,message);
    }
    xr_source_compile_diagnostic_free(&diagnostic);
    XrXirVmDebugLocation locations[32]={{0}};uint32_t count=0;
    for (uint32_t i=0;i<s->function_breakpoint_count;++i) {
        uint32_t found=UINT32_MAX,matches=0;
        for (uint32_t f=0;f<candidate->function_count;++f) {
            size_t bytes=strlen(candidate->functions[f].name)+strlen(s->function_breakpoints[i])+1;
            if (xr_compile_resources_work(s->resources,bytes)!=XR_COMPILE_RESOURCE_OK) {
                xdap_launch_free(candidate);return XR_XIR_BUDGET;
            }
            if (!strcmp(candidate->functions[f].name,s->function_breakpoints[i])) { found=f;++matches; }
        }
        if (matches==1) locations[count++]=(XrXirVmDebugLocation){found,0};
    }
    if (xdap_current_breakpoints_replace(candidate->control,locations,count)!=XR_XIR_VALUE_OK) {
        xdap_launch_free(candidate);return xdap_dispatch_response(s,r,false,NULL,"Restart breakpoint admission failed");
    }
    XrXirCallStatus started=xdap_current_start(candidate->control,candidate->entry,NULL,0);
    if (started!=XR_XIR_CALL_READY) { xdap_launch_free(candidate);return xdap_dispatch_response(s,r,false,NULL,"Restart start rejected"); }
    XdapJsonWriter response={0};status=xdap_dispatch_prepare_response(s,r,true,NULL,NULL,&response);
    if (status!=XR_XIR_OK) { xdap_launch_free(candidate);return status; }
    XrXirCallStatus retired=xdap_launch_free(s->launch);s->launch=NULL;
    if (retired!=XR_XIR_CALL_READY) {
        xdap_launch_free(candidate);xdap_json_writer_free(&response);xdap_dispatch_clear_stop(s);
        s->terminal=true;s->running=false;return xdap_dispatch_response(s,r,false,NULL,"Previous execution cleanup failed");
    }
    xdap_dispatch_clear_stop(s);xr_xir_call_result_drop(&s->outcome);
    s->launch=candidate;s->terminal=false;s->configured=true;s->running=true;s->wait_wake=0;
    s->stop_reason=s->stop_entry ? "entry" : "function breakpoint";
    if (s->stop_entry) xdap_current_pause(candidate->control);
    status=xdap_dispatch_send_prepared(s,&response);xdap_json_writer_free(&response);return status;
}
static XrXirStatus dispatch_start(XdapAdapter *s,const XdapRequest *r) {
    if (!s->launch || s->configured) return xdap_dispatch_response(s,r,false,NULL,"No unstarted launch");
    XrXirCallStatus started=xdap_current_start(s->launch->control,s->launch->entry,NULL,0);
    if (started!=XR_XIR_CALL_READY) return xdap_dispatch_response(s,r,false,NULL,"Instance start rejected");
    if (s->stop_entry) { xdap_current_pause(s->launch->control);s->stop_reason="entry"; }
    else s->stop_reason="function breakpoint";
    s->configured=true;s->running=true;return xdap_dispatch_response(s,r,true,NULL,NULL);
}
static XrXirStatus dispatch_control(XdapAdapter *s,const XdapRequest *r,const char *command) {
    uint64_t thread=0;XrXirStatus status=xdap_dispatch_number(r,"threadId",0,&thread);
    if (status!=XR_XIR_OK) return status;
    uint64_t stopped=s->snapshot ? xr_xir_vm_debug_snapshot_stopped_thread(s->snapshot) : 1;
    if (!s->launch || !s->configured || !thread || s->terminal ||
        (!strcmp(command,"pause") ? thread!=1 : thread!=stopped))
        return xdap_dispatch_response(s,r,false,NULL,"No active thread");
    if (!strcmp(command,"pause")) {
        if (!s->running) return xdap_dispatch_response(s,r,false,NULL,"Thread is already stopped");
        xdap_current_pause(s->launch->control);s->stop_reason="pause";return xdap_dispatch_response(s,r,true,NULL,NULL);
    }
    if (!s->snapshot || !s->stop_epoch) return xdap_dispatch_response(s,r,false,NULL,"notStopped");
    XrXirVmDebugMode mode=XR_XIR_VM_DEBUG_CONTINUE;
    if (strcmp(command,"continue")) {
        uint32_t granularity=0;bool instruction=false;
        status=xdap_dispatch_argument(r,"granularity",&granularity);
        if (status==XR_XIR_OK && granularity) status=xdap_json_equal(&r->document,granularity,"instruction",&instruction);
        if (status!=XR_XIR_OK) return status;
        if (!instruction) return xdap_dispatch_response(s,r,false,NULL,"Source stepping requires the owned source mapping");
        mode=!strcmp(command,"next") ? XR_XIR_VM_DEBUG_STEP_OVER :
            !strcmp(command,"stepIn") ? XR_XIR_VM_DEBUG_STEP_IN : XR_XIR_VM_DEBUG_STEP_OUT;
    }
    status=dispatch_value(xdap_current_continue(s->launch->control,s->stop_epoch,mode));
    if (status!=XR_XIR_OK) return xdap_dispatch_response(s,r,false,NULL,"Stop epoch rejected");
    xdap_dispatch_clear_stop(s);s->running=true;s->stop_reason=mode==XR_XIR_VM_DEBUG_CONTINUE ? "function breakpoint" : "step";
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);
    if (!strcmp(command,"continue")) XDAP_TEXT(&body,"{\"allThreadsContinued\":true}");
    status=xdap_dispatch_response(s,r,true,&body,NULL);xdap_json_writer_free(&body);return status;
}
static XrXirStatus dispatch_terminate(XdapAdapter *s,const XdapRequest *r,bool disconnect) {
    if (s->launch && !s->terminal) {
        XrXirCallStatus cancelled=xdap_current_stop(s->launch->control);
        if (cancelled==XR_XIR_CALL_BUSY) return xdap_dispatch_response(s,r,false,NULL,"Target is busy");
        xdap_dispatch_clear_stop(s);s->running=true;s->wait_wake=0;
        if (cancelled!=XR_XIR_CALL_READY && cancelled!=XR_XIR_CALL_CANCELLED)
            return xdap_dispatch_response(s,r,false,NULL,"Target cleanup failed");
    }
    XrXirStatus status=xdap_dispatch_response(s,r,true,NULL,NULL);
    if (disconnect && status==XR_XIR_OK) s->closed=true;
    return status;
}
XR_FUNC XrXirStatus xdap_adapter_request(XdapAdapter *s,const char *bytes,size_t length) {
    if (!s || s->busy || s->closed || !bytes) return XR_XIR_BAD_STRUCTURE;
    s->busy=true;XdapRequest r={0};XrXirStatus status=dispatch_request_parse(s,bytes,length,&r);
    if (status!=XR_XIR_OK) goto done;
    const char *command=r.document.nodes[r.command].text;
    if (!strcmp(command,"initialize")) status=dispatch_initialize(s,&r);
    else if (!strcmp(command,"launch")) status=dispatch_launch(s,&r);
    else if (!strcmp(command,"configurationDone")) status=dispatch_start(s,&r);
    else if (!strcmp(command,"restart")) status=dispatch_restart(s,&r);
    else if (!strcmp(command,"continue") || !strcmp(command,"next") || !strcmp(command,"stepIn") ||
        !strcmp(command,"stepOut") || !strcmp(command,"pause")) status=dispatch_control(s,&r,command);
    else if (!strcmp(command,"disconnect") || !strcmp(command,"terminate")) status=dispatch_terminate(s,&r,!strcmp(command,"disconnect"));
    else status=xdap_dispatch_inspect(s,&r,command);
done:
    xdap_json_document_free(&r.document);s->busy=false;return status;
}
static XrXirStatus dispatch_clock(XdapAdapter *s,uint64_t *output) {
    if (xr_compile_resources_work(s->resources,1)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
    const XrXirTimeProvider *time=&s->config.instance.time;int64_t now=-1;
    if (!time->clock || time->clock(time->context,XR_XIR_CLOCK_MONOTONIC,&now)!=XR_XIR_TIME_OK || now<0) return XR_XIR_IO;
    *output=(uint64_t)now;return XR_XIR_OK;
}
static XrXirStatus dispatch_wait_begin(XdapAdapter *s,const XdapCurrentResult *result) {
    XrXirWaitRequest request={0};
    if (xdap_current_wait_request(s->launch->control,result->instance_epoch,result->outcome.wake,&request)!=XR_XIR_CALL_READY)
        return XR_XIR_BAD_STRUCTURE;
    if (request.reserved || request.subject || request.generation || request.ticket) return XR_XIR_BAD_STRUCTURE;
    uint64_t start=0,duration=0;XrXirStatus status=XR_XIR_OK;
    if (request.kind==XR_XIR_WAIT_TIMER_MS) {
        if (!request.after_ms || request.after_ms>XR_XIR_TIMER_MAX_MS) return XR_XIR_BAD_STRUCTURE;
        status=dispatch_clock(s,&start);duration=request.after_ms*UINT64_C(1000000);
    } else if (request.kind!=XR_XIR_WAIT_YIELD || request.after_ms) return XR_XIR_UNSUPPORTED;
    if (status!=XR_XIR_OK) return status;
    s->wait_epoch=result->instance_epoch;s->wait_wake=result->outcome.wake;
    s->wait_start=start;s->wait_duration=duration;return XR_XIR_OK;
}
static XrXirStatus dispatch_wait_resume(XdapAdapter *s,bool *pending) {
    *pending=false;
    if (!s->wait_wake) return XR_XIR_OK;
    if (s->wait_duration) {
        uint64_t now=0;XrXirStatus status=dispatch_clock(s,&now);if (status!=XR_XIR_OK) return status;
        if (now<s->wait_start) return XR_XIR_IO;
        if (now-s->wait_start<s->wait_duration) { *pending=true;return XR_XIR_OK; }
    }
    if (xdap_current_resume(s->launch->control,s->wait_epoch,s->wait_wake)!=XR_XIR_CALL_READY) return XR_XIR_BAD_STRUCTURE;
    s->wait_wake=0;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xdap_adapter_step(XdapAdapter *s,uint64_t quantum) {
    if (!s || s->busy || !quantum) return XR_XIR_BAD_STRUCTURE;
    if (!s->running || s->closed || !s->launch) return XR_XIR_OK;
    s->busy=true;XdapCurrentResult result={0};bool pending=false;
    XrXirStatus status=dispatch_wait_resume(s,&pending);
    if (status!=XR_XIR_OK || pending) goto done;
    status=dispatch_value(xdap_current_poll(s->launch->control,quantum,&result));
    if (status!=XR_XIR_OK) goto done;
    if (result.state==XDAP_CURRENT_STOPPED) {
        status=dispatch_value(xdap_current_snapshot(s->launch->control,&s->snapshot));if (status!=XR_XIR_OK) goto done;
        s->depth=xr_xir_vm_debug_snapshot_depth(s->snapshot);
        if (s->depth>64 || s->next_handle>INT32_MAX-s->depth*2) { status=XR_XIR_BUDGET;goto done; }
        for (uint32_t i=0;i<s->depth;++i) {
            s->frames[i].handle=s->next_handle++;s->frames[i].scope=s->next_handle++;
        }
        s->stop_epoch=result.debug_epoch;s->running=false;
        XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);
        XDAP_TEXT(&body,"{\"reason\":");const char *reason=s->stop_reason ? s->stop_reason : "breakpoint";
        xdap_json_quote(&body,reason,strlen(reason));XDAP_TEXT(&body,",\"threadId\":");
        xdap_json_unsigned(&body,xr_xir_vm_debug_snapshot_stopped_thread(s->snapshot));XDAP_TEXT(&body,",\"allThreadsStopped\":true}");
        status=xdap_dispatch_event(s,"stopped",&body);xdap_json_writer_free(&body);
    } else if (result.state==XDAP_CURRENT_WAITING) {
        status=dispatch_wait_begin(s,&result);
    } else if (result.state==XDAP_CURRENT_ENDED) {
        s->running=false;s->terminal=true;xdap_dispatch_clear_stop(s);
        XrXirValueStatus copied=xr_xir_call_result_copy(&result.outcome,&s->outcome);
        if (copied!=XR_XIR_VALUE_OK) { status=dispatch_value(copied);goto done; }
        XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);
        XDAP_TEXT(&body,"{\"exitCode\":");xdap_json_integer(&body,result.outcome.status==XR_XIR_CALL_RETURNED ? 0 : 1);XDAP_TEXT(&body,"}");
        status=xdap_dispatch_event(s,"exited",&body);xdap_json_writer_free(&body);
        if (status==XR_XIR_OK) status=xdap_dispatch_event(s,"terminated",NULL);
    }
done:
    xdap_current_result_drop(&result);s->busy=false;return status;
}
