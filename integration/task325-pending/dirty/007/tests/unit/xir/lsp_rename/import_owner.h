/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * import_owner.h - Reject malformed import observations before publication
 *
 * KEY CONCEPT:
 *   Failed syntax attachment preserves the receiver and permits a clean retry.
 */
#ifndef LSP_IMPORT_OWNER_H
#define LSP_IMPORT_OWNER_H
#include "xir/xxir_source_query_internal.h"
#include "xir/xxir_construction.h"
static void import_owner(void) {
    XrCompileResources *r=sdk_ledger(&limits);XrXirCompileContext context={r,xr_xir_compile_default_limits()};
    XrXirSourceQueryModule modules[]={{.identity="root",.path="root.xr"},{.identity="library",.path="lib.xr"}};
    XrXirSourceDeclaration decls[]={
        {.id=1,.target=2,.kind=XR_XIR_SOURCE_IMPORT,.name="target",.range={0,1,9,1,15}},
        {.id=2,.kind=XR_XIR_SOURCE_FUNCTION,.name="target",.range={1,1,11,1,17},.exported=true},
        {.id=3,.kind=XR_XIR_SOURCE_MODULE,.name="lib",.range={0,0,0,0,0}}
    };
    XrXirSourceReference refs[]={{{0,1,9,1,15},2,2,XR_XIR_SOURCE_IMPORT_SELECTOR}};
    XrXirSourceDeclarationSyntax facts[]={
        {.name={0,1,9,1,15},.role=XR_XIR_SOURCE_SYNTAX_NAME},
        {.name={1,1,11,1,17},.role=XR_XIR_SOURCE_SYNTAX_NAME},
        {.name={0,0,0,0,0},.role=XR_XIR_SOURCE_SYNTAX_NONE}
    };
    XrXirSourceRange spans[]={{0,1,9,1,15}};
    XrXirSourceMarker markers[]={{{0,1,9,1,15},1,XR_XIR_SOURCE_MARKER_IMPORT_BINDING},
        {{0,2,15,2,15},3,XR_XIR_SOURCE_MARKER_IMPORT_IMPLICIT}};
    XrXirSourceView view={.complete=true,.modules=modules,.module_count=2,.declarations=decls,.declaration_count=3,
        .references=refs,.reference_count=1};
    XrXirSourceSyntaxView syntax={facts,3,spans,1,markers,2};
    XrXirConstruction *construction=NULL;
    CHECK(xr_xir_compile_construction_new(&context,NULL,NULL,0,&construction)==XR_XIR_OK&&construction);
    for(unsigned mode=0;mode<12;++mode) {
        XrXirSourceMarker changed[3]={markers[0],markers[1],markers[0]};
        XrXirSourceSyntaxView bad=syntax;bad.markers=changed;
        switch(mode) {
        case 0:changed[0].declaration=0;break;
        case 1:changed[0].declaration=4;break;
        case 2:changed[0].declaration=2;break;
        case 3:++changed[0].range.column;break;
        case 4:changed[0].role=XR_XIR_SOURCE_MARKER_IMPORT_IMPLICIT;break;
        case 5:changed[1].range.column=0;break;
        case 6:++changed[1].range.end_column;break;
        case 7:changed[1].range.end_line=3;break;
        case 8:changed[1].range.module=1;break;
        case 9:changed[1].role=XR_XIR_SOURCE_MARKER_IMPORT_BINDING;break;
        case 10:bad.marker_count=3;break;
        case 11:changed[0].role=(XrXirSourceMarkerRole)99;break;
        }
        XrXirSourceSnapshot *receiver=NULL;
        CHECK(xr_xir_compile_source_snapshot_copy_v2(&context,&view,construction,&receiver)==XR_XIR_OK);
        size_t blocks=runtime_live,bytes=runtime_bytes;uint64_t live=sdk_stats(r).live_bytes;
        CHECK(xr_xir_compile_source_snapshot_syntax_copy(receiver,&bad)==XR_XIR_BAD_STRUCTURE);
        CHECK(!xr_xir_compile_source_snapshot_syntax(receiver)&&runtime_live==blocks&&runtime_bytes==bytes&&sdk_stats(r).live_bytes==live);
        CHECK(xr_xir_compile_source_snapshot_syntax_copy(receiver,&syntax)==XR_XIR_OK);
        xr_xir_compile_source_snapshot_free(receiver);
    }
    size_t sites=0;XrXirSourceSnapshot *owned=NULL;
    for(size_t probe=0;probe<=sites;++probe) {
        CHECK(xr_xir_compile_source_snapshot_copy_v2(&context,&view,construction,&owned)==XR_XIR_OK);
        size_t blocks=runtime_live,bytes=runtime_bytes,start=runtime_attempts;uint64_t live=sdk_stats(r).live_bytes;
        runtime_fail_at=probe?start+probe-1:SIZE_MAX;
        XrXirStatus status=xr_xir_compile_source_snapshot_syntax_copy(owned,&syntax);runtime_fail_at=SIZE_MAX;
        if(!probe){CHECK(status==XR_XIR_OK);sites=runtime_attempts-start;CHECK(sites);}
        else {
            CHECK(status==XR_XIR_OUT_OF_MEMORY&&!xr_xir_compile_source_snapshot_syntax(owned));
            CHECK(runtime_live==blocks&&runtime_bytes==bytes&&sdk_stats(r).live_bytes==live);
            CHECK(xr_xir_compile_source_snapshot_syntax_copy(owned,&syntax)==XR_XIR_OK);
        }
        if(probe<sites){xr_xir_compile_source_snapshot_free(owned);owned=NULL;}
    }
    memset(modules,0xa5,sizeof(modules));memset(decls,0xa5,sizeof(decls));memset(refs,0xa5,sizeof(refs));
    memset(facts,0xa5,sizeof(facts));memset(spans,0xa5,sizeof(spans));memset(markers,0xa5,sizeof(markers));
    xr_xir_compile_construction_free(construction);
    xr_compile_resources_release(r);
    const XrXirSourceSyntaxView *escaped=xr_xir_compile_source_snapshot_syntax(owned);
    CHECK(escaped&&escaped->marker_count==2&&escaped->markers[1].range.column==15);
    CHECK(!strcmp(xr_xir_compile_source_snapshot_view(owned)->declarations[0].name,"target"));
    xr_xir_compile_source_snapshot_free(owned);CHECK(!runtime_live&&!runtime_bytes);
    fprintf(stderr,"Import owner invalid12 attachmentFI=%zu retry/producer-death compiler physical0/0\n",sites);
}
#endif // LSP_IMPORT_OWNER_H
