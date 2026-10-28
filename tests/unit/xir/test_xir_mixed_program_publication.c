/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_program_publication.c - Late descriptor changes cannot publish code
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "publication %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void publication_allocated(void);
#define XIR_EFFECT_COMPILE_ALLOCATED() publication_allocated()
#include "xir_effect_execution_owner.h"
#undef XIR_EFFECT_COMPILE_ALLOCATED
#include "xir_runtime_allocations.h"
XR_DATA const XrXirProgramSpec library_counter_fixture_program;
XR_DATA const uint32_t library_counter_entry;

typedef struct PublicationCode { XrXirProgram *vm; XrXirCallEntry *entries; } PublicationCode;
typedef struct PublicationInput {
    XrXirCompileContext context;
    XrXirProgramSpec spec;
    PublicationCode *code;
} PublicationInput;
static XrXirProgramSpec *publication_mutation_spec;
static size_t publication_mutation_site;
static unsigned publication_mutation_kind, publication_releases;
static bool publication_armed, publication_injected;
static const char *const publication_backends[] = {"0", "1", "vm", "native"};
static void publication_allocated(void) {
    if (!publication_armed || effects_compile_attempts != publication_mutation_site + 1) return;
    CHECK(!publication_injected && publication_mutation_spec);
    XrXirCallEntry *entries = (XrXirCallEntry *)publication_mutation_spec->entries;
    XrXirCallEntry *entry = &entries[library_counter_entry];
    if (!publication_mutation_kind) entry->result = XR_XIR_BOOL;
    else if (publication_mutation_kind == 1) entry->flags = XR_XIR_ENTRY_EXIT;
    else { CHECK(publication_mutation_kind == 2); entry->cleanup_owner = 1; }
    publication_injected = true;
}
static void publication_code_free(void *pointer) {
    PublicationCode *code = pointer;
    xr_xir_compile_program_drop(code->vm); xr_compile_resources_free(code->entries);
    xr_compile_resources_free(code); ++publication_releases;
}
/* Native code and its packet were produced from the real two-module Source.
 * The independent packet copy dies before lowering; the VM owns its own image. */
static PublicationInput publication_input(unsigned backend) {
    PublicationInput input = {0};
    input.context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    size_t length = library_counter_fixture_program.proof.length; void *packet = NULL;
    CHECK(xr_compile_resources_alloc(input.context.resources, length, &packet) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_work(input.context.resources, length) == XR_COMPILE_RESOURCE_OK);
    memcpy(packet, library_counter_fixture_program.proof.bytes, length);
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&input.context, packet, length, &checked, NULL) == XR_XIR_OK);
    memset(packet, 0xA5, length); xr_compile_resources_free(packet);
    CHECK(xr_xir_compile_lower(checked, &library_counter_fixture_program.target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    CHECK(module->function_count == library_counter_fixture_program.entry_count);
    CHECK(xr_compile_resources_calloc(input.context.resources, 1, sizeof(*input.code),
        (void **)&input.code) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(input.context.resources, module->function_count,
        sizeof(*input.code->entries), (void **)&input.code->entries) == XR_COMPILE_RESOURCE_OK);
    XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
    CHECK(xr_xir_compile_vm_program_take(&lowered, &input.code->vm) == XR_XIR_OK && !lowered);
    unsigned vm = 0, native = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        bool root = module->declarations->functions[f].module == module->declarations->root_module;
        bool use_native = backend == 3 || (backend < 2 && root == (backend == 1));
        CHECK(library_counter_fixture_program.entries[f].resume != input.code->vm->entries[f].resume);
        input.code->entries[f] = use_native ? library_counter_fixture_program.entries[f] : input.code->vm->entries[f];
        if (use_native) ++native; else ++vm;
    }
    if (backend == 2) CHECK(vm == module->function_count && !native);
    else if (backend == 3) CHECK(native == module->function_count && !vm);
    else CHECK(vm && native);
    input.spec = library_counter_fixture_program; input.spec.entries = input.code->entries;
    input.spec.types = module->types; input.spec.declarations = module->declarations;
    input.spec.proof = proof; input.spec.code = (XrXirCodeLease){input.code, publication_code_free};
    const XrXirCallEntry *entry = &input.code->entries[library_counter_entry];
    CHECK(entry->result == XR_XIR_I64 && !entry->flags && !entry->cleanup_owner);
    return input;
}
static void publication_poll(XrXirInstance *instance, bool *done, unsigned *yields) {
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
/* The successful retry really executes both selected providers. Values remain
 * readable after both Instance owners, code images and compiler ledgers die. */
static void publication_execute(XrXirProgram *program) {
    XrXirInstanceConfig config; XrXirInstance *instances[2] = {0}; XrXirValue values[4] = {{0}};
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i)
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    CHECK(instances[0]->domain != instances[1]->domain);
    xr_xir_compile_program_drop(program); CHECK(!publication_releases);
    for (unsigned round = 0; round < 2; ++round) {
        bool done[2] = {0}; unsigned yields[2] = {0}, polls = 0;
        for (unsigned i = 0; i < 2; ++i)
            CHECK(xr_xir_instance_start(instances[i], library_counter_entry, NULL, 0) == XR_XIR_CALL_READY);
        while (!done[0] || !done[1]) for (unsigned i = 0; i < 2; ++i) {
            if (done[i]) continue;
            XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(instances[1-i]->domain);
            publication_poll(instances[i], &done[i], &yields[i]); CHECK(++polls < 4096);
            XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(instances[1-i]->domain);
            CHECK(before.work == after.work && before.requested_bytes == after.requested_bytes &&
                before.requested_call_bytes == after.requested_call_bytes);
        }
        for (unsigned i = 0; i < 2; ++i) {
            CHECK(yields[i] == 1);
            CHECK(xr_xir_instance_take_result(instances[i], &values[2*round+i]) == XR_XIR_CALL_RETURNED);
            CHECK(values[2*round+i].type == XR_XIR_I64 && values[2*round+i].payload == round + 1);
        }
    }
    CHECK(xr_xir_instance_free(instances[0]) == XR_XIR_CALL_READY && !publication_releases);
    CHECK(xr_xir_instance_free(instances[1]) == XR_XIR_CALL_READY && publication_releases == 1);
    CHECK(!runtime_live && !runtime_bytes); effects_source_owners_free();
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(values[i].type == XR_XIR_I64 && values[i].payload == i/2 + 1); xr_xir_value_drop(&values[i]);
    }
}
static size_t publication_case(unsigned backend, unsigned attack, size_t expected_sites) {
    publication_releases = 0; publication_armed = false; publication_injected = false;
    PublicationInput input = publication_input(backend); XrXirProgram *program = NULL;
    XrCompileResourceStats before = {0}, after = {0};
    CHECK(xr_compile_resources_stats(input.context.resources, &before) == XR_COMPILE_RESOURCE_OK);
    size_t live = effects_compile_live, bytes = effects_compile_bytes, begin = effects_compile_attempts;
    XrXirCallEntry original = input.code->entries[library_counter_entry];
    if (expected_sites) {
        publication_mutation_spec = &input.spec; publication_mutation_kind = attack;
        publication_mutation_site = begin + expected_sites - 1; publication_armed = true;
    }
    XrXirStatus status = xr_xir_compile_program_seal(&input.context, &input.spec, &program);
    size_t sites = effects_compile_attempts - begin; publication_armed = false; publication_mutation_spec = NULL;
    if (expected_sites) {
        CHECK(publication_injected && status == XR_XIR_BAD_STRUCTURE && !program && !publication_releases);
        CHECK(effects_compile_live == live && effects_compile_bytes == bytes && !effects_compile_injected);
        CHECK(xr_compile_resources_stats(input.context.resources, &after) == XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes == before.live_bytes && after.allocated_bytes > before.allocated_bytes && after.work > before.work);
        input.code->entries[library_counter_entry] = original;
        CHECK(xr_xir_compile_program_seal(&input.context, &input.spec, &program) == XR_XIR_OK && program);
        printf("publication backend=%s attack=%u site=%zu/%zu rejected=%u outputEmpty leaseUntaken physicalBaseline sameOwnerRetry\n",
            publication_backends[backend], attack, expected_sites - 1, expected_sites, (unsigned)status);
    } else CHECK(status == XR_XIR_OK && program && sites && sites < 20000);
    publication_execute(program);
    return sites;
}
int main(int argc, char **argv) {
    CHECK(argc == 2); unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], publication_backends[backend])) ++backend;
    CHECK(backend < 4);
    size_t sites = publication_case(backend, 0, 0);
    for (unsigned attack = 0; attack < 3; ++attack) (void)publication_case(backend, attack, sites);
    printf("publication backend=%s lastAllocation=%zu, three exact rejections, real providers and two independent1/2 Instances, lateScalars physical0/0\n",
        publication_backends[backend], sites - 1);
    return 0;
}
