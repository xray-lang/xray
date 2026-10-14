/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_library_counter.c - Catalog Library private state stays Instance-local
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "library counter %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
XR_DATA const XrXirProgramSpec library_counter_fixture_program;
XR_DATA const uint32_t library_counter_entry;
typedef struct LibraryCounterCode { XrXirProgram *vm; XrXirCallEntry *entries; } LibraryCounterCode;
static unsigned counter_code_releases;
static const char *counter_backends[] = {"0", "1", "vm", "native"};
static void counter_code_free(void *pointer) {
    LibraryCounterCode *code = pointer;
    xr_xir_compile_program_drop(code->vm); xr_compile_resources_free(code->entries);
    xr_compile_resources_free(code); ++counter_code_releases;
}
static XrXirProgram *library_counter_program(unsigned backend) {
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, library_counter_fixture_program.proof.bytes,
        library_counter_fixture_program.proof.length, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked, &library_counter_fixture_program.target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    CHECK(module->function_count == library_counter_fixture_program.entry_count);
    LibraryCounterCode *code = NULL;
    CHECK(xr_compile_resources_calloc(context.resources, 1, sizeof(*code), (void **)&code) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(context.resources, module->function_count,
        sizeof(*code->entries), (void **)&code->entries) == XR_COMPILE_RESOURCE_OK);
    XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
    CHECK(xr_xir_compile_vm_program_take(&lowered, &code->vm) == XR_XIR_OK && !lowered);
    unsigned vm = 0, native = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        bool root = module->declarations->functions[f].module == module->declarations->root_module;
        bool use_native = backend == 3 || (backend < 2 && root == (backend == 1));
        CHECK(library_counter_fixture_program.entries[f].resume != code->vm->entries[f].resume);
        code->entries[f] = use_native ? library_counter_fixture_program.entries[f] : code->vm->entries[f];
        if (use_native) ++native; else ++vm;
    }
    if (backend == 2) CHECK(vm == module->function_count && !native);
    else if (backend == 3) CHECK(native == module->function_count && !vm);
    else CHECK(vm && native);
    XrXirProgramSpec spec = library_counter_fixture_program;
    spec.entries = code->entries; spec.types = module->types; spec.declarations = module->declarations;
    spec.proof = proof; spec.code = (XrXirCodeLease){code, counter_code_free};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&context, &spec, &program) == XR_XIR_OK && program);
    return program;
}
static void counter_poll(XrXirInstance *instance, bool *done, unsigned *yields) {
    XrXirInstanceResult event = xr_xir_instance_poll_bounded(instance, 1);
    if (event.outcome.status == XR_XIR_CALL_SUSPENDED) {
        CHECK(event.epoch && event.outcome.wake && !*yields); ++*yields;
        CHECK(xr_xir_instance_resume(instance, event.epoch + 1, event.outcome.wake) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_resume(instance, event.epoch, event.outcome.wake) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_resume(instance, event.epoch, event.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    } else {
        CHECK(event.outcome.status == XR_XIR_CALL_READY || event.outcome.status == XR_XIR_CALL_RETURNED);
        *done = event.outcome.status == XR_XIR_CALL_RETURNED;
    }
}
static int counter_normal_main(int argc, char **argv) {
    CHECK(argc == 2); unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], counter_backends[backend])) ++backend;
    CHECK(backend < 4);
    XrXirProgram *program = library_counter_program(backend);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instances[2] = {0}; XrXirValue retained[4] = {{0}};
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    CHECK(instances[0]->domain != instances[1]->domain);
    xr_xir_compile_program_drop(program); CHECK(!counter_code_releases);
    for (unsigned round = 0; round < 2; ++round) {
        bool done[2] = {0}; unsigned polls = 0, yields[2] = {0};
        for (unsigned i = 0; i < 2; ++i)
            CHECK(xr_xir_instance_start(instances[i], library_counter_entry, NULL, 0) == XR_XIR_CALL_READY);
        while (!done[0] || !done[1]) for (unsigned i = 0; i < 2; ++i) {
            if (done[i]) continue;
            XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(instances[1-i]->domain);
            counter_poll(instances[i], &done[i], &yields[i]); CHECK(++polls < 4096);
            XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(instances[1-i]->domain);
            CHECK(before.work == after.work && before.requested_bytes == after.requested_bytes &&
                before.requested_call_bytes == after.requested_call_bytes);
        }
        for (unsigned i = 0; i < 2; ++i) {
            XrXirValue occupied = {XR_XIR_I64, 0, 73}, before = occupied;
            CHECK(yields[i] == 1 && xr_xir_instance_take_result(instances[i], &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
            CHECK(!memcmp(&occupied, &before, sizeof(before)));
            CHECK(xr_xir_instance_take_result(instances[i], &retained[2*round+i]) == XR_XIR_CALL_RETURNED);
            CHECK(retained[2*round+i].type == XR_XIR_I64 && retained[2*round+i].payload == round + 1);
            XrXirValue empty = {0}; CHECK(xr_xir_instance_take_result(instances[i], &empty) == XR_XIR_CALL_BAD_STATE && !empty.type);
        }
    }
    CHECK(xr_xir_instance_free(instances[0]) == XR_XIR_CALL_READY && !counter_code_releases);
    CHECK(xr_xir_instance_free(instances[1]) == XR_XIR_CALL_READY && counter_code_releases == 1);
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(retained[i].type == XR_XIR_I64 && retained[i].payload == i/2 + 1); xr_xir_value_drop(&retained[i]);
    }
    CHECK(!runtime_live && !runtime_bytes); effects_source_owners_free();
    printf("Library counter backend=%s two Instances independently1/2, one real yield/activation, peer costs unchanged, code lease1, physical0/0\n", counter_backends[backend]);
    return 0;
}
int main(int argc, char **argv) { return counter_normal_main(argc, argv); }
