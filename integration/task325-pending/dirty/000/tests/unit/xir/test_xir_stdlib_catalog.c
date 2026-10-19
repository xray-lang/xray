/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_stdlib_catalog.c - Trusted Checked input ownership and real failures
 */
#include "xir/xxir_stdlib_internal.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_go_source_native_compile_owner.h"
static uint64_t catalog_initial_live;

static XrXirCompileContext catalog_context(const XrCompileResourceLimits *limits) {
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(limits,&context.resources) == XR_COMPILE_RESOURCE_OK);
    return context;
}
static XrCompileResourceStats catalog_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context->resources,&stats) == XR_COMPILE_RESOURCE_OK);
    return stats;
}
static void catalog_release(XrXirCompileContext *context) {
    CHECK(catalog_stats(context).live_bytes == catalog_initial_live);
    xr_compile_resources_release(context->resources); context->resources = NULL;
    CHECK(!effects_compile_live && !effects_compile_bytes);
}
static void catalog_received_check(const XrXirLibraryCatalog *catalog, const char *root) {
    size_t count = 0;
    const XrModuleResourceBinding *bindings = xr_xir_compile_library_catalog_resources_v2(catalog,&count);
    CHECK(count == 1 && bindings && bindings[0].checked);
    CHECK(!strcmp(bindings[0].canonical,"stdlib-module-v1:module=2:io:path=12:io/output.xr"));
    CHECK(!strcmp(bindings[0].logical_path,"io/output.xr"));
    CHECK(!strcmp(bindings[0].authority.namespace_id,"io"));
    CHECK(!strcmp(bindings[0].authority.physical_root,root));
    CHECK(bindings[0].source_locator && bindings[0].source_locator[0]);
    const XrXirModule *module = xr_xir_compile_artifact_module(bindings[0].checked);
    CHECK(module && module->stage == XR_XIR_CHECKED && module->linkage_kind == XR_XIR_LIBRARY);
    CHECK(module->function_count == 3 && module->declarations && module->declarations->module_count == 1);
    unsigned exports = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        if (fn->name_length == 11 && (!memcmp(fn->name,"writeStdout",11) || !memcmp(fn->name,"writeStderr",11))) {
            CHECK(fn->parameter_count == 1 && fn->parameters[0] == XR_XIR_STRING && fn->result == XR_XIR_BOOL);
            ++exports;
        }
    }
    CHECK(exports == 2);
    CHECK(xr_xir_compile_artifact_verify(bindings[0].checked,NULL) == XR_XIR_OK);
}
static void catalog_received_lifetime(const char *expected_root) {
    const XrCompileResourceLimits limits = {UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrXirCompileContext context = catalog_context(&limits);
    uint8_t *bytes = NULL;
    CHECK(xr_compile_resources_alloc(context.resources,xir_stdlib_output_checked_length,(void **)&bytes) == XR_COMPILE_RESOURCE_OK);
    memcpy(bytes,xir_stdlib_output_checked,xir_stdlib_output_checked_length);
    char root[4096], namespace_id[] = "io", logical[] = "io/output.xr";
    memcpy(root,expected_root,strlen(expected_root)+1);
    XrXirLibraryModuleInput binding = {{XR_MODULE_IDENTITY_STDLIB,namespace_id,root},logical};
    XrXirLibraryInput input = {bytes,xir_stdlib_output_checked_length,{0},&binding,1};
    memcpy(input.sha256,xir_stdlib_output_checked_sha256,sizeof(input.sha256));
    XrXirLibraryCatalog *first = NULL, *second = NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(&context,&input,1,&first) == XR_XIR_OK);
    /* Neither caller storage nor another Catalog may be the receiving owner. */
    memset(bytes,0,xir_stdlib_output_checked_length);
    xr_compile_resources_free(bytes); bytes = NULL;
    root[0] = '!'; namespace_id[0] = '!'; logical[0] = '!';
    memset(&input,0,sizeof(input)); memset(&binding,0,sizeof(binding));
    catalog_received_check(first,expected_root);
    size_t count = 0;
    const XrModuleResourceBinding *resources = xr_xir_compile_library_catalog_resources_v2(first,&count);
    CHECK(count == 1);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(resources[0].checked,&packet,NULL) == XR_XIR_OK);
    XrXirLibraryModuleInput received = {{XR_MODULE_IDENTITY_STDLIB,"io",expected_root},"io/output.xr"};
    XrXirLibraryInput received_input = {packet.bytes,packet.length,{0},&received,1};
    xr_sha256(packet.bytes,packet.length,received_input.sha256);
    CHECK(xr_xir_compile_library_catalog_new_v2(&context,&received_input,1,&second) == XR_XIR_OK);
    memset(packet.bytes,0,packet.length);
    xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_library_catalog_free(first); first = NULL;
    xr_compile_resources_release(context.resources);
    CHECK(xr_xir_compile_library_catalog_context(second)->resources == context.resources);
    catalog_received_check(second,expected_root);
    xr_xir_compile_library_catalog_free(second); second = NULL; context.resources = NULL;
    CHECK(!effects_compile_live && !effects_compile_bytes);
    printf("Catalog received bytes/names and first owner/caller ledger died; second owned Library verifies, compilerphysical0\n");
}
static void catalog_rejections(const char *root) {
    const XrCompileResourceLimits limits = {UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrXirCompileContext context = catalog_context(&limits);
    XrCompileResourceStats initial = catalog_stats(&context);
    uint8_t *bytes = NULL;
    CHECK(xr_compile_resources_alloc(context.resources,xir_stdlib_output_checked_length,(void **)&bytes) == XR_COMPILE_RESOURCE_OK);
    for (unsigned mode = 0; mode < 4; ++mode) {
        memcpy(bytes,xir_stdlib_output_checked,xir_stdlib_output_checked_length);
        XrXirLibraryModuleInput binding = {{XR_MODULE_IDENTITY_STDLIB,"io",root},"io/output.xr"};
        XrXirLibraryInput input = {bytes,xir_stdlib_output_checked_length,{0},&binding,1};
        memcpy(input.sha256,xir_stdlib_output_checked_sha256,32);
        if (mode == 0) input.sha256[0] ^= 1;
        else if (mode == 1) {
            bytes[0] ^= 1;
            xr_sha256(bytes,input.length,input.sha256);
        } else if (mode == 2) binding.authority.namespace_id = "untrusted";
        else input.module_count = 0;
        XrXirLibraryCatalog *rejected = NULL;
        CHECK(xr_xir_compile_library_catalog_new_v2(&context,&input,1,&rejected) == XR_XIR_BAD_STRUCTURE);
        CHECK(!rejected);
    }
    xr_compile_resources_free(bytes);
    CHECK(catalog_stats(&context).live_bytes == initial.live_bytes);
    xr_compile_resources_release(context.resources);
    CHECK(!effects_compile_live && !effects_compile_bytes);
}
int main(int argc, char **argv) {
    CHECK((argc == 2 || (argc == 3 && !strcmp(argv[2],"--lifetime-only"))) && strlen(argv[1]) < 4096);
    catalog_received_lifetime(argv[1]);
    catalog_rejections(argv[1]);
    if (argc == 3) return 0;
    const XrCompileResourceLimits generous = {UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrXirCompileContext context = catalog_context(&generous);
    XrXirLibraryCatalog *occupied = (XrXirLibraryCatalog *)(uintptr_t)17;
    XrCompileResourceStats before = catalog_stats(&context), after;
    catalog_initial_live = before.live_bytes;
    size_t attempts = effects_compile_attempts;
    CHECK(xir_stdlib_library_catalog_new(&context,argv[1],&occupied) == XR_XIR_BAD_STRUCTURE);
    CHECK(occupied == (XrXirLibraryCatalog *)(uintptr_t)17);
    CHECK(xir_stdlib_library_catalog_new(&context,NULL,&occupied) == XR_XIR_BAD_STRUCTURE);
    CHECK(xir_stdlib_library_catalog_new(NULL,argv[1],&occupied) == XR_XIR_BAD_STRUCTURE);
    after = catalog_stats(&context);
    CHECK(!memcmp(&before,&after,sizeof(before)) && attempts == effects_compile_attempts);
    XrXirLibraryCatalog *catalog = NULL;
    char root[4096]; memcpy(root,argv[1],strlen(argv[1])+1);
    attempts = effects_compile_attempts;
    CHECK(xir_stdlib_library_catalog_new(&context,root,&catalog) == XR_XIR_OK);
    size_t sites = effects_compile_attempts-attempts;
    XrCompileResourceStats measured = catalog_stats(&context);
    size_t count = 0;
    const XrModuleResourceBinding *bindings = xr_xir_compile_library_catalog_resources_v2(catalog,&count);
    static const char identity[] = "stdlib-module-v1:module=2:io:path=12:io/output.xr";
    CHECK(count == 1 && bindings && !strcmp(bindings[0].canonical,identity));
    CHECK(!strcmp(bindings[0].logical_path,"io/output.xr") && !strcmp(bindings[0].authority.namespace_id,"io"));
    CHECK(bindings[0].authority.kind == XR_MODULE_IDENTITY_STDLIB);
    CHECK(!strcmp(bindings[0].authority.physical_root,argv[1]));
    root[0] = '!'; CHECK(!strcmp(bindings[0].authority.physical_root,argv[1]));
    const XrXirModule *module = xr_xir_compile_artifact_module(bindings[0].checked);
    CHECK(module && module->stage == XR_XIR_CHECKED && module->linkage_kind == XR_XIR_LIBRARY);
    CHECK(module->function_count == 3 && module->declarations->module_count == 1);
    unsigned exports = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        if (fn->name_length == 11 && (!memcmp(fn->name,"writeStdout",11) || !memcmp(fn->name,"writeStderr",11))) {
            CHECK(fn->parameter_count == 1 && fn->parameters[0] == XR_XIR_STRING && fn->result == XR_XIR_BOOL);
            ++exports;
        }
    }
    CHECK(exports == 2);
    /* The catalog pins its original resources after the caller's reference dies. */
    xr_compile_resources_release(context.resources);
    CHECK(xr_xir_compile_library_catalog_context(catalog)->resources == context.resources);
    CHECK(catalog_stats(&context).live_bytes == measured.live_bytes);
    xr_xir_compile_library_catalog_free(catalog); catalog = NULL; context.resources = NULL;
    CHECK(!effects_compile_live && !effects_compile_bytes);
    for (size_t site = 0; site < sites; ++site) {
        context = catalog_context(&generous); attempts = effects_compile_attempts;
        effects_compile_fail_at = attempts+site; effects_compile_injected = false;
        CHECK(xir_stdlib_library_catalog_new(&context,argv[1],&catalog) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!catalog && effects_compile_injected && effects_compile_attempts >= attempts+site+1);
        effects_compile_fail_at = SIZE_MAX;
        catalog_release(&context);
        printf("catalog OOM %zu/%zu hit1 outputNULL compilerphysical0\n",site,sites);
    }
    uint64_t fees[3] = {measured.allocated_bytes,measured.peak_bytes,measured.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = generous;
        uint64_t bound = fees[axis]-minus;
        if (axis == 0) limits.allocated_bytes = bound;
        else if (axis == 1) limits.live_bytes = bound;
        else limits.work = bound;
        context = catalog_context(&limits);
        XrXirStatus status = xir_stdlib_library_catalog_new(&context,argv[1],&catalog);
        CHECK(status == (minus ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(minus ? !catalog : !!catalog);
        xr_xir_compile_library_catalog_free(catalog); catalog = NULL;
        catalog_release(&context);
        printf("catalog axis%u minus%u bound%llu status%u compilerphysical0\n",axis,minus,(unsigned long long)bound,status);
    }
    effects_compile_fail_at = effects_compile_attempts; effects_compile_injected = false;
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&generous,&resources) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    CHECK(!resources && effects_compile_injected && !effects_compile_live && !effects_compile_bytes);
    effects_compile_fail_at = SIZE_MAX;
    printf("trusted stdlib Catalog sites%zu allocated%llu peak%llu work%llu, deep root and caller death PASS\n",
        sites,(unsigned long long)fees[0],(unsigned long long)fees[1],(unsigned long long)fees[2]);
    return 0;
}
