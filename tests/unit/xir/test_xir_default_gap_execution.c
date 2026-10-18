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
#include "xir/xxir_program_internal.h"
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
#if XR_DEFAULT_GAP_MODE==3
static void default_gap_before_allocation(size_t bytes);
#define XR_INSTANCE_COMPILE_BEFORE_ALLOCATION(bytes) default_gap_before_allocation(bytes)
#endif
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
typedef struct DefaultMixedOwner { XrXirVmBinding *bindings; XrXirCallEntry *entries; } DefaultMixedOwner;
static unsigned default_mixed_releases;
#if DEFAULT_MODE!=2
static inline void default_mixed_free(void *pointer) {
    DefaultMixedOwner *owner=pointer;if(!owner)return;
    xr_compile_resources_free(owner->bindings);xr_compile_resources_free(owner->entries);
    xr_compile_resources_free(owner);
}
#if DEFAULT_MODE==3
static inline void default_mixed_release(void *pointer) {
    default_mixed_free(pointer);++default_mixed_releases;
}
#endif
#endif
#if DEFAULT_MODE==3
static XrXirStatus default_allocate(const XrXirCompileContext *context,size_t count,size_t bytes,void **output) {
    XrCompileResourceStatus status=xr_compile_resources_calloc(context->resources,count,bytes,output);
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
}
#endif
#include "xir_default_gap_owned.h"
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
    if(!output || *output)return XR_XIR_BAD_STRUCTURE;
#if DEFAULT_MODE==2
    return xr_xir_compile_program_seal(context,&DEFAULT_SPEC,output);
#else
    XrXirArtifact *checked=NULL,*special=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};XrXirCSource source={0};
    DefaultMixedOwner *owner=NULL;XrXirProgramInput *input=NULL;XrXirProgram *received=NULL;XrXirStatus status=XR_XIR_OK;
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
    status=xr_xir_compile_checked_read(context,DEFAULT_SPEC.packet.bytes,DEFAULT_SPEC.packet.length,&checked,NULL);
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
    xr_xir_compile_artifact_free(checked);checked=NULL;xr_xir_compile_artifact_free(special);special=NULL;
    xr_xir_compile_checked_packet_free(&packet);
#if DEFAULT_MODE==1
    xr_xir_compile_c_source_free(&source);
#endif
#if DEFAULT_MODE<=1
    if(status==XR_XIR_OK)status=xr_xir_compile_vm_program_take(&lowered,output);
#elif DEFAULT_MODE==2
    if(status==XR_XIR_OK)status=xr_xir_compile_program_seal(context,&DEFAULT_SPEC,output);
#else
    if(status==XR_XIR_OK)status=default_allocate(context,1,sizeof(*owner),(void **)&owner);
    if(status==XR_XIR_OK && module->function_count!=DEFAULT_SPEC.entry_count)status=XR_XIR_BAD_STRUCTURE;
    if(status==XR_XIR_OK)status=xr_xir_compile_vm_bind_table(lowered,&owner->bindings,&owner->entries);
    XirMixedProjection projection;
    if(status==XR_XIR_OK)status=xir_mixed_projection_prepare(context,lowered,owner->bindings,
        owner->entries,module->function_count,&projection);
    if(status==XR_XIR_OK){unsigned native=0,vm=0;
        for(uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f){
            CHECK(owner->bindings[f].artifact==lowered && owner->bindings[f].function==f);
            CHECK(owner->entries[f].environment==&owner->bindings[f]);
            if(f%2){status=xir_mixed_projection_native(context,&projection,f,&DEFAULT_SPEC.entries[f]);
                if(status==XR_XIR_OK){owner->entries[f]=DEFAULT_SPEC.entries[f];++native;}}
            else{CHECK(owner->entries[f].resume!=DEFAULT_SPEC.entries[f].resume);++vm;}}
        if(status!=XR_XIR_OK)goto finish;
        CHECK(native && vm);XrXirProgramSpec spec=DEFAULT_SPEC;spec.entries=owner->entries;spec.types=module->types;spec.declarations=module->declarations;
        spec.packet=(XrXirProgramPacket){0};spec.code=(XrXirCodeLease){0};
        default_attack_binding=default_binding_attack?owner->bindings:NULL;
        status=xir_program_input_take(context,&lowered,&input);
        if(status==XR_XIR_OK)status=default_owned_recovery?default_owned_retry(context,&spec,owner,&input,&received):
            xir_program_receive(context,&spec,&input,NULL,&received);
        default_attack_binding=NULL;
        if(status==XR_XIR_OK)status=xir_mixed_projection_match(context,received,&projection);
        if(status==XR_XIR_OK){CHECK(!input && !lowered && received);
            received->code=(XrXirCodeLease){owner,default_mixed_release};*output=received;received=NULL;owner=NULL;}}
#endif
#if DEFAULT_MODE==3
finish:
#endif
    if(status!=XR_XIR_OK)CHECK(!*output);
    xr_xir_compile_program_drop(received);
    xir_program_input_free(input);default_mixed_free(owner);xr_xir_compile_checked_packet_free(&packet);
#if DEFAULT_MODE==1
    xr_xir_compile_c_source_free(&source);
#else
    (void)source;
#endif
    xr_xir_compile_artifact_free(checked);xr_xir_compile_artifact_free(special);xr_xir_compile_artifact_free(lowered);return status;
#endif
}
static uint8_t *default_gap_inputs(const char *packet_path,const char *c_path) {
    uint8_t *bytes=NULL;
#if DEFAULT_MODE==0
    default_packet_path=packet_path;(void)c_path;
#elif DEFAULT_MODE==1
    FILE *file=fopen(packet_path,"rb");CHECK(file && !fseek(file,0,SEEK_END));
    long length=ftell(file);CHECK(length>=64 && length<=262144 && !fseek(file,0,SEEK_SET));
    bytes=malloc((size_t)length);CHECK(bytes && fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));
    default_packet_bytes=bytes;default_packet_length=(size_t)length;default_c_path=c_path;
#else
    (void)c_path;memcpy(default_ids,default_gaps_export_indices,sizeof(default_ids));
    if(packet_path){FILE *file=fopen(packet_path,"wb");CHECK(file &&
        fwrite(DEFAULT_SPEC.packet.bytes,1,DEFAULT_SPEC.packet.length,file)==DEFAULT_SPEC.packet.length && !fclose(file));}
#endif
    return bytes;
}
#include "xir_default_gap_runtime.h"
#include "xir_default_gap_partitions.h"
int main(int argc,char **argv) {
#if DEFAULT_MODE==2
    if(argc==2 && !strcmp(argv[1],"--serial-compiler-control")){
        (void)default_gap_inputs(NULL,NULL);gap_seal_faults(default_build);return 0;
    }
#endif
    if(argc>1 && !strncmp(argv[1],"--compiler-",11))return default_gap_compiler_command(argc,argv);
    /* Explicit artifact production and the separate owned lifetime control
     * leave the original registered fault sweeps intact. */
    bool generate_only=false,owned_recovery=false,normal_only=false;
#if DEFAULT_MODE==3
    bool binding_attack=false;
#endif
#if DEFAULT_MODE<=1
    if(argc==4){
        CHECK(!strcmp(argv[1],"--generate-only") || !strcmp(argv[1],"--normal-only"));
        generate_only=!strcmp(argv[1],"--generate-only");normal_only=!generate_only;++argv;
    }else CHECK(argc==3 && strcmp(argv[1],"--generate-only"));
#elif DEFAULT_MODE==3
    if(argc==2){
        if(!strcmp(argv[1],"--normal-only"))normal_only=true;
        else if(!strcmp(argv[1],"--owned-recovery"))owned_recovery=default_owned_recovery=true;
        else{CHECK(!strcmp(argv[1],"--owned-binding-artifact") || !strcmp(argv[1],"--owned-binding-function"));
            binding_attack=true;default_binding_attack=!strcmp(argv[1],"--owned-binding-artifact")?1u:2u;}
        generate_only=!normal_only;
    }else CHECK(argc==1);
#else
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--normal-only")));normal_only=argc==2;
#endif
    XrXirCompileContext context=default_context(default_limits());XrCompileResourceStats baseline=default_stats(&context);
#if DEFAULT_MODE<=1
    uint8_t *bytes=default_gap_inputs(argv[1],argv[2]);
#else
    uint8_t *bytes=default_gap_inputs(NULL,NULL);
#endif
    XrXirProgram *program=NULL;XrXirStatus built=default_build(&context,0,&program);
#if DEFAULT_MODE==3
    if(binding_attack){
        CHECK(built==XR_XIR_BAD_STRUCTURE && !program && default_binding_callbacks>0 && !default_mixed_releases);
        default_owner_free(&context,baseline);
        printf("default binding attack%u rejected after %u real allocator callbacks; no code lease moved, physicalzero\n",
            default_binding_attack,default_binding_callbacks);return 0;
    }
#endif
    CHECK(built==XR_XIR_OK);default_pipeline_report(&context,DEFAULT_SYMBOL);
    if(!generate_only)gap_runtime_faults(program,default_ids);XrXirValue held[2]={{0},{0}};
    unsigned releases=default_mixed_releases;
#if DEFAULT_MODE==3
    if(owned_recovery){
        gap_owned_lifetime(&context,&program,default_ids,held);gap_retained(held);
        instance_compile_zero();CHECK(!runtime_live && !runtime_bytes && !runtime_owned);
    }else
#endif
    {CHECK(gap_pair(program,default_ids,held));
        xr_xir_compile_program_drop(program);gap_retained(held);default_owner_free(&context,baseline);}
    CHECK(default_mixed_releases==releases+(DEFAULT_MODE==3 ? 1u : 0u));
    default_packet_path=default_c_path=NULL;
    if(!generate_only && !normal_only)gap_seal_faults(default_build);free(bytes);
    printf("default gap mode=%u %s outputs retained physicalzero\n",DEFAULT_MODE,
        owned_recovery?"owned recovery and lifetime":generate_only?"artifact producer normal":normal_only?"normal runtime and lifetime":"all original");return 0;
}
