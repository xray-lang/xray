/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_repl_api.inc.c - Public owner of the real paired Source transaction
 */
struct XrSourceRepl {
    XrSourceCompiler *compiler;
    XrSourceSubmission *submission;
    XrSourceCall sink;
    XrSourceResult *result;
    char *namespace_id;
    XrSourceEvent observed;
    bool active, terminal, busy, stopped;
};
static XrSourceStatus api_repl_admit(const XrSourceRepl *owner) {
    if(!owner)return XR_SOURCE_BAD_ARGUMENT;
    if(owner->busy)return XR_SOURCE_BUSY;
    if(owner->stopped)return XR_SOURCE_BAD_STATE;
    return api_resource(xr_compile_state_status(xr_source_compiler_state(owner->compiler)));
}
static XrSourceStatus api_repl_capture(XrSourceRepl *owner,XrSourceStatus status) {
    if(status==XR_SOURCE_OUT_OF_MEMORY || status==XR_SOURCE_BUDGET)
        return api_resource(xr_compile_state_fail(xr_source_compiler_state(owner->compiler),
            status==XR_SOURCE_OUT_OF_MEMORY?XR_COMPILE_RESOURCE_OUT_OF_MEMORY:XR_COMPILE_RESOURCE_BUDGET));
    return status;
}
XRAY_API XrSourceStatus xray_source_repl_new(const XrSourceCompilerOptions *compiler_options,
    const char *namespace_id,const XrSourceCallOptions *runtime_options,XrSourceRepl **output) {
    if(!namespace_id || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_options_admit(runtime_options);
    if(status!=XR_SOURCE_OK)return status;
    XrSourceCompiler *compiler=NULL;
    status=xray_source_compiler_new(compiler_options,&compiler);
    if(status!=XR_SOURCE_OK)return status;
    XrCompileState *state=xr_source_compiler_state(compiler);XrSourceRepl *owner=NULL;
    status=api_resource(xr_compile_state_calloc(state,1,sizeof(*owner),(void **)&owner));
    if(status==XR_SOURCE_OK) {
        owner->compiler=compiler;
        status=api_resource(xr_compile_state_strdup(state,namespace_id,&owner->namespace_id));
    }
    XrSourceSubmissionConfig config={0};config.state=state;
    if(status==XR_SOURCE_OK)status=api_compile(xr_source_compiler_generation_config(compiler,
        owner->namespace_id,&config.source));
    config.target=(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if(status==XR_SOURCE_OK)status=api_config(runtime_options,&owner->sink,&config.runtime);
    if(status==XR_SOURCE_OK)status=xr_source_submission_new(&config,&owner->submission);
    if(status==XR_SOURCE_OK)*output=owner;
    else {
        if(owner){xr_compile_state_free(owner->namespace_id);xr_compile_state_free(owner);}
        xray_source_compiler_free(compiler);
    }
    return status;
}
static XrSourceStatus api_repl_namespace(XrSourceRepl *owner,const XrSourceText *input) {
    if(!input || !input->bytes || input->absolute_path || !input->namespace_id)return XR_SOURCE_BAD_ARGUMENT;
    XrCompileState *state=xr_source_compiler_state(owner->compiler);
    size_t expected=0,actual=0;
    XrSourceStatus status=api_resource(xr_compile_state_string_length(state,owner->namespace_id,&expected));
    if(status==XR_SOURCE_OK)status=api_resource(xr_compile_state_string_length(state,input->namespace_id,&actual));
    if(status==XR_SOURCE_OK)status=api_resource(xr_compile_state_work(state,expected));
    if(status==XR_SOURCE_OK && (expected!=actual || memcmp(owner->namespace_id,input->namespace_id,expected)))
        status=XR_SOURCE_BAD_ARGUMENT;
    return status;
}
XRAY_API XrSourceStatus xray_source_repl_begin(XrSourceRepl *owner,const XrSourceText *input,
    XrSourceDiagnostic **diagnostic) {
    if(diagnostic && *diagnostic)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_repl_admit(owner);if(status!=XR_SOURCE_OK)return status;
    if(owner->active || owner->result)return XR_SOURCE_BUSY;
    owner->busy=true;status=api_repl_namespace(owner,input);
    XrCompileState *state=xr_source_compiler_state(owner->compiler);
    XrSourceAst *ast=NULL;XrSourceDiagnostic *detail=NULL;XrSourceResult *result=NULL;
    if(status==XR_SOURCE_OK)status=api_parse_text(owner->compiler,input,&ast,diagnostic?&detail:NULL,true);
    if(status==XR_SOURCE_OK)status=api_resource(xr_compile_state_calloc(state,1,sizeof(*result),(void **)&result));
    if(status==XR_SOURCE_OK && diagnostic)status=api_resource(xr_compile_state_calloc(state,1,sizeof(*detail),(void **)&detail));
    XrXirSourceAstOwner syntax={0};
    if(status==XR_SOURCE_OK)status=xr_source_ast_owned_input(ast,&syntax);
    if(status==XR_SOURCE_OK)status=xr_source_submission_prepare_repl_ast(owner->submission,&syntax,
        detail?&detail->detail.source.source:NULL);
    xray_source_ast_free(ast);
    if(status==XR_SOURCE_OK) {
        result->resources=xr_source_compiler_resources(owner->compiler);
        owner->result=result;owner->active=true;owner->terminal=false;
        owner->observed=api_event(XR_XIR_CALL_READY,0,0);xray_source_diagnostic_free(detail);
    } else {
        xray_source_result_free(result);
        if(detail){detail->detail.status=api_compile_failure(status);*diagnostic=detail;}
    }
    status=api_repl_capture(owner,status);owner->busy=false;return status;
}
typedef struct ApiReplBuffer {
    XrCompileState *state;
    char *bytes;
    size_t used, limit;
    XrSourceStatus status;
} ApiReplBuffer;
static int api_repl_buffer(void *context,const void *bytes,size_t length) {
    ApiReplBuffer *buffer=context;
    if(length>buffer->limit-buffer->used){buffer->status=XR_SOURCE_LIMIT;return 0;}
    buffer->status=api_resource(xr_compile_state_work(buffer->state,length?length:1));
    if(buffer->status!=XR_SOURCE_OK)return 0;
    if(buffer->bytes && length)memcpy(buffer->bytes+buffer->used,bytes,length);
    buffer->used+=length;return 1;
}
static XrSourceStatus api_repl_display(XrSourceRepl *owner) {
    const XrXirValue *value=&owner->result->value.value,*payload=NULL;bool some=false;
    if(xr_xir_nullable_view(value,&some,&payload)) {
        if(!some)return XR_SOURCE_OK;
        value=payload;
    }
    size_t remaining=owner->sink.sink.byte_limit;
    if(!remaining)return XR_SOURCE_LIMIT;
    ApiReplBuffer buffer={.state=xr_source_compiler_state(owner->compiler),.limit=remaining-1,.status=XR_SOURCE_OK};
    XrXirFormatContext format={0};
    if(!xr_value_format_value(xr_xir_value_format_reader(&format),(XrValueFormatNode){value,0},
        (XrValueFormatSink){&buffer,api_repl_buffer},0))return buffer.status==XR_SOURCE_OK?XR_SOURCE_INVALID:buffer.status;
    size_t total=buffer.used+1;
    XrSourceStatus status=api_resource(xr_compile_state_calloc(buffer.state,total,1,(void **)&buffer.bytes));
    if(status!=XR_SOURCE_OK)return status;
    buffer.used=0;buffer.limit=total-1;format=(XrXirFormatContext){0};
    if(!xr_value_format_value(xr_xir_value_format_reader(&format),(XrValueFormatNode){value,0},
        (XrValueFormatSink){&buffer,api_repl_buffer},0) || buffer.used!=total-1)
        status=buffer.status==XR_SOURCE_OK?XR_SOURCE_INVALID:buffer.status;
    if(status==XR_SOURCE_OK) {
        buffer.bytes[buffer.used]='\n';
        XrXirOutputStatus written=api_output(&owner->sink,XR_XIR_STDOUT,buffer.bytes,total);
        status=written==XR_XIR_OUTPUT_OK?XR_SOURCE_OK:written==XR_XIR_OUTPUT_OOM?XR_SOURCE_OUT_OF_MEMORY:
            written==XR_XIR_OUTPUT_LIMIT?XR_SOURCE_LIMIT:XR_SOURCE_IO;
    }
    xr_compile_state_free(buffer.bytes);return status;
}
static XrSourceStatus api_repl_finish(XrSourceRepl *owner,const XrXirInstanceResult *observed) {
    XrSourceStatus status=xr_source_submission_take(owner->submission,&owner->result->value);
    XrXirCallStatus cleanup=XR_XIR_CALL_READY;
    if(status==XR_SOURCE_OK && observed->outcome.status==XR_XIR_CALL_RETURNED &&
        xr_source_submission_repl_result(owner->submission))status=api_repl_display(owner);
    if(status==XR_SOURCE_OK && observed->outcome.status==XR_XIR_CALL_RETURNED)
        status=xr_source_submission_publish(owner->submission);
    if(status!=XR_SOURCE_OK || observed->outcome.status!=XR_XIR_CALL_RETURNED) {
        XrSourceStatus aborted=xr_source_submission_abort(owner->submission,&cleanup);
        if(aborted!=XR_SOURCE_OK)owner->stopped=true;
    }
    if(status!=XR_SOURCE_OK) {
        owner->stopped=true;
        xray_source_result_free(owner->result);owner->result=NULL;owner->active=false;
        return status;
    }
    owner->observed=api_event(observed->outcome.status,observed->epoch,observed->outcome.wake);
    if(cleanup!=XR_XIR_CALL_READY) {
        owner->observed=api_event(cleanup,0,0);owner->observed.state=XR_SOURCE_RUN_CLEANUP_FAILED;
        owner->stopped=true;
    }
    owner->active=false;owner->terminal=true;return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_repl_poll(XrSourceRepl *owner,uint64_t quantum,XrSourceEvent *output) {
    if(!owner || !output || !quantum)return XR_SOURCE_BAD_ARGUMENT;
    if(owner->busy)return XR_SOURCE_BUSY;
    if(owner->terminal){*output=owner->observed;return XR_SOURCE_OK;}
    if(!owner->active)return XR_SOURCE_BAD_STATE;
    owner->busy=true;XrXirInstanceResult observed={0};
    XrSourceStatus status=xr_source_submission_poll(owner->submission,quantum,&observed);
    if(status==XR_SOURCE_OK) {
        if(observed.outcome.status!=XR_XIR_CALL_READY && observed.outcome.status!=XR_XIR_CALL_SUSPENDED)
            status=api_repl_finish(owner,&observed);
        else owner->observed=api_event(observed.outcome.status,observed.epoch,observed.outcome.wake);
        if(status==XR_SOURCE_OK)*output=owner->observed;
    }
    if(status==XR_SOURCE_OK && owner->terminal && owner->result &&
        owner->result->value.status==XR_XIR_CALL_OOM)(void)api_repl_capture(owner,XR_SOURCE_OUT_OF_MEMORY);
    status=api_repl_capture(owner,status);owner->busy=false;return status;
}
XRAY_API XrSourceStatus xray_source_repl_wait_request(const XrSourceRepl *owner,uint64_t epoch,
    uint64_t wake,XrSourceWait *output,size_t size) {
    if(!owner || !output || size!=sizeof(*output))return XR_SOURCE_BAD_ARGUMENT;
    if(owner->busy)return XR_SOURCE_BUSY;
    if(!owner->active)return XR_SOURCE_BAD_STATE;
    XrXirWaitRequest request={0};XrSourceStatus status=xr_source_submission_wait(owner->submission,epoch,wake,&request);
    if(status!=XR_SOURCE_OK)return status;
    if(request.reserved)return XR_SOURCE_INVALID;
    XrSourceWaitKind kind;
    switch(request.kind) {
    case XR_XIR_WAIT_YIELD:kind=XR_SOURCE_WAIT_YIELD;break;
    case XR_XIR_WAIT_TIMER_MS:kind=XR_SOURCE_WAIT_TIMER;break;
    case XR_XIR_WAIT_TASK:kind=XR_SOURCE_WAIT_TASK;break;
    default:return XR_SOURCE_UNSUPPORTED;
    }
    *output=(XrSourceWait){kind,request.after_ms,request.subject,request.generation,request.ticket};return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_repl_resume(XrSourceRepl *owner,uint64_t epoch,uint64_t wake) {
    XrSourceStatus status=api_repl_admit(owner);if(status!=XR_SOURCE_OK)return status;
    if(!owner->active)return XR_SOURCE_BAD_STATE;
    owner->busy=true;status=xr_source_submission_resume(owner->submission,epoch,wake);owner->busy=false;return status;
}
XRAY_API XrSourceStatus xray_source_repl_cancel(XrSourceRepl *owner) {
    XrSourceStatus status=api_repl_admit(owner);if(status!=XR_SOURCE_OK)return status;
    if(!owner->active)return XR_SOURCE_BAD_STATE;
    owner->busy=true;status=xr_source_submission_cancel(owner->submission);owner->busy=false;return status;
}
XRAY_API XrSourceStatus xray_source_repl_take(XrSourceRepl *owner,XrSourceResult **output) {
    if(!owner || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    if(owner->busy)return XR_SOURCE_BUSY;
    if(!owner->terminal || !owner->result)return XR_SOURCE_BAD_STATE;
    *output=owner->result;owner->result=NULL;return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_repl_read(XrSourceRepl *owner,const char *name,XrSourceValue **output) {
    if(!name || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_repl_admit(owner);if(status!=XR_SOURCE_OK)return status;
    if(owner->active)return XR_SOURCE_BUSY;
    owner->busy=true;XrSourceValue *value=NULL;
    status=api_resource(xr_compile_state_calloc(xr_source_compiler_state(owner->compiler),1,sizeof(*value),(void **)&value));
    if(status==XR_SOURCE_OK) {
        value->resources=xr_source_compiler_resources(owner->compiler);
        status=xr_source_submission_read(owner->submission,name,&value->value);
    }
    if(status==XR_SOURCE_OK)*output=value;else xray_source_value_free(value);
    status=api_repl_capture(owner,status);owner->busy=false;return status;
}
XRAY_API XrSourceStatus xray_source_repl_drop(XrSourceRepl **output,XrSourceEvent *cleanup) {
    if(!output)return XR_SOURCE_BAD_ARGUMENT;
    if(!*output){if(cleanup)*cleanup=api_event(XR_XIR_CALL_READY,0,0);return XR_SOURCE_OK;}
    XrSourceRepl *owner=*output;if(owner->busy)return XR_SOURCE_BUSY;
    owner->busy=true;XrXirCallStatus retired=XR_XIR_CALL_READY;
    XrSourceStatus status=xr_source_submission_drop(&owner->submission,&retired);
    if(status==XR_SOURCE_BUSY){owner->busy=false;return status;}
    if(cleanup){*cleanup=api_event(retired,0,0);if(retired!=XR_XIR_CALL_READY)cleanup->state=XR_SOURCE_RUN_CLEANUP_FAILED;}
    xray_source_result_free(owner->result);XrSourceCompiler *compiler=owner->compiler;
    xr_compile_state_free(owner->namespace_id);xr_compile_state_free(owner);*output=NULL;
    xray_source_compiler_free(compiler);return XR_SOURCE_OK;
}
