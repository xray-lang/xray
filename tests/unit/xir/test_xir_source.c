/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source.c - Real source files through Checked and Lowered execution
 *
 * KEY CONCEPT:
 *   Destroy parser/session inputs before executing the owned artifact.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_checked.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_runtime_allocations.h"
#include "xir_source_cases.h"
int main(int argc, char **argv) {
    XrCompilerSession *session = xr_compiler_session_new(NULL);
    CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_SOURCE_FIXTURES};
    XrXirSourceRequest request = {session, XR_SOURCE_FIXTURES "/root.xr", &authority, NULL, XR_SOURCE_STDLIB};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    XrXirSourceDiagnostic diagnostic;
    XrXirStatus status = xr_xir_source_check(&request, &checked, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "source %u:%d:%d: %s (%u)\n", diagnostic.module,
        diagnostic.line, diagnostic.column, diagnostic.message, (unsigned) status);
    CHECK(status == XR_XIR_OK && checked);
    xr_compiler_session_delete(session);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    if (argc == 2) {
        FILE *file = fopen(argv[1], "wb"); CHECK(file);
        CHECK(fwrite(packet.bytes, 1, packet.length, file) == packet.length);
        CHECK(fclose(file) == 0);
    } else CHECK(argc == 1);
    xr_xir_checked_packet_free(&packet);
    XrXirArtifact *specialized = NULL;
    CHECK(xr_xir_specialize(checked, NULL, &specialized, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked); checked = specialized;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(checked, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirModule *module = xr_xir_artifact_module(lowered);
    uint32_t result = UINT32_MAX, advance = UINT32_MAX, update = UINT32_MAX, calculate = UINT32_MAX, resume_text = UINT32_MAX, stack_depth = UINT32_MAX;
    for (uint32_t i = 0; i < module->function_count; ++i) {
        if (module->functions[i].name_length == 6 && !memcmp(module->functions[i].name, "result", 6)) result = i;
        if (module->functions[i].name_length == 9 && !memcmp(module->functions[i].name, "calculate", 9)) calculate = i;
        if (module->functions[i].name_length == 6 && !memcmp(module->functions[i].name, "update", 6)) update = i;
        if (module->functions[i].name_length == 10 && !memcmp(module->functions[i].name, "resumeText", 10)) resume_text = i;
        if (module->functions[i].name_length == 10 && !memcmp(module->functions[i].name, "stackDepth", 10)) stack_depth = i;
        if (module->functions[i].name_length == 7 && !memcmp(module->functions[i].name, "advance", 7)) advance = i;
    }
    CHECK(result != UINT32_MAX && advance != UINT32_MAX && update != UINT32_MAX && calculate != UINT32_MAX && resume_text != UINT32_MAX && stack_depth != UINT32_MAX);
    uint32_t entry = module->declarations->entry_function;
    XrXirProgram *program = NULL;
    CHECK(xr_xir_vm_program_take(&lowered, 262144, &program) == XR_XIR_OK && !lowered);
    XrXirValue results[2] = {{0}, {0}};
    source_pair(program, entry, (SourceFunctions) {result, advance, update, calculate, resume_text, stack_depth}, results);
    runtime_source_failures(program, entry, resume_text);
    xr_xir_program_drop(program);
    source_result_drop(&results[0]); source_result_drop(&results[1]);
    puts("Real source modules, independent state and output passed in Lowered VM");
    CHECK(!runtime_live && !runtime_bytes);
    return 0;
}
