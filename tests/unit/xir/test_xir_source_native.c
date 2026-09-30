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
#include "xir_source_runtime_allocations.h"
#include "xir_source_inference_execution.h"
#include "xir_source_cases.h"
XR_DATA const XrXirProgramSpec fixture_source_program;
XR_DATA const uint32_t fixture_source_inference_values[6];
XR_DATA const uint32_t fixture_source_inference_count;
XR_DATA const uint32_t fixture_source_result, fixture_source_advance, fixture_source_update, fixture_source_calculate, fixture_source_resume_text, fixture_source_stack_depth, fixture_source_numeric_pause, fixture_source_bound_result, fixture_source_witness_result, fixture_source_enum_witness_result, fixture_source_enum_generic_witness_result, fixture_source_generic_method_number, fixture_source_generic_method_text, fixture_source_generic_method_array;
int main(void) {
    SourceInferenceEntries inference_entries = {{fixture_source_inference_values[0],fixture_source_inference_values[1],
        fixture_source_inference_values[2],fixture_source_inference_values[3],fixture_source_inference_values[4],
        fixture_source_inference_values[5]},fixture_source_inference_count};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&fixture_source_program, (XrXirProgramBudget) {16777216, 1}, &program) == XR_XIR_BUDGET && !program);
    CHECK(xr_xir_program_seal(&fixture_source_program, (XrXirProgramBudget) {33554432, 64000000}, &program) == XR_XIR_OK);
    XrXirValue results[2] = {{0}, {0}};
    source_pair(program, fixture_source_program.declarations->entry_function,
        (SourceFunctions) {fixture_source_result, fixture_source_advance, fixture_source_update, fixture_source_calculate, fixture_source_resume_text, fixture_source_stack_depth, fixture_source_numeric_pause, fixture_source_bound_result, fixture_source_witness_result, fixture_source_enum_witness_result, fixture_source_enum_generic_witness_result, fixture_source_generic_method_number, fixture_source_generic_method_text, fixture_source_generic_method_array}, results);
    runtime_source_failures(program, (RuntimeSourceEntries){fixture_source_program.declarations->entry_function, fixture_source_resume_text, fixture_source_numeric_pause, fixture_source_enum_witness_result, fixture_source_enum_generic_witness_result, fixture_source_generic_method_number, fixture_source_generic_method_text, fixture_source_generic_method_array});
    XrXirValue inference_retained[2][6] = {{{0}}};
    source_inference_pair(program,inference_entries,inference_retained);
    source_inference_runtime_failures(program,inference_entries);
    xr_xir_program_drop(program);
    source_inference_retained_drop(inference_retained);
    source_result_drop(&results[0]); source_result_drop(&results[1]);
    puts("Real source modules matched independent native expectations");
    CHECK(!runtime_live && !runtime_bytes);
    return 0;
}
