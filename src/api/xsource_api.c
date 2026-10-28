/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_api.c - Public ownership at the ordinary compiler and HostCall boundary
 */
#include "xsource_api.h"
#include "../base/xcompile_request.h"
#include "../xir/xxir_emit_c.h"
#include "../xir/xxir_library_catalog.h"
#include "xsource_ast_owner.h"
#include "../../include/xray_source_ast.h"
#include "../../include/xray_source_entry.h"
#include "../../include/xray_source_instance.h"
#include "../../include/xray_source_repl.h"
#include "xsource_compiler.h"
#include "xsource_submission.h"
#include "../frontend/parser/xparse.h"
#include "xsource_paths.h"
#include "../execution/xr_xir_host_execution.h"
#include "../execution/xr_xir_host_cli.h"
#include "../execution/xr_xir_host_time.h"
#include "../xir/xxir_vm.h"
#include "../xir/xxir_output.h"
#include "../xir/xxir_format.h"
#include "../xir/xxir_nullable.h"
#include "../xir/xxir_compile_domain.h"
#include "../xir/xxir_error.h"
#include "../shared/xr_utf8_core.h"
#include "../base/xresource_limits.h"
#include <stdio.h>
#include <string.h>

typedef struct ApiNamedEntry {
    char *name;
    size_t length;
    uint32_t entry, arguments;
} ApiNamedEntry;
struct XrSourceArtifact {
    XrCompileResources *resources; /* Memory only; no request/control owner. */
    XrXirProgram *code;
    XrXirCSource c;
    XrSourceArtifactInfo info;
    ApiNamedEntry *names;
    uint32_t name_count, references;
};
struct XrSourceExecution { XrCompileResources *resources; };
typedef struct ApiBuildRequest {
    XrCompileResources *resources;
    XrCompileRequestControl control;
    XrSourceCompiler *compiler;
    XrStdlibPath stdlib;
} ApiBuildRequest;
struct XrSourceAst {
    XrCompileState *state;
    AstNode *program;
    char *source, *path, *namespace_id;
    uint32_t references;
    size_t source_length;
};
struct XrSourceResult { XrXirCallResult value; XrCompileResources *resources; };
struct XrSourceValue { XrXirValue value; XrCompileResources *resources; };
typedef struct ApiEntry {
    XrSourceArtifact *program;
    XrCompileResources *resources;
    uint32_t entry;
    const XrXirValue *arguments;
    uint32_t argument_count;
} ApiEntry;
struct XrSourceCall {
    XrCompileResources *resources;
    XrXirHostCall *host;
    XrSourceInstance *owner;
    XrXirOutputSink sink;
    XrSourceWrite write;
    void *context;
    XrXirCallStatus observed;
    bool taken, busy;
};
struct XrSourceInstance {
    XrCompileResources *resources;
    XrSourceArtifact *program;
    XrXirHostInstance *host;
    XrSourceCall sink;
    XrSourceCall *active;
    bool busy;
};
static XrSourceStatus api_resource(XrCompileResourceStatus status) {
    switch (status) {
    case XR_COMPILE_RESOURCE_OK: return XR_SOURCE_OK;
    case XR_COMPILE_RESOURCE_MEMORY_LIMIT: return XR_SOURCE_MEMORY_LIMIT;
    case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XR_SOURCE_OUT_OF_MEMORY;
    case XR_COMPILE_RESOURCE_OVERFLOW: return XR_SOURCE_OVERFLOW;
    case XR_COMPILE_RESOURCE_CANCELLED: return XR_SOURCE_CANCELLED;
    case XR_COMPILE_RESOURCE_DEADLINE: return XR_SOURCE_DEADLINE;
    case XR_COMPILE_RESOURCE_CLOCK_FAILURE: return XR_SOURCE_CLOCK_FAILURE;
    default: return XR_SOURCE_BAD_ARGUMENT;
    }
}
XRAY_API XrSourceCompileOptions xray_source_compile_options_default(void) {
    return (XrSourceCompileOptions){.target=XR_SOURCE_TARGET_VM,
        .memory_mode=XR_SOURCE_MEMORY_ORDINARY,.deadline_mode=XR_SOURCE_DEADLINE_NONE};
}
XRAY_API XrSourceExecutionOptions xray_source_execution_options_default(void) {
    return (XrSourceExecutionOptions){XR_SOURCE_MEMORY_ORDINARY,0};
}
static XrSourceStatus api_memory_limits(XrSourceMemoryMode mode,uint64_t limit,
                                       int profile,XrCompileResourceLimits *output) {
    if (!output || (profile!=0 && profile!=1) ||
        (mode!=XR_SOURCE_MEMORY_ORDINARY && mode!=XR_SOURCE_MEMORY_FINITE) ||
        (mode==XR_SOURCE_MEMORY_ORDINARY && limit!=0))
        return XR_SOURCE_BAD_ARGUMENT;
    *output=(XrCompileResourceLimits){mode==XR_SOURCE_MEMORY_ORDINARY
        ? XR_COMPILE_MEMORY_ORDINARY : XR_COMPILE_MEMORY_FINITE,limit,profile!=0};
    return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_execution_new(const XrSourceExecutionOptions *options,
                                                XrSourceExecution **output) {
    if (!options || !output || *output) return XR_SOURCE_BAD_ARGUMENT;
    XrCompileResourceLimits limits;
    XrSourceStatus status=api_memory_limits(options->memory_mode,options->memory_limit,0,&limits);
    XrCompileResources *resources=NULL;
    if (status==XR_SOURCE_OK && options->memory_mode==XR_SOURCE_MEMORY_FINITE)
        return XR_SOURCE_UNSUPPORTED;
    if (status==XR_SOURCE_OK) status=api_resource(xr_compile_resources_new(&limits,&resources));
    XrSourceExecution *owner=NULL;
    if (status==XR_SOURCE_OK)
        status=api_resource(xr_compile_resources_calloc(resources,1,sizeof(*owner),(void **)&owner));
    if (status==XR_SOURCE_OK) {owner->resources=resources;*output=owner;}
    /* Each live block retains the memory owner, independently of this handle. */
    xr_compile_resources_release(resources);
    return status;
}
XRAY_API void xray_source_execution_free(XrSourceExecution *execution) {
    xr_compile_resources_free(execution);
}
static XrSourceStatus api_compile(XrSourceCompileStatus status) {
    switch(status) {
    case XR_SOURCE_COMPILE_OK:return XR_SOURCE_OK;
    case XR_SOURCE_COMPILE_BAD_ARGUMENT:return XR_SOURCE_BAD_ARGUMENT;
    case XR_SOURCE_COMPILE_NOT_FOUND:return XR_SOURCE_NOT_FOUND;
    case XR_SOURCE_COMPILE_INVALID:return XR_SOURCE_INVALID;
    case XR_SOURCE_COMPILE_LIMIT:return XR_SOURCE_LIMIT;
    case XR_SOURCE_COMPILE_BUDGET:return XR_SOURCE_COMPLEXITY;
    case XR_SOURCE_COMPILE_OUT_OF_MEMORY:return XR_SOURCE_OUT_OF_MEMORY;
    case XR_SOURCE_COMPILE_MEMORY_LIMIT:return XR_SOURCE_MEMORY_LIMIT;
    case XR_SOURCE_COMPILE_OVERFLOW:return XR_SOURCE_OVERFLOW;
    case XR_SOURCE_COMPILE_CANCELLED:return XR_SOURCE_CANCELLED;
    case XR_SOURCE_COMPILE_DEADLINE:return XR_SOURCE_DEADLINE;
    case XR_SOURCE_COMPILE_CLOCK_FAILURE:return XR_SOURCE_CLOCK_FAILURE;
    case XR_SOURCE_COMPILE_IO:return XR_SOURCE_IO;
    case XR_SOURCE_COMPILE_UNSUPPORTED:return XR_SOURCE_UNSUPPORTED;
    default:return XR_SOURCE_REJECTED;
    }
}
static XrSourceStatus api_xir(XrXirStatus status) {
    switch (status) {
    case XR_XIR_OK:return XR_SOURCE_OK;
    case XR_XIR_OUT_OF_MEMORY:return XR_SOURCE_OUT_OF_MEMORY;
    case XR_XIR_MEMORY_LIMIT:return XR_SOURCE_MEMORY_LIMIT;
    case XR_XIR_OVERFLOW:return XR_SOURCE_OVERFLOW;
    case XR_XIR_CANCELLED:return XR_SOURCE_CANCELLED;
    case XR_XIR_DEADLINE:return XR_SOURCE_DEADLINE;
    case XR_XIR_CLOCK_FAILURE:return XR_SOURCE_CLOCK_FAILURE;
    case XR_XIR_BUDGET:return XR_SOURCE_COMPLEXITY;
    case XR_XIR_UNSUPPORTED:return XR_SOURCE_UNSUPPORTED;
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
    if(length>call->sink.byte_limit)return XR_XIR_OUTPUT_LIMIT;
    XrSourceOutputStream public_stream=stream==XR_XIR_STDOUT?XR_SOURCE_STDOUT:XR_SOURCE_STDERR;
    XrSourceWriteStatus status=call->write(call->context,public_stream,bytes,length);
    switch(status) {
    case XR_SOURCE_WRITE_OK:call->sink.byte_limit-=length;return XR_XIR_OUTPUT_OK;
    case XR_SOURCE_WRITE_OOM:return XR_XIR_OUTPUT_OOM;
    case XR_SOURCE_WRITE_LIMIT:return XR_XIR_OUTPUT_LIMIT;
    default:return XR_XIR_OUTPUT_ERROR;
    }
}
static XrSourceStatus api_artifact_prepare(XrXirSourceProduct **product,
    XrSourceTarget target,XrSourceArtifact **output);
static bool api_request_cancel(const void *context) {
    const XrSourceCompileOptions *options=context;
    return options->cancel_poll && options->cancel_poll(options->cancel_context)!=0;
}
static void api_request_clear(ApiBuildRequest *request) {
    xr_source_compiler_free(request->compiler);
    xr_source_stdlib_path_free(&request->stdlib);
    xr_compile_resources_release(request->resources);
    *request=(ApiBuildRequest){0};
}
static XrSourceStatus api_request_begin(const XrSourceCompileOptions *options,
                                       ApiBuildRequest *request) {
    if (!options || !request || (options->target!=XR_SOURCE_TARGET_VM && options->target!=XR_SOURCE_TARGET_C) ||
        (options->deadline_mode!=XR_SOURCE_DEADLINE_NONE && options->deadline_mode!=XR_SOURCE_DEADLINE_AFTER) ||
        (options->deadline_mode==XR_SOURCE_DEADLINE_NONE && options->timeout_ns!=0) ||
        (!options->cancel_poll && options->cancel_context))
        return XR_SOURCE_BAD_ARGUMENT;
    XrCompileResourceLimits limits;
    XrSourceStatus status=api_memory_limits(options->memory_mode,options->memory_limit,
                                            options->profile_allocations,&limits);
    XrCompileRequestOptions control={options->cancel_poll?api_request_cancel:NULL,
        options->cancel_poll?options:NULL,options->deadline_mode==XR_SOURCE_DEADLINE_AFTER
            ? XR_COMPILE_DEADLINE_AFTER : XR_COMPILE_DEADLINE_NONE,options->timeout_ns};
    if (status==XR_SOURCE_OK)
        status=api_resource(xr_compile_request_control_init(&control,&request->control));
    if (status==XR_SOURCE_OK) status=api_resource(xr_compile_request_control_poll(&request->control));
    if (status==XR_SOURCE_OK) status=api_resource(xr_compile_resources_new(&limits,&request->resources));
    const char *stdlib=options->stdlib_path;
    if (status==XR_SOURCE_OK && !stdlib) {
        status=api_compile(xr_source_stdlib_path(request->resources,&request->stdlib,NULL));
        stdlib=request->stdlib.path;
    }
    if (status==XR_SOURCE_OK) {
        XrSourceCompilerConfig config={{request->resources,xr_xir_compile_default_limits()},
            xr_xir_host_target(),stdlib,xr_source_compile_default_manifest_limits()};
        status=api_compile(xr_source_compiler_new(&config,&request->control,&request->compiler,NULL));
    }
    return status;
}
static void api_diagnostic_take(XrCompileResources *resources,XrSourceCompileDiagnostic *source,
    XrSourceStatus status,XrSourceDiagnostic **output) {
    if(!resources||!output||status==XR_SOURCE_OK)return;
    if(!source->source.failure.report) {
        XrFrontendReport *report=NULL;
        XrCompileResourceStatus saved=xr_frontend_report_new(resources,&report);
        uint32_t file=0;
        if(saved==XR_COMPILE_RESOURCE_OK) saved=xr_frontend_report_source(report,
            source->source.failure.path,NULL,0,NULL,&file);
        if(saved==XR_COMPILE_RESOURCE_OK) saved=xr_frontend_report_add(report,0,
            source->status!=XR_SOURCE_COMPILE_OK?xr_source_compile_status_name(source->status):"source request failed",
            xr_frontend_report_location(report,file,0,0,NULL),NULL,0);
        if(saved!=XR_COMPILE_RESOURCE_OK){xr_frontend_report_free(report);return;}
        xr_frontend_report_seal(report,false);source->source.failure.report=report;
    }
    *output=source->source.failure.report;source->source.failure.report=NULL;
}
static XrSourceStatus api_build_owned(const XrSourceInput *input,const XrSourceAst *ast,
    const XrSourceCompileOptions *options,XrSourceArtifact **output,XrSourceFailure *failure,
    XrSourceDiagnostic **diagnostic) {
    XrSourceFailure fact={XR_SOURCE_BAD_ARGUMENT,XR_SOURCE_OK,XR_SOURCE_STAGE_OPTIONS};
    if (failure) *failure=fact;
    if ((!ast && !input) || (ast && input) || !output || *output || (diagnostic && *diagnostic) ||
        (input && input->kind!=XR_SOURCE_INPUT_FILE && input->kind!=XR_SOURCE_INPUT_TEXT))
        return fact.status;
    ApiBuildRequest request={0};XrSourceCompileDiagnostic detail={0};
    XrXirSourceProduct *product=NULL;XrSourceArtifact *artifact=NULL;
    XrSourceStatus status=api_request_begin(options,&request);
    if (status==XR_SOURCE_OK) {
        fact.stage=XR_SOURCE_STAGE_INPUT;
        status=api_resource(xr_compile_request_control_poll(&request.control));
    }
    if (status==XR_SOURCE_OK) {
        fact.stage=XR_SOURCE_STAGE_COMPILE;
        if (ast) {
            XrSourceCompilerText source={ast->path,ast->namespace_id,ast->source,ast->source_length};
            status=api_compile(xr_source_compiler_build_ast(request.compiler,&source,ast->program,&product,&detail));
        } else if (input->kind==XR_SOURCE_INPUT_TEXT) {
            const XrSourceText *text=&input->value.text;
            XrSourceCompilerText source={text->absolute_path,text->namespace_id,text->bytes,text->length};
            status=api_compile(xr_source_compiler_build_text(request.compiler,&source,&product,&detail));
        } else {
            status=api_compile(xr_source_compiler_build_file(request.compiler,
                input->value.absolute_path,&product,&detail));
        }
        if (status!=XR_SOURCE_OK) {
            XrSourceStatus cause=api_resource(xr_compile_state_status(xr_source_compiler_state(request.compiler)));
            if (cause!=XR_SOURCE_OK) status=cause;
        }
    }
    if (status==XR_SOURCE_OK) status=api_resource(xr_compile_request_control_poll(&request.control));
    if (status==XR_SOURCE_OK) {
        fact.stage=XR_SOURCE_STAGE_PREPARE;
        status=api_artifact_prepare(&product,options->target,&artifact);
    }
    if (status==XR_SOURCE_OK) {
        fact.stage=XR_SOURCE_STAGE_PUBLISH;
        status=api_resource(xr_compile_request_control_poll(&request.control));
    }
    fact.status=status;
    if (status==XR_SOURCE_OK) {*output=artifact;artifact=NULL;}
    else api_diagnostic_take(request.resources,&detail,status,diagnostic);
    xray_source_artifact_free(artifact);
    xr_xir_compile_source_product_free(product);
    xr_source_compile_diagnostic_free(&detail);
    api_request_clear(&request);
    if (failure) *failure=fact;
    return status;
}
XRAY_API XrSourceStatus xray_source_build(const XrSourceInput *input,const XrSourceCompileOptions *options,
    XrSourceArtifact **output,XrSourceFailure *failure,XrSourceDiagnostic **diagnostic) {
    return api_build_owned(input,NULL,options,output,failure,diagnostic);
}
#include "xsource_ast_api.inc.c"
XRAY_API XrSourceStatus xray_source_artifact_info(const XrSourceArtifact *program,XrSourceArtifactInfo *output) {
    if(!program || !output)return XR_SOURCE_BAD_ARGUMENT;*output=program->info;return XR_SOURCE_OK;
}
XRAY_API void xray_source_artifact_free(XrSourceArtifact *artifact) {
    if (!artifact) return;
    XR_CHECK(artifact->references,"an artifact reference is owned before release");
    if (--artifact->references) return;
    xr_xir_compile_program_drop(artifact->code);
    xr_xir_compile_c_source_free(&artifact->c);
    for (uint32_t i=0;i<artifact->name_count;++i) xr_compile_resources_free(artifact->names[i].name);
    xr_compile_resources_free(artifact->names);
    xr_compile_resources_free(artifact);
}
XRAY_API XrSourceStatus xray_source_artifact_c_text(const XrSourceArtifact *artifact,
    const char **bytes,size_t *length) {
    if (!artifact || !bytes || !length) return XR_SOURCE_BAD_ARGUMENT;
    if (artifact->info.target!=XR_SOURCE_TARGET_C) return XR_SOURCE_BAD_STATE;
    *bytes=artifact->c.text;*length=artifact->c.length;return XR_SOURCE_OK;
}
static XrSourceDiagnosticLocation api_diagnostic_location(XrFrontendLocation v) {
    XrSourceDiagnosticLocation out={v.path,v.start_byte,v.end_byte,v.line,v.column,v.end_line,v.end_column,
        v.utf16_line,v.utf16_column,v.utf16_end_line,v.utf16_end_column,v.flags,{0}};
    memcpy(out.revision,v.revision,32);return out;
}
XRAY_API XrSourceStatus xray_source_diagnostic_report(const XrSourceDiagnostic *report,XrSourceDiagnosticReportInfo *out) {
    XrFrontendReportInfo view;
    if(!out||!xr_frontend_report_info(report,&view))return XR_SOURCE_BAD_ARGUMENT;
    *out=(XrSourceDiagnosticReportInfo){view.roots,view.complete};return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_diagnostic_root(const XrSourceDiagnostic *report,uint32_t root,XrSourceDiagnosticRoot *out) {
    XrFrontendRoot view;
    if(!out||!xr_frontend_report_root(report,root,&view))return XR_SOURCE_BAD_ARGUMENT;
    *out=(XrSourceDiagnosticRoot){view.code,view.severity,view.message,api_diagnostic_location(view.primary),view.labels};return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_diagnostic_label(const XrSourceDiagnostic *report,uint32_t root,uint32_t label,XrSourceDiagnosticLabel *out) {
    XrFrontendLabel view;
    if(!out||!xr_frontend_report_label(report,root,label,&view))return XR_SOURCE_BAD_ARGUMENT;
    *out=(XrSourceDiagnosticLabel){view.message,api_diagnostic_location(view.location)};return XR_SOURCE_OK;
}
XRAY_API void xray_source_diagnostic_free(XrSourceDiagnostic *report) {xr_frontend_report_free(report);}
XRAY_API XrSourceStatus xray_source_call_options_init(XrSourceCallOptions *options,size_t size) {
    if(!options || size!=sizeof(*options))return XR_SOURCE_BAD_ARGUMENT;
    XrXirInstanceConfig config;XrXirCallStatus status=xr_xir_host_config_init(&config,sizeof(config),false);
    if(status!=XR_XIR_CALL_READY)return api_call(status);
    *options=(XrSourceCallOptions){XRAY_SOURCE_EXECUTION_ABI_VERSION,sizeof(*options),config.metadata_limit,config.value_limit,
        config.call_limit,config.poll_limit,config.work_limit,config.depth_limit,XR_RESOURCE_DEFAULT_SIZE_LIMIT,NULL,NULL};return XR_SOURCE_OK;
}
static XrSourceStatus api_options_admit(const XrSourceCallOptions *options) {
    if(!options || options->api_version!=XRAY_SOURCE_EXECUTION_ABI_VERSION || options->struct_size!=sizeof(*options))return XR_SOURCE_BAD_ARGUMENT;
    return options->metadata_limit<sizeof(XrSourceCall)?XR_SOURCE_LIMIT:XR_SOURCE_OK;
}
static XrSourceStatus api_config(const XrSourceCallOptions *options,XrSourceCall *call,XrXirInstanceConfig *config) {
    XrSourceStatus admitted=api_options_admit(options);if(admitted!=XR_SOURCE_OK)return admitted;
    XrXirCallStatus made=xr_xir_host_config_init(config,sizeof(*config),false);if(made!=XR_XIR_CALL_READY)return api_call(made);
    config->metadata_limit=options->metadata_limit-sizeof(*call);config->value_limit=options->value_limit;
    config->call_limit=options->call_limit;config->poll_limit=options->poll_limit;config->work_limit=options->work_limit;config->depth_limit=options->depth_limit;
    call->write=options->write?options->write:api_stdio;call->context=options->context;
    call->sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,api_output,call,options->output_byte_limit};
    config->output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&call->sink};
    return XR_SOURCE_OK;
}
static XrSourceStatus api_ready_vm(const XrSourceArtifact *artifact) {
    return !artifact ? XR_SOURCE_BAD_ARGUMENT :
        artifact->info.target!=XR_SOURCE_TARGET_VM || !artifact->code ? XR_SOURCE_BAD_STATE : XR_SOURCE_OK;
}
static XrSourceStatus api_prepare_names(XrXirSourceProduct *product,XrSourceArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_source_product_module(product);
    if(!module || !module->declarations)return XR_SOURCE_INVALID;
    const XrXirDeclarations *declarations=module->declarations;
    XrSourceStatus status=XR_SOURCE_OK;
    if(module->function_count)
        status=api_resource(xr_compile_resources_calloc(artifact->resources,module->function_count,
            sizeof(*artifact->names),(void **)&artifact->names));
    for(uint32_t f=0;status==XR_SOURCE_OK && f<module->function_count;++f) {
        const XrXirFunctionIdentity *identity=&declarations->functions[f];
        const XrXirFunction *function=&module->functions[f];
        if(identity->module!=declarations->root_module || !identity->exported ||
            identity->method_kind!=XR_XIR_NON_MEMBER || identity->cleanup_owner || !function->name_length)
            continue;
        if((uint64_t)function->name_length>=SIZE_MAX){status=XR_SOURCE_OVERFLOW;break;}
        ApiNamedEntry *entry=&artifact->names[artifact->name_count];
        status=api_resource(xr_compile_resources_calloc(artifact->resources,
            (size_t)function->name_length+1u,1,(void **)&entry->name));
        if(status!=XR_SOURCE_OK)break;
        memcpy(entry->name,function->name,function->name_length);
        entry->length=function->name_length;entry->entry=f;entry->arguments=function->parameter_count;
        ++artifact->name_count;
    }
    artifact->info.named_entries=artifact->name_count;
    return status;
}
static XrSourceStatus api_artifact_prepare(XrXirSourceProduct **owner,XrSourceTarget target,
                                          XrSourceArtifact **output) {
    if (!owner || !*owner || !output || *output ||
        (target!=XR_SOURCE_TARGET_VM && target!=XR_SOURCE_TARGET_C)) return XR_SOURCE_BAD_ARGUMENT;
    XrXirSourceProduct *product=*owner;
    const XrXirCompileContext *context=xr_xir_compile_source_product_context(product);
    const XrXirSourceProductFacts *facts=xr_xir_compile_source_product_facts(product);
    if (!context || !context->resources || !facts) return XR_SOURCE_INVALID;
    XrXirStatus admitted=xr_xir_compile_source_product_public_admit(product,NULL);
    if (admitted!=XR_XIR_OK) return api_xir(admitted);
    XrSourceArtifact *artifact=NULL;
    XrSourceStatus status=api_resource(xr_compile_resources_calloc(context->resources,1,
        sizeof(*artifact),(void **)&artifact));
    if (status!=XR_SOURCE_OK) return status;
    artifact->resources=context->resources;artifact->references=1;
    artifact->info=(XrSourceArtifactInfo){target,facts->entry,facts->function_count,facts->module_count,0};
    if (target==XR_SOURCE_TARGET_VM) {
        status=api_prepare_names(product,artifact);
        if (status==XR_SOURCE_OK) {
            XrXirStatus ready=xr_xir_compile_source_product_vm_take(product,&artifact->code);
            status=api_xir(ready);
        }
    } else {
        /* Target dispatch precedes the consuming VM projection. C text has no
         * native execution authority and never enters a VM preparation path. */
        XrXirStatus emitted=xr_xir_compile_source_product_emit(product,"source_artifact",SIZE_MAX,&artifact->c);
        status=api_xir(emitted);
    }
    if (status!=XR_SOURCE_OK) {xray_source_artifact_free(artifact);return status;}
    xr_xir_compile_source_product_free(product);*owner=NULL;*output=artifact;return XR_SOURCE_OK;
}
XR_FUNC XrSourceStatus xr_source_artifact_take_product(XrXirSourceProduct **product,
    XrSourceTarget target,XrSourceArtifact **output) {
    return api_artifact_prepare(product,target,output);
}
XR_FUNC XrSourceStatus xr_source_artifact_take_vm(XrSourceArtifact **owner,XrXirProgram **output,
                                                uint32_t *entry) {
    if (!owner || !*owner || !output || *output || !entry) return XR_SOURCE_BAD_ARGUMENT;
    XrSourceArtifact *artifact=*owner;
    if (artifact->references!=1) return XR_SOURCE_BAD_STATE;
    XrSourceStatus status=api_ready_vm(artifact);
    if (status!=XR_SOURCE_OK) return status;
    *output=artifact->code;*entry=artifact->info.entry;artifact->code=NULL;
    xray_source_artifact_free(artifact);*owner=NULL;return XR_SOURCE_OK;
}
static XrSourceStatus api_call_begin_entry(const ApiEntry *entry,
    const XrSourceCallOptions *options,XrSourceCall **output) {
    XrSourceArtifact *program=entry ? entry->program : NULL;
    if(!program || !entry->resources || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus admitted=api_options_admit(options);if(admitted!=XR_SOURCE_OK)return admitted;
    XrSourceCall *call=NULL;XrSourceStatus status=api_resource(xr_compile_resources_calloc(entry->resources,1,sizeof(*call),(void **)&call));
    if(status!=XR_SOURCE_OK)return status;
    call->resources=entry->resources;XrXirInstanceConfig config;
    status=api_config(options,call,&config);
    if(status==XR_SOURCE_OK)status=api_ready_vm(program);
    if(status==XR_SOURCE_OK) {
        XrXirHostExecutionRequest request={program->code,&config,entry->entry,entry->arguments,entry->argument_count};
        status=api_call(xr_xir_host_call_begin(&request,&call->host));
    }
    if(status==XR_SOURCE_OK)*output=call;else xr_compile_resources_free(call);return status;
}
XRAY_API XrSourceStatus xray_source_artifact_begin(XrSourceExecution *execution,XrSourceArtifact *program,
    const XrSourceCallOptions *options,XrSourceCall **output) {
    if(!execution || !program)return XR_SOURCE_BAD_ARGUMENT;
    const ApiEntry entry={program,execution->resources,program->info.entry,NULL,0};
    return api_call_begin_entry(&entry,options,output);
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
XRAY_API XrSourceStatus xray_source_call_wait_request(const XrSourceCall *call,uint64_t epoch,uint64_t wake,XrSourceWait *output,size_t size) {
    if(!call || !output || size!=sizeof(*output))return XR_SOURCE_BAD_ARGUMENT;
    if(call->busy)return XR_SOURCE_BUSY;
    XrXirWaitRequest request={0};XrXirCallStatus status=xr_xir_host_call_wait_request(call->host,epoch,wake,&request);
    if(status!=XR_XIR_CALL_READY)return api_call(status);
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
    result->resources=call->resources;
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
    if(cleanup){*cleanup=api_event(dropped,0,0);if(dropped!=XR_XIR_CALL_READY)cleanup->state=XR_SOURCE_RUN_CLEANUP_FAILED;}
    if(call->owner)call->owner->active=NULL;
    xr_compile_resources_free(call);*owner=NULL;return XR_SOURCE_OK;
}
static XrSourceStatus api_execute_entry(const ApiEntry *entry,
    const XrSourceCallOptions *options,XrSourceResult **output,XrSourceEvent *failure) {
    XrSourceArtifact *program=entry ? entry->program : NULL;
    if(!program || !entry->resources || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus admitted=api_options_admit(options);if(admitted!=XR_SOURCE_OK)return admitted;
    XrSourceResult *result=NULL;XrSourceStatus status=api_resource(xr_compile_resources_calloc(entry->resources,1,sizeof(*result),(void **)&result));
    if(status!=XR_SOURCE_OK)return status;
    result->resources=entry->resources;
    XrSourceCall sink={0};XrXirInstanceConfig config;status=api_config(options,&sink,&config);
    if(status==XR_SOURCE_OK)status=api_ready_vm(program);
    if(status==XR_SOURCE_OK) {
        XrXirHostExecutionRequest request={program->code,&config,entry->entry,entry->arguments,entry->argument_count};
        XrXirCallStatus executed=xr_xir_host_execute(&request,&result->value);
        if(result->value.status!=XR_XIR_CALL_READY && executed==result->value.status){*output=result;return XR_SOURCE_OK;}
        if(failure){*failure=api_event(executed,0,0);if(xr_xir_call_panic_status(executed) || executed==XR_XIR_CALL_DEFER_ASYNC)failure->state=XR_SOURCE_RUN_CLEANUP_FAILED;}
        status=api_call(executed);
    }
    xray_source_result_free(result);return status;
}
XRAY_API XrSourceStatus xray_source_artifact_execute(XrSourceExecution *execution,XrSourceArtifact *program,
    const XrSourceCallOptions *options,XrSourceResult **output,XrSourceEvent *failure) {
    if(!execution || !program)return XR_SOURCE_BAD_ARGUMENT;
    const ApiEntry entry={program,execution->resources,program->info.entry,NULL,0};
    return api_execute_entry(&entry,options,output,failure);
}
#include "xsource_entry_api.inc.c"
#include "xsource_instance_api.inc.c"
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
    XrXirFormatContext format={0};XrValueFormatReader reader=xr_xir_value_format_reader(&format);XrValueFormatView view={0};
    if(!reader.read(reader.context,(XrValueFormatNode){value,0},&view) || view.kind!=XR_VALUE_FORMAT_BYTES)return XR_SOURCE_INVALID;
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
    XrXirFormatContext format={0};
    XrValueFormatSink sink={&out,api_format_write};XrValueFormatReader reader=xr_xir_value_format_reader(&format);
    const XrXirValue *value=&result->value.value;
    int formatted;
    if(result->value.status==XR_XIR_CALL_THROWN || xr_xir_call_panic_status(result->value.status))
        formatted=xr_xir_host_report_result(&result->value,sink,false)==XR_XIR_HOST_REPORT_OK;
    else if(result->value.status==XR_XIR_CALL_RETURNED)
        formatted=xr_value_format_value(reader,(XrValueFormatNode){value,0},sink,0);
    else return XR_SOURCE_UNSUPPORTED;
    if(formatted)return XR_SOURCE_OK;
    return out.status==XR_SOURCE_WRITE_OOM?XR_SOURCE_OUT_OF_MEMORY:out.status==XR_SOURCE_WRITE_LIMIT?XR_SOURCE_LIMIT:
        out.status==XR_SOURCE_WRITE_IO?XR_SOURCE_IO:XR_SOURCE_INVALID;
}
XRAY_API void xray_source_result_free(XrSourceResult *result) {
    if(!result)return;xr_xir_call_result_drop(&result->value);xr_compile_resources_free(result);
}
#include "xsource_value_api.inc.c"
#include "xsource_value_projection_api.inc.c"
#include "xsource_repl_api.inc.c"
