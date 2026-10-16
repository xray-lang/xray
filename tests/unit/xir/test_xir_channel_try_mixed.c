/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_channel_try_mixed.c - Authenticated Channel and Task transport across both providers
 */
#define main channel_try_reference_main
#include "test_xir_channel_try_source.c"
#undef main
extern const XrXirProgramSpec channel_try_program;
typedef struct ChannelMixed {
    XrXirArtifact *lowered;XrXirCallEntry *entries;XrXirVmBinding *bindings;unsigned releases;
} ChannelMixed;
static void channel_mixed_drop(void *opaque) {
    ChannelMixed *owner=opaque;CHECK(!owner->releases++ && owner->lowered);
    xr_xir_compile_artifact_free(owner->lowered);owner->lowered=NULL;
    xr_free(owner->bindings);xr_free(owner->entries);owner->bindings=NULL;owner->entries=NULL;
}
int main(int argc,char **argv) {
    CHECK(argc==2 || (argc==3 && !strcmp(argv[2],"--runtime-faults")));
    bool root_native=!strcmp(argv[1],"native-vm");CHECK(root_native || !strcmp(argv[1],"vm-native"));
    LibraryCompileOwner compiler={0};CHECK(library_compile_owner_new(&compiler,&library_compile_limits)==XR_XIR_OK);
    ChannelTryPipeline capture={.keep_lowered=true};CHECK(channel_try_pipeline(&compiler.context,&capture)==XR_XIR_OK && capture.lowered && !capture.program);
    ChannelMixed owner={.lowered=capture.lowered};const XrXirModule *module=xr_xir_compile_artifact_module(owner.lowered);
    XrXirProgramProof proof=xr_xir_compile_program_proof(owner.lowered);const XrXirProgramSpec *native=&channel_try_program;
    CHECK(module->function_count==native->entry_count && proof.length==native->proof.length && !memcmp(proof.bytes,native->proof.bytes,proof.length));
    owner.entries=xr_calloc(module->function_count,sizeof(*owner.entries));owner.bindings=xr_calloc(module->function_count,sizeof(*owner.bindings));
    CHECK(owner.entries && owner.bindings);unsigned workers=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        bool worker=function->name_length>=11 && (!memcmp(function->name,"offerWorker",11) || !memcmp(function->name,"echoChannel",11));
        if (worker) ++workers;
        XrXirCallEntry vm={0};CHECK(xr_xir_compile_vm_bind(owner.lowered,f,&owner.bindings[f],&vm)==XR_XIR_OK);
        owner.entries[f]=(worker!=root_native) ? native->entries[f] : vm;
    }
    CHECK(workers==2);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,native->target,owner.entries,module->function_count,
        module->declarations,{&owner,channel_mixed_drop},module->types,proof};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&compiler.context,&spec,&program)==XR_XIR_OK);
    if (argc==3) { rendezvous_backend_faults(program,capture.rendezvous);channel_try_runtime_faults(program,capture.entry); }
    else { channel_retry_execute(program,capture.retries);rendezvous_backend_execute(program,capture.rendezvous);channel_try_execute(program,capture.entry); }
    CHECK(owner.releases==1 && !owner.lowered);library_compile_owner_drop(&compiler);library_compile_observer_free();
    printf("Channel actual mixed %s Task argument/result, typed outputs and code lease physical0 %s\n",argv[1],argc==3 ? "runtimeFI" : "normal/retry");return 0;
}
