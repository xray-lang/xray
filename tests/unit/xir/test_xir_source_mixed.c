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
#include "xir_source_cases.h"
XR_DATA const XrXirProgramSpec fixture_source_program;
XR_DATA const uint32_t fixture_source_result, fixture_source_advance, fixture_source_update, fixture_source_calculate, fixture_source_resume_text, fixture_source_stack_depth;
typedef struct MixedSource {
    XrXirArtifact *artifact;
    XrXirCallEntry entries[64];
    XrXirVmBinding bindings[64];
} MixedSource;
static uint32_t released;
static void mixed_release(void *pointer) {
    MixedSource *owner = pointer;
    xr_xir_artifact_free(owner->artifact); xr_free(owner); ++released;
}
int main(void) {
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_SOURCE_FIXTURES};
    XrXirSourceRequest request = {session, XR_SOURCE_FIXTURES "/root.xr", &authority, NULL, XR_SOURCE_STDLIB};
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_source_check(&request, &checked, NULL) == XR_XIR_OK);
    xr_compiler_session_delete(session);
    XrXirArtifact *specialized = NULL;
    CHECK(xr_xir_specialize(checked, NULL, &specialized, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked); checked = specialized;
    MixedSource *owner = xr_calloc(1, sizeof(*owner)); CHECK(owner);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(checked, &target, NULL, &owner->artifact, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirModule *module = xr_xir_artifact_module(owner->artifact);
    CHECK(module->function_count <= 64 && module->function_count == fixture_source_program.entry_count);
    unsigned native_resumes = 0, vm_pauses = 0;
    for (uint32_t i = 0; i < module->function_count; ++i) {
        CHECK(xr_xir_vm_bind(owner->artifact, i, &owner->bindings[i], &owner->entries[i]) == XR_XIR_OK);
        CHECK(owner->entries[i].result == fixture_source_program.entries[i].result);
        CHECK(owner->entries[i].parameter_count == fixture_source_program.entries[i].parameter_count);
        if ((module->functions[i].name_length == 11 && (!memcmp(module->functions[i].name, "writeStdout", 11) ||
            !memcmp(module->functions[i].name, "writeStderr", 11))) ||
            (module->functions[i].name_length == 4 && !memcmp(module->functions[i].name, "next", 4)) ||
            (module->functions[i].name_length == 4 && !memcmp(module->functions[i].name, "pack", 4)) ||
            (module->functions[i].name_length == 6 && !memcmp(module->functions[i].name, "result", 6)) ||
            (module->functions[i].name_length == 13 && !memcmp(module->functions[i].name, "resumedNative", 13)))
            owner->entries[i] = fixture_source_program.entries[i];
        if (module->functions[i].name_length == 13 && !memcmp(module->functions[i].name, "resumedNative", 13)) {
            CHECK(owner->entries[i].resume == fixture_source_program.entries[i].resume); ++native_resumes;
        }
        if (module->functions[i].name_length > 6 && !memcmp(module->functions[i].name, "pause$", 6)) {
            CHECK(owner->entries[i].parameter_count == 1 && owner->entries[i].result == XR_XIR_STRING);
            CHECK(owner->entries[i].resume != fixture_source_program.entries[i].resume); ++vm_pauses;
        }
    }
    CHECK(native_resumes == 1 && vm_pauses == 1);
    CHECK(owner->entries[fixture_source_resume_text].resume != fixture_source_program.entries[fixture_source_resume_text].resume);
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, target, owner->entries, module->function_count,
        module->declarations, {owner, mixed_release}};
    uint32_t entry = module->declarations->entry_function;
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&spec, 262144, &program) == XR_XIR_OK);
    XrXirValue results[2] = {{0}, {0}};
    source_pair(program, entry, (SourceFunctions) {fixture_source_result, fixture_source_advance, fixture_source_update, fixture_source_calculate, fixture_source_resume_text, fixture_source_stack_depth}, results);
    runtime_source_failures(program, entry, fixture_source_resume_text);
    CHECK(!released);
    xr_xir_program_drop(program); CHECK(released == 1);
    source_result_drop(&results[0]); source_result_drop(&results[1]);
    puts("VM to native and native to VM source calls shared instance state and ownership");
    CHECK(!runtime_live && !runtime_bytes);
    return 0;
}
