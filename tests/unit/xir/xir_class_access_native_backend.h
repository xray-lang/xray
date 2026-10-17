/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_access_native_backend.h - Authentic native and mixed Program ownership
 */
#ifndef XIR_CLASS_ACCESS_NATIVE_BACKEND_H
#define XIR_CLASS_ACCESS_NATIVE_BACKEND_H
typedef struct AccessGeneratedFixture {
    const XrXirProgramSpec *program;
    const uint8_t *native;
    CellSourceEntries entries;
    uint32_t count;
} AccessGeneratedFixture;
#include "xir/xxir_program_packet_internal.h"
#include "class_access_generated.h"

typedef struct AccessMixedBinding {
    XrXirVmBinding vm;
    XrXirCallEntry actual;
    bool native;
} AccessMixedBinding;
typedef struct AccessNativeOwner {
    XrXirArtifact *lowered;
    XrXirVmBinding *bindings;
    XrXirCallEntry *entries;
    AccessMixedBinding *mixed;
    uint32_t count;
    uint64_t steps[2], calls, returns, inner;
} AccessNativeOwner;
static AccessNativeOwner access_native_owner;

#if XR_CLASS_ACCESS_BACKEND==2
static const AccessMixedBinding *access_mixed_binding(const void *environment) {
    uintptr_t first=(uintptr_t)access_native_owner.mixed,at=(uintptr_t)environment;
    size_t bytes=(size_t)access_native_owner.count*sizeof(*access_native_owner.mixed);
    CHECK(first && at>=first && at-first<bytes && (at-first)%sizeof(*access_native_owner.mixed)==0);
    return environment;
}
static XrXirAction access_mixed_resume(XrXirCallView *view) {
    const AccessMixedBinding *binding=access_mixed_binding(view->environment);
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    CHECK(admission && admission->function==admit_function_binding &&
        admission->context==view->instance && view->activation->top);
    ++access_native_owner.steps[binding->native];
    XrXirAction action=binding->actual.resume(view);CHECK(view->environment==binding);
    if (action.kind==XR_XIR_ACTION_CALL) {
        CHECK(action.callee<access_native_owner.count);
        if (binding->native!=access_native_owner.mixed[action.callee].native) ++access_native_owner.calls;
    } else if (action.kind==XR_XIR_ACTION_RETURN && view->activation->top->parent) {
        const CallFrame *parent=view->activation->top->parent;
        uintptr_t first=(uintptr_t)access_native_owner.mixed,at=(uintptr_t)parent->entry->environment;
        size_t bytes=(size_t)access_native_owner.count*sizeof(*access_native_owner.mixed);
        if (at>=first && at-first<bytes && (at-first)%sizeof(*access_native_owner.mixed)==0) {
            const AccessMixedBinding *caller=access_mixed_binding(parent->entry->environment);
            if (binding->native!=caller->native) ++access_native_owner.returns;
        } else CHECK(parent->entry->resume==initialization_resume);
    }
    uint32_t function=(uint32_t)(binding-access_native_owner.mixed);
    const XrXirModule *module=xr_xir_compile_artifact_module(access_native_owner.lowered);
    const XrXirFunction *body=&module->functions[function];
    if (body->name_length==5 && !memcmp(body->name,"inner",5)) ++access_native_owner.inner;
    return action;
}
static void access_mixed_release(XrXirCallView *view,XrXirCallStatus reason) {
    const AccessMixedBinding *binding=access_mixed_binding(view->environment);
    binding->actual.release(view,reason);CHECK(view->environment==binding);
}
static void access_mixed_prepare(const XrXirCompileContext *context,const AccessGeneratedFixture *generated,
    XrXirProgramSpec *spec) {
    XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_checked_read(context,generated->program->packet.bytes,
        generated->program->packet.length,&checked,NULL)==XR_XIR_OK && checked);
    CHECK(xr_xir_compile_lower(checked,&generated->program->target,&access_native_owner.lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *module=xr_xir_compile_artifact_module(access_native_owner.lowered);
    CHECK(module && module->function_count==generated->count);
    CHECK(cell_source_entry(module,"run",true)==generated->entries.run &&
        cell_source_entry(module,"cellCounter",false)==generated->entries.counter &&
        cell_source_entry(module,"cellFactory",false)==generated->entries.factory);
    CHECK(xr_xir_compile_vm_bind_table(access_native_owner.lowered,&access_native_owner.bindings,
        &access_native_owner.entries)==XR_XIR_OK && access_native_owner.bindings && access_native_owner.entries);
    void *memory=NULL;
    CHECK(xr_compile_resources_calloc(context->resources,module->function_count,sizeof(AccessMixedBinding),
        &memory)==XR_COMPILE_RESOURCE_OK && memory);
    access_native_owner.mixed=memory;access_native_owner.count=module->function_count;
    for (uint32_t f=0;f<module->function_count;++f) {
        AccessMixedBinding *binding=&access_native_owner.mixed[f];
        const XrXirFunction *function=&module->functions[f];
        bool named=(function->name_length==4 && !memcmp(function->name,"bump",4)) ||
            (function->name_length==6 && !memcmp(function->name,"adjust",6)) ||
            (function->name_length==5 && !memcmp(function->name,"inner",5)) ||
            (function->name_length==11 && !memcmp(function->name,"cellCounter",11));
        CHECK(generated->native[f]==(unsigned)named);
        binding->native=named;
        if (named) {
            binding->actual=generated->program->entries[f];CHECK(!binding->actual.environment);
        } else {
            binding->vm=access_native_owner.bindings[f];binding->actual=access_native_owner.entries[f];
            CHECK(binding->vm.artifact==access_native_owner.lowered && binding->vm.function==f &&
                binding->actual.environment==&access_native_owner.bindings[f]);
            binding->actual.environment=&binding->vm;
        }
        access_native_owner.entries[f]=binding->actual;
        access_native_owner.entries[f].environment=binding;
        access_native_owner.entries[f].resume=access_mixed_resume;
        access_native_owner.entries[f].release=access_mixed_release;
    }
    spec->entries=access_native_owner.entries;spec->entry_count=module->function_count;
    spec->types=module->types;spec->declarations=module->declarations;
    spec->packet=xir_program_packet_borrow(access_native_owner.lowered);
}
#endif
static XrXirStatus access_native_program(const XrXirCompileContext *context,const char *name,
    XrXirProgram **output,CellSourceEntries *entries,bool unit) {
    CHECK(!unit && output && !*output && !access_native_owner.lowered && !access_native_owner.entries);
    unsigned i=0;while (i<5 && strcmp(name,access_source_cases[i].name)) ++i;
    CHECK(i<5 && !access_source_cases[i].rejected);
    const AccessGeneratedFixture *generated=&access_generated_fixtures[i];
    CHECK(generated->program && generated->native && generated->count==generated->program->entry_count);
    CHECK(generated->entries.run<generated->count && generated->entries.factory==UINT32_MAX);
    XrXirProgramSpec spec=*generated->program;
#if XR_CLASS_ACCESS_BACKEND==2
    access_mixed_prepare(context,generated,&spec);
#endif
    XrXirStatus status=xr_xir_compile_program_seal(context,&spec,output);
    CHECK(status==XR_XIR_OK && *output);*entries=generated->entries;return status;
}
static void access_native_drop(void) {
    /* Program disposal precedes every borrowed binding and Lowered owner. */
    xr_compile_resources_free(access_native_owner.entries);
    xr_compile_resources_free(access_native_owner.bindings);
    xr_compile_resources_free(access_native_owner.mixed);
    xr_xir_compile_artifact_free(access_native_owner.lowered);
    access_native_owner=(AccessNativeOwner){0};
}
static void access_native_trial_begin(void) {
    access_native_owner.steps[0]=access_native_owner.steps[1]=0;
    access_native_owner.calls=access_native_owner.returns=access_native_owner.inner=0;
}
static void access_native_trial_end(const AccessSourceCase *fixture,unsigned mode) {
#if XR_CLASS_ACCESS_BACKEND==2
    CHECK(access_native_owner.steps[0] && access_native_owner.steps[1] && access_native_owner.calls);
    if (fixture->alias) CHECK(!access_native_owner.inner);
    else CHECK(access_native_owner.returns);
#endif
    printf("CLASS_ACCESS_BACKEND fixture=%s mode=%u backend=%u VMsteps=%llu nativeSteps=%llu calls=%llu returns=%llu innerSteps=%llu\n",
        fixture->name,mode,XR_CLASS_ACCESS_BACKEND,(unsigned long long)access_native_owner.steps[0],
        (unsigned long long)access_native_owner.steps[1],(unsigned long long)access_native_owner.calls,
        (unsigned long long)access_native_owner.returns,(unsigned long long)access_native_owner.inner);
}
#define XR_CLASS_ACCESS_PROGRAM_NEW access_native_program
#define XR_CLASS_ACCESS_PROGRAM_DROP() access_native_drop()
#define XR_CLASS_ACCESS_TRIAL_BEGIN() access_native_trial_begin()
#define XR_CLASS_ACCESS_TRIAL_END(fixture,mode) access_native_trial_end(fixture,mode)
#endif // XIR_CLASS_ACCESS_NATIVE_BACKEND_H
