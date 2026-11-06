/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_compress_suite.c - Own, discover, emit and execute all compression tests
 *
 * KEY CONCEPT:
 *   One SourceProduct supplies the exact test IDs, VM program and generated C.
 *   Session and Source owners retire before any original test body executes.
 */
#include "xir/xr_xir_source_product.h"
#include "base/xresource_limits.h"
#include "toolchain/xcompiler_session.h"
#include "base/xsha256.h"
#include "compress_suite_runtime.h"

static void compress_digest_text(const uint8_t digest[32], char output[65]) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        output[i * 2] = hex[digest[i] >> 4];
        output[i * 2 + 1] = hex[digest[i] & 15];
    }
    output[64] = 0;
}

static void compress_source_digest(const char *path, char output[65]) {
    FILE *file = fopen(path, "rb");
    COMPRESS_CHECK(file);
    XrSHA256Context sha;
    xr_sha256_init(&sha);
    uint8_t buffer[4096], digest[32];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file))) xr_sha256_update(&sha, buffer, count);
    COMPRESS_CHECK(!ferror(file));
    COMPRESS_CHECK(fclose(file) == 0);
    xr_sha256_final(&sha, digest);
    compress_digest_text(digest, output);
}

static void compress_emit_bytes(FILE *file, const char *data, size_t length, bool verify) {
    if (!verify) {
        COMPRESS_CHECK(fwrite(data, 1, length, file) == length);
        return;
    }
    char buffer[4096];
    size_t offset = 0;
    while (offset < length) {
        size_t count = length - offset;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        COMPRESS_CHECK(fread(buffer, 1, count, file) == count);
        COMPRESS_CHECK(!memcmp(buffer, data + offset, count));
        offset += count;
    }
}

static void compress_emit_file(const char *path, const XrXirCSource *code,
    const uint32_t functions[COMPRESS_SUITE_COUNT], const CompressSuiteDigests *digests, bool verify) {
    char footer[1024];
    int length = snprintf(footer, sizeof(footer), "\nconst uint32_t compress_suite_functions[%u]={",
        COMPRESS_SUITE_COUNT);
    COMPRESS_CHECK(length > 0 && (size_t)length < sizeof(footer));
    size_t used = (size_t)length;
    for (uint32_t i = 0; i < COMPRESS_SUITE_COUNT; ++i) {
        length = snprintf(footer + used, sizeof(footer) - used, "%s%uu", i ? "," : "", functions[i]);
        COMPRESS_CHECK(length > 0 && (size_t)length < sizeof(footer) - used);
        used += (size_t)length;
    }
    length = snprintf(footer + used, sizeof(footer) - used,
        "};\nconst uint32_t compress_suite_count=%uu;\n"
        "const char compress_suite_source_sha256[65]=\"%s\";\n"
        "const char compress_suite_emit_sha256[65]=\"%s\";\n",
        COMPRESS_SUITE_COUNT, digests->source_sha256, digests->emit_sha256);
    COMPRESS_CHECK(length > 0 && (size_t)length < sizeof(footer) - used);
    used += (size_t)length;
    FILE *file = fopen(path, verify ? "rb" : "wb");
    COMPRESS_CHECK(file);
    compress_emit_bytes(file, code->text, code->length, verify);
    compress_emit_bytes(file, footer, used, verify);
    if (verify) COMPRESS_CHECK(fgetc(file) == EOF && !ferror(file));
    COMPRESS_CHECK(fclose(file) == 0);
}

int main(int argc, char **argv) {
    COMPRESS_CHECK(argc == 4 || (argc == 5 && !strcmp(argv[4], "--emit-only")));
    bool execute = argc == 4;
    char source[2048], stdlib[2048], digest[65], after[65], emit_digest[65];
    int length = snprintf(source, sizeof(source), "%s/%s", argv[1], COMPRESS_SUITE_SOURCE);
    COMPRESS_CHECK(length > 0 && (size_t)length < sizeof(source));
    length = snprintf(stdlib, sizeof(stdlib), "%s/stdlib", argv[1]);
    COMPRESS_CHECK(length > 0 && (size_t)length < sizeof(stdlib));
    compress_source_digest(source, digest);
    XrCompileResourceLimits limits = XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrCompileResources *resources = NULL;
    COMPRESS_CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    XrCompilerSession *session = NULL;
    COMPRESS_CHECK(xr_compile_session_new(resources, &session) == XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, argv[1]};
    XrXirSourceProductRequest request = {
        {session, source, &authority, &context, stdlib, NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_LE64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product = NULL;
    XrXirSourceProductDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_product_build(&request, &product, &diagnostic);
    if (status != XR_XIR_OK) {
        XrFrontendRoot error = {0};
        if (diagnostic.failure.report) xr_frontend_report_root(diagnostic.failure.report, 0, &error);
        fprintf(stderr, "source status=%u stage=%u %d:%d %s\n", status, diagnostic.stage,
            error.primary.line, error.primary.column, error.message ? error.message : "");
    }
    COMPRESS_CHECK(status == XR_XIR_OK);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_compile_session_free(session);
    compress_source_digest(source, after);
    COMPRESS_CHECK(!strcmp(digest, after));
    const XrXirSourceTests *tests = xr_xir_compile_source_product_tests(product);
    COMPRESS_CHECK(tests && tests->count == COMPRESS_SUITE_COUNT);
    const XrXirModule *module = xr_xir_compile_source_product_module(product);
    COMPRESS_CHECK(module && module->declarations);
    uint32_t functions[COMPRESS_SUITE_COUNT];
    for (uint32_t i = 0; i < COMPRESS_SUITE_COUNT; ++i) {
        const XrXirSourceTestEntry *test = tests->entries + i;
        COMPRESS_CHECK(test->name_length == strlen(compress_suite_names[i]));
        COMPRESS_CHECK(!strcmp(test->name, compress_suite_names[i]));
        COMPRESS_CHECK(test->role == XR_XIR_TEST_ROLE_TEST && test->timeout_seconds == 0);
        functions[i] = test->function;
        COMPRESS_CHECK(functions[i] < module->function_count);
        COMPRESS_CHECK(module->declarations->functions[functions[i]].module == module->declarations->root_module);
        COMPRESS_CHECK(module->functions[functions[i]].parameter_count == 0);
        COMPRESS_CHECK(module->functions[functions[i]].result == XR_XIR_UNIT);
        for (uint32_t j = 0; j < i; ++j) COMPRESS_CHECK(functions[j] != functions[i]);
    }
    XrXirCSource code = {0};
    COMPRESS_CHECK(xr_xir_compile_source_product_emit(product, "compress_suite", 33554432, &code) == XR_XIR_OK);
    uint8_t emit_bytes[32];
    xr_sha256((const uint8_t *)code.text, code.length, emit_bytes);
    compress_digest_text(emit_bytes, emit_digest);
    CompressSuiteDigests digests = {digest, emit_digest};
    compress_emit_file(argv[2], &code, functions, &digests, execute);
    xr_xir_compile_c_source_free(&code);
    XrXirProgram *program = NULL;
    COMPRESS_CHECK(xr_xir_compile_source_product_vm_take(product, &program) == XR_XIR_OK);
    xr_xir_compile_source_product_free(product);
    XrXirCallStatus statuses[COMPRESS_SUITE_COUNT] = {0};
    uint32_t passed = 0;
    if (execute) {
        for (uint32_t i = 0; i < COMPRESS_SUITE_COUNT; ++i) {
            statuses[i] = compress_suite_run(program, functions[i], "VM", compress_suite_names[i]);
            if (statuses[i] == XR_XIR_CALL_RETURNED) ++passed;
        }
    }
    xr_xir_compile_program_drop(program);
    xr_compile_resources_release(resources);
    compress_suite_report(argv[3], "VM", &digests, functions, statuses, execute);
    if (execute) {
        printf("compress catalog: %u PASS, 0 SKIP, %u FAIL\n", passed, COMPRESS_SUITE_COUNT - passed);
        COMPRESS_CHECK(passed == COMPRESS_SUITE_COUNT);
    }
    return 0;
}
