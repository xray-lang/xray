/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_wait_resource_fixture.h - Genuine provider selection and final code lease
 */
#ifndef XIR_GENERATOR_WAIT_RESOURCE_FIXTURE_H
#define XIR_GENERATOR_WAIT_RESOURCE_FIXTURE_H
XR_DATA const XrXirProgramSpec generator_wait_program;
XR_DATA const uint32_t generator_wait_ids[8];
typedef struct WaitResourceProgram {
    const XrXirProgramSpec *native;
    const uint32_t *ids;
    uint32_t factory,nth;
} WaitResourceProgram;
typedef struct WaitResourceFixture {
    const uint32_t *ids;
    uint32_t factory,nth;
    SourceFixtureOwner compiler;
    XrXirArtifact *lowered;
    XrXirVmBinding *bindings;
    XrXirCallEntry *vm;
    XrXirProgram *program;
    XrXirCallEntry entries[128];
    const XrXirCallEntry *selected[128];
    unsigned native[128],steps[2],code_releases;
} WaitResourceFixture;
static WaitResourceFixture *resource_fixture;
static XrXirAction resource_resume(XrXirCallView *view) {
    uint32_t entry=xr_xir_call_current_entry(view->activation);
    CHECK(resource_fixture && entry<128 && resource_fixture->selected[entry]);
    ++resource_fixture->steps[resource_fixture->native[entry]];
    return resource_fixture->selected[entry]->resume(view);
}
static void resource_code_release(void *owner) {
    WaitResourceFixture *fixture=owner;CHECK(!fixture->code_releases++);
    xr_compile_resources_free(fixture->vm);fixture->vm=NULL;
    xr_compile_resources_free(fixture->bindings);fixture->bindings=NULL;
    xr_xir_compile_artifact_free(fixture->lowered);fixture->lowered=NULL;
}
static void resource_fixture_new(WaitResourceFixture *fixture,const char *provider,const WaitResourceProgram *input) {
    CHECK(!resource_fixture);resource_fixture=fixture;source_fixture_owner_new(&fixture->compiler);
    const XrXirProgramSpec *native=input ? input->native : &generator_wait_program;
    fixture->ids=input ? input->ids : generator_wait_ids;
    fixture->factory=input ? input->factory : generator_wait_ids[0];
    fixture->nth=input ? input->nth : 0;
    CHECK(native->entry_count<=128);
    bool all_native=!strcmp(provider,"native"),all_vm=!strcmp(provider,"vm");
    bool root_native=!strcmp(provider,"native-vm");
    CHECK(all_native || all_vm || root_native || !strcmp(provider,"vm-native"));
    if(!all_native) {
        XrXirArtifact *checked=NULL,*closed=NULL;
        CHECK(xr_xir_compile_checked_read(&fixture->compiler.context,native->proof.bytes,native->proof.length,&checked,NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
        CHECK(xr_xir_compile_lower(closed,&native->target,&fixture->lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
        CHECK(xr_xir_compile_vm_bind_table(fixture->lowered,&fixture->bindings,&fixture->vm)==XR_XIR_OK);
    }
    for(uint32_t i=0;i<native->entry_count;++i) {
        bool producer=(native->entries[i].flags & XR_XIR_ENTRY_GENERATOR)!=0;
        bool use_native=all_native || (!all_vm && (producer ? !root_native : root_native));
        fixture->native[i]=use_native ? 1u : 0u;
        fixture->selected[i]=use_native ? &native->entries[i] : &fixture->vm[i];
        fixture->entries[i]=*fixture->selected[i];fixture->entries[i].resume=resource_resume;
    }
    XrXirProgramSpec spec=*native;spec.entries=fixture->entries;
    spec.code=(XrXirCodeLease){fixture,resource_code_release};
    CHECK(xr_xir_compile_program_seal(&fixture->compiler.context,&spec,&fixture->program)==XR_XIR_OK);
    CHECK(!runtime_live && !runtime_bytes);
}
static void resource_fixture_free(WaitResourceFixture *fixture) {
    xr_xir_compile_program_drop(fixture->program);fixture->program=NULL;
    CHECK(fixture->code_releases==1 && !fixture->lowered && !runtime_live && !runtime_bytes);
    source_fixture_owner_free(&fixture->compiler);resource_fixture=NULL;
}
#endif // XIR_GENERATOR_WAIT_RESOURCE_FIXTURE_H
