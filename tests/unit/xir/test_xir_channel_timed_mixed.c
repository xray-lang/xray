/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_channel_timed_mixed.c - Real timed bodies across both code providers
 */
#define main channel_timed_source_reference_main
#include "test_xir_channel_timed_source.c"
#undef main
extern const XrXirProgramSpec channel_timed_program;
typedef struct TimedSourceMixed {
    XrXirArtifact *lowered;XrXirCallEntry *entries,*vm_entries;XrXirVmBinding *bindings;unsigned releases;
} TimedSourceMixed;
static void timed_source_mixed_drop(void *opaque) {
    TimedSourceMixed *owner=opaque;CHECK(!owner->releases++ && owner->lowered);
    xr_xir_compile_artifact_free(owner->lowered);owner->lowered=NULL;
    xr_compile_resources_free(owner->bindings);xr_compile_resources_free(owner->vm_entries);xr_free(owner->entries);
    owner->bindings=NULL;owner->vm_entries=NULL;owner->entries=NULL;
}
int main(int argc,char **argv) {
    CHECK(argc==2 || (argc==3 && (!strcmp(argv[2],"--runtime-faults") || !strcmp(argv[2],"--host"))));
    bool root_native=!strcmp(argv[1],"native-vm");CHECK(root_native || !strcmp(argv[1],"vm-native"));
    LibraryCompileOwner compiler={0};CHECK(library_compile_owner_new(&compiler,&library_compile_limits)==XR_XIR_OK);
    TimedSourcePipeline capture={.keep_lowered=true};CHECK(timed_source_pipeline(&compiler.context,&capture)==XR_XIR_OK && capture.lowered && !capture.program);
    TimedSourceMixed owner={.lowered=capture.lowered};const XrXirModule *module=xr_xir_compile_artifact_module(owner.lowered);
    XrXirProgramProof proof=xr_xir_compile_program_proof(owner.lowered);const XrXirProgramSpec *native=&channel_timed_program;
    CHECK(module->function_count==native->entry_count && proof.length==native->proof.length && !memcmp(proof.bytes,native->proof.bytes,proof.length));
    owner.entries=xr_calloc(module->function_count,sizeof(*owner.entries));CHECK(owner.entries);
    XrCompileResourceStats before_bind=library_compile_stats(&compiler.context);
    CHECK(xr_xir_compile_vm_bind_table(owner.lowered,&owner.bindings,&owner.vm_entries)==XR_XIR_OK && owner.bindings && owner.vm_entries);
    XrCompileResourceStats after_bind=library_compile_stats(&compiler.context);
    printf("TimedSource mixed actual complete VM bind table: functions=%u work=%llu allocated=%llu; original cumulative ledger retained\n",
        module->function_count,(unsigned long long)(after_bind.work-before_bind.work),(unsigned long long)(after_bind.allocated_bytes-before_bind.allocated_bytes));
    unsigned workers=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        bool worker=(function->name_length==6 && !memcmp(function->name,"worker",6)) ||
            (function->name_length==13 && !memcmp(function->name,"timedUnitSend",13));
        if (worker) ++workers;
        owner.entries[f]=(worker!=root_native) ? native->entries[f] : owner.vm_entries[f];
    }
    CHECK(workers==2);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,native->target,owner.entries,module->function_count,
        module->declarations,{&owner,timed_source_mixed_drop},module->types,proof};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&compiler.context,&spec,&program)==XR_XIR_OK);
    if (argc==3 && !strcmp(argv[2],"--runtime-faults")) timed_source_runtime_faults(program,capture.entry);
    else if (argc==3) timed_source_host_execute(program,capture.entry);
    else { timed_life_cases(program,capture.endpoints);timed_source_execute(program,capture.entry); }
    CHECK(owner.releases==1 && !owner.lowered);library_compile_owner_drop(&compiler);library_compile_observer_free();
    printf("TimedSource Channel actual mixed %s: real timed Channel and child go/await across code providers; code lease physical0\n",argv[1]);return 0;
}
