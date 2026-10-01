/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_default_invoke_execution.c - Independent default parameter execution
 *
 * KEY CONCEPT:
 *   The same Checked packet feeds source-free VM, generated C and mixed execution.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#if XR_DEFAULT_INVOKE_MODE==0
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_runtime_allocations.h"
#include "xir_default_invoke_runtime.h"
#if XR_DEFAULT_INVOKE_MODE>=2
extern const XrXirProgramSpec default_invoke_program;
extern const uint32_t default_invoke_export_indices[14];
#endif
#if XR_DEFAULT_INVOKE_MODE<=1
#include "xir/xxir_effects.h"
#include "xir_effect_witness_cases.h"
static void default_invoke_effects(XrXirArtifact *checked) {
    const XrXirModule *module=xr_xir_artifact_module(checked);XrXirEffects *effects=NULL;
    XrXirBudget budget=xr_xir_default_budget();budget.work=0;
    CHECK(xr_xir_effects_analyze(checked,&budget,&effects)==XR_XIR_BUDGET && !effects);
    budget=xr_xir_default_budget();budget.metadata_bytes=0;
    CHECK(xr_xir_effects_analyze(checked,&budget,&effects)==XR_XIR_BUDGET && !effects);
    CHECK(xr_xir_effects_analyze(checked,NULL,&effects)==XR_XIR_OK);
    effect_witness_paths(module,effects);
    for(uint32_t f=0;f<module->function_count;++f){
        if(!module->declarations->functions[f].exported)continue;
        const XrXirFunction *function=&module->functions[f];
        bool escaping=function->name_length==8 && !memcmp(function->name,"escaping",8);
        bool yielding=function->name_length==10 && !memcmp(function->name,"yieldCatch",10);
        const XrXirFunctionEffects *fact=xr_xir_effects_function(effects,f);CHECK(fact);
        CHECK(fact->throws==(escaping ? XR_XIR_EFFECT_MAY : XR_XIR_EFFECT_NONE));
        CHECK(fact->suspend==(yielding ? XR_XIR_EFFECT_MAY : XR_XIR_EFFECT_NONE));
    }
    xr_xir_effects_free(effects);
}
static XrXirArtifact *default_invoke_input(const char *path) {
    XrXirArtifact *checked=NULL;
#if XR_DEFAULT_INVOKE_MODE==0
    XrCompilerSession *session=xr_compiler_session_new(NULL);CHECK(session);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_source_check(&request,&result,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"source %u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
    checked=result.checked;result.checked=NULL;xr_xir_source_result_free(&result);xr_compiler_session_delete(session);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL)==XR_XIR_OK);
    FILE *file=fopen(path,"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));
    xr_xir_checked_packet_free(&packet);
#else
    FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));long size=ftell(file);
    CHECK(size>=64 && size<=262144 && !fseek(file,0,SEEK_SET));
    void *bytes=malloc((size_t)size);CHECK(bytes && fread(bytes,1,(size_t)size,file)==(size_t)size && !fclose(file));
    CHECK(xr_xir_checked_read(bytes,(size_t)size,NULL,&checked,NULL)==XR_XIR_OK);free(bytes);
#endif
    CHECK(xr_xir_artifact_module(checked)->defaults && xr_xir_artifact_module(checked)->defaults->count==7);
    default_invoke_effects(checked);return checked;
}
static XrXirArtifact *default_invoke_lower(XrXirArtifact *checked,uint32_t ids[14]) {
    XrXirArtifact *special=NULL,*lowered=NULL;
    CHECK(xr_xir_specialize(checked,NULL,&special,NULL)==XR_XIR_OK);xr_xir_artifact_free(checked);
    CHECK(xr_xir_artifact_verify(special,NULL,NULL)==XR_XIR_OK);
    CHECK(!xr_xir_artifact_module(special)->defaults);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(special,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_artifact_free(special);
    const XrXirModule *module=xr_xir_artifact_module(lowered);const char *names[]={"plain","typed","genericCatch","method","staticMethod","construction","nested","rethrow","cleanup","explicitControl","heldString","heldError","escaping","yieldCatch"};
    for(uint32_t e=0;e<14;++e){ids[e]=UINT32_MAX;
        for(uint32_t f=0;f<module->function_count;++f)
            if(module->functions[f].name_length==strlen(names[e]) && !memcmp(module->functions[f].name,names[e],strlen(names[e])))ids[e]=f;
        CHECK(ids[e]!=UINT32_MAX);
    }
    printf("invoke defaults Lowered functions=%u\n",module->function_count);return lowered;
}
#endif
int main(int argc,char **argv) {
    XrXirArtifact *lowered=NULL;XrXirProgramSpec spec={0};uint32_t ids[14];
#if XR_DEFAULT_INVOKE_MODE<=1
    CHECK(argc==3);lowered=default_invoke_lower(default_invoke_input(argv[1]),ids);
    const XrXirModule *module=xr_xir_artifact_module(lowered);
    spec.abi_version=XR_XIR_PROGRAM_ABI_VERSION;spec.target=(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    spec.entry_count=module->function_count;spec.declarations=module->declarations;spec.types=module->types;spec.proof=xr_xir_program_proof(lowered);
#if XR_DEFAULT_INVOKE_MODE==1
    XrXirCSource output={0};CHECK(xr_xir_emit_c(lowered,"default_invoke",1048576,&output)==XR_XIR_OK);
    FILE *file=fopen(argv[2],"wb");CHECK(file && fwrite(output.text,1,output.length,file)==output.length);
    CHECK(fprintf(file,"\nconst uint32_t default_invoke_export_indices[14]={")>0);
    for(uint32_t e=0;e<14;++e)CHECK(fprintf(file,"%s%u",e ? "," : "",ids[e])>0);
    CHECK(fprintf(file,"};\n")>0 && !fclose(file));
    xr_xir_c_source_free(&output);
#endif
#else
    CHECK(argc==1);(void)argv;spec=default_invoke_program;memcpy(ids,default_invoke_export_indices,sizeof(ids));
#if XR_DEFAULT_INVOKE_MODE==3
    XrXirArtifact *checked=NULL;CHECK(xr_xir_checked_read(spec.proof.bytes,spec.proof.length,NULL,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_lower(checked,&spec.target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_artifact_free(checked);
    const XrXirModule *module=xr_xir_artifact_module(lowered);spec.types=module->types;spec.declarations=module->declarations;spec.proof=xr_xir_program_proof(lowered);
#endif
#endif
#if XR_DEFAULT_INVOKE_MODE!=2
    XrXirCallEntry entries[64];XrXirVmBinding bindings[64];CHECK(spec.entry_count<=64);
    for(uint32_t f=0;f<spec.entry_count;++f){CHECK(xr_xir_vm_bind(lowered,f,&bindings[f],&entries[f])==XR_XIR_OK);
#if XR_DEFAULT_INVOKE_MODE==3
        if(f%2)entries[f]=default_invoke_program.entries[f];
#endif
    }
    spec.entries=entries;
#endif
    default_invoke_seal_faults(&spec);XrXirProgram *program=NULL;
    CHECK(xr_xir_program_seal(&spec,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
    default_invoke_cancel(program,ids[13]);
    default_invoke_runtime_faults(program,ids);XrXirValue held[2][3]={0};CHECK(default_invoke_pair(program,ids,held));
    xr_xir_program_drop(program);xr_xir_artifact_free(lowered);default_invoke_retained(held);
    printf("default invoke mode=%u catches41 retainedStringError physicalzero\n",XR_DEFAULT_INVOKE_MODE);return 0;
}
