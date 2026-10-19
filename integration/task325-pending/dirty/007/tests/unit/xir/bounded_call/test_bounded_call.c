/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_bounded_call.c - Action limits, sticky failures and cancellation boundaries
 */
#include "xir/xxir_call.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#ifdef XR_BOUNDED_INJECTED
static size_t allocation_attempts, allocation_failure = SIZE_MAX, physical;
static void *bounded_calloc(size_t count, size_t size) {
    if (allocation_attempts++ == allocation_failure) return NULL;
    void *p = calloc(count,size); if (p) ++physical; return p;
}
static void bounded_free(void *p) { if (p) { CHECK(physical); --physical; free(p); } }
#undef xr_calloc
#undef xr_free
#define xr_calloc bounded_calloc
#define xr_free bounded_free
#include "xir/xxir_call.c"
#endif

typedef struct Witness {
    uint32_t mode, resumes, releases, exit_resumes, cleanup_resumes;
    XrXirCallStatus reason;
} Witness;
typedef struct State { uint32_t pc; XrXirValue child; } State;
static XrXirAction action(XrXirActionKind kind) { return (XrXirAction){kind,0,NULL,0,{0},{0},0}; }
static XrXirAction resume(XrXirCallView *view) {
    Witness *w=view->instance; State *s=view->state; ++w->resumes;
    CHECK(xr_xir_call_poll_bounded(view->activation,1).status==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_free(view->activation)==XR_XIR_CALL_BUSY);
    if (view->phase==XR_XIR_CALL_EXIT) {
        ++w->exit_resumes;
        if (w->mode==6 && w->exit_resumes==1)
            return (XrXirAction){XR_XIR_ACTION_CALL,1,NULL,0,{0},{0},XR_XIR_ACTION_CLEANUP};
        if (w->mode==7 || w->mode==9)
            CHECK(xr_xir_call_request_cancel(view->activation)==XR_XIR_CALL_CANCEL_REQUESTED);
        return action(XR_XIR_ACTION_EXIT_DONE);
    }
    if ((w->mode==8 || w->mode==9) && !s->pc++) return action(XR_XIR_ACTION_LEAVE);
    if (w->mode==1) return action(XR_XIR_ACTION_CONTINUE);
    if (w->mode==2) return action(XR_XIR_ACTION_SUSPEND);
    if (view->argument_count && !s->pc++) {
        if (view->arguments[0].payload) {
            s->child=(XrXirValue){XR_XIR_I64,0,view->arguments[0].payload-1};
            return (XrXirAction){XR_XIR_ACTION_CALL,0,&s->child,1,{0},{0},0};
        }
        if (w->mode==3) return xr_xir_call_fault(XR_XIR_RUN_OUT_OF_MEMORY);
        if (w->mode==4) return action((XrXirActionKind)999);
        if (w->mode==11) return action(XR_XIR_ACTION_SUSPEND);
    }
    ++s->pc;
    return action(XR_XIR_ACTION_RETURN);
}
static XrXirAction cleanup_resume(XrXirCallView *view) {
    Witness *w=view->instance; ++w->cleanup_resumes;
    CHECK(xr_xir_call_cleanup_active(view->activation));
    CHECK(xr_xir_call_request_cancel(view->activation)==XR_XIR_CALL_CANCEL_REQUESTED);
    return action(XR_XIR_ACTION_RETURN);
}
static void release(XrXirCallView *view,XrXirCallStatus reason) {
    Witness *w=view->instance; ++w->releases; w->reason=reason;
    CHECK(xr_xir_call_request_cancel(view->activation)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_poll_bounded(view->activation,1).status==XR_XIR_CALL_BUSY);
}
static XrXirCall *create(Witness *w,XrXirCallAccounting *accounting,uint64_t polls,uint32_t depth,bool exits) {
    const XrXirType parameter=XR_XIR_I64;
    XrXirCallEntry entries[]={
        {XR_XIR_CALL_ABI_VERSION,&parameter,1,XR_XIR_UNIT,sizeof(State),resume,release,NULL,exits ? XR_XIR_ENTRY_EXIT : 0,0},
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_UNIT,0,cleanup_resume,release,NULL,0,1}};
    XrXirCallConfig config;
    CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=entries; config.entry_count=exits ? 2 : 1; config.instance=w;
    config.byte_limit=1u<<20; config.poll_limit=polls; config.depth_limit=128; config.accounting=accounting;
    XrXirValue value={XR_XIR_I64,0,depth}; XrXirCall *call=NULL;
    XrXirCallStatus status=xr_xir_call_new(&config,0,&value,1,&call);
#ifdef XR_BOUNDED_INJECTED
    if (status==XR_XIR_CALL_OOM) return NULL;
#endif
    CHECK(status==XR_XIR_CALL_READY); return call;
}
static void freed(XrXirCall *call,XrXirCallAccounting *accounting) {
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);
    CHECK(!accounting->depth && !accounting->live_bytes && accounting->allocations==accounting->frees);
#ifdef XR_BOUNDED_INJECTED
    CHECK(!physical);
#endif
}
static void zero_and_exit(void) {
    CHECK(xr_xir_call_poll_bounded(NULL,0).status==XR_XIR_CALL_BAD_ARGUMENT);
    Witness w={0}; XrXirCallAccounting a={0}; XrXirCall *c=create(&w,&a,2,0,true);
    XrXirCallAccounting before=a;
    CHECK(xr_xir_call_poll_bounded(c,0).status==XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&a,&before,sizeof(a)));
    CHECK(xr_xir_call_poll_bounded(c,1).status==XR_XIR_CALL_READY && w.resumes==1 && !w.releases);
    CHECK(xr_xir_call_poll_bounded(c,1).status==XR_XIR_CALL_READY && w.resumes==2 && !w.releases);
    CHECK(xr_xir_call_poll_bounded(c,1).status==XR_XIR_CALL_RETURNED && w.releases==1 && a.polls==2);
    before=a;
    CHECK(xr_xir_call_poll_bounded(c,0).status==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_call_poll_bounded(c,UINT64_MAX).status==XR_XIR_CALL_RETURNED);
    CHECK(!memcmp(&a,&before,sizeof(a))); freed(c,&a);
    w=(Witness){0}; a=(XrXirCallAccounting){0}; c=create(&w,&a,1,0,true);
    CHECK(xr_xir_call_poll_bounded(c,1).status==XR_XIR_CALL_READY);
    CHECK(xr_xir_call_poll_bounded(c,1).status==XR_XIR_CALL_LIMIT && a.polls==1 && !w.exit_resumes);
    freed(c,&a);
}
static void deep_abort(void) {
    const uint32_t modes[]={1,3,4};
    for (size_t m=0;m<3;++m) {
        Witness w={0}; w.mode=modes[m]; XrXirCallAccounting a={0};
        if (m==0) w.mode=0;
        XrXirCall *c=create(&w,&a,m==0 ? 17 : 1000,31,false);
        XrXirCallStatus expected=m==0 ? XR_XIR_CALL_LIMIT : m==1 ? XR_XIR_CALL_OOM : XR_XIR_CALL_BAD_STATE;
        XrXirCallResult r={0}; bool draining=false;
        while (r.status==XR_XIR_CALL_READY) {
            uint32_t releases=w.releases; uint64_t polls=a.polls;
            r=xr_xir_call_poll_bounded(c,1);
            CHECK(w.releases-releases<=1 && a.polls-polls<=1 && (w.releases==releases || a.polls==polls));
            if (w.releases>releases || (m && a.polls==32)) {
                draining=true;
                CHECK(xr_xir_call_request_cancel(c)==XR_XIR_CALL_BAD_STATE);
                XrXirValue out={0}; CHECK(xr_xir_call_take_result(c,&out)==XR_XIR_CALL_BAD_STATE && !out.payload);
            }
            if (draining) CHECK(w.reason==expected || !w.releases);
        }
        CHECK(r.status==expected && w.releases==(m==0 ? 18u : 32u));
        CHECK(a.polls==(m==0 ? 17u : 32u)); freed(c,&a);
    }
}
static void cancellation(void) {
    Witness w={0}; w.mode=2; XrXirCallAccounting a={0}; XrXirCall *c=create(&w,&a,100,0,false);
    XrXirCallResult r=xr_xir_call_poll_bounded(c,1); CHECK(r.status==XR_XIR_CALL_SUSPENDED && r.wake);
    uint64_t polls=a.polls,bytes=a.live_bytes;
    CHECK(xr_xir_call_request_cancel(c)==XR_XIR_CALL_CANCEL_REQUESTED);
    CHECK(xr_xir_call_request_cancel(c)==XR_XIR_CALL_CANCEL_REQUESTED);
    CHECK(!w.releases && a.polls==polls && a.live_bytes==bytes);
    CHECK(xr_xir_call_resume(c,r.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_call_poll_bounded(c,1).status==XR_XIR_CALL_CANCELLED && w.releases==1);
    CHECK(xr_xir_call_request_cancel(c)==XR_XIR_CALL_BAD_STATE); freed(c,&a);
    for (unsigned mode=6;mode<=7;++mode) {
        w=(Witness){0}; w.mode=mode; a=(XrXirCallAccounting){0}; c=create(&w,&a,100,0,true);
        do { r=xr_xir_call_poll_bounded(c,1); } while (r.status==XR_XIR_CALL_READY);
        CHECK(r.status==XR_XIR_CALL_CANCELLED && w.cleanup_resumes==(mode==6 ? 1u : 0u));
        CHECK(w.exit_resumes==(mode==6 ? 2u : 1u)); freed(c,&a);
    }
    w=(Witness){0}; w.mode=1; a=(XrXirCallAccounting){0}; c=create(&w,&a,7,0,false);
    CHECK(xr_xir_call_poll_bounded(c,3).status==XR_XIR_CALL_READY && a.polls==3);
    CHECK(xr_xir_call_poll_bounded(c,3).status==XR_XIR_CALL_READY && a.polls==6);
    CHECK(xr_xir_call_poll_bounded(c,1).status==XR_XIR_CALL_READY && a.polls==7);
    CHECK(xr_xir_call_poll_bounded(c,1).status==XR_XIR_CALL_LIMIT && a.polls==7); freed(c,&a);
    w=(Witness){0}; w.mode=11; a=(XrXirCallAccounting){0}; c=create(&w,&a,100,31,false);
    CHECK(xr_xir_call_poll_bounded(c,32).status==XR_XIR_CALL_SUSPENDED && a.depth==32);
    CHECK(xr_xir_call_request_cancel(c)==XR_XIR_CALL_CANCEL_REQUESTED && !w.releases);
    for (unsigned remaining=32;remaining;--remaining) {
        r=xr_xir_call_poll_bounded(c,1);
        CHECK(a.depth==remaining-1 && w.releases==33-remaining && a.polls==32);
        CHECK(r.status==(remaining==1 ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_READY));
    }
    freed(c,&a);
}
static void scope_and_synchronous_free(void) {
    for (unsigned mode=8;mode<=9;++mode) {
        Witness w={0}; w.mode=mode; XrXirCallAccounting a={0}; XrXirCall *c=create(&w,&a,100,0,true);
        XrXirCallResult r={0}; unsigned slices=0;
        do {
            uint32_t released=w.releases; uint64_t polls=a.polls;
            r=xr_xir_call_poll_bounded(c,1); ++slices;
            CHECK(a.polls-polls+w.releases-released<=1);
        } while (r.status==XR_XIR_CALL_READY);
        CHECK(r.status==(mode==8 ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_CANCELLED));
        CHECK(w.exit_resumes==2 && w.releases==1 && slices==(mode==8 ? 6u : 5u));
        freed(c,&a);
    }
    Witness w={0}; w.mode=3; XrXirCallAccounting a={0}; XrXirCall *c=create(&w,&a,100,31,false);
    CHECK(xr_xir_call_poll_bounded(c,32).status==XR_XIR_CALL_READY && !w.releases);
    CHECK(xr_xir_call_request_cancel(c)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_call_free(c)==XR_XIR_CALL_OOM && w.releases==32 && a.polls==32);
    CHECK(!a.depth && !a.live_bytes && a.allocations==a.frees);
}
#ifdef XR_BOUNDED_INJECTED
static void oom(void) {
    size_t sites=0;
    for (size_t index=0;index<=sites;++index) {
        allocation_attempts=0; allocation_failure=index ? index-1 : SIZE_MAX;
        Witness w={0}; XrXirCallAccounting a={0}; XrXirCall *c=create(&w,&a,1000,31,false);
        XrXirCallStatus status=XR_XIR_CALL_OOM;
        if (c) {
            do { uint32_t released=w.releases; status=xr_xir_call_poll_bounded(c,1).status; CHECK(w.releases-released<=1); }
            while (status==XR_XIR_CALL_READY);
            freed(c,&a);
        }
        if (!index) { CHECK(status==XR_XIR_CALL_RETURNED); sites=allocation_attempts; }
        else CHECK(status==XR_XIR_CALL_OOM && allocation_attempts>allocation_failure);
        CHECK(!physical && !a.live_bytes && a.allocations==a.frees);
    }
    allocation_failure=SIZE_MAX; printf("actual Call allocation OOM points=%zu; physical zero\n",sites);
}
#endif
int main(void) {
    zero_and_exit(); deep_abort(); cancellation(); scope_and_synchronous_free();
#ifdef XR_BOUNDED_INJECTED
    oom();
#endif
    puts("bounded resume/exit/pop, cumulative polls, sticky abort and cleanup mask PASS"); return 0;
}
