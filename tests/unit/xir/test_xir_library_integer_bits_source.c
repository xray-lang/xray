/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_library_integer_bits_source.c - Complete Source Libraries and cold integer instruction reception
 */
#include "base/xresource_limits.h"
#include "base/xsha256.h"
#include "module/xmodule_identity.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_library_integer_bits_cases.h"
#include "xir_library_integer_bits_runtime.h"
static const char *const bits_entry_names[]={"andEntry","orEntry","xorEntry","notEntry","leftEntry","rightEntry"};
static void bits_ids(const XrXirModule *module,uint32_t selection[6]) {
    for(unsigned s=0;s<6;++s) {
        selection[s]=UINT32_MAX;
        for(uint32_t f=0;f<module->function_count;++f) {
            const XrXirFunction *fn=&module->functions[f];const XrXirFunctionIdentity *id=&module->declarations->functions[f];
            if(id->module==module->declarations->root_module && !id->nominal_owner && fn->name_length==strlen(bits_entry_names[s]) &&
                !memcmp(fn->name,bits_entry_names[s],fn->name_length)) {
                CHECK(selection[s]==UINT32_MAX && id->exported);selection[s]=f;
            }
        }
        CHECK(selection[s]!=UINT32_MAX);
    }
}

static XrXirStatus bits_check(const XrXirCompileContext *context,const char *text,const char *logical,
    XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog,XrXirSourceResult *result) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_LIBRARY_INTEGER_BITS_WORK};char path[1024];
    int size=snprintf(path,sizeof(path),"%s/%s",XR_LIBRARY_INTEGER_BITS_WORK,logical);CHECK(size>0 && (size_t)size<sizeof(path));
    FILE *absent=fopen(path,"rb");CHECK(!absent);
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog};
    size_t length=strlen(text);char *copy=NULL;
    CHECK(xr_compile_resources_alloc(context->resources,length+1,(void **)&copy)==XR_COMPILE_RESOURCE_OK);
    memcpy(copy,text,length+1);XrXirSourceText input={logical,copy,length};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,result,&diagnostic,NULL);
    memset(copy,0xa5,length+1);xr_compile_resources_free(copy);xr_compile_session_free(session);
    fprintf(stderr,"integer bits Source %s status%u %s\n",logical,status,diagnostic.message);
    if(status!=XR_XIR_OK)CHECK(!result->checked && !result->snapshot);return status;
}
static XrXirSourceResult bits_source(const XrXirCompileContext *context,const char *text,const char *logical,
    XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog) {
    XrXirSourceResult result={0};CHECK(bits_check(context,text,logical,kind,catalog,&result)==XR_XIR_OK);
    CHECK(result.checked && result.snapshot && xr_xir_compile_artifact_verify(result.checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_module(result.checked)->declarations->slot_count==0);return result;
}
static XrXirLibraryCatalog *bits_catalog(const XrXirCompileContext *context,const XrXirArtifact *producer) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(producer,&packet,NULL)==XR_XIR_OK);
    XrXirArtifact *read=NULL;XrXirStatus reading=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);
    CHECK(reading==XR_XIR_OK && read);const XrXirModule *module=xr_xir_compile_artifact_module(read);
    CHECK(module->stage==XR_XIR_CHECKED && module->linkage_kind==XR_XIR_LIBRARY);
    uint32_t count=module->declarations->module_count;XrXirLibraryModuleInput *bindings=NULL;
    CHECK(xr_compile_resources_calloc(context->resources,count,sizeof(*bindings),(void **)&bindings)==XR_COMPILE_RESOURCE_OK);
    const char *names[]={"base.xr","bits.xr"};XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_LIBRARY_INTEGER_BITS_WORK};
    for(uint32_t m=0;m<count;++m) {
        const XrXirSourceModule *actual=&module->declarations->modules[m];bool found=false;
        for(unsigned n=0;n<2;++n) {
            char *canonical=NULL;CHECK(xr_compile_module_identity_from_logical(context->resources,&authority,names[n],&canonical)==XR_MODULE_OK);
            bool same=actual->name_length==strlen(canonical) && !memcmp(actual->name,canonical,actual->name_length);
            xr_compile_resources_free(canonical);if(!same)continue;
            size_t length=strlen(names[n]);char *owned=NULL;
            CHECK(xr_compile_resources_alloc(context->resources,length+1,(void **)&owned)==XR_COMPILE_RESOURCE_OK);
            memcpy(owned,names[n],length+1);bindings[m]=(XrXirLibraryModuleInput){authority,owned};found=true;break;
        }
        CHECK(found);
    }
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=bindings,.module_count=count};
    xr_sha256(packet.bytes,packet.length,input.sha256);XrXirLibraryCatalog *catalog=NULL;
    XrXirStatus status=xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog);
    fprintf(stderr,"integer bits complete Library modules%u checked_read%u Catalog%u\n",count,reading,status);
    fflush(stderr);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(read);
    for(uint32_t m=0;m<count;++m){char *name=(char *)bindings[m].logical_path;memset(name,0xa5,strlen(name));xr_compile_resources_free(name);}
    memset(bindings,0xa5,count*sizeof(*bindings));xr_compile_resources_free(bindings);
    CHECK(status==XR_XIR_OK && catalog);return catalog;
}
static void bits_operations(const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);unsigned seen=0;bool complement=false;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *fn=&module->functions[f];bool not_function=fn->name_length==6 && !memcmp(fn->name,"bitNot",6);
        bool all_ones=false,xor_value=false;
        for(uint32_t i=0;i<fn->instruction_count;++i) {
            const XrXirInstruction *op=&fn->instructions[i];
            switch(op->op) {
            case XR_XIR_AND_INT:seen|=1;break;case XR_XIR_OR_INT:seen|=2;break;
            case XR_XIR_XOR_INT:seen|=4;xor_value=true;break;
            case XR_XIR_SHL_INT:seen|=8;break;case XR_XIR_SHR_INT:seen|=16;break;
            default:break;
            }
            if(op->op==XR_XIR_CONST_INT && op->type==XR_XIR_I64 && op->immediate==-1)all_ones=true;
        }
        if(not_function && all_ones && xor_value)complement=true;
    }
    CHECK(seen==31 && complement);fprintf(stderr,"integer bits actual five body operations and NOT via XOR PASS\n");
}
static void bits_common_rejects(const XrXirCompileContext *context,const XrXirArtifact *producer) {
    const XrXirModule *module=xr_xir_compile_artifact_module(producer);uint32_t function=UINT32_MAX,index=0;
    for(uint32_t f=0;f<module->function_count && function==UINT32_MAX;++f)
        for(uint32_t i=0;i<module->functions[f].instruction_count;++i)
            if(module->functions[f].instructions[i].op==XR_XIR_AND_INT){function=f;index=i;break;}
    CHECK(function!=UINT32_MAX);XrXirFunction *functions=NULL;XrXirInstruction *ops=NULL;
    CHECK(xr_compile_resources_alloc(context->resources,module->function_count*sizeof(*functions),(void **)&functions)==XR_COMPILE_RESOURCE_OK);
    memcpy(functions,module->functions,module->function_count*sizeof(*functions));uint32_t count=functions[function].instruction_count;
    CHECK(xr_compile_resources_alloc(context->resources,count*sizeof(*ops),(void **)&ops)==XR_COMPILE_RESOURCE_OK);
    memcpy(ops,functions[function].instructions,count*sizeof(*ops));functions[function].instructions=ops;
    XrXirModule invalid=*module;invalid.functions=functions;ops[index].args[0]=UINT32_MAX;XrXirArtifact *rejected=NULL;
    CHECK(xr_xir_compile_recheck_v2(context,&invalid,xr_xir_compile_artifact_construction(producer),&rejected,NULL)!=XR_XIR_OK && !rejected);
    xr_compile_resources_free(ops);xr_compile_resources_free(functions);
    XrXirSourceResult refused={0};
    CHECK(bits_check(context,"export fn bad(a:i64,b:bool)->i64{return a&b}","wrong-type.xr",XR_XIR_LIBRARY,NULL,&refused)!=XR_XIR_OK);
    xr_xir_compile_source_result_free(&refused);
}
static XrXirArtifact *bits_receive(const XrXirCompileContext *context,const XrXirCheckedPacket *packet) {
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet->bytes,packet->length,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);return lowered;
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);XrXirCompileContext context=bits_context();
    XrXirSourceResult base=bits_source(&context,bits_base_source,"base.xr",XR_XIR_LIBRARY,NULL);
    XrXirLibraryCatalog *first=bits_catalog(&context,base.checked);xr_xir_compile_source_result_free(&base);
    XrXirSourceResult library=bits_source(&context,bits_library_source,"bits.xr",XR_XIR_LIBRARY,first);
    xr_xir_compile_library_catalog_free(first);bits_operations(library.checked);bits_common_rejects(&context,library.checked);
    XrXirLibraryCatalog *second=bits_catalog(&context,library.checked);xr_xir_compile_source_result_free(&library);
    XrXirSourceResult root=bits_source(&context,bits_root_source,"root.xr",XR_XIR_PROGRAM,second);
    xr_xir_compile_library_catalog_free(second);bits_operations(root.checked);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(root.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&root);
    if(argc==2){FILE *file=fopen(argv[1],"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));}
    XrXirArtifact *lowered=bits_receive(&context,&packet);memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    uint32_t selection[6];bits_ids(xr_xir_compile_artifact_module(lowered),selection);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirValue kept=bits_runtime_cases(program,selection);xr_compile_resources_release(context.resources);bits_retained(&kept);
    fprintf(stderr,"integer bits cold two-Library chain, wire and retired inputs PASS\n");return 0;
}
