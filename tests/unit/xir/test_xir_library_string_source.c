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
static size_t source_attempts,source_fail_at=SIZE_MAX,source_live,source_bytes,source_peak;
typedef struct LibrarySourceAllocation {void *pointer;size_t bytes;} LibrarySourceAllocation;
static LibrarySourceAllocation source_owned[4096];
XR_FUNC void *xr_test_library_string_source_calloc(size_t count,size_t size){
 CHECK(!size||count<=SIZE_MAX/size);
 if(source_attempts++==source_fail_at)return NULL;void *p=xr_calloc(count,size);
 if(p){CHECK(source_live<4096);CHECK(count*size<=SIZE_MAX-source_bytes);source_owned[source_live++]=(LibrarySourceAllocation){p,count*size};source_bytes+=count*size;if(source_bytes>source_peak)source_peak=source_bytes;}return p;
}
XR_FUNC void xr_test_library_string_source_free(void *p){
 if(p)for(size_t i=0;i<source_live;++i)if(source_owned[i].pointer==p){source_bytes-=source_owned[i].bytes;source_owned[i]=source_owned[--source_live];break;}
 xr_free(p);
}
XR_FUNC void *xr_test_library_string_source_malloc(size_t size){return xr_test_library_string_source_calloc(1,size);}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_calloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#define xr_malloc(s) xr_test_library_string_source_malloc(s)
#define xr_calloc(c,s) xr_test_library_string_source_calloc(c,s)
#define xr_free(p) xr_test_library_string_source_free(p)
#include "xir/xxir_source_query.c"
#include "xir/xxir_type_inference.c"
#include "xir/xxir_source.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_calloc")
#pragma pop_macro("xr_malloc")
#include "xir_library_string_runtime.h"
#include "xir/xxir_library_catalog.h"
#include "base/xsha256.h"
#include "xir_library_string_goldens.h"
#include "xir_library_string_source_cases.h"

static void string_library_packet(const char *name, const char *expected, XrXirCheckedPacket *packet) {
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_SOURCE_FIXTURES,name)>0);
    XrCompilerSession *session=xr_compiler_session_new(NULL);CHECK(session);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,path,&authority,NULL,NULL,NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_source_check(&request,&result,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"library %s: %u %s\n",name,status,diagnostic.message);
    CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
    const XrXirDeclarations *d=xr_xir_artifact_module(result.checked)->declarations;
    CHECK(d&&d->literal_count&&d->literals[0].length==9&&!memcmp(d->literals[0].bytes,expected,9));
    CHECK(xr_xir_checked_write(result.checked,NULL,packet,NULL)==XR_XIR_OK);
    xr_xir_source_result_free(&result);xr_compiler_session_delete(session);
    CHECK(!source_live&&!source_bytes);
}
static void string_snapshot_owned(XrXirSourceSnapshot *snapshot) {
    const XrXirSourceView *view=xr_xir_source_snapshot_view(snapshot);CHECK(view&&view->complete);
    bool left=false,alpha=false;
    for(uint32_t i=0;i<view->declaration_count;++i){
        const XrXirSourceDeclaration *d=&view->declarations[i];
        if(!strcmp(d->name,"left")){CHECK(d->type.known);left=true;}
        if(!strcmp(d->name,"alpha"))alpha=true;
    }
    CHECK(left&&alpha);xr_xir_source_snapshot_free(snapshot);
}
static void string_source_case(unsigned reverse,const char *output_path) {
    XrXirCheckedPacket packets[2]={{0}};
    string_library_packet("alpha.xr","甲-owned",&packets[0]);
    string_library_packet("beta.xr","乙-owned",&packets[1]);
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
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_library_catalog_new(inputs,5,NULL,&catalog)==XR_XIR_OK);
    xr_xir_checked_packet_free(&packets[0]);xr_xir_checked_packet_free(&packets[1]);
    for(unsigned i=0;i<3;++i){memset(buffers[i],0,lengths[i]);xr_free(buffers[i]);}
    memset(inputs,0,sizeof(inputs));memset(names,0,sizeof(names));memset(catalog_root,0,sizeof(catalog_root));
    XrCompilerSession *session=xr_compiler_session_new(NULL);CHECK(session);
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,NULL,NULL,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    if(!reverse){library_string_source_negatives(&request);library_string_source_budgets(&request);}
    source_attempts=0;runtime_attempts=0;
    XrXirStatus status=xr_xir_source_check(&request,&result,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"root %u: %u %s\n",reverse,status,diagnostic.message);
    CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
    size_t sites=source_attempts,core_sites=runtime_attempts;
    XrXirArtifact *owned=result.checked;result.checked=NULL;
    XrXirSourceSnapshot *snapshot=result.snapshot;result.snapshot=NULL;
    xr_xir_source_result_free(&result);
    size_t live=source_live,bytes=source_bytes,rlive=runtime_live,rbytes=runtime_bytes;
    for(size_t f=0;f<sites;++f){
        source_attempts=0;source_fail_at=f;
        XrXirSourceResult failed={0};
        CHECK(xr_xir_source_check(&request,&failed,&diagnostic)==XR_XIR_OUT_OF_MEMORY);
        CHECK(!failed.checked&&!failed.snapshot);xr_xir_source_result_free(&failed);
        CHECK(source_live==live&&source_bytes==bytes&&runtime_live==rlive&&runtime_bytes==rbytes);
    }
    source_fail_at=SIZE_MAX;
    for(size_t f=0;f<core_sites;++f){
        runtime_attempts=0;runtime_fail_at=f;XrXirSourceResult failed={0};
        XrXirStatus failed_status=xr_xir_source_check(&request,&failed,&diagnostic);runtime_fail_at=SIZE_MAX;
        if(failed_status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"string Source core fault%zu/%zu status%u %s\n",f,core_sites,failed_status,diagnostic.message);
        CHECK(failed_status==XR_XIR_OUT_OF_MEMORY&&!failed.checked&&!failed.snapshot);
        xr_xir_source_result_free(&failed);
        CHECK(source_live==live&&source_bytes==bytes&&runtime_live==rlive&&runtime_bytes==rbytes);
    }
    printf("Library STRING Source combined Core OOM=%zu physicalbaseline\n",core_sites);
    xr_compiler_session_delete(session);xr_xir_library_catalog_free(catalog);
    string_snapshot_owned(snapshot);CHECK(!source_live&&!source_bytes);
    CHECK(xr_xir_artifact_verify(owned,NULL,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_checked_write(owned,NULL,&packet,NULL)==XR_XIR_OK);
    if(output_path){FILE *f=fopen(output_path,"wb");CHECK(f);CHECK(fwrite(packet.bytes,1,packet.length,f)==packet.length&&!fclose(f));}
    xr_xir_checked_packet_free(&packet);
    xr_test_library_string_run(owned);
    CHECK(!source_live&&!source_bytes&&!runtime_live&&!runtime_bytes);
    printf("Library STRING source reverse=%u source OOM=%zu physicalzero\n",reverse,sites);
}
int main(int argc,char **argv) {
    CHECK(argc==1||argc==2);
    library_string_source_boundaries();library_string_growth_boundaries();
    string_source_case(0,argc==2?argv[1]:NULL);string_source_case(1,NULL);
    puts("Library STRING original Source Checked lifetime PASS");return 0;
}
