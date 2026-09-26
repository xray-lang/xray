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
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_cases.h"
int main(int argc, char **argv) {
    XrCompilerSession *session = xr_compiler_session_new(NULL);
    CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_SOURCE_FIXTURES};
    XrXirSourceRequest request = {session, XR_SOURCE_FIXTURES "/root.xr", &authority, NULL};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    XrXirSourceDiagnostic diagnostic;
    XrXirStatus status = xr_xir_source_check(&request, &checked, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "source %u:%d:%d: %s (%u)\n", diagnostic.module,
        diagnostic.line, diagnostic.column, diagnostic.message, (unsigned) status);
    CHECK(status == XR_XIR_OK && checked);
    xr_compiler_session_delete(session);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(checked, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirModule *module = xr_xir_artifact_module(lowered);
    uint32_t result = UINT32_MAX, advance = UINT32_MAX;
    for (uint32_t i = 0; i < module->function_count; ++i) {
        if (module->functions[i].name_length == 6 && !memcmp(module->functions[i].name, "result", 6)) result = i;
        if (module->functions[i].name_length == 7 && !memcmp(module->functions[i].name, "advance", 7)) advance = i;
    }
    CHECK(result != UINT32_MAX && advance != UINT32_MAX);
    uint32_t entry = module->declarations->entry_function;
    XrXirCSource source;
    CHECK(xr_xir_emit_c(lowered, "fixture_source", 262144, &source) == XR_XIR_OK);
    if (argc == 2) {
        FILE *file = fopen(argv[1], "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fprintf(file, "\nconst uint32_t fixture_source_result = %uu;\nconst uint32_t fixture_source_advance = %uu;\n", result, advance) > 0);
        CHECK(fclose(file) == 0);
    } else CHECK(argc == 1);
    xr_xir_c_source_free(&source);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_vm_program_take(&lowered, 262144, &program) == XR_XIR_OK && !lowered);
    XrXirValue first = source_run(program, entry, result, advance);
    XrXirValue second = source_run(program, entry, result, advance);
    xr_xir_program_drop(program);
    source_result_drop(&first); source_result_drop(&second);
    puts("Real source modules, independent state and output passed in Lowered VM");
    return 0;
}
