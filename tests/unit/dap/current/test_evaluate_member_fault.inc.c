/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_evaluate_member_fault.inc.c - Field projection allocation rollback and retry
 */
static size_t evaluate_member_fault(const EvaluateDataCase *test,size_t fail) {
    EvaluateFixture fixture=evaluate_member_open();XdapAdapter *adapter=fixture.adapter;XdapLexicalView lexical={0};
    CHECK(xdap_lexical_open(adapter->launch,adapter->snapshot,0,&lexical)==XR_XIR_OK);
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(fixture.ledger,&before)==XR_COMPILE_RESOURCE_OK);
    size_t allocations=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;
    XdapExpressionRequest input={adapter->compiler,&lexical,test->text,strlen(test->text),16384,NULL};XrXirValue value={0};
    runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xdap_expression_evaluate(&input,&value);size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) { CHECK(status==XR_XIR_OK);evaluate_data_result(test,&value,fixture.domain); }
    else {
        if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"Member fault [%s] site=%zu/%zu status=%u\n",test->text,fail,sites,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && !value.type && !value.reserved && !value.payload);
    }
    xr_xir_value_drop(&value);
    CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && !adapter->running && !adapter->value_count && !capture.count);
    CHECK(runtime_live==allocations && runtime_bytes==bytes);
    CHECK(xr_compile_resources_stats(fixture.ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==before.live_bytes && after.work>before.work && after.allocated_bytes>=before.allocated_bytes);
    if (fail!=SIZE_MAX) {
        CHECK(xdap_expression_evaluate(&input,&value)==XR_XIR_OK);evaluate_data_result(test,&value,fixture.domain);xr_xir_value_drop(&value);
        CHECK(runtime_live==allocations && runtime_bytes==bytes && adapter->snapshot==snapshot && adapter->stop_epoch==epoch && !capture.count);
    }
    evaluate_dispose(&fixture);return sites;
}
static void evaluate_member_failures(bool resources) {
    const uint32_t selected[]={0,5,4};
    for (uint32_t i=0;i<sizeof(selected)/sizeof(*selected);++i) {
        const EvaluateDataCase *test=&evaluate_member_cases[selected[i]];size_t sites=evaluate_member_fault(test,SIZE_MAX);CHECK(sites);
        if (resources) for (size_t site=0;site<sites;++site) evaluate_member_fault(test,site);
        else { evaluate_member_fault(test,0);if (sites>1) evaluate_member_fault(test,sites-1); }
        printf("Nominal field [%s] sites=%zu injection=%s same-ledger retry and physical=0/0 PASS\n",
            test->text,sites,resources ? "ALL" : "FIRST_LAST_ONLY");
    }
}
