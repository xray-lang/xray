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
#include "xir_source_compile_owner.h"
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
    XrXirProgram *vm_program;
} MixedSource;
static uint32_t released;
static void mixed_release(void *pointer) {
    MixedSource *owner = pointer;
    xr_xir_compile_artifact_free(owner->artifact); xr_compile_resources_free(owner->entries); xr_xir_compile_program_drop(owner->vm_program); xr_compile_resources_free(owner); ++released;
}
int main(void) {
    SourceLateResultEntries late_result_entries = {{fixture_source_late_result_values[0],fixture_source_late_result_values[1],fixture_source_late_result_values[2]},fixture_source_late_result_count};
    SourceMethodValueEntries method_value_entries = {{fixture_source_method_value_values[0],fixture_source_method_value_values[1],
        fixture_source_method_value_values[2],fixture_source_method_value_values[3],fixture_source_method_value_values[4],
        fixture_source_method_value_values[5]},fixture_source_method_value_count};
    SourceInferenceEntries inference_entries = {{fixture_source_inference_values[0],fixture_source_inference_values[1],
        fixture_source_inference_values[2],fixture_source_inference_values[3],fixture_source_inference_values[4],
        fixture_source_inference_values[5]},fixture_source_inference_count};
    const XrXirCompileContext context=*source_fixture_source_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
    MixedSource *owner=NULL;CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    owner->artifact=source_fixture_lower(&context,NULL);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    const XrXirModule *module = xr_xir_compile_artifact_module(owner->artifact);
    CHECK(module->function_count == fixture_source_program.entry_count);
    CHECK(xr_compile_resources_calloc(context.resources,module->function_count,sizeof(*owner->entries),(void **)&owner->entries)==XR_COMPILE_RESOURCE_OK);
    XrXirProgramProof proof=xr_xir_compile_program_proof(owner->artifact);
    CHECK(proof.length==fixture_source_program.proof.length && !memcmp(proof.bytes,fixture_source_program.proof.bytes,proof.length));
    CHECK(xr_xir_compile_vm_program_take(&owner->artifact,&owner->vm_program)==XR_XIR_OK && !owner->artifact);
    CHECK(owner->entries && owner->vm_program && owner->vm_program->entry_count==module->function_count);
    unsigned native_resumes = 0, vm_pauses = 0, pause_types = 0;
    for (uint32_t i = 0; i < module->function_count; ++i) {
        /* Test assembly copies the actual sealed table. Its code lease remains
         * owned until the mixed Program has fully exited and released. */
        owner->entries[i]=owner->vm_program->entries[i];
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
        module->declarations, {owner, mixed_release}, module->types, proof};
    uint32_t entry = module->declarations->entry_function;
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&context,&spec, &program) == XR_XIR_OK);
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
    xr_xir_compile_program_drop(program); CHECK(released == 1);
    source_late_result_retained_drop(late_result_retained);
    source_method_value_retained_drop(method_value_retained);
    source_inference_retained_drop(inference_retained);
    source_result_drop(&results[0]); source_result_drop(&results[1]);
    puts("VM to native and native to VM source calls shared instance state and ownership");
    CHECK(!runtime_live && !runtime_bytes);
    source_fixture_source_owners_free();
    return 0;
}
