/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_native_stdlib_resources.c - Actual Checked VM binding allocation boundaries
 *
 * KEY CONCEPT: Preparation and binding share one ledger; injection is exact.
 */
#define main stdlib_original_main
#include "test_xir_stdlib_vm.c"
#undef main

typedef struct StdlibResourceRun {
    XrXirStatus status;
    unsigned phase;
    size_t constructor_sites, preparation_sites, bind_sites, attempts;
    XrCompileResourceStats preparation, whole;
    bool injected;
} StdlibResourceRun;

static XrXirStatus resource_prepare(const XrXirCompileContext *context,
    const char *root, const char *entry, XrXirArtifact **output) {
    XrXirLibraryCatalog *catalog = NULL;
    XrCompilerSession *session = NULL;
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL, *closed = NULL;
    XrXirStatus status = xir_stdlib_library_catalog_new(context,root,&catalog);
    if (status != XR_XIR_OK) goto finish;
    XrCompilerSessionStatus session_status = xr_compile_session_new(context->resources,&session);
    if (session_status != XR_COMPILER_SESSION_OK) {
        status = session_status == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
        goto finish;
    }
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request = {session,entry,&authority,context,root,NULL,XR_XIR_PROGRAM,catalog,NULL};
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

static StdlibResourceRun resource_run(const char *root, const char *entry,
    XrCompileResourceLimits caps, size_t failure) {
    StdlibResourceRun run = {0};
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    effects_compile_fail_at = SIZE_MAX; effects_compile_injected = false;
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    size_t constructor_first = effects_compile_attempts;
    CHECK(xr_compile_resources_new(&caps,&context.resources) == XR_COMPILE_RESOURCE_OK);
    run.constructor_sites = effects_compile_attempts-constructor_first;
    CHECK(run.constructor_sites == 1);
    XrCompileResourceStats initial = {0};
    CHECK(xr_compile_resources_stats(context.resources,&initial) == XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *lowered = NULL;
    XrXirProgram *program = NULL;
    size_t preparation_first = effects_compile_attempts;
    run.phase = 0; run.status = resource_prepare(&context,root,entry,&lowered);
    run.preparation_sites = effects_compile_attempts-preparation_first;
    if (run.status != XR_XIR_OK) { CHECK(!lowered); goto finish; }
    CHECK(xr_compile_resources_stats(context.resources,&run.preparation) == XR_COMPILE_RESOURCE_OK);
    size_t first = effects_compile_attempts;
    if (failure != SIZE_MAX) { CHECK(failure <= SIZE_MAX-first); effects_compile_fail_at = first+failure; }
    XrXirArtifact *original = lowered;
    run.phase = 1; run.status = xr_xir_compile_vm_program_take(&lowered,&program);
    run.bind_sites = effects_compile_attempts-first;
    if (run.status != XR_XIR_OK) CHECK(!program && lowered == original);
    else CHECK(program && !lowered);

    run.attempts = effects_compile_attempts-first;
    run.injected = effects_compile_injected;
    if (failure != SIZE_MAX) {
        CHECK(run.injected && effects_compile_attempts > first+failure);
        CHECK(run.status == XR_XIR_OUT_OF_MEMORY && !program && lowered == original);
    }
finish:
    effects_compile_fail_at = SIZE_MAX;
    size_t release_attempts = effects_compile_attempts;
    xr_xir_compile_program_drop(program);
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
    bool full = !strcmp(argv[3],"faults");
    bool binding_only = !strcmp(argv[3],"binding-measure");
    CHECK(full || binding_only || !strcmp(argv[3],"measure"));
    if (!full && !binding_only) CHECK(stdlib_original_main(3,argv) == 0);
    StdlibResourceRun normal = resource_run(argv[1],argv[2],caps,SIZE_MAX);
    CHECK(normal.status == XR_XIR_OK && normal.phase == 1 && !normal.injected);
    CHECK(normal.attempts == normal.bind_sites && normal.attempts);
    printf("stdlib resource baseline bindseal=%zu actual=%zu preparation allocated=%llu peak=%llu work=%llu whole allocated=%llu peak=%llu work=%llu physical0\n",
        normal.bind_sites,normal.attempts,
        (unsigned long long)normal.preparation.allocated_bytes,(unsigned long long)normal.preparation.peak_bytes,(unsigned long long)normal.preparation.work,
        (unsigned long long)normal.whole.allocated_bytes,(unsigned long long)normal.whole.peak_bytes,(unsigned long long)normal.whole.work);
    printf("stdlib resource owner-constructor=%zu preparation-sites=%zu excluded-from-selection-denominator\n",normal.constructor_sites,normal.preparation_sites);
    if (full) {
        for (size_t failure = 0; failure < normal.attempts; ++failure) {
            StdlibResourceRun fault = resource_run(argv[1],argv[2],caps,failure);
            printf("stdlib resource ordinal=%zu phase=%u hit=%u status=%u attempts=%zu physical0\n",failure,fault.phase,fault.injected,fault.status,fault.attempts);
        }
    } else {
        const size_t failures[] = {0,normal.attempts-1};
        for (unsigned i = 0; i < 2; ++i) {
            StdlibResourceRun fault = resource_run(argv[1],argv[2],caps,failures[i]);
            printf("stdlib resource control ordinal=%zu phase=%u hit=%u status=%u attempts=%zu physical0\n",failures[i],fault.phase,fault.injected,fault.status,fault.attempts);
        }
        const uint64_t exact[] = {normal.whole.allocated_bytes,normal.whole.peak_bytes,normal.whole.work};
        for (unsigned axis = 0; axis < 3; ++axis)
            for (unsigned below = 0; below < 2; ++below) {
                XrCompileResourceLimits limited = caps;
                if (!axis) limited.allocated_bytes = exact[axis]-below;
                else if (axis == 1) limited.live_bytes = exact[axis]-below;
                else limited.work = exact[axis]-below;
                StdlibResourceRun cut = resource_run(argv[1],argv[2],limited,SIZE_MAX);
                CHECK(cut.status == (below ? XR_XIR_BUDGET : XR_XIR_OK));
                printf("stdlib resource axis=%u minus=%u status=%u phase=%u physical0\n",axis,below,cut.status,cut.phase);
            }
    }
    printf("stdlib resource completed mode=%s actual-denominator=%zu; cancellation not applicable to synchronous constructor/bind/seal\n",argv[3],normal.attempts);
    return 0;
}
