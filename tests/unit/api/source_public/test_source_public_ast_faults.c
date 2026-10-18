/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_public_ast_faults.c - Actual AST mutation, physical faults and shared limits
 */
#include "xray_source_ast.h"
#define main source_public_faults_previous_main
#include "test_source_public_faults.c"
#undef main
typedef struct AstFaultOwners {
    XrSourceCompiler *compiler;
    XrSourceAst *ast;
    XrSourceProgram *program;
    XrSourceDiagnostic *diagnostic;
    XrSourceResult *result;
    XrCompileResources *resources;
    char output[8];size_t output_size;
} AstFaultOwners;
static const XrCompileResourceLimits ast_limits={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
static XrSourceWriteStatus ast_fault_output(void *context,XrSourceOutputStream stream,const char *data,size_t size) {
    AstFaultOwners *owner=context;CHECK(stream==XR_SOURCE_STDOUT && size<=sizeof(owner->output)-owner->output_size);
    memcpy(owner->output+owner->output_size,data,size);owner->output_size+=size;return XR_SOURCE_WRITE_OK;
}
static void ast_fault_close(AstFaultOwners *owner) {
    size_t attempts=runtime_attempts;
    xray_source_compiler_free(owner->compiler);xray_source_ast_free(owner->ast);xray_source_program_free(owner->program);
    xray_source_diagnostic_free(owner->diagnostic);xray_source_result_free(owner->result);
    xr_compile_resources_release(owner->resources);*owner=(AstFaultOwners){0};
    CHECK(runtime_attempts==attempts && !runtime_live && !runtime_bytes);
}
static void ast_fault_sticky(AstFaultOwners *owner,XrSourceStatus failure) {
    if(!owner->compiler || (failure!=XR_SOURCE_BUDGET && failure!=XR_SOURCE_OUT_OF_MEMORY))return;
    const XrSourceText retry={NULL,"sticky-ast-retry","print(42)\n",10};
    XrSourceAst *ast=NULL;XrSourceProgram *program=NULL;XrSourceDiagnostic *diagnostic=NULL;
    XrCompileResourceStats before={0},after={0};size_t attempts=runtime_attempts;
    CHECK(xr_compile_resources_stats(owner->resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xray_source_parse_text(owner->compiler,&retry,&ast,&diagnostic)==failure && !ast && !diagnostic);
    CHECK(xray_source_compile_text(owner->compiler,&retry,&program,&diagnostic)==failure && !program && !diagnostic);
    if(owner->ast)CHECK(xray_source_compile_ast(owner->compiler,owner->ast,&program,&diagnostic)==failure && !program && !diagnostic);
    CHECK(xr_compile_resources_stats(owner->resources,&after)==XR_COMPILE_RESOURCE_OK && runtime_attempts==attempts &&
        before.work==after.work && before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes);
}
static XrSourceStatus ast_fault_drive(unsigned family,const XrCompileResourceLimits *limits,AstFaultOwners *owner) {
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;options.allocated_bytes=limits->allocated_bytes;
    options.live_bytes=limits->live_bytes;options.work=limits->work;
    XrSourceStatus status=xray_source_compiler_new(&options,&owner->compiler);if(status!=XR_SOURCE_OK)return status;
    owner->resources=xr_source_compiler_resources(owner->compiler);
    CHECK(xr_compile_resources_retain(owner->resources)==XR_COMPILE_RESOURCE_OK);
    const char embedded[]="print(1)\0print(2)\n";
    const char *source=family==0 ? "print(41)\n" : family==1 ?
        "import \"./sibling\" as sibling\nprint(sibling.answer()+1)\n" : family==2 ?
        "const value=1\nvar =\n" : family==3 ? "const value=missing\n" : embedded;
    XrSourceText text={family==1 || family==2 ? XR_PUBLIC_FIXTURE "/unsaved-public-fault.xr" : NULL,
        family==1 || family==2 ? NULL : "public-ast-faults",source,family==4 ? sizeof(embedded)-1 : strlen(source)};
    status=xray_source_parse_text(owner->compiler,&text,&owner->ast,&owner->diagnostic);if(status!=XR_SOURCE_OK)return status;
    if(!family) {
        AstNode *statement=owner->ast->program->as.program.statements[0];CHECK(statement->type==AST_EXPR_STMT);
        AstNode *call=statement->as.expr_stmt;CHECK(call->type==AST_CALL_EXPR);
        AstNode *value=call->as.call_expr.arguments[0];CHECK(value->type==AST_LITERAL_INT);
        value->as.literal.int_bits=42;value->as.literal.raw_value.int_val=42;
        CHECK(strstr(owner->ast->source,"41"));
    }
    status=xray_source_compile_ast(owner->compiler,owner->ast,&owner->program,&owner->diagnostic);
    if(status!=XR_SOURCE_OK)return status;
    xray_source_compiler_free(owner->compiler);owner->compiler=NULL;
    xray_source_ast_free(owner->ast);owner->ast=NULL;
    XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    runtime.write=ast_fault_output;runtime.context=owner;
    status=xray_source_execute(owner->program,&runtime,&owner->result,NULL);if(status!=XR_SOURCE_OK)return status;
    XrSourceEvent event={0};CHECK(xray_source_result_event(owner->result,&event)==XR_SOURCE_OK);
    if(event.state==XR_SOURCE_RUN_OUT_OF_MEMORY)return XR_SOURCE_OUT_OF_MEMORY;
    CHECK(event.state==XR_SOURCE_RUN_RETURNED && owner->output_size==3 &&
        !memcmp(owner->output,family ? "43\n" : "42\n",3));
    return XR_SOURCE_OK;
}
static void ast_faults(bool resources) {
    for(unsigned family=0;family<5;++family) {
        XrSourceStatus expected=family<2 ? XR_SOURCE_OK : family==3 ? XR_SOURCE_REJECTED : XR_SOURCE_INVALID;
        size_t sites=0,observed=0;XrCompileResourceStats required={0};
        for(size_t point=0;point<=sites;++point) {
            runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;AstFaultOwners owner={0};
            XrSourceStatus status=ast_fault_drive(family,&ast_limits,&owner);
            if(!point) {
                CHECK(status==expected);observed=runtime_attempts;CHECK(observed);
                sites=resources ? observed : 0;
                CHECK(xr_compile_resources_stats(owner.resources,&required)==XR_COMPILE_RESOURCE_OK);
            } else CHECK(status==XR_SOURCE_OUT_OF_MEMORY && runtime_attempts>runtime_fail_at);
            ast_fault_sticky(&owner,status);ast_fault_close(&owner);
        }
        runtime_fail_at=SIZE_MAX;
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
            XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
            uint64_t *bound=axis==0 ? &limits.allocated_bytes : axis==1 ? &limits.live_bytes : &limits.work;
            CHECK(*bound && *bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
            AstFaultOwners owner={0};XrSourceStatus status=ast_fault_drive(family,&limits,&owner);
            CHECK(status==(delta<0 ? XR_SOURCE_BUDGET : expected));
            ast_fault_sticky(&owner,status);ast_fault_close(&owner);
        }
        printf("Public actual AST family%u observed=%zu swept=%zu; three axes exact/minus1/plus1; physical0\n",family,observed,sites);
    }
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--resources")));
    ast_faults(argc==2);return 0;
}
