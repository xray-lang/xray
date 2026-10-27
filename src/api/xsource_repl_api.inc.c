/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_repl_api.inc.c - Public owner of the real paired Source transaction
 */
struct XrSourceRepl {
    XrCompileResources *resources, *execution;
    XrSourceCompilerConfig source_config;
    XrXirLibraryCatalog *libraries;
    char *stdlib_path;
    XrSourceCompileOptions options;
    XrSourceSubmission *submission;
    XrSourceCall sink;
    XrSourceResult *result;
    XrSourceReplBindingInfo *names, *prepared_names;
    size_t name_count, prepared_count;
    char *namespace_id;
    XrSourceEvent observed;
    bool active, terminal, busy, stopped, type_query;
};
static XrSourceStatus api_repl_admit(const XrSourceRepl *owner) {
    if(!owner)return XR_SOURCE_BAD_ARGUMENT;
    if(owner->busy)return XR_SOURCE_BUSY;
    return owner->stopped ? XR_SOURCE_BAD_STATE : XR_SOURCE_OK;
}
static XrSourceStatus api_repl_request(XrSourceRepl *owner,ApiBuildRequest *request) {
    XrCompileRequestOptions options={owner->options.cancel_poll?api_request_cancel:NULL,
        owner->options.cancel_poll?&owner->options:NULL,
        owner->options.deadline_mode==XR_SOURCE_DEADLINE_AFTER ? XR_COMPILE_DEADLINE_AFTER : XR_COMPILE_DEADLINE_NONE,
        owner->options.timeout_ns};
    XrSourceStatus status=api_resource(xr_compile_request_control_init(&options,&request->control));
    if(status==XR_SOURCE_OK)status=api_resource(xr_compile_request_control_poll(&request->control));
    if(status==XR_SOURCE_OK)status=api_resource(xr_compile_resources_retain(owner->resources));
    if(status==XR_SOURCE_OK) {
        request->resources=owner->resources;
        status=api_compile(xr_source_compiler_catalog_request(&owner->source_config,owner->libraries,&request->control,&request->compiler));
    }
    return status;
}
static XrSourceStatus api_owned_text(XrCompileResources *resources,const char *text,char **output) {
    size_t length=strlen(text);
    if(length==SIZE_MAX)return XR_SOURCE_OVERFLOW;
    XrSourceStatus status=api_resource(xr_compile_resources_alloc(resources,length+1,(void **)output));
    if(status==XR_SOURCE_OK)memcpy(*output,text,length+1);
    return status;
}
XRAY_API XrSourceStatus xray_source_repl_new(const XrSourceCompileOptions *options,XrSourceExecution *execution,
    const char *namespace_id,const XrSourceCallOptions *runtime_options,XrSourceRepl **output) {
    if(!options || options->target!=XR_SOURCE_TARGET_VM || !execution || !namespace_id || !*namespace_id || !output || *output)
        return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_options_admit(runtime_options);
    ApiBuildRequest request={0};XrSourceRepl *owner=NULL;
    if(status==XR_SOURCE_OK)status=api_request_begin(options,&request);
    if(status==XR_SOURCE_OK)status=api_resource(xr_compile_resources_calloc(execution->resources,1,sizeof(*owner),(void **)&owner));
    if(status==XR_SOURCE_OK) {
        owner->resources=request.resources;owner->execution=execution->resources;owner->options=*options;
        status=api_owned_text(owner->resources,namespace_id,&owner->namespace_id);
    }
    if(status==XR_SOURCE_OK)status=api_compile(xr_source_compiler_take_catalog(&request.compiler,
        &owner->source_config,&owner->libraries,&owner->stdlib_path));
    XrSourceSubmissionConfig config={0};
    if(status==XR_SOURCE_OK) {
        owner->options.stdlib_path=owner->stdlib_path;
        config.resources=owner->resources;
        config.source=(XrXirSourceGenerationConfig){owner->source_config.context,owner->namespace_id,
            owner->stdlib_path,NULL,owner->libraries};
        config.target=owner->source_config.target;
        status=api_config(runtime_options,&owner->sink,&config.runtime);
    }
    if(status==XR_SOURCE_OK)status=xr_source_submission_new(&config,&owner->submission);
    if(status==XR_SOURCE_OK)status=api_resource(xr_compile_request_control_poll(&request.control));
    if(status==XR_SOURCE_OK)*output=owner;
    else if(owner) {
        XrXirCallStatus cleanup=XR_XIR_CALL_READY;
        (void)xr_source_submission_drop(&owner->submission,&cleanup);
        xr_xir_compile_library_catalog_free(owner->libraries);
        xr_compile_resources_free(owner->stdlib_path);xr_compile_resources_free(owner->namespace_id);
        xr_compile_resources_free(owner);
    }
    api_request_clear(&request);return status;
}
static XrSourceStatus api_repl_namespace(XrSourceRepl *owner,const XrSourceText *input) {
    if(!input || !input->bytes)return XR_SOURCE_BAD_ARGUMENT;
    if(input->absolute_path)return *input->absolute_path && !input->namespace_id ? XR_SOURCE_OK : XR_SOURCE_BAD_ARGUMENT;
    if(!input->namespace_id)return XR_SOURCE_BAD_ARGUMENT;
    return strcmp(owner->namespace_id,input->namespace_id) ? XR_SOURCE_BAD_ARGUMENT : XR_SOURCE_OK;
}
static XrSourceStatus api_repl_prepare_names(XrSourceRepl *owner) {
    const XrXirSourceGenerationName *names=NULL;size_t count=0;
    XrSourceStatus status=xr_source_submission_names(owner->submission,true,&names,&count);
    if(status!=XR_SOURCE_OK || !count)return status;
    status=api_resource(xr_compile_resources_calloc(owner->resources,count,
        sizeof(*owner->prepared_names),(void **)&owner->prepared_names));
    if(status!=XR_SOURCE_OK)return status;
    for(size_t i=0;i<count;++i) {
        const XrXirSourceDeclaration *declaration=names[i].binding.declaration;
        XrSourceReplBindingKind kind;
        switch(declaration->kind) {
        case XR_XIR_SOURCE_BINDING:kind=XR_SOURCE_REPL_VALUE;break;
        case XR_XIR_SOURCE_FUNCTION:kind=XR_SOURCE_REPL_FUNCTION;break;
        case XR_XIR_SOURCE_TYPE:kind=XR_SOURCE_REPL_TYPE;break;
        case XR_XIR_SOURCE_IMPORT:kind=XR_SOURCE_REPL_IMPORT;break;
        case XR_XIR_SOURCE_MODULE:kind=XR_SOURCE_REPL_MODULE;break;
        default:return XR_SOURCE_INVALID;
        }
        owner->prepared_names[i]=(XrSourceReplBindingInfo){names[i].name,names[i].type_name,
            names[i].binding.generation,kind,kind==XR_SOURCE_REPL_VALUE && !declaration->mutable,names[i].implicit_result};
    }
    owner->prepared_count=count;return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_repl_bindings(const XrSourceRepl *owner,
    const XrSourceReplBindingInfo **output,size_t *count) {
    if(!owner || !output || *output || !count || *count)return XR_SOURCE_BAD_ARGUMENT;
    if(owner->busy || owner->active)return XR_SOURCE_BUSY;
    *output=owner->names;*count=owner->name_count;return XR_SOURCE_OK;
}
static XrSourceStatus api_repl_prepare(XrSourceRepl *owner,const XrSourceText *input,
    const XrSourceAuthority *borrowed,ApiBuildRequest *request,XrSourceDiagnostic **diagnostic,bool type_query) {
    XrSourceCompileDiagnostic detail={0};XrSourceAst *ast=NULL;XrSourceResult *result=NULL;
    XrSourceStatus status=api_parse_text(request->compiler,input,&ast,&detail,true);
    if(status==XR_SOURCE_OK)status=api_resource(xr_compile_resources_calloc(owner->execution,1,sizeof(*result),(void **)&result));
    XrSourceAuthority *opened=NULL;const XrSourceAuthority *authority=borrowed;
    XrXirSourceReplInput repl={0};
    repl.request=&request->control;
    if(status==XR_SOURCE_OK && input->absolute_path && !authority) {
        const XrSourceEntryInput rooted={XR_SOURCE_ENTRY_AST,ast->path,NULL,0};
        status=api_compile(xr_source_compiler_open_input(request->compiler,&rooted,&opened,&detail));
        authority=opened;
    }
    if(status==XR_SOURCE_OK && authority) {
        repl.authority=xr_source_authority_entry(authority);
        repl.absolute_path=xr_source_authority_source_path(authority);
        repl.logical_path=xr_source_authority_logical_path(authority);
        repl.lockfile=xr_source_authority_lockfile(authority);
    }
    XrXirSourceAstOwner syntax={0};
    if(status==XR_SOURCE_OK)status=xr_source_ast_owned_input(ast,&syntax);
    repl.ast=&syntax;
    if(status==XR_SOURCE_OK)detail.stage=XR_SOURCE_COMPILE_PRODUCT;
    XrXirSourceFailure failed={0};repl.failure=&failed;
    if(status==XR_SOURCE_OK)status=api_resource(xr_compile_request_control_poll(&request->control));
    if(status==XR_SOURCE_OK)status=type_query ? xr_source_submission_prepare_repl_type_ast(owner->submission,&repl,
        &detail.source.source) : xr_source_submission_prepare_repl_ast(owner->submission,&repl,&detail.source.source);
    bool prepared=status==XR_SOURCE_OK;
    if((status==XR_SOURCE_BAD_ARGUMENT || status==XR_SOURCE_REJECTED) &&
        detail.source.source.status==XR_XIR_BAD_STRUCTURE &&
        (failed.origin==XR_XIR_SOURCE_DIAGNOSTIC_PARSE || failed.origin==XR_XIR_SOURCE_DIAGNOSTIC_INPUT))status=XR_SOURCE_INVALID;
    if(!xr_xir_compile_source_failure_empty(&failed)) {
        xr_xir_compile_source_failure_free(&detail.source.failure);
        detail.source.failure=failed;failed=(XrXirSourceFailure){0};
    }
    xr_source_authority_close(opened);
    if(status==XR_SOURCE_OK)status=api_resource(xr_compile_request_control_poll(&request->control));
    if(status==XR_SOURCE_OK && !type_query)status=api_repl_prepare_names(owner);
    xray_source_ast_free(ast);
    if(status!=XR_SOURCE_OK && prepared) {
        XrXirCallStatus cleanup=XR_XIR_CALL_READY;
        if(xr_source_submission_abort(owner->submission,&cleanup)!=XR_SOURCE_OK)owner->stopped=true;
        xr_compile_resources_free(owner->prepared_names);owner->prepared_names=NULL;owner->prepared_count=0;
    }
    if(status==XR_SOURCE_OK) {
        result->resources=owner->execution;
        owner->result=result;owner->active=true;owner->terminal=false;owner->type_query=type_query;
        owner->observed=api_event(XR_XIR_CALL_READY,0,0);
    } else {
        xray_source_result_free(result);api_diagnostic_snapshot(request->resources,&detail,status,diagnostic);
    }
    xr_source_compile_diagnostic_free(&detail);return status;
}
static XrSourceStatus api_repl_begin(XrSourceRepl *owner,const XrSourceText *input,
    XrSourceDiagnostic **diagnostic,bool type_query) {
    if(diagnostic && *diagnostic)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_repl_admit(owner);if(status!=XR_SOURCE_OK)return status;
    if(owner->active || owner->result)return XR_SOURCE_BUSY;
    owner->busy=true;ApiBuildRequest request={0};status=api_repl_namespace(owner,input);
    if(status==XR_SOURCE_OK)status=api_repl_request(owner,&request);
    if(status==XR_SOURCE_OK)status=api_repl_prepare(owner,input,NULL,&request,diagnostic,type_query);
    api_request_clear(&request);owner->busy=false;return status;
}
XRAY_API XrSourceStatus xray_source_repl_begin_file(XrSourceRepl *owner,const char *path,
    XrSourceDiagnostic **diagnostic) {
    if(diagnostic && *diagnostic)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_repl_admit(owner);if(status!=XR_SOURCE_OK)return status;
    if(owner->active || owner->result)return XR_SOURCE_BUSY;
    if(!path || !*path)return XR_SOURCE_BAD_ARGUMENT;
    owner->busy=true;ApiBuildRequest request={0};XrSourceCompilerFile file={0};XrSourceCompileDiagnostic detail={0};
    status=api_repl_request(owner,&request);
    if(status==XR_SOURCE_OK)status=api_compile(xr_source_compiler_read_file(request.compiler,path,&file,&detail));
    if(status==XR_SOURCE_OK) {
        const XrSourceText input={xr_source_authority_source_path(file.authority),NULL,(const char *)file.bytes,file.length};
        status=api_repl_prepare(owner,&input,file.authority,&request,diagnostic,false);
    } else api_diagnostic_snapshot(request.resources,&detail,status,diagnostic);
    xr_source_compile_diagnostic_free(&detail);xr_source_compiler_file_clear(&file);api_request_clear(&request);
    owner->busy=false;return status;
}
XRAY_API XrSourceStatus xray_source_repl_begin(XrSourceRepl *owner,const XrSourceText *input,
    XrSourceDiagnostic **diagnostic) {return api_repl_begin(owner,input,diagnostic,false);}
XRAY_API XrSourceStatus xray_source_repl_begin_type(XrSourceRepl *owner,const XrSourceText *input,
    XrSourceDiagnostic **diagnostic) {return api_repl_begin(owner,input,diagnostic,true);}

typedef struct ApiReplBuffer {
    XrCompileResources *resources;
    char *bytes;
    size_t used, limit;
    XrSourceStatus status;
} ApiReplBuffer;
static int api_repl_buffer(void *context,const void *bytes,size_t length) {
    ApiReplBuffer *buffer=context;
    if(length>buffer->limit-buffer->used){buffer->status=XR_SOURCE_LIMIT;return 0;}
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
    ApiReplBuffer buffer={.resources=owner->execution,.limit=remaining-1,.status=XR_SOURCE_OK};
    XrXirFormatContext format={0};
    if(!xr_value_format_value(xr_xir_value_format_reader(&format),(XrValueFormatNode){value,0},
        (XrValueFormatSink){&buffer,api_repl_buffer},0))return buffer.status==XR_SOURCE_OK?XR_SOURCE_INVALID:buffer.status;
    size_t total=buffer.used+1;
    XrSourceStatus status=api_resource(xr_compile_resources_calloc(buffer.resources,total,1,(void **)&buffer.bytes));
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
    xr_compile_resources_free(buffer.bytes);return status;
}
static XrSourceStatus api_repl_finish(XrSourceRepl *owner,const XrXirInstanceResult *observed) {
    XrSourceStatus status=xr_source_submission_take(owner->submission,&owner->result->value);
    XrXirCallStatus cleanup=XR_XIR_CALL_READY;
    if(status==XR_SOURCE_OK && observed->outcome.status==XR_XIR_CALL_RETURNED &&
        xr_source_submission_repl_result(owner->submission))status=api_repl_display(owner);
    if(status==XR_SOURCE_OK && observed->outcome.status==XR_XIR_CALL_RETURNED && !owner->type_query) {
        status=xr_source_submission_publish(owner->submission);
        if(status==XR_SOURCE_OK) {
            xr_compile_resources_free(owner->names);owner->names=owner->prepared_names;owner->name_count=owner->prepared_count;
            owner->prepared_names=NULL;owner->prepared_count=0;
        }
    }
    if(status!=XR_SOURCE_OK || observed->outcome.status!=XR_XIR_CALL_RETURNED || owner->type_query) {
        XrSourceStatus aborted=xr_source_submission_abort(owner->submission,&cleanup);
        if(aborted!=XR_SOURCE_OK)owner->stopped=true;
        xr_compile_resources_free(owner->prepared_names);owner->prepared_names=NULL;owner->prepared_count=0;
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
    owner->busy=false;return status;
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
    status=api_resource(xr_compile_resources_calloc(owner->execution,1,sizeof(*value),(void **)&value));
    if(status==XR_SOURCE_OK) {
        value->resources=owner->execution;
        status=xr_source_submission_read(owner->submission,name,&value->value);
    }
    if(status==XR_SOURCE_OK)*output=value;else xray_source_value_free(value);
    owner->busy=false;return status;
}
XRAY_API XrSourceStatus xray_source_repl_write(XrSourceRepl *owner,const char *name,const XrSourceValue *value) {
    if(!name || !value)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_repl_admit(owner);if(status!=XR_SOURCE_OK)return status;
    if(owner->active)return XR_SOURCE_BUSY;
    owner->busy=true;XrXirCallStatus cleanup=XR_XIR_CALL_READY;
    status=xr_source_submission_write(owner->submission,name,&value->value,&cleanup);
    if(cleanup!=XR_XIR_CALL_READY || status==XR_SOURCE_LIMIT)owner->stopped=true;
    owner->busy=false;return status;
}
XRAY_API XrSourceStatus xray_source_repl_drop(XrSourceRepl **output,XrSourceEvent *cleanup) {
    if(!output)return XR_SOURCE_BAD_ARGUMENT;
    if(!*output){if(cleanup)*cleanup=api_event(XR_XIR_CALL_READY,0,0);return XR_SOURCE_OK;}
    XrSourceRepl *owner=*output;if(owner->busy)return XR_SOURCE_BUSY;
    owner->busy=true;XrXirCallStatus retired=XR_XIR_CALL_READY;
    XrSourceStatus status=xr_source_submission_drop(&owner->submission,&retired);
    if(status==XR_SOURCE_BUSY){owner->busy=false;return status;}
    if(cleanup){*cleanup=api_event(retired,0,0);if(retired!=XR_XIR_CALL_READY)cleanup->state=XR_SOURCE_RUN_CLEANUP_FAILED;}
    xray_source_result_free(owner->result);
    xr_compile_resources_free(owner->names);xr_compile_resources_free(owner->prepared_names);
    xr_compile_resources_free(owner->namespace_id);
    xr_xir_compile_library_catalog_free(owner->libraries);
    xr_compile_resources_free(owner->stdlib_path);xr_compile_resources_free(owner);*output=NULL;return XR_SOURCE_OK;
}

#include "xsource_repl_input.inc.c"
