/* Real standalone stdlib selector uses the same counted production TU. */
static XrSourceCompileStatus run_stdlib(const XrCompileResourceLimits *limits,
    size_t failure, XrCompileResourceStats *stats, const char *stdlib, unsigned origin, const char *report) {
    reset(failure); XrCompileResources *r = NULL;
    XrCompileResourceStatus opened = xr_compile_resources_new(limits, &r);
    if (opened != XR_COMPILE_RESOURCE_OK) {
        CHECK(!r && !live);
        CHECK(opened == XR_COMPILE_RESOURCE_MEMORY_LIMIT || opened == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
        return opened == XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_SOURCE_COMPILE_MEMORY_LIMIT : XR_SOURCE_COMPILE_OUT_OF_MEMORY;
    }
    XrStdlibPath paths = {0}; XrSourcePathsDiagnostic diagnostic = {0};
    XrSourceCompileStatus status = xr_source_stdlib_path(r, &paths, &diagnostic);
    CHECK(status == diagnostic.status);
    CHECK(xr_compile_resources_stats(r, stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats->allocated_bytes == total && stats->held_bytes == live && stats->peak_bytes == peak);
    xr_compile_resources_release(r);
    if (status == XR_SOURCE_COMPILE_OK) {
        expected_path(paths.path, stdlib); CHECK((unsigned)paths.origin == origin);
    } else CHECK(!paths.path && !paths.origin);
    candidates_check(status, &diagnostic, report);
    xr_source_stdlib_path_free(&paths); CHECK(!live);
    return status;
}
static void stdlib_matrix(const char *stdlib, unsigned origin) {
    XrCompileResourceStats baseline = {0}, stats = {0};
    CHECK(run_stdlib(&public_limits, SIZE_MAX, &baseline, stdlib, origin, NULL) == XR_SOURCE_COMPILE_OK);
    size_t points = attempts;
    for (size_t i = 0; i < points; ++i)
        CHECK(run_stdlib(&public_limits, i, &stats, stdlib, origin, NULL) == XR_SOURCE_COMPILE_OUT_OF_MEMORY);
    XrCompileResourceLimits limits = public_limits;
    limits.memory_mode = XR_COMPILE_MEMORY_FINITE; limits.memory_limit = baseline.peak_bytes;
    CHECK(run_stdlib(&limits, SIZE_MAX, &stats, stdlib, origin, NULL) == XR_SOURCE_COMPILE_OK);
    CHECK(limits.memory_limit > 0); --limits.memory_limit;
    CHECK(run_stdlib(&limits, SIZE_MAX, &stats, stdlib, origin, NULL) == XR_SOURCE_COMPILE_MEMORY_LIMIT);
    CHECK(run_stdlib(&public_limits, SIZE_MAX, &stats, stdlib, origin, NULL) == XR_SOURCE_COMPILE_OK);
    printf("stdlib selector ordinary profile; %zu malloc points; total=%llu peak=%llu finite exact/minus1/recovery physical zero PASS\n",
        points, (unsigned long long)baseline.allocated_bytes, (unsigned long long)baseline.peak_bytes);
}
