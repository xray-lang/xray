/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_native_cache.c - Static native hits and genuine Lowered misses
 */
#include "xir/xxir_vm_internal.h"
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_output.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(test) do { if (!(test)) { fprintf(stderr,"cache test FAIL %d %s\n",__LINE__,#test); exit(1); } } while (0)
#include "xir_go_source_native_compile_owner.h"
#include "xir_runtime_allocations.h"
/* Static registry mutations are test inputs to validation, never public code authority. */
#include "xir/xxir_native_cache.c"
typedef struct CacheProbe { unsigned vector, calls; XrXirOutputStatus status; } CacheProbe;
static const char *const cache_oracle_bytes[] = {"out\0\xe4\xb8\xad","err!",""};
static const size_t cache_oracle_lengths[] = {7,4,0};
static XrXirOutputStatus cache_write(void *context, XrXirOutputStream stream,
    const char *data, size_t length) {
    CacheProbe *probe = context;
    CHECK(stream == (probe->vector == 1 ? XR_XIR_STDERR : XR_XIR_STDOUT));
    CHECK(length == cache_oracle_lengths[probe->vector] && (!length || !memcmp(data,cache_oracle_bytes[probe->vector],length)));
    ++probe->calls; return probe->status;
}
static XrXirArtifact *cache_program(const XrXirCompileContext *context,
    const char *root, const char *entry, uint32_t ids[6]) {
    XrXirLibraryInput input = {xir_native_cache_registry.checked.bytes,xir_native_cache_registry.checked.length,{0}, (XrXirLibraryModuleInput[]){{{XR_MODULE_IDENTITY_STDLIB,"io",root},"io/output.xr"}},1};
    memcpy(input.sha256,xir_native_cache_registry.checked.digest,32);
    XrXirLibraryCatalog *catalog = NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog) == XR_XIR_OK);
    XrCompilerSession *session = NULL; CHECK(xr_compile_session_new(context->resources,&session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request = {session,entry,&authority,context,root,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if (status != XR_XIR_OK) fprintf(stderr,"cache consumer Source status=%u %s\n",status,diagnostic.message);
    CHECK(status == XR_XIR_OK);
    XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&decoded,NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(closed,NULL) == XR_XIR_OK);
    XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    for (unsigned i = 0; i < 6; ++i) ids[i] = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        const XrXirFunctionIdentity *declaration = &module->declarations->functions[f];
        const char *names[] = {"writeStdout","writeStderr","stdoutText","stderrText","writeStdout","writer"};
        for (unsigned i = 0; i < 6; ++i) {
            bool library = declaration->module != module->declarations->root_module;
            if (library != (i < 2)) continue;
            if (function->name_length == strlen(names[i]) && !memcmp(function->name,names[i],function->name_length)) {
                CHECK(ids[i] == UINT32_MAX); ids[i] = f;
            }
        }
    }
    for (unsigned i = 0; i < 6; ++i) CHECK(ids[i] != UINT32_MAX);
    xr_xir_compile_artifact_free(closed); xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_checked_packet_free(&packet); xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session); xr_xir_compile_library_catalog_free(catalog);
    return lowered;
}
static void cache_execute(XrXirProgram *program, const uint32_t ids[2]) {
    const XrXirOutputStatus statuses[] = {XR_XIR_OUTPUT_OK,XR_XIR_OUTPUT_ERROR,XR_XIR_OUTPUT_OOM,XR_XIR_OUTPUT_LIMIT};
    for (unsigned instance_number = 0; instance_number < 2; ++instance_number)
        for (unsigned mode = 0; mode < 4; ++mode)
            for (unsigned vector = 0; vector < 3; ++vector) {
                CacheProbe probe = {vector,0,statuses[mode]};
                XrXirOutputSink sink = {XR_XIR_CALL_ABI_VERSION,0,cache_write,&probe,1024};
                XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config,sizeof(config)) == XR_XIR_CALL_READY);
                config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
                XrXirInstance *instance = NULL; CHECK(xr_xir_instance_new(program,&config,&instance) == XR_XIR_CALL_READY);
                XrXirDomain *domain = NULL; XrXirValue argument = {0}, output = {0};
                CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
                CHECK(xr_xir_string_new(domain,cache_oracle_bytes[vector],cache_oracle_lengths[vector],&argument) == XR_XIR_VALUE_OK);
                CHECK(xr_xir_instance_start(instance,ids[vector == 1],&argument,1) == XR_XIR_CALL_READY);
                xr_xir_value_drop(&argument); xr_xir_domain_drop(domain);
                XrXirCallStatus status = xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
                CHECK(probe.calls == 1);
                CHECK(status == (mode < 2 ? XR_XIR_CALL_RETURNED : mode == 2 ? XR_XIR_CALL_OOM : XR_XIR_CALL_LIMIT));
                if (mode < 2) {
                    CHECK(xr_xir_instance_take_result(instance,&output) == XR_XIR_CALL_RETURNED);
                    CHECK(output.type == XR_XIR_BOOL && output.payload == (mode == 0));
                } else CHECK(xr_xir_instance_take_result(instance,&output) == XR_XIR_CALL_BAD_STATE && !output.type && !output.payload && !output.reserved);
                xr_xir_value_drop(&output); CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
            }
}
static void cache_corruption(const XrXirCompileContext *context) {
    const XirNativeCacheRegistry *original = &xir_native_cache_registry;
    CHECK(cache_registry_verify(context,original) == XR_XIR_OK);
    for (unsigned field = 0; field < 15; ++field) {
        XirNativeCacheRegistry bad = *original;
        switch (field) {
        case 0: bad.schema = 2; break;
        case 1: bad.checked.digest[0] ^= 1; break;
        case 2: bad.native_object.digest[0] ^= 1; break;
        case 3: bad.generated_c.digest[0] ^= 1; break;
        case 4: bad.semantic_id[0] ^= 1; break;
        case 5: bad.runtime_layout[0] ^= 1; break;
        case 6: bad.sdk_identity[0] ^= 1; break;
        case 7: bad.variant = "wrong-provider-profile-target"; break;
        case 8: bad.architecture = 2; break;
        case 9: ++bad.value_abi; break;
        case 10: ++bad.call_abi; break;
        case 11: ++bad.program_abi; break;
        case 12: bad.leaf_digests[0][0] ^= 1; break;
        case 13: bad.leaf_indices[0] = bad.leaf_indices[1]; break;
        default: bad.entries = original->entries+1; break;
        }
        CHECK(cache_registry_verify(context,&bad) == XR_XIR_BAD_STRUCTURE);
    }
    for (unsigned component = 0; component < 3; ++component) {
        XirNativeCacheRegistry bad = *original;
        XirNativeCacheComponent *selected = component == 0 ? &bad.checked : component == 1 ? &bad.native_object : &bad.generated_c;
        XrXirStatus status = XR_XIR_OK;
        uint8_t *copy = xir_compile_copy(context,selected->bytes,selected->length,&status);
        CHECK(copy && status == XR_XIR_OK); copy[0] ^= 1; selected->bytes = copy;
        CHECK(cache_registry_verify(context,&bad) == XR_XIR_BAD_STRUCTURE);
        xr_compile_resources_free(copy);
    }
}
static void cache_lookup_failure(XirNativeCache *cache, XrXirArtifact *lowered, uint32_t index) {
    XirNativeCacheHit output; uint8_t before[sizeof(output)];
    memset(&output,0x6a,sizeof(output)); memcpy(before,&output,sizeof(output));
    cache->manifest.leaf_digests[0][0] ^= 1;
    CHECK(xir_native_cache_lookup(cache,lowered,index,&output) == XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(before,&output,sizeof(output)));
    const XrXirCompileContext *context = xr_xir_compile_artifact_context(lowered);
    XrCompileResourceStats baseline = {0}, after = {0};
    CHECK(xr_compile_resources_stats(context->resources,&baseline) == XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *original = lowered; XrXirProgram *program = NULL;
    CHECK(xir_compile_vm_program_take_cached(&lowered,cache,&program) == XR_XIR_BAD_STRUCTURE && lowered == original && !program);
    CHECK(xr_compile_resources_stats(context->resources,&after) == XR_COMPILE_RESOURCE_OK && after.live_bytes == baseline.live_bytes && after.work > baseline.work);
    cache->manifest.leaf_digests[0][0] ^= 1;
}
static void cache_targeted_failures(void) {
    XrCompileResourceLimits caps = {UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps,&context.resources) == XR_COMPILE_RESOURCE_OK);
    XirNativeCache *cache = NULL;
    effects_compile_fail_at = effects_compile_attempts; effects_compile_injected = false;
    CHECK(xir_native_cache_open(&context,&cache) == XR_XIR_OUT_OF_MEMORY && !cache && effects_compile_injected);
    effects_compile_fail_at = SIZE_MAX; xr_compile_resources_release(context.resources); context.resources = NULL;
    CHECK(!effects_compile_live && !effects_compile_bytes);
    CHECK(xr_compile_resources_new(&caps,&context.resources) == XR_COMPILE_RESOURCE_OK);
    CHECK(xir_native_cache_open(&context,&cache) == XR_XIR_OK);
    XrCompileResourceStats baseline = {0}; CHECK(xr_compile_resources_stats(context.resources,&baseline) == XR_COMPILE_RESOURCE_OK);
    XirNativeCache *occupied = cache;
    CHECK(xir_native_cache_open(&context,&occupied) == XR_XIR_BAD_STRUCTURE && occupied == cache);
    XrCompileResourceStats after = {0}; CHECK(xr_compile_resources_stats(context.resources,&after) == XR_COMPILE_RESOURCE_OK);
    CHECK(after.allocated_bytes == baseline.allocated_bytes && after.work == baseline.work);
    cache->references = UINT32_MAX; CHECK(xir_native_cache_retain(cache) == XR_XIR_BUDGET && cache->references == UINT32_MAX);
    cache->references = 1; xir_native_cache_drop(cache); cache = NULL; xr_compile_resources_release(context.resources); context.resources = NULL;
    CHECK(!effects_compile_live && !effects_compile_bytes);
    const uint64_t exacts[] = {baseline.allocated_bytes,baseline.peak_bytes,baseline.work};
    for (unsigned axis = 0; axis < 3; ++axis)
        for (unsigned below = 0; below < 2; ++below) {
            XrCompileResourceLimits limited = caps;
            if (axis == 0) limited.allocated_bytes = exacts[axis]-below;
            else if (axis == 1) limited.live_bytes = exacts[axis]-below;
            else limited.work = exacts[axis]-below;
            CHECK(xr_compile_resources_new(&limited,&context.resources) == XR_COMPILE_RESOURCE_OK);
            XrXirStatus status = xir_native_cache_open(&context,&cache);
            CHECK(status == (below ? XR_XIR_BUDGET : XR_XIR_OK));
            CHECK(below ? cache == NULL : cache != NULL);
            xir_native_cache_drop(cache); cache = NULL; xr_compile_resources_release(context.resources); context.resources = NULL;
            CHECK(!effects_compile_live && !effects_compile_bytes);
        }
    printf("cache constructor first actual OOM; occupied; retain overflow; three exact/minus1; physical0\n");
}
static void cache_escape(XrXirProgram *program, uint32_t writer) {
    CacheProbe probe = {0,0,XR_XIR_OUTPUT_OK};
    XrXirOutputSink sink = {XR_XIR_CALL_ABI_VERSION,0,cache_write,&probe,1024};
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config,sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirInstance *first = NULL, *second = NULL;
    CHECK(xr_xir_instance_new(program,&config,&first) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&second) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(first,writer,NULL,0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(first,UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue function = {0}, alias = {0}, argument = {0}, retained = {0}, output = {0};
    CHECK(xr_xir_instance_take_result(first,&function) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_value_copy(&function,&alias) == XR_XIR_VALUE_OK);
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,cache_oracle_bytes[0],cache_oracle_lengths[0],&argument) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&argument,&retained) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_instance_start_function(second,&alias,&argument,1) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start_function(first,&alias,&argument,1) == XR_XIR_CALL_READY);
    xr_xir_value_drop(&argument); xr_xir_domain_drop(domain);
    CHECK(xr_xir_instance_poll_bounded(first,UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(first,&output) == XR_XIR_CALL_RETURNED && output.type == XR_XIR_BOOL && output.payload == 1);
    CHECK(probe.calls == 1); CHECK(xr_xir_instance_free(first) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start_function(second,&alias,NULL,0) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_free(second) == XR_XIR_CALL_READY);
    CHECK(xr_xir_value_copy(&alias,&argument) == XR_XIR_VALUE_OK);
    const char *text = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&retained,&text,&length) && length == 7 && !memcmp(text,cache_oracle_bytes[0],7));
    xr_xir_value_drop(&argument); xr_xir_value_drop(&retained); xr_xir_value_drop(&output);
    xr_xir_value_drop(&function); xr_xir_value_drop(&alias);
    CHECK(!runtime_live && !runtime_bytes);
}
int main(int argc, char **argv) {
    CHECK(argc == 3);
    cache_targeted_failures();
    XrCompileResourceLimits caps = {UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps,&context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats initial = {0}; CHECK(xr_compile_resources_stats(context.resources,&initial) == XR_COMPILE_RESOURCE_OK);
    cache_corruption(&context);
    for (unsigned mode = 0; mode < 3; ++mode) {
        uint32_t ids[6]; XrXirArtifact *lowered = cache_program(&context,argv[1],argv[2],ids);
        XirNativeCache *cache = NULL;
        if (mode) {
            CHECK(xir_native_cache_open(&context,&cache) == XR_XIR_OK);
            for (unsigned i = 0; i < 6; ++i) {
                XirNativeCacheHit hit = {0}; CHECK(xir_native_cache_lookup(cache,lowered,ids[i],&hit) == XR_XIR_OK);
                CHECK(hit.kind == (i < 2 ? XIR_NATIVE_CACHE_MATCH : XIR_NATIVE_CACHE_MISS));
                if (i < 2) CHECK(hit.entry.resume == xir_native_cache_registry.entries[xir_native_cache_registry.leaf_indices[i]].resume);
            }
            cache_lookup_failure(cache,lowered,ids[0]);
        }
        XrXirProgram *program = NULL;
        CHECK((mode ? xir_compile_vm_program_take_cached(&lowered,cache,&program) :
            xr_xir_compile_vm_program_take(&lowered,&program)) == XR_XIR_OK && !lowered);
        xir_native_cache_drop(cache); /* Only the Program aggregate now owns its cache lease. */
        const uint32_t selected[] = {ids[mode == 2 ? 2 : 0],ids[mode == 2 ? 3 : 1]};
        cache_execute(program,selected);
        if (mode == 1) cache_escape(program,ids[5]); else xr_xir_compile_program_drop(program);
        printf("cache mode=%u twoInstances provider4 vector3 owns producer-death baseline\n",mode);
    }
    XrCompileResourceStats final = {0}; CHECK(xr_compile_resources_stats(context.resources,&final) == XR_COMPILE_RESOURCE_OK);
    CHECK(final.live_bytes == initial.live_bytes); xr_compile_resources_release(context.resources); context.resources = NULL;
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    printf("cache normal allocated=%llu peak=%llu work=%llu ledger baseline\n",(unsigned long long)final.allocated_bytes,
        (unsigned long long)final.peak_bytes,(unsigned long long)final.work);
    return 0;
}
