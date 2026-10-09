/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_native_cache_preparation.c - Fallible pair preparation and ledger creation
 */
#define main cache_preparation_original_main
#include "test_xir_native_cache.c"
#undef main

typedef struct CachePreparationRun {
    XrXirStatus status;
    unsigned stage;
    size_t sites, attempts;
    bool injected;
    XrCompileResourceStats preparation, whole;
    size_t stage_offsets[10];
} CachePreparationRun;

static size_t preparation_stage_offsets[10];

static XrXirStatus preparation_pipeline(const XrXirCompileContext *context,
    const char *root, const char *entry, XrXirArtifact **output, unsigned *stage) {
    size_t first_attempt = effects_compile_attempts;
    memset(preparation_stage_offsets,0,sizeof(preparation_stage_offsets));
    XrXirLibraryInput input = {xir_native_cache_registry.checked.bytes,xir_native_cache_registry.checked.length,{0}, (XrXirLibraryModuleInput[]){{{XR_MODULE_IDENTITY_STDLIB,"io",root},"io/output.xr"}},1};
    memcpy(input.sha256,xir_native_cache_registry.checked.digest,32);
    XrXirLibraryCatalog *catalog = NULL;
    XrCompilerSession *session = NULL;
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL, *closed = NULL;
    preparation_stage_offsets[1] = effects_compile_attempts-first_attempt;
    *stage = 1;
    XrXirStatus status = xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog);
    if (status != XR_XIR_OK) goto finish;
    preparation_stage_offsets[2] = effects_compile_attempts-first_attempt;
    *stage = 2;
    XrCompilerSessionStatus session_status = xr_compile_session_new(context->resources,&session);
    if (session_status != XR_COMPILER_SESSION_OK) {
        status = session_status == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
        goto finish;
    }
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request = {session,entry,&authority,context,root,NULL,XR_XIR_PROGRAM,catalog};
    preparation_stage_offsets[3] = effects_compile_attempts-first_attempt;
    *stage = 3;
    status = xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if (status != XR_XIR_OK) goto finish;
    preparation_stage_offsets[4] = effects_compile_attempts-first_attempt;
    *stage = 4;
    status = xr_xir_compile_checked_write(result.checked,&packet,NULL);
    if (status != XR_XIR_OK) goto finish;
    preparation_stage_offsets[5] = effects_compile_attempts-first_attempt;
    *stage = 5;
    status = xr_xir_compile_checked_read(context,packet.bytes,packet.length,&decoded,NULL);
    if (status != XR_XIR_OK) goto finish;
    preparation_stage_offsets[6] = effects_compile_attempts-first_attempt;
    *stage = 6;
    status = xr_xir_compile_specialize(decoded,&closed,NULL);
    if (status != XR_XIR_OK) goto finish;
    preparation_stage_offsets[7] = effects_compile_attempts-first_attempt;
    *stage = 7;
    status = xr_xir_compile_artifact_verify(closed,NULL);
    if (status != XR_XIR_OK) goto finish;
    XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    preparation_stage_offsets[8] = effects_compile_attempts-first_attempt;
    *stage = 8;
    status = xr_xir_compile_lower(closed,&target,output,NULL);
finish:;
    size_t cleanup_first = effects_compile_attempts;
    preparation_stage_offsets[9] = cleanup_first-first_attempt;
    xr_xir_compile_artifact_free(closed); xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_checked_packet_free(&packet); xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session); xr_xir_compile_library_catalog_free(catalog);
    CHECK(effects_compile_attempts == cleanup_first);
    return status;
}


static CachePreparationRun preparation_run(const char *root, const char *entry,
    XrCompileResourceLimits caps, size_t failure) {
    CachePreparationRun run = {0}; XrXirCompileContext context = {0};
    context.limits = xr_xir_compile_default_limits();
    effects_compile_fail_at = SIZE_MAX; effects_compile_injected = false;
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    CHECK(xr_compile_resources_new(&caps,&context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats initial = {0};
    CHECK(xr_compile_resources_stats(context.resources,&initial) == XR_COMPILE_RESOURCE_OK);
    CHECK(initial.allocated_bytes == 80 && initial.live_bytes == 80 && initial.work == 1);
    XrXirArtifact *lowered = NULL; XirNativeCache *cache = NULL; XrXirProgram *program = NULL;
    size_t first = effects_compile_attempts;
    if (failure != SIZE_MAX) { CHECK(failure <= SIZE_MAX-first); effects_compile_fail_at = first+failure; }
    run.status = preparation_pipeline(&context,root,entry,&lowered,&run.stage);
    run.sites = effects_compile_attempts-first;
    memcpy(run.stage_offsets,preparation_stage_offsets,sizeof(run.stage_offsets));
    run.injected = effects_compile_injected;
    CHECK(xr_compile_resources_stats(context.resources,&run.preparation) == XR_COMPILE_RESOURCE_OK);
    if (run.status != XR_XIR_OK) {
        CHECK(!lowered && !cache && !program);
        if (failure != SIZE_MAX) CHECK(run.injected && run.sites > failure && run.status == XR_XIR_OUT_OF_MEMORY);
        goto finish;
    }
    CHECK(!run.injected && lowered);
    run.stage = 9; run.status = xir_native_cache_open(&context,&cache);
    if (run.status != XR_XIR_OK) { CHECK(!cache && !program); goto finish; }
    run.stage = 10; run.status = xir_native_cache_retain(cache);
    if (run.status != XR_XIR_OK) { CHECK(cache->references == 1 && !program); goto finish; }
    CHECK(cache->references == 2); xir_native_cache_drop(cache); CHECK(cache->references == 1);
    XrXirArtifact *original = lowered;
    run.stage = 11; run.status = xir_compile_vm_program_take_cached(&lowered,cache,&program);
    if (run.status != XR_XIR_OK) CHECK(lowered == original && !program && cache->references == 1);
    else CHECK(!lowered && program && cache->references == 2);
finish:
    run.attempts = effects_compile_attempts-first;
    effects_compile_fail_at = SIZE_MAX;
    size_t before_drop = effects_compile_attempts;
    xr_xir_compile_program_drop(program); xir_native_cache_drop(cache); xr_xir_compile_artifact_free(lowered);
    CHECK(effects_compile_attempts == before_drop);
    CHECK(xr_compile_resources_stats(context.resources,&run.whole) == XR_COMPILE_RESOURCE_OK);
    CHECK(run.whole.live_bytes == initial.live_bytes);
    CHECK(run.whole.work >= run.preparation.work && run.whole.allocated_bytes >= run.preparation.allocated_bytes);
    xr_compile_resources_release(context.resources);
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    return run;
}

static void preparation_constructor_controls(XrCompileResourceLimits caps) {
    effects_compile_fail_at = effects_compile_attempts; effects_compile_injected = false;
    size_t first = effects_compile_attempts; XrCompileResources *resources = NULL;
    XrCompileResourceStatus status = xr_compile_resources_new(&caps,&resources);
    CHECK(status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !resources);
    CHECK(effects_compile_injected && effects_compile_attempts == first+1);
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    effects_compile_fail_at = SIZE_MAX;
    printf("cache preparation constructor ordinal=0 hit=1 status=%u attempts=1 source-not-started physical0\n",status);
    for (unsigned axis = 0; axis < 3; ++axis)
        for (unsigned below = 0; below < 2; ++below) {
            XrCompileResourceLimits limited = caps;
            if (!axis) limited.allocated_bytes = 80-below;
            else if (axis == 1) limited.live_bytes = 80-below;
            else limited.work = 1-below;
            first = effects_compile_attempts; resources = NULL;
            status = xr_compile_resources_new(&limited,&resources);
            CHECK(status == (below ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OK));
            CHECK(effects_compile_attempts-first == (below ? 0u : 1u));
            if (!below) {
                XrCompileResourceStats stats = {0};
                CHECK(xr_compile_resources_stats(resources,&stats) == XR_COMPILE_RESOURCE_OK);
                CHECK(stats.allocated_bytes == 80 && stats.live_bytes == 80 && stats.peak_bytes == 80 && stats.work == 1);
            }
            xr_compile_resources_release(resources);
            CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
            printf("cache preparation constructor-axis=%u minus=%u status=%u source-not-started physical0\n",axis,below,status);
        }
}

int main(int argc, char **argv) {
    CHECK(argc == 4); bool full = !strcmp(argv[3],"faults"); CHECK(full || !strcmp(argv[3],"measure"));
    const XrCompileResourceLimits caps = {UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    preparation_constructor_controls(caps);
    CachePreparationRun normal = preparation_run(argv[1],argv[2],caps,SIZE_MAX);
    CHECK(normal.status == XR_XIR_OK && normal.stage == 11 && normal.sites && !normal.injected);
    printf("cache preparation baseline actual=%zu operation=%zu preparation allocated=%llu peak=%llu work=%llu whole allocated=%llu peak=%llu work=%llu physical0\n",
        normal.sites,normal.attempts-normal.sites,
        (unsigned long long)normal.preparation.allocated_bytes,(unsigned long long)normal.preparation.peak_bytes,(unsigned long long)normal.preparation.work,
        (unsigned long long)normal.whole.allocated_bytes,(unsigned long long)normal.whole.peak_bytes,(unsigned long long)normal.whole.work);
    for (unsigned stage = 1; stage <= 8; ++stage)
        printf("cache preparation stage=%u begin=%zu end=%zu actual=%zu\n",stage,normal.stage_offsets[stage],normal.stage_offsets[stage+1],normal.stage_offsets[stage+1]-normal.stage_offsets[stage]);
    size_t count = full ? normal.sites : 2;
    for (size_t i = 0; i < count; ++i) {
        size_t failure = full ? i : (i ? normal.sites-1 : 0);
        CachePreparationRun fault = preparation_run(argv[1],argv[2],caps,failure);
        CHECK(fault.status == XR_XIR_OUT_OF_MEMORY && fault.injected);
        printf("cache preparation ordinal=%zu stage=%u hit=%u status=%u attempts=%zu empty-preserved drop-no-malloc physical0\n",failure,fault.stage,fault.injected,fault.status,fault.attempts);
    }
    if (!full) {
        const uint64_t exact[] = {normal.whole.allocated_bytes,normal.whole.peak_bytes,normal.whole.work};
        for (unsigned axis = 0; axis < 3; ++axis)
            for (unsigned below = 0; below < 2; ++below) {
                XrCompileResourceLimits limited = caps;
                if (!axis) limited.allocated_bytes = exact[axis]-below;
                else if (axis == 1) limited.live_bytes = exact[axis]-below;
                else limited.work = exact[axis]-below;
                CachePreparationRun cut = preparation_run(argv[1],argv[2],limited,SIZE_MAX);
                CHECK(cut.status == (below ? XR_XIR_BUDGET : XR_XIR_OK));
                printf("cache preparation whole-axis=%u minus=%u status=%u stage=%u physical0\n",axis,below,cut.status,cut.stage);
            }
    }
    printf("cache preparation completed mode=%s preparation=%zu constructor=1 separately-measured selection=%zu outside-injection; synchronous no-cancel\n",argv[3],normal.sites,normal.attempts-normal.sites);
    return 0;
}
