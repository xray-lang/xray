/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_sort_owner.c - Every sorting work prefix preserves each receiving owner exactly once
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
static bool once(uint64_t work,uint64_t *setup,uint64_t *total) {
    XrCompileResourceLimits limits=sdk_unlimited;limits.work=work;XrCompileResources *r=sdk_ledger(&limits);
    XrXirCompileContext context={r,xr_xir_compile_default_limits()};XlspSourceCallRequest request={&context,docs,NULL,3,0,NULL,0,0};
    CallWork w={0};w.resources=r;w.request=&request;w.status=XR_XIR_OK;w.count=w.capacity=3;
    CHECK(xr_compile_resources_calloc(r,3,sizeof(*w.edges),(void **)&w.edges)==XR_COMPILE_RESOURCE_OK);
    XrLspRange *original[3]={0};
    for(size_t i=0;i<3;++i){w.edges[i].other.document=i;w.edges[i].count=w.edges[i].capacity=1;
        CHECK(xr_compile_resources_calloc(r,1,sizeof(XrLspRange),(void **)&w.edges[i].sites)==XR_COMPILE_RESOURCE_OK);original[i]=w.edges[i].sites;}
    uint64_t before=sdk_stats(r).work;if(setup)*setup=before;bool result=CALL_SORT_ENTRY(&w);if(total)*total=sdk_stats(r).work;
    CHECK(w.status==(result?XR_XIR_OK:XR_XIR_BUDGET));
    for(size_t i=0;i<3;++i){unsigned found=0;for(size_t j=0;j<3;++j)found+=w.edges[j].sites==original[i];CHECK(found==1);}
    if(result)CHECK(w.edges[0].other.document==2&&w.edges[1].other.document==1&&w.edges[2].other.document==0);
    for(size_t i=0;i<3;++i)xr_compile_resources_free(w.edges[i].sites);xr_compile_resources_free(w.edges);xr_compile_resources_release(r);
    CHECK(!runtime_live&&!runtime_bytes);return result;
}
int main(void) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;uint64_t setup=0,total=0;CHECK(once(UINT64_MAX,&setup,&total)&&total>setup);
    for(uint64_t prefix=0;prefix<=total-setup;++prefix)CHECK(once(setup+prefix,NULL,NULL)==(prefix==total-setup));
    printf("Call edge sorting: all%llu work prefixes, unique receiving owners, exact boundary and physical0/0\n",(unsigned long long)(total-setup+1));return 0;
}
