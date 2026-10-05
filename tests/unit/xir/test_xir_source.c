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
#include "xir_source_compile_owner.h"
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
    CHECK(argc==1 || argc==2);
    const XrXirCompileContext context=*source_fixture_source_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
    XrXirArtifact *lowered=source_fixture_lower(&context,argc==2 ? argv[1] : NULL);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
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
    XrCompileResourceStats prefix={0};CHECK(xr_compile_resources_stats(context.resources,&prefix)==XR_COMPILE_RESOURCE_OK);
    size_t negative_physical_blocks=source_fixture_compile_live,negative_physical_bytes=source_fixture_compile_bytes;
    const XrXirCompileContext negative=*source_fixture_source_owner(prefix.allocated_bytes+UINT64_C(1048576),UINT64_C(128000000));
    XrXirArtifact *negative_lowered=source_fixture_lower(&negative,NULL);
    XrCompileResourceStats before_failure={0},after_failure={0};
    CHECK(xr_compile_resources_stats(negative.resources,&before_failure)==XR_COMPILE_RESOURCE_OK);
    CHECK(before_failure.allocated_bytes==prefix.allocated_bytes && before_failure.work==prefix.work);
    XrXirArtifact *sentinel=negative_lowered;XrXirProgram *failed_program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&negative_lowered,&failed_program)==XR_XIR_BUDGET);
    CHECK(negative_lowered==sentinel && !failed_program);
    CHECK(xr_xir_compile_artifact_module(negative_lowered)->stage==XR_XIR_LOWERED);
    CHECK(xr_compile_resources_stats(negative.resources,&after_failure)==XR_COMPILE_RESOURCE_OK);
    CHECK(after_failure.live_bytes==before_failure.live_bytes);
    fprintf(stderr,"Source seal negative: prefix allocated=%llu work=%llu; failure allocated=%llu work=%llu live=%llu\n",
        (unsigned long long)prefix.allocated_bytes,(unsigned long long)prefix.work,
        (unsigned long long)after_failure.allocated_bytes,(unsigned long long)after_failure.work,(unsigned long long)after_failure.live_bytes);
    xr_xir_compile_artifact_free(negative_lowered);source_fixture_source_owner_close(&negative);
    CHECK(source_fixture_compile_live==negative_physical_blocks && source_fixture_compile_bytes==negative_physical_bytes);
    XrXirStatus seal_status=xr_xir_compile_vm_program_take(&lowered,&program);
    if(seal_status!=XR_XIR_OK)fprintf(stderr,"source seal status: %u\n",(unsigned)seal_status);
    CHECK(seal_status==XR_XIR_OK && !lowered);
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
    xr_xir_compile_program_drop(program);
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
    source_fixture_source_owners_free();
    return 0;
}
