/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_ast.c - Actual AST semantics and independent Program ownership
 */
#include "program/xr_xir_source_product.h"
#include "frontend/parser/xparse.h"
#include "frontend/parser/xast.h"
#include "toolchain/xcompiler_session.h"
#include "toolchain/xcompiler_arena_backing.h"
#include "base/xarena.h"
#include "xir/xxir_vm.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_compile_owner.h"

static const char source_text[] = "export fn answer()->i64 { return 41 }\n";
static XrCompileResourceStats observed;
static XrCompileResourceLimits limits(void) {
    return (XrCompileResourceLimits){UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
}
static XrXirStatus session_status(XrCompilerSessionStatus status) {
    return status == XR_COMPILER_SESSION_OK ? XR_XIR_OK :
        status == XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET :
        status == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus parse_status(XrParseStatus status) {
    return status == XR_PARSE_OK ? XR_XIR_OK : status == XR_PARSE_BUDGET ? XR_XIR_BUDGET :
        status == XR_PARSE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
}
static AstNode *literal(AstNode *ast) {
    CHECK(ast && ast->type == AST_PROGRAM && ast->as.program.count == 1);
    AstNode *function = ast->as.program.statements[0]; CHECK(function->type == AST_FUNCTION_DECL);
    AstNode *body = function->as.function_decl.body; CHECK(body && body->type == AST_BLOCK && body->as.block.count == 1);
    AstNode *ret = body->as.block.statements[0]; CHECK(ret->type == AST_RETURN_STMT && ret->as.return_stmt.value_count == 1);
    AstNode *value = ret->as.return_stmt.values[0]; CHECK(value->type == AST_LITERAL_INT);
    return value;
}
static void physical_zero(void) { CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes); }
static void execute(XrXirSourceProduct *product, int64_t expected) {
    const XrXirSourceView *view = xr_xir_compile_source_product_view(product);
    CHECK(view && view->complete);
    bool found = false;
    for (uint32_t i=0;i<view->declaration_count;++i) if (!strcmp(view->declarations[i].name,"answer")) {
        const XrXirSourceQueryModule *module = &view->modules[view->declarations[i].range.module];
        XrFingerprint unknown = {0}; CHECK(!memcmp(&module->fingerprint,&unknown,sizeof(unknown))); found = true;
    }
    CHECK(found);
    XrXirSourceProductPacketView packet = {0}; XrXirArtifact *checked = NULL;
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(xr_xir_compile_source_product_context(product),packet.bytes,packet.length,&checked,NULL) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(checked); uint32_t answer = UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length == 6 && !memcmp(module->functions[f].name,"answer",6)) answer = f;
    CHECK(answer != UINT32_MAX); xr_xir_compile_artifact_free(checked);
    uint32_t entry = xr_xir_compile_source_product_facts(product)->entry;
    XrXirProgram *program = NULL; CHECK(xr_xir_compile_source_product_vm_take(product,&program) == XR_XIR_OK);
    xr_xir_compile_source_product_free(product);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config,sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instance = NULL; CHECK(xr_xir_instance_new(program,&config,&instance) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    for (unsigned i=0;i<2;++i) {
        CHECK(xr_xir_instance_start(instance,i ? answer : entry,NULL,0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(instance,&value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && value.payload == (i ? expected : 0)); xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
}
/* Each run owns one finite ledger, including parsing, both sessions and product. */
static XrXirStatus build(XrCompileResourceLimits caps, const char *root, const char *text, bool run) {
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    XrCompileResourceStatus created = xr_compile_resources_new(&caps,&context.resources);
    if (created != XR_COMPILE_RESOURCE_OK) return created == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    XrCompilerSession *producer = NULL, *consumer = NULL; AstNode *ast = NULL; XrXirSourceProduct *product = NULL;
    XrXirStatus status = session_status(xr_compile_session_new(context.resources,&producer));
    char path[XR_TEST_PATH_MAX] = {0};
    if (root) CHECK(snprintf(path,sizeof(path),"%s/root.xr",root) > 0);
    if (status == XR_XIR_OK) status = parse_status(xr_compile_parse_with_source(producer,text,root ? path : NULL,&ast));
    if (status == XR_XIR_OK && !root) {
        AstNode *value = literal(ast); value->as.literal.int_bits = 42; value->as.literal.raw_value.int_val = 42;
    }
    xr_compile_session_free(producer);
    if (status == XR_XIR_OK) status = session_status(xr_compile_session_new(context.resources,&consumer));
    XrModuleIdentityAuthority authority = {root ? XR_MODULE_IDENTITY_SCRIPT : XR_MODULE_IDENTITY_MEMORY,
        root ? NULL : "actual-ast",root};
    XrXirSourceProductDiagnostic diagnostic = {0};
    if (status == XR_XIR_OK) {
        XrXirSourceProductRequest request = {{consumer,root ? path : NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL},
            {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
        XrXirSourceAst input = {ast,root ? "root.xr" : NULL};
        status = xr_xir_compile_source_product_build_ast(&request,&input,&product,&diagnostic);
        CHECK(diagnostic.status == status);
        if (!root) CHECK(literal(ast)->as.literal.int_bits == 42 && ast->as.program.owns_arena);
    }
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_program_destroy(ast); xr_compile_session_free(consumer);
    CHECK(xr_compile_resources_stats(context.resources,&observed) == XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(context.resources);
    if (status == XR_XIR_OK && run) { execute(product,root ? 43 : 42); product = NULL; }
    if (status != XR_XIR_OK) CHECK(!product);
    xr_xir_compile_source_product_free(product); physical_zero(); return status;
}
static void rejections(void) {
    XrCompileResourceLimits caps = limits(); XrCompileResources *resources = NULL, *foreign = NULL;
    CHECK(xr_compile_resources_new(&caps,&resources) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_new(&caps,&foreign) == XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *producer = NULL, *consumer = NULL;
    CHECK(xr_compile_session_new(resources,&producer) == XR_COMPILER_SESSION_OK);
    CHECK(xr_compile_session_new(foreign,&consumer) == XR_COMPILER_SESSION_OK);
    AstNode *ast = NULL; CHECK(xr_compile_parse(producer,source_text,&ast) == XR_PARSE_OK);
    xr_compile_session_free(producer);
    XrXirCompileContext context = {foreign,xr_xir_compile_default_limits()};
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY,"reject-ast",NULL};
    XrXirSourceRequest request = {consumer,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0}; XrXirSourceAst input = {ast,NULL};
    CHECK(xr_xir_compile_source_check_ast(&request,&input,&result,&diagnostic,NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(!result.checked && !result.snapshot && ast->as.program.owns_arena);
    xr_compile_session_free(consumer); consumer = NULL; xr_compile_resources_release(foreign);
    CHECK(xr_compile_session_new(resources,&consumer) == XR_COMPILER_SESSION_OK);
    context.resources = resources; request.session = consumer;
    ast->as.program.owns_arena = false;
    CHECK(xr_xir_compile_source_check_ast(&request,&input,&result,&diagnostic,NULL) == XR_XIR_BAD_STRUCTURE);
    ast->as.program.owns_arena = true;
    XrArena *original_arena = ast->as.program.arena;
    XrArena unrelated = {.backing = {.context = (void *)(uintptr_t)1}, .retained = true};
    ast->as.program.arena = &unrelated;
    CHECK(xr_xir_compile_source_check_ast(&request,&input,&result,&diagnostic,NULL) == XR_XIR_BAD_STRUCTURE);
    ast->as.program.arena = original_arena;
    CHECK(xr_xir_compile_source_check_ast(&request,NULL,&result,&diagnostic,NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(xr_xir_compile_source_check_ast(&request,&input,&result,&diagnostic,NULL) == XR_XIR_OK);
    XrXirSourceResult saved = result; size_t attempts = source_fixture_compile_attempts;
    CHECK(xr_xir_compile_source_check_ast(&request,&input,&result,&diagnostic,NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(result.checked == saved.checked && result.snapshot == saved.snapshot && source_fixture_compile_attempts == attempts);
    xr_xir_compile_source_result_free(&result);
    AstNode *value = literal(ast); value->type = AST_VARIABLE; value->as.variable.name = "absent_name";
    XrXirStatus rejected = xr_xir_compile_source_check_ast(&request,&input,&result,&diagnostic,NULL);
    CHECK(rejected == XR_XIR_BAD_VALUE && !strcmp(diagnostic.message,"name is not an initialized value"));
    CHECK(!result.checked && !result.snapshot && ast->as.program.owns_arena);
    xr_program_destroy(ast); xr_compile_session_free(consumer); xr_compile_resources_release(resources); physical_zero();
}
static void resources(const char *root, const char *text, const char *family) {
    size_t sites = 0; XrCompileResourceStats required = {0};
    for (size_t point=0;point<=sites;++point) {
        source_fixture_compile_attempts = 0; source_fixture_compile_injected = false;
        source_fixture_compile_fail_at = point ? point-1 : SIZE_MAX;
        XrXirStatus status = build(limits(),root,text,false); physical_zero();
        if (!point) { CHECK(status == XR_XIR_OK); sites = source_fixture_compile_attempts; required = observed; CHECK(sites); }
        else CHECK(status == XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected);
    }
    source_fixture_compile_fail_at = SIZE_MAX;
    for (unsigned axis=0;axis<3;++axis) for (int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits caps = {required.allocated_bytes,required.peak_bytes,required.work};
        uint64_t *bound = axis == 0 ? &caps.allocated_bytes : axis == 1 ? &caps.live_bytes : &caps.work;
        CHECK(*bound && *bound < UINT64_MAX); *bound = (uint64_t)((int64_t)*bound+delta);
        CHECK(build(caps,root,text,false) == (delta < 0 ? XR_XIR_BUDGET : XR_XIR_OK));
    }
    printf("Actual AST %s complete product sites=%zu; three axes exact/minus1/plus1; physical=0/0\n",family,sites);
}
int main(int argc, char **argv) {
    CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1],"--resources")));
    bool faults = argc == 2;
    if (faults) resources(NULL,source_text,"memory");
    else { CHECK(build(limits(),NULL,source_text,true) == XR_XIR_OK); rejections(); }
    char directory[XR_TEST_PATH_MAX] = "xir-actual-ast-XXXXXX", root[XR_TEST_PATH_MAX], path[XR_TEST_PATH_MAX];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory,root,sizeof(root)));
    CHECK(snprintf(path,sizeof(path),"%s/library.xr",root) > 0);
    FILE *file = fopen(path,"wb"); CHECK(file);
    CHECK(fputs("export fn dependency()->i64 { return 41 }\n",file) >= 0 && fclose(file) == 0);
    const char imported[] = "import { dependency } from \"./library\"\nexport fn answer()->i64 { return dependency()+2 }\n";
    if (faults) resources(root,imported,"rooted-import");
    else CHECK(build(limits(),root,imported,true) == XR_XIR_OK);
    char manifest[XR_TEST_PATH_MAX]; CHECK(snprintf(manifest,sizeof(manifest),"%s/xray.toml",root) > 0);
    file = fopen(manifest,"wb"); CHECK(file);
    CHECK(fputs("[declarations]\nversion=1\n[[declarations.function]]\nmodule=\"root.xr\"\nname=\"answer\"\nno_suspend=true\n",file) >= 0 && fclose(file) == 0);
    if (faults) resources(root,imported,"rooted-manifest");
    else CHECK(build(limits(),root,imported,true) == XR_XIR_OK);
    file = fopen(manifest,"wb"); CHECK(file);
    CHECK(fputs("[declarations]\nversion=1\n[[declarations.function]]\nmodule=\"root.xr\"\nname=\"absent\"\nno_suspend=true\n",file) >= 0 && fclose(file) == 0);
    CHECK(build(limits(),root,imported,false) == XR_XIR_BAD_STRUCTURE);
    CHECK(xr_test_unlink(manifest) == 0 && xr_test_unlink(path) == 0 && xr_test_rmdir(directory) == 0);
    puts("Actual AST mutation42, producer/AST death, owned Program, ordinary imports and failure preservation passed"); return 0;
}
