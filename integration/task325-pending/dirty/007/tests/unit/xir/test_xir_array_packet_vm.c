/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_packet_vm.c - Parser-free Array Checked consumer and C producer
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_source_array_cases.h"
#include "xir_source_array_reordering_cases.h"
#include "xir_source_array_pipeline.h"
static void source_array_bounds_packet(SourceFixtureOwner *compiler, const char *output) {
    FILE *file = fopen(XR_ARRAY_BOUNDS_CHECKED_FIXTURE, "rb"); CHECK(file);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file); CHECK(size > 0 && size < 1048576 && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = xr_malloc((size_t) size); CHECK(bytes);
    CHECK(fread(bytes, 1, (size_t) size, file) == (size_t) size && fclose(file) == 0);
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_compile_checked_read(&compiler->context, bytes, (size_t) size, &checked, NULL) == XR_XIR_OK);
    memset(bytes, 0xCC, (size_t) size); xr_free(bytes);
    XrXirArtifact *lowered = source_array_lower(checked);
    uint32_t entry = xr_xir_compile_artifact_module(lowered)->declarations->entry_function;
    XrXirCSource source = {0};
    CHECK(xr_xir_compile_emit_c(lowered, "array_bounds", 1048576, &source) == XR_XIR_OK);
    if (output) {
        file = fopen(output, "ab"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length && fclose(file) == 0);
    }
    xr_xir_compile_c_source_free(&source);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    source_array_sticky_bounds(program, entry); program = NULL;
}
static void source_array_original_packet(const char *output) {
    SourceFixtureOwner compiler = {0}; source_fixture_owner_new(&compiler);
    FILE *file = fopen(XR_ARRAY_CHECKED_FIXTURE, "rb"); CHECK(file);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file); CHECK(size > 0 && size < 1048576 && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = xr_malloc((size_t) size); CHECK(bytes);
    CHECK(fread(bytes, 1, (size_t) size, file) == (size_t) size && fclose(file) == 0);
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_compile_checked_read(&compiler.context, bytes, (size_t) size, &checked, NULL) == XR_XIR_OK);
    memset(bytes, 0xCC, (size_t) size); xr_free(bytes);
    XrXirArtifact *lowered = source_array_lower(checked);
    uint32_t functions[ARRAY_FUNCTION_COUNT]; source_array_find(xr_xir_compile_artifact_module(lowered), functions);
    XrXirCSource source = {0};
    XrXirStatus emitted = xr_xir_compile_emit_c(lowered, "array_source", 1048576, &source);
    if (emitted != XR_XIR_OK) {
        XrCompileResourceStats stats = {0};
        CHECK(xr_compile_resources_stats(compiler.context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
        fprintf(stderr, "Array C emission status=%u allocated=%llu work=%llu live=%llu peak=%llu; output cap=1048576\n",
            (unsigned) emitted, (unsigned long long) stats.allocated_bytes, (unsigned long long) stats.work,
            (unsigned long long) stats.live_bytes, (unsigned long long) stats.peak_bytes);
    }
    CHECK(emitted == XR_XIR_OK);
    printf("Array original generated C: %zu bytes (cap 1048576)\n", source.length);
    if (output) {
        file = fopen(output, "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fprintf(file, "\nconst uint32_t array_source_functions[%u] = {", ARRAY_FUNCTION_COUNT) > 0);
        for (unsigned i = 0; i < ARRAY_FUNCTION_COUNT; ++i) CHECK(fprintf(file, "%s%uu", i ? "," : "", functions[i]) > 0);
        CHECK(fputs("};\n", file) >= 0 && fclose(file) == 0);
    }
    xr_xir_compile_c_source_free(&source);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    source_array_program_cases(program, functions); program = NULL;
    source_array_bounds_packet(&compiler, output);
    if (output) {
        file = fopen(output, "rb"); CHECK(file && fseek(file, 0, SEEK_END) == 0);
        long emitted_bytes = ftell(file); CHECK(emitted_bytes > 0 && emitted_bytes < 1048576 && fclose(file) == 0);
        printf("Array original plus bounds C: %ld bytes (cap 1048576)\n", emitted_bytes);
    }
    source_fixture_owner_free(&compiler);
    puts("Parser-free Array Checked consumer matched independent expectations and generated native C");
}

static void source_array_reordering_packet(const char *output) {
    SourceFixtureOwner compiler = {0}; source_fixture_owner_new(&compiler);
    FILE *file = fopen(XR_ARRAY_REORDER_CHECKED_FIXTURE, "rb"); CHECK(file);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file); CHECK(size > 0 && size < 1048576 && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = xr_malloc((size_t) size); CHECK(bytes);
    CHECK(fread(bytes, 1, (size_t) size, file) == (size_t) size && fclose(file) == 0);
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_compile_checked_read(&compiler.context, bytes, (size_t) size, &checked, NULL) == XR_XIR_OK);
    memset(bytes, 0xCC, (size_t) size); xr_free(bytes);
    XrXirArtifact *lowered = source_array_lower(checked);
    uint32_t functions[REORDER_FUNCTION_COUNT]; source_array_reorder_find(xr_xir_compile_artifact_module(lowered), functions);
    XrXirCSource source = {0};
    XrXirStatus emitted = xr_xir_compile_emit_c(lowered, "array_reorder", 1048576, &source);
    if (emitted != XR_XIR_OK) {
        XrCompileResourceStats stats = {0};
        CHECK(xr_compile_resources_stats(compiler.context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
        fprintf(stderr, "Array reorder C emission status=%u allocated=%llu work=%llu live=%llu peak=%llu; output cap=1048576\n",
            (unsigned) emitted, (unsigned long long) stats.allocated_bytes, (unsigned long long) stats.work,
            (unsigned long long) stats.live_bytes, (unsigned long long) stats.peak_bytes);
    }
    CHECK(emitted == XR_XIR_OK && source.length < 1048576);
    if (output) {
        file = fopen(output, "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fprintf(file, "\nconst uint32_t array_reorder_functions[%u] = {", REORDER_FUNCTION_COUNT) > 0);
        for (unsigned i = 0; i < REORDER_FUNCTION_COUNT; ++i) CHECK(fprintf(file, "%s%uu", i ? "," : "", functions[i]) > 0);
        CHECK(fputs("};\n", file) >= 0);
        long emitted_bytes = ftell(file); CHECK(emitted_bytes > 0 && emitted_bytes < 1048576 && fclose(file) == 0);
    }
    printf("Array reorder generated C: %zu bytes (cap 1048576)\n", source.length);
    xr_xir_compile_c_source_free(&source);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    source_array_reordering_program_cases(program, functions);
    source_fixture_owner_free(&compiler);
}
int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 3);
    source_array_original_packet(argc == 3 ? argv[1] : NULL);
    source_array_reordering_packet(argc == 3 ? argv[2] : NULL);
    return 0;
}
