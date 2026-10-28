/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_range_value.c - Real Source and cold Catalog range values and definitions
 */
#include "base/xresource_limits.h"
#include "base/xsha256.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_range_value_cases.h"
#include "xir_range_value_runtime.h"
#define RANGE_PRELUDE_CANONICAL "stdlib-module-v1:module=7:prelude:path=27:prelude/builtin_symbols.def"
static void range_file(const char *logical,const void *bytes,size_t size) {
    char path[1024];int length=snprintf(path,sizeof(path),"%s/%s",XR_RANGE_VALUE_WORK,logical);
    CHECK(length>0 && (size_t)length<sizeof(path));FILE *file=fopen(path,"wb");
    CHECK(file && fwrite(bytes,1,size,file)==size && !fclose(file));
}
static XrXirStatus range_check(const XrXirCompileContext *context,const char *text,const char *logical,
    XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog,XrXirSourceResult *result) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_RANGE_VALUE_WORK};char path[1024];
    int length=snprintf(path,sizeof(path),"%s/%s",XR_RANGE_VALUE_WORK,logical);CHECK(length>0 && (size_t)length<sizeof(path));
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog};
    XrXirSourceDiagnostic diagnostic={0};XrXirStatus status;
    if(text) {
        size_t size=strlen(text);char *copy=NULL;
        CHECK(xr_compile_resources_alloc(context->resources,size+1,(void **)&copy)==XR_COMPILE_RESOURCE_OK);
        memcpy(copy,text,size+1);XrXirSourceText input={logical,copy,size};
        status=xr_xir_compile_source_check_text(&request,&input,result,&diagnostic,NULL);
        memset(copy,0xa5,size+1);xr_compile_resources_free(copy);
    } else status=xr_xir_compile_source_check(&request,result,&diagnostic,NULL);
    xr_compile_session_free(session);
    fprintf(stderr,"Range %s Source%u %d:%d %s\n",logical,status,diagnostic.line,diagnostic.column,diagnostic.message);
    if(status!=XR_XIR_OK) CHECK(!result->checked && !result->snapshot && diagnostic.line>0);
    return status;
}
static XrXirSourceResult range_source(const XrXirCompileContext *context,const char *text,const char *logical,
    XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog) {
    XrXirSourceResult result={0};CHECK(range_check(context,text,logical,kind,catalog,&result)==XR_XIR_OK);
    CHECK(result.checked && result.snapshot);return result;
}
static XrXirLibraryCatalog *range_catalog(const XrXirCompileContext *context,const XrXirArtifact *artifact,bool valid) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);uint32_t count=module->declarations->module_count;
    XrXirLibraryModuleInput *bindings=NULL;CHECK(xr_compile_resources_calloc(context->resources,count,sizeof(*bindings),(void **)&bindings)==XR_COMPILE_RESOURCE_OK);
    const XrModuleIdentityAuthority script={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_RANGE_VALUE_WORK};
    char *canonical=NULL;CHECK(xr_compile_module_identity_from_logical(context->resources,&script,"range-lib.xr",&canonical)==XR_MODULE_OK);
    for(uint32_t m=0;m<count;++m) {
        const XrXirSourceModule *actual=&module->declarations->modules[m];
        fprintf(stderr,"Range library module%u root%u %.*s\n",m,module->declarations->root_module,(int)actual->name_length,actual->name);
        if(actual->name_length==strlen(RANGE_PRELUDE_CANONICAL) && !memcmp(actual->name,RANGE_PRELUDE_CANONICAL,actual->name_length))
            bindings[m]=(XrXirLibraryModuleInput){{XR_MODULE_IDENTITY_STDLIB,"prelude",XR_RANGE_VALUE_STDLIB},"prelude/builtin_symbols.def"};
        else {CHECK(actual->name_length==strlen(canonical) && !memcmp(actual->name,canonical,actual->name_length));
            bindings[m]=(XrXirLibraryModuleInput){script,"range-lib.xr"};}
    }
    xr_compile_resources_free(canonical);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_OK);
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=bindings,.module_count=count};
    xr_sha256(packet.bytes,packet.length,input.sha256);XrXirLibraryCatalog *catalog=NULL;
    XrXirStatus status=xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog);
    fprintf(stderr,"Range Catalog status%u expected-valid%u\n",status,valid?1u:0u);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);xr_compile_resources_free(bindings);
    CHECK(valid?(status==XR_XIR_OK && catalog):(status==XR_XIR_BAD_STRUCTURE && !catalog));return catalog;
}
static uint32_t range_entry(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];const XrXirFunctionIdentity *id=&module->declarations->functions[f];
        if(id->module==module->declarations->root_module && !id->nominal_owner && body->name_length==strlen(name) &&
            !memcmp(body->name,name,body->name_length)){CHECK(found==UINT32_MAX && id->exported);found=f;}
    }
    CHECK(found!=UINT32_MAX);return found;
}
static void range_definition_negatives(const XrXirCompileContext *context,const XrXirArtifact *producer) {
    const XrXirModule *original=xr_xir_compile_artifact_module(producer);uint32_t ctor=UINT32_MAX,count=UINT32_MAX;
    for(uint32_t f=0;f<original->function_count;++f) {
        uint32_t owner=original->declarations->functions[f].nominal_owner;if(!owner)continue;
        const XrXirNominalDeclaration *d=&original->types->nominals->declarations[owner-1];
        if(d->name.length!=5 || memcmp(d->name.bytes,"Range",5))continue;
        const XrXirFunction *body=&original->functions[f];
        if(body->name_length==4 && !memcmp(body->name,"init",4))ctor=f;
        if(body->name_length==5 && !memcmp(body->name,"count",5))count=f;
    }
    CHECK(ctor!=UINT32_MAX && count!=UINT32_MAX);
    for(unsigned mode=0;mode<2;++mode) {
        XrXirFunction *functions=NULL;CHECK(xr_compile_resources_alloc(context->resources,
            (size_t)original->function_count*sizeof(*functions),(void **)&functions)==XR_COMPILE_RESOURCE_OK);
        memcpy(functions,original->functions,(size_t)original->function_count*sizeof(*functions));
        XrXirInstruction *instructions=NULL;uint32_t operands[4]={13,12,14,15};
        if(!mode){CHECK(functions[ctor].operand_count==4);functions[ctor].operands=operands;}
        else {
            XrXirFunction *body=&functions[count];CHECK(xr_compile_resources_alloc(context->resources,
                (size_t)body->instruction_count*sizeof(*instructions),(void **)&instructions)==XR_COMPILE_RESOURCE_OK);
            memcpy(instructions,body->instructions,(size_t)body->instruction_count*sizeof(*instructions));body->instructions=instructions;
            bool changed=false;for(uint32_t i=0;i<body->instruction_count;++i)if(instructions[i].op==XR_XIR_CONST_INT && instructions[i].immediate==INT64_MAX){instructions[i].immediate=INT64_MAX-1;changed=true;}
            CHECK(changed);
        }
        XrXirModule altered=*original;altered.functions=functions;XrXirArtifact *owned=NULL;
        CHECK(xr_xir_compile_recheck_v2(context,&altered,xr_xir_compile_artifact_construction(producer),&owned,NULL)==XR_XIR_OK);
        CHECK(!range_catalog(context,owned,false));xr_xir_compile_artifact_free(owned);
        xr_compile_resources_free(instructions);xr_compile_resources_free(functions);
    }
}
static void range_authority(void) {
    XrXirCompileContext context=range_context();
    const char *positive="type R=Range\nexport fn f(r:R)->i64{return r.start+r.end+r.count()+len(r)}";
    XrXirSourceResult result=range_source(&context,positive,"typed.xr",XR_XIR_PROGRAM,NULL);xr_xir_compile_source_result_free(&result);
    result=range_source(&context,range_constructor_source,"generic.xr",XR_XIR_PROGRAM,NULL);
    XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(result.checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&result);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    uint32_t entry=range_entry(xr_xir_compile_artifact_module(lowered),"generic");
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue answer=range_take(instance,entry,NULL,0);range_integer(&answer,60);
    xr_xir_compile_program_drop(program);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    for(size_t i=0;i<sizeof(range_rejections)/sizeof(*range_rejections);++i) {
        XrXirSourceResult refused={0};CHECK(range_check(&context,range_rejections[i],"refused.xr",XR_XIR_PROGRAM,NULL,&refused)!=XR_XIR_OK);
        xr_xir_compile_source_result_free(&refused);
    }
    result=range_source(&context,range_library_source,"range-lib.xr",XR_XIR_LIBRARY,NULL);
    range_definition_negatives(&context,result.checked);xr_xir_compile_source_result_free(&result);
    xr_compile_resources_release(context.resources);
}
static void range_flow(unsigned mode,const char *packet_path) {
    XrXirCompileContext context=range_context();XrXirLibraryCatalog *catalog=NULL;
    range_file("range-lib.xr",range_library_source,strlen(range_library_source));
    if(mode==2) {
        XrXirSourceResult producer=range_source(&context,range_library_source,"range-lib.xr",XR_XIR_LIBRARY,NULL);
        catalog=range_catalog(&context,producer.checked,true);xr_xir_compile_source_result_free(&producer);
    }
    const char *logical=mode==1?"entry-file.xr":"receiver.xr";
    if(mode==1)range_file(logical,range_consumer_source,strlen(range_consumer_source));
    XrXirSourceResult source=range_source(&context,mode==1?NULL:range_consumer_source,logical,XR_XIR_PROGRAM,catalog);
    xr_xir_compile_library_catalog_free(catalog);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    if(packet_path){FILE *file=fopen(packet_path,"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));}
    XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t selection[9];
    for(unsigned f=0;f<9;++f)selection[f]=range_entry(module,range_entry_names[f]);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirValue kept=range_runtime_cases(program,selection);xr_compile_resources_release(context.resources);range_retained(&kept);
    fprintf(stderr,"Range flow%u complete: actual fields/count/len, once, all producer deaths\n",mode);
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--authority")){range_authority();return 0;}
    CHECK(argc==1 || argc==3);
    range_flow(0,argc==3?argv[1]:NULL);range_flow(1,NULL);range_flow(2,argc==3?argv[2]:NULL);return 0;
}
