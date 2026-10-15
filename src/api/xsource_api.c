/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_api.c - Public ownership at the ordinary compiler and HostCall boundary
 */
#include "../../include/xray_source.h"
#include "xsource_compiler.h"
#include "xsource_paths.h"
#include "../execution/xr_xir_host_execution.h"
#include "../execution/xr_xir_host_cli.h"
#include "../execution/xr_xir_host_time.h"
#include "../xir/xxir_vm.h"
#include "../xir/xxir_output.h"
#include "../xir/xxir_format.h"
#include <stdio.h>
#include <string.h>

struct XrSourceProgram { XrCompileResources *resources; XrXirSourceProduct *product; XrXirProgram *code; XrSourceProgramInfo info; };
struct XrSourceDiagnostic { XrSourceCompileDiagnostic detail; };
struct XrSourceResult { XrXirCallResult value; };
struct XrSourceCall {
    XrCompileResources *resources;
    XrXirHostCall *host;
    XrXirOutputSink sink;
    XrSourceWrite write;
    void *context;
    XrXirCallStatus observed;
    bool taken, busy;
};
static XrSourceStatus api_resource(XrCompileResourceStatus status) {
    return status == XR_COMPILE_RESOURCE_OK ? XR_SOURCE_OK : status == XR_COMPILE_RESOURCE_BUDGET ? XR_SOURCE_BUDGET :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_SOURCE_OUT_OF_MEMORY : XR_SOURCE_BAD_ARGUMENT;
}
static XrSourceStatus api_compile(XrSourceCompileStatus status) {
    switch(status) {
    case XR_SOURCE_COMPILE_OK:return XR_SOURCE_OK;
    case XR_SOURCE_COMPILE_BAD_ARGUMENT:return XR_SOURCE_BAD_ARGUMENT;
    case XR_SOURCE_COMPILE_NOT_FOUND:return XR_SOURCE_NOT_FOUND;
    case XR_SOURCE_COMPILE_INVALID:return XR_SOURCE_INVALID;
    case XR_SOURCE_COMPILE_LIMIT:return XR_SOURCE_LIMIT;
    case XR_SOURCE_COMPILE_BUDGET:return XR_SOURCE_BUDGET;
    case XR_SOURCE_COMPILE_OUT_OF_MEMORY:return XR_SOURCE_OUT_OF_MEMORY;
    case XR_SOURCE_COMPILE_IO:return XR_SOURCE_IO;
    case XR_SOURCE_COMPILE_UNSUPPORTED:return XR_SOURCE_UNSUPPORTED;
    default:return XR_SOURCE_REJECTED;
    }
}
static XrSourceStatus api_call(XrXirCallStatus status) {
    switch(status) {
    case XR_XIR_CALL_READY:case XR_XIR_CALL_CANCEL_REQUESTED:return XR_SOURCE_OK;
    case XR_XIR_CALL_OOM:return XR_SOURCE_OUT_OF_MEMORY;
    case XR_XIR_CALL_LIMIT:return XR_SOURCE_LIMIT;
    case XR_XIR_CALL_BUSY:return XR_SOURCE_BUSY;
    case XR_XIR_CALL_BAD_STATE:case XR_XIR_CALL_CONSUMED:return XR_SOURCE_BAD_STATE;
    case XR_XIR_CALL_UNSUPPORTED:return XR_SOURCE_UNSUPPORTED;
    case XR_XIR_CALL_HOST_ERROR:case XR_XIR_CALL_OUTPUT_ERROR:return XR_SOURCE_IO;
    case XR_XIR_CALL_BAD_ARGUMENT:case XR_XIR_CALL_BAD_ABI:return XR_SOURCE_BAD_ARGUMENT;
    default:return XR_SOURCE_EXECUTION_FAILED;
    }
}
static XrSourceEvent api_event(XrXirCallStatus status,uint64_t epoch,uint64_t wake) {
    XrSourceRunState state=XR_SOURCE_RUN_API_ERROR;
    if(xr_xir_call_panic_status(status))state=XR_SOURCE_RUN_PANICKED;
    else switch(status) {
    case XR_XIR_CALL_READY:case XR_XIR_CALL_CANCEL_REQUESTED:state=XR_SOURCE_RUN_READY;break;
    case XR_XIR_CALL_SUSPENDED:state=XR_SOURCE_RUN_SUSPENDED;break;
    case XR_XIR_CALL_RETURNED:state=XR_SOURCE_RUN_RETURNED;break;
    case XR_XIR_CALL_THROWN:state=XR_SOURCE_RUN_THROWN;break;
    case XR_XIR_CALL_CANCELLED:state=XR_SOURCE_RUN_CANCELLED;break;
    case XR_XIR_CALL_LIMIT:state=XR_SOURCE_RUN_LIMIT;break;
    case XR_XIR_CALL_OOM:state=XR_SOURCE_RUN_OUT_OF_MEMORY;break;
    case XR_XIR_CALL_HOST_ERROR:state=XR_SOURCE_RUN_HOST_ERROR;break;
    case XR_XIR_CALL_OUTPUT_ERROR:state=XR_SOURCE_RUN_OUTPUT_ERROR;break;
    case XR_XIR_CALL_UNSUPPORTED:state=XR_SOURCE_RUN_UNSUPPORTED;break;
    default:break;
    }
    return(XrSourceEvent){state,(uint32_t)status,epoch,wake};
}
static XrSourceWriteStatus api_stdio(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    (void)context;
    FILE *file=stream==XR_SOURCE_STDOUT?stdout:stream==XR_SOURCE_STDERR?stderr:NULL;
    return file && (!length || fwrite(bytes,1,length,file)==length)?XR_SOURCE_WRITE_OK:XR_SOURCE_WRITE_IO;
}
static XrXirOutputStatus api_output(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    XrSourceCall *call=context;
    XrSourceOutputStream public_stream=stream==XR_XIR_STDOUT?XR_SOURCE_STDOUT:XR_SOURCE_STDERR;
    XrSourceWriteStatus status=call->write(call->context,public_stream,bytes,length);
    switch(status) {
    case XR_SOURCE_WRITE_OK:return XR_XIR_OUTPUT_OK;
    case XR_SOURCE_WRITE_OOM:return XR_XIR_OUTPUT_OOM;
    case XR_SOURCE_WRITE_LIMIT:return XR_XIR_OUTPUT_LIMIT;
    default:return XR_XIR_OUTPUT_ERROR;
    }
}
XRAY_API XrSourceStatus xray_source_compiler_options_init(XrSourceCompilerOptions *options,size_t size) {
    if(!options || size!=sizeof(*options))return XR_SOURCE_BAD_ARGUMENT;
    XrCompileResourceLimits limits=xr_source_compile_default_resource_limits();
    *options=(XrSourceCompilerOptions){XRAY_SOURCE_API_VERSION,sizeof(*options),limits.allocated_bytes,limits.live_bytes,limits.work,NULL};
    return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_compiler_new(const XrSourceCompilerOptions *options,XrSourceCompiler **output) {
    if(!options || options->api_version!=XRAY_SOURCE_API_VERSION || options->struct_size!=sizeof(*options) || !output || *output)
        return XR_SOURCE_BAD_ARGUMENT;
    XrCompileResourceLimits limits={options->allocated_bytes,options->live_bytes,options->work};
    XrCompileResources *resources=NULL;
    XrSourceStatus status=api_resource(xr_compile_resources_new(&limits,&resources));
    if(status!=XR_SOURCE_OK)return status;
    XrStdlibPath selected={0};const char *stdlib=options->stdlib_path;
    if(!stdlib){status=api_compile(xr_source_stdlib_path(resources,&selected,NULL));stdlib=selected.path;}
    if(status==XR_SOURCE_OK) {
        XrSourceCompilerConfig config={{resources,xr_xir_compile_default_limits()},
            {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},stdlib,xr_source_compile_default_manifest_limits()};
        status=api_compile(xr_source_compiler_new(&config,output,NULL));
    }
    xr_source_stdlib_path_free(&selected);xr_compile_resources_release(resources);return status;
}
XRAY_API void xray_source_compiler_free(XrSourceCompiler *compiler) {xr_source_compiler_free(compiler);}
static XrSourceStatus api_build(XrSourceCompiler *compiler,const char *path,const XrSourceText *text,
    XrSourceProgram **output,XrSourceDiagnostic **diagnostic) {
    if(!compiler || !output || *output || (diagnostic && *diagnostic))return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus available=api_resource(xr_compile_state_status(xr_source_compiler_state(compiler)));
    if(available!=XR_SOURCE_OK)return available;
    XrCompileResources *resources=xr_source_compiler_resources(compiler);
    XrSourceDiagnostic *detail=NULL;XrSourceProgram *program=NULL;
    XrSourceStatus status=XR_SOURCE_OK;
    if(diagnostic)status=api_resource(xr_compile_resources_calloc(resources,1,sizeof(*detail),(void **)&detail));
    if(status==XR_SOURCE_OK)status=api_resource(xr_compile_resources_calloc(resources,1,sizeof(*program),(void **)&program));
    if(status==XR_SOURCE_OK) {
        program->resources=resources;
        XrSourceCompileStatus built;
        if(text) {
            XrSourceCompilerText input={text->absolute_path,text->namespace_id,text->bytes,text->length};
            built=xr_source_compiler_build_text(compiler,&input,&program->product,detail?&detail->detail:NULL);
        } else built=xr_source_compiler_build_file(compiler,path,&program->product,detail?&detail->detail:NULL);
        status=api_compile(built);
        if(status==XR_SOURCE_OK) {
            const XrXirSourceProductFacts *facts=xr_xir_compile_source_product_facts(program->product);
            program->info=(XrSourceProgramInfo){facts->entry,facts->function_count,facts->module_count};
        }
    }
    if(status==XR_SOURCE_BUDGET || status==XR_SOURCE_OUT_OF_MEMORY)
        (void)xr_compile_state_fail(xr_source_compiler_state(compiler),status==XR_SOURCE_BUDGET?XR_COMPILE_RESOURCE_BUDGET:XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    if(status==XR_SOURCE_OK){*output=program;xray_source_diagnostic_free(detail);}
    else {
        xray_source_program_free(program);
        if(detail){if(detail->detail.status==XR_SOURCE_COMPILE_OK)detail->detail.status=status==XR_SOURCE_BUDGET?XR_SOURCE_COMPILE_BUDGET:XR_SOURCE_COMPILE_OUT_OF_MEMORY;*diagnostic=detail;}
    }
    return status;
}
XRAY_API XrSourceStatus xray_source_compile_file(XrSourceCompiler *compiler,const char *path,XrSourceProgram **output,XrSourceDiagnostic **diagnostic) {
    return api_build(compiler,path,NULL,output,diagnostic);
}
XRAY_API XrSourceStatus xray_source_compile_text(XrSourceCompiler *compiler,const XrSourceText *text,XrSourceProgram **output,XrSourceDiagnostic **diagnostic) {
    if(!text)return XR_SOURCE_BAD_ARGUMENT;
    return api_build(compiler,NULL,text,output,diagnostic);
}
XRAY_API XrSourceStatus xray_source_program_info(const XrSourceProgram *program,XrSourceProgramInfo *output) {
    if(!program || !output)return XR_SOURCE_BAD_ARGUMENT;*output=program->info;return XR_SOURCE_OK;
}
XRAY_API void xray_source_program_free(XrSourceProgram *program) {
    if(!program)return;
    xr_xir_compile_program_drop(program->code);xr_xir_compile_source_product_free(program->product);xr_compile_resources_free(program);
}
XRAY_API XrSourceStatus xray_source_diagnostic_info(const XrSourceDiagnostic *diagnostic,XrSourceDiagnosticInfo *output) {
    if(!diagnostic || !output)return XR_SOURCE_BAD_ARGUMENT;
    const XrSourceCompileDiagnostic *detail=&diagnostic->detail;
    const XrXirSourceDiagnostic *source=&detail->source.source;
    *output=(XrSourceDiagnosticInfo){api_compile(detail->status),(uint32_t)detail->stage,source->line,source->column,
        detail->source.source_path,source->message};return XR_SOURCE_OK;
}
XRAY_API void xray_source_diagnostic_free(XrSourceDiagnostic *diagnostic) {
    if(!diagnostic)return;xr_source_compile_diagnostic_free(&diagnostic->detail);xr_compile_resources_free(diagnostic);
}
XRAY_API XrSourceStatus xray_source_call_options_init(XrSourceCallOptions *options,size_t size) {
    if(!options || size!=sizeof(*options))return XR_SOURCE_BAD_ARGUMENT;
    XrXirInstanceConfig config;XrXirCallStatus status=xr_xir_instance_config_init(&config,sizeof(config));
    if(status!=XR_XIR_CALL_READY)return api_call(status);
    *options=(XrSourceCallOptions){XRAY_SOURCE_API_VERSION,sizeof(*options),config.metadata_limit,config.value_limit,
        config.call_limit,config.poll_limit,config.work_limit,config.depth_limit,1048576,NULL,NULL};return XR_SOURCE_OK;
}
static XrSourceStatus api_options_admit(const XrSourceCallOptions *options) {
    if(!options || options->api_version!=XRAY_SOURCE_API_VERSION || options->struct_size!=sizeof(*options))return XR_SOURCE_BAD_ARGUMENT;
    return options->metadata_limit<sizeof(XrSourceCall)?XR_SOURCE_LIMIT:XR_SOURCE_OK;
}
static XrSourceStatus api_config(const XrSourceCallOptions *options,XrSourceCall *call,XrXirInstanceConfig *config) {
    XrSourceStatus admitted=api_options_admit(options);if(admitted!=XR_SOURCE_OK)return admitted;
    XrXirCallStatus made=xr_xir_instance_config_init(config,sizeof(*config));if(made!=XR_XIR_CALL_READY)return api_call(made);
    config->metadata_limit=options->metadata_limit-sizeof(*call);config->value_limit=options->value_limit;
    config->call_limit=options->call_limit;config->poll_limit=options->poll_limit;config->work_limit=options->work_limit;config->depth_limit=options->depth_limit;
    call->write=options->write?options->write:api_stdio;call->context=options->context;
    call->sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,api_output,call,options->output_byte_limit};
    config->output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&call->sink};
    xr_xir_host_time_provider(&config->time);return XR_SOURCE_OK;
}
static XrSourceStatus api_program_code(XrSourceProgram *program) {
    if(!program)return XR_SOURCE_BAD_ARGUMENT;
    if(program->code)return XR_SOURCE_OK;
    XrXirStatus status=xr_xir_compile_source_product_vm_take(program->product,&program->code);
    return status==XR_XIR_OK?XR_SOURCE_OK:status==XR_XIR_BUDGET?XR_SOURCE_BUDGET:
        status==XR_XIR_OUT_OF_MEMORY?XR_SOURCE_OUT_OF_MEMORY:XR_SOURCE_INVALID;
}
XRAY_API XrSourceStatus xray_source_call_begin(XrSourceProgram *program,const XrSourceCallOptions *options,XrSourceCall **output) {
    if(!program || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus admitted=api_options_admit(options);if(admitted!=XR_SOURCE_OK)return admitted;
    XrSourceCall *call=NULL;XrSourceStatus status=api_resource(xr_compile_resources_calloc(program->resources,1,sizeof(*call),(void **)&call));
    if(status!=XR_SOURCE_OK)return status;
    call->resources=program->resources;XrXirInstanceConfig config;
    status=api_config(options,call,&config);
    if(status==XR_SOURCE_OK)status=api_program_code(program);
    if(status==XR_SOURCE_OK) {
        XrXirHostExecutionRequest request={program->code,&config,program->info.entry,NULL,0};
        status=api_call(xr_xir_host_call_begin(&request,&call->host));
    }
    if(status==XR_SOURCE_OK)*output=call;else xr_compile_resources_free(call);return status;
}
XRAY_API XrSourceStatus xray_source_call_poll(XrSourceCall *call,uint64_t quantum,XrSourceEvent *output) {
    if(!call || !output || !quantum)return XR_SOURCE_BAD_ARGUMENT;
    if(call->busy)return XR_SOURCE_BUSY;
    if(call->taken)return XR_SOURCE_BAD_STATE;
    call->busy=true;XrXirInstanceResult observed=xr_xir_host_call_step_bounded(call->host,quantum);call->busy=false;
    if(observed.outcome.status==XR_XIR_CALL_BUSY)return XR_SOURCE_BUSY;
    call->observed=observed.outcome.status;*output=api_event(observed.outcome.status,observed.epoch,observed.outcome.wake);return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_call_resume(XrSourceCall *call,uint64_t epoch,uint64_t wake) {
    if(!call)return XR_SOURCE_BAD_ARGUMENT;
    if(call->busy)return XR_SOURCE_BUSY;
    call->busy=true;XrXirCallStatus status=xr_xir_host_call_resume(call->host,epoch,wake);call->busy=false;
    if(status==XR_XIR_CALL_READY)call->observed=XR_XIR_CALL_READY;return api_call(status);
}
XRAY_API XrSourceStatus xray_source_call_wait_request(const XrSourceCall *call,uint64_t epoch,uint64_t wake,XrSourceWait *output) {
    if(!call || !output)return XR_SOURCE_BAD_ARGUMENT;
    if(call->busy)return XR_SOURCE_BUSY;
    XrXirWaitRequest request={0};XrXirCallStatus status=xr_xir_host_call_wait_request(call->host,epoch,wake,&request);
    if(status!=XR_XIR_CALL_READY)return api_call(status);
    XrSourceWaitKind kind=request.kind==XR_XIR_WAIT_YIELD?XR_SOURCE_WAIT_YIELD:request.kind==XR_XIR_WAIT_TIMER_MS?XR_SOURCE_WAIT_TIMER:XR_SOURCE_WAIT_TASK;
    *output=(XrSourceWait){kind,request.after_ms};return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_call_cancel(XrSourceCall *call) {
    if(!call)return XR_SOURCE_BAD_ARGUMENT;
    if(call->busy)return XR_SOURCE_BUSY;
    call->busy=true;XrXirCallStatus status=xr_xir_host_call_request_cancel(call->host);call->busy=false;
    if(status==XR_XIR_CALL_CANCEL_REQUESTED)call->observed=XR_XIR_CALL_READY;return api_call(status);
}
XRAY_API XrSourceStatus xray_source_call_take(XrSourceCall *call,XrSourceResult **output) {
    if(!call || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    if(call->busy)return XR_SOURCE_BUSY;
    if(call->taken || call->observed==XR_XIR_CALL_READY || call->observed==XR_XIR_CALL_SUSPENDED)return XR_SOURCE_BAD_STATE;
    XrSourceResult *result=NULL;XrSourceStatus status=api_resource(xr_compile_resources_calloc(call->resources,1,sizeof(*result),(void **)&result));
    if(status!=XR_SOURCE_OK)return status;
    call->busy=true;XrXirCallStatus taken=xr_xir_host_call_take(call->host,&result->value);call->busy=false;
    if(result->value.status!=XR_XIR_CALL_READY && taken==result->value.status){call->taken=true;*output=result;return XR_SOURCE_OK;}
    xray_source_result_free(result);return api_call(taken);
}
XRAY_API XrSourceStatus xray_source_call_drop(XrSourceCall **owner,XrSourceEvent *cleanup) {
    if(!owner)return XR_SOURCE_BAD_ARGUMENT;
    if(!*owner){if(cleanup)*cleanup=api_event(XR_XIR_CALL_READY,0,0);return XR_SOURCE_OK;}
    XrSourceCall *call=*owner;if(call->busy)return XR_SOURCE_BUSY;
    call->busy=true;XrXirCallStatus dropped=xr_xir_host_call_drop(call->host);
    if(dropped==XR_XIR_CALL_BUSY){call->busy=false;return XR_SOURCE_BUSY;}
    if(cleanup){*cleanup=api_event(dropped,0,0);if(dropped!=XR_XIR_CALL_READY)cleanup->state=XR_SOURCE_RUN_CLEANUP_FAILED;}xr_compile_resources_free(call);*owner=NULL;return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_execute(XrSourceProgram *program,const XrSourceCallOptions *options,XrSourceResult **output,XrSourceEvent *failure) {
    if(!program || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus admitted=api_options_admit(options);if(admitted!=XR_SOURCE_OK)return admitted;
    XrSourceResult *result=NULL;XrSourceStatus status=api_resource(xr_compile_resources_calloc(program->resources,1,sizeof(*result),(void **)&result));
    if(status!=XR_SOURCE_OK)return status;
    XrSourceCall sink={0};XrXirInstanceConfig config;status=api_config(options,&sink,&config);
    if(status==XR_SOURCE_OK)status=api_program_code(program);
    if(status==XR_SOURCE_OK) {
        XrXirHostExecutionRequest request={program->code,&config,program->info.entry,NULL,0};
        XrXirCallStatus executed=xr_xir_host_execute(&request,&result->value);
        if(result->value.status!=XR_XIR_CALL_READY && executed==result->value.status){*output=result;return XR_SOURCE_OK;}
        if(failure){*failure=api_event(executed,0,0);if(xr_xir_call_panic_status(executed) || executed==XR_XIR_CALL_DEFER_ASYNC)failure->state=XR_SOURCE_RUN_CLEANUP_FAILED;}
        status=api_call(executed);
    }
    xray_source_result_free(result);return status;
}
XRAY_API XrSourceStatus xray_source_result_event(const XrSourceResult *result,XrSourceEvent *output) {
    if(!result || !output)return XR_SOURCE_BAD_ARGUMENT;*output=api_event(result->value.status,0,result->value.wake);return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_result_i64(const XrSourceResult *result,int64_t *output) {
    if(!result || !output)return XR_SOURCE_BAD_ARGUMENT;if(result->value.value.type!=XR_XIR_I64)return XR_SOURCE_INVALID;
    *output=result->value.value.payload;return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_result_bytes(const XrSourceResult *result,const char **bytes,size_t *length) {
    if(!result || !bytes || !length)return XR_SOURCE_BAD_ARGUMENT;
    const XrXirValue *value=result->value.value.type==XR_XIR_STRING?&result->value.value:&result->value.panic.message;
    if(value->type!=XR_XIR_STRING)return XR_SOURCE_INVALID;
    XrValueFormatReader reader=xr_xir_value_format_reader(value);XrValueFormatView view={0};
    if(!reader.read(reader.context,(XrValueFormatNode){value,0, NULL},&view) || view.kind!=XR_VALUE_FORMAT_BYTES)return XR_SOURCE_INVALID;
    *bytes=(const char *)view.bytes;*length=view.size;return XR_SOURCE_OK;
}
typedef struct ApiFormat { XrSourceWrite write;void *context;XrSourceOutputStream stream;XrSourceWriteStatus status; } ApiFormat;
static int api_format_write(void *context,const void *bytes,size_t length) {
    ApiFormat *out=context;out->status=out->write(out->context,out->stream,bytes,length);return out->status==XR_SOURCE_WRITE_OK;
}
XRAY_API XrSourceStatus xray_source_result_format(const XrSourceResult *result,XrSourceWrite write,void *context) {
    if(!result)return XR_SOURCE_BAD_ARGUMENT;
    bool failure=result->value.status!=XR_XIR_CALL_RETURNED;
    ApiFormat out={write?write:api_stdio,context,failure?XR_SOURCE_STDERR:XR_SOURCE_STDOUT,XR_SOURCE_WRITE_OK};
    const XrXirValue *value=&result->value.value;
    XrValueFormatSink sink={&out,api_format_write};XrValueFormatReader reader=xr_xir_value_format_reader(value);
    int formatted;
    if(result->value.status==XR_XIR_CALL_THROWN || xr_xir_call_panic_status(result->value.status))
        formatted=xr_xir_host_report_result(&result->value,sink,false)==XR_XIR_HOST_REPORT_OK;
    else if(result->value.status==XR_XIR_CALL_RETURNED)
        formatted=xr_value_format_value(reader,(XrValueFormatNode){value,0, NULL},sink,0);
    else return XR_SOURCE_UNSUPPORTED;
    if(formatted)return XR_SOURCE_OK;
    return out.status==XR_SOURCE_WRITE_OOM?XR_SOURCE_OUT_OF_MEMORY:out.status==XR_SOURCE_WRITE_LIMIT?XR_SOURCE_LIMIT:
        out.status==XR_SOURCE_WRITE_IO?XR_SOURCE_IO:XR_SOURCE_INVALID;
}
XRAY_API void xray_source_result_free(XrSourceResult *result) {
    if(!result)return;xr_xir_call_result_drop(&result->value);xr_compile_resources_free(result);
}
