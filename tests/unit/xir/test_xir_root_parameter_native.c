/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_parameter_native.c - Real native and mixed high-order Programs
 *
 * KEY CONCEPT:
 *   Authentic bodies preserve their active view while observations count edges.
 */
#include "xir/xxir_program.h"
#include "xir/xxir_output.h"
#include "xir/xxir_vm.h"
#ifdef H1_MIXED
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#endif
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"H1 native/mixed %d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
#include "xir_root_parameter_execution_cases.h"
#include "root_parameter_native_generated.h"

typedef struct H1NativeOwner H1NativeOwner;
typedef struct H1NativeBinding {
    XrXirVmBinding vm;
    XrXirCallEntry actual;
    H1NativeOwner *owner;
    uint32_t function;
    bool native;
} H1NativeBinding;
struct H1NativeOwner {
    XrXirArtifact *lowered;
    XrXirCallEntry *entries;
    H1NativeBinding *bindings;
    uint32_t count;
    unsigned code_releases;
    uintptr_t instance_ids[2]; /* Sampled during a live provider callback. */
    uint64_t steps[2][2], releases[2][2], crossings[2];
};

static unsigned h1n_instance(H1NativeOwner *owner, XrXirInstance *instance) {
    CHECK(owner && instance);
    const uintptr_t identity = (uintptr_t)instance;
    for (unsigned i = 0; i < 2; ++i) {
        if (owner->instance_ids[i] == identity) return i;
        if (!owner->instance_ids[i]) { owner->instance_ids[i] = identity; return i; }
    }
    CHECK(false); return 0;
}

static XrXirAction h1n_resume(XrXirCallView *view) {
    CHECK(view && view->environment && xr_xir_call_admission(view));
    H1NativeBinding *binding = (H1NativeBinding *)view->environment;
    H1NativeOwner *owner = binding->owner;
    CHECK(owner && binding == &owner->bindings[binding->function] && !owner->code_releases);
    unsigned instance = h1n_instance(owner,view->instance);
    ++owner->steps[instance][binding->native];
    const void *environment = view->environment;
    XrXirAction action = binding->actual.resume(view);
    CHECK(view->environment == environment && xr_xir_call_admission(view));
    if (action.kind == XR_XIR_ACTION_CALL) {
        CHECK(action.callee < owner->count);
        if (binding->native != owner->bindings[action.callee].native) ++owner->crossings[instance];
    }
    return action;
}

static void h1n_release(XrXirCallView *view, XrXirCallStatus reason) {
    CHECK(view && view->environment && !xr_xir_call_admission(view));
    H1NativeBinding *binding = (H1NativeBinding *)view->environment;
    H1NativeOwner *owner = binding->owner;
    unsigned instance = h1n_instance(owner,view->instance);
    ++owner->releases[instance][binding->native];
    const void *environment = view->environment;
    binding->actual.release(view,reason);
    CHECK(view->environment == environment);
}

static void h1n_code_drop(void *opaque) {
    H1NativeOwner *owner = opaque;
    CHECK(owner && !owner->code_releases && owner->entries && owner->bindings);
    xr_xir_compile_artifact_free(owner->lowered); owner->lowered = NULL;
    xr_compile_resources_free(owner->entries); owner->entries = NULL;
    xr_compile_resources_free(owner->bindings); owner->bindings = NULL;
    ++owner->code_releases;
}

static H1ExecFixture h1n_program(const XrXirProgramSpec *native, uint32_t run,
    const RootParameterSourceOracle *oracle, H1NativeOwner *owner, bool run_native) {
    instance_compile_zero(); CHECK(!runtime_live && !runtime_bytes);
    const XrCompileResourceLimits caps = {UINT64_C(67108864),UINT64_C(8388608),UINT64_C(128000000)};
    XrXirCompileContext context = {0};
    CHECK(xr_compile_resources_new(&caps,&context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    CHECK(native && native->entry_count && run < native->entry_count);
    CHECK(!native->code.owner && !native->code.release);
    CHECK(native->declarations && native->declarations->module_count == 1);
    CHECK(native->declarations->slot_count == (oracle->root ? 1u : 0u));
    XrXirProgramSpec spec = *native;
#ifdef H1_MIXED
    XrXirArtifact *checked = NULL, *closed = NULL;
    CHECK(xr_xir_compile_checked_read(&context,native->proof.bytes,native->proof.length,&checked,NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_artifact_verify(closed,NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(closed,&packet,NULL) == XR_XIR_OK);
    CHECK(packet.length == native->proof.length && !memcmp(packet.bytes,native->proof.bytes,packet.length));
    memset(packet.bytes,0xa5,packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_lower(closed,&native->target,&owner->lowered,NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_verify(owner->lowered,NULL) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(owner->lowered);
    CHECK(module && module->function_count == native->entry_count);
    spec.proof = xr_xir_compile_program_proof(owner->lowered);
    CHECK(spec.proof.length == native->proof.length && !memcmp(spec.proof.bytes,native->proof.bytes,spec.proof.length));
    CHECK(!memcmp(spec.proof.identity,native->proof.identity,32));
    spec.declarations = module->declarations; spec.types = module->types;
#else
    CHECK(run_native);
#endif
    owner->count = native->entry_count;
    void *entries = NULL, *bindings = NULL;
    CHECK(xr_compile_resources_calloc(context.resources,owner->count,sizeof(*owner->entries),&entries) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(context.resources,owner->count,sizeof(*owner->bindings),&bindings) == XR_COMPILE_RESOURCE_OK);
    owner->entries = entries; owner->bindings = bindings;
    for (uint32_t i = 0; i < owner->count; ++i) {
        H1NativeBinding *binding = &owner->bindings[i];
        binding->owner = owner; binding->function = i; binding->native = true;
#ifdef H1_MIXED
        binding->native = i == run ? run_native : !run_native;
        if (!binding->native) {
            CHECK(xr_xir_compile_vm_bind(owner->lowered,i,&binding->vm,&binding->actual) == XR_XIR_OK);
            CHECK(binding->actual.environment == &binding->vm);
        } else
#endif
        {
            CHECK(!native->entries[i].environment);
            binding->actual = native->entries[i];
        }
        CHECK(binding->actual.resume && binding->actual.release);
        owner->entries[i] = binding->actual;
        owner->entries[i].resume = h1n_resume; owner->entries[i].release = h1n_release;
        owner->entries[i].environment = &binding->vm;
    }
    spec.entries = owner->entries; spec.code = (XrXirCodeLease){owner,h1n_code_drop};
    H1ExecFixture fixture = {NULL,spec.declarations->entry_function,run,
        spec.declarations->root_module,spec.declarations->slot_count};
    CHECK(xr_xir_compile_program_seal(&context,&spec,&fixture.program) == XR_XIR_OK);
    CHECK(fixture.program && fixture.program->permissions);
    const XrXirProgramPermission *permission = &fixture.program->permissions->entries[run];
    CHECK(permission->requires_root == oracle->root && permission->unresolved == oracle->unresolved);
    xr_compile_resources_release(context.resources);
    return fixture;
}

int main(int argc, char **argv) {
    CHECK(argc == 3);
    const XrXirProgramSpec *programs[] = {&h1native_0_program,&h1native_1_program,&h1native_2_program,&h1native_3_program};
    const uint32_t runs[] = {h1native_0_run,h1native_1_run,h1native_2_run,h1native_3_run};
    size_t index = SIZE_MAX;
    for (size_t i = 0; i < 4; ++i) if (!strcmp(argv[1],rps_oracles[i].file)) index = i;
    CHECK(index != SIZE_MAX);
    bool run_native = true;
#ifdef H1_MIXED
    run_native = !strcmp(argv[2],"native-vm");
    CHECK(run_native || !strcmp(argv[2],"vm-native"));
#else
    CHECK(!strcmp(argv[2],"native"));
#endif
    H1NativeOwner owner = {0};
    h1exec_pair(&rps_oracles[index],h1n_program(programs[index],runs[index],&rps_oracles[index],&owner,run_native));
    CHECK(owner.code_releases == 1 && !owner.lowered && !owner.entries && !owner.bindings);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(owner.steps[i][1] && owner.releases[i][1]);
#ifdef H1_MIXED
        CHECK(owner.steps[i][0] && owner.releases[i][0] && owner.crossings[i]);
#else
        CHECK(!owner.steps[i][0] && !owner.releases[i][0] && !owner.crossings[i]);
#endif
        printf("H1_PROVIDER file=%s mode=%s instance=%u nativeSteps=%llu vmSteps=%llu nativeReleases=%llu vmReleases=%llu crossings=%llu\n",
            argv[1],argv[2],i,(unsigned long long)owner.steps[i][1],(unsigned long long)owner.steps[i][0],
            (unsigned long long)owner.releases[i][1],(unsigned long long)owner.releases[i][0],(unsigned long long)owner.crossings[i]);
    }
    CHECK(!runtime_live && !runtime_bytes); instance_compile_zero();
    printf("H1_NATIVE_DONE file=%s mode=%s independentI64=%lld outputBytes=2 twoInstances=1 codeReleases=1 physical=0/0\n",
        argv[1],argv[2],(long long)rps_oracles[index].result);
    return 0;
}
