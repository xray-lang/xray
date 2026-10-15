/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_print_transaction.c - Actual call output rollback at every allocation
 */
#define main original_value_allocation_main
#include "test_xir_value_allocations.c"
#undef main
#include "xir/xxir_format.c"

typedef struct PrintFixture {
    XrXirDomain *domain; XrXirTypeArena *arena; XrXirValue values[3];
    size_t providers, sinks;
    bool cancel;
} PrintFixture;
typedef struct PrintRun { XrXirCallStatus status; size_t sites; uint64_t work, peak; } PrintRun;
static XrXirAction print_resume(XrXirCallView *view) {
    const PrintFixture *fixture=view->environment;
    uint32_t *pc=view->state;
    if (!(*pc)++) {
        if (fixture->cancel) CHECK(xr_xir_call_request_cancel(view->activation)==XR_XIR_CALL_CANCEL_REQUESTED);
        return (XrXirAction){.kind=XR_XIR_ACTION_OUTPUT,.callee=XR_XIR_OUTPUT_LINE,
            .arguments=fixture->values,.argument_count=3};
    }
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,0}};
}
static XrXirOutputStatus print_sink(void *context, XrXirOutputStream stream, const char *bytes, size_t length) {
    PrintFixture *fixture=context;
    static const char expected[]="[1, 2] 9 [3]\n";
    CHECK(stream==XR_XIR_STDOUT && length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    ++fixture->sinks; return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus print_provider(void *context, const XrXirOutputGroup *group) {
    PrintFixture *fixture=context;
    ++fixture->providers;
    CHECK(group->line && group->count==3 && group->values[0].type==XR_XIR_STRING &&
        group->values[1].type==XR_XIR_I64 && group->values[2].type==XR_XIR_STRING);
    XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,print_sink,fixture,512};
    return xr_xir_output_render(&sink,group);
}
static PrintRun print_run(PrintFixture *fixture, size_t fault, uint64_t work,
    uint64_t scratch, uint64_t bytes) {
    size_t baseline=live, before=calls;
    uint64_t physical=xr_xir_domain_stats(fixture->domain).live_bytes;
    fixture->providers=fixture->sinks=0;
    XrXirCallEntry entry={XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_I64,sizeof(uint32_t),
        print_resume,NULL,fixture,0,0};
    XrXirCallAccounting accounting={0}; XrXirCallConfig config;
    CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=&entry; config.entry_count=1; config.accounting=&accounting;
    config.byte_limit=bytes; config.poll_limit=10; config.depth_limit=10;
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,print_provider,fixture};
    config.admission=(XrXirValueAdmission){fixture->arena,fixture->domain,NULL,NULL,work,scratch};
    XrXirCall *call=NULL;
    fail_at=fault==SIZE_MAX ? SIZE_MAX : before+fault;
    XrXirCallStatus status=xr_xir_call_new(&config,0,NULL,0,&call);
    uint64_t consumed=0;
    if (status==XR_XIR_CALL_READY) {
        status=xr_xir_call_poll_bounded(call,UINT64_MAX).status;
        consumed=work-call->config.admission.work;
        CHECK(call->config.admission.scratch_bytes==scratch);
        size_t cleanup=calls;
        CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && calls==cleanup);
    }
    PrintRun result={status,calls-before,consumed,accounting.peak_bytes}; fail_at=SIZE_MAX;
    CHECK(live==baseline && xr_xir_domain_stats(fixture->domain).live_bytes==physical);
    CHECK(!accounting.live_bytes && accounting.allocations==accounting.frees);
    CHECK(atomic_load(&object_pointer(&fixture->values[0])->references)==1 &&
        atomic_load(&object_pointer(&fixture->values[2])->references)==1);
    if (status==XR_XIR_CALL_RETURNED) CHECK(fixture->providers==1 && fixture->sinks==1);
    else CHECK(!fixture->sinks);
    return result;
}
int main(void) {
    PrintFixture fixture={0}; XrXirTypeNode node={.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    XrXirTypes types={&node,1,NULL,NULL};
    CHECK(value_compile_arena(&types,65536,value_compile_limits(65536,0,10000),&fixture.arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(65536,&fixture.domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={fixture.arena,fixture.domain,NULL,NULL,65536,65536};
    XrXirValue numbers[]={{XR_XIR_I64,0,1},{XR_XIR_I64,0,2},{XR_XIR_I64,0,3}};
    CHECK(xr_xir_array_new((XrXirType)256,numbers,2,&admission,&fixture.values[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)256,&numbers[2],1,&admission,&fixture.values[2])==XR_XIR_VALUE_OK);
    fixture.values[1]=(XrXirValue){XR_XIR_I64,0,9};
    PrintRun baseline=print_run(&fixture,SIZE_MAX,65536,65536,65536); CHECK(baseline.status==XR_XIR_CALL_RETURNED && baseline.sites>6);
    for (size_t site=0;site<baseline.sites;++site)
        CHECK(print_run(&fixture,site,65536,65536,65536).status==XR_XIR_CALL_OOM);
    CHECK(print_run(&fixture,SIZE_MAX,baseline.work,65536,65536).status==XR_XIR_CALL_RETURNED);
    CHECK(print_run(&fixture,SIZE_MAX,baseline.work-1,65536,65536).status==XR_XIR_CALL_LIMIT && !fixture.providers);
    CHECK(print_run(&fixture,SIZE_MAX,65536,65536,baseline.peak).status==XR_XIR_CALL_RETURNED);
    CHECK(print_run(&fixture,SIZE_MAX,65536,65536,baseline.peak-1).status==XR_XIR_CALL_LIMIT && !fixture.providers);
    uint64_t low=0,high=65536;
    while (low<high) {
        uint64_t middle=low+(high-low)/2;
        if (print_run(&fixture,SIZE_MAX,65536,middle,65536).status==XR_XIR_CALL_RETURNED) high=middle;
        else low=middle+1;
    }
    CHECK(low && print_run(&fixture,SIZE_MAX,65536,low,65536).status==XR_XIR_CALL_RETURNED);
    CHECK(print_run(&fixture,SIZE_MAX,65536,low-1,65536).status==XR_XIR_CALL_LIMIT && !fixture.providers);
    fixture.cancel=true;
    CHECK(print_run(&fixture,SIZE_MAX,65536,65536,65536).status==XR_XIR_CALL_CANCELLED && !fixture.providers);
    fixture.cancel=false;
    printf("Array print transaction: %zu complete allocation cuts, work=%llu call_peak=%llu scratch=%llu exact/minus-one, zero failed publication and physical rollback passed\n",
        baseline.sites,(unsigned long long)baseline.work,(unsigned long long)baseline.peak,(unsigned long long)low);
    xr_xir_value_drop(&fixture.values[0]); xr_xir_value_drop(&fixture.values[2]);
    xr_xir_domain_drop(fixture.domain); xr_xir_compile_type_arena_drop(fixture.arena);
    CHECK(!live && !value_compile_live && !value_compile_bytes); return 0;
}
