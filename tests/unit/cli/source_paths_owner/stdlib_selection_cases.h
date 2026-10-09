/* Real standalone stdlib selector uses the same counted production TU. */
static XrCliCompileSourceStatus run_stdlib(const XrCompileResourceLimits *limits,
    size_t failure, XrCompileResourceStats *stats, const char *stdlib, unsigned origin) {
    reset(failure); XrCompileResources *r = NULL;
    XrCompileResourceStatus opened = xr_compile_resources_new(limits, &r);
    if (opened != XR_COMPILE_RESOURCE_OK) {
        CHECK(!r && !live); return opened == XR_COMPILE_RESOURCE_BUDGET ? XR_CLI_COMPILE_SOURCE_BUDGET : XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY;
    }
    XrCliStdlibPath paths = {0}; XrCliSourcePathsDiagnostic diagnostic = {0};
    XrCliCompileSourceStatus status = xr_cli_compile_stdlib_path(r, &paths, &diagnostic);
    CHECK(status == diagnostic.status);
    CHECK(xr_compile_resources_stats(r, stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats->allocated_bytes == total && stats->live_bytes == live && stats->peak_bytes == peak);
    xr_compile_resources_release(r);
    if (status == XR_CLI_COMPILE_SOURCE_OK) {
        expected_path(paths.path, stdlib); CHECK((unsigned)paths.origin == origin);
    } else CHECK(!paths.path && !paths.origin);
    xr_cli_compile_stdlib_path_free(&paths); CHECK(!live);
    return status;
}
static void stdlib_matrix(const char *stdlib, unsigned origin) {
    XrCompileResourceStats baseline = {0}, stats = {0};
    record_edges = true; edge_count = 0;
    CHECK(run_stdlib(&public_limits, SIZE_MAX, &baseline, stdlib, origin) == XR_CLI_COMPILE_SOURCE_OK);
    record_edges = false;
    size_t points = attempts, cuts = edge_count;
    for (size_t i = 0; i < points; ++i)
        CHECK(run_stdlib(&public_limits, i, &stats, stdlib, origin) == XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY);
    for (size_t i = 0; i < cuts; ++i) {
        XrCompileResourceLimits limits = public_limits; limits.work = edges[i] - 1;
        CHECK(run_stdlib(&limits, SIZE_MAX, &stats, stdlib, origin) == XR_CLI_COMPILE_SOURCE_BUDGET);
    }
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = public_limits;
        if (axis == 0) limits.allocated_bytes = baseline.allocated_bytes - minus;
        if (axis == 1) limits.live_bytes = baseline.peak_bytes - minus;
        if (axis == 2) limits.work = baseline.work - minus;
        CHECK(run_stdlib(&limits, SIZE_MAX, &stats, stdlib, origin) ==
            (minus ? XR_CLI_COMPILE_SOURCE_BUDGET : XR_CLI_COMPILE_SOURCE_OK));
    }
    printf("stdlib selector public defaults; %zu malloc %zu work cuts; total=%llu peak=%llu work=%llu exact/minus1 physical zero PASS\n",
        points, cuts, (unsigned long long)baseline.allocated_bytes, (unsigned long long)baseline.peak_bytes, (unsigned long long)baseline.work);
}
