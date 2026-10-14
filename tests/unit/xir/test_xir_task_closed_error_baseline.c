/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_closed_error_baseline.c - Certified errors locate the runtime gap
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Task closed Error FAIL %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir_task_closed_error_pipeline.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_tuple.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_atomic.h"
#include "task_closed_error_generated.h"

enum { CLOSED_ERROR_FUNCTIONS = 128 };
typedef struct ClosedErrorFixture {
    XrXirProgram *program;
    XrXirArtifact *lowered;
    XrXirCallEntry entries[CLOSED_ERROR_FUNCTIONS];
    XrXirVmBinding bindings[CLOSED_ERROR_FUNCTIONS];
    uint32_t entry[CLOSED_ERROR_CASES][2];
    unsigned code_releases;
} ClosedErrorFixture;
static void closed_error_code_drop(void *owner) {
    ClosedErrorFixture *fixture = owner;
    CHECK(fixture->lowered && !fixture->code_releases++);
    xr_xir_compile_artifact_free(fixture->lowered); fixture->lowered = NULL;
}
static void closed_error_build(ClosedErrorFixture *fixture, const char *backend) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    fixture->lowered = closed_error_lower(context, fixture->entry);
    const XrXirModule *module = xr_xir_compile_artifact_module(fixture->lowered);
    const XrXirProgramSpec *native = &task_closed_error_native_program;
    CHECK(module->function_count <= CLOSED_ERROR_FUNCTIONS && module->function_count == native->entry_count);
    XrXirProgramProof proof = xr_xir_compile_program_proof(fixture->lowered);
    CHECK(proof.length == native->proof.length && !memcmp(proof.bytes, native->proof.bytes, proof.length));
    bool vm = !strcmp(backend, "vm"), all_native = !strcmp(backend, "native");
    bool root_native = !strcmp(backend, "native-vm");
    CHECK(vm || all_native || root_native || !strcmp(backend, "vm-native"));
    for (uint32_t f = 0; f < module->function_count; ++f) {
        bool worker = module->functions[f].name_length >= 7 && !memcmp(module->functions[f].name, "worker_", 7);
        bool use_native = all_native || (!vm && (worker != root_native));
        XrXirCallEntry entry = {0};
        CHECK(xr_xir_compile_vm_bind(fixture->lowered, f, &fixture->bindings[f], &entry) == XR_XIR_OK);
        fixture->entries[f] = use_native ? native->entries[f] : entry;
    }
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, native->target, fixture->entries, module->function_count,
        module->declarations, {fixture, closed_error_code_drop}, module->types, proof};
    CHECK(xr_xir_compile_program_seal(context, &spec, &fixture->program) == XR_XIR_OK);
    for (unsigned n = 0; n < CLOSED_ERROR_CASES; ++n) for (unsigned mode = 0; mode < 2; ++mode)
        CHECK(fixture->entry[n][mode] == closed_error_entries[n][mode]);
}
static void closed_error_task_field_gap(void) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,XR_TASK_CLOSED_ERROR_FIXTURES};
    XrXirSourceRequest request = {session,XR_TASK_CLOSED_ERROR_FIXTURES "/task-field.xr",&authority,
        context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request,&result,&diagnostic,&failure);
    if (status != XR_XIR_BAD_TYPE) fprintf(stderr,"Task field Source status=%u %s\n",status,diagnostic.message);
    CHECK(status == XR_XIR_BAD_TYPE);
    xr_compile_session_free(session);
    CHECK(!result.checked && !result.snapshot && !strcmp(diagnostic.message,"source declaration type structure is invalid"));
    xr_compile_resources_free(failure); xr_xir_compile_source_result_free(&result);
    effects_source_owners_free(); CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    printf("TASK_ERROR_TASK_FIELD_GAP Source=BAD_TYPE nominalFieldTaskMissing noArtifact physical=0/0\n");
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    if (!strcmp(argv[1],"source-task-field")) { closed_error_task_field_gap(); return 0; }
    CHECK(argc == 2); ClosedErrorFixture fixture = {0}; closed_error_build(&fixture, argv[1]);
    XrXirType problem = XR_XIR_UNIT;
    for (uint32_t t = 0; t < fixture.program->types->count; ++t) {
        const XrXirTypeNode *node = &fixture.program->types->nodes[t];
        if (node->kind != XR_XIR_TYPE_NOMINAL) continue;
        const XrXirNominalIdentity *identity = &fixture.program->types->nominals->identities[node->nominal.declaration];
        if (identity->name.length == 7 && !memcmp(identity->name.bytes, "Problem", 7)) {
            CHECK(problem == XR_XIR_UNIT && identity->variant_count == CLOSED_ERROR_CASES);
            problem = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
        }
    }
    CHECK(problem != XR_XIR_UNIT);
    unsigned workers = 0;
    for (uint32_t f = 0; f < fixture.program->entry_count; ++f) {
        const XrXirModule *module = xr_xir_compile_artifact_module(fixture.lowered);
        if (module->functions[f].name_length < 7 || memcmp(module->functions[f].name, "worker_", 7)) continue;
        CHECK(fixture.program->permissions->entries[f].worker == XR_XIR_OK);
        const XrXirEffects *effects = fixture.program->permissions->effects;
        CHECK(effects && !xr_xir_effects_error_unknown(effects, f) && !xr_xir_effects_error_unidentified(effects, f));
        unsigned family = CLOSED_ERROR_CASES;
        for (unsigned n = 0; n < CLOSED_ERROR_CASES; ++n)
            if (module->functions[f].name_length == 7+strlen(closed_error_names[n]) &&
                !memcmp(module->functions[f].name+7, closed_error_names[n], strlen(closed_error_names[n]))) family = n;
        CHECK(family < CLOSED_ERROR_CASES);
        for (unsigned v = 0; v < CLOSED_ERROR_CASES; ++v)
            CHECK(xr_xir_effects_error(effects, f, problem, v) == (family == v));
        ++workers;
    }
    CHECK(workers == CLOSED_ERROR_CASES);
    for (unsigned n = 0; n < CLOSED_ERROR_CASES; ++n) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        XrXirInstance *instance = NULL;
        CHECK(xr_xir_instance_new(fixture.program, &config, &instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance, fixture.entry[n][1], NULL, 0) == XR_XIR_CALL_READY);
        unsigned polls = 0; XrXirValue task = {0};
        while (!instance->executor->active) {
            CHECK(xr_xir_instance_poll_bounded(instance, 1).outcome.status == XR_XIR_CALL_READY && ++polls < 4096);
        }
        XrXirValue handle = task_core_task_value(instance->executor->active);
        CHECK(xr_xir_value_copy(&handle, &task) == XR_XIR_VALUE_OK);
        XrXirInstanceResult result = {0};
        do { result = xr_xir_instance_poll_bounded(instance, 1); CHECK(++polls < 4096); }
        while (result.outcome.status == XR_XIR_CALL_READY);
        CHECK(result.outcome.status == XR_XIR_CALL_UNSUPPORTED);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_UNSUPPORTED);
        XrXirCallResult outcome = {0};
        CHECK(xr_xir_task_copy_outcome(&task, &outcome) == XR_XIR_CALL_UNSUPPORTED);
        CHECK(!outcome.value.type && !outcome.value.payload && xr_xir_panic_empty(&outcome.panic));
        xr_xir_call_result_drop(&outcome); xr_xir_value_drop(&task);
        CHECK(!runtime_live && !runtime_bytes);
    }
    xr_xir_compile_program_drop(fixture.program); CHECK(fixture.code_releases == 1 && !fixture.lowered);
    effects_source_owners_free(); CHECK(!effects_compile_live && !effects_compile_bytes);
    printf("TASK_CLOSED_ERROR_GAP %s workers=%u Source/Checked73/Lowered/sealedWorker=OK runtime=UNSUPPORTED physical=0/0\n",argv[1],workers);
    return 0;
}
