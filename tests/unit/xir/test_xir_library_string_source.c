/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_library_string_source.c - Owned multi-library string source import
 *
 * KEY CONCEPT:
 *   Independent library literals survive every producer before real execution.
 */
#include "base/xmalloc.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_library_compile_owner.h"
#include "xir/xxir_source_query.c"
#include "xir/xxir_type_inference.c"
#include "xir/xxir_source.c"
#include "xir_library_string_runtime.h"
#include "xir/xxir_library_catalog.h"
#include "base/xsha256.h"
#include "xir_library_string_goldens.h"
#include "xir_library_source_owner.h"
#include "xir_library_string_source_cases.h"

static void string_library_packet(const XrXirCompileContext *context,const char *name, const char *expected, XrXirCheckedPacket *packet) {
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_SOURCE_FIXTURES,name)>0);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"library %s: %u %s\n",name,status,diagnostic.message);
    CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
    const XrXirDeclarations *d=xr_xir_compile_artifact_module(result.checked)->declarations;
    CHECK(d&&d->literal_count&&d->literals[0].length==9&&!memcmp(d->literals[0].bytes,expected,9));
    CHECK(xr_xir_compile_checked_write(result.checked,packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);

}
static void string_snapshot_owned(XrXirSourceSnapshot *snapshot) {
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(snapshot);CHECK(view&&view->complete);
    bool left=false,alpha=false;
    for(uint32_t i=0;i<view->declaration_count;++i){
        const XrXirSourceDeclaration *d=&view->declarations[i];
        if(!strcmp(d->name,"left")){CHECK(d->type.known);left=true;}
        if(!strcmp(d->name,"alpha"))alpha=true;
    }
    CHECK(left&&alpha);xr_xir_compile_source_snapshot_free(snapshot);
}
static void string_source_case(unsigned reverse,const char *output_path) {
    LibraryCompileOwner compiler={0};CHECK(library_compile_owner_new(&compiler,&library_compile_limits)==XR_XIR_OK);
    XrXirCheckedPacket packets[2]={{0}};
    string_library_packet(&compiler.context,"alpha.xr","甲-owned",&packets[0]);
    string_library_packet(&compiler.context,"beta.xr","乙-owned",&packets[1]);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    char names[5][16]={"alpha.xr","beta.xr","nul.xr","empty.xr","long.xr"};
    char catalog_root[1024];CHECK(strlen(XR_SOURCE_FIXTURES)<sizeof(catalog_root));strcpy(catalog_root,XR_SOURCE_FIXTURES);
    XrModuleIdentityAuthority owned_authority={XR_MODULE_IDENTITY_SCRIPT,NULL,catalog_root};
    const unsigned char *manual[3]={library_string_nul,library_string_empty,library_string_long};
    size_t lengths[3]={sizeof(library_string_nul),sizeof(library_string_empty),sizeof(library_string_long)};
    void *buffers[3]={0};XrXirLibraryInput inputs[5]={0};
    for(unsigned i=0;i<5;++i){inputs[i].authority=owned_authority;inputs[i].logical_path=names[i];
        if(i<2){inputs[i].packet=packets[i].bytes;inputs[i].length=packets[i].length;}
        else {buffers[i-2]=xr_malloc(lengths[i-2]);CHECK(buffers[i-2]);memcpy(buffers[i-2],manual[i-2],lengths[i-2]);
            inputs[i].packet=buffers[i-2];inputs[i].length=lengths[i-2];}
        xr_sha256(inputs[i].packet,inputs[i].length,inputs[i].sha256);}
    if(reverse)for(unsigned i=0;i<2;++i){XrXirLibraryInput swap=inputs[i];inputs[i]=inputs[4-i];inputs[4-i]=swap;}
    /* Fault replays borrow independent bounded copies, never the Catalog's owned storage. */
    XrXirLibraryInput replay_inputs[5];void *replay_bytes[5]={0};const char *replay_names[5]={"alpha.xr","beta.xr","nul.xr","empty.xr","long.xr"};
    for(unsigned i=0;i<5;++i){replay_inputs[i]=inputs[i];replay_inputs[i].authority=authority;
        unsigned n=reverse?4-i:i;replay_inputs[i].logical_path=replay_names[n];
        replay_bytes[i]=malloc(inputs[i].length);CHECK(replay_bytes[i]);memcpy(replay_bytes[i],inputs[i].packet,inputs[i].length);replay_inputs[i].packet=replay_bytes[i];}
    LibrarySourceFixture replay={replay_inputs,5,XR_SOURCE_FIXTURES "/root.xr",authority,XR_XIR_PROGRAM};
    library_compile_operation_cases(reverse?"String Source reverse catalog":"String Source forward catalog",library_catalog_operation,&replay);
    library_compile_operation_cases(reverse?"String Source reverse whole":"String Source forward whole",library_source_operation,&replay);
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new(&compiler.context,inputs,5,&catalog)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packets[0]);xr_xir_compile_checked_packet_free(&packets[1]);
    for(unsigned i=0;i<3;++i){memset(buffers[i],0,lengths[i]);xr_free(buffers[i]);}
    memset(inputs,0,sizeof(inputs));memset(names,0,sizeof(names));memset(catalog_root,0,sizeof(catalog_root));
    for(unsigned i=0;i<5;++i){memset(replay_bytes[i],0,replay_inputs[i].length);free(replay_bytes[i]);}
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(compiler.context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,&compiler.context,NULL,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    if(!reverse)library_string_source_negatives(&request);
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"root %u: %u %s\n",reverse,status,diagnostic.message);
    CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
    XrXirArtifact *owned=result.checked;result.checked=NULL;XrXirSourceSnapshot *snapshot=result.snapshot;result.snapshot=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    string_snapshot_owned(snapshot);
    CHECK(xr_xir_compile_artifact_verify(owned,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(owned,&packet,NULL)==XR_XIR_OK);
    if(output_path){FILE *f=fopen(output_path,"wb");CHECK(f);CHECK(fwrite(packet.bytes,1,packet.length,f)==packet.length&&!fclose(f));}
    xr_xir_compile_checked_packet_free(&packet);
    xr_test_library_string_run(&compiler.context,owned);
    CHECK(!runtime_live&&!runtime_bytes);library_compile_owner_drop(&compiler);
    printf("Library STRING source reverse=%u compiler/runtime physicalzero\n",reverse);
}
int main(int argc,char **argv) {
    CHECK(argc==1||argc==2);
    library_string_source_boundaries();library_string_growth_boundaries();
    string_source_case(0,argc==2?argv[1]:NULL);string_source_case(1,NULL);
    library_compile_observer_free();puts("Library STRING original Source Checked lifetime PASS");return 0;
}
