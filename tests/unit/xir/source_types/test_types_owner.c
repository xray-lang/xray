/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_types_owner.c - Type observation validation and receiving-owner rollback
 */
#include "xir/xxir_source_query_internal.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_interface.h"
#include "xir/xxir_nominal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
typedef struct TypeFixture {
    XrXirSourceQueryModule module;
    XrXirSourceDeclaration declarations[3];
    XrXirSourceDeclarationSyntax names[3];
    XrXirNominalDeclaration nominals[2];
    XrXirNominalTable nominal_table;
    XrXirInterfaceDeclaration interface;
    XrXirInterfaceTable interface_table;
    XrXirTypes types;
    XrXirSourceView view;
    XrXirSourceSyntaxView syntax;
    XrXirSourceTypeFact entries[3];
    XrXirSourceTypeFacts facts;
} TypeFixture;
static const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
static void fixture(TypeFixture *f) {
    memset(f,0,sizeof(*f));f->module.identity="module";f->module.path="source.xr";
    for(uint32_t i=0;i<3;++i){const char *name=i==0?"C":i==1?"S":"I";
        f->declarations[i].id=i+1;f->declarations[i].kind=XR_XIR_SOURCE_TYPE;f->declarations[i].name=name;
        f->declarations[i].range=(XrXirSourceRange){0,1+(int)i*2,7,1+(int)i*2,8};
        f->names[i]=(XrXirSourceDeclarationSyntax){.name=f->declarations[i].range,.role=XR_XIR_SOURCE_SYNTAX_NAME};
        f->entries[i]=(XrXirSourceTypeFact){i==0?XR_XIR_SOURCE_TYPE_CLASS:i==1?XR_XIR_SOURCE_TYPE_STRUCT:XR_XIR_SOURCE_TYPE_INTERFACE,
            i==1?1:0,{0,1+(int)i*2,1,2+(int)i*2,2},true};
        if(i<2){f->nominals[i].module=(XrXirLiteral){"module",6};f->nominals[i].name=(XrXirLiteral){name,1};
            f->nominals[i].kind=i?XR_XIR_NOMINAL_STRUCT:XR_XIR_NOMINAL_CLASS;f->nominals[i].flags=i?0:XR_XIR_NOMINAL_FINAL;}}
    f->nominal_table=(XrXirNominalTable){f->nominals,2,NULL};
    f->interface.module=(XrXirLiteral){"module",6};f->interface.name=(XrXirLiteral){"I",1};f->interface_table=(XrXirInterfaceTable){&f->interface,1};
    f->types.nominals=&f->nominal_table;f->types.interfaces=&f->interface_table;
    f->view.complete=true;f->view.modules=&f->module;f->view.module_count=1;f->view.declarations=f->declarations;f->view.declaration_count=3;f->view.types=&f->types;
    f->syntax=(XrXirSourceSyntaxView){f->names,3,NULL,0,NULL,0};f->facts=(XrXirSourceTypeFacts){f->entries,3};
}
static XrXirStatus make_snapshot(const XrXirCompileContext *context,TypeFixture *f,XrXirSourceSnapshot **out) {
    XrXirConstructionRow rows[2]={{0},{0}};XrXirConstruction *construction=NULL;
    XrXirStatus status=xr_xir_compile_construction_new(context,&f->types,rows,2,&construction);
    if(status==XR_XIR_OK)status=xr_xir_compile_source_snapshot_copy_v2(context,&f->view,construction,out);
    xr_xir_compile_construction_free(construction);
    if(status==XR_XIR_OK)status=xr_xir_compile_source_snapshot_syntax_copy(*out,&f->syntax);
    return status;
}
static void invalid(void) {
    for(unsigned mode=0;mode<14;++mode){TypeFixture f;fixture(&f);
        if(mode==12){f.declarations[1].name="C";f.declarations[1].range=f.declarations[0].range;f.names[1]=f.names[0];f.entries[1]=f.entries[0];}
        if(mode==13)f.view.complete=false;
        XrCompileResources *r=sdk_ledger(&limits);XrXirCompileContext context={r,xr_xir_compile_default_limits()};XrXirSourceSnapshot *s=NULL;
        CHECK(make_snapshot(&context,&f,&s)==XR_XIR_OK&&s);const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(s);
        const XrXirTypes *types=view->types;XrCompileResourceStats before=sdk_stats(r);size_t physical=runtime_live,bytes=runtime_bytes;
        switch(mode){
        case 0:f.facts.declaration_count=2;break;case 1:f.facts.declarations=NULL;break;
        case 2:f.entries[0].kind=XR_XIR_SOURCE_TYPE_NONE;break;case 3:f.entries[0].table_index=2;break;
        case 4:f.entries[2].table_index=1;break;case 5:f.entries[0].kind=XR_XIR_SOURCE_TYPE_ENUM;break;
        case 6:f.entries[0].range_known=false;break;case 7:f.entries[0].range.end_line=0;break;
        case 8:f.entries[0].range.module=1;break;case 9:f.entries[0].range.column=8;break;
        case 10:f.entries[0].kind=(XrXirSourceTypeKind)6;break;case 11:f.entries[2].kind=XR_XIR_SOURCE_TYPE_CLASS;break;
        default:break;}
        CHECK(xr_xir_compile_source_snapshot_type_facts_copy(s,&f.facts)==XR_XIR_BAD_STRUCTURE);
        CHECK(!xr_xir_compile_source_snapshot_type_facts(s)&&view->types==types&&sdk_stats(r).live_bytes==before.live_bytes&&runtime_live==physical&&runtime_bytes==bytes);
        xr_xir_compile_source_snapshot_free(s);xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);
    }
}
static void allocation_failures(void) {
    for(size_t point=0;point<3;++point){TypeFixture f;fixture(&f);XrCompileResources *r=sdk_ledger(&limits);
        XrXirCompileContext context={r,xr_xir_compile_default_limits()};XrXirSourceSnapshot *s=NULL;CHECK(make_snapshot(&context,&f,&s)==XR_XIR_OK);
        XrCompileResourceStats before=sdk_stats(r);size_t physical=runtime_live,bytes=runtime_bytes;
        runtime_fail_at=runtime_attempts+point;CHECK(xr_xir_compile_source_snapshot_type_facts_copy(s,&f.facts)==XR_XIR_OUT_OF_MEMORY);
        CHECK(runtime_attempts>runtime_fail_at&&!xr_xir_compile_source_snapshot_type_facts(s)&&runtime_live==physical&&runtime_bytes==bytes&&sdk_stats(r).live_bytes==before.live_bytes);
        runtime_fail_at=SIZE_MAX;CHECK(xr_xir_compile_source_snapshot_type_facts_copy(s,&f.facts)==XR_XIR_OK);
        CHECK(xr_xir_compile_source_snapshot_type_facts_copy(s,&f.facts)==XR_XIR_BAD_STRUCTURE);
        const XrXirSourceTypeFacts *owned=xr_xir_compile_source_snapshot_type_facts(s);CHECK(owned&&owned->declarations!=f.entries);
        memset(&f,0xa5,sizeof(f));xr_compile_resources_release(r);
        CHECK(owned->declaration_count==3&&owned->declarations[1].kind==XR_XIR_SOURCE_TYPE_STRUCT&&owned->declarations[1].table_index==1&&owned->declarations[2].range.end_line==6);
        CHECK(!strcmp(xr_xir_compile_source_snapshot_view(s)->declarations[0].name,"C"));xr_xir_compile_source_snapshot_free(s);CHECK(!runtime_live&&!runtime_bytes);
    }
}
static XrXirStatus once(XrCompileResourceLimits bounds,XrCompileResourceStats *stats) {
    TypeFixture f;fixture(&f);XrCompileResources *r=sdk_ledger(&bounds);XrXirCompileContext context={r,xr_xir_compile_default_limits()};XrXirSourceSnapshot *s=NULL;
    XrXirStatus status=make_snapshot(&context,&f,&s);if(status==XR_XIR_OK)status=xr_xir_compile_source_snapshot_type_facts_copy(s,&f.facts);
    if(status!=XR_XIR_OK)CHECK(!xr_xir_compile_source_snapshot_type_facts(s));if(stats)*stats=sdk_stats(r);
    xr_xir_compile_source_snapshot_free(s);xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);return status;
}
static void boundaries(void) {
    XrCompileResourceStats baseline;CHECK(once(limits,&baseline)==XR_XIR_OK);
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){XrCompileResourceLimits exact={baseline.allocated_bytes,baseline.peak_bytes,baseline.work};
        uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;CHECK(*bound&&*bound<INT64_MAX);
        *bound=(uint64_t)((int64_t)*bound+delta);CHECK(once(exact,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));}
}
static void unknown(void) {
    TypeFixture f;fixture(&f);f.entries[0].range_known=false;f.entries[0].range=(XrXirSourceRange){0};
    f.entries[1]=(XrXirSourceTypeFact){0};XrCompileResources *r=sdk_ledger(&limits);XrXirCompileContext context={r,xr_xir_compile_default_limits()};XrXirSourceSnapshot *s=NULL;
    CHECK(make_snapshot(&context,&f,&s)==XR_XIR_OK&&xr_xir_compile_source_snapshot_type_facts_copy(s,&f.facts)==XR_XIR_OK);
    CHECK(!xr_xir_compile_source_snapshot_type_facts(s)->declarations[0].range_known&&xr_xir_compile_source_snapshot_type_facts(s)->declarations[1].kind==XR_XIR_SOURCE_TYPE_NONE);
    xr_xir_compile_source_snapshot_free(s);xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);
}
int main(void){(void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_unlimited;
    CHECK(!xr_xir_compile_source_snapshot_type_facts(NULL));invalid();allocation_failures();boundaries();unknown();
    puts("Source type owner:14 structural rejects, all3 attachment allocation prefixes, original-ledger retry,9 cumulative boundaries, explicit unknown and producer death physical0/0");return 0;}
