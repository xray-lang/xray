/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_diamond.c - Diamond initialization runs once in each independent Instance
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_call_observer_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "diamond %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
XR_DATA const XrXirProgramSpec diamond_fixture_program;
XR_DATA const uint32_t diamond_entry;
XR_DATA const uint32_t diamond_initializers[4];
enum { DIAMOND_FUNCTIONS = 64, DIAMOND_INSTANCES = 2 };
typedef struct DiamondCode DiamondCode;
typedef struct DiamondRecord {
    DiamondCode *code;
    XrXirInstance *instance;
    XrXirCallObserver observer;
    unsigned initializers[4], initializer_leaves[4], roots, vm_frames, native_frames;
} DiamondRecord;
struct DiamondCode {
    XrXirProgram *vm;
    XrXirCallEntry *entries;
    XrXirResumeEntry delegates[DIAMOND_FUNCTIONS];
    bool native[DIAMOND_FUNCTIONS];
    uint32_t functions;
    DiamondRecord records[DIAMOND_INSTANCES];
};
static DiamondCode *diamond_code;
static unsigned diamond_code_releases;
static XrXirCallStatus diamond_expected_fault;
static bool diamond_expected_cancellation;
static const char *diamond_backends[] = {"0", "1", "vm", "native"};
static void diamond_event(void *owner, const XrXirCallEvent *event) {
    DiamondRecord *record = owner;
    CHECK(event && event->instance == record->instance && event->entry <= record->code->functions);
    CHECK(event->binding.owner && event->binding.activation && event->binding.generation && event->binding.ticket && event->frame);
    if (event->kind == XR_XIR_CALL_FRAME_ENTER || event->kind == XR_XIR_CALL_FRAME_PRESENT) {
        for (unsigned i = 0; i < 4; ++i) if (event->entry == diamond_initializers[i]) {
            ++record->initializers[i]; CHECK(record->initializers[i] == 1);
        }
        if (event->entry == diamond_entry) { ++record->roots; CHECK(record->roots <= 2); }
    }
    if (event->kind == XR_XIR_CALL_FRAME_LEAVE && event->entry < record->code->functions) {
        for (unsigned i = 0; i < 4; ++i) if (event->entry == diamond_initializers[i]) {
            ++record->initializer_leaves[i]; CHECK(record->initializer_leaves[i] == record->initializers[i]);
        }
        CHECK(event->status == XR_XIR_CALL_RETURNED ||
            (diamond_expected_fault != XR_XIR_CALL_READY && event->status == diamond_expected_fault) ||
            (diamond_expected_cancellation && event->status == XR_XIR_CALL_CANCELLED));
        if (event->code == XR_XIR_CALL_CODE_UNKNOWN) {
            CHECK(event->status == diamond_expected_fault || (diamond_expected_cancellation && event->status == XR_XIR_CALL_CANCELLED));
            return;
        }
        CHECK(event->code == (record->code->native[event->entry] ? XR_XIR_CALL_CODE_NATIVE : XR_XIR_CALL_CODE_VM));
        if (event->code == XR_XIR_CALL_CODE_NATIVE) ++record->native_frames; else ++record->vm_frames;
    }
}
static XrXirAction diamond_resume(XrXirCallView *view) {
    DiamondRecord *record = NULL; CHECK(diamond_code);
    for (unsigned i = 0; i < DIAMOND_INSTANCES; ++i)
        if (diamond_code->records[i].instance == view->instance) record = &diamond_code->records[i];
    CHECK(record); uint32_t entry = xr_xir_call_current_entry(view->activation);
    CHECK(entry < diamond_code->functions);
    XrXirCallStatus bound = xr_xir_call_observer_bind(view, &record->observer);
    if (bound != XR_XIR_CALL_READY) {
        CHECK(bound == XR_XIR_CALL_LIMIT && diamond_expected_fault == XR_XIR_CALL_LIMIT);
        return xr_xir_call_fault(XR_XIR_RUN_FRAME_LIMIT);
    }
    return diamond_code->delegates[entry](view);
}
static void diamond_code_free(void *pointer) {
    DiamondCode *code = pointer; CHECK(code == diamond_code);
    xr_xir_compile_program_drop(code->vm); xr_compile_resources_free(code->entries);
    diamond_code = NULL; xr_compile_resources_free(code); ++diamond_code_releases;
}
static XrXirProgram *diamond_program(unsigned backend) {
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, diamond_fixture_program.proof.bytes,
        diamond_fixture_program.proof.length, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked, &diamond_fixture_program.target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    CHECK(module->function_count == diamond_fixture_program.entry_count && module->function_count <= DIAMOND_FUNCTIONS);
    CHECK(module->declarations->module_count == 4 && module->declarations->slot_count == 6);
    DiamondCode *code = NULL;
    CHECK(xr_compile_resources_calloc(context.resources, 1, sizeof(*code), (void **)&code) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(context.resources, module->function_count,
        sizeof(*code->entries), (void **)&code->entries) == XR_COMPILE_RESOURCE_OK);
    code->functions = module->function_count;
    XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
    CHECK(xr_xir_compile_vm_program_take(&lowered, &code->vm) == XR_XIR_OK && !lowered);
    for (uint32_t f = 0; f < code->functions; ++f) {
        bool root = module->declarations->functions[f].module == module->declarations->root_module;
        code->native[f] = backend == 3 || (backend < 2 && root == (backend == 1));
        CHECK(diamond_fixture_program.entries[f].resume != code->vm->entries[f].resume);
        code->entries[f] = code->native[f] ? diamond_fixture_program.entries[f] : code->vm->entries[f];
        code->delegates[f] = code->entries[f].resume; code->entries[f].resume = diamond_resume;
    }
    for (unsigned i = 0; i < DIAMOND_INSTANCES; ++i) {
        code->records[i].code = code;
        code->records[i].observer = (XrXirCallObserver){&code->records[i], diamond_event};
    }
    XrXirProgramSpec spec = diamond_fixture_program;
    spec.entries = code->entries; spec.types = module->types; spec.declarations = module->declarations;
    spec.proof = proof; spec.code = (XrXirCodeLease){code, diamond_code_free};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&context, &spec, &program) == XR_XIR_OK && program);
    diamond_code = code; return program;
}
static void diamond_value(const XrXirValue *value) {
    const char *text = NULL; size_t length = 0;
    CHECK(value->type == XR_XIR_STRING && xr_xir_string_view(value, &text, &length));
    CHECK(length == 4 && !memcmp(text, "SLRT", 4));
}
static bool diamond_once(const DiamondRecord *record) {
    return record->initializers[0] == 1 && record->initializers[1] == 1 &&
        record->initializers[2] == 1 && record->initializers[3] == 1 && record->roots == 2 &&
        record->initializer_leaves[0] == 1 && record->initializer_leaves[1] == 1 &&
        record->initializer_leaves[2] == 1 && record->initializer_leaves[3] == 1;
}
static void diamond_verify(const DiamondRecord *record, unsigned backend) {
    CHECK(diamond_once(record));
    for (unsigned i = 0; i < 4; ++i) {
        DiamondRecord bad = *record; bad.initializers[i] = 0; CHECK(!diamond_once(&bad));
        bad.initializers[i] = 2; CHECK(!diamond_once(&bad));
    }
    if (backend == 2) CHECK(record->vm_frames && !record->native_frames);
    else if (backend == 3) CHECK(record->native_frames && !record->vm_frames);
    else CHECK(record->vm_frames && record->native_frames);
}
static void diamond_round(DiamondCode *code, XrXirInstance **instances, XrXirValue *values) {
    bool done[2] = {0}; unsigned polls = 0;
    for (unsigned i = 0; i < 2; ++i)
        CHECK(xr_xir_instance_start(instances[i], diamond_entry, NULL, 0) == XR_XIR_CALL_READY);
    while (!done[0] || !done[1]) for (unsigned i = 0; i < 2; ++i) {
        if (done[i]) continue;
        DiamondRecord peer = code->records[1-i]; XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(instances[1-i]->domain);
        XrXirCallStatus status = xr_xir_instance_poll_bounded(instances[i], 1).outcome.status;
        XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(instances[1-i]->domain);
        CHECK(++polls < 4096 && !memcmp(&peer, &code->records[1-i], sizeof(peer)));
        CHECK(before.work == after.work && before.requested_bytes == after.requested_bytes &&
            before.requested_call_bytes == after.requested_call_bytes);
        CHECK(status == XR_XIR_CALL_READY || status == XR_XIR_CALL_RETURNED); done[i] = status == XR_XIR_CALL_RETURNED;
    }
    for (unsigned i = 0; i < 2; ++i) {
        XrXirValue occupied = {XR_XIR_I64,0,73}, before = occupied;
        CHECK(xr_xir_instance_take_result(instances[i], &occupied) == XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&occupied, &before, sizeof(before)));
        CHECK(xr_xir_instance_take_result(instances[i], &values[i]) == XR_XIR_CALL_RETURNED); diamond_value(&values[i]);
        XrXirValue empty = {0}; CHECK(xr_xir_instance_take_result(instances[i], &empty) == XR_XIR_CALL_BAD_STATE && !empty.type);
    }
}
static int diamond_normal_main(int argc, char **argv) {
    CHECK(argc == 2); unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], diamond_backends[backend])) ++backend;
    CHECK(backend < 4); XrXirProgram *program = diamond_program(backend); DiamondCode *code = diamond_code;
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instances[2] = {0}; XrXirDomain *domains[2] = {0}; XrXirValue retained[4] = {{0}};
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
        code->records[i].instance = instances[i]; domains[i] = instances[i]->domain; CHECK(xr_xir_domain_retain(domains[i]));
    }
    CHECK(domains[0] != domains[1]); xr_xir_compile_program_drop(program); CHECK(!diamond_code_releases);
    for (unsigned round = 0; round < 2; ++round) diamond_round(code, instances, &retained[2*round]);
    for (unsigned i = 0; i < 2; ++i) diamond_verify(&code->records[i], backend);
    CHECK(xr_xir_instance_free(instances[0]) == XR_XIR_CALL_READY && !diamond_code_releases);
    CHECK(xr_xir_instance_free(instances[1]) == XR_XIR_CALL_READY && diamond_code_releases == 1 && !diamond_code);
    for (unsigned i = 0; i < 4; ++i) { diamond_value(&retained[i]); xr_xir_value_drop(&retained[i]); }
    for (unsigned i = 0; i < 2; ++i) {
        XrXirDomainBudgetStats cost = xr_xir_domain_budget_stats(domains[i]);
        CHECK(!cost.call_live && !cost.metadata_live && cost.call_allocations == cost.call_frees && cost.metadata_allocations == cost.metadata_frees);
        CHECK(xr_xir_domain_stats(domains[i]).live_bytes == sizeof(*domains[i])); xr_xir_domain_drop(domains[i]);
    }
    CHECK(!runtime_live && !runtime_bytes); effects_source_owners_free();
    printf("Diamond backend=%s two Instances/two rounds: independentSLRT/count4/left12/right13, four init frames once perInstance/root twice, missing/duplicate oracle rejects, actual backend frames, peer unchanged, owned text after code lease1, physical0/0\n", diamond_backends[backend]);
    return 0;
}
#ifndef XR_DIAMOND_RESOURCE_MAIN
int main(int argc, char **argv) { return diamond_normal_main(argc, argv); }
#endif
