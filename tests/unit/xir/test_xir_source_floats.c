/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_floats.c - Real source Float values through the single pipeline
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_runtime_allocations.h"
#include "xir_source_float_cases.h"
#include "xir_source_float_pipeline.h"
int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 2);
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_FLOAT_SOURCE_FIXTURES};
    XrXirSourceRequest request = {session, XR_FLOAT_SOURCE_FIXTURES "/root.xr", &authority, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_source_check(&request, &result, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "float source %u %u:%d:%d %s\n", status,
        diagnostic.module, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    XrXirArtifact *checked = result.checked; result.checked = NULL;
    xr_compiler_session_delete(session);
    xr_xir_source_result_free(&result);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    if (argc == 2) {
        FILE *file = fopen(argv[1], "wb"); CHECK(file);
        CHECK(fwrite(packet.bytes, 1, packet.length, file) == packet.length && fclose(file) == 0);
    }
    xr_xir_checked_packet_free(&packet);
    XrXirArtifact *lowered = source_float_lower(checked);
    uint32_t functions[FLOAT_FUNCTION_COUNT]; source_float_find(xr_xir_artifact_module(lowered), functions);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_vm_program_take(&lowered, (XrXirProgramBudget) {2097152, 16000000}, &program) == XR_XIR_OK && !lowered);
    source_float_program_cases(program, functions);
    puts("Source floating program matched independent output and lifecycle expectations");
    return 0;
}
