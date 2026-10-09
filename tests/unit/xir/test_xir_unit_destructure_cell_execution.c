/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_unit_destructure_cell_execution.c - Same program private state on every backend
 */
#ifndef UNIT_PENDING
#define UNIT_PENDING 0
#endif
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
#if CONSUMER_KIND==0
static size_t unit_destructure_cell_specializations,unit_destructure_cell_verifications;
static size_t unit_destructure_cell_reads,unit_destructure_cell_lowers,unit_destructure_cell_emissions;
static size_t unit_destructure_cell_scans;
static void unit_destructure_cell_observe_cases(const char *name,LibraryCompileOperation operation,void *fixture) {
    ++unit_destructure_cell_scans;library_compile_operation_cases(name,operation,fixture);
}
static XrXirStatus unit_destructure_cell_observe_read(const XrXirCompileContext *context,const void *bytes,
    size_t length,XrXirArtifact **output,XrXirDiagnostic *diagnostic) {
    ++unit_destructure_cell_reads;return xr_xir_compile_checked_read(context,bytes,length,output,diagnostic);
}
static XrXirStatus unit_destructure_cell_observe_specialize(const XrXirArtifact *artifact,
    XrXirArtifact **output,XrXirDiagnostic *diagnostic) {
    ++unit_destructure_cell_specializations;return xr_xir_compile_specialize(artifact,output,diagnostic);
}
static XrXirStatus unit_destructure_cell_observe_verify(const XrXirArtifact *artifact,XrXirDiagnostic *diagnostic) {
    ++unit_destructure_cell_verifications;return xr_xir_compile_artifact_verify(artifact,diagnostic);
}
static XrXirStatus unit_destructure_cell_observe_lower(const XrXirArtifact *artifact,const XrXirTarget *target,
    XrXirArtifact **output,XrXirDiagnostic *diagnostic) {
    ++unit_destructure_cell_lowers;return xr_xir_compile_lower(artifact,target,output,diagnostic);
}
static XrXirStatus unit_destructure_cell_observe_emit(const XrXirArtifact *artifact,const char *prefix,
    size_t limit,XrXirCSource *output) {
    ++unit_destructure_cell_emissions;return xr_xir_compile_emit_c(artifact,prefix,limit,output);
}
#define xr_xir_compile_specialize unit_destructure_cell_observe_specialize
#define xr_xir_compile_checked_read unit_destructure_cell_observe_read
#define xr_xir_compile_artifact_verify unit_destructure_cell_observe_verify
#define xr_xir_compile_lower unit_destructure_cell_observe_lower
#define xr_xir_compile_emit_c unit_destructure_cell_observe_emit
#define library_compile_operation_cases unit_destructure_cell_observe_cases
#endif
#define XR_UNIT_DESTRUCTURE_CELL_RUNTIME_IMPLEMENTATION
#include "xir_unit_destructure_cell_runtime.h"
#if CONSUMER_KIND==1 || CONSUMER_KIND==2
extern const XrXirProgramSpec unit_destructure_cell_program;
extern const uint32_t unit_destructure_cell_export_indices[UNIT_DESTRUCTURE_CELL_EXPORTS];
#endif
#if CONSUMER_KIND==2
typedef struct UnitDestructureCellMixedBinding {
    XrXirVmBinding vm;
    XrXirCallEntry actual;
    bool native;
} UnitDestructureCellMixedBinding;
static UnitDestructureCellMixedBinding unit_destructure_cell_mixed[128];
static uint32_t unit_destructure_cell_mixed_count;
static uint64_t unit_destructure_cell_steps[2],unit_destructure_cell_crossings;
static XrXirAction unit_destructure_cell_mixed_resume(XrXirCallView *view) {
    UnitDestructureCellMixedBinding *binding=(UnitDestructureCellMixedBinding *)view->environment;
    CHECK(binding>=unit_destructure_cell_mixed && binding<unit_destructure_cell_mixed+unit_destructure_cell_mixed_count);
    ++unit_destructure_cell_steps[binding->native];
    uint32_t before=binding->native ? UINT32_MAX : ((VmState *)view->state)->instruction;
    XrXirAction action=binding->actual.resume(view);
    CHECK(view->environment==binding);
    if (!binding->native && action.kind==XR_XIR_ACTION_FAULT) {
        const XrXirModule *module=xr_xir_compile_artifact_module(binding->vm.artifact);
        const XrXirFunction *function=&module->functions[binding->vm.function];
        fprintf(stderr,"UNIT_VM_FAULT function=%u before=%u after=%u op=%u inbox=%u value=%lld phase=%u\n",
            binding->vm.function,before,((VmState *)view->state)->instruction,
            before<function->instruction_count ? function->instructions[before].op : UINT32_MAX,
            view->inbox.status,(long long)action.value.payload,(unsigned)view->phase);
    }
    if (action.kind==XR_XIR_ACTION_CALL) {
        CHECK(action.callee<unit_destructure_cell_mixed_count);
        if (binding->native!=unit_destructure_cell_mixed[action.callee].native) ++unit_destructure_cell_crossings;
    }
    return action;
}
static void unit_destructure_cell_mixed_release(XrXirCallView *view,XrXirCallStatus reason) {
    UnitDestructureCellMixedBinding *binding=(UnitDestructureCellMixedBinding *)view->environment;
    CHECK(binding>=unit_destructure_cell_mixed && binding<unit_destructure_cell_mixed+unit_destructure_cell_mixed_count);
    binding->actual.release(view,reason);CHECK(view->environment==binding);
}
#endif
#if CONSUMER_KIND==0 || CONSUMER_KIND==3
static XrXirStatus unit_destructure_cell_pipeline_operation(const XrXirCompileContext *context,void *opaque) {
    const XrXirCheckedPacket *packet=opaque;
    XrXirArtifact *checked=NULL,*special=NULL,*lowered=NULL;XrXirProgram *program=NULL;
    XrXirCallEntry *entries=NULL;XrXirVmBinding *bindings=NULL;
    XrXirStatus status=xr_xir_compile_checked_read(context,packet->bytes,packet->length,&checked,NULL);
    if (status!=XR_XIR_OK) {CHECK(!checked);goto done;}
    status=xr_xir_compile_specialize(checked,&special,NULL);
    if (status!=XR_XIR_OK) {CHECK(!special);goto done;}
    status=xr_xir_compile_artifact_verify(special,NULL);if (status!=XR_XIR_OK) goto done;
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    status=xr_xir_compile_lower(special,&target,&lowered,NULL);
    if (status!=XR_XIR_OK) {CHECK(!lowered);goto done;}
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);CHECK(module->function_count<=128);
    status=xr_xir_compile_vm_bind_table(lowered,&bindings,&entries);
    if (status!=XR_XIR_OK) {CHECK(!bindings && !entries);goto done;}
    CHECK(bindings && entries);
    XrXirProgramSpec spec={.abi_version=XR_XIR_PROGRAM_ABI_VERSION,.target=target,
        .entries=entries,.entry_count=module->function_count,.types=module->types,
        .declarations=module->declarations,.proof=xr_xir_compile_program_proof(lowered)};
    status=xr_xir_compile_program_seal(context,&spec,&program);
    CHECK(status==XR_XIR_OK ? program!=NULL : program==NULL);
done:
    xr_xir_compile_program_drop(program);
    xr_compile_resources_free(entries);xr_compile_resources_free(bindings);
    xr_xir_compile_artifact_free(lowered);
    xr_xir_compile_artifact_free(special);xr_xir_compile_artifact_free(checked);return status;
}
static XrXirArtifact *unit_destructure_cell_lower_verified(XrXirArtifact *owned,uint32_t ids[UNIT_DESTRUCTURE_CELL_EXPORTS]) {
    XrXirArtifact *special=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(owned,&special,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(owned);
    CHECK(xr_xir_compile_artifact_verify(special,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(special,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(special);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    unit_destructure_cell_ids(xr_xir_compile_artifact_module(lowered),ids);return lowered;
}
#if CONSUMER_KIND==0
static XrXirArtifact *unit_destructure_cell_lower_once(XrXirArtifact *owned,uint32_t ids[UNIT_DESTRUCTURE_CELL_EXPORTS]) {
    CHECK(xr_xir_compile_artifact_verify(owned,NULL)==XR_XIR_OK);
    return unit_destructure_cell_lower_verified(owned,ids);
}
#endif
static XrXirArtifact *unit_destructure_cell_lower(XrXirArtifact *owned,uint32_t ids[UNIT_DESTRUCTURE_CELL_EXPORTS]) {
    CHECK(xr_xir_compile_artifact_verify(owned,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(owned,&packet,NULL)==XR_XIR_OK);
    library_compile_operation_cases("State reader specialization recheck lower bind seal",unit_destructure_cell_pipeline_operation,&packet);
    xr_xir_compile_checked_packet_free(&packet);
    return unit_destructure_cell_lower_verified(owned,ids);
}
#endif
#if CONSUMER_KIND!=1
static XrXirProgramSpec unit_destructure_cell_vm_spec(XrXirArtifact *lowered,XrXirCallEntry **entries,XrXirVmBinding **bindings) {
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);CHECK(module->function_count<=128);
    XrXirProgramSpec spec={0};spec.abi_version=XR_XIR_PROGRAM_ABI_VERSION;
    spec.target=(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};spec.entry_count=module->function_count;
    spec.types=module->types;spec.declarations=module->declarations;spec.proof=xr_xir_compile_program_proof(lowered);
    CHECK(xr_xir_compile_vm_bind_table(lowered,bindings,entries)==XR_XIR_OK);
    CHECK(*bindings && *entries);spec.entries=*entries;return spec;
}
#endif
#if CONSUMER_KIND==3
XR_FUNC void xr_test_unit_destructure_cell_run(const XrXirCompileContext *context,XrXirArtifact *owned,bool pending) {
    uint32_t ids[UNIT_DESTRUCTURE_CELL_EXPORTS];XrXirArtifact *lowered=unit_destructure_cell_lower(owned,ids);
    XrXirCallEntry *entries=NULL;XrXirVmBinding *bindings=NULL;
    XrXirProgramSpec spec=unit_destructure_cell_vm_spec(lowered,&entries,&bindings);
    if (pending) unit_destructure_cell_pending_finish(context,&spec,lowered,ids);
    else unit_destructure_cell_finish(context,&spec,lowered,ids);
    /* The finish helper has dropped every Program and its Lowered producer.
     * No binding is read after that point; these arrays only release storage. */
    xr_compile_resources_free(entries);xr_compile_resources_free(bindings);
}
#else
int main(int argc,char **argv) {
#if CONSUMER_KIND==0
    bool generating=argc==4 && !strcmp(argv[1],"--write-c");
    bool pending=argc==4 && !strcmp(argv[3],"--pending");
    CHECK(argc==3 || generating || pending);
    if (argc==3) CHECK(strncmp(argv[1],"--",2));
    const char *input=argv[generating ? 2 : 1],*output_path=argv[generating ? 3 : 2];
    CHECK(input[0] && output_path[0] && strcmp(input,output_path));
#else
    CHECK(argc==1);bool pending=UNIT_PENDING;
#endif
    LibraryCompileOwner compiler={0};CHECK(library_compile_owner_new(&compiler,&library_compile_limits)==XR_XIR_OK);
    XrXirArtifact *lowered=NULL;
#if CONSUMER_KIND!=1
    XrXirArtifact *checked=NULL;
#endif
    uint32_t ids[UNIT_DESTRUCTURE_CELL_EXPORTS];XrXirProgramSpec spec={0};
#if CONSUMER_KIND==0
    FILE *file=fopen(input,"rb");CHECK(file && !fseek(file,0,SEEK_END));long length=ftell(file);
    CHECK(length>=64 && length<=262144 && !fseek(file,0,SEEK_SET));void *bytes=malloc((size_t)length);
    CHECK(bytes && fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));
    CHECK(xr_xir_compile_checked_read(&compiler.context,bytes,(size_t)length,&checked,NULL)==XR_XIR_OK);
    memset(bytes,0,(size_t)length);free(bytes);
    lowered=generating ? unit_destructure_cell_lower_once(checked,ids) : unit_destructure_cell_lower(checked,ids);checked=NULL;
    XrXirCSource output={0};CHECK(xr_xir_compile_emit_c(lowered,"unit_destructure_cell",1048576,&output)==XR_XIR_OK);
    if (generating) CHECK(unit_destructure_cell_reads==1 && unit_destructure_cell_specializations==1 &&
        unit_destructure_cell_verifications==3 && unit_destructure_cell_lowers==1 &&
        unit_destructure_cell_emissions==1 && !unit_destructure_cell_scans);
    file=fopen(output_path,"wb");CHECK(file && fwrite(output.text,1,output.length,file)==output.length);
    CHECK(fprintf(file,"\nconst uint32_t unit_destructure_cell_export_indices[5]={%u,%u,%u,%u,%u};\n",ids[0],ids[1],ids[2],ids[3],ids[4])>0 && !fclose(file));
    xr_xir_compile_c_source_free(&output);
    if (generating) {
        xr_xir_compile_artifact_free(lowered);
        XrCompileResourceStats measured=library_compile_stats(&compiler.context);
        fprintf(stderr,"unit-destructure-generation resources producer=C allocated=%llu peak=%llu work=%llu\n",
            (unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
        library_compile_owner_drop(&compiler);library_compile_observer_free();
        CHECK(!runtime_live && !runtime_bytes);
        fprintf(stderr,"unit-destructure-generation C directReaderCalls=%zu specialize=%zu explicitVerify=%zu lower=%zu emit=%zu fullScans=%zu runtimeAttempts=%zu fullFI=NOT_RUN\n",
            unit_destructure_cell_reads,unit_destructure_cell_specializations,unit_destructure_cell_verifications,
            unit_destructure_cell_lowers,unit_destructure_cell_emissions,unit_destructure_cell_scans,runtime_attempts);
        return 0;
    }
#else
    CHECK(argc==1);(void)argv;spec=unit_destructure_cell_program;memcpy(ids,unit_destructure_cell_export_indices,sizeof(ids));
#if CONSUMER_KIND==2
    CHECK(xr_xir_compile_checked_read(&compiler.context,spec.proof.bytes,spec.proof.length,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked,&spec.target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);checked=NULL;
#endif
#endif
#if CONSUMER_KIND!=1
    XrXirCallEntry *entries=NULL;XrXirVmBinding *bindings=NULL;
    spec=unit_destructure_cell_vm_spec(lowered,&entries,&bindings);
#if CONSUMER_KIND==2
    CHECK(spec.entry_count==unit_destructure_cell_program.entry_count);
    unit_destructure_cell_mixed_count=spec.entry_count;
    for (uint32_t f=0;f<spec.entry_count;++f) {
        UnitDestructureCellMixedBinding *binding=&unit_destructure_cell_mixed[f];
        binding->native=spec.declarations->functions[f].module!=spec.declarations->root_module;
        if (binding->native) {binding->actual=unit_destructure_cell_program.entries[f];CHECK(!binding->actual.environment);}
        else {
            binding->vm=bindings[f];binding->actual=entries[f];
            CHECK(binding->vm.artifact==lowered && binding->vm.function==f &&
                binding->actual.environment==&bindings[f]);
            binding->actual.environment=&binding->vm;
        }
        entries[f]=binding->actual;entries[f].environment=binding;
        entries[f].resume=unit_destructure_cell_mixed_resume;entries[f].release=unit_destructure_cell_mixed_release;
    }
#endif
#endif
    if (pending) unit_destructure_cell_pending_finish(&compiler.context,&spec,lowered,ids);
    else unit_destructure_cell_finish(&compiler.context,&spec,lowered,ids);
#if CONSUMER_KIND!=1
    xr_compile_resources_free(entries);xr_compile_resources_free(bindings);
#endif
    library_compile_owner_drop(&compiler);
#if CONSUMER_KIND==2
    CHECK(unit_destructure_cell_steps[0] && unit_destructure_cell_steps[1]);
    if (!pending) CHECK(unit_destructure_cell_crossings);
    printf("Library private state actual VMsteps=%llu nativeSteps=%llu crossings=%llu\n",
        (unsigned long long)unit_destructure_cell_steps[0],(unsigned long long)unit_destructure_cell_steps[1],(unsigned long long)unit_destructure_cell_crossings);
#endif
    library_compile_observer_free();printf("Library private state consumer=%u PASS\n",CONSUMER_KIND);return 0;
}
#endif
