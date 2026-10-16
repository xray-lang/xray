/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_native.c - Pull real generated entries with independent frames
 *
 * KEY CONCEPT:
 *   Native production consumes the same typed yield and scheduling actions.
 */
#include "xir/xxir_program.h"
#include "xir/xxir_call_generator.h"
#include "xir/xxir_type_arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
XR_DATA const XrXirProgramSpec generator_source_program;
static void pull(XrXirCall *call,bool unit,int64_t expected,bool wait) {
    XrXirCallResult r=xr_xir_call_poll_bounded(call,1000);
    if(wait) {
        CHECK(r.status==XR_XIR_CALL_SUSPENDED && r.wake);
        CHECK(xr_xir_call_resume(call,r.wake)==XR_XIR_CALL_READY);
        r=xr_xir_call_poll_bounded(call,1000);
    }
    CHECK(r.status==XR_XIR_CALL_YIELDED && !r.wake);
    XrXirValue value={0};CHECK(xr_xir_call_generator_take(call,&value)==XR_XIR_CALL_YIELDED);
    CHECK(value.type==(uint32_t)(unit ? XR_XIR_UNIT : XR_XIR_I64) && value.payload==expected);
    xr_xir_value_drop(&value);
}
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(&owner.context,generator_source_program.types,&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    CHECK(generator_source_program.entry_count==6);
    CHECK(generator_source_program.entries[1].flags==XR_XIR_ENTRY_GENERATOR);
    CHECK(generator_source_program.entries[2].flags==XR_XIR_ENTRY_GENERATOR);
    XrXirCallConfig config;CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirCallAccounting accounts[2]={{0}};config.entries=generator_source_program.entries;
    config.entry_count=generator_source_program.entry_count;config.accounting=&accounts[0];
    config.admission=(XrXirValueAdmission){arena,domain,NULL,NULL,1000000,65536};
    XrXirCall *left=NULL,*right=NULL;
    CHECK(xr_xir_call_generator_new(&config,1,NULL,0,&left)==XR_XIR_CALL_READY);
    config.accounting=&accounts[1];CHECK(xr_xir_call_generator_new(&config,1,NULL,0,&right)==XR_XIR_CALL_READY);
    pull(left,false,11,false);pull(right,false,11,false);
    CHECK(xr_xir_call_free(left)==XR_XIR_CALL_READY);pull(right,false,22,true);
    CHECK(xr_xir_call_poll_bounded(right,1000).status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_call_free(right)==XR_XIR_CALL_READY);left=NULL;config.accounting=&accounts[0];
    CHECK(xr_xir_call_generator_new(&config,2,NULL,0,&left)==XR_XIR_CALL_READY);
    pull(left,true,0,false);pull(left,true,0,false);
    CHECK(xr_xir_call_poll_bounded(left,1000).status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_call_free(left)==XR_XIR_CALL_READY);
    for(unsigned i=0;i<2;++i) CHECK(!accounts[i].live_bytes && accounts[i].allocations==accounts[i].frees && !accounts[i].depth);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);source_fixture_owner_free(&owner);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Real Source generated native independent frames, Unit versus completion and wake separation; physical0 PASS");return 0;
}
