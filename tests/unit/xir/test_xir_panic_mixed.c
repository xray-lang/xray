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
    xr_xir_artifact_free(owner->artifact); xr_free(owner->entries); xr_free(owner->bindings);
    xr_free(owner); ++releases;
}
static void panic_mixed(bool root_native) {
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_checked_read(panic_source_program.proof.bytes, panic_source_program.proof.length,
        NULL, &checked, NULL) == XR_XIR_OK);
    PanicMixed *owner = xr_calloc(1, sizeof(*owner)); CHECK(owner);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(checked, &target, NULL, &owner->artifact, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirModule *module = xr_xir_artifact_module(owner->artifact);
    CHECK(module->function_count == panic_source_program.entry_count);
    owner->entries = xr_calloc(module->function_count, sizeof(*owner->entries));
    owner->bindings = xr_calloc(module->function_count, sizeof(*owner->bindings));
    CHECK(owner->entries && owner->bindings);
    unsigned native = 0, vm = 0;
    for (uint32_t i = 0; i < module->function_count; ++i) {
        CHECK(xr_xir_vm_bind(owner->artifact, i, &owner->bindings[i], &owner->entries[i]) == XR_XIR_OK);
        bool root = module->declarations->functions[i].module == module->declarations->root_module;
        if (root == root_native) { owner->entries[i] = panic_source_program.entries[i]; ++native; }
        else ++vm;
    }
    CHECK(native && vm);
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, target, owner->entries, module->function_count,
        module->declarations, {owner, panic_mixed_free}, module->types, xr_xir_program_proof(owner->artifact)};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&spec, (XrXirProgramBudget) {16777216, 64000000}, &program) == XR_XIR_OK);
    panic_cases(program, panic_source_functions);
}
int main(void) {
    panic_mixed(false); CHECK(releases == 1);
    panic_mixed(true); CHECK(releases == 2);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&panic_bad_program,
        (XrXirProgramBudget) {16777216, 64000000}, &program) == XR_XIR_OK);
    panic_sticky(program, panic_bad_program.declarations->entry_function);
    puts("Both panic VM/native directions matched independent values and release expectations");
    return 0;
}
