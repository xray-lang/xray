/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_language_native.c - Generated source execution with independent results
 *
 * KEY CONCEPT:
 *   Mixed selection must contain a real caller-to-callee edge, and both entry
 *   directions retain the same checked proof rather than a saved verdict.
 */
#include "xir/xxir_program_packet_internal.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "../xir_source_fixture_owner.h"
XR_DATA const XrXirProgramSpec language_reference_program;

static bool answer_function(const XrXirFunction *function) {
    return function->name_length == 15 && !memcmp(function->name, "referenceAnswer", 15);
}

static void native_pair(XrXirProgram *program, uint32_t entry, int64_t expected, unsigned suspensions) {
    XrXirInstance *instances[2] = {NULL, NULL}; XrXirValue results[2] = {{0}, {0}};
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    for (uint32_t i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instances[i], entry, NULL, 0) == XR_XIR_CALL_READY);
    }
    for (uint32_t i = 0; i < 2; ++i) {
        unsigned resumed = 0;
        XrXirInstanceResult observed = xr_xir_instance_poll_bounded(instances[i], UINT64_MAX);
        while (observed.outcome.status == XR_XIR_CALL_SUSPENDED) {
            CHECK(resumed++ < suspensions);
            CHECK(xr_xir_instance_resume(instances[i], observed.epoch, observed.outcome.wake) == XR_XIR_CALL_READY);
            observed = xr_xir_instance_poll_bounded(instances[i], UINT64_MAX);
        }
        CHECK(resumed == suspensions);
        XrXirCallResult result = observed.outcome;
        if (result.status != XR_XIR_CALL_RETURNED) fprintf(stderr, "instance=%u status=%u\n", i, (unsigned)result.status);
        CHECK(result.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instances[i], &results[i]) == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for (uint32_t i = 0; i < 2; ++i) {
        CHECK(results[i].type == XR_XIR_I64 && results[i].payload == expected);
        xr_xir_value_drop(&results[i]);
    }
}

static void execute_profile(int64_t expected, bool direction, unsigned suspensions) {
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    XrXirProgramSpec spec = language_reference_program;
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context, spec.packet.bytes, spec.packet.length, &checked, NULL) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(checked);
    CHECK(module->function_count == spec.entry_count);
    uint32_t answer = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f)
        if (answer_function(&module->functions[f])) { CHECK(answer == UINT32_MAX); answer = f; }
    CHECK(answer != UINT32_MAX);
    XrXirCallEntry *entries = NULL; XrXirVmBinding *bindings = NULL;
    if (XR_LANGUAGE_MIXED) {
        CHECK(xr_xir_compile_lower(checked, &spec.target, &lowered, NULL) == XR_XIR_OK);
        CHECK(xr_compile_resources_calloc(owner.context.resources, spec.entry_count, sizeof(*entries), (void **)&entries) == XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_resources_calloc(owner.context.resources, spec.entry_count, sizeof(*bindings), (void **)&bindings) == XR_COMPILE_RESOURCE_OK);
        module = xr_xir_compile_artifact_module(lowered);
        uint32_t vm = 0, native = 0, crossing = 0;
        for (uint32_t f = 0; f < spec.entry_count; ++f) {
            CHECK(xr_xir_compile_vm_bind(lowered, f, &bindings[f], &entries[f]) == XR_XIR_OK);
            bool use_vm = f == answer ? direction : !direction;
            if (use_vm) { CHECK(entries[f].resume != spec.entries[f].resume); ++vm; }
            else { entries[f] = spec.entries[f]; ++native; }
            const XrXirFunction *function = &module->functions[f];
            for (uint32_t i = 0; i < function->instruction_count; ++i) {
                const XrXirInstruction *instruction = &function->instructions[i];
                if (instruction->op == XR_XIR_CALL) {
                    CHECK(instruction->immediate < spec.entry_count);
                    bool callee_vm = instruction->immediate == answer ? direction : !direction;
                    crossing += use_vm != callee_vm;
                }
            }
        }
        CHECK(vm && native && crossing);
        spec.entries = entries; spec.types = module->types; spec.declarations = module->declarations;
        XrXirProgramPacket matching = xir_program_packet_borrow(lowered);
        CHECK(matching.identity && matching.length == spec.packet.length &&
              !memcmp(matching.identity, spec.packet.identity, 32) && !memcmp(matching.bytes, spec.packet.bytes, matching.length));
        spec.packet = matching;
        printf("mixed direction=%u vm=%u native=%u direct_cross_edges=%u\n", (unsigned)direction, vm, native, crossing);
    }
    xr_xir_compile_artifact_free(checked);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context, &spec, &program) == XR_XIR_OK);
    native_pair(program, answer, expected, suspensions);
    xr_xir_compile_artifact_free(lowered);
    xr_compile_resources_free(bindings); xr_compile_resources_free(entries);
    source_fixture_owner_free(&owner);
}

int main(int argc, char **argv) {
    CHECK(argc == 2 || argc == 3); char *end = NULL; int64_t expected = (int64_t)strtoll(argv[1], &end, 10);
    CHECK(end && end != argv[1] && !*end);
    unsigned suspensions = 0;
    if (argc == 3) {
        char *count_end = NULL;
        unsigned long long count = strtoull(argv[2], &count_end, 10);
        CHECK(*argv[2] && *argv[2] != '-' && count_end && !*count_end && count <= UINT32_MAX);
        suspensions = (unsigned)count;
    }
    execute_profile(expected, false, suspensions);
    if (XR_LANGUAGE_MIXED) execute_profile(expected, true, suspensions);
    printf("generated source independent result=%lld; mixed=%u\n", (long long)expected, (unsigned)XR_LANGUAGE_MIXED);
    return 0;
}
