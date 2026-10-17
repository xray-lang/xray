/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_program_input_carrier.c - Owning transport grants no execution permission
 */
#define XR_OWNED_ENTRIES_FIXTURE_ONLY
#include "test_xir_program_owned_entries.c"
#undef XR_OWNED_ENTRIES_FIXTURE_ONLY

int main(int argc,char **argv) {
    CHECK(argc==2);const char *mode=argv[1];
    bool semantic=!strcmp(mode,"semantic"),body=!strcmp(mode,"body"),layout=!strcmp(mode,"layout");
    CHECK(semantic || body || layout || !strcmp(mode,"valid"));
    const XrCompileResourceLimits caps={67108864,8388608,128000000};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0},before={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirProgramInput *input=NULL;XrXirCallEntry *entries=NULL;XrXirVmBinding bindings[3];
    XrXirProgramSpec spec={0};XrXirProgram *program=NULL;
    receive_parts(&context,&input,&entries,bindings,&spec);
    XrXirArtifact *artifact=input->artifact;input->artifact=NULL;xir_program_input_free(input);input=NULL;
    XrXirArtifact *original=artifact;XrXirCallEntry *original_entries=entries;
    XrXirInstruction *instruction=(XrXirInstruction *)artifact->module.functions[1].instructions;
    int64_t immediate=instruction[0].immediate;uint32_t frame=artifact->layouts[1].frame_bytes;
    if(semantic){probe_artifact=artifact;probe_attack=10;final_callback(10);}
    if(body)instruction[0].immediate=43;
    if(layout)++artifact->layouts[1].frame_bytes;
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirStatus status=xir_program_input_take(&context,&artifact,&input);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.allocation_count==before.allocation_count+1);
    if(body || layout) {
        CHECK(status==XR_XIR_BAD_STRUCTURE && artifact==original && !input && !program);
        instruction[0].immediate=immediate;original->layouts[1].frame_bytes=frame;
        CHECK(xir_program_input_take(&context,&artifact,&input)==XR_XIR_OK && !artifact);
    } else CHECK(status==XR_XIR_OK && !artifact && input && !program);
    if(semantic) {
        XrXirProgramInput *original_input=input;
        status=xir_program_receive(&context,&spec,&input,&entries,&program);
        CHECK(status==XR_XIR_BAD_STRUCTURE && input==original_input && entries==original_entries && !program);
        CHECK(xr_compile_resources_allocation(context.resources,entries,3*sizeof(*entries))==XR_COMPILE_RESOURCE_OK);
        final_restore();
    }
    CHECK(xir_program_receive(&context,&spec,&input,&entries,&program)==XR_XIR_OK && !input && !entries);
    CHECK(execute(program)==42);xr_xir_compile_program_drop(program);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    printf("carrier %s: physical shape checked, no semantic authority; same-ledger recovery42 and physical baseline\n",mode);
    return 0;
}
