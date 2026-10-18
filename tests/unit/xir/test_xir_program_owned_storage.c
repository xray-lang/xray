/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_program_owned_storage.c - Reject unreadable and unreleasable owners
 *
 * KEY CONCEPT:
 *   Failed reception preserves the same owners for recovery on the same ledger.
 */
#define XR_OWNED_ENTRIES_FIXTURE_ONLY
#include "test_xir_program_owned_entries.c"
#undef XR_OWNED_ENTRIES_FIXTURE_ONLY

static XrXirCallEntry *storage_entries, *storage_short;
static XrXirCallEntry **storage_slot;
static XrXirProgramSpec *storage_spec;
static bool storage_table_attack;
static void corrupt_owned_storage(void) {
    if (storage_table_attack) {
        *storage_slot=storage_short;
        storage_spec->entries=storage_short;
    } else storage_entries[2].parameters=(const XrXirType *)(uintptr_t)1;
}
static int late_storage(const char *mode) {
    const XrCompileResourceLimits caps={67108864,8388608,128000000};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirProgramInput *input=NULL;XrXirCallEntry *entries=NULL;XrXirVmBinding bindings[3];
    XrXirProgramSpec spec={0};XrXirProgram *program=NULL;
    receive_parts(&context,&input,&entries,bindings,&spec);
    probe_artifact=input->artifact;probe_attack=19;
    probe_allocations=strstr(mode,"alloc")!=NULL;probe_at=SIZE_MAX;probe_calls=0;probe_injected=false;probe_armed=true;
    CHECK(xir_program_receive(&context,&spec,&input,&entries,&program)==XR_XIR_OK && !input && !entries);
    probe_armed=false;size_t calls=probe_calls;CHECK(calls && execute(program)==42);
    xr_xir_compile_program_drop(program);program=NULL;
    receive_parts(&context,&input,&entries,bindings,&spec);
    XrXirProgramInput *original=input;storage_entries=entries;
    const XrXirType *parameters=entries[2].parameters;
    storage_slot=&entries;storage_spec=&spec;storage_table_attack=strstr(mode,"table")!=NULL;
    XrXirStatus allocated=XR_XIR_OK;
    storage_short=xir_compile_copy(&context,entries,sizeof(*entries),&allocated);CHECK(allocated==XR_XIR_OK);
    probe_artifact=input->artifact;probe_storage_callback=corrupt_owned_storage;
    probe_at=calls-1;probe_calls=0;probe_injected=false;probe_armed=true;
    XrXirStatus status=xir_program_receive(&context,&spec,&input,&entries,&program);
    probe_armed=false;probe_storage_callback=NULL;
    CHECK(probe_injected && status==XR_XIR_BAD_STRUCTURE && !program && input==original);
    CHECK(xr_compile_resources_allocation(context.resources,storage_entries,3*sizeof(*storage_entries))==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_allocation(context.resources,parameters,sizeof(*parameters))==XR_COMPILE_RESOURCE_OK);
    entries=storage_entries;spec.entries=entries;entries[2].parameters=parameters;
    CHECK(xir_program_receive(&context,&spec,&input,&entries,&program)==XR_XIR_OK && !input && !entries);
    CHECK(execute(program)==42);xr_xir_compile_program_drop(program);xr_compile_resources_free(storage_short);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==before.live_bytes);
    xr_compile_resources_release(context.resources);
    printf("%s: last callback rejected; original owners recovered42; physical baseline\n",mode);
    return 0;
}

int main(int argc,char **argv) {
    CHECK(argc==2);const char *mode=argv[1];
    if (!strncmp(mode,"late-",5)) return late_storage(mode);
    const XrCompileResourceLimits caps={67108864,8388608,128000000};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirProgramInput *input=NULL;XrXirCallEntry *entries=NULL;XrXirVmBinding bindings[3];XrXirProgramSpec spec={0};XrXirProgram *program=NULL;
    receive_parts(&context,&input,&entries,bindings,&spec);
    XrXirProgramInput *real_input=input;XrXirArtifact *real_artifact=input->artifact;
    XrXirCallEntry *real_entries=entries;const XrXirType *real_parameters=entries[2].parameters;
    XrCompileResources *foreign=NULL;void *storage=NULL;XrXirStatus allocated=XR_XIR_OK;
    if(!strcmp(mode,"table-short")) {
        storage=xir_compile_copy(&context,entries,sizeof(*entries),&allocated);
        CHECK(allocated==XR_XIR_OK);entries=storage;spec.entries=entries;
    } else if(!strcmp(mode,"table-interior")) {
        storage=xir_compile_calloc(&context,4,sizeof(*entries),&allocated);CHECK(allocated==XR_XIR_OK);
        entries=(XrXirCallEntry *)storage+1;memcpy(entries,real_entries,3*sizeof(*entries));spec.entries=entries;
    } else if(!strcmp(mode,"parameter-pointer")) entries[2].parameters=(const XrXirType *)(uintptr_t)1;
    else if(!strcmp(mode,"parameter-foreign")) {
        CHECK(xr_compile_resources_new(&caps,&foreign)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_resources_alloc(foreign,sizeof(*real_parameters),&storage)==XR_COMPILE_RESOURCE_OK);
        memcpy(storage,real_parameters,sizeof(*real_parameters));entries[2].parameters=storage;
    } else if(!strcmp(mode,"parameter-interior")) {
        storage=xir_compile_calloc(&context,2,sizeof(*real_parameters),&allocated);CHECK(allocated==XR_XIR_OK);
        XrXirType *p=(XrXirType *)storage+1;*p=*real_parameters;entries[2].parameters=p;
    } else if(!strcmp(mode,"zero-count-alias")) entries[0].parameters=real_parameters;
    else if(!strcmp(mode,"input-interior")) {
        size_t padding=_Alignof(XrXirProgramInput);
        storage=xir_compile_calloc(&context,1,padding+sizeof(*input),&allocated);CHECK(allocated==XR_XIR_OK);
        input=(XrXirProgramInput *)((char *)storage+padding);*input=*real_input;
    } else if(!strcmp(mode,"artifact-interior")) {
        size_t padding=_Alignof(XrXirArtifact);
        storage=xir_compile_calloc(&context,1,padding+sizeof(*real_artifact),&allocated);CHECK(allocated==XR_XIR_OK);
        XrXirArtifact *p=(XrXirArtifact *)((char *)storage+padding);*p=*real_artifact;input->artifact=p;
    } else CHECK(!strcmp(mode,"valid"));
    XrXirProgramInput *physical_input=input;XrXirCallEntry *physical_entries=entries;
    XrCompileResourceStats pre_receive={0},post_receive={0};CHECK(xr_compile_resources_stats(context.resources,&pre_receive)==XR_COMPILE_RESOURCE_OK);
    XrXirStatus status=xir_program_receive(&context,&spec,&input,&entries,&program);
    CHECK(xr_compile_resources_stats(context.resources,&post_receive)==XR_COMPILE_RESOURCE_OK);
    bool valid=!strcmp(mode,"valid");bool refused=status==XR_XIR_BAD_STRUCTURE && !program && input==physical_input && entries==physical_entries;
    bool no_allocation=pre_receive.allocation_count==post_receive.allocation_count && pre_receive.allocated_bytes==post_receive.allocated_bytes && pre_receive.live_bytes==post_receive.live_bytes;
    printf("mode=%s status=%u program=%u input_preserved=%u entries_preserved=%u no_allocation=%u independent_refusal=%u\n",mode,status,program!=NULL,input==physical_input,entries==physical_entries,no_allocation,refused);fflush(stdout);
    real_entries[0].parameters=NULL;real_entries[2].parameters=real_parameters;real_input->artifact=real_artifact;
    if(program) {
        program->entries=real_entries;program->permissions->input=real_input;
        CHECK(execute(program)==42);xr_xir_compile_program_drop(program);
    } else {
        input=real_input;entries=real_entries;spec.entries=entries;
        CHECK(xir_program_receive(&context,&spec,&input,&entries,&program)==XR_XIR_OK && !input && !entries);
        CHECK(execute(program)==42);xr_xir_compile_program_drop(program);
    }
    xr_compile_resources_free(storage);xr_compile_resources_release(foreign);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==before.live_bytes);
    xr_compile_resources_release(context.resources);puts("recovery42 and physical baseline reached without refill");
    return valid?(status==XR_XIR_OK?0:1):(refused && no_allocation?0:1);
}
