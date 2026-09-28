/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_float_mixed.c - Both directions of Float calls across VM/native
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_runtime_allocations.h"
#include "xir_source_float_cases.h"
#include "xir_source_float_pipeline.h"
XR_DATA const XrXirProgramSpec float_source_program;
XR_DATA const uint32_t float_source_functions[FLOAT_FUNCTION_COUNT];
typedef struct SourceFloatMixed {
    XrXirArtifact *artifact;
    XrXirCallEntry *entries;
    XrXirVmBinding *bindings;
} SourceFloatMixed;
static unsigned mixed_releases;
static void source_float_mixed_free(void *pointer) {
    SourceFloatMixed *owner = pointer;
    xr_xir_artifact_free(owner->artifact); xr_free(owner->entries); xr_free(owner->bindings);
    xr_free(owner); ++mixed_releases;
}
static void source_float_mixed(bool root_native) {
    FILE *file = fopen(XR_FLOAT_CHECKED_FIXTURE, "rb"); CHECK(file);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file); CHECK(size > 0 && size < 1048576 && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = xr_malloc((size_t) size); CHECK(bytes);
    CHECK(fread(bytes, 1, (size_t) size, file) == (size_t) size && fclose(file) == 0);
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_checked_read(bytes, (size_t) size, NULL, &checked, NULL) == XR_XIR_OK); xr_free(bytes);
    SourceFloatMixed *owner = xr_calloc(1, sizeof(*owner)); CHECK(owner);
    owner->artifact = source_float_lower(checked);
    const XrXirModule *module = xr_xir_artifact_module(owner->artifact);
    CHECK(module->function_count == float_source_program.entry_count);
    uint32_t functions[FLOAT_FUNCTION_COUNT]; source_float_find(module, functions);
    CHECK(!memcmp(functions, float_source_functions, sizeof(functions)));
    owner->entries = xr_calloc(module->function_count, sizeof(*owner->entries));
    owner->bindings = xr_calloc(module->function_count, sizeof(*owner->bindings));
    CHECK(owner->entries && owner->bindings);
    unsigned native = 0, vm = 0;
    for (uint32_t i = 0; i < module->function_count; ++i) {
        CHECK(xr_xir_vm_bind(owner->artifact, i, &owner->bindings[i], &owner->entries[i]) == XR_XIR_OK);
        bool root = module->declarations->functions[i].module == module->declarations->root_module;
        if (root == root_native) { owner->entries[i] = float_source_program.entries[i]; ++native; }
        else ++vm;
    }
    CHECK(native && vm);
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
        owner->entries, module->function_count, module->declarations, {owner, source_float_mixed_free}, module->types, xr_xir_program_proof(owner->artifact)};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&spec, (XrXirProgramBudget) {2097152, 16000000}, &program) == XR_XIR_OK);
    source_float_program_cases(program, functions);
}
int main(void) {
    source_float_mixed(false); CHECK(mixed_releases == 1);
    source_float_mixed(true); CHECK(mixed_releases == 2);
    puts("Float source VM-to-native and native-to-VM calls passed the same independent expectations");
    return 0;
}
