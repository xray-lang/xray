/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_library_state_task_i64_execution.c - Same program private state on every backend
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#if CONSUMER_KIND!=3
#include "base/xsha256.c"
#endif
#include "xir_library_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
#include "xir/xxir_specialize.c"
#include "xir/xxir_effects.c"
#include "xir/xxir_vm.c"
#if CONSUMER_KIND==0
#include "xir/xxir_emit_c.c"
#endif
#if CONSUMER_KIND==3
#include "xir/xxir_library_catalog.c"
#endif
#define XR_LIBRARY_STATE_TASK_I64_RUNTIME_IMPLEMENTATION
#include "xir_library_state_task_i64_runtime.h"
#if CONSUMER_KIND==1 || CONSUMER_KIND==2
extern const XrXirProgramSpec library_state_task_i64_program;
extern const uint32_t library_state_task_i64_export_indices[LIBRARY_STATE_TASK_I64_EXPORTS];
#endif
#if CONSUMER_KIND==2
typedef struct LibraryStateTaskI64MixedBinding {
    XrXirVmBinding vm;
    XrXirCallEntry actual;
    bool native;
} LibraryStateTaskI64MixedBinding;
static LibraryStateTaskI64MixedBinding library_state_task_i64_mixed[128];
static uint32_t library_state_task_i64_mixed_count;
static uint64_t library_state_task_i64_steps[2],library_state_task_i64_crossings;
static XrXirAction library_state_task_i64_mixed_resume(XrXirCallView *view) {
    LibraryStateTaskI64MixedBinding *binding=(LibraryStateTaskI64MixedBinding *)view->environment;
    CHECK(binding>=library_state_task_i64_mixed && binding<library_state_task_i64_mixed+library_state_task_i64_mixed_count);
    ++library_state_task_i64_steps[binding->native];
    XrXirAction action=binding->actual.resume(view);
    library_state_task_i64_capture(&action);
    CHECK(view->environment==binding);
    if (action.kind==XR_XIR_ACTION_CALL) {
        CHECK(action.callee<library_state_task_i64_mixed_count);
        if (binding->native!=library_state_task_i64_mixed[action.callee].native) ++library_state_task_i64_crossings;
    }
    return action;
}
static void library_state_task_i64_mixed_release(XrXirCallView *view,XrXirCallStatus reason) {
    LibraryStateTaskI64MixedBinding *binding=(LibraryStateTaskI64MixedBinding *)view->environment;
    CHECK(binding>=library_state_task_i64_mixed && binding<library_state_task_i64_mixed+library_state_task_i64_mixed_count);
    binding->actual.release(view,reason);CHECK(view->environment==binding);
}
#endif
#if CONSUMER_KIND==0 || CONSUMER_KIND==3
static XrXirStatus library_state_task_i64_pipeline_operation(const XrXirCompileContext *context,void *opaque) {
    const XrXirCheckedPacket *packet=opaque;
    XrXirArtifact *checked=NULL,*special=NULL,*lowered=NULL;XrXirProgram *program=NULL;
    XrXirStatus status=xr_xir_compile_checked_read(context,packet->bytes,packet->length,&checked,NULL);
    if (status!=XR_XIR_OK) {CHECK(!checked);goto done;}
    status=xr_xir_compile_specialize(checked,&special,NULL);
    if (status!=XR_XIR_OK) {CHECK(!special);goto done;}
    status=xr_xir_compile_artifact_verify(special,NULL);if (status!=XR_XIR_OK) goto done;
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    status=xr_xir_compile_lower(special,&target,&lowered,NULL);
    if (status!=XR_XIR_OK) {CHECK(!lowered);goto done;}
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);CHECK(module->function_count<=128);
    XrXirCallEntry entries[128];XrXirVmBinding bindings[128];
    for (uint32_t f=0;f<module->function_count;++f) {
        status=xr_xir_compile_vm_bind(lowered,f,&bindings[f],&entries[f]);if (status!=XR_XIR_OK) goto done;
    }
    XrXirProgramSpec spec={.abi_version=XR_XIR_PROGRAM_ABI_VERSION,.target=target,
        .entries=entries,.entry_count=module->function_count,.types=module->types,
        .declarations=module->declarations,.proof=xr_xir_compile_program_proof(lowered)};
    status=xr_xir_compile_program_seal(context,&spec,&program);
    CHECK(status==XR_XIR_OK ? program!=NULL : program==NULL);
done:
    xr_xir_compile_program_drop(program);xr_xir_compile_artifact_free(lowered);
    xr_xir_compile_artifact_free(special);xr_xir_compile_artifact_free(checked);return status;
}
static XrXirArtifact *library_state_task_i64_lower(XrXirArtifact *owned,uint32_t ids[LIBRARY_STATE_TASK_I64_EXPORTS]) {
    CHECK(xr_xir_compile_artifact_verify(owned,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(owned,&packet,NULL)==XR_XIR_OK);
    library_compile_operation_cases("State reader specialization recheck lower bind seal",library_state_task_i64_pipeline_operation,&packet);
    xr_xir_compile_checked_packet_free(&packet);
    XrXirArtifact *special=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(owned,&special,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(owned);
    CHECK(xr_xir_compile_artifact_verify(special,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(special,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(special);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    library_state_task_i64_ids(xr_xir_compile_artifact_module(lowered),ids);return lowered;
}
#endif
#if CONSUMER_KIND!=1
static XrXirProgramSpec library_state_task_i64_vm_spec(XrXirArtifact *lowered,XrXirCallEntry *entries,XrXirVmBinding *bindings) {
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);CHECK(module->function_count<=128);
    XrXirProgramSpec spec={0};spec.abi_version=XR_XIR_PROGRAM_ABI_VERSION;
    spec.target=(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};spec.entry_count=module->function_count;
    spec.types=module->types;spec.declarations=module->declarations;spec.proof=xr_xir_compile_program_proof(lowered);
    for (uint32_t f=0;f<module->function_count;++f) CHECK(xr_xir_compile_vm_bind(lowered,f,&bindings[f],&entries[f])==XR_XIR_OK);
    spec.entries=entries;return spec;
}
#endif
#if CONSUMER_KIND==3
XR_FUNC void xr_test_library_state_task_i64_run(const XrXirCompileContext *context,XrXirArtifact *owned) {
    uint32_t ids[LIBRARY_STATE_TASK_I64_EXPORTS];XrXirArtifact *lowered=library_state_task_i64_lower(owned,ids);
    XrXirCallEntry entries[128];XrXirVmBinding bindings[128];XrXirProgramSpec spec=library_state_task_i64_vm_spec(lowered,entries,bindings);
    library_state_task_i64_finish(context,&spec,lowered,ids);
}
#else
int main(int argc,char **argv) {
    LibraryCompileOwner compiler={0};CHECK(library_compile_owner_new(&compiler,&library_compile_limits)==XR_XIR_OK);
    XrXirArtifact *lowered=NULL;
#if CONSUMER_KIND!=1
    XrXirArtifact *checked=NULL;
#endif
    uint32_t ids[LIBRARY_STATE_TASK_I64_EXPORTS];XrXirProgramSpec spec={0};
#if CONSUMER_KIND==0
    CHECK(argc==3);FILE *file=fopen(argv[1],"rb");CHECK(file && !fseek(file,0,SEEK_END));long length=ftell(file);
    CHECK(length>=64 && length<=262144 && !fseek(file,0,SEEK_SET));void *bytes=malloc((size_t)length);
    CHECK(bytes && fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));
    CHECK(xr_xir_compile_checked_read(&compiler.context,bytes,(size_t)length,&checked,NULL)==XR_XIR_OK);
    memset(bytes,0,(size_t)length);free(bytes);lowered=library_state_task_i64_lower(checked,ids);checked=NULL;
    XrXirCSource output={0};CHECK(xr_xir_compile_emit_c(lowered,"library_state_task_i64",1048576,&output)==XR_XIR_OK);
    file=fopen(argv[2],"wb");CHECK(file && fwrite(output.text,1,output.length,file)==output.length);
    CHECK(fprintf(file,"\nconst uint32_t library_state_task_i64_export_indices[13]={%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u};\n",ids[0],ids[1],ids[2],ids[3],ids[4],ids[5],ids[6],ids[7],ids[8],ids[9],ids[10],ids[11],ids[12])>0 && !fclose(file));
    xr_xir_compile_c_source_free(&output);
#else
    CHECK(argc==1);(void)argv;spec=library_state_task_i64_program;memcpy(ids,library_state_task_i64_export_indices,sizeof(ids));
#if CONSUMER_KIND==2
    CHECK(xr_xir_compile_checked_read(&compiler.context,spec.proof.bytes,spec.proof.length,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked,&spec.target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);checked=NULL;
#endif
#endif
#if CONSUMER_KIND!=1
    XrXirCallEntry entries[128];XrXirVmBinding bindings[128];spec=library_state_task_i64_vm_spec(lowered,entries,bindings);
#if CONSUMER_KIND==2
    CHECK(spec.entry_count==library_state_task_i64_program.entry_count);
    library_state_task_i64_mixed_count=spec.entry_count;
    for (uint32_t f=0;f<spec.entry_count;++f) {
        LibraryStateTaskI64MixedBinding *binding=&library_state_task_i64_mixed[f];
        binding->native=spec.declarations->functions[f].module!=spec.declarations->root_module;
        if (binding->native) {binding->actual=library_state_task_i64_program.entries[f];CHECK(!binding->actual.environment);}
        else CHECK(xr_xir_compile_vm_bind(lowered,f,&binding->vm,&binding->actual)==XR_XIR_OK && binding->actual.environment==&binding->vm);
        entries[f]=binding->actual;entries[f].environment=binding;
        entries[f].resume=library_state_task_i64_mixed_resume;entries[f].release=library_state_task_i64_mixed_release;
    }
#endif
#endif
    library_state_task_i64_finish(&compiler.context,&spec,lowered,ids);library_compile_owner_drop(&compiler);
#if CONSUMER_KIND==2
    CHECK(library_state_task_i64_steps[0] && library_state_task_i64_steps[1] && library_state_task_i64_crossings);
    printf("Library private state actual VMsteps=%llu nativeSteps=%llu crossings=%llu\n",
        (unsigned long long)library_state_task_i64_steps[0],(unsigned long long)library_state_task_i64_steps[1],(unsigned long long)library_state_task_i64_crossings);
#endif
    library_compile_observer_free();printf("Library private state consumer=%u PASS\n",CONSUMER_KIND);return 0;
}
#endif
