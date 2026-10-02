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
#include "xir_source_method_value_execution.h"
#include "xir_source_late_result_execution.h"
#include "xir_source_method_value_selection.h"
#include "xir_source_late_result_selection.h"
#include "xir_source_inference_execution.h"
#include "xir_source_inference_selection.h"
/* Lowering frees abstract type payloads allocated by the counted type clone. */
#include "xir_source_cases.h"
#include "xir_source_default_runtime_gaps.h"
int main(int argc, char **argv) {
    source_default_runtime_gaps();
    XrCompilerSession *session = xr_compiler_session_new(NULL);
    CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_SOURCE_FIXTURES};
    XrXirSourceRequest request = {session, XR_SOURCE_FIXTURES "/root.xr", &authority, NULL, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    XrXirSourceDiagnostic diagnostic;
    XrXirSourceResult query_result_1 = {0};
    XrXirStatus query_status_1 = xr_xir_source_check(&request, &query_result_1, &diagnostic);
    checked = query_result_1.checked; query_result_1.checked = NULL;
    xr_xir_source_result_free(&query_result_1);
    XrXirStatus status = query_status_1;
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
    XrXirDiagnostic lower_diagnostic;
    status = xr_xir_lower(checked, &target, NULL, &lowered, &lower_diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "lower status %u function %u block %u instruction %u\n",
        (unsigned) status, lower_diagnostic.function, lower_diagnostic.block, lower_diagnostic.instruction);
    CHECK(status == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirModule *module = xr_xir_artifact_module(lowered);
    SourceMethodValueEntries method_value_entries = source_method_value_select(module);
    SourceLateResultEntries late_result_entries = source_late_result_select(module);
    SourceInferenceEntries inference_entries = source_inference_select(module);
    uint32_t result = UINT32_MAX, advance = UINT32_MAX, update = UINT32_MAX, calculate = UINT32_MAX, resume_text = UINT32_MAX, stack_depth = UINT32_MAX, numeric_pause = UINT32_MAX, bound_result = UINT32_MAX, witness_result = UINT32_MAX, enum_witness_result = UINT32_MAX, enum_generic_witness_result = UINT32_MAX, generic_method_number = UINT32_MAX, generic_method_text = UINT32_MAX, generic_method_array = UINT32_MAX;
    for (uint32_t i = 0; i < module->function_count; ++i) {
        if (module->functions[i].name_length == 6 && !memcmp(module->functions[i].name, "result", 6)) result = i;
        if (module->functions[i].name_length == 9 && !memcmp(module->functions[i].name, "calculate", 9)) calculate = i;
        if (module->functions[i].name_length == 6 && !memcmp(module->functions[i].name, "update", 6)) update = i;
        if (module->functions[i].name_length == 10 && !memcmp(module->functions[i].name, "resumeText", 10)) resume_text = i;
        if (module->functions[i].name_length == 10 && !memcmp(module->functions[i].name, "stackDepth", 10)) stack_depth = i;
        if (module->functions[i].name_length == 12 && !memcmp(module->functions[i].name, "numericPause", 12)) numeric_pause = i;
        if (module->functions[i].name_length == 11 && !memcmp(module->functions[i].name, "boundResult", 11)) bound_result = i;
        if (module->functions[i].name_length == 13 && !memcmp(module->functions[i].name, "witnessResult", 13)) witness_result = i;
        if (module->functions[i].name_length == 17 && !memcmp(module->functions[i].name, "enumWitnessResult", 17)) enum_witness_result = i;
        if (module->functions[i].name_length == 24 && !memcmp(module->functions[i].name, "enumGenericWitnessResult", 24)) enum_generic_witness_result = i;
        if (module->functions[i].name_length == 19 && !memcmp(module->functions[i].name, "genericMethodNumber", 19)) generic_method_number = i;
        if (module->functions[i].name_length == 17 && !memcmp(module->functions[i].name, "genericMethodText", 17)) generic_method_text = i;
        if (module->functions[i].name_length == 18 && !memcmp(module->functions[i].name, "genericMethodArray", 18)) generic_method_array = i;
        if (module->functions[i].name_length == 7 && !memcmp(module->functions[i].name, "advance", 7)) advance = i;
    }
    CHECK(result != UINT32_MAX && advance != UINT32_MAX && update != UINT32_MAX && calculate != UINT32_MAX && resume_text != UINT32_MAX && stack_depth != UINT32_MAX && numeric_pause != UINT32_MAX && bound_result != UINT32_MAX && witness_result != UINT32_MAX && enum_witness_result != UINT32_MAX && enum_generic_witness_result != UINT32_MAX && generic_method_number != UINT32_MAX && generic_method_text != UINT32_MAX && generic_method_array != UINT32_MAX);
    uint32_t entry = module->declarations->entry_function;
    XrXirProgram *program = NULL;
    CHECK(xr_xir_vm_program_take(&lowered, (XrXirProgramBudget) {1048576, 64000000}, &program) == XR_XIR_BUDGET);
    CHECK(lowered && !program);
    XrXirStatus seal_status = xr_xir_vm_program_take(&lowered, (XrXirProgramBudget) {33554432, 64000000}, &program);
    if (seal_status != XR_XIR_OK) fprintf(stderr, "source seal status: %u\n", (unsigned)seal_status);
    CHECK(seal_status == XR_XIR_OK && !lowered);
    XrXirValue results[2] = {{0}, {0}};
    source_pair(program, entry, (SourceFunctions) {result, advance, update, calculate, resume_text, stack_depth, numeric_pause, bound_result, witness_result, enum_witness_result, enum_generic_witness_result, generic_method_number, generic_method_text, generic_method_array}, results);
    runtime_source_failures(program, (RuntimeSourceEntries){entry, resume_text, numeric_pause, enum_witness_result, enum_generic_witness_result, generic_method_number, generic_method_text, generic_method_array});
    XrXirValue inference_retained[2][6] = {{{0}}};
    source_inference_pair(program,inference_entries,inference_retained);
    source_inference_runtime_failures(program,inference_entries);
    XrXirValue method_value_retained[2][6] = {{{0}}};
    source_method_value_pair(program,method_value_entries,method_value_retained);
    source_method_value_runtime_failures(program,method_value_entries);
    XrXirValue late_result_retained[2][3] = {{{0}}};
    source_late_result_pair(program,late_result_entries,late_result_retained);
    source_late_result_runtime_failures(program,late_result_entries);
    xr_xir_program_drop(program);
    source_late_result_retained_drop(late_result_retained);
    source_method_value_retained_drop(method_value_retained);
    source_inference_retained_drop(inference_retained);
    source_result_drop(&results[0]); source_result_drop(&results[1]);
    puts("Real source modules, independent state and output passed in Lowered VM");
    if (runtime_live) {
        fprintf(stderr, "runtime residual: %zu allocations, %zu bytes\n", runtime_live, runtime_bytes);
        runtime_report_residuals(stderr);
    }
    CHECK(!runtime_live && !runtime_bytes);
    return 0;
}
