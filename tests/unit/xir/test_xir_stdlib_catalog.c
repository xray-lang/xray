/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_stdlib_catalog.c - Trusted Checked input ownership and real failures
 */
#include "xir/xxir_native_cache_internal.h"
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
int main(int argc, char **argv) {
    CHECK(argc == 2 && strlen(argv[1]) < 4096);
    const XrCompileResourceLimits generous = {UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrXirCompileContext context = catalog_context(&generous);
    XrXirLibraryCatalog *occupied = (XrXirLibraryCatalog *)(uintptr_t)17;
    XrCompileResourceStats before = catalog_stats(&context), after;
    catalog_initial_live = before.live_bytes;
    size_t attempts = effects_compile_attempts;
    CHECK(xir_native_cache_library_catalog_new(&context,argv[1],&occupied) == XR_XIR_BAD_STRUCTURE);
    CHECK(occupied == (XrXirLibraryCatalog *)(uintptr_t)17);
    CHECK(xir_native_cache_library_catalog_new(&context,NULL,&occupied) == XR_XIR_BAD_STRUCTURE);
    CHECK(xir_native_cache_library_catalog_new(NULL,argv[1],&occupied) == XR_XIR_BAD_STRUCTURE);
    after = catalog_stats(&context);
    CHECK(!memcmp(&before,&after,sizeof(before)) && attempts == effects_compile_attempts);
    XrXirLibraryCatalog *catalog = NULL;
    char root[4096]; memcpy(root,argv[1],strlen(argv[1])+1);
    attempts = effects_compile_attempts;
    CHECK(xir_native_cache_library_catalog_new(&context,root,&catalog) == XR_XIR_OK);
    size_t sites = effects_compile_attempts-attempts;
    XrCompileResourceStats measured = catalog_stats(&context);
    size_t count = 0;
    const XrModuleResourceBinding *bindings = xr_xir_compile_library_catalog_resources(catalog,&count);
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
        CHECK(xir_native_cache_library_catalog_new(&context,argv[1],&catalog) == XR_XIR_OUT_OF_MEMORY);
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
        XrXirStatus status = xir_native_cache_library_catalog_new(&context,argv[1],&catalog);
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
