/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_calls_owner.c - Attachment validation, rollback and independent receiving ownership
 */
#include "xir/xxir_source_query_internal.h"
#include "xir/xxir_construction.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
typedef struct CallsFixture {
    XrXirSourceQueryModule module;
    XrXirSourceDeclaration declarations[2];
    XrXirSourceReference references[3];
    XrXirSourceDeclarationSyntax names[2];
    XrXirSourceRange selections[3];
    XrXirSourceFunctionRange functions[2];
    XrXirSourceCall items[3];
    XrXirSourceView view;
    XrXirSourceSyntaxView syntax;
    XrXirSourceCalls calls;
} CallsFixture;
static void fixture(CallsFixture *f) {
    memset(f,0,sizeof(*f));f->module.identity="module";f->module.path="source.xr";
    for(uint32_t i=0;i<2;++i) {
        f->declarations[i].id=i+1;f->declarations[i].kind=XR_XIR_SOURCE_FUNCTION;
        f->declarations[i].name=i?"g":"f";
        f->declarations[i].range=(XrXirSourceRange){0,1+(int)i*2,4,1+(int)i*2,5};
        f->names[i]=(XrXirSourceDeclarationSyntax){f->declarations[i].range,XR_XIR_SOURCE_SYNTAX_NAME,0};
        f->functions[i]=(XrXirSourceFunctionRange){{0,1+(int)i*2,1,2+(int)i*4,2},true};
    }
    f->selections[0]=(XrXirSourceRange){0,4,3,4,4};
    f->selections[1]=(XrXirSourceRange){0,4,9,4,10};
    f->selections[2]=(XrXirSourceRange){0,7,1,7,2};
    for(uint32_t i=0;i<3;++i)f->references[i]=(XrXirSourceReference){f->selections[i],i==2?2u:1u,i==2?2u:1u,i?XR_XIR_SOURCE_CALL:XR_XIR_SOURCE_READ};
    f->items[0]=(XrXirSourceCall){1,2,f->selections[1],false,true};
    f->items[1]=(XrXirSourceCall){UINT32_MAX,2,{0,5,3,5,4},false,true};
    f->items[2]=(XrXirSourceCall){2,0,f->selections[2],true,true};
    f->view.complete=true;f->view.modules=&f->module;f->view.module_count=1;
    f->view.declarations=f->declarations;f->view.declaration_count=2;f->view.references=f->references;f->view.reference_count=3;
    f->syntax=(XrXirSourceSyntaxView){f->names,2,f->selections,3,NULL,0};
    f->calls=(XrXirSourceCalls){f->items,3,f->functions,2};
}
static XrXirSourceSnapshot *snapshot(const XrXirCompileContext *context,CallsFixture *f) {
    XrXirConstruction *construction=NULL;
    CHECK(xr_xir_compile_construction_new(context,NULL,NULL,0,&construction)==XR_XIR_OK&&construction);
    XrXirSourceSnapshot *s=NULL;CHECK(xr_xir_compile_source_snapshot_copy_v2(context,&f->view,construction,&s)==XR_XIR_OK&&s);
    xr_xir_compile_construction_free(construction);
    CHECK(!xr_xir_compile_source_snapshot_calls(s));
    CHECK(xr_xir_compile_source_snapshot_syntax_copy(s,&f->syntax)==XR_XIR_OK);return s;
}
static void invalid(void) {
    for(unsigned mode=0;mode<12;++mode) {
        CallsFixture f;fixture(&f);XrCompileResources *r=sdk_ledger(&sdk_unlimited);
        XrXirCompileContext context={r,xr_xir_compile_default_limits()};XrXirSourceSnapshot *s=snapshot(&context,&f);
        const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(s);
        const XrXirSourceReference *references=view->references;XrCompileResourceStats before=sdk_stats(r);
        switch(mode) {
        case 0:f.calls.declaration_count=1;break;
        case 1:f.calls.items=NULL;break;
        case 2:f.calls.functions=NULL;break;
        case 3:f.functions[0].known=false;break;
        case 4:f.functions[0].range.end_line=0;break;
        case 5:f.items[0].caller=3;break;
        case 6:f.items[0].module_initializer=true;break;
        case 7:f.items[0].reference=2;break;
        case 8:f.items[2].reference=1;break;
        case 9:f.items[0].range.column=8;break;
        case 10:f.items[0].selection_known=false;f.items[0].range=(XrXirSourceRange){0};break;
        case 11:f.calls.count=2;break;
        }
        CHECK(xr_xir_compile_source_snapshot_calls_copy(s,&f.calls)==XR_XIR_BAD_STRUCTURE);
        CHECK(!xr_xir_compile_source_snapshot_calls(s)&&view->references==references&&sdk_stats(r).live_bytes==before.live_bytes);
        fixture(&f);CHECK(xr_xir_compile_source_snapshot_calls_copy(s,&f.calls)==XR_XIR_OK);
        CHECK(xr_xir_compile_source_snapshot_calls_copy(s,&f.calls)==XR_XIR_BAD_STRUCTURE);
        xr_xir_compile_source_snapshot_free(s);xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);
    }
}
static void allocation_failures(void) {
    for(size_t point=0;point<2;++point) {
        CallsFixture f;fixture(&f);XrCompileResources *r=sdk_ledger(&sdk_unlimited);
        XrXirCompileContext context={r,xr_xir_compile_default_limits()};XrXirSourceSnapshot *s=snapshot(&context,&f);
        XrCompileResourceStats before=sdk_stats(r);size_t physical=runtime_live,bytes=runtime_bytes;
        runtime_fail_at=runtime_attempts+point;
        CHECK(xr_xir_compile_source_snapshot_calls_copy(s,&f.calls)==XR_XIR_OUT_OF_MEMORY);
        CHECK(runtime_attempts>runtime_fail_at&&!xr_xir_compile_source_snapshot_calls(s));
        CHECK(runtime_live==physical&&runtime_bytes==bytes&&sdk_stats(r).live_bytes==before.live_bytes);
        runtime_fail_at=SIZE_MAX;
        CHECK(xr_xir_compile_source_snapshot_calls_copy(s,&f.calls)==XR_XIR_OK);
        const XrXirSourceCalls *owned=xr_xir_compile_source_snapshot_calls(s);
        CHECK(owned&&owned->items!=f.items&&owned->functions!=f.functions);
        memset(&f,0xa5,sizeof(f));xr_compile_resources_release(r);
        CHECK(owned->count==3&&owned->declaration_count==2&&owned->items[0].reference==1&&owned->items[0].caller==2);
        CHECK(owned->items[1].reference==UINT32_MAX&&owned->items[2].module_initializer&&owned->functions[1].range.end_line==6);
        xr_xir_compile_source_snapshot_free(s);CHECK(!runtime_live&&!runtime_bytes);
    }
}
static void unknown_selection(void) {
    CallsFixture f;fixture(&f);f.selections[1]=(XrXirSourceRange){0};
    f.items[0].range=(XrXirSourceRange){0};f.items[0].selection_known=false;
    XrCompileResources *r=sdk_ledger(&sdk_unlimited);XrXirCompileContext context={r,xr_xir_compile_default_limits()};
    XrXirSourceSnapshot *s=snapshot(&context,&f);CHECK(xr_xir_compile_source_snapshot_calls_copy(s,&f.calls)==XR_XIR_OK);
    CHECK(!xr_xir_compile_source_snapshot_calls(s)->items[0].selection_known);
    xr_xir_compile_source_snapshot_free(s);xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);
}
int main(void) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;
    CHECK(!xr_xir_compile_source_snapshot_calls(NULL));invalid();allocation_failures();unknown_selection();
    puts("Source calls owner: 12 structural rejections, both attachment allocation prefixes, retry without ledger reset, producer death, explicit unknown selection, physical0/0");return 0;
}
