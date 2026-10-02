/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_native_projection_faults.h - Publication and physical failure probes
 */
#ifndef XIR_NATIVE_PROJECTION_FAULTS_H
#define XIR_NATIVE_PROJECTION_FAULTS_H
static void product_projection_guards(const XrXirSourceProduct *product) {
    XrToolchainBinding binding = projection_fixture_binding(product);
    XrXirNativeProjectionRequest request = {product, &binding, "original_source", 16777216};
    XrXirNativeProjection output, saved;
    memset(&output, 0x5a, sizeof(output)); output.source = (XrXirCSource){0};
    output.input.schema_version = 0; memset(output.input.id.bytes, 0, 32); saved = output;
    const char *invalid[] = {NULL, "", "1name", "bad-name", "bad name",
        "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklm"};
    size_t live = runtime_live, bytes = runtime_bytes;
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        request.prefix = invalid[i];
        CHECK(xr_compile_native_projection_build(&request, &output, NULL) == XR_XIR_BAD_STRUCTURE);
        CHECK(!memcmp(&output, &saved, sizeof(saved)));
    }
    request.prefix = "original_source"; binding.schema_version = 1;
    CHECK(xr_compile_native_projection_build(&request, &output, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(&output, &saved, sizeof(saved)));
    CHECK(runtime_live == live && runtime_bytes == bytes);
}
static void product_projection_oom(const XrXirSourceProduct *product) {
    XrToolchainBinding binding = projection_fixture_binding(product);
    XrXirNativeProjectionRequest request = {product, &binding, "original_source", 16777216};
    size_t live = runtime_live, bytes = runtime_bytes, sites = 0;
    for (size_t point = 0; point <= sites; ++point) {
        XrXirNativeProjection output, saved;
        memset(&output, 0x5a, sizeof(output)); output.source = (XrXirCSource){0};
        output.input.schema_version = 0; memset(output.input.id.bytes, 0, 32); saved = output;
        runtime_attempts = 0; runtime_fail_at = point ? point - 1 : SIZE_MAX;
        XrXirStatus status = xr_compile_native_projection_build(&request, &output, NULL);
        if (!point) {
            CHECK(status == XR_XIR_OK); sites = runtime_attempts;
            projection_fixture_check(product, &output, false);
            xr_compile_native_projection_free(&output);
        } else {
            if (status != XR_XIR_OUT_OF_MEMORY) fprintf(stderr, "projection OOM %zu: %u\n", point - 1, status);
            CHECK(status == XR_XIR_OUT_OF_MEMORY && runtime_attempts > runtime_fail_at);
            CHECK(!memcmp(&output, &saved, sizeof(saved)));
        }
        runtime_fail_at = SIZE_MAX; CHECK(runtime_live == live && runtime_bytes == bytes);
    }
    printf("ordinary source native projection allocations=%zu; output preservation and physical baseline PASS\n", sites);
}
#endif // XIR_NATIVE_PROJECTION_FAULTS_H
