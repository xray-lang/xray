/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "base/xcompile_resources.h"
#include "base/xcompile_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
typedef struct Poll {bool requested;unsigned calls;} Poll;
static bool test_cancel_poll(const void *context){Poll *p=(Poll *)context;++p->calls;return p->requested;}
static bool other_poll(const void *context){(void)context;return false;}
static bool same(XrCompileResourceStats a,XrCompileResourceStats b){return !memcmp(&a,&b,sizeof(a));}
static XrCompileResourceStatus once(void) {
    XrCompileResourceLimits limits={4096,4096,1024};XrCompileResources *r=NULL;void *block=NULL,*empty=NULL;Poll token={0},other={0};bool bound=false;
    XrCompileResourceStatus s=xr_compile_resources_new(&limits,&r);if(s!=XR_COMPILE_RESOURCE_OK)goto done;
    XrCompileResourceStats before=sdk_stats(r);CHECK(xr_compile_resources_begin_cancel(r,NULL,&token)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_begin_cancel(r,test_cancel_poll,&token)==XR_COMPILE_RESOURCE_OK);bound=true;
    CHECK(xr_compile_resources_begin_cancel(r,test_cancel_poll,&other)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_end_cancel(r,other_poll,&token)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_end_cancel(r,test_cancel_poll,&other)==XR_COMPILE_RESOURCE_BAD_ARGUMENT&&same(before,sdk_stats(r)));
    s=xr_compile_resources_alloc(r,8,&block);if(s!=XR_COMPILE_RESOURCE_OK)goto done;memset(block,42,8);
    CHECK(xr_compile_resources_work(r,7)==XR_COMPILE_RESOURCE_OK);
    s=xr_compile_resources_resize(r,&block,16);if(s!=XR_COMPILE_RESOURCE_OK)goto done;
    for(unsigned i=0;i<8;++i)CHECK(((unsigned char *)block)[i]==42);
    token.requested=true;before=sdk_stats(r);void *saved=block;size_t advanced=99;bool found=true;
    CHECK(xr_compile_resources_work(r,1)==XR_COMPILE_RESOURCE_CANCELLED);unsigned calls=token.calls;token.requested=false;
    CHECK(xr_compile_resources_admit(r,1,0)==XR_COMPILE_RESOURCE_CANCELLED);
    CHECK(xr_compile_resources_alloc(r,1,&empty)==XR_COMPILE_RESOURCE_CANCELLED&&!empty);
    CHECK(xr_compile_resources_calloc(r,1,1,&empty)==XR_COMPILE_RESOURCE_CANCELLED&&!empty);
    CHECK(xr_compile_resources_resize(r,&block,32)==XR_COMPILE_RESOURCE_CANCELLED&&block==saved);
    CHECK(xr_compile_resources_scan_delimiter(r,"a",1,'x',&advanced,&found)==XR_COMPILE_RESOURCE_CANCELLED&&advanced==99&&found);
    CHECK(token.calls==calls&&same(before,sdk_stats(r)));
    CHECK(xr_compile_resources_end_cancel(r,test_cancel_poll,&token)==XR_COMPILE_RESOURCE_CANCELLED);bound=false;
    CHECK(xr_compile_resources_end_cancel(r,test_cancel_poll,&token)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_work(r,3)==XR_COMPILE_RESOURCE_OK&&sdk_stats(r).work==before.work+3&&token.calls==calls);
    CHECK(xr_compile_resources_begin_cancel(r,test_cancel_poll,&other)==XR_COMPILE_RESOURCE_OK);other.requested=true;
    CHECK(xr_compile_resources_end_cancel(r,test_cancel_poll,&other)==XR_COMPILE_RESOURCE_CANCELLED);
    CHECK(xr_compile_resources_begin_cancel(r,test_cancel_poll,&token)==XR_COMPILE_RESOURCE_OK);bound=true;
    CHECK(xr_compile_resources_end_cancel(r,test_cancel_poll,&token)==XR_COMPILE_RESOURCE_OK);bound=false;
done:
    if(bound)CHECK(xr_compile_resources_end_cancel(r,test_cancel_poll,&token)==XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(r);xr_compile_resources_free(block);CHECK(!runtime_live&&!runtime_bytes);return s;
}
static void sticky_cancellation(void) {
    XrCompileResourceLimits limits={4096,4096,1024};XrCompileResources *r=NULL;XrCompileState *state=NULL,*other=NULL;Poll token={true,0};
    CHECK(xr_compile_resources_new(&limits,&r)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_state_new(r,&state)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_begin_cancel(r,test_cancel_poll,&token)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_state_work(state,1)==XR_COMPILE_RESOURCE_CANCELLED);
    CHECK(xr_compile_state_status(state)==XR_COMPILE_RESOURCE_CANCELLED);
    CHECK(xr_compile_resources_end_cancel(r,test_cancel_poll,&token)==XR_COMPILE_RESOURCE_CANCELLED);
    CHECK(xr_compile_state_work(state,1)==XR_COMPILE_RESOURCE_CANCELLED);
    CHECK(xr_compile_resources_work(r,1)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_state_new(r,&other)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_state_fail(other,XR_COMPILE_RESOURCE_OUT_OF_MEMORY)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    CHECK(xr_compile_state_fail(other,XR_COMPILE_RESOURCE_CANCELLED)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    xr_compile_resources_release(r);CHECK(xr_compile_state_status(state)==XR_COMPILE_RESOURCE_CANCELLED);
    xr_compile_state_release(state);xr_compile_state_release(other);CHECK(!runtime_live&&!runtime_bytes);
    puts("Compile state preserves cancellation after scope/lease death and keeps the first failure");
}
int main(void) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_ledger;(void)sdk_unlimited;
    CHECK(xr_compile_resources_begin_cancel(NULL,test_cancel_poll,NULL)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_end_cancel(NULL,test_cancel_poll,NULL)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    runtime_attempts=0;CHECK(once()==XR_COMPILE_RESOURCE_OK);size_t source=runtime_attempts;CHECK(source==3);
    for(size_t prefix=0;prefix<source;++prefix){runtime_attempts=0;runtime_fail_at=prefix;
        CHECK(once()==XR_COMPILE_RESOURCE_OUT_OF_MEMORY&&runtime_attempts>prefix);}
    runtime_fail_at=SIZE_MAX;sticky_cancellation();puts("Compile cancellation scope: latch, identity, nested rejection, work/scan/admission/alloc/resize atomicity, unchanged counters, detach/reuse and all3 physical prefixes PASS");return 0;
}
