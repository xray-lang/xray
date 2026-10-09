/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_native_cache_resources.c - Actual static-pair allocation boundaries
 *
 * KEY CONCEPT: Preparation and selection share one ledger; injection is exact.
 */
#define main cache_original_main
#include "test_xir_native_cache.c"
#undef main

typedef struct CacheResourceRun {
    XrXirStatus status;
    unsigned phase;
    size_t constructor_sites, preparation_sites, open_sites, retain_sites, bind_sites, attempts;
    uint64_t after_open_work;
    XrCompileResourceStats preparation, whole;
    bool injected;
} CacheResourceRun;

static XrXirStatus resource_prepare(const XrXirCompileContext *context,
    const char *root, const char *entry, XrXirArtifact **output) {
    XrXirLibraryInput input = {xir_native_cache_registry.checked.bytes,xir_native_cache_registry.checked.length,{0}, (XrXirLibraryModuleInput[]){{{XR_MODULE_IDENTITY_STDLIB,"io",root},"io/output.xr"}},1};
    memcpy(input.sha256,xir_native_cache_registry.checked.digest,32);
    XrXirLibraryCatalog *catalog = NULL;
    XrCompilerSession *session = NULL;
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL, *closed = NULL;
    XrXirStatus status = xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog);
    if (status != XR_XIR_OK) goto finish;
    XrCompilerSessionStatus session_status = xr_compile_session_new(context->resources,&session);
    if (session_status != XR_COMPILER_SESSION_OK) {
        status = session_status == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
        goto finish;
    }
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request = {session,entry,&authority,context,root,NULL,XR_XIR_PROGRAM,catalog};
    status = xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if (status != XR_XIR_OK) goto finish;
    status = xr_xir_compile_checked_write(result.checked,&packet,NULL);
    if (status != XR_XIR_OK) goto finish;
    status = xr_xir_compile_checked_read(context,packet.bytes,packet.length,&decoded,NULL);
    if (status != XR_XIR_OK) goto finish;
    status = xr_xir_compile_specialize(decoded,&closed,NULL);
    if (status != XR_XIR_OK) goto finish;
    status = xr_xir_compile_artifact_verify(closed,NULL);
    if (status != XR_XIR_OK) goto finish;
    XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    status = xr_xir_compile_lower(closed,&target,output,NULL);
finish:
    xr_xir_compile_artifact_free(closed); xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_checked_packet_free(&packet); xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session); xr_xir_compile_library_catalog_free(catalog);
    return status;
}

static CacheResourceRun resource_run(const char *root, const char *entry,
    XrCompileResourceLimits caps, size_t failure) {
    CacheResourceRun run = {0};
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    effects_compile_fail_at = SIZE_MAX; effects_compile_injected = false;
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    size_t constructor_first = effects_compile_attempts;
    CHECK(xr_compile_resources_new(&caps,&context.resources) == XR_COMPILE_RESOURCE_OK);
    run.constructor_sites = effects_compile_attempts-constructor_first;
    CHECK(run.constructor_sites == 1);
    XrCompileResourceStats initial = {0};
    CHECK(xr_compile_resources_stats(context.resources,&initial) == XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *lowered = NULL; XirNativeCache *cache = NULL;
    XrXirProgram *program = NULL;
    size_t preparation_first = effects_compile_attempts;
    run.phase = 0; run.status = resource_prepare(&context,root,entry,&lowered);
    run.preparation_sites = effects_compile_attempts-preparation_first;
    if (run.status != XR_XIR_OK) { CHECK(!lowered); goto finish; }
    CHECK(xr_compile_resources_stats(context.resources,&run.preparation) == XR_COMPILE_RESOURCE_OK);
    size_t first = effects_compile_attempts;
    if (failure != SIZE_MAX) { CHECK(failure <= SIZE_MAX-first); effects_compile_fail_at = first+failure; }
    XrXirArtifact *original = lowered;
    run.phase = 1; run.status = xir_native_cache_open(&context,&cache);
    run.open_sites = effects_compile_attempts-first;
    if (run.status != XR_XIR_OK) { CHECK(!cache && !program && lowered == original); goto measured; }
    XrCompileResourceStats opened = {0};
    CHECK(xr_compile_resources_stats(context.resources,&opened) == XR_COMPILE_RESOURCE_OK);
    run.after_open_work = opened.work;
    size_t retained = effects_compile_attempts;
    run.phase = 2; run.status = xir_native_cache_retain(cache);
    run.retain_sites = effects_compile_attempts-retained;
    if (run.status != XR_XIR_OK) { CHECK(cache->references == 1 && !program && lowered == original); goto measured; }
    CHECK(cache->references == 2);
    /* The first caller dies; only the retained selection lease remains. */
    xir_native_cache_drop(cache); CHECK(cache->references == 1);
    size_t binding = effects_compile_attempts;
    run.phase = 3; run.status = xir_compile_vm_program_take_cached(&lowered,cache,&program);
    run.bind_sites = effects_compile_attempts-binding;
    if (run.status != XR_XIR_OK) CHECK(!program && lowered == original && cache->references == 1);
    else CHECK(program && !lowered && cache->references == 2);
measured:
    run.attempts = effects_compile_attempts-first;
    run.injected = effects_compile_injected;
    if (failure != SIZE_MAX) {
        CHECK(run.injected && effects_compile_attempts > first+failure);
        CHECK(run.status == XR_XIR_OUT_OF_MEMORY && !program && lowered == original);
    }
finish:
    effects_compile_fail_at = SIZE_MAX;
    size_t release_attempts = effects_compile_attempts;
    xr_xir_compile_program_drop(program); xir_native_cache_drop(cache);
    xr_xir_compile_artifact_free(lowered);
    CHECK(effects_compile_attempts == release_attempts);
    CHECK(xr_compile_resources_stats(context.resources,&run.whole) == XR_COMPILE_RESOURCE_OK);
    CHECK(run.whole.live_bytes == initial.live_bytes);
    CHECK(run.whole.work >= run.preparation.work && run.whole.allocated_bytes >= run.preparation.allocated_bytes);
    xr_compile_resources_release(context.resources);
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    return run;
}

int main(int argc, char **argv) {
    CHECK(argc == 4);
    const XrCompileResourceLimits caps = {UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    bool full = !strcmp(argv[3],"faults"); CHECK(full || !strcmp(argv[3],"measure"));
    if (!full) CHECK(cache_original_main(3,argv) == 0);
    CacheResourceRun normal = resource_run(argv[1],argv[2],caps,SIZE_MAX);
    CHECK(normal.status == XR_XIR_OK && normal.phase == 3 && !normal.injected);
    CHECK(normal.attempts == normal.open_sites+normal.retain_sites+normal.bind_sites && normal.attempts);
    CHECK(normal.retain_sites == 0);
    printf("cache resource baseline open=%zu retain=%zu bindseal=%zu actual=%zu preparation allocated=%llu peak=%llu work=%llu whole allocated=%llu peak=%llu work=%llu physical0\n",
        normal.open_sites,normal.retain_sites,normal.bind_sites,normal.attempts,
        (unsigned long long)normal.preparation.allocated_bytes,(unsigned long long)normal.preparation.peak_bytes,(unsigned long long)normal.preparation.work,
        (unsigned long long)normal.whole.allocated_bytes,(unsigned long long)normal.whole.peak_bytes,(unsigned long long)normal.whole.work);
    printf("cache resource owner-constructor=%zu preparation-sites=%zu after-open-work=%llu excluded-from-selection-denominator\n",normal.constructor_sites,normal.preparation_sites,(unsigned long long)normal.after_open_work);
    if (full) {
        for (size_t failure = 0; failure < normal.attempts; ++failure) {
            CacheResourceRun fault = resource_run(argv[1],argv[2],caps,failure);
            printf("cache resource ordinal=%zu phase=%u hit=%u status=%u attempts=%zu physical0\n",failure,fault.phase,fault.injected,fault.status,fault.attempts);
        }
    } else {
        const size_t failures[] = {0,normal.attempts-1};
        for (unsigned i = 0; i < 2; ++i) {
            CacheResourceRun fault = resource_run(argv[1],argv[2],caps,failures[i]);
            printf("cache resource control ordinal=%zu phase=%u hit=%u status=%u attempts=%zu physical0\n",failures[i],fault.phase,fault.injected,fault.status,fault.attempts);
        }
        for (unsigned admitted = 0; admitted < 2; ++admitted) {
            XrCompileResourceLimits limited = caps;
            limited.work = normal.after_open_work+admitted;
            CacheResourceRun cut = resource_run(argv[1],argv[2],limited,SIZE_MAX);
            CHECK(cut.status == XR_XIR_BUDGET && cut.phase == (admitted ? 3u : 2u));
            printf("cache resource retain-work=%u status=%u phase=%u references-preserved physical0\n",admitted,cut.status,cut.phase);
        }
        const uint64_t exact[] = {normal.whole.allocated_bytes,normal.whole.peak_bytes,normal.whole.work};
        for (unsigned axis = 0; axis < 3; ++axis)
            for (unsigned below = 0; below < 2; ++below) {
                XrCompileResourceLimits limited = caps;
                if (!axis) limited.allocated_bytes = exact[axis]-below;
                else if (axis == 1) limited.live_bytes = exact[axis]-below;
                else limited.work = exact[axis]-below;
                CacheResourceRun cut = resource_run(argv[1],argv[2],limited,SIZE_MAX);
                CHECK(cut.status == (below ? XR_XIR_BUDGET : XR_XIR_OK));
                printf("cache resource axis=%u minus=%u status=%u phase=%u physical0\n",axis,below,cut.status,cut.phase);
            }
    }
    printf("cache resource completed mode=%s actual-denominator=%zu; cancellation not applicable to synchronous constructor/bind/seal; retain work included\n",argv[3],normal.attempts);
    return 0;
}
