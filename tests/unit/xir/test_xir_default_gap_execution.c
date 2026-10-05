/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_default_gap_execution.c - Independent default parameter execution
 *
 * KEY CONCEPT:
 *   The same Checked packet feeds source-free VM, generated C and mixed execution.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#if XR_DEFAULT_GAP_MODE==0
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir_default_execution_owner.h"
#define DEFAULT_MODE XR_DEFAULT_GAP_MODE
#define DEFAULT_COUNT 3u
#define DEFAULT_BINDINGS 4u
#define DEFAULT_SYMBOL "default_gaps"
#define DEFAULT_SPEC default_gaps_program
#if DEFAULT_MODE!=2
static const char *default_names[]={"order","retained","construct"};
#endif
#if DEFAULT_MODE>=2
extern const XrXirProgramSpec default_gaps_program;
extern const uint32_t default_gaps_export_indices[3];
#endif
#define DEFAULT_EFFECTS(checked) XR_XIR_OK
typedef struct DefaultMixedOwner { XrXirProgram *program; XrXirCallEntry *entries; } DefaultMixedOwner;
static unsigned default_mixed_releases;
#if DEFAULT_MODE!=2
static inline void default_mixed_free(void *pointer) {
    DefaultMixedOwner *owner=pointer;if(!owner)return;
    xr_xir_compile_program_drop(owner->program);xr_compile_resources_free(owner->entries);
    xr_compile_resources_free(owner);++default_mixed_releases;
}
#endif
#if DEFAULT_MODE==3
static XrXirStatus default_allocate(const XrXirCompileContext *context,size_t count,size_t bytes,void **output) {
    XrCompileResourceStatus status=xr_compile_resources_calloc(context->resources,count,bytes,output);
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
}
#endif
static const char *default_packet_path,*default_c_path;
#if DEFAULT_MODE==1
static const uint8_t *default_packet_bytes;static size_t default_packet_length;
#endif
static uint32_t default_ids[DEFAULT_COUNT];
#if DEFAULT_MODE<=1
static XrXirStatus default_input(const XrXirCompileContext *context,XrXirArtifact **output) {
#if DEFAULT_MODE==0
    XrCompilerSession *session=NULL;XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrCompilerSessionStatus setup=xr_compile_session_new(context->resources,&session);
    XrXirStatus status=setup==XR_COMPILER_SESSION_OK?XR_XIR_OK:setup==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    if(status==XR_XIR_OK)status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status==XR_XIR_OK){CHECK(result.checked && result.snapshot);*output=result.checked;result.checked=NULL;}
    else if(status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_BUDGET)fprintf(stderr,"source %u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    if(status!=XR_XIR_OK)CHECK(!result.checked && !result.snapshot);
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);return status;
#else
    return xr_xir_compile_checked_read(context,default_packet_bytes,default_packet_length,output,NULL);
#endif
}
#endif
static XrXirStatus default_build(const XrXirCompileContext *context,unsigned variant,XrXirProgram **output) {
    (void)variant;
#if DEFAULT_MODE==2
    return xr_xir_compile_program_seal(context,&DEFAULT_SPEC,output);
#else
    XrXirArtifact *checked=NULL,*special=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};XrXirCSource source={0};
    DefaultMixedOwner *owner=NULL;XrXirStatus status=XR_XIR_OK;
#if DEFAULT_MODE<=1
    status=default_input(context,&checked);
    if(status==XR_XIR_OK){const XrXirModule *module=xr_xir_compile_artifact_module(checked);CHECK(module->defaults && module->defaults->count==DEFAULT_BINDINGS);}
    if(status==XR_XIR_OK)status=DEFAULT_EFFECTS(checked);
#if DEFAULT_MODE==0
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_write(checked,&packet,NULL);
    if(status==XR_XIR_OK && default_packet_path){FILE *file=fopen(default_packet_path,"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));}
    if(status==XR_XIR_OK){xr_xir_compile_artifact_free(checked);checked=NULL;status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL);}
#endif
#else
    status=xr_xir_compile_checked_read(context,DEFAULT_SPEC.proof.bytes,DEFAULT_SPEC.proof.length,&checked,NULL);
#endif
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(checked,&special,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(special,NULL);
    if(status==XR_XIR_OK){CHECK(!xr_xir_compile_artifact_module(special)->defaults);status=xr_xir_compile_lower(special,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL);}
    const XrXirModule *module=status==XR_XIR_OK?xr_xir_compile_artifact_module(lowered):NULL;
    if(status==XR_XIR_OK){CHECK(module->function_count<=64);for(uint32_t e=0;e<DEFAULT_COUNT;++e){default_ids[e]=UINT32_MAX;
        for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(default_names[e]) && !memcmp(module->functions[f].name,default_names[e],strlen(default_names[e])))default_ids[e]=f;
        CHECK(default_ids[e]!=UINT32_MAX);}}
#if DEFAULT_MODE==1
    if(status==XR_XIR_OK)status=xr_xir_compile_emit_c(lowered,DEFAULT_SYMBOL,1048576,&source);
    if(status==XR_XIR_OK){CHECK(source.text[source.length]==0 && !strstr(source.text,"({"));
        if(default_c_path){FILE *file=fopen(default_c_path,"wb");CHECK(file && fwrite(source.text,1,source.length,file)==source.length);
            CHECK(fprintf(file,"\nconst uint32_t " DEFAULT_SYMBOL "_export_indices[%u]={",DEFAULT_COUNT)>0);
            for(uint32_t e=0;e<DEFAULT_COUNT;++e)CHECK(fprintf(file,"%s%u",e?",":"",default_ids[e])>0);
            CHECK(fprintf(file,"};\n")>0 && !fclose(file));}}
#endif
#if DEFAULT_MODE<=1
    if(status==XR_XIR_OK)status=xr_xir_compile_vm_program_take(&lowered,output);
#elif DEFAULT_MODE==2
    if(status==XR_XIR_OK)status=xr_xir_compile_program_seal(context,&DEFAULT_SPEC,output);
#else
    if(status==XR_XIR_OK)status=default_allocate(context,1,sizeof(*owner),(void **)&owner);
    if(status==XR_XIR_OK)status=default_allocate(context,module->function_count,sizeof(*owner->entries),(void **)&owner->entries);
    const XrXirArtifact *proof=lowered;
    if(status==XR_XIR_OK)status=xr_xir_compile_vm_program_take(&lowered,&owner->program);
    if(status==XR_XIR_OK){CHECK(owner->program->context.resources==context->resources);unsigned native=0,vm=0;
        for(uint32_t f=0;f<module->function_count;++f){if(f%2){owner->entries[f]=DEFAULT_SPEC.entries[f];++native;}else{owner->entries[f]=owner->program->entries[f];++vm;}}
        CHECK(native && vm);XrXirProgramSpec spec=DEFAULT_SPEC;spec.entries=owner->entries;spec.types=module->types;spec.declarations=module->declarations;
        spec.proof=xr_xir_compile_program_proof(proof);spec.code=(XrXirCodeLease){owner,default_mixed_free};
        status=xr_xir_compile_program_seal(context,&spec,output);if(status==XR_XIR_OK)owner=NULL;}
#endif
    if(status!=XR_XIR_OK)CHECK(!*output);
    default_mixed_free(owner);xr_xir_compile_checked_packet_free(&packet);
#if DEFAULT_MODE==1
    xr_xir_compile_c_source_free(&source);
#else
    (void)source;
#endif
    xr_xir_compile_artifact_free(checked);xr_xir_compile_artifact_free(special);xr_xir_compile_artifact_free(lowered);return status;
#endif
}
#include "xir_default_gap_runtime.h"
int main(int argc,char **argv) {
    /* Only explicit artifact-producer invocations omit repeated fault sweeps. */
    bool generate_only=false;
#if DEFAULT_MODE<=1
    if(argc==4){
        CHECK(!strcmp(argv[1],"--generate-only"));generate_only=true;++argv;
    }else CHECK(argc==3 && strcmp(argv[1],"--generate-only"));
#else
    CHECK(argc==1);(void)argv;
#endif
    XrXirCompileContext context=default_context(default_limits());XrCompileResourceStats baseline=default_stats(&context);uint8_t *bytes=NULL;
#if DEFAULT_MODE<=1
#if DEFAULT_MODE==0
    default_packet_path=argv[1];
#else
    FILE *file=fopen(argv[1],"rb");CHECK(file && !fseek(file,0,SEEK_END));long length=ftell(file);CHECK(length>=64 && length<=262144 && !fseek(file,0,SEEK_SET));
    bytes=malloc((size_t)length);CHECK(bytes && fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));
    default_packet_bytes=bytes;default_packet_length=(size_t)length;default_c_path=argv[2];
#endif
#else
    memcpy(default_ids,default_gaps_export_indices,sizeof(default_ids));
#endif
    XrXirProgram *program=NULL;CHECK(default_build(&context,0,&program)==XR_XIR_OK);default_pipeline_report(&context,DEFAULT_SYMBOL);
    if(!generate_only)gap_runtime_faults(program,default_ids);XrXirValue held[2]={{0},{0}};CHECK(gap_pair(program,default_ids,held));
    unsigned releases=default_mixed_releases;
    xr_xir_compile_program_drop(program);gap_retained(held);default_owner_free(&context,baseline);
    CHECK(default_mixed_releases==releases+(DEFAULT_MODE==3 ? 1u : 0u));
    default_packet_path=default_c_path=NULL;
    if(!generate_only)gap_seal_faults(default_build);free(bytes);
    printf("default gap mode=%u %s outputs retained physicalzero\n",DEFAULT_MODE,generate_only?"artifact producer normal":"all original");return 0;
}
