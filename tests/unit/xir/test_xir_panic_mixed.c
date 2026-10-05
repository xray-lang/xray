/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_panic_mixed.c - Panic unwinding across both VM/native call directions
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_panic_cases.h"
XR_DATA const XrXirProgramSpec panic_source_program;
XR_DATA const XrXirProgramSpec panic_bad_program;
XR_DATA const uint32_t panic_source_functions[PANIC_FUNCTIONS];
typedef struct PanicMixed {
    XrXirArtifact *artifact;
    XrXirCallEntry *entries;
    XrXirVmBinding *bindings;
} PanicMixed;
static unsigned releases;
static void panic_mixed_free(void *pointer) {
    PanicMixed *owner = pointer;
    xr_xir_compile_artifact_free(owner->artifact); xr_compile_resources_free(owner->entries); xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner); ++releases;
}
static void panic_mixed(bool root_native) {
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrXirArtifact *checked = NULL, *closed = NULL;
    CHECK(xr_xir_compile_checked_read(&context, panic_source_program.proof.bytes, panic_source_program.proof.length,
        &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    PanicMixed *owner = NULL;
    CHECK(xr_compile_resources_calloc(context.resources, 1, sizeof(*owner), (void **)&owner) == XR_COMPILE_RESOURCE_OK && owner);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &owner->artifact, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *module = xr_xir_compile_artifact_module(owner->artifact);
    CHECK(module->function_count == panic_source_program.entry_count);
    CHECK(xr_compile_resources_calloc(context.resources, module->function_count, sizeof(*owner->entries), (void **)&owner->entries) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(context.resources, module->function_count, sizeof(*owner->bindings), (void **)&owner->bindings) == XR_COMPILE_RESOURCE_OK);
    CHECK(owner->entries && owner->bindings);
    unsigned native = 0, vm = 0;
    for (uint32_t i = 0; i < module->function_count; ++i) {
        CHECK(xr_xir_compile_vm_bind(owner->artifact, i, &owner->bindings[i], &owner->entries[i]) == XR_XIR_OK);
        bool root = module->declarations->functions[i].module == module->declarations->root_module;
        if (root == root_native) { owner->entries[i] = panic_source_program.entries[i]; ++native; }
        else ++vm;
    }
    CHECK(native && vm);
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, target, owner->entries, module->function_count,
        module->declarations, {owner, panic_mixed_free}, module->types, xr_xir_compile_program_proof(owner->artifact)};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&context, &spec, &program) == XR_XIR_OK);
    panic_cases(program, panic_source_functions);
}
int main(void) {
    panic_mixed(false); CHECK(releases == 1);
    panic_mixed(true); CHECK(releases == 2);
    const XrXirCompileContext bad_context = *effects_source_owner(UINT64_C(16)*1024*1024, UINT64_C(64000000));
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&bad_context, &panic_bad_program, &program) == XR_XIR_OK);
    panic_sticky(program, panic_bad_program.declarations->entry_function);
    effects_source_owners_free();
    puts("Both panic VM/native directions matched independent values and release expectations");
    return 0;
}
