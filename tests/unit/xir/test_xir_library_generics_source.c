/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_library_generics_source.c - Checked generic Library import and query ownership
 */
#include "base/xmalloc.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "toolchain/xcompiler_session.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_library_compile_owner.h"
#include "xir/xxir_source_query.c"
#include "xir/xxir_type_inference.c"
#include "xir/xxir_source.c"
#include "xir_library_generics_fixture.h"
#include "xir_library_generics_runtime.h"
static void text(const char *name,const char *data) {
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_GENERIC_LIBRARY_WORK,name)>0);
    FILE *file=fopen(path,"wb");CHECK(file);size_t n=strlen(data);
    CHECK(fwrite(data,1,n,file)==n&&!fclose(file));
}
static XrXirStatus source(const XrXirCompileContext *context,const XrXirLibraryCatalog *catalog,
    const char *name,XrXirSourceResult *result) {
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_GENERIC_LIBRARY_WORK,name)>0);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_GENERIC_LIBRARY_WORK};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceDiagnostic diagnostic={0};XrXirStatus status=xr_xir_compile_source_check(&request,result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"source %s status%u module%u line%d col%d %s\n",name,status,diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static void query(const XrXirSourceResult *result) {
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result->snapshot);CHECK(view&&view->complete);
    unsigned identities=0,helpers=0;
    for(uint32_t i=0;i<view->declaration_count;++i){const XrXirSourceDeclaration *d=&view->declarations[i];
        if(d->kind!=XR_XIR_SOURCE_FUNCTION)continue;
        if(!strcmp(d->name,"identity"))++identities;
        else if(!strcmp(d->name,"helper"))++helpers;else continue;
        CHECK(d->generic_parameter_count==1&&!d->generic_parent&&!d->generic_parent_count);
        CHECK(d->generic_constraints&&d->parameter_count==1&&d->parameters);
        CHECK(d->type.generic_owner==d->id&&d->parameters[0].generic_owner==d->id);
        CHECK(d->type.type==XR_XIR_TYPE_PARAMETER_BASE&&d->parameters[0].type==XR_XIR_TYPE_PARAMETER_BASE);
        CHECK(d->exported==(!strcmp(d->name,"identity")));
    }
    CHECK(identities==2&&helpers==2);
}
int main(int argc,char **argv) {
    CHECK(argc==1||argc==2);
    text("user.xr","export enum Envelope { Ready { value:i64 } }\n");
    const char *imports[]={
        "import {identity as left} from \"./alpha\";\nimport {identity as right} from \"./beta\";\n",
        "import {identity as right} from \"./beta\";\nimport {identity as left} from \"./alpha\";\n"};
    const char *body=
        "import {Envelope} from \"./user\";\n"
        "fn forward<T:Sendable>(value:T)->T{return left<T>(right(value));}\n"
        "export fn main_i64()->i64{return forward(21)+right<i64>(21);}\n"
        "export fn main_string()->string{return left<string>(\"A\\0B\");}\n"
        "export fn main_bool()->bool{return right(false);}\n"
        "export fn main_box()->Envelope{return forward(Envelope.Ready{value:73});}\n";
    for(unsigned mode=0;mode<4;++mode){
        LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_GENERIC_LIBRARY_WORK};
        const char *names[]={"alpha.xr","beta.xr"};XrXirCheckedPacket packets[2]={{0}};XrXirLibraryInput inputs[2]={{0}};XrXirLibraryModuleInput bindings[2]={{0}};
        for(unsigned i=0;i<2;++i){
            char *id=NULL;CHECK(xr_compile_module_identity_from_logical(owner.context.resources,&authority,names[i],&id)==XR_MODULE_OK);
            XrXirArtifact *checked=NULL;CHECK(generic_library_fixture(&owner.context,id,i==0,false,&checked)==XR_XIR_OK);
            CHECK(xr_xir_compile_checked_write(checked,&packets[i],NULL)==XR_XIR_OK);
            xr_xir_compile_artifact_free(checked);
            bindings[i]=(XrXirLibraryModuleInput){authority,names[i]};
            inputs[i]=(XrXirLibraryInput){packets[i].bytes,packets[i].length,{0}, &bindings[i],1};
            xr_sha256(packets[i].bytes,packets[i].length,inputs[i].sha256);
            if(!i){XrXirArtifact *bad=NULL;CHECK(generic_library_fixture(&owner.context,id,true,true,&bad)==XR_XIR_BAD_TYPE&&!bad);}
            if(mode==0&&!i)for(unsigned family=0;family<3;++family){
                XrXirArtifact *unsupported=NULL;
                XrXirStatus status=generic_library_admission_fixture(&owner.context,id,family,&unsupported);
                if(family==2){CHECK(status==XR_XIR_BAD_TYPE&&!unsupported);puts("RESULT free binder Checked BAD_TYPE PASS");continue;}
                CHECK(status==XR_XIR_OK&&unsupported);
                XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(unsupported,&packet,NULL)==XR_XIR_OK);
                xr_xir_compile_artifact_free(unsupported);
                XrXirLibraryInput rejected_input={packet.bytes,packet.length,{0}, (XrXirLibraryModuleInput[]){{authority,names[i]}},1};
                xr_sha256(packet.bytes,packet.length,rejected_input.sha256);
                XrXirLibraryCatalog *typed_catalog=NULL;
                CHECK(xr_xir_compile_library_catalog_new_v2(&owner.context,&rejected_input,1,&typed_catalog)==XR_XIR_OK&&typed_catalog);
                xr_xir_compile_checked_packet_free(&packet);
                size_t typed_count=0;
                const XrModuleResourceBinding *typed=xr_xir_compile_library_catalog_resources_v2(typed_catalog,&typed_count);
                CHECK(typed&&typed_count==1&&typed->checked);
                const XrXirTypes *owned_types=xr_xir_compile_artifact_module(typed->checked)->types;
                CHECK(owned_types&&xr_xir_compile_artifact_construction(typed->checked));
                if(!family)CHECK(owned_types->count==1&&owned_types->nodes[0].kind==XR_XIR_TYPE_ARRAY&&owned_types->nodes[0].element==XR_XIR_I64);
                else CHECK(owned_types->interfaces&&owned_types->interfaces->count==1&&owned_types->interfaces->declarations[0].name.length==8&&
                    !memcmp(owned_types->interfaces->declarations[0].name.bytes,"Boundary",8));
                xr_xir_compile_library_catalog_free(typed_catalog);
                printf("genuine Checked Library family%u owned typed Catalog producer-death PASS\n",family);
            }
            xr_compile_resources_free(id);
        }
        if(mode&1){XrXirLibraryInput swap=inputs[0];inputs[0]=inputs[1];inputs[1]=swap;}
        XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(&owner.context,inputs,2,&catalog)==XR_XIR_OK);
        for(unsigned i=0;i<2;++i){memset(packets[i].bytes,0,packets[i].length);xr_xir_compile_checked_packet_free(&packets[i]);}
        char program[4096];CHECK(snprintf(program,sizeof(program),"%s%s%s",
            mode&2?"fn prefix()->i64{return 99;}\n":"",imports[mode&1],body)>0);
        text("root.xr",program);
        text("private.xr","import {helper} from \"./alpha\";export fn main()->i64{return helper(21);}\n");
        XrXirSourceResult rejected={0};CHECK(source(&owner.context,catalog,"private.xr",&rejected)==XR_XIR_BAD_STRUCTURE);
        CHECK(!rejected.checked&&!rejected.snapshot);
        text("constraint.xr","import {identity} from \"./alpha\";\nfn bad<T>(value:T)->T{return identity<T>(value);}\nexport fn main()->i64{return 1;}\n");
        CHECK(source(&owner.context,catalog,"constraint.xr",&rejected)==XR_XIR_BAD_TYPE);CHECK(!rejected.checked&&!rejected.snapshot);
        XrXirSourceResult result={0};CHECK(source(&owner.context,catalog,"root.xr",&result)==XR_XIR_OK);
        xr_xir_compile_library_catalog_free(catalog);
        query(&result);XrXirArtifact *owned=result.checked;result.checked=NULL;
        xr_xir_compile_source_result_free(&result);
        CHECK(xr_xir_compile_artifact_verify(owned,NULL)==XR_XIR_OK);
        char generated[1024],prefix[64];
        CHECK(snprintf(prefix,sizeof(prefix),"generic_library_%u",mode)>0);
        CHECK(argc!=2||snprintf(generated,sizeof(generated),"%s.%u.c",argv[1],mode)>0);
        library_generics_execute_owned(owned,prefix,argc==2?generated:NULL);
        library_compile_owner_drop(&owner);
        printf("generic Library mode%u source-free private closure VM fixed 42/NUL3/false/Enum73 PASS\n",mode);
    }
    library_compile_observer_free();return 0;
}
