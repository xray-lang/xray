/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_mixed.c - Native and VM entries share one source instance
 *
 * KEY CONCEPT:
 *   One code lease owns VM environments while native entries use the same slots.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "toolchain/xcompiler_session.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_runtime_allocations.h"
#include "xir_source_method_value_execution.h"
#include "xir_source_late_result_execution.h"
#include "xir_source_inference_execution.h"
/* Lowering frees abstract type payloads allocated by the counted type clone. */
#include "xir_source_cases.h"
XR_DATA const XrXirProgramSpec fixture_source_program;
XR_DATA const uint32_t fixture_source_method_value_values[6];
XR_DATA const uint32_t fixture_source_method_value_count;
XR_DATA const uint32_t fixture_source_late_result_values[3];
XR_DATA const uint32_t fixture_source_late_result_count;
XR_DATA const uint32_t fixture_source_inference_values[6];
XR_DATA const uint32_t fixture_source_inference_count;
XR_DATA const uint32_t fixture_source_result, fixture_source_advance, fixture_source_update, fixture_source_calculate, fixture_source_resume_text, fixture_source_stack_depth, fixture_source_numeric_pause, fixture_source_bound_result, fixture_source_witness_result, fixture_source_enum_witness_result, fixture_source_enum_generic_witness_result, fixture_source_generic_method_number, fixture_source_generic_method_text, fixture_source_generic_method_array;
typedef struct MixedSource {
    XrXirArtifact *artifact;
    XrXirCallEntry *entries;
    XrXirVmBinding *bindings;
} MixedSource;
static uint32_t released;
static void mixed_release(void *pointer) {
    MixedSource *owner = pointer;
    xr_xir_artifact_free(owner->artifact); xr_free(owner->entries); xr_free(owner->bindings); xr_free(owner); ++released;
}
int main(void) {
    SourceLateResultEntries late_result_entries = {{fixture_source_late_result_values[0],fixture_source_late_result_values[1],fixture_source_late_result_values[2]},fixture_source_late_result_count};
    SourceMethodValueEntries method_value_entries = {{fixture_source_method_value_values[0],fixture_source_method_value_values[1],
        fixture_source_method_value_values[2],fixture_source_method_value_values[3],fixture_source_method_value_values[4],
        fixture_source_method_value_values[5]},fixture_source_method_value_count};
    SourceInferenceEntries inference_entries = {{fixture_source_inference_values[0],fixture_source_inference_values[1],
        fixture_source_inference_values[2],fixture_source_inference_values[3],fixture_source_inference_values[4],
        fixture_source_inference_values[5]},fixture_source_inference_count};
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_SOURCE_FIXTURES};
    XrXirSourceRequest request = {session, XR_SOURCE_FIXTURES "/root.xr", &authority, NULL, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL;
    XrXirSourceResult query_result_1 = {0};
    XrXirStatus query_status_1 = xr_xir_source_check(&request, &query_result_1, NULL);
    checked = query_result_1.checked; query_result_1.checked = NULL;
    xr_xir_source_result_free(&query_result_1);
    CHECK(query_status_1 == XR_XIR_OK);
    xr_compiler_session_delete(session);
    XrXirArtifact *specialized = NULL;
    CHECK(xr_xir_specialize(checked, NULL, &specialized, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked); checked = specialized;
    MixedSource *owner = xr_calloc(1, sizeof(*owner)); CHECK(owner);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(checked, &target, NULL, &owner->artifact, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirModule *module = xr_xir_artifact_module(owner->artifact);
    CHECK(module->function_count == fixture_source_program.entry_count);
    owner->entries = xr_calloc(module->function_count, sizeof(*owner->entries));
    owner->bindings = xr_calloc(module->function_count, sizeof(*owner->bindings));
    CHECK(owner->entries && owner->bindings);
    unsigned native_resumes = 0, vm_pauses = 0, pause_types = 0;
    for (uint32_t i = 0; i < module->function_count; ++i) {
        CHECK(xr_xir_vm_bind(owner->artifact, i, &owner->bindings[i], &owner->entries[i]) == XR_XIR_OK);
        CHECK(owner->entries[i].result == fixture_source_program.entries[i].result);
        CHECK(owner->entries[i].parameter_count == fixture_source_program.entries[i].parameter_count);
        if ((module->functions[i].name_length == 11 && (!memcmp(module->functions[i].name, "writeStdout", 11) ||
            !memcmp(module->functions[i].name, "writeStderr", 11))) ||
            (module->functions[i].name_length == 4 && !memcmp(module->functions[i].name, "next", 4)) ||
            (module->functions[i].name_length == 4 && !memcmp(module->functions[i].name, "pack", 4)) ||
            (module->functions[i].name_length == 6 && !memcmp(module->functions[i].name, "result", 6)) ||
            (module->functions[i].name_length == 13 && !memcmp(module->functions[i].name, "resumedNative", 13)) ||
            i == fixture_source_numeric_pause || i == fixture_source_calculate ||
            i == fixture_source_generic_method_number ||
            i == fixture_source_inference_values[0] || i == fixture_source_inference_values[4] ||
            i == fixture_source_method_value_values[0] || i == fixture_source_method_value_values[4] ||
            i == fixture_source_late_result_values[0])
            owner->entries[i] = fixture_source_program.entries[i];
        if (module->functions[i].name_length == 13 && !memcmp(module->functions[i].name, "resumedNative", 13)) {
            CHECK(owner->entries[i].resume == fixture_source_program.entries[i].resume); ++native_resumes;
        }
        if (module->functions[i].name_length > 6 && !memcmp(module->functions[i].name, "pause$", 6)) {
            XrXirType result = owner->entries[i].result;
            CHECK(owner->entries[i].parameter_count == 1);
            CHECK(result == XR_XIR_STRING || result == XR_XIR_I8 || result == XR_XIR_I16 || result == XR_XIR_F32 || result == XR_XIR_F64);
            unsigned bit = result == XR_XIR_STRING ? 1 : result == XR_XIR_I8 ? 2 : result == XR_XIR_I16 ? 4 : result == XR_XIR_F32 ? 8 : 16;
            CHECK(!(pause_types & bit)); pause_types |= bit;
            CHECK(owner->entries[i].resume != fixture_source_program.entries[i].resume); ++vm_pauses;
        }
    }
    CHECK(native_resumes == 1 && vm_pauses == 5 && pause_types == 31);
    CHECK(owner->entries[fixture_source_late_result_values[0]].resume == fixture_source_program.entries[fixture_source_late_result_values[0]].resume);
    CHECK(owner->entries[fixture_source_late_result_values[1]].resume != fixture_source_program.entries[fixture_source_late_result_values[1]].resume);
    CHECK(owner->entries[fixture_source_late_result_values[2]].resume != fixture_source_program.entries[fixture_source_late_result_values[2]].resume);
    CHECK(owner->entries[fixture_source_method_value_values[0]].resume == fixture_source_program.entries[fixture_source_method_value_values[0]].resume);
    CHECK(owner->entries[fixture_source_method_value_values[4]].resume == fixture_source_program.entries[fixture_source_method_value_values[4]].resume);
    CHECK(owner->entries[fixture_source_method_value_values[1]].resume != fixture_source_program.entries[fixture_source_method_value_values[1]].resume);
    CHECK(owner->entries[fixture_source_method_value_values[3]].resume != fixture_source_program.entries[fixture_source_method_value_values[3]].resume);
    CHECK(owner->entries[fixture_source_inference_values[0]].resume == fixture_source_program.entries[fixture_source_inference_values[0]].resume);
    CHECK(owner->entries[fixture_source_inference_values[4]].resume == fixture_source_program.entries[fixture_source_inference_values[4]].resume);
    CHECK(owner->entries[fixture_source_inference_values[1]].resume != fixture_source_program.entries[fixture_source_inference_values[1]].resume);
    CHECK(owner->entries[fixture_source_inference_values[3]].resume != fixture_source_program.entries[fixture_source_inference_values[3]].resume);
    CHECK(owner->entries[fixture_source_generic_method_number].resume == fixture_source_program.entries[fixture_source_generic_method_number].resume);
    CHECK(owner->entries[fixture_source_generic_method_text].resume != fixture_source_program.entries[fixture_source_generic_method_text].resume);
    CHECK(owner->entries[fixture_source_numeric_pause].resume == fixture_source_program.entries[fixture_source_numeric_pause].resume);
    CHECK(owner->entries[fixture_source_resume_text].resume != fixture_source_program.entries[fixture_source_resume_text].resume);
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, target, owner->entries, module->function_count,
        module->declarations, {owner, mixed_release}, module->types, xr_xir_program_proof(owner->artifact)};
    uint32_t entry = module->declarations->entry_function;
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&spec, (XrXirProgramBudget) {33554432, 64000000}, &program) == XR_XIR_OK);
    XrXirValue results[2] = {{0}, {0}};
    source_pair(program, entry, (SourceFunctions) {fixture_source_result, fixture_source_advance, fixture_source_update, fixture_source_calculate, fixture_source_resume_text, fixture_source_stack_depth, fixture_source_numeric_pause, fixture_source_bound_result, fixture_source_witness_result, fixture_source_enum_witness_result, fixture_source_enum_generic_witness_result, fixture_source_generic_method_number, fixture_source_generic_method_text, fixture_source_generic_method_array}, results);
    runtime_source_failures(program, (RuntimeSourceEntries){entry, fixture_source_resume_text, fixture_source_numeric_pause, fixture_source_enum_witness_result, fixture_source_enum_generic_witness_result, fixture_source_generic_method_number, fixture_source_generic_method_text, fixture_source_generic_method_array});
    XrXirValue inference_retained[2][6] = {{{0}}};
    source_inference_pair(program,inference_entries,inference_retained);
    source_inference_runtime_failures(program,inference_entries);
    XrXirValue method_value_retained[2][6] = {{{0}}};
    source_method_value_pair(program,method_value_entries,method_value_retained);
    source_method_value_runtime_failures(program,method_value_entries);
    XrXirValue late_result_retained[2][3] = {{{0}}};
    source_late_result_pair(program,late_result_entries,late_result_retained);
    source_late_result_runtime_failures(program,late_result_entries);
    CHECK(!released);
    xr_xir_program_drop(program); CHECK(released == 1);
    source_late_result_retained_drop(late_result_retained);
    source_method_value_retained_drop(method_value_retained);
    source_inference_retained_drop(inference_retained);
    source_result_drop(&results[0]); source_result_drop(&results[1]);
    puts("VM to native and native to VM source calls shared instance state and ownership");
    CHECK(!runtime_live && !runtime_bytes);
    return 0;
}
