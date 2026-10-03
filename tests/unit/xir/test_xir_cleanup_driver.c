/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_cleanup_driver.c - Exit ownership, barriers and cancellation witnesses
 */
#include "xir/xxir_call.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_error_fixture.h"
static size_t attempts, fail_at = SIZE_MAX, live;
static void *exit_calloc(size_t count, size_t size) {
    if (attempts++ == fail_at) return NULL;
    void *p = calloc(count, size); if (p) ++live; return p;
}
static void exit_free(void *p) { if (p) { CHECK(live); --live; free(p); } }
#undef xr_calloc
#undef xr_free
#define xr_calloc exit_calloc
#define xr_free exit_free
#include "xir/xxir_call.c"
#undef xr_calloc
#undef xr_free

typedef struct ExitFrame { uint32_t pc, frontier; bool waiting; XrXirValue value, argument; } ExitFrame;
typedef struct ExitWitness {
    XrXirDomain *domain;
    XrXirValue error;
    ExitFrame *owner;
    uint32_t mode, events[32], count, released, normal_continuations;
} ExitWitness;
static XrXirAction exit_action(XrXirActionKind kind) {
    return (XrXirAction){kind,0,NULL,0,{0},{0},0};
}
static void exit_event(ExitWitness *w, uint32_t event) { CHECK(w->count < 32); w->events[w->count++] = event; }
static XrXirAction exit_owner_resume(XrXirCallView *view) {
    ExitWitness *w = (ExitWitness *)view->environment; ExitFrame *s = view->state;
    if (view->phase == XR_XIR_CALL_EXIT) {
        CHECK(xr_xir_call_cleanup_active(view->activation));
        XrXirCallView copied = *view; CHECK(!xr_xir_call_admission(&copied));
        if (s->waiting) { CHECK(view->inbox.status == XR_XIR_CALL_RETURNED); --s->frontier; s->waiting = false; }
        uint32_t target = view->scope_exit ? 1u : 0u;
        if (s->frontier == target) return exit_action(XR_XIR_ACTION_EXIT_DONE);
        CHECK(s->frontier > target);
        s->argument = (XrXirValue){XR_XIR_I64,0,s->frontier}; s->waiting = true;
        if (view->exit.status == XR_XIR_CALL_RETURNED) xr_xir_value_drop(&s->value);
        return (XrXirAction){XR_XIR_ACTION_CALL,w->mode == 21 ? 2u : 1u,&s->argument,1,{0},{0},XR_XIR_ACTION_CLEANUP};
    }
    CHECK(!xr_xir_call_cleanup_active(view->activation));
    if (!s->pc++) {
        w->owner = s; s->frontier = 2;
        CHECK(xr_xir_string_new(w->domain,"saved",5,&s->value) == XR_XIR_VALUE_OK);
        if (w->mode == 1 || w->mode == 14) return exit_action(XR_XIR_ACTION_SUSPEND);
        if (w->mode == 17 || w->mode == 24)
            return (XrXirAction){XR_XIR_ACTION_LEAVE,0,NULL,0,w->error,{0},XR_XIR_ACTION_LEAVE_ERROR};
        if (w->mode == 18 || w->mode == 25)
            return (XrXirAction){XR_XIR_ACTION_LEAVE,0,NULL,0,{XR_XIR_I64,0,XR_XIR_CALL_DIVIDE_BY_ZERO},{{420,0,0,0},{0}},XR_XIR_ACTION_LEAVE_PANIC};
        if (w->mode == 2 || w->mode == 15) return exit_action(XR_XIR_ACTION_LEAVE);
    } else {
        ++w->normal_continuations;
        if (w->mode == 17) CHECK(view->inbox.status == XR_XIR_CALL_THROWN && error_fixture_is_code(&view->inbox.value,w->domain,91));
        if (w->mode == 18) CHECK(view->inbox.status == XR_XIR_CALL_DIVIDE_BY_ZERO && view->inbox.panic.detail.code == 420);
    }
    if (w->mode == 20) return (XrXirAction){XR_XIR_ACTION_CALL,1,&s->argument,1,{0},{0},0};
    if (w->mode == 22) return exit_action(XR_XIR_ACTION_EXIT_DONE);
    if (w->mode == 9) return (XrXirAction){XR_XIR_ACTION_THROW,0,NULL,0,w->error,{0},0};
    return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,s->value,{0},0};
}
static XrXirAction exit_child_resume(XrXirCallView *view) {
    ExitWitness *w = (ExitWitness *)view->environment;
    CHECK(xr_xir_call_cleanup_active(view->activation));
    exit_event(w,30);
    if (w->mode == 3 || w->mode == 15) {
        CHECK(xr_xir_call_request_cancel(view->activation) == XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(xr_xir_call_state(view->activation) == XR_XIR_CALL_BUSY);
        CHECK(xr_xir_call_free(view->activation) == XR_XIR_CALL_BUSY);
    }
    if (w->mode == 4 || w->mode == 5 || w->mode == 8 || w->mode == 9 || w->mode == 24 || w->mode == 25)
        return xr_xir_call_numeric_fault(XR_XIR_RUN_DIVIDE_BY_ZERO, false);
    if (w->mode == 6 || w->mode == 7) return exit_action(XR_XIR_ACTION_SUSPEND);
    if (w->mode == 11) return (XrXirAction){XR_XIR_ACTION_THROW,0,NULL,0,w->error,{0},0};
    if (w->mode == 12) return xr_xir_call_fault(XR_XIR_RUN_OUT_OF_MEMORY);
    return exit_action(XR_XIR_ACTION_RETURN);
}
static XrXirAction exit_body_resume(XrXirCallView *view) {
    ExitWitness *w = (ExitWitness *)view->environment; uint32_t *pc = view->state;
    CHECK(xr_xir_call_cleanup_active(view->activation));
    uint32_t number = (uint32_t)view->arguments[0].payload;
    if (!(*pc)++) {
        exit_event(w,10 + number);
        return (XrXirAction){XR_XIR_ACTION_CALL,2,NULL,0,{0},{0},
            w->mode == 4 || w->mode == 7 ? XR_XIR_ACTION_PROTECTED : 0};
    }
    if (view->inbox.status == XR_XIR_CALL_THROWN)
        return (XrXirAction){XR_XIR_ACTION_THROW,0,NULL,0,view->inbox.value,{0},0};
    if (w->mode == 4) CHECK(view->inbox.status == XR_XIR_CALL_DIVIDE_BY_ZERO && view->inbox.panic.detail.code == 420);
    else if (w->mode == 7) CHECK(view->inbox.status == XR_XIR_CALL_DEFER_ASYNC && view->inbox.panic.detail.code == 444 && !view->inbox.wake);
    else CHECK(view->inbox.status == XR_XIR_CALL_RETURNED);
    exit_event(w,20 + number);
    return exit_action(XR_XIR_ACTION_RETURN);
}
static XrXirAction exit_outer_resume(XrXirCallView *view) {
    uint32_t *pc = view->state;
    if (!(*pc)++) return (XrXirAction){XR_XIR_ACTION_CALL,0,NULL,0,{0},{0},XR_XIR_ACTION_PROTECTED};
    CHECK(false); return exit_action(XR_XIR_ACTION_RETURN);
}
static void exit_release(XrXirCallView *view, XrXirCallStatus reason) {
    ExitWitness *w = (ExitWitness *)view->environment; (void)reason;
    CHECK(!xr_xir_call_admission(view)); ++w->released;
    if (view->state == w->owner) xr_xir_value_drop(&w->owner->value);
}
static size_t exit_run(uint32_t mode, uint64_t polls, uint32_t depth) {
    XrXirDomain *domain = NULL, *metadata = NULL;
    CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(65536,&metadata) == XR_XIR_VALUE_OK);
    uint64_t domain_base = xr_xir_domain_stats(domain).live_bytes, metadata_base = xr_xir_domain_stats(metadata).live_bytes;
    XrXirTypeArena *arena = error_fixture_arena(metadata);
    XrXirValueAdmission admission = error_fixture_admission(domain,arena);
    ExitWitness w = {0}; w.domain = domain; w.mode = mode; w.error = error_fixture_code(&admission,91);
    const XrXirType argument = XR_XIR_I64;
    XrXirCallEntry entries[] = {
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_STRING,sizeof(ExitFrame),exit_owner_resume,exit_release,&w,XR_XIR_ENTRY_EXIT,0},
        {XR_XIR_CALL_ABI_VERSION,&argument,1,XR_XIR_UNIT,8193,exit_body_resume,exit_release,&w,0,1},
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_UNIT,0,exit_child_resume,exit_release,&w,0,0},
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_STRING,sizeof(uint32_t),exit_outer_resume,exit_release,&w,0,0}};
    XrXirCallAccounting accounting = {0};
    XrXirCallConfig config; CHECK(xr_xir_call_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); config.entries = entries; config.entry_count = 4; config.instance = NULL; config.byte_limit = 65536; config.poll_limit = polls; config.depth_limit = depth; config.accounting = &accounting; config.output = (XrXirOutputProvider) {0}; config.admission = admission;
    XrXirCall *call = NULL; attempts = 0;
    XrXirCallStatus status = xr_xir_call_new(&config,mode == 8 ? 3u : 0u,NULL,0,&call);
    XrXirValue owned = {0};
    if (status == XR_XIR_CALL_READY) {
        if (mode == 13) status = xr_xir_call_request_cancel(call);
        else status = xr_xir_call_poll_bounded(call, UINT64_MAX).status;
        if (status == XR_XIR_CALL_SUSPENDED) {
            if (mode == 14) { CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY); call = NULL; status = XR_XIR_CALL_CANCELLED; }
            else status = xr_xir_call_request_cancel(call);
        }
        if (status == XR_XIR_CALL_CANCEL_REQUESTED) status = xr_xir_call_poll_bounded(call, UINT64_MAX).status;
        if (status == XR_XIR_CALL_RETURNED) {
            CHECK(xr_xir_call_take_result(call,&owned) == XR_XIR_CALL_RETURNED);
            const uint32_t expected[] = {12,30,22,11,30,21};
            CHECK(w.count == 6 && !memcmp(w.events,expected,sizeof(expected)));
            CHECK(w.normal_continuations == (mode == 2 || mode == 17 || mode == 18 ? 1u : 0u));
        } else if (status == XR_XIR_CALL_CANCELLED) {
            CHECK(w.count == (mode == 13 ? 0u : 6u) && !w.normal_continuations);
        } else CHECK(status == XR_XIR_CALL_OOM || status == XR_XIR_CALL_LIMIT || status == XR_XIR_CALL_BAD_STATE);
        if (call) CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
    } else CHECK(status == XR_XIR_CALL_OOM || status == XR_XIR_CALL_LIMIT);
    if (fail_at == SIZE_MAX && polls == 1000 && depth == 16) {
        XrXirCallStatus expected = mode == 1 || mode == 3 || mode == 13 || mode == 14 || mode == 15 ? XR_XIR_CALL_CANCELLED :
            mode == 12 ? XR_XIR_CALL_OOM : mode >= 20 ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_RETURNED;
        CHECK(status == expected);
    }
    size_t sites = attempts;
    CHECK(!live && !accounting.live_bytes && accounting.allocations == accounting.frees && !accounting.depth);
    if (owned.type) { const char *bytes; size_t length; CHECK(xr_xir_string_view(&owned,&bytes,&length) && length == 5 && !memcmp(bytes,"saved",5)); }
    xr_xir_value_drop(&owned); xr_xir_value_drop(&w.error); xr_xir_type_arena_drop(arena);
    CHECK(xr_xir_domain_stats(domain).live_bytes == domain_base && xr_xir_domain_stats(metadata).live_bytes == metadata_base);
    xr_xir_domain_drop(domain); xr_xir_domain_drop(metadata);
    return sites;
}
#include "xir_cleanup_instance_cases.h"
int main(int argc, char **argv) {
    if (argc == 2) { exit_run((uint32_t)atoi(argv[1]),1000,16); return 1; }
    cleanup_instance_cases();
    const uint32_t modes[] = {0,1,2,3,4,7,12,13,14,15,17,18,20,21,22};
    for (size_t i = 0; i < sizeof(modes)/sizeof(modes[0]); ++i) exit_run(modes[i],1000,16);
    for (uint64_t limit = 1; limit < 24; ++limit) exit_run(0,limit,16);
    exit_run(0,1000,1); exit_run(0,1000,2);
    size_t sites = exit_run(0,1000,16);
    for (size_t i = 0; i < sites; ++i) { fail_at = i; exit_run(0,1000,16); }
    fail_at = SIZE_MAX;
    printf("Cleanup exits, owned results, cancellation, budgets and %zu allocation failures passed\n",sites);
    return 0;
}
