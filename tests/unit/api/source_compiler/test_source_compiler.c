/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_compiler.c - Real compiler owner, shared state and complete failures
 */
#include "api/xsource_compiler.h"
#include "frontend/parser/xparse.h"
#include "frontend/parser/xast.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_output.h"
#include "execution/xr_xir_host_execution.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
#include "api/xsource_compiler.c"

static XrSourceCompilerConfig configuration(XrCompileResources *resources) {
    return (XrSourceCompilerConfig){{resources,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_COMPILER_STDLIB,{1048576,64}};
}
static XrSourceCompileStatus build_actual_ast(XrSourceCompiler *compiler, bool rooted,
    XrXirSourceProduct **product, XrSourceCompileDiagnostic *diagnostic) {
    const char *source = rooted ? "import \"./sibling\" as sibling\nexport fn publicAstAnswer()->i64 { return sibling.answer()+1 }\n" :
        "export fn publicAstAnswer()->i64 { return 41 }\n";
    AstNode *ast = NULL;
    XrParseStatus parsed = xr_compile_parse(xr_source_compiler_session(compiler),source,&ast);
    if (parsed != XR_PARSE_OK) return parsed == XR_PARSE_BUDGET ? XR_SOURCE_COMPILE_BUDGET :
        parsed == XR_PARSE_OUT_OF_MEMORY ? XR_SOURCE_COMPILE_OUT_OF_MEMORY : XR_SOURCE_COMPILE_BAD_ARGUMENT;
    if (!rooted) {
        AstNode *function=ast->as.program.statements[0]; CHECK(function->type==AST_FUNCTION_DECL);
        AstNode *value=function->as.function_decl.body->as.block.statements[0]->as.return_stmt.values[0];
        CHECK(value->type==AST_LITERAL_INT); value->as.literal.int_bits=42; value->as.literal.raw_value.int_val=42;
    }
    XrSourceCompilerText input={rooted ? XR_COMPILER_FIXTURE "/unsaved-ast.xr" : NULL,
        rooted ? NULL : "owned-actual-ast",NULL,0};
    XrSourceCompileStatus status=xr_source_compiler_build_ast(compiler,&input,ast,product,diagnostic);
    CHECK(ast->as.program.owns_arena); xr_program_destroy(ast); return status;
}
static void owned_ast_compiler(void) {
    XrCompileResources *resources=NULL; XrCompileResourceLimits limits={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig config=configuration(resources); XrSourceCompiler *compiler=NULL;
    CHECK(xr_source_compiler_new(&config,NULL,&compiler,NULL)==XR_SOURCE_COMPILE_OK);
    XrXirSourceProduct *products[2]={0};
    for(unsigned i=0;i<2;++i) CHECK(build_actual_ast(compiler,i!=0,&products[i],NULL)==XR_SOURCE_COMPILE_OK);
    CHECK(xr_source_compiler_build_ast(compiler,NULL,NULL,&products[0],NULL)==XR_SOURCE_COMPILE_BAD_ARGUMENT);
    xr_source_compiler_free(compiler); xr_compile_resources_release(resources);
    for(unsigned i=0;i<2;++i) {
        CHECK(xr_xir_compile_source_product_verify(products[i],1048576,NULL)==XR_XIR_OK);
        XrXirSourceProductPacketView packet={0}; XrXirArtifact *checked=NULL;
        CHECK(xr_xir_compile_source_product_packet(products[i],XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
        CHECK(xr_xir_compile_checked_read(xr_xir_compile_source_product_context(products[i]),packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
        const XrXirModule *module=xr_xir_compile_artifact_module(checked); uint32_t answer=UINT32_MAX;
        for(uint32_t f=0;f<module->function_count;++f)
            if(module->functions[f].name_length==15 && !memcmp(module->functions[f].name,"publicAstAnswer",15)) answer=f;
        CHECK(answer!=UINT32_MAX); xr_xir_compile_artifact_free(checked);
        uint32_t entry=xr_xir_compile_source_product_facts(products[i])->entry;
        XrXirProgram *program=NULL; CHECK(xr_xir_compile_source_product_vm_take(products[i],&program)==XR_XIR_OK);
        xr_xir_compile_source_product_free(products[i]);
        XrXirInstanceConfig instance_config; CHECK(xr_xir_instance_config_init(&instance_config,sizeof(instance_config))==XR_XIR_CALL_READY);
        XrXirInstance *instance=NULL; CHECK(xr_xir_instance_new(program,&instance_config,&instance)==XR_XIR_CALL_READY);
        xr_xir_compile_program_drop(program);
        for(unsigned call=0;call<2;++call) {
            CHECK(xr_xir_instance_start(instance,call ? answer : entry,NULL,0)==XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
            XrXirValue value={0}; CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
            CHECK(value.type==XR_XIR_I64 && value.payload==(call ? (i ? 43 : 42) : 0)); xr_xir_value_drop(&value);
        }
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    CHECK(!runtime_live && !runtime_bytes);
    puts("SourceCompiler actual AST42/import43 after compiler and AST death; normal public admission and Program verification PASS");
}
static XrSourceCompileStatus compiler_ast_trial(XrSourceCompiler *compiler,
    const XrSourceCompilerText *input, XrXirSourceProduct **output,
    XrSourceCompileDiagnostic *diagnostic) {
    AstNode *ast = NULL;
    XrParseStatus parsed = xr_compile_parse_with_source(xr_source_compiler_session(compiler),
        input->text, input->absolute_path, &ast);
    if (parsed != XR_PARSE_OK) {
        CHECK(parsed == XR_PARSE_BUDGET || parsed == XR_PARSE_OUT_OF_MEMORY);
        return parsed == XR_PARSE_BUDGET ? XR_SOURCE_COMPILE_BUDGET : XR_SOURCE_COMPILE_OUT_OF_MEMORY;
    }
    XrSourceCompileStatus status = xr_source_compiler_build_ast(compiler, input, ast, output, diagnostic);
    xr_program_destroy(ast);
    return status;
}
typedef struct CompilerOutput { char data[16]; size_t length; } CompilerOutput;
static XrXirOutputStatus compiler_write(void *context, XrXirOutputStream stream,
    const char *bytes, size_t length) {
    CompilerOutput *out = context;
    if (stream != XR_XIR_STDOUT || length > sizeof(out->data) - out->length) return XR_XIR_OUTPUT_ERROR;
    memcpy(out->data + out->length, bytes, length); out->length += length;
    return XR_XIR_OUTPUT_OK;
}
static void caller_ast_compiler(void) {
    XrCompileResources *resources = NULL;
    XrCompileResourceLimits limits = {UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig config = configuration(resources); XrSourceCompiler *compiler = NULL;
    CHECK(xr_source_compiler_new(&config,NULL,&compiler,NULL)==XR_SOURCE_COMPILE_OK);
    XrCompilerSession *session = xr_source_compiler_session(compiler);
    char source[] = "const n:i64 = 41\nprint(n)\n";
    XrSourceCompilerText text = {NULL,"compiler-caller-ast",source,sizeof(source)-1};
    AstNode *ast = NULL; CHECK(xr_compile_parse(session,source,&ast)==XR_PARSE_OK);
    CHECK(ast->type==AST_PROGRAM && ast->as.program.count==2);
    AstNode *literal = ast->as.program.statements[0]->as.var_decl.initializer;
    CHECK(literal && literal->type==AST_LITERAL_INT); literal->as.literal.int_bits=42;
    XrXirSourceProduct *product=NULL,*rooted=NULL;
    CHECK(xr_source_compiler_build_ast(compiler,&text,NULL,&product,NULL)==XR_SOURCE_COMPILE_BAD_ARGUMENT && !product);
    XrCompilerSession *foreign=NULL;
    CHECK(xr_compile_session_new(resources,&foreign)==XR_COMPILER_SESSION_OK);
    AstNode *foreign_ast=NULL; CHECK(xr_compile_parse(foreign,source,&foreign_ast)==XR_PARSE_OK);
    CHECK(xr_source_compiler_build_ast(compiler,&text,foreign_ast,&product,NULL)==XR_SOURCE_COMPILE_BAD_ARGUMENT && !product);
    xr_program_destroy(foreign_ast);xr_compile_session_free(foreign);
    CHECK(xr_source_compiler_build_ast(compiler,&text,ast,&product,NULL)==XR_SOURCE_COMPILE_OK);
    XrXirSourceProduct *occupied=product;
    CHECK(xr_source_compiler_build_ast(compiler,&text,ast,&product,NULL)==XR_SOURCE_COMPILE_BAD_ARGUMENT && product==occupied);
    CHECK(literal->as.literal.int_bits==42 && xr_source_compiler_session(compiler)==session);
    const char import[]="import \"./sibling\" as sibling\nprint(sibling.answer())\n";
    XrSourceCompilerText rooted_text={XR_COMPILER_FIXTURE "/unsaved-ast-compiler.xr",NULL,import,sizeof(import)-1};
    CHECK(compiler_ast_trial(compiler,&rooted_text,&rooted,NULL)==XR_SOURCE_COMPILE_OK);
    CHECK(xr_xir_compile_source_product_facts(rooted)->module_count==2);
    uint32_t entry=xr_xir_compile_source_product_facts(product)->entry;
    xr_program_destroy(ast);memset(source,'!',sizeof(source));
    xr_compile_resources_release(resources);xr_source_compiler_free(compiler);
    CHECK(xr_xir_compile_source_product_verify(rooted,1048576,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_source_product_verify(product,1048576,NULL)==XR_XIR_OK);
    xr_xir_compile_source_product_free(rooted);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK);
    xr_xir_compile_source_product_free(product);
    CompilerOutput output={{0},0};
    XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,compiler_write,&output,sizeof(output.data)};
    XrXirInstanceConfig run_config;CHECK(xr_xir_instance_config_init(&run_config,sizeof(run_config))==XR_XIR_CALL_READY);
    run_config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirHostExecutionRequest run={program,&run_config,entry,NULL,0};XrXirCallResult result={0};
    CHECK(xr_xir_host_execute(&run,&result)==XR_XIR_CALL_RETURNED);
    CHECK(output.length==3 && !memcmp(output.data,"42\n",3));
    xr_xir_compile_program_drop(program);xr_xir_call_result_drop(&result);
    CHECK(!runtime_live && !runtime_bytes);
    puts("compiler actual edited AST42, exact parser owner, rooted imports and execution after producer death PASS");
}
static void owned_compiler(void) {
    XrCompileResources *resources = NULL;
    XrCompileResourceLimits limits = {UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig config = configuration(resources); XrSourceCompiler *compiler = NULL;
    CHECK(xr_source_compiler_new(&config,NULL,&compiler,NULL)==XR_SOURCE_COMPILE_OK);
    CHECK(xr_source_compiler_resources(compiler)==resources);
    CHECK(xr_source_compiler_context(compiler)->resources==resources);
    XrCompilerSession *session = xr_source_compiler_session(compiler);
    CHECK(session && xr_compile_session_resources(session)==resources);
    CHECK(xr_source_compiler_state(compiler)==xr_compile_session_compile_state(session));
    memset(&config,0,sizeof(config)); xr_compile_resources_release(resources);
    char source[] = "print(42)\n";
    XrSourceCompilerText text = {NULL,"owned-compiler-text",source,sizeof(source)-1};
    XrXirSourceProduct *first = NULL, *second = NULL, *file = NULL;
    CHECK(xr_source_compiler_build_text(compiler,&text,&first,NULL)==XR_SOURCE_COMPILE_OK);
    XrCompileResourceStats before, after;
    CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_source_compiler_build_text(compiler,&text,&second,NULL)==XR_SOURCE_COMPILE_OK);
    CHECK(xr_source_compiler_session(compiler)==session);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.allocated_bytes>before.allocated_bytes && after.work>before.work);
    CHECK(xr_source_compiler_build_file(compiler,XR_COMPILER_FIXTURE "/sibling.xr",&file,NULL)==XR_SOURCE_COMPILE_OK);
    XrXirSourceProduct *rooted = NULL;
    const char import[] = "import \"./sibling\" as sibling\nprint(sibling.answer())\n";
    text = (XrSourceCompilerText){XR_COMPILER_FIXTURE "/unsaved-compiler.xr",NULL,import,sizeof(import)-1};
    CHECK(xr_source_compiler_build_text(compiler,&text,&rooted,NULL)==XR_SOURCE_COMPILE_OK);
    CHECK(xr_xir_compile_source_product_facts(rooted)->module_count==2);
    const char invalid[] = "var value = missing_symbol\n";
    text.text=invalid; text.length=sizeof(invalid)-1;
    XrXirSourceProduct *rejected=NULL; XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_build_text(compiler,&text,&rejected,&diagnostic)==XR_SOURCE_COMPILE_REJECTED);
    CHECK(!rejected && diagnostic.source.source.line==1 && diagnostic.source.source.column==13);
    CHECK(diagnostic.source.failure.path && strstr(diagnostic.source.failure.path,"unsaved-compiler.xr"));
    memset(source,'!',sizeof(source)); xr_source_compiler_free(compiler);
    CHECK(strstr(diagnostic.source.source.message,"initialized value"));
    CHECK(strstr(diagnostic.source.failure.path,"unsaved-compiler.xr"));
    CHECK(xr_xir_compile_source_product_verify(first,1048576,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_source_product_verify(second,1048576,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_source_product_verify(rooted,1048576,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_source_product_verify(file,1048576,NULL)==XR_XIR_OK);
    xr_xir_compile_source_product_free(second); xr_xir_compile_source_product_free(rooted);
    xr_xir_compile_source_product_free(file); xr_xir_compile_source_product_free(first);
    xr_source_compile_diagnostic_free(&diagnostic); CHECK(!runtime_live && !runtime_bytes);
    puts("one ledger/state/Session; repeated memory/file/rooted builds; producer death and located diagnostics PASS");
}
static XrSourceCompileStatus limited_build(XrCompileResourceLimits limits,
    XrCompileResourceStats *stats, XrSourceCompiler **owner, XrXirSourceProduct **product,
    XrCompileResources **ledger, unsigned mode) {
    CHECK(xr_compile_resources_new(&limits,ledger)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig config=configuration(*ledger);
    XrSourceCompileStatus status=xr_source_compiler_new(&config,NULL,owner,NULL);
    if(status==XR_SOURCE_COMPILE_OK) {
        const char source[]="const n:i64 = 42\n";
        XrSourceCompilerText text={NULL,"compiler-owner-limits",source,sizeof(source)-1};
        status=mode ? build_actual_ast(*owner,mode==4,product,NULL) :
            xr_source_compiler_build_text(*owner,&text,product,NULL);
    }
    CHECK(xr_compile_resources_stats(*ledger,stats)==XR_COMPILE_RESOURCE_OK);
    return status;
}
static void compiler_limits(void) {
    XrCompileResourceLimits ample={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
    XrCompileResourceStats required;
    XrSourceCompiler *compiler=NULL; XrXirSourceProduct *product=NULL;XrCompileResources *resources=NULL;
    CHECK(limited_build(ample,&required,&compiler,&product,&resources,0)==XR_SOURCE_COMPILE_OK);
    xr_source_compiler_free(compiler);xr_xir_compile_source_product_free(product);xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes);
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(*bound>1 && *bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        compiler=NULL;product=NULL;resources=NULL;XrCompileResourceStats stats;
        CHECK(limited_build(limits,&stats,&compiler,&product,&resources,0)==
            (delta<0?XR_SOURCE_COMPILE_BUDGET:XR_SOURCE_COMPILE_OK));
        if(delta<0)CHECK(!product);
        xr_xir_compile_source_product_free(product);xr_source_compiler_free(compiler);xr_compile_resources_release(resources);
        CHECK(!runtime_live && !runtime_bytes);
    }
    compiler=NULL;product=NULL;resources=NULL;XrCompileResourceStats before,after;
    XrCompileResourceLimits exact={required.allocated_bytes,required.peak_bytes,required.work};
    CHECK(limited_build(exact,&before,&compiler,&product,&resources,0)==XR_SOURCE_COMPILE_OK);
    xr_xir_compile_source_product_free(product);product=NULL;
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes<before.live_bytes && after.allocated_bytes==before.allocated_bytes && after.work==before.work);
    const char source[]="const n:i64 = 42\n";
    XrSourceCompilerText text={NULL,"compiler-owner-limits",source,sizeof(source)-1};
    CHECK(xr_source_compiler_build_text(compiler,&text,&product,NULL)==XR_SOURCE_COMPILE_BUDGET && !product);
    CHECK(xr_compile_state_status(xr_source_compiler_state(compiler))==XR_COMPILE_RESOURCE_BUDGET);
    size_t attempts=runtime_attempts;
    CHECK(xr_source_compiler_build_text(compiler,&text,&product,NULL)==XR_SOURCE_COMPILE_BUDGET && !product);
    CHECK(xr_source_compiler_build_file(compiler,XR_COMPILER_FIXTURE "/sibling.xr",&product,NULL)==XR_SOURCE_COMPILE_BUDGET && !product);
    CHECK(runtime_attempts==attempts);
    xr_source_compiler_free(compiler);xr_compile_resources_release(resources);CHECK(!runtime_live && !runtime_bytes);
    puts("compiler exact/minus1/plus1; cumulative teardown; first budget failure sticks across memory and file retry PASS");
}
static void compiler_ast_limits(void) {
    for(unsigned mode=3;mode<5;++mode) {
        XrCompileResourceLimits ample={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
        XrSourceCompiler *compiler=NULL; XrXirSourceProduct *product=NULL; XrCompileResources *resources=NULL;
        XrCompileResourceStats required={0};
        CHECK(limited_build(ample,&required,&compiler,&product,&resources,mode)==XR_SOURCE_COMPILE_OK);
        xr_xir_compile_source_product_free(product); xr_source_compiler_free(compiler); xr_compile_resources_release(resources);
        CHECK(!runtime_live && !runtime_bytes);
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
            XrCompileResourceLimits caps={required.allocated_bytes,required.peak_bytes,required.work};
            uint64_t *bound=axis==0 ? &caps.allocated_bytes : axis==1 ? &caps.live_bytes : &caps.work;
            CHECK(*bound>1 && *bound<UINT64_MAX); *bound=(uint64_t)((int64_t)*bound+delta);
            compiler=NULL; product=NULL; resources=NULL; XrCompileResourceStats observed={0};
            CHECK(limited_build(caps,&observed,&compiler,&product,&resources,mode)==
                (delta<0 ? XR_SOURCE_COMPILE_BUDGET : XR_SOURCE_COMPILE_OK));
            if(delta<0)CHECK(!product);
            xr_xir_compile_source_product_free(product); xr_source_compiler_free(compiler); xr_compile_resources_release(resources);
            CHECK(!runtime_live && !runtime_bytes);
        }
        printf("Compiler actual AST mode=%u three axes exact/minus1/plus1 physical=0/0\n",mode);
    }
}
static void compiler_faults(unsigned first,unsigned end) {
    for(unsigned mode=first;mode<end;++mode) {
    size_t count=0;
    for(size_t point=0;!point || point<=count;++point) {
        runtime_attempts=0;runtime_fail_at=point?point-1:SIZE_MAX;
        XrCompileResources *resources=NULL;XrSourceCompiler *compiler=NULL;XrXirSourceProduct *product=NULL;
        XrSourceCompileDiagnostic diagnostic={0};
        XrCompileResourceLimits limits={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
        XrCompileResourceStatus created=xr_compile_resources_new(&limits,&resources);
        XrSourceCompileStatus status=XR_SOURCE_COMPILE_OUT_OF_MEMORY;
        if(created==XR_COMPILE_RESOURCE_OK) {
            XrSourceCompilerConfig config=configuration(resources);
            status=xr_source_compiler_new(&config,NULL,&compiler,&diagnostic);
            if(status==XR_SOURCE_COMPILE_OK) {
                const char source[]="const n:i64 = 42\n";
                XrSourceCompilerText text={NULL,"compiler-owner-faults",source,sizeof(source)-1};
                if(mode==1)status=xr_source_compiler_build_file(compiler,XR_COMPILER_FIXTURE "/sibling.xr",&product,&diagnostic);
                else if(mode==2) {
                    const char rooted[]="import \"./sibling\" as sibling\nprint(sibling.answer())\n";
                    text=(XrSourceCompilerText){XR_COMPILER_FIXTURE "/unsaved-faults.xr",NULL,rooted,sizeof(rooted)-1};
                    status=xr_source_compiler_build_text(compiler,&text,&product,&diagnostic);
                } else if(mode>=3) status=build_actual_ast(compiler,mode==4,&product,&diagnostic);
                else status=xr_source_compiler_build_text(compiler,&text,&product,&diagnostic);
            }
        }
        if(!point){CHECK(status==XR_SOURCE_COMPILE_OK && product);count=runtime_attempts;CHECK(count);}
        else CHECK(status==XR_SOURCE_COMPILE_OUT_OF_MEMORY && !product && runtime_attempts>runtime_fail_at);
        runtime_fail_at=SIZE_MAX;
        if(point && compiler) {
            CHECK(xr_compile_state_status(xr_source_compiler_state(compiler))==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
            size_t attempts=runtime_attempts;
            const char source[]="const n:i64 = 42\n";
            XrSourceCompilerText retry={NULL,"compiler-owner-faults",source,sizeof(source)-1};
            CHECK(xr_source_compiler_build_text(compiler,&retry,&product,NULL)==XR_SOURCE_COMPILE_OUT_OF_MEMORY && !product);
            CHECK(runtime_attempts==attempts);
        }
        xr_compile_resources_release(resources);
        xr_source_compiler_free(compiler);xr_xir_compile_source_product_free(product);
        xr_source_compile_diagnostic_free(&diagnostic);CHECK(!runtime_live && !runtime_bytes);
    }
    printf("compiler owner mode=%u full allocation faults=%zu physical=0/0\n",mode,count);
    }
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--compiler-faults"))compiler_faults(0,3);
    else if(argc==2 && !strcmp(argv[1],"--ast-faults")){compiler_faults(3,5);compiler_ast_limits();}
    else if(argc==2 && !strcmp(argv[1],"--ast-smoke")){owned_ast_compiler();caller_ast_compiler();}
    else {CHECK(argc==1);owned_compiler();owned_ast_compiler();caller_ast_compiler();compiler_limits();}
    return 0;
}
