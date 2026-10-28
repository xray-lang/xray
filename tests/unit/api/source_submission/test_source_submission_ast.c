/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_submission_ast.c - Real AST ownership across published generations
 */
#include "api/xsource_submission.h"
#include "api/xsource_ast_owner.h"
#include "api/xsource_compiler.h"
#include "frontend/parser/xast.h"
#include "xir/xxir_format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
typedef struct AstSubmissionTrace {XrSourceSubmission *owner;unsigned outputs;} AstSubmissionTrace;
static XrXirOutputStatus ast_output(void *context,const XrXirOutputGroup *group) {
    AstSubmissionTrace *trace=context;
    CHECK(group->count==1 && group->values[0].type==XR_XIR_I64 && group->values[0].payload==17);
    ++trace->outputs;
    XrXirSourceAstOwner empty={0};
    CHECK(xr_source_submission_prepare_ast(trace->owner,&empty,NULL)==XR_SOURCE_BUSY);
    XrSourceSubmission *before=trace->owner;XrXirCallStatus cleanup=XR_XIR_CALL_RETURNED;
    CHECK(xr_source_submission_drop(&trace->owner,&cleanup)==XR_SOURCE_BUSY && trace->owner==before && cleanup==XR_XIR_CALL_RETURNED);
    return XR_XIR_OUTPUT_OK;
}
static XrSourceAst *parse(XrSourceCompiler *compiler,const char *source) {
    XrSourceText text={NULL,"owned-ast-input",source,strlen(source)};
    XrSourceAst *ast=NULL;XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_parse_text(compiler,&text,&ast,&diagnostic);
    if(diagnostic) {XrSourceDiagnosticRoot info={0};XrSourceDiagnosticReportInfo report_info={0};CHECK((xray_source_diagnostic_report(diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(diagnostic,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK);fprintf(stderr,"%s\n",info.message);}
    CHECK(status==XR_SOURCE_OK && ast && !diagnostic);return ast;
}
static XrXirCallStatus finish(XrSourceSubmission *owner) {
    XrXirInstanceResult result={0};unsigned steps=0;
    do {CHECK(xr_source_submission_poll(owner,8,&result)==XR_SOURCE_OK && ++steps<10000);}
    while(result.outcome.status==XR_XIR_CALL_READY);
    XrXirCallResult owned={0};CHECK(xr_source_submission_take(owner,&owned)==XR_SOURCE_OK);
    CHECK(owned.status==result.outcome.status);XrXirCallStatus status=owned.status;xr_xir_call_result_drop(&owned);return status;
}
static void prepare_ast(XrSourceSubmission *owner,XrSourceAst **ast) {
    static unsigned attempt;
    ++attempt;
    XrXirSourceAstOwner input={0};CHECK(xr_source_ast_owned_input(*ast,&input)==XR_SOURCE_OK);
    AstNode before=*input.input.program;XrXirSourceDiagnostic diagnostic={0};
    XrSourceStatus status=xr_source_submission_prepare_ast(owner,&input,&diagnostic);
    if(status!=XR_SOURCE_OK)fprintf(stderr,"AST prepare attempt%u status%u XIR%u: %s\n",attempt,(unsigned)status,(unsigned)diagnostic.status,diagnostic.message);
    CHECK(status==XR_SOURCE_OK && !memcmp(&before,input.input.program,sizeof(before)));
    xray_source_ast_free(*ast);*ast=NULL;memset(&input,0xCC,sizeof(input));
}
static void read_i64(XrSourceSubmission *owner,const char *name,int64_t expected) {
    XrXirValue value={0};CHECK(xr_source_submission_read(owner,name,&value)==XR_SOURCE_OK);
    CHECK(value.type==XR_XIR_I64 && (int64_t)value.payload==expected);xr_xir_value_drop(&value);
}
static void read_array(const XrXirValue *array,int64_t expected) {
    XrXirFormatCursor root={0},child={0};XrXirFormatAggregate aggregate={0};XrXirValue element={0};
    CHECK(xr_xir_value_format_cursor(array,&root) && xr_xir_value_format_aggregate(&root,&aggregate));
    CHECK(aggregate.array && aggregate.count==1 && xr_xir_value_format_child(&root,0,&child));
    CHECK(xr_xir_value_format_leaf(&child,&element) && element.type==XR_XIR_I64 && (int64_t)element.payload==expected);
}
static void source_only_owner(void) {
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_SUBMISSION_STDLIB;options.allocated_bytes=UINT64_C(64)<<20;
    options.live_bytes=UINT64_C(8)<<20;options.work=UINT64_C(128000000);XrSourceCompiler *compiler=NULL;
    CHECK(xray_source_compiler_new(&options,&compiler)==XR_SOURCE_OK);
    XrSourceAst *ast=parse(compiler,"const answer=42\n");
    XrXirSourceGenerationConfig config={*xr_source_compiler_context(compiler),"owned-ast-record",XR_SUBMISSION_STDLIB,NULL,NULL};
    XrXirSourceGenerations *owner=NULL;CHECK(xr_xir_compile_source_generations_new(&config,&owner)==XR_XIR_OK);
    XrXirSourceAstOwner input={0};CHECK(xr_source_ast_owned_input(ast,&input)==XR_SOURCE_OK);
    uint64_t serial=0;CHECK(xr_xir_compile_source_generation_prepare_ast_bindings(owner,&input,&serial,NULL)==XR_XIR_OK && serial==1);
    AstNode *actual=input.input.program;xray_source_ast_free(ast);xray_source_compiler_free(compiler);
    XrXirSourceGenerationView view={0};CHECK(xr_xir_compile_source_generation_view(owner,serial,&view)==XR_XIR_OK);
    XrFingerprint empty={0};CHECK(view.ast==actual && view.ast->type==AST_PROGRAM && !view.source && !view.length &&
        !memcmp(&view.fingerprint,&empty,sizeof(empty)));
    xr_xir_compile_source_generations_free(owner);
}
static uint64_t empty_ledger_live(void) {
    const XrCompileResourceLimits limits={4096,4096,4096};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(resources);return stats.live_bytes;
}
int main(void) {
    source_only_owner();
    uint64_t ledger_live=empty_ledger_live();
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_SUBMISSION_STDLIB;options.allocated_bytes=UINT64_C(64)<<20;
    options.live_bytes=UINT64_C(8)<<20;options.work=UINT64_C(128000000);
    XrSourceCompiler *compiler=NULL;CHECK(xray_source_compiler_new(&options,&compiler)==XR_SOURCE_OK);
    XrCompileResources *resources=xr_source_compiler_resources(compiler);
    CHECK(xr_compile_resources_retain(resources)==XR_COMPILE_RESOURCE_OK);
    XrSourceAst *first=parse(compiler,"var state=40\nconst unit=()\nfn next()->i64{state+=1;return state}\n"
        "fn identity<T>(value:T)->T{return value}\nprint(17)\n");
    XrSourceAst *later=parse(compiler,"var captured=identity([next()])\nconst snapshot=observed\nconst carriedUnit=unit\n");
    XrSourceAst *invalid=parse(compiler,"var abandoned=missingDefinition\n");
    XrSourceAst *failed=parse(compiler,"state+=1\nvar zero=0\nvar never=1/zero\n");
    XrSourceSubmissionConfig config={0};config.state=xr_source_compiler_state(compiler);
    config.source=(XrXirSourceGenerationConfig){*xr_source_compiler_context(compiler),"paired-owned-ast",XR_SUBMISSION_STDLIB,NULL,NULL};
    config.target=(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_instance_config_init(&config.runtime,sizeof(config.runtime))==XR_XIR_CALL_READY);
    AstSubmissionTrace trace={0};config.runtime.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,ast_output,&trace};
    CHECK(xr_source_submission_new(&config,&trace.owner)==XR_SOURCE_OK);
    XrSourceCompiler *foreign=NULL;CHECK(xray_source_compiler_new(&options,&foreign)==XR_SOURCE_OK);
    XrSourceAst *foreign_ast=parse(foreign,"const foreign=1\n");XrXirSourceAstOwner foreign_input={0};
    CHECK(xr_source_ast_owned_input(foreign_ast,&foreign_input)==XR_SOURCE_OK);
    XrCompileResourceStats before_foreign={0},after_foreign={0};
    CHECK(xr_compile_resources_stats(resources,&before_foreign)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_source_submission_prepare_ast(trace.owner,&foreign_input,NULL)==XR_SOURCE_REJECTED);
    CHECK(xr_compile_resources_stats(resources,&after_foreign)==XR_COMPILE_RESOURCE_OK &&
        !memcmp(&before_foreign,&after_foreign,sizeof(before_foreign)));
    xray_source_ast_free(foreign_ast);xray_source_compiler_free(foreign);
    XrXirSourceAstOwner original={0},other={0};
    CHECK(xr_source_ast_owned_input(first,&original)==XR_SOURCE_OK && xr_source_ast_owned_input(later,&other)==XR_SOURCE_OK);
    XrXirSourceAstOwner occupied=original;
    CHECK(xr_source_ast_owned_input(first,&occupied)==XR_SOURCE_BAD_ARGUMENT && !memcmp(&occupied,&original,sizeof(original)));
    XrXirSourceAstOwner mismatched=original;mismatched.input.program=other.input.program;
    CHECK(xr_source_submission_prepare_ast(trace.owner,&mismatched,NULL)==XR_SOURCE_REJECTED);
    prepare_ast(trace.owner,&first);xray_source_compiler_free(compiler);compiler=NULL;
    CHECK(finish(trace.owner)==XR_XIR_CALL_RETURNED && trace.outputs==1);
    CHECK(xr_source_submission_publish(trace.owner)==XR_SOURCE_OK);read_i64(trace.owner,"state",40);
    XrXirSourceText text={NULL,"const observed=next()\n",0};
    text.length=strlen(text.text);XrXirSourceDiagnostic diagnostic={0};
    XrSourceStatus prepared=xr_source_submission_prepare(trace.owner,&text,&diagnostic);
    if(prepared!=XR_SOURCE_OK)fprintf(stderr,"text after AST %u: %s\n",(unsigned)prepared,diagnostic.message);
    CHECK(prepared==XR_SOURCE_OK);
    CHECK(finish(trace.owner)==XR_XIR_CALL_RETURNED && xr_source_submission_publish(trace.owner)==XR_SOURCE_OK);
    read_i64(trace.owner,"observed",41);
    prepare_ast(trace.owner,&later);CHECK(finish(trace.owner)==XR_XIR_CALL_RETURNED);
    CHECK(xr_source_submission_publish(trace.owner)==XR_SOURCE_OK);read_i64(trace.owner,"state",42);
    read_i64(trace.owner,"snapshot",41);XrXirValue unit={0};
    CHECK(xr_source_submission_read(trace.owner,"carriedUnit",&unit)==XR_SOURCE_OK && unit.type==XR_XIR_UNIT && !unit.payload);
    XrXirValue array={0};CHECK(xr_source_submission_read(trace.owner,"captured",&array)==XR_SOURCE_OK);
    read_array(&array,42);
    XrXirSourceAstOwner bad={0};CHECK(xr_source_ast_owned_input(invalid,&bad)==XR_SOURCE_OK);
    AstNode before=*bad.input.program;
    CHECK(xr_source_submission_prepare_ast(trace.owner,&bad,NULL)==XR_SOURCE_REJECTED);
    CHECK(!memcmp(&before,bad.input.program,sizeof(before)));read_i64(trace.owner,"state",42);
    CHECK(xr_source_submission_prepare_ast(trace.owner,&bad,NULL)==XR_SOURCE_REJECTED);
    xray_source_ast_free(invalid);invalid=NULL;
    prepare_ast(trace.owner,&failed);CHECK(xr_xir_call_panic_status(finish(trace.owner)));
    CHECK(xr_source_submission_publish(trace.owner)==XR_SOURCE_BAD_STATE);
    XrXirCallStatus cleanup=XR_XIR_CALL_RETURNED;CHECK(xr_source_submission_abort(trace.owner,&cleanup)==XR_SOURCE_OK && cleanup==XR_XIR_CALL_READY);
    read_i64(trace.owner,"state",43);CHECK(trace.outputs==1);
    XrXirSourceGenerationBinding absent={0};CHECK(xr_source_submission_lookup(trace.owner,"never",&absent)!=XR_SOURCE_OK && !absent.declaration);
    CHECK(xr_source_submission_drop(&trace.owner,&cleanup)==XR_SOURCE_OK && !trace.owner && cleanup==XR_XIR_CALL_READY);
    read_array(&array,42);xr_xir_value_drop(&array);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.live_bytes==ledger_live);
    xr_compile_resources_release(resources);
    puts("owned AST records: producer death, no reparse, mixed text/AST history, actual slots/generics, foreign/mispaired refusal, failure preservation and all payload bytes released PASS");
    return 0;
}
