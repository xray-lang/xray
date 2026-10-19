/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_text.c - Explicit source bytes through the owned product pipeline
 *
 * KEY CONCEPT:
 *   Text, authority and parser owners die before product queries and execution.
 */
#include "program/xr_xir_source_product.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_output.h"
#include "toolchain/xcompiler_session.h"
#include "execution/xr_xir_host_execution.h"
#include "frontend/parser/xparse.h"
#include "frontend/parser/xast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
#include "xir/xxir_vm.c"
#include "program/xr_xir_source_product.c"

typedef struct TextWorld {
    XrCompileResources *resources;
    XrCompilerSession *session;
    XrXirCompileContext context;
    XrModuleIdentityAuthority authority;
    XrXirSourceProductRequest request;
} TextWorld;
static void world_open_limits(TextWorld *w, XrCompileResourceLimits limits) {
    memset(w, 0, sizeof(*w));
    CHECK(xr_compile_resources_new(&limits, &w->resources) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_new(w->resources, &w->session) == XR_COMPILER_SESSION_OK);
    w->context = (XrXirCompileContext){w->resources, xr_xir_compile_default_limits()};
    w->authority = (XrModuleIdentityAuthority){.kind = XR_MODULE_IDENTITY_MEMORY,
        .namespace_id = "source-product-text-owned"};
    w->request = (XrXirSourceProductRequest){{w->session, NULL, &w->authority,
        &w->context, NULL, NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
}
static void world_open(TextWorld *w, uint64_t work) {
    world_open_limits(w, (XrCompileResourceLimits){UINT64_C(64) << 20, UINT64_C(8) << 20, work});
}
static void world_close(TextWorld *w) {
    xr_compile_session_free(w->session);
    xr_compile_resources_release(w->resources);
    memset(w, 0, sizeof(*w));
}
typedef struct Bytes { char data[32]; size_t length; } Bytes;
static XrXirOutputStatus write_bytes(void *context, XrXirOutputStream stream,
                                     const char *bytes, size_t length) {
    Bytes *out = context;
    if (stream != XR_XIR_STDOUT || length > sizeof(out->data) - out->length)
        return XR_XIR_OUTPUT_ERROR;
    memcpy(out->data + out->length, bytes, length); out->length += length;
    return XR_XIR_OUTPUT_OK;
}
static void text_owned_execution(void) {
    TextWorld w; world_open(&w, UINT64_C(128000000));
    const char original[] = "print(\"A\\0B\")\n";
    char bytes[sizeof(original) - 1]; memcpy(bytes, original, sizeof(bytes));
    XrXirSourceText text = {NULL, bytes, sizeof(bytes)};
    XrXirSourceProduct *product = NULL;
    CHECK(xr_xir_compile_source_product_build_text(&w.request, &text, &product, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_source_product_public_admit(product, NULL) == XR_XIR_OK);
    memset(bytes, '!', sizeof(bytes));
    world_close(&w);
    CHECK(xr_xir_compile_source_product_view(product)->complete);
    CHECK(xr_xir_compile_source_product_verify(product, 1048576, NULL) == XR_XIR_OK);
    const XrXirSourceProductFacts *facts = xr_xir_compile_source_product_facts(product);
    CHECK(facts && facts->module_count == 1); uint32_t entry = facts->entry;
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_source_product_vm_take(product, &program) == XR_XIR_OK);
    CHECK(xr_xir_compile_source_product_vm_take(product, &program) == XR_XIR_BAD_STRUCTURE);
    xr_xir_compile_source_product_free(product);
    Bytes out = {{0}, 0};
    XrXirOutputSink sink = {XR_XIR_CALL_ABI_VERSION, 0, write_bytes, &out, sizeof(out.data)};
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, xr_xir_output_render, &sink};
    XrXirHostExecutionRequest run = {program, &config, entry, NULL, 0};
    XrXirCallResult result = {0};
    CHECK(xr_xir_host_execute(&run, &result) == XR_XIR_CALL_RETURNED);
    CHECK(result.value.type == XR_XIR_I64 && result.value.payload == 0);
    CHECK(out.length == 4 && !memcmp(out.data, "A\0B\n", 4));
    xr_xir_compile_program_drop(program); xr_xir_call_result_drop(&result);
    CHECK(!runtime_live && !runtime_bytes);
}
static void rejects_and_diagnostics(void) {
    TextWorld w; world_open(&w, UINT64_C(128000000));
    XrXirSourceProduct *out = NULL; XrXirSourceProductDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_source_product_build_text(&w.request, NULL, &out, &diagnostic) == XR_XIR_BAD_STRUCTURE);
    CHECK(!out && diagnostic.stage == XR_XIR_SOURCE_PRODUCT_INPUT);
    const char source[] = "var value = missing_symbol\n";
    XrXirSourceText text = {NULL, source, sizeof(source) - 1};
    CHECK(xr_xir_compile_source_product_build_text(&w.request, &text, &out, &diagnostic) != XR_XIR_OK);
    CHECK(!out && diagnostic.stage == XR_XIR_SOURCE_PRODUCT_CHECK);
    CHECK(diagnostic.source.line == 1 && diagnostic.source.column == 13);
    CHECK(strstr(diagnostic.source.message, "initialized value"));
    world_close(&w);
    CHECK(diagnostic.source.line == 1 && diagnostic.source.column == 13);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    CHECK(!runtime_live && !runtime_bytes);
    world_open(&w, UINT64_C(128000000));
    const char nul[] = {'p','r','i','n','t','(',')',0,'\n'};
    text = (XrXirSourceText){NULL, nul, sizeof(nul)};
    CHECK(xr_xir_compile_source_product_build_text(&w.request, &text, &out, &diagnostic) != XR_XIR_OK && !out);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic); world_close(&w);
    CHECK(!runtime_live && !runtime_bytes);
}
static XrXirStatus build_constant(TextWorld *w, XrXirSourceProduct **out) {
    const char source[] = "const n:i64 = 42\n";
    XrXirSourceText text = {NULL, source, sizeof(source)-1};
    return xr_xir_compile_source_product_build_text(&w->request, &text, out, NULL);
}
static void exact_resources_and_retry(void) {
    TextWorld w; world_open(&w, UINT64_C(128000000));
    XrXirSourceProduct *product = NULL;
    CHECK(build_constant(&w, &product) == XR_XIR_OK);
    XrCompileResourceStats required;
    CHECK(xr_compile_resources_stats(w.resources, &required) == XR_COMPILE_RESOURCE_OK);
    xr_xir_compile_source_product_free(product); world_close(&w);
    CHECK(!runtime_live && !runtime_bytes);
    for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
        XrCompileResourceLimits limits = {required.allocated_bytes, required.peak_bytes, required.work};
        uint64_t *bound = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        CHECK(*bound > 1 && *bound < UINT64_MAX);
        *bound = (uint64_t)((int64_t)*bound + delta);
        world_open_limits(&w, limits); product = NULL;
        CHECK(build_constant(&w, &product) == (delta < 0 ? XR_XIR_BUDGET : XR_XIR_OK));
        if (delta < 0) CHECK(!product);
        xr_xir_compile_source_product_free(product); world_close(&w);
        CHECK(!runtime_live && !runtime_bytes);
    }
    world_open_limits(&w, (XrCompileResourceLimits){required.allocated_bytes, required.peak_bytes, required.work});
    product = NULL; CHECK(build_constant(&w, &product) == XR_XIR_OK);
    XrCompileResourceStats before, after;
    CHECK(xr_compile_resources_stats(w.resources, &before) == XR_COMPILE_RESOURCE_OK);
    xr_xir_compile_source_product_free(product); product = NULL;
    CHECK(xr_compile_resources_stats(w.resources, &after) == XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes < before.live_bytes && after.allocated_bytes == before.allocated_bytes && after.work == before.work);
    CHECK(build_constant(&w, &product) == XR_XIR_BUDGET && !product);
    CHECK(build_constant(&w, &product) == XR_XIR_BUDGET && !product);
    world_close(&w); CHECK(!runtime_live && !runtime_bytes);
    puts("three resource axes exact/minus1/plus1 and cumulative retry without refund PASS");
}
static void rooted_text_authority(void) {
    TextWorld w; world_open(&w, UINT64_C(128000000));
    w.authority = (XrModuleIdentityAuthority){XR_MODULE_IDENTITY_SCRIPT, NULL, XR_TEXT_FIXTURE_ROOT};
    w.request.source.entry_path = XR_TEXT_FIXTURE_ROOT "/unsaved.xr";
    const char source[] = "import \"./sibling\" as sibling\nprint(sibling.answer())\n";
    XrXirSourceText text = {"unsaved.xr", source, sizeof(source)-1};
    XrXirSourceProduct *product = NULL;
    XrXirSourceProductDiagnostic diagnostic = {0};
    XrXirStatus built = xr_xir_compile_source_product_build_text(&w.request, &text, &product, &diagnostic);
    if (built != XR_XIR_OK) fprintf(stderr, "rooted status=%d stage=%u path=%s message=%s\n", built, (unsigned)diagnostic.stage, diagnostic.source_path ? diagnostic.source_path : "", diagnostic.source.message);
    CHECK(built == XR_XIR_OK);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    CHECK(xr_xir_compile_source_product_public_admit(product, NULL) == XR_XIR_OK);
    const XrXirSourceProductFacts *facts = xr_xir_compile_source_product_facts(product);
    CHECK(facts && facts->module_count == 2);
    AstNode *rooted_ast = NULL;
    CHECK(xr_compile_parse_with_source(w.session, text.text, w.request.source.entry_path,
        &rooted_ast) == XR_PARSE_OK);
    XrXirSourceAst rooted_syntax = {text, rooted_ast};
    XrXirSourceProduct *ast_product = NULL;
    CHECK(xr_xir_compile_source_product_build_ast(&w.request, &rooted_syntax,
        &ast_product, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_source_product_facts(ast_product)->module_count == 2);
    xr_program_destroy(rooted_ast);
    CHECK(xr_xir_compile_source_product_verify(ast_product, 1048576, NULL) == XR_XIR_OK);
    xr_xir_compile_source_product_free(ast_product);
    const XrXirSourceView *view = xr_xir_compile_source_product_view(product);
    bool found = false;
    for (uint32_t i = 0; i < view->module_count; ++i)
        if (view->modules[i].path && !strcmp(view->modules[i].path, w.request.source.entry_path)) found = true;
    CHECK(found);
    XrXirSourceProduct *occupied = product;
    CHECK(xr_xir_compile_source_product_build_text(&w.request, &text, &product, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(product == occupied);
    text.logical_path = "other.xr";
    XrXirSourceProduct *rejected = NULL;
    CHECK(xr_xir_compile_source_product_build_text(&w.request, &text, &rejected, NULL) == XR_XIR_BAD_STRUCTURE && !rejected);
    world_close(&w);
    CHECK(xr_xir_compile_source_product_verify(product, 1048576, NULL) == XR_XIR_OK);
    xr_xir_compile_source_product_free(product);
    CHECK(!runtime_live && !runtime_bytes);
}
static void complete_compiler_failures(void) {
    size_t count = 0;
    for (size_t point = 0; !point || point <= count; ++point) {
        runtime_attempts = 0; runtime_fail_at = point ? point - 1 : SIZE_MAX;
        XrCompileResourceLimits limits = {UINT64_C(64) << 20, UINT64_C(8) << 20, UINT64_C(128000000)};
        TextWorld w = {0}; XrXirSourceProduct *out = NULL; XrXirSourceProductDiagnostic diagnostic = {0};
        XrCompileResourceStatus created = xr_compile_resources_new(&limits, &w.resources);
        XrCompilerSessionStatus opened = created == XR_COMPILE_RESOURCE_OK ?
            xr_compile_session_new(w.resources, &w.session) : XR_COMPILER_SESSION_OUT_OF_MEMORY;
        XrXirStatus status = XR_XIR_OUT_OF_MEMORY;
        if (opened == XR_COMPILER_SESSION_OK) {
            w.context = (XrXirCompileContext){w.resources, xr_xir_compile_default_limits()};
            w.authority = (XrModuleIdentityAuthority){.kind=XR_MODULE_IDENTITY_MEMORY,.namespace_id="source-text-faults"};
            w.request = (XrXirSourceProductRequest){{w.session,NULL,&w.authority,&w.context,NULL,NULL,XR_XIR_PROGRAM,NULL},
                {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
            const char source[] = "const n:i64 = 42\n";
            XrXirSourceText text = {NULL,source,sizeof(source)-1};
            status = xr_xir_compile_source_product_build_text(&w.request,&text,&out,&diagnostic);
        }
        if (!point) { CHECK(status == XR_XIR_OK); count = runtime_attempts; CHECK(count); }
        else { CHECK(status == XR_XIR_OUT_OF_MEMORY && !out && runtime_attempts > runtime_fail_at); }
        runtime_fail_at = SIZE_MAX; world_close(&w);
        xr_xir_compile_source_product_free(out); xr_xir_compile_source_product_diagnostic_free(&diagnostic);
        CHECK(!runtime_live && !runtime_bytes);
    }
    printf("text-product full compiler allocation faults=%zu; original ledger and physical zero\n",count);
}
static XrXirStatus ast_compile_trial(XrCompileResourceLimits limits,
    XrCompileResourceStats *measured) {
    TextWorld w = {0}; AstNode *ast = NULL;
    XrXirSourceProduct *product = NULL; XrXirSourceProductDiagnostic diagnostic = {0};
    XrCompileResourceStatus allocation = xr_compile_resources_new(&limits, &w.resources);
    XrXirStatus status = allocation == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        allocation == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    if (status == XR_XIR_OK) {
        XrCompilerSessionStatus opened = xr_compile_session_new(w.resources, &w.session);
        status = opened == XR_COMPILER_SESSION_OK ? XR_XIR_OK :
            opened == XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    }
    const char source[] = "const n:i64 = 42\n";
    if (status == XR_XIR_OK) {
        w.context = (XrXirCompileContext){w.resources, xr_xir_compile_default_limits()};
        w.authority = (XrModuleIdentityAuthority){XR_MODULE_IDENTITY_MEMORY,"caller-ast",NULL};
        w.request = (XrXirSourceProductRequest){{w.session,NULL,&w.authority,&w.context,
            NULL,NULL,XR_XIR_PROGRAM,NULL},{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
        XrParseStatus parsed = xr_compile_parse(w.session, source, &ast);
        status = parsed == XR_PARSE_OK ? XR_XIR_OK : parsed == XR_PARSE_BUDGET ?
            XR_XIR_BUDGET : parsed == XR_PARSE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
    }
    if (status == XR_XIR_OK) {
        XrXirSourceAst syntax = {{NULL,source,sizeof(source)-1},ast};
        status = xr_xir_compile_source_product_build_ast(&w.request,&syntax,&product,&diagnostic);
    }
    if (measured && w.resources)
        CHECK(xr_compile_resources_stats(w.resources,measured)==XR_COMPILE_RESOURCE_OK);
    runtime_fail_at = SIZE_MAX;
    xr_program_destroy(ast); world_close(&w);
    xr_xir_compile_source_product_free(product);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    CHECK(!runtime_live && !runtime_bytes);
    return status;
}
static void ast_complete_faults_and_limits(void) {
    XrCompileResourceLimits limits = {UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
    XrCompileResourceStats normal = {0}; runtime_attempts=0; runtime_fail_at=SIZE_MAX;
    CHECK(ast_compile_trial(limits,&normal)==XR_XIR_OK);
    size_t sites=runtime_attempts; CHECK(sites && sites<20000);
    for (size_t i=0;i<sites;++i) {
        runtime_attempts=0; runtime_fail_at=i;
        CHECK(ast_compile_trial(limits,NULL)==XR_XIR_OUT_OF_MEMORY);
        CHECK(runtime_attempts>i);
    }
    for (unsigned axis=0;axis<3;++axis) for (int delta=-1;delta<=1;++delta) {
        limits=(XrCompileResourceLimits){normal.allocated_bytes,normal.peak_bytes,normal.work};
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(*bound>1); *bound=(uint64_t)((int64_t)*bound+delta);
        runtime_fail_at=SIZE_MAX;
        CHECK(ast_compile_trial(limits,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
    }
    printf("caller-AST complete compiler faults=%zu; three original axes exact/minus/plus and physical zero\n",sites);
}
static void ast_owned_execution_and_guards(void) {
    TextWorld w; world_open(&w,UINT64_C(128000000));
    const char source[]="const n:i64 = 41\nprint(n)\n";
    AstNode *ast=NULL; CHECK(xr_compile_parse(w.session,source,&ast)==XR_PARSE_OK);
    CHECK(ast->type==AST_PROGRAM && ast->as.program.count==2);
    AstNode *literal=ast->as.program.statements[0]->as.var_decl.initializer;
    CHECK(literal && literal->type==AST_LITERAL_INT); literal->as.literal.int_bits=42;
    XrXirSourceAst syntax={{NULL,source,sizeof(source)-1},ast};
    XrXirSourceProduct *product=NULL;
    XrCompilerSession *foreign=NULL;
    CHECK(xr_compile_session_new(w.resources,&foreign)==XR_COMPILER_SESSION_OK);
    XrXirSourceProductRequest bad=w.request; bad.source.session=foreign;
    CHECK(xr_xir_compile_source_product_build_ast(&bad,&syntax,&product,NULL)==XR_XIR_BAD_STRUCTURE && !product);
    xr_compile_session_free(foreign);
    XrXirSourceProduct *occupied=(XrXirSourceProduct *)(uintptr_t)17;
    CHECK(xr_xir_compile_source_product_build_ast(&w.request,&syntax,&occupied,NULL)==XR_XIR_BAD_STRUCTURE && occupied==(XrXirSourceProduct *)(uintptr_t)17);
    CHECK(xr_xir_compile_source_product_build_ast(&w.request,&syntax,&product,NULL)==XR_XIR_OK);
    CHECK(literal->as.literal.int_bits==42 && ast->as.program.count==2);
    CHECK(xr_xir_compile_source_product_public_admit(product,NULL)==XR_XIR_OK);
    uint32_t entry=xr_xir_compile_source_product_facts(product)->entry;
    xr_program_destroy(ast); world_close(&w);
    CHECK(xr_xir_compile_source_product_view(product)->complete);
    CHECK(xr_xir_compile_source_product_verify(product,1048576,NULL)==XR_XIR_OK);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK);
    xr_xir_compile_source_product_free(product);
    Bytes output={{0},0};
    XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,write_bytes,&output,sizeof(output.data)};
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirHostExecutionRequest run={program,&config,entry,NULL,0};XrXirCallResult result={0};
    CHECK(xr_xir_host_execute(&run,&result)==XR_XIR_CALL_RETURNED);
    CHECK(output.length==3 && !memcmp(output.data,"42\n",3));
    puts("caller-AST actual edited tree42, exact parser state, rooted imports and producer-death execution PASS");
    xr_xir_compile_program_drop(program);xr_xir_call_result_drop(&result);
    CHECK(!runtime_live && !runtime_bytes);
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--compiler-faults")) { complete_compiler_failures(); ast_complete_faults_and_limits(); }
    else { CHECK(argc == 1); text_owned_execution(); rejects_and_diagnostics(); exact_resources_and_retry(); rooted_text_authority(); ast_owned_execution_and_guards(); }
    puts("owned explicit source product PASS"); return 0;
}
