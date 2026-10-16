/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_public_ast.c - Public syntax ownership and ordinary Program admission
 */
#include "xray_source_ast.h"
#define main source_public_previous_main
#include "test_source_public.c"
#undef main
static void ast_execute(XrSourceProgram *program,const char *expected) {
    Bytes bytes={0};XrSourceCallOptions options=options_for(&bytes);XrSourceResult *result=NULL;
    CHECK(xray_source_execute(program,&options,&result,NULL)==XR_SOURCE_OK);
    xray_source_program_free(program);
    int64_t value=-1;CHECK(xray_source_result_i64(result,&value)==XR_SOURCE_OK && !value);
    CHECK(bytes.size==strlen(expected) && !memcmp(bytes.data,expected,bytes.size));xray_source_result_free(result);
}
static void ast_owned_programs(void) {
    XrSourceCompiler *compiler=compiler_new();char source[]="print(42)\n",name[]="public-ast-memory";
    XrSourceText text={NULL,name,source,sizeof(source)-1};XrSourceAst *ast=NULL;XrSourceDiagnostic *diagnostic=NULL;
    CHECK(xray_source_parse_text(compiler,&text,&ast,&diagnostic)==XR_SOURCE_OK && ast && !diagnostic);
    memset(source,'!',sizeof(source)-1);memset(name,'?',sizeof(name)-1);
    XrSourceAst *occupied=ast;
    CHECK(xray_source_parse_text(compiler,&text,&occupied,NULL)==XR_SOURCE_BAD_ARGUMENT && occupied==ast);
    XrSourceCompiler *foreign=compiler_new();XrSourceProgram *empty=NULL;
    CHECK(xray_source_compile_ast(foreign,ast,&empty,NULL)==XR_SOURCE_BAD_ARGUMENT && !empty);
    xray_source_compiler_free(foreign);
    XrSourceProgram *first=NULL,*second=NULL;
    CHECK(xray_source_compile_ast(compiler,ast,&first,&diagnostic)==XR_SOURCE_OK && first && !diagnostic);
    XrSourceProgram *same=first;
    CHECK(xray_source_compile_ast(compiler,ast,&same,NULL)==XR_SOURCE_BAD_ARGUMENT && same==first);
    CHECK(xray_source_compile_ast(compiler,ast,&second,NULL)==XR_SOURCE_OK && second);
    xray_source_compiler_free(compiler);xray_source_ast_free(ast);
    ast_execute(first,"42\n");ast_execute(second,"42\n");
}
static void ast_rooted_import(void) {
    XrSourceCompiler *compiler=compiler_new();
    char path[]=XR_PUBLIC_FIXTURE "/unsaved-public-ast.xr";
    char source[]="import \"./sibling\" as sibling\nprint(sibling.answer()+1)\n";
    XrSourceText text={path,NULL,source,sizeof(source)-1};XrSourceAst *ast=NULL;
    CHECK(xray_source_parse_text(compiler,&text,&ast,NULL)==XR_SOURCE_OK);
    memset(path,'!',sizeof(path)-1);memset(source,'!',sizeof(source)-1);
    XrSourceProgram *program=NULL;
    CHECK(xray_source_compile_ast(compiler,ast,&program,NULL)==XR_SOURCE_OK);
    xray_source_ast_free(ast);xray_source_compiler_free(compiler);ast_execute(program,"43\n");
}
static void ast_diagnostics(void) {
    XrSourceCompiler *compiler=compiler_new();XrSourceAst *ast=NULL;XrSourceDiagnostic *diagnostic=NULL;
    const XrSourceText syntax={XR_PUBLIC_FIXTURE "/unsaved-syntax.xr",NULL,"const ok=1\nvar =\n",17};
    CHECK(xray_source_parse_text(compiler,&syntax,&ast,&diagnostic)==XR_SOURCE_INVALID && !ast && diagnostic);
    XrSourceDiagnosticInfo info={0};CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK &&
        info.status==XR_SOURCE_INVALID && info.line==2 && info.path && strstr(info.path,"unsaved-syntax.xr") && *info.message);
    const XrSourceText valid={NULL,"syntax-retry","print(42)\n",10};XrSourceDiagnostic *held=diagnostic;
    CHECK(xray_source_parse_text(compiler,&valid,&ast,&held)==XR_SOURCE_BAD_ARGUMENT && !ast && held==diagnostic);
    CHECK(xray_source_parse_text(compiler,&valid,&ast,NULL)==XR_SOURCE_OK && ast);
    xray_source_compiler_free(compiler);
    CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK && info.line==2 && *info.message);
    xray_source_diagnostic_free(diagnostic);xray_source_ast_free(ast);xray_source_ast_free(NULL);
    compiler=compiler_new();ast=NULL;diagnostic=NULL;
    const char embedded[]="print(1)\0print(2)\n";XrSourceText input={NULL,"nul-ast",embedded,sizeof(embedded)-1};
    CHECK(xray_source_parse_text(compiler,&input,&ast,&diagnostic)==XR_SOURCE_INVALID && !ast && diagnostic);
    xray_source_diagnostic_free(diagnostic);diagnostic=NULL;
    input=(XrSourceText){NULL,"semantic-ast","const value=missing\n",sizeof("const value=missing\n")-1};
    CHECK(xray_source_parse_text(compiler,&input,&ast,NULL)==XR_SOURCE_OK);
    XrSourceProgram *program=NULL;
    CHECK(xray_source_compile_ast(compiler,ast,&program,&diagnostic)==XR_SOURCE_REJECTED && !program && diagnostic);
    xray_source_ast_free(ast);xray_source_compiler_free(compiler);
    CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK && info.line==1 && *info.message);
    xray_source_diagnostic_free(diagnostic);
}
int main(void) {
    ast_owned_programs();ast_rooted_import();ast_diagnostics();
    puts("Public owning AST: source/locator copies, exact compiler refusal, repeat borrow, import43, detached Program42 and owned syntax/semantic diagnostics passed");
    return 0;
}
