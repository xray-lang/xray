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
int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 2);
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
    CHECK(xr_xir_compile_emit_c(lowered, "array_source", 1048576, &source) == XR_XIR_OK);
    if (argc == 2) {
        file = fopen(argv[1], "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fprintf(file, "\nconst uint32_t array_source_functions[%u] = {", ARRAY_FUNCTION_COUNT) > 0);
        for (unsigned i = 0; i < ARRAY_FUNCTION_COUNT; ++i) CHECK(fprintf(file, "%s%uu", i ? "," : "", functions[i]) > 0);
        CHECK(fputs("};\n", file) >= 0 && fclose(file) == 0);
    }
    xr_xir_compile_c_source_free(&source);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    source_array_program_cases(program, functions); program = NULL;
    source_array_bounds_packet(&compiler, argc == 2 ? argv[1] : NULL);
    source_fixture_owner_free(&compiler);
    puts("Parser-free Array Checked consumer matched independent expectations and generated native C");
    return 0;
}
