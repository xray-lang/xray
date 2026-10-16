/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_channel_blocking_mixed.c - Real blocking bodies across both code providers
 */
#define main channel_blocking_reference_main
#include "test_xir_channel_blocking_source.c"
#undef main
extern const XrXirProgramSpec channel_blocking_program;
typedef struct BlockingMixed {
    XrXirArtifact *lowered;XrXirCallEntry *entries,*vm_entries;XrXirVmBinding *bindings;unsigned releases;
} BlockingMixed;
static void blocking_mixed_drop(void *opaque) {
    BlockingMixed *owner=opaque;CHECK(!owner->releases++ && owner->lowered);
    xr_xir_compile_artifact_free(owner->lowered);owner->lowered=NULL;
    xr_compile_resources_free(owner->bindings);xr_compile_resources_free(owner->vm_entries);xr_free(owner->entries);
    owner->bindings=NULL;owner->vm_entries=NULL;owner->entries=NULL;
}
int main(int argc,char **argv) {
    CHECK(argc==2 || (argc==3 && !strcmp(argv[2],"--runtime-faults")));
    bool root_native=!strcmp(argv[1],"native-vm");CHECK(root_native || !strcmp(argv[1],"vm-native"));
    LibraryCompileOwner compiler={0};CHECK(library_compile_owner_new(&compiler,&library_compile_limits)==XR_XIR_OK);
    BlockingPipeline capture={.keep_lowered=true};CHECK(blocking_pipeline(&compiler.context,&capture)==XR_XIR_OK && capture.lowered && !capture.program);
    BlockingMixed owner={.lowered=capture.lowered};const XrXirModule *module=xr_xir_compile_artifact_module(owner.lowered);
    XrXirProgramProof proof=xr_xir_compile_program_proof(owner.lowered);const XrXirProgramSpec *native=&channel_blocking_program;
    CHECK(module->function_count==native->entry_count && proof.length==native->proof.length && !memcmp(proof.bytes,native->proof.bytes,proof.length));
    owner.entries=xr_calloc(module->function_count,sizeof(*owner.entries));CHECK(owner.entries);
    XrCompileResourceStats before_bind=library_compile_stats(&compiler.context);
    CHECK(xr_xir_compile_vm_bind_table(owner.lowered,&owner.bindings,&owner.vm_entries)==XR_XIR_OK && owner.bindings && owner.vm_entries);
    XrCompileResourceStats after_bind=library_compile_stats(&compiler.context);
    printf("Blocking mixed actual complete VM bind table: functions=%u work=%llu allocated=%llu; original cumulative ledger retained\n",
        module->function_count,(unsigned long long)(after_bind.work-before_bind.work),(unsigned long long)(after_bind.allocated_bytes-before_bind.allocated_bytes));
    unsigned workers=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        bool worker=(function->name_length==10 && !memcmp(function->name,"sendWorker",10)) ||
            (function->name_length==10 && !memcmp(function->name,"recvWorker",10)) ||
            (function->name_length==10 && !memcmp(function->name,"caughtSend",10)) ||
            (function->name_length==14 && !memcmp(function->name,"sendUnitWorker",14)) ||
            (function->name_length==8 && !memcmp(function->name,"failSend",8)) ||
            (function->name_length==16 && (!memcmp(function->name,"blockingUnitSend",16) || !memcmp(function->name,"blockingUnitRecv",16))) ||
            (function->name_length==19 && !memcmp(function->name,"blockingUnitDefault",19));
        if (worker) ++workers;
        owner.entries[f]=(worker!=root_native) ? native->entries[f] : owner.vm_entries[f];
    }
    CHECK(workers==8);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,native->target,owner.entries,module->function_count,
        module->declarations,{&owner,blocking_mixed_drop},module->types,proof};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&compiler.context,&spec,&program)==XR_XIR_OK);
    if (argc==3) blocking_runtime_faults(program,capture.entry);
    else { blocking_lifecycle_cases(program,capture.endpoints);blocking_execute(program,capture.entry); }
    CHECK(owner.releases==1 && !owner.lowered);library_compile_owner_drop(&compiler);library_compile_observer_free();
    printf("Blocking Channel actual mixed %s: real send/receive, go/await and Closed error across code providers; code lease physical0\n",argv[1]);return 0;
}
