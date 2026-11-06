/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_native.c - Real source closure in a runtime-only executable
 *
 * KEY CONCEPT:
 *   Native execution uses fixed expectations without the compiler or VM.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_compile_owner.h"
#include "xir_source_runtime_allocations.h"
#include "xir_source_method_value_execution.h"
#include "xir_source_late_result_execution.h"
#include "xir_source_inference_execution.h"
#include "xir_source_cases.h"
XR_DATA const XrXirProgramSpec fixture_source_program;
XR_DATA const uint32_t fixture_source_method_value_values[6];
XR_DATA const uint32_t fixture_source_method_value_count;
XR_DATA const uint32_t fixture_source_late_result_values[3];
XR_DATA const uint32_t fixture_source_late_result_count;
XR_DATA const uint32_t fixture_source_inference_values[6];
XR_DATA const uint32_t fixture_source_inference_count;
XR_DATA const uint32_t fixture_source_result, fixture_source_advance, fixture_source_update, fixture_source_calculate, fixture_source_resume_text, fixture_source_stack_depth, fixture_source_numeric_pause, fixture_source_bound_result, fixture_source_witness_result, fixture_source_enum_witness_result, fixture_source_enum_generic_witness_result, fixture_source_generic_method_number, fixture_source_generic_method_text, fixture_source_generic_method_array;
int main(void) {
    SourceLateResultEntries late_result_entries = {{fixture_source_late_result_values[0],fixture_source_late_result_values[1],fixture_source_late_result_values[2]},fixture_source_late_result_count};
    SourceMethodValueEntries method_value_entries = {{fixture_source_method_value_values[0],fixture_source_method_value_values[1],
        fixture_source_method_value_values[2],fixture_source_method_value_values[3],fixture_source_method_value_values[4],
        fixture_source_method_value_values[5]},fixture_source_method_value_count};
    SourceInferenceEntries inference_entries = {{fixture_source_inference_values[0],fixture_source_inference_values[1],
        fixture_source_inference_values[2],fixture_source_inference_values[3],fixture_source_inference_values[4],
        fixture_source_inference_values[5]},fixture_source_inference_count};
    size_t physical_blocks=source_fixture_compile_live,physical_bytes=source_fixture_compile_bytes;
    const XrXirCompileContext *measured_context=source_fixture_source_owner((XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE,UINT64_C(8)*1024*1024,true});
    XrXirProgram *measured_program=NULL;XrCompileResourceStats measured={0};
    CHECK(xr_xir_compile_program_seal(measured_context,&fixture_source_program,&measured_program)==XR_XIR_OK);
    CHECK(xr_compile_resources_stats(measured_context->resources,&measured)==XR_COMPILE_RESOURCE_OK && measured.peak_bytes>1);
    xr_xir_compile_program_drop(measured_program);source_fixture_source_owner_close(measured_context);
    CHECK(source_fixture_compile_live==physical_blocks && source_fixture_compile_bytes==physical_bytes);
    const XrXirCompileContext *context=source_fixture_source_owner(
        (XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE,measured.peak_bytes,true});
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(context,&fixture_source_program,&program)==XR_XIR_OK);
    physical_blocks=source_fixture_compile_live;physical_bytes=source_fixture_compile_bytes;
    const XrXirCompileContext *negative=source_fixture_source_owner(
        (XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE,measured.peak_bytes-1,true});
    XrXirProgram *failed_program=NULL;
    CHECK(xr_xir_compile_program_seal(negative,&fixture_source_program,&failed_program)==XR_XIR_MEMORY_LIMIT && !failed_program);
    source_fixture_source_owner_close(negative);
    CHECK(source_fixture_compile_live==physical_blocks && source_fixture_compile_bytes==physical_bytes);
    XrXirValue results[2] = {{0}, {0}};
    source_pair(program, fixture_source_program.declarations->entry_function,
        (SourceFunctions) {fixture_source_result, fixture_source_advance, fixture_source_update, fixture_source_calculate, fixture_source_resume_text, fixture_source_stack_depth, fixture_source_numeric_pause, fixture_source_bound_result, fixture_source_witness_result, fixture_source_enum_witness_result, fixture_source_enum_generic_witness_result, fixture_source_generic_method_number, fixture_source_generic_method_text, fixture_source_generic_method_array}, results);
    runtime_source_failures(program, (RuntimeSourceEntries){fixture_source_program.declarations->entry_function, fixture_source_resume_text, fixture_source_numeric_pause, fixture_source_enum_witness_result, fixture_source_enum_generic_witness_result, fixture_source_generic_method_number, fixture_source_generic_method_text, fixture_source_generic_method_array});
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
    puts("Real source modules matched independent native expectations");
    CHECK(!runtime_live && !runtime_bytes);
    source_fixture_source_owners_free();
    return 0;
}
