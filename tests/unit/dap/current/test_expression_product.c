/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_expression_product.c - Source inference and authenticated scalar evaluation
 */
#define main reference_expression_main
#include "test_expression.c"
#undef main
#include "api/xsource_compiler.h"
static size_t product_expression(size_t fail,bool boolean) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig config={{resources,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&config,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);xr_source_compile_diagnostic_free(&diagnostic);
    const char *text=boolean ? "const evaluator = fn(value:i64) { return value==40 && value<42 }\n" :
        "const evaluator = fn(value:i64) { return value+2 }\n";
    XrSourceCompilerText input={NULL,"dap-expression-product",text,strlen(text)};XrXirSourceProduct *product=NULL;
    XrSourceCompileStatus built=xr_source_compiler_build_text(compiler,&input,&product,&diagnostic);
    if (built!=XR_SOURCE_COMPILE_OK) fprintf(stderr,"expression product source=%u: %s\n",built,diagnostic.source.source.message);
    CHECK(built==XR_SOURCE_COMPILE_OK && product);xr_source_compile_diagnostic_free(&diagnostic);xr_source_compiler_free(compiler);
    const XrXirSourceDebugView *debug=xr_xir_compile_source_product_debug(product);
    const XrXirSourceView *view=xr_xir_compile_source_product_view(product);CHECK(debug && view);
    uint32_t selected=UINT32_MAX;
    for (uint32_t f=0;f<debug->function_count;++f) {
        uint32_t declaration=debug->functions[f].declaration;if (!declaration) continue;
        CHECK(declaration<=view->declaration_count);
        const XrXirSourceDeclaration *entry=&view->declarations[declaration-1];
        if (entry->kind==XR_XIR_SOURCE_FUNCTION) {
            CHECK(!entry->parent && selected==UINT32_MAX && debug->functions[f].parameter_count==1);selected=f;
        }
    }
    CHECK(selected!=UINT32_MAX);
    XrXirValue argument={XR_XIR_I64,0,40},output={0};XrXirCtfeLimits budget={65536,65536,8};
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirCtfeStatus status=xr_xir_compile_source_product_scalar(product,selected,&argument,1,budget,&output);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) {
        if (status!=XR_XIR_CTFE_OK) fprintf(stderr,"Product boolean=%u CTFE status=%u\n",boolean,status);
        CHECK(status==XR_XIR_CTFE_OK && output.type==(uint32_t)(boolean ? XR_XIR_BOOL : XR_XIR_I64) && output.payload==(int64_t)(boolean ? 1 : 42));
    }
    else CHECK(status==XR_XIR_CTFE_OUT_OF_MEMORY && sites>fail && !output.type && !output.payload);
    CHECK(argument.type==XR_XIR_I64 && argument.payload==40);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==before.live_bytes &&
        after.work>before.work && after.allocated_bytes>=before.allocated_bytes);
    xr_xir_value_drop(&output);
    CHECK(xr_xir_compile_source_product_scalar(product,selected,&argument,0,budget,&output)==XR_XIR_CTFE_BAD_ARGUMENT && !output.type && !output.payload);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK && program);
    CHECK(xr_xir_compile_source_product_scalar(product,selected,&argument,1,budget,&output)==XR_XIR_CTFE_BAD_ARGUMENT && !output.type && !output.payload);
    xr_xir_compile_source_product_free(product);xr_xir_compile_program_drop(program);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(resources);CHECK(!runtime_live && !runtime_bytes);return sites;
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool full=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || full);
    size_t sites=product_expression(SIZE_MAX,false);CHECK(sites);
    if (full) for (size_t i=0;i<sites;++i) product_expression(i,false);
    else { product_expression(0,false);if (sites>1) product_expression(sites-1,false); }
    product_expression(SIZE_MAX,true);
    printf("Real Source inferred closure/Closed proof/scalar42, producer death, original ledger, transfer refusal; CTFE sites=%zu injection=%s physical=0/0 PASS\n",sites,full ? "ALL" : "FIRST_LAST_ONLY");return 0;
}
