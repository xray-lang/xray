/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_string_bytes_native.c - One packet across native and mixed consumers
 *
 * KEY CONCEPT:
 *   Both providers consume the same retired producer's function identities.
 */
#define main string_bytes_driver_main
#include "test_xir_string_bytes.c"
#undef main

extern const XrXirProgramSpec string_bytes_program;
typedef struct StringBytesMixedOwner StringBytesMixedOwner;
typedef struct StringBytesMixedBinding {
    XrXirVmBinding vm;
    XrXirCallEntry actual;
    StringBytesMixedOwner *owner;
    bool native;
} StringBytesMixedBinding;
struct StringBytesMixedOwner {
    XrXirArtifact *code;
    XrXirCallEntry *entries;
    StringBytesMixedBinding *bindings;
    uint32_t count;
    bool native_to_vm,vm_to_native;
};
static XrXirAction string_bytes_mixed_resume(XrXirCallView *view) {
    CHECK(view && view->environment && xr_xir_call_admission(view));
    StringBytesMixedBinding *binding=(StringBytesMixedBinding *)view->environment;
    StringBytesMixedOwner *owner=binding->owner;
    XrXirAction action=binding->actual.resume(view);
    if (action.kind==XR_XIR_ACTION_CALL) {
        CHECK(action.callee<owner->count);
        bool callee_native=owner->bindings[action.callee].native;
        if (binding->native && !callee_native) owner->native_to_vm=true;
        if (!binding->native && callee_native) owner->vm_to_native=true;
    }
    return action;
}
static void string_bytes_mixed_release(XrXirCallView *view,XrXirCallStatus reason) {
    CHECK(view && view->environment && !xr_xir_call_admission(view));
    StringBytesMixedBinding *binding=(StringBytesMixedBinding *)view->environment;
    binding->actual.release(view,reason);
}
static void string_bytes_mixed_drop(void *opaque) {
    StringBytesMixedOwner *owner=opaque;
    xr_free(owner->entries);
    xr_free(owner->bindings);
    xr_xir_compile_artifact_free(owner->code);
    xr_free(owner);
}
static XrXirProgram *string_bytes_mixed(StringBytesFixture *fixture,bool native_callers,
    StringBytesMixedOwner **owner_out) {
    StringBytesMixedOwner *owner=xr_calloc(1,sizeof(*owner));
    CHECK(owner);
    owner->code=string_bytes_lowered(fixture);
    const XrXirModule *module=xr_xir_compile_artifact_module(owner->code);
    owner->count=module->function_count;
    CHECK(owner->count==string_bytes_program.entry_count);
    owner->entries=xr_calloc(owner->count,sizeof(*owner->entries));
    owner->bindings=xr_calloc(owner->count,sizeof(*owner->bindings));
    CHECK(owner->entries && owner->bindings);
    for (uint32_t i=0;i<owner->count;++i) {
        StringBytesMixedBinding *binding=owner->bindings+i;
        bool caller=i==fixture->functions.printable || i==fixture->functions.c_string ||
            i==fixture->functions.ref_panic;
        binding->native=caller==native_callers;
        binding->owner=owner;
        if (binding->native) binding->actual=string_bytes_program.entries[i];
        else CHECK(xr_xir_compile_vm_bind(owner->code,i,&binding->vm,&binding->actual)==XR_XIR_OK);
        owner->entries[i]=binding->actual;
        owner->entries[i].environment=binding;
        owner->entries[i].resume=string_bytes_mixed_resume;
        owner->entries[i].release=string_bytes_mixed_release;
    }
    XrXirProgramSpec spec=string_bytes_program;
    spec.entries=owner->entries;
    spec.code=(XrXirCodeLease){owner,string_bytes_mixed_drop};
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&fixture->context,&spec,&program)==XR_XIR_OK);
    *owner_out=owner;
    return program;
}
int main(int argc,char **argv) {
    CHECK(argc==3);
    bool native=!strcmp(argv[1],"--native");
    bool native_vm=!strcmp(argv[1],"--native-vm");
    bool vm_native=!strcmp(argv[1],"--vm-native");
    CHECK(native || native_vm || vm_native);
    StringBytesFixture fixture=string_bytes_packet_fixture(argv[2]);
    StringBytesFunctions functions=fixture.functions;
    XrXirProgram *program=NULL;
    StringBytesMixedOwner *owner=NULL;
    if (native) CHECK(xr_xir_compile_program_seal(&fixture.context,&string_bytes_program,&program)==XR_XIR_OK);
    else program=string_bytes_mixed(&fixture,native_vm,&owner);
    string_bytes_fixture_free(&fixture);
    string_bytes_run(program,&functions);
    if (native_vm) CHECK(owner->native_to_vm);
    if (vm_native) CHECK(owner->vm_to_native);
    xr_xir_compile_program_drop(program);
    puts("governed string bytes: native or actual mixed calls after packet retirement PASS");
    return 0;
}
