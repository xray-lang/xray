/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_ast_api.inc.c - Exact parser ownership at the public AST boundary
 */
#include <limits.h>

typedef struct ApiParseDiagnostic {
    XrSourceCompileDiagnostic *output;
} ApiParseDiagnostic;
static void api_parse_error(void *context,int line,int column,int end_line,int end_column,const char *message) {
    (void)end_line;(void)end_column;
    ApiParseDiagnostic *capture=context;
    if(!capture->output || capture->output->source.source.line)return;
    XrXirSourceDiagnostic *detail=&capture->output->source.source;
    capture->output->source.failure.origin=XR_XIR_SOURCE_DIAGNOSTIC_PARSE;
    (void)snprintf(detail->message,sizeof(detail->message),"%s",message?message:"");
    detail->line=line;detail->column=column;
}
XRAY_API void xray_source_ast_free(XrSourceAst *ast) {
    if(!ast)return;
    XR_CHECK(ast->references,"a parser AST reference is owned before release");
    if(--ast->references)return;
    xr_program_destroy(ast->program);
    xr_compile_state_free(ast->source);xr_compile_state_free(ast->path);xr_compile_state_free(ast->namespace_id);
    xr_compile_state_release(ast->state);xr_compile_state_free(ast);
}
static XrSourceStatus api_parse_owned(XrSourceCompiler *compiler,const XrSourceText *text,
    XrSourceAst *ast,XrSourceCompileDiagnostic *diagnostic,bool interactive) {
    XrCompileState *state=xr_source_compiler_state(compiler);
    ast->source_length=text->length;
    XrSourceStatus status=api_resource(xr_compile_state_strndup(state,text->bytes,text->length,&ast->source));
    if(status==XR_SOURCE_OK && text->absolute_path)
        status=api_resource(xr_compile_state_strdup(state,text->absolute_path,&ast->path));
    if(status==XR_SOURCE_OK && text->namespace_id)
        status=api_resource(xr_compile_state_strdup(state,text->namespace_id,&ast->namespace_id));
    size_t line=1,line_start=0;
    for(size_t i=0;status==XR_SOURCE_OK && i<text->length;++i) {
        if(!ast->source[i]) {
            if(line>INT_MAX || i-line_start>=INT_MAX)return XR_SOURCE_OVERFLOW;
            if(diagnostic) {
                XrXirSourceDiagnostic *detail=&diagnostic->source.source;
                diagnostic->source.failure.origin=XR_XIR_SOURCE_DIAGNOSTIC_INPUT;
                detail->line=(int)line;detail->column=(int)(i-line_start)+1;
                (void)snprintf(detail->message,sizeof(detail->message),"source text contains an embedded NUL");
            }
            return XR_SOURCE_INVALID;
        }
        if(ast->source[i]=='\n'){++line;line_start=i+1;}
    }
    if(status!=XR_SOURCE_OK)return status;
    ApiParseDiagnostic capture={diagnostic};
    XrParseDiagnostics options={api_parse_error,&capture,1};
    XrParseStatus parsed=interactive ? xr_compile_parse_repl_with_trivia(xr_source_compiler_session(compiler),
        ast->source,ast->path,&options,&ast->program) : xr_compile_parse_with_trivia(xr_source_compiler_session(compiler),
        ast->source,ast->path,&options,&ast->program);
    if(parsed==XR_PARSE_OK)return XR_SOURCE_OK;
    XrSourceStatus cause=api_resource(xr_compile_state_status(state));
    if(cause!=XR_SOURCE_OK)return cause;
    return parsed==XR_PARSE_OUT_OF_MEMORY ? XR_SOURCE_OUT_OF_MEMORY :
        parsed==XR_PARSE_BUDGET ? XR_SOURCE_COMPLEXITY : parsed==XR_PARSE_BAD_ARGUMENT ? XR_SOURCE_BAD_ARGUMENT :
        parsed==XR_PARSE_IO ? XR_SOURCE_IO : XR_SOURCE_INVALID;
}
static XrSourceStatus api_parse_text(XrSourceCompiler *compiler,const XrSourceText *text,
    XrSourceAst **output,XrSourceCompileDiagnostic *diagnostic,bool interactive) {
    if(!compiler || !text || !text->bytes || text->length==SIZE_MAX || !output || *output ||
        (text->absolute_path ? !*text->absolute_path || text->namespace_id :
            !text->namespace_id || !*text->namespace_id))return XR_SOURCE_BAD_ARGUMENT;
    XrCompileState *state=xr_source_compiler_state(compiler);
    XrSourceStatus status=api_resource(xr_compile_state_status(state));
    if(status!=XR_SOURCE_OK)return status;
    XrSourceAst *ast=NULL;
    status=api_resource(xr_compile_state_calloc(state,1,sizeof(*ast),(void **)&ast));
    if(status==XR_SOURCE_OK) {
        ast->references=1;
        status=api_resource(xr_compile_state_retain(state));
        if(status==XR_SOURCE_OK){ast->state=state;status=api_parse_owned(compiler,text,ast,diagnostic,interactive);}
    }
    if(status==XR_SOURCE_OK)*output=ast;
    else {
        if(diagnostic && ast && ast->path && !diagnostic->source.failure.path)
            (void)xr_compile_state_strdup(state,ast->path,&diagnostic->source.failure.path);
        xray_source_ast_free(ast);
    }
    return status;
}
XRAY_API XrSourceStatus xray_source_parse(const XrSourceText *text,const XrSourceCompileOptions *options,
    XrSourceAst **output,XrSourceFailure *failure,XrSourceDiagnostic **diagnostic) {
    XrSourceFailure fact={XR_SOURCE_BAD_ARGUMENT,XR_SOURCE_OK,XR_SOURCE_STAGE_OPTIONS,0,0};
    if(failure)*failure=fact;
    if(!output || *output || (diagnostic && *diagnostic))return fact.status;
    ApiBuildRequest request={0};XrSourceCompileDiagnostic detail={0};XrSourceAst *ast=NULL;
    XrSourceStatus status=api_request_begin(options,&request);
    if(status==XR_SOURCE_OK) {
        fact.stage=XR_SOURCE_STAGE_INPUT;
        status=api_parse_text(request.compiler,text,&ast,&detail,false);
    }
    if(status==XR_SOURCE_OK)status=api_resource(xr_compile_request_control_poll(&request.control));
    fact.status=status;fact.line=detail.source.source.line;fact.column=detail.source.source.column;
    if(status==XR_SOURCE_OK){fact.stage=XR_SOURCE_STAGE_PUBLISH;*output=ast;ast=NULL;}
    else api_diagnostic_snapshot(request.resources,&detail,status,diagnostic);
    xray_source_ast_free(ast);xr_source_compile_diagnostic_free(&detail);api_request_clear(&request);
    if(failure)*failure=fact;
    return status;
}
static XrXirStatus api_ast_retain(void *context,const XrXirSourceAst *input) {
    XrSourceAst *ast=context;
    if(!ast || !ast->state || !ast->program || !ast->references || !input ||
        input->program!=ast->program || input->source.logical_path!=ast->path)return XR_XIR_BAD_STRUCTURE;
    if(ast->references==UINT32_MAX)return XR_XIR_BUDGET;
    ++ast->references;return XR_XIR_OK;
}
static void api_ast_release(void *context) {xray_source_ast_free(context);}
XR_FUNC XrSourceStatus xr_source_ast_owned_input(const XrSourceAst *ast,XrXirSourceAstOwner *output) {
    if(!ast || !ast->state || !ast->program || !ast->references || !output || output->input.program ||
        output->input.source.logical_path || output->resources || output->owner || output->retain || output->release)
        return XR_SOURCE_BAD_ARGUMENT;
    *output=(XrXirSourceAstOwner){.input={{ast->path,ast->source,ast->source_length},ast->program},
        .resources=xr_compile_state_resources(ast->state),.owner=(void *)ast,
        .retain=api_ast_retain,.release=api_ast_release};
    return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_build_ast(const XrSourceAst *ast,const XrSourceCompileOptions *options,
    XrSourceArtifact **output,XrSourceFailure *failure,XrSourceDiagnostic **diagnostic) {
    return api_build_owned(NULL,ast,options,output,failure,diagnostic);
}
