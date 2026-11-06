/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_sort_owner.c - Cancellation during sorting preserves each receiving owner exactly once
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xir/xxir_source.h"
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
#include "../../../../src/app/lsp/xlsp_source_call_hierarchy.c"
#ifdef CALL_SORT_COUNTERFACTUAL
#include "sort_before.inc.c"
#define CALL_SORT_ENTRY call_sort_before
#else
#define CALL_SORT_ENTRY call_sort
#endif
static const XlspSourceDocument docs[]={{"file:///z.xr","",12,0,0},{"file:///m.xr","",12,0,0},{"file:///a.xr","",12,0,0}};
typedef struct SortCancel {size_t calls,cut;} SortCancel;
static bool sort_cancel_poll(const void *context) {
    SortCancel *cancel=(SortCancel *)context;
    return cancel->calls++>=cancel->cut;
}
static bool once(size_t cut,size_t *polls) {
    XrCompileResources *r=sdk_ledger(&sdk_unlimited);
    SortCancel cancel={0,cut};XrCompileRequestControl control={0};
    const XrCompileRequestOptions options={.cancel_poll=sort_cancel_poll,.cancel_context=&cancel,
        .deadline_mode=XR_COMPILE_DEADLINE_NONE};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={r,xr_xir_compile_default_limits()};XlspSourceHierarchyRequest request={&context,docs,NULL,3,0,NULL,0,0};
    CallWork w={0};w.resources=r;w.control=&control;w.request=&request;w.status=XR_XIR_OK;w.count=w.capacity=3;
    CHECK(xr_compile_resources_calloc(r,3,sizeof(*w.edges),(void **)&w.edges)==XR_COMPILE_RESOURCE_OK);
    XrLspRange *original[3]={0};
    for(size_t i=0;i<3;++i){w.edges[i].other.document=i;w.edges[i].count=w.edges[i].capacity=1;
        CHECK(xr_compile_resources_calloc(r,1,sizeof(XrLspRange),(void **)&w.edges[i].sites)==XR_COMPILE_RESOURCE_OK);original[i]=w.edges[i].sites;}
    bool result=CALL_SORT_ENTRY(&w);if(polls)*polls=cancel.calls;
    CHECK(w.status==(result?XR_XIR_OK:XR_XIR_CANCELLED));
    for(size_t i=0;i<3;++i){unsigned found=0;for(size_t j=0;j<3;++j)found+=w.edges[j].sites==original[i];CHECK(found==1);}
    if(result)CHECK(w.edges[0].other.document==2&&w.edges[1].other.document==1&&w.edges[2].other.document==0);
    for(size_t i=0;i<3;++i)xr_compile_resources_free(w.edges[i].sites);xr_compile_resources_free(w.edges);xr_compile_resources_release(r);
    CHECK(!runtime_live&&!runtime_bytes);return result;
}
int main(void) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;size_t polls=0;CHECK(once(SIZE_MAX,&polls)&&polls>2);
    const size_t cuts[]={0,polls/2,polls-1};
    for(size_t i=0;i<sizeof(cuts)/sizeof(*cuts);++i)CHECK(!once(cuts[i],NULL));
    CHECK(once(SIZE_MAX,NULL));
    printf("Call edge sorting: 3 cancellation safe points, unique receiving owners, recovery and physical0/0\n");return 0;
}
