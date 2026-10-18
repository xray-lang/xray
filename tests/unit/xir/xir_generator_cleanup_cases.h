/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_cleanup_cases.h - Exact cleanup witnesses across generator exits
 *
 * KEY CONCEPT:
 *   Real generated and interpreted bodies share the canonical exit driver.
 */
#ifndef XIR_GENERATOR_CLEANUP_CASES_H
#define XIR_GENERATOR_CLEANUP_CASES_H
#include "xir/xxir_call_generator.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_types.h"
#include "xir/xxir_enum.h"
#include <string.h>
typedef struct GeneratorCleanupLog {uint32_t count;int64_t expected;} GeneratorCleanupLog;
static XrXirOutputStatus generator_cleanup_output(void *opaque,const XrXirOutputGroup *group) {
    GeneratorCleanupLog *log=opaque;
    CHECK(group && group->line && group->stream==XR_XIR_STDOUT && group->count==1);
    CHECK(group->values[0].type==XR_XIR_I64 && group->values[0].payload==log->expected);
    ++log->count;return XR_XIR_OUTPUT_OK;
}
static void generator_cleanup_string(const XrXirValue *value,const char *expected) {
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(value,&bytes,&length));
    CHECK(length==strlen(expected) && !memcmp(bytes,expected,length));
}
static XrXirCall *generator_cleanup_new(XrXirCallConfig *config,uint32_t entry) {
    XrXirCall *call=NULL;XrXirValue arguments[2]={{0}};uint32_t count=entry==1 ? 2u : 0u;
    if(count) {
        CHECK(xr_xir_string_new(config->admission.domain,"first",5,&arguments[0])==XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_new(config->admission.domain,"second",6,&arguments[1])==XR_XIR_VALUE_OK);
    }
    CHECK(xr_xir_call_generator_new(config,entry,count ? arguments : NULL,count,&call)==XR_XIR_CALL_READY && call);
    xr_xir_value_drop(&arguments[0]);xr_xir_value_drop(&arguments[1]);
    return call;
}
static void generator_cleanup_cases(const XrXirCallEntry *entries,uint32_t count,
    XrXirTypeArena *arena,XrXirDomain *domain) {
    CHECK(count>3 && (entries[1].flags & XR_XIR_ENTRY_GENERATOR) &&
        (entries[1].flags & XR_XIR_ENTRY_EXIT));
    XrXirCallAccounting accounting={0};GeneratorCleanupLog log={0,301};
    XrXirCallConfig config;CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=entries;config.entry_count=count;config.byte_limit=65536;
    config.accounting=&accounting;config.admission=(XrXirValueAdmission){arena,domain,NULL,NULL,1000000,65536};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,generator_cleanup_output,&log};
    for(unsigned mode=0;mode<5;++mode) {
        log.count=0;XrXirCall *call=generator_cleanup_new(&config,1);XrXirValue first={0};
        if(mode) {
            XrXirCallResult observed=xr_xir_call_poll_bounded(call,10000);
            if(observed.status!=XR_XIR_CALL_YIELDED || log.count)
                fprintf(stderr,"Generator cleanup mode%u status%u outputs%u\n",mode,observed.status,log.count);
            CHECK(observed.status==XR_XIR_CALL_YIELDED && !log.count);
            if(mode!=2) {
                CHECK(xr_xir_call_generator_take(call,&first)==XR_XIR_CALL_YIELDED);
                generator_cleanup_string(&first,"first");
            }
            if(mode>=3) {
                XrXirCallResult wait=xr_xir_call_poll_bounded(call,10000);
                CHECK(wait.status==XR_XIR_CALL_SUSPENDED && wait.wake && !log.count);
                if(mode==4) {
                    CHECK(xr_xir_call_resume(call,wait.wake)==XR_XIR_CALL_READY);
                    CHECK(xr_xir_call_poll_bounded(call,10000).status==XR_XIR_CALL_YIELDED && !log.count);
                    XrXirValue second={0};CHECK(xr_xir_call_generator_take(call,&second)==XR_XIR_CALL_YIELDED);
                    generator_cleanup_string(&second,"second");xr_xir_value_drop(&second);
                    CHECK(xr_xir_call_poll_bounded(call,10000).status==XR_XIR_CALL_RETURNED && log.count==1);
                    CHECK(xr_xir_call_poll_bounded(call,10000).status==XR_XIR_CALL_RETURNED && log.count==1);
                }
            }
        }
        CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);
        CHECK(log.count==(mode ? 1u : 0u));
        if(first.type) generator_cleanup_string(&first,"first");
        xr_xir_value_drop(&first);
        CHECK(!accounting.live_bytes && !accounting.depth && accounting.allocations==accounting.frees);
    }
    log=(GeneratorCleanupLog){0,302};XrXirCall *call=generator_cleanup_new(&config,2);
    CHECK(xr_xir_call_poll_bounded(call,10000).status==XR_XIR_CALL_YIELDED && !log.count);
    XrXirValue number={0};CHECK(xr_xir_call_generator_take(call,&number)==XR_XIR_CALL_YIELDED);
    CHECK(number.type==XR_XIR_I64 && number.payload==7);xr_xir_value_drop(&number);
    CHECK(xr_xir_call_poll_bounded(call,10000).status==XR_XIR_CALL_THROWN && log.count==1);
    XrXirValue failure={0};CHECK(xr_xir_call_take_result(call,&failure)==XR_XIR_CALL_THROWN);
    CHECK(xr_xir_value_valid(&failure));
    const XrXirTypes *types=xr_xir_compile_type_arena_types(arena);
    const XrXirTypeNode *error=xr_xir_type_node(types,(XrXirType)failure.type);
    CHECK(error && xr_xir_type_is_enum(types,(XrXirType)failure.type) && error->nominal.declaration==0);
    uint32_t variant=UINT32_MAX;
    CHECK(xr_xir_enum_variant(&failure,&variant)==XR_XIR_VALUE_OK && !variant);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && log.count==1);
    CHECK(xr_xir_value_valid(&failure));xr_xir_value_drop(&failure);
    log=(GeneratorCleanupLog){0,303};call=generator_cleanup_new(&config,3);
    CHECK(xr_xir_call_poll_bounded(call,10000).status==XR_XIR_CALL_YIELDED && !log.count);
    XrXirValue unit={0};CHECK(xr_xir_call_generator_take(call,&unit)==XR_XIR_CALL_YIELDED);
    CHECK(!unit.type && !unit.reserved && !unit.payload);
    CHECK(xr_xir_call_poll_bounded(call,10000).status==XR_XIR_CALL_RETURNED && log.count==1);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && log.count==1);
    log=(GeneratorCleanupLog){0,301};XrXirCallAccounting separate[2]={{0}};
    config.accounting=&separate[0];XrXirCall *left=generator_cleanup_new(&config,1);
    config.accounting=&separate[1];XrXirCall *right=generator_cleanup_new(&config,1);
    CHECK(xr_xir_call_poll_bounded(left,10000).status==XR_XIR_CALL_YIELDED);
    CHECK(xr_xir_call_poll_bounded(right,10000).status==XR_XIR_CALL_YIELDED && !log.count);
    CHECK(xr_xir_call_free(left)==XR_XIR_CALL_READY && log.count==1);
    CHECK(xr_xir_call_state(right)==XR_XIR_CALL_YIELDED);
    CHECK(xr_xir_call_free(right)==XR_XIR_CALL_READY && log.count==2);
    for(unsigned i=0;i<2;++i) CHECK(!separate[i].live_bytes && !separate[i].depth && separate[i].allocations==separate[i].frees);
    CHECK(!accounting.live_bytes && !accounting.depth && accounting.allocations==accounting.frees);
}
#endif
