/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_compress_suite_native.c - Execute the emitted compression catalog
 *
 * KEY CONCEPT:
 *   Program, discovered IDs and source digest come from the same emitted C.
 *   Sealing and test-role admission execute every original body independently.
 */
#include "base/xresource_limits.h"
#include "compress_suite_runtime.h"

extern const XrXirProgramSpec compress_suite_program;
extern const uint32_t compress_suite_functions[COMPRESS_SUITE_COUNT];
extern const uint32_t compress_suite_count;
extern const char compress_suite_source_sha256[65];
extern const char compress_suite_emit_sha256[65];

int main(int argc, char **argv) {
    COMPRESS_CHECK(argc == 2);
    COMPRESS_CHECK(compress_suite_count == COMPRESS_SUITE_COUNT);
    XrCompileResourceLimits limits = XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrXirCompileContext context = {NULL, xr_xir_compile_default_limits()};
    COMPRESS_CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program = NULL;
    COMPRESS_CHECK(xr_xir_compile_program_seal(&context, &compress_suite_program, &program) == XR_XIR_OK);
    const XrXirDeclarations *declarations = compress_suite_program.declarations;
    uint32_t discovered = 0;
    for (uint32_t f = 0; f < compress_suite_program.entry_count; ++f) {
        const XrXirFunctionIdentity *identity = declarations->functions + f;
        if (identity->module != declarations->root_module || !identity->test_role) continue;
        COMPRESS_CHECK(identity->test_role == XR_XIR_TEST_ROLE_TEST && identity->test_timeout_seconds == 0);
        ++discovered;
    }
    COMPRESS_CHECK(discovered == COMPRESS_SUITE_COUNT);
    XrXirCallStatus statuses[COMPRESS_SUITE_COUNT] = {0};
    uint32_t passed = 0;
    for (uint32_t i = 0; i < COMPRESS_SUITE_COUNT; ++i) {
        uint32_t f = compress_suite_functions[i];
        COMPRESS_CHECK(f < compress_suite_program.entry_count);
        for (uint32_t j = 0; j < i; ++j) COMPRESS_CHECK(compress_suite_functions[j] != f);
        const XrXirFunctionIdentity *identity = declarations->functions + f;
        COMPRESS_CHECK(identity->module == declarations->root_module);
        COMPRESS_CHECK(identity->test_role == XR_XIR_TEST_ROLE_TEST && identity->test_timeout_seconds == 0);
        COMPRESS_CHECK(compress_suite_program.entries[f].parameter_count == 0);
        COMPRESS_CHECK(compress_suite_program.entries[f].result == XR_XIR_UNIT);
        statuses[i] = compress_suite_run(program, f, "native", compress_suite_names[i]);
        if (statuses[i] == XR_XIR_CALL_RETURNED) ++passed;
    }
    xr_xir_compile_program_drop(program);
    xr_compile_resources_release(context.resources);
    CompressSuiteDigests digests = {compress_suite_source_sha256, compress_suite_emit_sha256};
    compress_suite_report(argv[1], "native", &digests,
        compress_suite_functions, statuses, true);
    printf("compress catalog: %u PASS, 0 SKIP, %u FAIL\n", passed, COMPRESS_SUITE_COUNT - passed);
    COMPRESS_CHECK(passed == COMPRESS_SUITE_COUNT);
    return 0;
}
