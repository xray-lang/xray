/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_select_fixture.h - Actual typed Channel and allocation owners
 *
 * KEY CONCEPT:
 *   This fixture proves only the channels used by the selection kernel and
 *   reuses the existing physical allocation and queue-lock observer.
 */
#ifndef XIR_CHANNEL_SELECT_FIXTURE_H
#define XIR_CHANNEL_SELECT_FIXTURE_H
#include "base/xmalloc.h"
#include "base/xcompile_resources.h"
#include "xir/xxir_channel.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_constraint_proof.h"
#include "shared/xr_sync_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);} } while(0)
#include "xir_channel_value_observer.h"
enum { RESULT_CHANNEL_I64=256,RESULT_CHANNEL_STRING };
typedef struct ResultOwner {
    XrXirCompileContext context;
    XrXirTypeArena *arena;
    XrXirDomain *domain;
    XrXirValueAdmission admission;
} ResultOwner;
static bool result_empty(const XrXirValue *value) {return !value->type&&!value->reserved&&!value->payload;}
static XrXirValue result_integer(int64_t value) {return (XrXirValue){XR_XIR_I64,0,value};}
static void result_owner_ready(ResultOwner *owner) {
    XrCompileResourceLimits limits={8388608,4194304,128000000};
    CHECK(xr_compile_resources_new(&limits,&owner->context.resources)==XR_COMPILE_RESOURCE_OK);
    owner->context.limits=xr_xir_compile_default_limits();
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_CHANNEL,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=XR_XIR_STRING}};
    XrXirTypes types={nodes,2,NULL,NULL};XrXirFunction function={0};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=&function,.function_count=1,.types=&types};
    for(unsigned i=0;i<2;++i)CHECK(xr_xir_compile_type_constraints(&owner->context,&module,0,
        (XrXirType)(RESULT_CHANNEL_I64+i),(XrXirConstraint){.markers=XR_XIR_CONSTRAINT_SENDABLE})==XR_XIR_OK);
    CHECK(xr_xir_compile_type_arena_new(&owner->context,&types,&owner->arena)==XR_XIR_VALUE_OK);
    XrXirDomainBudgetControls controls={8388608,8388608,128000000,4194304,4194304};
    CHECK(xr_xir_domain_new_budgeted(8388608,&controls,&owner->domain)==XR_XIR_VALUE_OK);
    owner->admission=(XrXirValueAdmission){.arena=owner->arena,.domain=owner->domain,.work=128000000,.scratch_bytes=4194304};
}
static void result_owner_free(ResultOwner *owner) {
    xr_xir_domain_drop(owner->domain);xr_xir_compile_type_arena_drop(owner->arena);
    xr_compile_resources_release(owner->context.resources);*owner=(ResultOwner){0};
    CHECK(!compiler_memory.live&&!compiler_memory.bytes&&!runtime_memory.live&&!runtime_memory.bytes);
}
static void result_send(ResultOwner *owner,const XrXirValue *channel,XrXirValue *value) {
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_send_owned(channel,value,&owner->admission,&state)==XR_XIR_VALUE_OK&&
        state==XR_XIR_CHANNEL_SENT&&result_empty(value));
}
#endif // XIR_CHANNEL_SELECT_FIXTURE_H
