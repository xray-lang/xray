/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_worker_controls.c - Independent Source worker results across real backends
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "worker control %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#define WORKER_DECLARE(n) XR_DATA const XrXirProgramSpec worker_control_##n##_program; XR_DATA const uint32_t worker_control_##n##_entry;
WORKER_DECLARE(0) WORKER_DECLARE(1) WORKER_DECLARE(2) WORKER_DECLARE(3)
WORKER_DECLARE(4) WORKER_DECLARE(5) WORKER_DECLARE(6)
#undef WORKER_DECLARE
typedef struct WorkerControlCase { const char *name; const XrXirProgramSpec *spec; const uint32_t *entry; } WorkerControlCase;
static const WorkerControlCase worker_control_cases[] = {
    {"pure-control", &worker_control_0_program, &worker_control_0_entry},
    {"local-worker-control", &worker_control_1_program, &worker_control_1_entry},
    {"snapshot-control", &worker_control_2_program, &worker_control_2_entry},
    {"generic-pure-control", &worker_control_3_program, &worker_control_3_entry},
    {"alias-pure-control", &worker_control_4_program, &worker_control_4_entry},
    {"cross-control", &worker_control_5_program, &worker_control_5_entry},
    {"mutual-control", &worker_control_6_program, &worker_control_6_entry}
};
enum { WORKER_FUNCTIONS = 128 };
typedef struct WorkerControlCode {
    XrXirProgram *vm; XrXirCallEntry *entries;
    XrXirResumeEntry delegates[WORKER_FUNCTIONS];
    bool native[WORKER_FUNCTIONS], workers[WORKER_FUNCTIONS];
    uint32_t functions, root; unsigned root_dispatches, worker_dispatches, vm_dispatches, native_dispatches;
} WorkerControlCode;
static WorkerControlCode *worker_control_code;
static unsigned worker_code_releases;
static const char *worker_backends[] = {"0", "1", "vm", "native"};
static XrXirAction worker_control_resume(XrXirCallView *view) {
    WorkerControlCode *code = worker_control_code;
    uint32_t entry = xr_xir_call_current_entry(view->activation);
    CHECK(code && entry < code->functions && code->delegates[entry]);
    if (entry == code->root) ++code->root_dispatches;
    if (code->workers[entry]) ++code->worker_dispatches;
    if (code->native[entry]) ++code->native_dispatches; else ++code->vm_dispatches;
    return code->delegates[entry](view);
}
static void worker_control_free(void *pointer) {
    WorkerControlCode *code = pointer; CHECK(code == worker_control_code); worker_control_code = NULL;
    xr_xir_compile_program_drop(code->vm); xr_compile_resources_free(code->entries); xr_compile_resources_free(code);
    ++worker_code_releases;
}
static void worker_control_targets(WorkerControlCode *code, const XrXirModule *module) {
    unsigned targets = 0;
    for (uint32_t f = 0; f < module->function_count; ++f)
        for (uint32_t i = 0; i < module->functions[f].instruction_count; ++i) {
            const XrXirInstruction *op = &module->functions[f].instructions[i];
            if (op->op != XR_XIR_GO) continue;
            CHECK(op->immediate >= 0 && (uint64_t)op->immediate < module->function_count);
            uint32_t target = (uint32_t)op->immediate;
            CHECK(target != code->root && xr_xir_function_has_go_role(module, target));
            code->workers[target] = true; ++targets;
        }
    CHECK(targets == 1);
}
static XrXirProgram *worker_control_program(const WorkerControlCase *oracle, unsigned backend) {
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, oracle->spec->proof.bytes, oracle->spec->proof.length, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked, &oracle->spec->target, &lowered, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(checked);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    CHECK(module->function_count == oracle->spec->entry_count && module->function_count <= WORKER_FUNCTIONS);
    WorkerControlCode *code = NULL; CHECK(!worker_control_code);
    CHECK(xr_compile_resources_calloc(context.resources, 1, sizeof(*code), (void **)&code) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(context.resources, module->function_count, sizeof(*code->entries), (void **)&code->entries) == XR_COMPILE_RESOURCE_OK);
    code->functions = module->function_count; code->root = *oracle->entry;
    worker_control_targets(code, module);
    XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
    CHECK(xr_xir_compile_vm_program_take(&lowered, &code->vm) == XR_XIR_OK && !lowered);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        code->native[f] = backend == 3 || (backend < 2 && (f == code->root) == (backend == 1));
        CHECK(oracle->spec->entries[f].resume != code->vm->entries[f].resume);
        code->entries[f] = code->native[f] ? oracle->spec->entries[f] : code->vm->entries[f];
        code->delegates[f] = code->entries[f].resume; code->entries[f].resume = worker_control_resume;
    }
    XrXirProgramSpec spec = *oracle->spec;
    spec.entries = code->entries; spec.types = module->types; spec.declarations = module->declarations;
    spec.proof = proof; spec.code = (XrXirCodeLease){code, worker_control_free}; worker_control_code = code;
    XrXirProgram *program = NULL; CHECK(xr_xir_compile_program_seal(&context, &spec, &program) == XR_XIR_OK);
    return program;
}
static void worker_control_execute(const WorkerControlCase *oracle, unsigned backend) {
    XrXirProgram *program = worker_control_program(oracle, backend); XrXirInstance *instances[2] = {0};
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program); CHECK(!worker_code_releases);
    XrXirValue retained[4] = {{0}};
    for (unsigned round = 0; round < 2; ++round) {
        bool done[2] = {0}; unsigned polls = 0;
        for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_instance_start(instances[i], *oracle->entry, NULL, 0) == XR_XIR_CALL_READY);
        while (!done[0] || !done[1]) for (unsigned i = 0; i < 2; ++i) {
            if (done[i]) continue;
            XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(instances[1-i]->domain);
            XrXirCallStatus status = xr_xir_instance_poll_bounded(instances[i], 1).outcome.status;
            if (status != XR_XIR_CALL_READY && status != XR_XIR_CALL_RETURNED) fprintf(stderr, "worker %s backend=%s status=%u\n", oracle->name, worker_backends[backend], status);
            CHECK(++polls < 32768 && (status == XR_XIR_CALL_READY || status == XR_XIR_CALL_RETURNED));
            done[i] = status == XR_XIR_CALL_RETURNED;
            XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(instances[1-i]->domain);
            CHECK(before.work == after.work && before.requested_bytes == after.requested_bytes && before.requested_call_bytes == after.requested_call_bytes);
        }
        for (unsigned i = 0; i < 2; ++i) {
            CHECK(xr_xir_instance_take_result(instances[i], &retained[2*round+i]) == XR_XIR_CALL_RETURNED);
            CHECK(retained[2*round+i].type == XR_XIR_I64 && retained[2*round+i].payload == 7);
        }
    }
    WorkerControlCode *code = worker_control_code;
    CHECK(code->root_dispatches >= 4 && code->worker_dispatches >= 4);
    if (backend == 2) CHECK(code->vm_dispatches && !code->native_dispatches);
    else if (backend == 3) CHECK(code->native_dispatches && !code->vm_dispatches);
    else CHECK(code->native_dispatches && code->vm_dispatches);
    printf("Worker control case=%s backend=%s actual root=%u worker=%u vm=%u native=%u, four independent7, peer costs unchanged\n",
        oracle->name, worker_backends[backend], code->root_dispatches, code->worker_dispatches, code->vm_dispatches, code->native_dispatches);
    CHECK(xr_xir_instance_free(instances[0]) == XR_XIR_CALL_READY && !worker_code_releases);
    CHECK(xr_xir_instance_free(instances[1]) == XR_XIR_CALL_READY && worker_code_releases == 1);
    for (unsigned i = 0; i < 4; ++i) { CHECK(retained[i].type == XR_XIR_I64 && retained[i].payload == 7); xr_xir_value_drop(&retained[i]); }
    CHECK(!runtime_live && !runtime_bytes); effects_source_owners_free();
}
static int worker_control_main(int argc, char **argv) {
    CHECK(argc == 3); const WorkerControlCase *oracle = NULL; unsigned backend = 0;
    for (unsigned i = 0; i < 7; ++i) if (!strcmp(argv[1], worker_control_cases[i].name)) oracle = &worker_control_cases[i];
    while (backend < 4 && strcmp(argv[2], worker_backends[backend])) ++backend;
    CHECK(oracle && backend < 4); worker_control_execute(oracle, backend); return 0;
}
#ifndef XR_WORKER_CONTROL_RESOURCE_MAIN
int main(int argc, char **argv) { return worker_control_main(argc, argv); }
#endif
