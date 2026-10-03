/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_array_mixed.c - Both directions of Array calls across VM/native
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_source_array_cases.h"
#include "xir_source_array_reordering_cases.h"
#include "xir_source_array_pipeline.h"
XR_DATA const XrXirProgramSpec array_source_program;
XR_DATA const XrXirProgramSpec array_reorder_program;
XR_DATA const uint32_t array_reorder_functions[REORDER_FUNCTION_COUNT];
XR_DATA const XrXirProgramSpec array_bounds_program;
XR_DATA const uint32_t array_source_functions[ARRAY_FUNCTION_COUNT];
typedef struct SourceArrayMixed {
    XrXirArtifact *artifact;
    XrXirCallEntry *entries;
    XrXirVmBinding *bindings;
} SourceArrayMixed;
static unsigned mixed_releases;
static void source_array_mixed_free(void *pointer) {
    SourceArrayMixed *owner = pointer;
    xr_xir_compile_artifact_free(owner->artifact); xr_compile_resources_free(owner->entries); xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner); ++mixed_releases;
}
static void source_array_mixed(bool root_native, const char *packet_path,
    const XrXirProgramSpec *native_program, const uint32_t *native_functions, unsigned function_count,
    void (*find)(const XrXirModule *, uint32_t *), void (*run_cases)(XrXirProgram *, const uint32_t *)) {
    SourceFixtureOwner compiler = {0}; source_fixture_owner_new(&compiler);
    FILE *file = fopen(packet_path, "rb"); CHECK(file);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file); CHECK(size > 0 && size < 1048576 && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = xr_malloc((size_t) size); CHECK(bytes);
    CHECK(fread(bytes, 1, (size_t) size, file) == (size_t) size && fclose(file) == 0);
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_compile_checked_read(&compiler.context, bytes, (size_t) size, &checked, NULL) == XR_XIR_OK); xr_free(bytes);
    SourceArrayMixed *owner = NULL;
    CHECK(xr_compile_resources_calloc(compiler.context.resources, 1, sizeof(*owner), (void **) &owner) == XR_COMPILE_RESOURCE_OK);
    owner->artifact = source_array_lower(checked);
    const XrXirModule *module = xr_xir_compile_artifact_module(owner->artifact);
    CHECK(module->function_count == native_program->entry_count && function_count <= ARRAY_FUNCTION_COUNT);
    XrXirProgramProof proof = xr_xir_compile_program_proof(owner->artifact);
    CHECK(proof.identity && native_program->proof.identity && proof.length == native_program->proof.length);
    CHECK(!memcmp(proof.identity, native_program->proof.identity, 32));
    CHECK(!memcmp(proof.bytes, native_program->proof.bytes, proof.length));
    uint32_t functions[ARRAY_FUNCTION_COUNT]; find(module, functions);
    CHECK(!memcmp(functions, native_functions, function_count * sizeof(functions[0])));
    CHECK(xr_compile_resources_calloc(compiler.context.resources, module->function_count,
        sizeof(*owner->entries), (void **) &owner->entries) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(compiler.context.resources, module->function_count,
        sizeof(*owner->bindings), (void **) &owner->bindings) == XR_COMPILE_RESOURCE_OK);
    CHECK(owner->entries && owner->bindings);
    unsigned native = 0, vm = 0;
    for (uint32_t i = 0; i < module->function_count; ++i) {
        CHECK(xr_xir_compile_vm_bind(owner->artifact, i, &owner->bindings[i], &owner->entries[i]) == XR_XIR_OK);
        bool root = module->declarations->functions[i].module == module->declarations->root_module;
        if (root == root_native) { owner->entries[i] = native_program->entries[i]; ++native; }
        else ++vm;
    }
    CHECK(native && vm);
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
        owner->entries, module->function_count, module->declarations, {owner, source_array_mixed_free}, module->types, xr_xir_compile_program_proof(owner->artifact)};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&compiler.context, &spec, &program) == XR_XIR_OK);
    run_cases(program, functions); program = NULL;
    source_fixture_owner_free(&compiler);
}
int main(void) {
    SourceFixtureOwner compiler = {0}; source_fixture_owner_new(&compiler);
    source_array_mixed(false, XR_ARRAY_CHECKED_FIXTURE, &array_source_program, array_source_functions,
        ARRAY_FUNCTION_COUNT, source_array_find, source_array_program_cases); CHECK(mixed_releases == 1);
    source_array_mixed(true, XR_ARRAY_CHECKED_FIXTURE, &array_source_program, array_source_functions,
        ARRAY_FUNCTION_COUNT, source_array_find, source_array_program_cases); CHECK(mixed_releases == 2);
    source_array_mixed(false, XR_ARRAY_REORDER_CHECKED_FIXTURE, &array_reorder_program, array_reorder_functions,
        REORDER_FUNCTION_COUNT, source_array_reorder_find, source_array_reordering_program_cases); CHECK(mixed_releases == 3);
    source_array_mixed(true, XR_ARRAY_REORDER_CHECKED_FIXTURE, &array_reorder_program, array_reorder_functions,
        REORDER_FUNCTION_COUNT, source_array_reorder_find, source_array_reordering_program_cases); CHECK(mixed_releases == 4);
    XrXirProgram *bounds = NULL;
    CHECK(xr_xir_compile_program_seal(&compiler.context, &array_bounds_program, &bounds) == XR_XIR_OK);
    source_array_sticky_bounds(bounds, array_bounds_program.declarations->entry_function); bounds = NULL;
    source_fixture_owner_free(&compiler);
    puts("Array source VM-to-native and native-to-VM calls passed the same independent expectations");
    return 0;
}
