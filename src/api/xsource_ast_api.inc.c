/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_ast_api.inc.c - Exact parser ownership at the public AST boundary
 */
typedef struct ApiParseDiagnostic {
    XrCompileState *state;
    XrSourceDiagnostic *output;
} ApiParseDiagnostic;
static void api_parse_error(void *context,int line,int column,int end_line,int end_column,const char *message) {
    (void)end_line;(void)end_column;
    ApiParseDiagnostic *capture=context;
    if(!capture->output || capture->output->detail.source.source.line)return;
    XrXirSourceDiagnostic *detail=&capture->output->detail.source.source;
    detail->origin=XR_XIR_SOURCE_DIAGNOSTIC_PARSE;
    size_t size=0;
    if(xr_compile_state_string_length(capture->state,message,&size)!=XR_COMPILE_RESOURCE_OK)return;
    if(size>=sizeof(detail->message))size=sizeof(detail->message)-1;
    if(xr_compile_state_copy(capture->state,detail->message,message,size)!=XR_COMPILE_RESOURCE_OK)return;
    detail->message[size]=0;detail->line=line;detail->column=column;
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
    XrSourceAst *ast,XrSourceDiagnostic *diagnostic,bool interactive) {
    XrCompileState *state=xr_source_compiler_state(compiler);
    XrSourceStatus status=api_resource(xr_compile_state_strndup(state,text->bytes,text->length,&ast->source));
    if(status==XR_SOURCE_OK && text->absolute_path)
        status=api_resource(xr_compile_state_strdup(state,text->absolute_path,&ast->path));
    if(status==XR_SOURCE_OK && text->namespace_id)
        status=api_resource(xr_compile_state_strdup(state,text->namespace_id,&ast->namespace_id));
    if(status==XR_SOURCE_OK && diagnostic && ast->path)
        status=api_resource(xr_compile_state_strdup(state,ast->path,&diagnostic->detail.source.source_path));
    for(size_t i=0;status==XR_SOURCE_OK && i<text->length;++i) {
        status=api_resource(xr_compile_state_work(state,1));
        if(status==XR_SOURCE_OK && !ast->source[i]) {
            status=XR_SOURCE_INVALID;
            if(diagnostic) {
                static const char message[]="source text contains an embedded NUL";
                XrCompileResourceStatus copied=xr_compile_state_copy(state,diagnostic->detail.source.source.message,message,sizeof(message));
                if(copied!=XR_COMPILE_RESOURCE_OK)status=api_resource(copied);
            }
        }
    }
    if(status!=XR_SOURCE_OK)return status;
    ApiParseDiagnostic capture={state,diagnostic};
    XrParseDiagnostics options={api_parse_error,&capture,1};
    XrParseStatus parsed=interactive ? xr_compile_parse_repl_with_trivia(xr_source_compiler_session(compiler),
        ast->source,ast->path,&options,&ast->program) : xr_compile_parse_with_trivia(xr_source_compiler_session(compiler),
        ast->source,ast->path,&options,&ast->program);
    return parsed==XR_PARSE_OK ? XR_SOURCE_OK : parsed==XR_PARSE_OUT_OF_MEMORY ? XR_SOURCE_OUT_OF_MEMORY :
        parsed==XR_PARSE_BUDGET ? XR_SOURCE_BUDGET : parsed==XR_PARSE_BAD_ARGUMENT ? XR_SOURCE_BAD_ARGUMENT :
        parsed==XR_PARSE_IO ? XR_SOURCE_IO : XR_SOURCE_INVALID;
}
static XrSourceStatus api_parse_text(XrSourceCompiler *compiler,const XrSourceText *text,
    XrSourceAst **output,XrSourceDiagnostic **diagnostic,bool interactive) {
    if(!compiler || !text || !text->bytes || text->length==SIZE_MAX || !output || *output ||
        (diagnostic && *diagnostic) || (text->absolute_path ? !*text->absolute_path || text->namespace_id :
            !text->namespace_id || !*text->namespace_id))return XR_SOURCE_BAD_ARGUMENT;
    XrCompileState *state=xr_source_compiler_state(compiler);
    XrSourceStatus status=api_resource(xr_compile_state_status(state));
    if(status!=XR_SOURCE_OK)return status;
    XrSourceDiagnostic *detail=NULL;XrSourceAst *ast=NULL;
    if(diagnostic)status=api_resource(xr_compile_state_calloc(state,1,sizeof(*detail),(void **)&detail));
    if(status==XR_SOURCE_OK)status=api_resource(xr_compile_state_calloc(state,1,sizeof(*ast),(void **)&ast));
    if(status==XR_SOURCE_OK) {
        ast->references=1;
        status=api_resource(xr_compile_state_retain(state));
        if(status==XR_SOURCE_OK){ast->state=state;status=api_parse_owned(compiler,text,ast,detail,interactive);}
    }
    if(status==XR_SOURCE_BUDGET || status==XR_SOURCE_OUT_OF_MEMORY)
        (void)xr_compile_state_fail(state,status==XR_SOURCE_BUDGET ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    if(status==XR_SOURCE_OK){*output=ast;xray_source_diagnostic_free(detail);}
    else {
        xray_source_ast_free(ast);
        if(detail){detail->detail.status=api_compile_failure(status);detail->detail.stage=XR_SOURCE_COMPILE_INPUT;*diagnostic=detail;}
    }
    return status;
}
XRAY_API XrSourceStatus xray_source_parse_text(XrSourceCompiler *compiler,const XrSourceText *text,
    XrSourceAst **output,XrSourceDiagnostic **diagnostic) {
    return api_parse_text(compiler,text,output,diagnostic,false);
}
static XrXirStatus api_ast_retain(void *context,const XrXirSourceAst *input) {
    XrSourceAst *ast=context;
    if(!ast || !ast->state || !ast->program || !ast->references || !input ||
        input->program!=ast->program || input->logical_path!=ast->path)return XR_XIR_BAD_STRUCTURE;
    if(ast->references==UINT32_MAX)return XR_XIR_BUDGET;
    XrCompileResourceStatus status=xr_compile_state_work(ast->state,1);
    if(status!=XR_COMPILE_RESOURCE_OK)return status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ?
        XR_XIR_OUT_OF_MEMORY : status==XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
    ++ast->references;return XR_XIR_OK;
}
static void api_ast_release(void *context) {xray_source_ast_free(context);}
XR_FUNC XrSourceStatus xr_source_ast_owned_input(const XrSourceAst *ast,XrXirSourceAstOwner *output) {
    if(!ast || !ast->state || !ast->program || !ast->references || !output || output->input.program ||
        output->input.logical_path || output->resources || output->owner || output->retain || output->release)
        return XR_SOURCE_BAD_ARGUMENT;
    *output=(XrXirSourceAstOwner){.input={ast->program,ast->path},
        .resources=xr_compile_state_resources(ast->state),.owner=(void *)ast,
        .retain=api_ast_retain,.release=api_ast_release};
    return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_compile_ast(XrSourceCompiler *compiler,const XrSourceAst *ast,
    XrSourceProgram **output,XrSourceDiagnostic **diagnostic) {
    if(!ast)return XR_SOURCE_BAD_ARGUMENT;
    return api_build(compiler,NULL,NULL,ast,output,diagnostic);
}
