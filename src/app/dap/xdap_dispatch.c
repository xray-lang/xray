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
        status==XR_XIR_VALUE_MEMORY_LIMIT ? XR_XIR_MEMORY_LIMIT :
        status==XR_XIR_VALUE_OVERFLOW ? XR_XIR_OVERFLOW :
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
}
XR_FUNC XrXirStatus xdap_dispatch_send_prepared(XdapAdapter *s,XdapJsonWriter *message) {
    if (message->status!=XR_XIR_OK) return message->status;
    if (s->seq==INT32_MAX) return XR_XIR_BUDGET;
    XrXirStatus status=s->config.write(s->config.context,message->bytes,message->length);
    if (status==XR_XIR_OK) ++s->seq;
    return status;
}
static XrXirStatus dispatch_prepare_sequence(XdapAdapter *s,const XdapRequest *request,bool success,
    const XdapJsonWriter *body,const char *error,uint32_t sequence,XdapJsonWriter *output) {
    if (!output || output->resources || output->bytes) return XR_XIR_BAD_STRUCTURE;
    if (body && body->status!=XR_XIR_OK) return body->status;
    XdapJsonWriter message=xdap_json_writer(s->resources,s->config.message_limit);
    XDAP_TEXT(&message,"{\"seq\":");xdap_json_unsigned(&message,sequence);
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
XR_FUNC XrXirStatus xdap_dispatch_prepare_response(XdapAdapter *s,const XdapRequest *request,bool success,
    const XdapJsonWriter *body,const char *error,XdapJsonWriter *output) {
    return dispatch_prepare_sequence(s,request,success,body,error,s->seq,output);
}
XR_FUNC XrXirStatus xdap_dispatch_stage_response(XdapAdapter *s,const XdapRequest *request,bool success,
    const XdapJsonWriter *body,const char *error,XdapJsonWriter *output) {
    return dispatch_prepare_sequence(s,request,success,body,error,INT32_MAX,output);
}
XR_FUNC XrXirStatus xdap_dispatch_send_staged(XdapAdapter *s,XdapJsonWriter *message) {
    const char prefix[]="{\"seq\":2147483647";
    if (!message || message->status!=XR_XIR_OK || message->length<sizeof(prefix)-1 ||
        memcmp(message->bytes,prefix,sizeof(prefix)-1)) return message && message->status!=XR_XIR_OK ? message->status : XR_XIR_BAD_STRUCTURE;
    char digits[10];uint32_t number=s->seq;size_t used=0;
    do { digits[used++]=(char)('0'+number%10);number/=10; } while (number);
    memset(message->bytes+7,' ',10);
    for (size_t i=0;i<used;++i) message->bytes[7+i]=digits[used-i-1];
    return xdap_dispatch_send_prepared(s,message);
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
XR_FUNC void xdap_value_ref_drop(XdapValueRef *ref) {
    if (!ref) return;xr_xir_value_drop(&ref->value);xr_compile_resources_free((void *)ref->origin.path.steps);*ref=(XdapValueRef){0};
}
XR_FUNC void xdap_dispatch_clear_stop(XdapAdapter *s) {
    for (uint32_t i=0;i<s->value_count;++i) xdap_value_ref_drop(&s->values[i]);
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
#include "xdap_detached.inc.c"
XR_FUNC XrXirStatus xdap_adapter_new(XrSourceCompiler **compiler,const XdapAdapterConfig *config,XdapAdapter **output) {
    if (!compiler || !*compiler || !config || !output || *output || !config->write ||
        !config->message_limit || config->message_limit>1048576 || !config->instance.depth_limit ||
        config->instance.depth_limit>64 || (!config->native.load && config->native.context) ||
        !xr_xir_domain_current(config->observer_metadata)) return XR_XIR_BAD_STRUCTURE;
    if (config->detached && (*config->detached || !config->instance.output.write ||
        !xr_xir_output_provider_valid(&config->instance.output))) return XR_XIR_BAD_STRUCTURE;
    if ((config->detach_commit || config->retain_output || config->stdout_path || config->stderr_path) && !config->detached)
        return XR_XIR_BAD_STRUCTURE;
    if ((config->stdout_path!=NULL)!=(config->stderr_path!=NULL)) return XR_XIR_BAD_STRUCTURE;
    const XrXirCompileContext *context=xr_source_compiler_context(*compiler);
    if (!context || !xr_xir_domain_retain(config->observer_metadata)) return XR_XIR_BUDGET;
    XdapAdapter *s=NULL;XrCompileResourceStatus allocated=xr_compile_resources_calloc(context->resources,1,sizeof(*s),(void **)&s);
    if (allocated!=XR_COMPILE_RESOURCE_OK) {
        xr_xir_domain_drop(config->observer_metadata);
        return allocated==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
        allocated==XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_MEMORY_LIMIT :
        allocated==XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_OVERFLOW :
        allocated==XR_COMPILE_RESOURCE_CANCELLED ? XR_XIR_CANCELLED :
        allocated==XR_COMPILE_RESOURCE_DEADLINE ? XR_XIR_DEADLINE :
        allocated==XR_COMPILE_RESOURCE_CLOCK_FAILURE ? XR_XIR_CLOCK_FAILURE : XR_XIR_BAD_STRUCTURE;
    }
    s->compiler=*compiler;s->resources=context->resources;s->config=*config;s->seq=1;s->next_handle=1;s->next_watch_id=1;
    allocated=xr_compile_resources_calloc(context->resources,1,sizeof(*s->execution),(void **)&s->execution);
    if (allocated!=XR_COMPILE_RESOURCE_OK) {
        xr_compile_resources_free(s);xr_xir_domain_drop(config->observer_metadata);
        return allocated==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
        allocated==XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_MEMORY_LIMIT :
        allocated==XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_OVERFLOW :
        allocated==XR_COMPILE_RESOURCE_CANCELLED ? XR_XIR_CANCELLED :
        allocated==XR_COMPILE_RESOURCE_DEADLINE ? XR_XIR_DEADLINE :
        allocated==XR_COMPILE_RESOURCE_CLOCK_FAILURE ? XR_XIR_CLOCK_FAILURE : XR_XIR_BAD_STRUCTURE;
    }
    s->execution->resources=s->resources;s->execution->adapter=s;
    s->execution->output=config->instance.output;s->execution->time=config->instance.time;
    s->execution->retain_output=config->retain_output;
    /* This stable routing owner can outlive the compiler and control connection. */
    s->sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,dispatch_output,s,s->config.message_limit};
    s->config.instance.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,detached_output,s->execution};
    *compiler=NULL;*output=s;return XR_XIR_OK;
}
XR_FUNC bool xdap_adapter_running(const XdapAdapter *s) { return s && s->running && !s->closed; }
XR_FUNC bool xdap_adapter_waiting(const XdapAdapter *s) { return s && s->wait_wake && s->running && !s->closed; }
XR_FUNC bool xdap_adapter_closed(const XdapAdapter *s) { return !s || s->closed; }
XR_FUNC XrXirCallStatus xdap_adapter_free(XdapAdapter *s) {
    if (!s) return XR_XIR_CALL_READY;
    if (s->busy) return XR_XIR_CALL_BUSY;
    s->busy=true;xdap_dispatch_clear_stop(s);XrXirCallStatus status=xdap_launch_free(s->launch);
    if (status==XR_XIR_CALL_BUSY) { s->busy=false;return status; }
    xr_xir_call_result_drop(&s->outcome);
    for (uint32_t i=0;i<s->function_breakpoint_count;++i) {
        xr_compile_resources_free(s->function_breakpoints[i]);xdap_predicate_free(&s->function_predicates[i]);
    }
    for (uint32_t i=0;i<s->source_breakpoint_count;++i) {
        xr_compile_resources_free(s->source_breakpoints[i].identity);xdap_predicate_free(&s->source_breakpoints[i].predicate);
    }
    if (s->execution) { s->execution->adapter=NULL;xdap_detached_free(s->execution); }
    xdap_watches_dispose(s);xr_source_compiler_free(s->compiler);
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
        "\"supportsValueFormattingOptions\":false,\"supportsExceptionInfoRequest\":true,\"supportsHitConditionalBreakpoints\":true,"
        "\"supportsConditionalBreakpoints\":true,\"supportsLogPoints\":true,"
        "\"supportsSetVariable\":true,\"supportsEvaluateForHovers\":true,\"supportsPauseRequest\":true,"
        "\"exceptionBreakpointFilters\":[{\"filter\":\"all\",\"label\":\"Source language throws\",\"default\":false},"
        "{\"filter\":\"caught\",\"label\":\"Handled Source language throws\",\"default\":false},"
        "{\"filter\":\"uncaught\",\"label\":\"Language throws escaping the current execution flow\",\"default\":false}]}");
    if (s->config.detached) {
        if (body.length && body.bytes[body.length-1]=='}') --body.length;
        XDAP_TEXT(&body,",\"supportTerminateDebuggee\":true}");
    }
    if (s->config.stdout_path && s->config.stderr_path) {
        if (body.length && body.bytes[body.length-1]=='}') --body.length;
        XDAP_TEXT(&body,",\"xrayRetainedOutput\":{\"stdout\":");
        xdap_json_quote(&body,s->config.stdout_path,strlen(s->config.stdout_path));
        XDAP_TEXT(&body,",\"stderr\":");xdap_json_quote(&body,s->config.stderr_path,strlen(s->config.stderr_path));
        XDAP_TEXT(&body,",\"processId\":");xdap_json_unsigned(&body,s->config.process_id);XDAP_TEXT(&body,"}}");
    }
    XrXirStatus status=xdap_dispatch_response(s,r,true,&body,NULL);xdap_json_writer_free(&body);
    if (status==XR_XIR_OK) { s->initialized=true;status=xdap_dispatch_event(s,"initialized",NULL); }
    return status;
}
static XrXirStatus dispatch_source_options(const XdapRequest *r,bool *supported) {
    *supported=false;uint32_t args=0,cwd=0,env=0,process=0;
    XrXirStatus status=xdap_dispatch_argument(r,"args",&args);
    if (status==XR_XIR_OK) status=xdap_dispatch_argument(r,"cwd",&cwd);
    if (status==XR_XIR_OK) status=xdap_dispatch_argument(r,"env",&env);
    if (status==XR_XIR_OK) status=xdap_dispatch_argument(r,"processId",&process);
    if (status!=XR_XIR_OK) return status;
    /* These host settings require an admitted provider, not process-global
     * changes or a silently ignored request. Empty settings use the host defaults. */
    *supported=!process && (!args || (r->document.nodes[args].kind==XDAP_JSON_ARRAY && !r->document.nodes[args].count)) &&
        (!cwd || (r->document.nodes[cwd].kind==XDAP_JSON_STRING && !r->document.nodes[cwd].length)) &&
        (!env || (r->document.nodes[env].kind==XDAP_JSON_OBJECT && !r->document.nodes[env].count));
    return XR_XIR_OK;
}
static XrXirStatus dispatch_source(XdapAdapter *s,const XdapRequest *r) {
    if (!s->initialized || s->launch) return xdap_dispatch_response(s,r,false,NULL,"Source target is not valid in this state");
    bool supported=false;XrXirStatus status=dispatch_source_options(r,&supported);
    if (status!=XR_XIR_OK) return status;
    if (!supported) return xdap_dispatch_response(s,r,false,NULL,"Nonempty args/cwd/env and processId are unsupported");
    uint32_t file=0,stop=0;status=xdap_dispatch_argument(r,"program",&file);
    if (status!=XR_XIR_OK) return status;
    if (!file || r->document.nodes[file].kind!=XDAP_JSON_STRING || !r->document.nodes[file].length)
        return xdap_dispatch_response(s,r,false,NULL,"An absolute program path is required");
    status=xdap_dispatch_argument(r,"stopOnEntry",&stop);if (status!=XR_XIR_OK) return status;
    if (stop && r->document.nodes[stop].kind!=XDAP_JSON_BOOL) return xdap_dispatch_response(s,r,false,NULL,"Invalid stopOnEntry");
    XdapLaunchRequest input={.compiler=s->compiler,.absolute_file=r->document.nodes[file].text,
        .observer_metadata=s->config.observer_metadata,.instance=s->config.instance,.native=s->config.native};
    XdapLaunch *launch=NULL;XrSourceCompileDiagnostic diagnostic={0};
    status=xdap_launch_build(&input,&launch,&diagnostic);
    if (status!=XR_XIR_OK) {
        char *message=NULL;
        XrSourceCompileStatus shown=xr_source_compile_diagnostic_render(&diagnostic,xr_source_compiler_resources(s->compiler),&message);
        xr_source_compile_diagnostic_free(&diagnostic);
        XrXirStatus sent=xdap_dispatch_response(s,r,false,NULL,message?message:xr_source_compile_status_name(shown));
        xr_compile_resources_free(message);return sent;
    }
    xr_source_compile_diagnostic_free(&diagnostic);
    if (xdap_current_break_on_throw(launch->control,s->break_throw)!=XR_XIR_VALUE_OK) {
        xdap_launch_free(launch);return xdap_dispatch_response(s,r,false,NULL,"Exception filter admission failed");
    }
    if (launch->native_startup && stop && r->document.nodes[stop].boolean) {
        xdap_launch_free(launch);return xdap_dispatch_response(s,r,false,NULL,"Native startup pause is unsupported");
    }
    status=xdap_dispatch_response(s,r,true,NULL,NULL);
    if (status!=XR_XIR_OK) { xdap_launch_free(launch);return status; }
    s->launch=launch;s->stop_entry=stop && r->document.nodes[stop].boolean;
    s->attached=!strcmp(r->document.nodes[r->command].text,"attach");
    return XR_XIR_OK;
}
static XrXirStatus dispatch_restart(XdapAdapter *s,const XdapRequest *r) {
    if (!s->initialized || !s->launch || !s->launch->absolute_file)
        return xdap_dispatch_response(s,r,false,NULL,"No restartable source launch");
    XdapLaunchRequest input={.compiler=s->compiler,.absolute_file=s->launch->absolute_file,
        .observer_metadata=s->config.observer_metadata,.instance=s->config.instance,.native=s->config.native};
    XdapLaunch *candidate=NULL;XrSourceCompileDiagnostic diagnostic={0};
    XrXirStatus status=xdap_launch_build(&input,&candidate,&diagnostic);
    if (status!=XR_XIR_OK) {
        char *message=NULL;
        XrSourceCompileStatus shown=xr_source_compile_diagnostic_render(&diagnostic,xr_source_compiler_resources(s->compiler),&message);
        xr_source_compile_diagnostic_free(&diagnostic);
        XrXirStatus sent=xdap_dispatch_response(s,r,false,NULL,message?message:xr_source_compile_status_name(shown));
        xr_compile_resources_free(message);return sent;
    }
    xr_source_compile_diagnostic_free(&diagnostic);
    if (candidate->native_startup && s->stop_entry) {
        xdap_launch_free(candidate);return xdap_dispatch_response(s,r,false,NULL,"Native startup pause is unsupported");
    }
    XrXirVmDebugRule rules[32]={{0}};uint32_t count=0;
    XdapBreakpointSet breakpoints={s->function_breakpoints,s->function_rules,s->function_breakpoint_count,
        s->source_breakpoints,s->source_breakpoint_count};
    status=xdap_breakpoints_collect(candidate,&breakpoints,rules,&count);
    if (status!=XR_XIR_OK) { xdap_launch_free(candidate);return status; }
    if (xdap_current_rules_replace(candidate->control,rules,count)!=XR_XIR_VALUE_OK) {
        xdap_launch_free(candidate);return xdap_dispatch_response(s,r,false,NULL,"Restart breakpoint admission failed");
    }
    if (xdap_current_break_on_throw(candidate->control,s->break_throw)!=XR_XIR_VALUE_OK) {
        xdap_launch_free(candidate);return xdap_dispatch_response(s,r,false,NULL,"Restart exception filter admission failed");
    }
    XrXirCallStatus started=xdap_current_start(candidate->control,candidate->entry,NULL,0);
    if (started!=XR_XIR_CALL_READY) { xdap_launch_free(candidate);return xdap_dispatch_response(s,r,false,NULL,"Restart start rejected"); }
    XdapJsonWriter response={0};status=xdap_dispatch_stage_response(s,r,true,NULL,NULL,&response);
    if (status!=XR_XIR_OK) { xdap_launch_free(candidate);return status; }
    XrXirCallStatus retired=xdap_launch_free(s->launch);s->launch=NULL;
    if (retired!=XR_XIR_CALL_READY) {
        xdap_launch_free(candidate);xdap_json_writer_free(&response);xdap_dispatch_clear_stop(s);
        s->terminal=true;s->running=false;return xdap_dispatch_response(s,r,false,NULL,"Previous execution cleanup failed");
    }
    xdap_dispatch_clear_stop(s);xr_xir_call_result_drop(&s->outcome);
    s->launch=candidate;s->terminal=false;s->configured=true;s->running=true;s->wait_wake=0;
    s->stop_reason=s->stop_entry ? "entry" : s->source_breakpoint_count ? "breakpoint" : "function breakpoint";
    if (s->stop_entry) xdap_current_pause(candidate->control);
    status=xdap_dispatch_send_staged(s,&response);xdap_json_writer_free(&response);return status;
}
static XrXirStatus dispatch_start(XdapAdapter *s,const XdapRequest *r) {
    if (!s->launch || s->configured) return xdap_dispatch_response(s,r,false,NULL,"No unstarted launch");
    XrXirCallStatus started=xdap_current_start(s->launch->control,s->launch->entry,NULL,0);
    if (started!=XR_XIR_CALL_READY) return xdap_dispatch_response(s,r,false,NULL,"Instance start rejected");
    if (s->stop_entry) { xdap_current_pause(s->launch->control);s->stop_reason="entry"; }
    else s->stop_reason=s->source_breakpoint_count ? "breakpoint" : "function breakpoint";
    s->configured=true;s->running=true;return xdap_dispatch_response(s,r,true,NULL,NULL);
}
static XrXirStatus dispatch_pause(XdapAdapter *s,const XdapRequest *r,uint64_t thread) {
    if (!s->running) return xdap_dispatch_response(s,r,false,NULL,"Thread is already stopped");
    if (s->launch->native_only) return xdap_dispatch_response(s,r,false,NULL,"Native instruction pause is unsupported");
    uint64_t ids[65];uint32_t count=0;
    XrXirValueStatus observed=xdap_current_threads(s->launch->control,ids,65,&count);
    if (observed==XR_XIR_VALUE_UNSUPPORTED)
        return xdap_dispatch_response(s,r,false,NULL,"Complete thread observation requires admitted VM ingress");
    if (observed!=XR_XIR_VALUE_OK) return dispatch_value(observed);
    bool found=false;for (uint32_t i=0;i<count;++i) if (ids[i]==thread) found=true;
    if (!found) return xdap_dispatch_response(s,r,false,NULL,"No active thread");
    XdapJsonWriter response={0};XrXirStatus status=xdap_dispatch_prepare_response(s,r,true,NULL,NULL,&response);
    if (status!=XR_XIR_OK) return status;
    /* This is an all-stop target. A real active identity may request the next
     * admitted VM boundary; preparing a response never schedules that pause. */
    xdap_current_pause(s->launch->control);s->stop_reason="pause";
    status=xdap_dispatch_send_prepared(s,&response);xdap_json_writer_free(&response);return status;
}
static XrXirStatus dispatch_control(XdapAdapter *s,const XdapRequest *r,const char *command) {
    uint64_t thread=0;XrXirStatus status=xdap_dispatch_number(r,"threadId",0,&thread);
    if (status!=XR_XIR_OK) return status;
    uint64_t stopped=s->snapshot ? xr_xir_vm_debug_snapshot_stopped_thread(s->snapshot) : 1;
    if (!s->launch || !s->configured || !thread || s->terminal || (strcmp(command,"pause") && thread!=stopped))
        return xdap_dispatch_response(s,r,false,NULL,"No active thread");
    if (!strcmp(command,"pause")) return dispatch_pause(s,r,thread);
    if (!s->snapshot || !s->stop_epoch) return xdap_dispatch_response(s,r,false,NULL,"notStopped");
    XrXirVmDebugMode mode=XR_XIR_VM_DEBUG_CONTINUE;
    if (strcmp(command,"continue")) {
        uint32_t granularity=0;bool instruction=false,line=false,statement=true;
        status=xdap_dispatch_argument(r,"granularity",&granularity);
        if (status==XR_XIR_OK && granularity) {
            status=xdap_json_equal(&r->document,granularity,"instruction",&instruction);
            if (status==XR_XIR_OK) status=xdap_json_equal(&r->document,granularity,"line",&line);
            if (status==XR_XIR_OK) status=xdap_json_equal(&r->document,granularity,"statement",&statement);
        }
        if (status!=XR_XIR_OK) return status;
        if (!instruction && !line && !statement) return xdap_dispatch_response(s,r,false,NULL,"Invalid stepping granularity");
        mode=instruction ? (!strcmp(command,"next") ? XR_XIR_VM_DEBUG_STEP_OVER :
            !strcmp(command,"stepIn") ? XR_XIR_VM_DEBUG_STEP_IN : XR_XIR_VM_DEBUG_STEP_OUT) :
            line ? (!strcmp(command,"next") ? XR_XIR_VM_DEBUG_LINE_OVER :
            !strcmp(command,"stepIn") ? XR_XIR_VM_DEBUG_LINE_IN : XR_XIR_VM_DEBUG_LINE_OUT) :
            (!strcmp(command,"next") ? XR_XIR_VM_DEBUG_STATEMENT_OVER :
            !strcmp(command,"stepIn") ? XR_XIR_VM_DEBUG_STATEMENT_IN : XR_XIR_VM_DEBUG_STATEMENT_OUT);
    }
    status=dispatch_value(xdap_current_continue(s->launch->control,s->stop_epoch,mode));
    if (status!=XR_XIR_OK) return xdap_dispatch_response(s,r,false,NULL,"Stop epoch rejected");
    xdap_dispatch_clear_stop(s);s->running=true;
    s->stop_reason=mode==XR_XIR_VM_DEBUG_CONTINUE ? (s->source_breakpoint_count ? "breakpoint" : "function breakpoint") : "step";
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);
    if (!strcmp(command,"continue")) XDAP_TEXT(&body,"{\"allThreadsContinued\":true}");
    status=xdap_dispatch_response(s,r,true,&body,NULL);xdap_json_writer_free(&body);return status;
}
static XrXirStatus dispatch_terminate(XdapAdapter *s,const XdapRequest *r,bool disconnect) {
    if (disconnect) {
        uint32_t terminate=0;XrXirStatus status=xdap_dispatch_argument(r,"terminateDebuggee",&terminate);
        if (status!=XR_XIR_OK) return status;
        if (terminate && r->document.nodes[terminate].kind!=XDAP_JSON_BOOL)
            return xdap_dispatch_response(s,r,false,NULL,"Invalid terminateDebuggee policy");
        bool keep=terminate ? !r->document.nodes[terminate].boolean : s->attached;
        if (keep && s->launch && !s->terminal) return dispatch_detach(s,r);
    }
    bool notify=disconnect && !s->terminal;
    if (notify && s->seq>INT32_MAX-2) return XR_XIR_BUDGET;
    XdapJsonWriter response={0},event={0};XrXirStatus status=xdap_dispatch_stage_response(s,r,true,NULL,NULL,&response);
    if (status!=XR_XIR_OK) goto done;
    if (notify) { status=dispatch_prepare_end(s,&event);if (status!=XR_XIR_OK) goto done; }
    if (s->launch && !s->terminal) {
        XrXirCallStatus cancelled=xdap_current_stop(s->launch->control);
        if (cancelled==XR_XIR_CALL_BUSY) {
            status=xdap_dispatch_response(s,r,false,NULL,"Target is busy");goto done;
        }
        xdap_dispatch_clear_stop(s);s->running=true;s->wait_wake=0;
        if (cancelled!=XR_XIR_CALL_READY && cancelled!=XR_XIR_CALL_CANCELLED) {
            status=xdap_dispatch_response(s,r,false,NULL,"Target cleanup failed");goto done;
        }
    }
    status=xdap_dispatch_send_staged(s,&response);
    if (status==XR_XIR_OK && notify) status=xdap_dispatch_send_staged(s,&event);
    if (disconnect && status==XR_XIR_OK) s->closed=true;
done:
    xdap_json_writer_free(&response);xdap_json_writer_free(&event);return status;
}
XR_FUNC XrXirStatus xdap_adapter_request(XdapAdapter *s,const char *bytes,size_t length) {
    if (!s || s->busy || s->closed || !bytes) return XR_XIR_BAD_STRUCTURE;
    if (s->seq==INT32_MAX) return XR_XIR_BUDGET;
    s->busy=true;XdapRequest r={0};XrXirStatus status=dispatch_request_parse(s,bytes,length,&r);
    if (status!=XR_XIR_OK) goto done;
    const char *command=r.document.nodes[r.command].text;
    if (!strcmp(command,"initialize")) status=dispatch_initialize(s,&r);
    else if (!strcmp(command,"launch") || !strcmp(command,"attach")) status=dispatch_source(s,&r);
    else if (!strcmp(command,"configurationDone")) status=dispatch_start(s,&r);
    else if (!strcmp(command,"restart")) status=dispatch_restart(s,&r);
    else if (!strcmp(command,"setExceptionBreakpoints") || !strcmp(command,"exceptionInfo"))
        status=xdap_dispatch_exception(s,&r,!strcmp(command,"setExceptionBreakpoints"));
    else if (!strcmp(command,"continue") || !strcmp(command,"next") || !strcmp(command,"stepIn") ||
        !strcmp(command,"stepOut") || !strcmp(command,"pause")) status=dispatch_control(s,&r,command);
    else if (!strcmp(command,"disconnect") || !strcmp(command,"terminate")) status=dispatch_terminate(s,&r,!strcmp(command,"disconnect"));
    else status=xdap_dispatch_inspect(s,&r,command);
done:
    xdap_json_document_free(&r.document);s->busy=false;return status;
}
static XrXirStatus dispatch_clock(XdapAdapter *s,uint64_t *output) {
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
        bool visible=false;status=xdap_predicates_probe(s,&visible);
        if (status!=XR_XIR_OK || !visible) {
            if (status==XR_XIR_OK) status=dispatch_value(xdap_current_continue_filtered(s->launch->control,result.debug_epoch));
            xr_xir_vm_debug_snapshot_drop(s->snapshot);s->snapshot=NULL;goto done;
        }
        s->depth=xr_xir_vm_debug_snapshot_depth(s->snapshot);
        if (s->depth>64 || s->next_handle>INT32_MAX-s->depth*3) { status=XR_XIR_BUDGET;goto done; }
        for (uint32_t i=0;i<s->depth;++i) {
            s->frames[i].handle=s->next_handle++;s->frames[i].scope=s->next_handle++;s->frames[i].global_scope=s->next_handle++;
        }
        s->stop_epoch=result.debug_epoch;s->running=false;
        XrXirValue exception={0};XrXirValueStatus observed=xr_xir_vm_debug_snapshot_exception(s->snapshot,&exception);
        bool throwing=observed==XR_XIR_VALUE_OK;xr_xir_value_drop(&exception);
        if (!throwing && observed!=XR_XIR_VALUE_UNSUPPORTED) { status=dispatch_value(observed);goto done; }
        XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);
        XDAP_TEXT(&body,"{\"reason\":");const char *reason=throwing ? "exception" : s->stop_reason ? s->stop_reason : "breakpoint";
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
