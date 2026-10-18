/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_return_move.c - Real driver ownership transfer and protocol exits
 *
 * KEY CONCEPT:
 *   Producer slots are empty before release, while the receiving owner stays live.
 */
#include "xir/xxir_call_move_internal.h"
#include "xir/xxir_type_arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)

typedef struct MoveState {XrXirValue value;uint32_t pc;} MoveState;
typedef struct MoveWitness {
    XrXirDomain *domain;
    uint32_t mode,resumes,releases,empty_releases,parent_reads,exit_resumes;
    bool nested;
} MoveWitness;
enum {MOVE_OK,MOVE_TAMPER,MOVE_CONTINUE,MOVE_LOW_WORK,MOVE_WRONG_TYPE,MOVE_CANCEL,MOVE_CANCEL_EXIT,MOVE_CANCEL_BEFORE};

static XrXirAction move_resume(XrXirCallView *view) {
    MoveWitness *w=(MoveWitness *)view->environment;MoveState *state=view->state;
    if (view->phase==XR_XIR_CALL_EXIT) {
        ++w->exit_resumes;return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};
    }
    ++w->resumes;
    CHECK(xr_xir_string_new(w->domain,"late\0return",11,&state->value)==XR_XIR_VALUE_OK);
    XrXirValue original=state->value;XrXirAction action={.kind=XR_XIR_ACTION_CONTINUE};
    XrXirAction before=action;XrXirCallView copied=*view;
    CHECK(xr_xir_call_return_move(&copied,&state->value,&action)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_call_return_move(view,&view->inbox.value,&action)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_call_return_move(view,&state->value,(XrXirAction *)&state->value)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(state->value.payload==original.payload && action.kind==before.kind);
    XrXirValueAdmission *admission=xr_xir_call_admission(view);CHECK(admission);
    if (w->mode==MOVE_LOW_WORK) admission->work=sizeof(action)+2*sizeof(original)-1;
    else admission->work=sizeof(action)+2*sizeof(original);
    if (w->mode==MOVE_CANCEL_BEFORE)
        CHECK(xr_xir_call_request_cancel(view->activation)==XR_XIR_CALL_CANCEL_REQUESTED);
    XrXirCallStatus status=xr_xir_call_return_move(view,&state->value,&action);
    if (w->mode==MOVE_LOW_WORK || w->mode==MOVE_WRONG_TYPE || w->mode==MOVE_CANCEL_BEFORE) {
        CHECK(status==(w->mode==MOVE_LOW_WORK?XR_XIR_CALL_LIMIT:w->mode==MOVE_CANCEL_BEFORE?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_BAD_ARGUMENT));
        CHECK(state->value.type==original.type && state->value.payload==original.payload && action.kind==before.kind);
        return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,
            .value={XR_XIR_I64,0,w->mode==MOVE_LOW_WORK?XR_XIR_CALL_LIMIT:XR_XIR_CALL_BAD_ARGUMENT}};
    }
    CHECK(status==XR_XIR_CALL_READY && !state->value.type && !state->value.payload && !admission->work);
    XrXirAction same=action;
    CHECK(xr_xir_call_return_move(view,&state->value,&same)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(same.kind==action.kind && same.value.payload==original.payload);
    if (w->mode==MOVE_TAMPER) action.value=(XrXirValue){XR_XIR_STRING,0,0};
    if (w->mode==MOVE_CONTINUE) action=(XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};
    if (w->mode==MOVE_CANCEL || w->mode==MOVE_CANCEL_EXIT)
        CHECK(xr_xir_call_request_cancel(view->activation)==XR_XIR_CALL_CANCEL_REQUESTED);
    return action;
}
static void move_release(XrXirCallView *view,XrXirCallStatus reason) {
    MoveWitness *w=(MoveWitness *)view->environment;MoveState *state=view->state;(void)reason;
    ++w->releases;if (!state->value.type) ++w->empty_releases;
    xr_xir_value_drop(&state->value);
}
static XrXirAction move_parent(XrXirCallView *view) {
    MoveWitness *w=(MoveWitness *)view->environment;MoveState *state=view->state;
    if (!state->pc++) return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=1};
    CHECK(w->releases==1 && w->empty_releases==1 && view->inbox.status==XR_XIR_CALL_RETURNED);
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&view->inbox.value,&bytes,&length) && length==11 && !memcmp(bytes,"late\0return",11));
    ++w->parent_reads;
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,77}};
}
static void move_case(uint32_t mode,bool nested) {
    MoveWitness witness={.mode=mode,.nested=nested};
    CHECK(xr_xir_domain_new(65536,&witness.domain)==XR_XIR_VALUE_OK);
    XrXirDomainStats initial=xr_xir_domain_stats(witness.domain);
    XrXirCallAccounting accounting={0};XrXirCallConfig config={0};
    XrXirCallEntry entries[2]={
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=nested?XR_XIR_I64:mode==MOVE_WRONG_TYPE?XR_XIR_I64:XR_XIR_STRING,
         .state_bytes=sizeof(MoveState),.resume=nested?move_parent:move_resume,.release=nested?NULL:move_release,.environment=&witness},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_STRING,.state_bytes=sizeof(MoveState),
         .resume=move_resume,.release=move_release,.environment=&witness}
    };
    if (mode==MOVE_CANCEL_EXIT) entries[0].flags=XR_XIR_ENTRY_EXIT;
    CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=entries;config.entry_count=nested?2:1;config.accounting=&accounting;config.admission.domain=witness.domain;
    config.admission.work=100000;config.poll_limit=32;config.byte_limit=65536;
    XrXirCall *call=NULL;CHECK(xr_xir_call_new(&config,0,NULL,0,&call)==XR_XIR_CALL_READY);
    XrXirCallResult result=xr_xir_call_poll_bounded(call,UINT64_MAX);
    XrXirCallStatus expected=mode==MOVE_TAMPER || mode==MOVE_CONTINUE?XR_XIR_CALL_BAD_STATE:
        mode==MOVE_LOW_WORK?XR_XIR_CALL_LIMIT:mode==MOVE_WRONG_TYPE?XR_XIR_CALL_BAD_ARGUMENT:
        mode==MOVE_CANCEL || mode==MOVE_CANCEL_EXIT || mode==MOVE_CANCEL_BEFORE?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_RETURNED;
    CHECK(result.status==expected && witness.resumes==1 && witness.releases==1);
    CHECK(witness.empty_releases==(mode==MOVE_LOW_WORK || mode==MOVE_WRONG_TYPE || mode==MOVE_CANCEL_BEFORE?0u:1u));
    CHECK(witness.exit_resumes==(mode==MOVE_CANCEL_EXIT?1u:0u));
    if (expected==XR_XIR_CALL_RETURNED) {
        XrXirValue output={0};CHECK(xr_xir_call_take_result(call,&output)==expected);
        if (nested) CHECK(output.payload==77 && witness.parent_reads==1);
        else {const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&output,&bytes,&length) && length==11 && !memcmp(bytes,"late\0return",11));}
        xr_xir_value_drop(&output);
    }
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && !accounting.live_bytes && !accounting.depth && accounting.allocations==accounting.frees);
    XrXirDomainStats stats=xr_xir_domain_stats(witness.domain);
    CHECK(stats.live_bytes==initial.live_bytes && stats.allocations-initial.allocations==stats.frees-initial.frees);
    xr_xir_domain_drop(witness.domain);
}
typedef struct ReentryOwner {
    XrXirCallView *view;XrXirValue *source;
    uint32_t checks,frame_releases,binding_releases;
} ReentryOwner;
static void reentry_binding_release(void *context) {++((ReentryOwner *)context)->binding_releases;}
static XrXirValueStatus reentry_admit(void *context,const XrXirFunctionBinding *binding,
    XrXirType type,uint64_t *work) {
    ReentryOwner *owner=context;
    if (!work || !*work) return XR_XIR_VALUE_LIMIT;
    --*work;
    if (owner->view) {
        XrXirValue before=*owner->source;XrXirAction action={.kind=XR_XIR_ACTION_CONTINUE};
        CHECK(xr_xir_call_return_move(owner->view,owner->source,&action)==XR_XIR_CALL_BUSY);
        CHECK(owner->source->payload==before.payload && owner->source->type==before.type && action.kind==XR_XIR_ACTION_CONTINUE);
        ++owner->checks;
    }
    return binding && binding->owner==owner && binding->release==reentry_binding_release &&
        binding->entry==1 && !binding->captures && !binding->capture_count && type==(XrXirType)256 ?
        XR_XIR_VALUE_OK:XR_XIR_VALUE_BAD_ARGUMENT;
}
static XrXirAction reentry_resume(XrXirCallView *view) {
    ReentryOwner *owner=(ReentryOwner *)view->environment;
    MoveState *state=view->state;XrXirValueAdmission *admission=xr_xir_call_admission(view);
    XrXirFunctionBinding binding={owner,reentry_binding_release,1,NULL,0};
    CHECK(xr_xir_function_new(admission->domain,(XrXirTypeArena *)admission->arena,
        (XrXirType)256,&binding,admission,&state->value)==XR_XIR_VALUE_OK);
    owner->view=view;owner->source=&state->value;XrXirAction action={0};
    CHECK(xr_xir_call_return_move(view,&state->value,&action)==XR_XIR_CALL_READY && !state->value.type);
    owner->view=NULL;owner->source=NULL;return action;
}
static void reentry_release(XrXirCallView *view,XrXirCallStatus reason) {
    ReentryOwner *owner=(ReentryOwner *)view->environment;MoveState *state=view->state;
    CHECK(reason==XR_XIR_CALL_RETURNED && !state->value.type);++owner->frame_releases;
}
static XrXirAction reentry_target(XrXirCallView *view) {
    (void)view;return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,1}};
}
static void reentry_case(void) {
    const XrCompileResourceLimits limits={1048576,1048576,1000000};XrXirCompileContext c={0};
    CHECK(xr_compile_resources_new(&limits,&c.resources)==XR_COMPILE_RESOURCE_OK);c.limits=xr_xir_compile_default_limits();
    XrCompileResourceStats initial={0};CHECK(xr_compile_resources_stats(c.resources,&initial)==XR_COMPILE_RESOURCE_OK);
    const XrXirTypeNode node={.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    const XrXirTypes types={&node,1,NULL,NULL};XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(&c,&types,&arena)==XR_XIR_VALUE_OK && xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    ReentryOwner owner={0};XrXirCallAccounting accounting={0};XrXirCallConfig config={0};
    const XrXirCallEntry entries[]={
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=(XrXirType)256,.state_bytes=sizeof(MoveState),
         .resume=reentry_resume,.release=reentry_release,.environment=&owner},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_I64,.resume=reentry_target}
    };
    CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=entries;config.entry_count=2;config.accounting=&accounting;
    config.admission=(XrXirValueAdmission){arena,domain,reentry_admit,&owner,100000,65536};
    XrXirCall *call=NULL;CHECK(xr_xir_call_new(&config,0,NULL,0,&call)==XR_XIR_CALL_READY);
    CHECK(xr_xir_call_poll_bounded(call,UINT64_MAX).status==XR_XIR_CALL_RETURNED);
    XrXirValue output={0};CHECK(xr_xir_call_take_result(call,&output)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && owner.checks==1 && owner.frame_releases==1 && !owner.binding_releases);
    const XrXirFunctionBinding *binding=xr_xir_function_binding(&output);CHECK(binding && binding->entry==1 && binding->owner==&owner);
    xr_xir_value_drop(&output);CHECK(owner.binding_releases==1 && !accounting.live_bytes && accounting.allocations==accounting.frees);
    xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);XrCompileResourceStats after={0};
    CHECK(xr_compile_resources_stats(c.resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==initial.live_bytes);
    xr_compile_resources_release(c.resources);
}
int main(void) {
    XrXirValue source={XR_XIR_I64,0,1};XrXirAction action={.kind=XR_XIR_ACTION_CONTINUE};
    CHECK(xr_xir_call_return_move(NULL,&source,&action)==XR_XIR_CALL_BAD_ARGUMENT && source.payload==1 && action.kind==XR_XIR_ACTION_CONTINUE);
    for (uint32_t mode=MOVE_OK;mode<=MOVE_CANCEL_BEFORE;++mode) move_case(mode,false);
    move_case(MOVE_OK,true);
    reentry_case();
    puts("Real Call return moves: atomic state transfer, exact action, failed inputs intact, cancellation and late parent read PASS");return 0;
}
