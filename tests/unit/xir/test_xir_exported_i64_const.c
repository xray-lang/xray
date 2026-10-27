/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_exported_i64_const.c - Real exported slot Source and cold Library owners
 */
#include "base/xresource_limits.h"
#include "base/xsha256.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_effect_contract_internal.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_exported_i64_const_cases.h"
#include "xir_exported_i64_const_runtime.h"
static char ci_root[1024];
static void ci_work(bool cold) {
    int size=snprintf(ci_root,sizeof(ci_root),"%s/%s",XR_EXPORTED_I64_CONST_WORK,cold?"cold":"source");
    CHECK(size>0 && (size_t)size<sizeof(ci_root));
}
static void ci_file(const char *logical,const char *text) {
    char path[1024];int size=snprintf(path,sizeof(path),"%s/%s",ci_root,logical);
    CHECK(size>0 && (size_t)size<sizeof(path));FILE *file=fopen(path,"wb");
    CHECK(file && fwrite(text,1,strlen(text),file)==strlen(text) && !fclose(file));
}
static XrXirStatus ci_check(const XrXirCompileContext *context,const char *text,const char *logical,
    XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog,XrXirSourceResult *result) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,ci_root};char path[1024];
    int size=snprintf(path,sizeof(path),"%s/%s",ci_root,logical);CHECK(size>0 && (size_t)size<sizeof(path));
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog};
    XrXirSourceDiagnostic diagnostic={0};XrXirStatus status;
    if(text) {
        size_t length=strlen(text);char *copy=NULL;
        CHECK(xr_compile_resources_alloc(context->resources,length+1,(void **)&copy)==XR_COMPILE_RESOURCE_OK);
        memcpy(copy,text,length+1);XrXirSourceText input={logical,copy,length};
        status=xr_xir_compile_source_check_text(&request,&input,result,&diagnostic,NULL);
        memset(copy,0xa5,length+1);xr_compile_resources_free(copy);
    } else status=xr_xir_compile_source_check(&request,result,&diagnostic,NULL);
    xr_compile_session_free(session);
    fprintf(stderr,"const %s Source%u %d:%d %s\n",logical,status,diagnostic.line,diagnostic.column,diagnostic.message);
    if(status!=XR_XIR_OK)CHECK(!result->checked && !result->snapshot);
    return status;
}
static void ci_queries(const XrXirSourceResult *result) {
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result->snapshot);CHECK(view && view->complete);
    for(uint32_t i=0;i<view->declaration_count;++i) {
        const XrXirSourceDeclaration *d=&view->declarations[i];
        if(d->kind==XR_XIR_SOURCE_FUNCTION && !d->parent)CHECK(d->name[0]!='$');
        if(d->kind==XR_XIR_SOURCE_BINDING && d->exported)CHECK(!d->mutable && d->type.type==XR_XIR_I64);
    }
}
static XrXirSourceResult ci_source(const XrXirCompileContext *context,const char *text,const char *logical,
    XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog) {
    XrXirSourceResult result={0};CHECK(ci_check(context,text,logical,kind,catalog,&result)==XR_XIR_OK);
    CHECK(result.checked && result.snapshot);ci_queries(&result);return result;
}
static XrXirLibraryCatalog *ci_catalog(const XrXirCompileContext *context,const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);uint32_t count=module->declarations->module_count;
    XrXirLibraryModuleInput *bindings=NULL;CHECK(xr_compile_resources_calloc(context->resources,count,sizeof(*bindings),(void **)&bindings)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,ci_root};
    for(uint32_t m=0;m<count;++m) {
        const XrXirSourceModule *actual=&module->declarations->modules[m];bool found=false;
        for(size_t n=0;n<sizeof(ci_logical_names)/sizeof(*ci_logical_names);++n) {
            char *canonical=NULL;CHECK(xr_compile_module_identity_from_logical(context->resources,&authority,ci_logical_names[n],&canonical)==XR_MODULE_OK);
            bool same=actual->name_length==strlen(canonical) && !memcmp(actual->name,canonical,actual->name_length);
            xr_compile_resources_free(canonical);if(!same)continue;
            size_t length=strlen(ci_logical_names[n]);char *owned=NULL;
            CHECK(xr_compile_resources_alloc(context->resources,length+1,(void **)&owned)==XR_COMPILE_RESOURCE_OK);
            memcpy(owned,ci_logical_names[n],length+1);bindings[m]=(XrXirLibraryModuleInput){authority,owned};found=true;break;
        }
        CHECK(found);
    }
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_OK);
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=bindings,.module_count=count};
    xr_sha256(packet.bytes,packet.length,input.sha256);XrXirLibraryCatalog *catalog=NULL;
    XrXirStatus status=xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog);
    fprintf(stderr,"const Catalog status%u modules%u\n",status,count);
    if(status!=XR_XIR_OK) {
        XrXirArtifact *replay=NULL;XrXirDiagnostic diagnostic={0};
        XrXirStatus read=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&replay,&diagnostic);
        fprintf(stderr,"const failed reader%u function%u block%u instruction%u\n",read,diagnostic.function,diagnostic.block,diagnostic.instruction);
        if(replay) {
            const XrXirModule *m=xr_xir_compile_artifact_module(replay);
            fprintf(stderr,"const replay stage%u linkage%u provenance%u\n",m->stage,m->linkage_kind,m->provenance?m->provenance->kind:0);
            const XrXirConstruction *construction=xr_xir_compile_artifact_construction(replay);
            for(uint32_t n=0;n<xr_xir_compile_construction_count(construction);++n)fprintf(stderr,"const construction%u default%u\n",n,xr_xir_compile_construction_row(construction,n)->default_initializer);
            for(uint32_t f=0;f<m->function_count;++f)fprintf(stderr,"const function%u %.*s module%u member%u owner%u result%u\n",f,(int)m->functions[f].name_length,m->functions[f].name,m->declarations->functions[f].module,m->declarations->functions[f].method_kind,m->declarations->functions[f].nominal_owner,m->functions[f].result);
        }
        xr_xir_compile_artifact_free(replay);
    }
    CHECK(status==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    for(uint32_t m=0;m<count;++m) {char *name=(char *)bindings[m].logical_path;memset(name,0xa5,strlen(name));xr_compile_resources_free(name);}
    memset(bindings,0xa5,count*sizeof(*bindings));xr_compile_resources_free(bindings);return catalog;
}
static XrXirLibraryCatalog *ci_cold(const XrXirCompileContext *context) {
    const char *texts[]={ci_other_source,ci_flags_source,ci_facade_source,ci_hop_source};XrXirLibraryCatalog *catalog=NULL;
    for(unsigned i=0;i<4;++i) {
        char path[1024];int length=snprintf(path,sizeof(path),"%s/%s",ci_root,ci_logical_names[i]);
        CHECK(length>0 && (size_t)length<sizeof(path));FILE *absent=fopen(path,"rb");CHECK(!absent);
        XrXirSourceResult result=ci_source(context,texts[i],ci_logical_names[i],XR_XIR_LIBRARY,catalog);
        xr_xir_compile_library_catalog_free(catalog);catalog=ci_catalog(context,result.checked);xr_xir_compile_source_result_free(&result);
    }
    return catalog;
}
static uint32_t ci_entry(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *fn=&module->functions[f];const XrXirFunctionIdentity *id=&module->declarations->functions[f];
        if(id->module==module->declarations->root_module && !id->nominal_owner && fn->name_length==strlen(name) && !memcmp(fn->name,name,fn->name_length)) {
            CHECK(found==UINT32_MAX && id->exported);found=f;
        }
    }
    CHECK(found!=UINT32_MAX);return found;
}
static void ci_flow(bool cold,const char *packet_path) {
    ci_work(cold);XrXirCompileContext context=ci_context();XrXirLibraryCatalog *catalog=NULL;
    if(cold)catalog=ci_cold(&context);
    else {ci_file("other.xr",ci_other_source);ci_file("flags.xr",ci_flags_source);ci_file("facade.xr",ci_facade_source);ci_file("hop.xr",ci_hop_source);ci_file("entry.xr",ci_consumer_source);}
    XrXirSourceResult result=ci_source(&context,cold?ci_consumer_source:NULL,"entry.xr",XR_XIR_PROGRAM,catalog);
    xr_xir_compile_library_catalog_free(catalog);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&result);
    if(packet_path){FILE *file=fopen(packet_path,"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));}
    XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    uint32_t selection[6];for(unsigned i=0;i<6;++i)selection[i]=ci_entry(xr_xir_compile_artifact_module(lowered),ci_entry_names[i]);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirValue kept=ci_runtime_cases(program,selection);xr_compile_resources_release(context.resources);ci_retained(&kept);
    fprintf(stderr,"public i64 flow %s canonical slots, cold hops and retirement PASS\n",cold?"catalog":"source");
}
static XrXirStatus ci_reowned(const XrXirCompileContext *context,const XrXirArtifact *producer,
    const XrXirSlotFacts *facts,XrXirArtifact **output) {
    const XrXirConstruction *original=xr_xir_compile_artifact_construction(producer);
    uint32_t count=xr_xir_compile_construction_count(original),materials=xr_xir_compile_construction_materialization_count(original);
    XrXirConstructionRow *rows=NULL;XrXirMaterializationBinding *bindings=NULL;
    if(count)CHECK(xr_compile_resources_alloc(context->resources,count*sizeof(*rows),(void **)&rows)==XR_COMPILE_RESOURCE_OK);
    if(materials)CHECK(xr_compile_resources_alloc(context->resources,materials*sizeof(*bindings),(void **)&bindings)==XR_COMPILE_RESOURCE_OK);
    for(uint32_t i=0;i<count;++i)rows[i]=*xr_xir_compile_construction_row(original,i);
    for(uint32_t i=0;i<materials;++i)bindings[i]=*xr_xir_compile_construction_materialization(original,i);
    const XrXirModule *module=xr_xir_compile_artifact_module(producer);
    XrXirConstructionSpec spec={rows,count,bindings,materials,xr_xir_compile_construction_comptime(original)};
    XrXirConstruction *owned=NULL;XrXirStatus status=xr_xir_compile_construction_new_v4(context,module->types,&spec,facts,&owned);
    xr_compile_resources_free(rows);xr_compile_resources_free(bindings);
    if(status==XR_XIR_OK)status=xr_xir_compile_recheck_v2(context,module,owned,output,NULL);
    xr_xir_compile_construction_free(owned);return status;
}
static void ci_receive(const XrXirCompileContext *context,const XrXirArtifact *producer,bool valid,
    const char *function_name,bool function_visible) {
    XrXirLibraryCatalog *catalog=ci_catalog(context,producer);XrXirSourceResult result={0};
    XrXirStatus status=ci_check(context,"import \"./flags\" as flags;export fn f()->i64{return flags.PROT_READ}",
        "probe.xr",XR_XIR_PROGRAM,catalog,&result);
    xr_xir_compile_library_catalog_free(catalog);
    CHECK(valid?status==XR_XIR_OK:status!=XR_XIR_OK);
    if(valid) {
        ci_queries(&result);bool found=false;const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);
        if(function_name)for(uint32_t i=0;i<view->declaration_count;++i) {
            const XrXirSourceDeclaration *d=&view->declarations[i];
            if(d->kind==XR_XIR_SOURCE_FUNCTION && !d->parent && !strcmp(d->name,function_name))found=true;
        }
        if(function_name)CHECK(found==function_visible);
    }
    xr_xir_compile_source_result_free(&result);
}
static void ci_name_cases(const XrXirCompileContext *context,const XrXirArtifact *producer) {
    const XrXirModule *module=xr_xir_compile_artifact_module(producer);uint32_t plain=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==5 && !memcmp(module->functions[f].name,"plain",5))plain=f;
    CHECK(plain!=UINT32_MAX);
    const struct {const char *name;bool visible;} cases[]={
        {"$transported",false},{"i64",false},{"_",false},{"plain ",false},{"plain/*comment*/",false},
        {"ordinaryName",true},{"caf\xc3\xa9",true},{"from",true}
    };
    XrXirFunction *functions=NULL;CHECK(xr_compile_resources_alloc(context->resources,module->function_count*sizeof(*functions),(void **)&functions)==XR_COMPILE_RESOURCE_OK);
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        memcpy(functions,module->functions,module->function_count*sizeof(*functions));
        functions[plain].name=cases[i].name;functions[plain].name_length=(uint32_t)strlen(cases[i].name);
        XrXirModule changed=*module;changed.functions=functions;XrXirArtifact *owned=NULL;
        CHECK(xr_xir_compile_recheck_v2(context,&changed,xr_xir_compile_artifact_construction(producer),&owned,NULL)==XR_XIR_OK);
        ci_receive(context,owned,true,cases[i].name,cases[i].visible);xr_xir_compile_artifact_free(owned);
    }
    xr_compile_resources_free(functions);
}
static void ci_fact_cases(const XrXirCompileContext *context,const XrXirArtifact *producer) {
    const XrXirSlotFacts *facts=xr_xir_compile_construction_slots(xr_xir_compile_artifact_construction(producer));
    const XrXirModule *module=xr_xir_compile_artifact_module(producer);CHECK(facts && facts->count);
    XrXirSlotFact *rows=NULL;CHECK(xr_compile_resources_alloc(context->resources,facts->count*sizeof(*rows),(void **)&rows)==XR_COMPILE_RESOURCE_OK);
    uint32_t pub=UINT32_MAX,peer=UINT32_MAX,private_slot=UINT32_MAX,owner=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==5 && !memcmp(module->functions[f].name,"plain",5))owner=module->declarations->functions[f].module;
    CHECK(owner!=UINT32_MAX);
    for(uint32_t i=0;i<facts->count;++i)if(facts->rows[i].name_length==9 && !memcmp(facts->rows[i].name,"PROT_READ",9) && module->declarations->slots[i].module==owner)pub=i;
    CHECK(pub!=UINT32_MAX);
    for(uint32_t i=0;i<facts->count;++i)if(i!=pub && module->declarations->slots[i].module==module->declarations->slots[pub].module) {
        if(facts->rows[i].exported)peer=i;else private_slot=i;
    }
    CHECK(peer!=UINT32_MAX && private_slot!=UINT32_MAX);
    memcpy(rows,facts->rows,facts->count*sizeof(*rows));XrXirSlotFacts input={rows,facts->count};
    /* Exact-length caller names deliberately have no byte for a terminator. */
    char **names=NULL;CHECK(xr_compile_resources_calloc(context->resources,facts->count,sizeof(*names),(void **)&names)==XR_COMPILE_RESOURCE_OK);
    for(uint32_t i=0;i<facts->count;++i) {
        CHECK(xr_compile_resources_alloc(context->resources,rows[i].name_length,(void **)&names[i])==XR_COMPILE_RESOURCE_OK);
        memcpy(names[i],rows[i].name,rows[i].name_length);rows[i].name=names[i];
    }
    XrXirArtifact *owned=NULL;CHECK(ci_reowned(context,producer,&input,&owned)==XR_XIR_OK);
    for(uint32_t i=0;i<facts->count;++i){memset(names[i],0xa5,rows[i].name_length);xr_compile_resources_free(names[i]);}
    xr_compile_resources_free(names);ci_receive(context,owned,true,NULL,false);xr_xir_compile_artifact_free(owned);
    const char *conflicts[]={facts->rows[peer].name,facts->rows[private_slot].name,"plain","Namesake","a b"};
    const uint32_t lengths[]={facts->rows[peer].name_length,facts->rows[private_slot].name_length,5,8,3};
    for(unsigned i=0;i<5;++i) {
        memcpy(rows,facts->rows,facts->count*sizeof(*rows));rows[pub].name=conflicts[i];rows[pub].name_length=lengths[i];owned=NULL;
        CHECK(ci_reowned(context,producer,&input,&owned)==XR_XIR_OK);ci_receive(context,owned,false,NULL,false);xr_xir_compile_artifact_free(owned);
    }
    memcpy(rows,facts->rows,facts->count*sizeof(*rows));rows[pub].exported=0;owned=NULL;
    CHECK(ci_reowned(context,producer,&input,&owned)==XR_XIR_OK);ci_receive(context,owned,false,NULL,false);xr_xir_compile_artifact_free(owned);
    memcpy(rows,facts->rows,facts->count*sizeof(*rows));rows[pub].slot=facts->count;owned=NULL;
    CHECK(ci_reowned(context,producer,&input,&owned)!=XR_XIR_OK && !owned);
    CHECK(xr_xir_compile_check_v2(context,module,NULL,&owned,NULL)!=XR_XIR_OK && !owned);
    xr_compile_resources_free(rows);
}
static void ci_direct_slot_negative(const XrXirCompileContext *context,const XrXirArtifact *producer) {
    const XrXirModule *module=xr_xir_compile_artifact_module(producer);uint32_t f=ci_entry(module,"all");
    XrXirFunction *functions=NULL;XrXirInstruction *ops=NULL;
    CHECK(xr_compile_resources_alloc(context->resources,module->function_count*sizeof(*functions),(void **)&functions)==XR_COMPILE_RESOURCE_OK);
    memcpy(functions,module->functions,module->function_count*sizeof(*functions));
    CHECK(xr_compile_resources_alloc(context->resources,functions[f].instruction_count*sizeof(*ops),(void **)&ops)==XR_COMPILE_RESOURCE_OK);
    memcpy(ops,functions[f].instructions,functions[f].instruction_count*sizeof(*ops));functions[f].instructions=ops;bool changed=false;
    for(uint32_t i=0;i<functions[f].instruction_count;++i)if(ops[i].op==XR_XIR_CALL) {
        const XrXirFunction *callee=&module->functions[ops[i].immediate];
        if(callee->instruction_count && callee->instructions[0].op==XR_XIR_SLOT_LOAD) {
            ops[i]=(XrXirInstruction){XR_XIR_SLOT_LOAD,XR_XIR_I64,{0},{0},callee->instructions[0].immediate,{0}};changed=true;break;
        }
    }
    CHECK(changed);XrXirModule invalid=*module;invalid.functions=functions;XrXirArtifact *rejected=NULL;
    CHECK(xr_xir_compile_recheck_v2(context,&invalid,xr_xir_compile_artifact_construction(producer),&rejected,NULL)!=XR_XIR_OK && !rejected);
    xr_compile_resources_free(ops);xr_compile_resources_free(functions);
}
static void ci_authority(void) {
    ci_work(true);XrXirCompileContext context=ci_context();XrXirLibraryCatalog *catalog=ci_cold(&context);
    for(size_t i=0;i<sizeof(ci_rejections)/sizeof(*ci_rejections);++i) {
        XrXirSourceResult refused={0};CHECK(ci_check(&context,ci_rejections[i],"refused.xr",XR_XIR_PROGRAM,catalog,&refused)!=XR_XIR_OK);
        xr_xir_compile_source_result_free(&refused);
    }
    XrXirSourceResult result=ci_source(&context,ci_consumer_source,"entry.xr",XR_XIR_PROGRAM,catalog);
    ci_direct_slot_negative(&context,result.checked);xr_xir_compile_source_result_free(&result);xr_xir_compile_library_catalog_free(catalog);
    XrXirSourceResult other=ci_source(&context,ci_other_source,"other.xr",XR_XIR_LIBRARY,NULL);
    catalog=ci_catalog(&context,other.checked);xr_xir_compile_source_result_free(&other);
    result=ci_source(&context,ci_flags_source,"flags.xr",XR_XIR_LIBRARY,catalog);xr_xir_compile_library_catalog_free(catalog);
    ci_name_cases(&context,result.checked);ci_fact_cases(&context,result.checked);xr_xir_compile_source_result_free(&result);
    xr_compile_resources_release(context.resources);fprintf(stderr,"public i64 complete facts, namespace and permissions PASS\n");
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--authority")){ci_authority();return 0;}
    CHECK(argc==1 || argc==3);ci_flow(false,argc==3?argv[1]:NULL);ci_flow(true,argc==3?argv[2]:NULL);return 0;
}
