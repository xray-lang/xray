/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_origin.c - Deep physical input ancestry and complete failures
 *
 * KEY CONCEPT:
 *   Returned leaves share proven prefixes without keeping logical parents live.
 */
#define XR_VIEW_VALUES_ENTRY original_view_origin_values_main
#include "test_xir_p09_view_values.c"
#undef XR_VIEW_VALUES_ENTRY

static XrXirValueStatus view_origin_deep_sequence(XrCompileResources *resources) {
    XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    XrXirValue values[5]={{0}},chain[80]={{0}},fields[2]={{0}};XrXirViewOriginPin input={0};
    XrXirValueStatus status=view_values_fixture(resources,&arena,&domain);if (status!=XR_XIR_VALUE_OK) goto done;
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
    ViewScope scope={.active=true};XrXirViewAccess access={&admission,view_authorize,&scope};
    const XrXirValue item={XR_XIR_I64,0,41};
    TRY_VALUE(xr_xir_array_new((XrXirType)256,&item,1,&admission,&values[0]));
    TRY_VALUE(xr_xir_cell_new(domain,arena,(XrXirType)257,&values[0],&admission,&values[1]));
    for (uint32_t i=0;i<80;++i) {
        XrXirViewStorageSpec spec={{domain,&scope,100+i},0,1,XR_XIR_TYPE_MUT_SLICE};
        if (!i) TRY_VALUE(xr_xir_view_new((XrXirType)259,&values[1],NULL,&spec,&admission,&chain[i]));
        else TRY_VALUE(xr_xir_view_reborrow((XrXirType)259,&chain[i-1],&spec,&access,&chain[i]));
        view_bind(&scope,&chain[i],spec.authority);
        if (!i) TRY_VALUE(xr_xir_borrowed_value_origin_pin(&chain[i],(XrXirType)259,&access,&input));
    }
    XrXirViewStorageSpec spec={{domain,&scope,179},0,1,XR_XIR_TYPE_SLICE};
    TRY_VALUE(xr_xir_view_reborrow((XrXirType)258,&chain[79],&spec,&access,&values[2]));view_bind(&scope,&values[2],spec.authority);
    spec.authority.epoch=180;
    TRY_VALUE(xr_xir_view_reborrow((XrXirType)258,&values[2],&spec,&access,&values[3]));view_bind(&scope,&values[3],spec.authority);
    for (unsigned i=0;i<79;i+=2) xr_xir_value_drop(&chain[i]);
    for (unsigned i=1;i<80;i+=2) xr_xir_value_drop(&chain[i]);
    TRY_VALUE(xr_xir_value_copy(&values[2],&fields[0]));TRY_VALUE(xr_xir_value_copy(&values[3],&fields[1]));
    TRY_VALUE(xr_xir_borrowed_tuple_new_move((XrXirType)262,fields,2,&access,&values[4]));
    XrXirValue before=values[4];void *identity=input.body;uint64_t work[2],requests[2];
    uint64_t scratch=admission.scratch_bytes;
    for (unsigned repeat=0;repeat<2;++repeat) {
        uint64_t prior_work=xr_xir_domain_budget_stats(domain).work;
        uint64_t prior_requests=xr_xir_domain_budget_stats(domain).requested_bytes;
        uint32_t prior_calls=scope.calls;
        TRY_VALUE(xr_xir_borrowed_value_origin_match(&values[4],(XrXirType)262,&input,&access));
        work[repeat]=xr_xir_domain_budget_stats(domain).work-prior_work;
        requests[repeat]=xr_xir_domain_budget_stats(domain).requested_bytes-prior_requests;
        CHECK(scope.calls==prior_calls+2 && admission.scratch_bytes==scratch &&
            values[4].payload==before.payload && input.body==identity && xr_xir_value_valid(&values[4]));
    }
    CHECK(work[0]==work[1] && requests[0]==requests[1] && requests[0]>0 && work[0]<50000);
done:
    for (unsigned i=2;i;--i) xr_xir_value_drop(&fields[i-1]);
    for (unsigned i=5;i;--i) xr_xir_value_drop(&values[i-1]);
    for (unsigned i=80;i;--i) xr_xir_value_drop(&chain[i-1]);
    xr_xir_view_origin_unpin(&input);xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    CHECK(!live);return status;
}
static XrXirValueStatus view_origin_dag_sequence(XrCompileResources *resources) {
    XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;XrXirValue values[4]={{0}},fields[2]={{0}};
    XrXirViewOriginPin input={0};XrXirValueStatus status=view_values_fixture(resources,&arena,&domain);
    if (status!=XR_XIR_VALUE_OK) goto done;
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
    ViewScope scope={.active=true};XrXirViewAccess access={&admission,view_authorize,&scope};
    const XrXirValue item={XR_XIR_I64,0,41};
    TRY_VALUE(xr_xir_array_new((XrXirType)256,&item,1,&admission,&values[0]));
    TRY_VALUE(xr_xir_cell_new(domain,arena,(XrXirType)257,&values[0],&admission,&values[1]));
    XrXirViewStorageSpec spec={{domain,&scope,301},0,1,XR_XIR_TYPE_SLICE};
    TRY_VALUE(xr_xir_view_new((XrXirType)258,&values[1],NULL,&spec,&admission,&values[2]));view_bind(&scope,&values[2],spec.authority);
    TRY_VALUE(xr_xir_borrowed_value_origin_pin(&values[2],(XrXirType)258,&access,&input));spec.authority.epoch=302;
    TRY_VALUE(xr_xir_view_reborrow((XrXirType)258,&values[2],&spec,&access,&values[3]));view_bind(&scope,&values[3],spec.authority);
    xr_xir_value_drop(&values[2]);TRY_VALUE(xr_xir_value_move(&values[3],&values[2]));
    XrXirViewOriginInfo info={0};TRY_VALUE(xr_xir_view_origin_info(&input,&admission,&info));CHECK(!info.logical_active);
    XrXirType type=(XrXirType)258;
    for (uint32_t layer=0;layer<69;++layer) {
        TRY_VALUE(xr_xir_value_copy(&values[2],&fields[0]));TRY_VALUE(xr_xir_value_copy(&values[2],&fields[1]));
        type=(XrXirType)(layer ? 256+11+layer : 262);
        TRY_VALUE(xr_xir_borrowed_tuple_new_move(type,fields,2,&access,&values[3]));
        xr_xir_value_drop(&values[2]);TRY_VALUE(xr_xir_value_move(&values[3],&values[2]));
    }
    uint64_t work[2],requests[2];XrXirValue before=values[2];uint64_t scratch=admission.scratch_bytes;
    for (unsigned repeat=0;repeat<2;++repeat) {
        uint64_t prior_work=xr_xir_domain_budget_stats(domain).work;
        uint64_t prior_requests=xr_xir_domain_budget_stats(domain).requested_bytes;uint32_t prior_calls=scope.calls;
        TRY_VALUE(xr_xir_borrowed_value_origin_match(&values[2],type,&input,&access));
        work[repeat]=xr_xir_domain_budget_stats(domain).work-prior_work;
        requests[repeat]=xr_xir_domain_budget_stats(domain).requested_bytes-prior_requests;
        CHECK(scope.calls==prior_calls+1 && admission.scratch_bytes==scratch && values[2].payload==before.payload);
    }
    CHECK(work[0]==work[1] && requests[0]==requests[1] && requests[0]>0 && work[0]<20000);
done:
    for (unsigned i=2;i;--i) xr_xir_value_drop(&fields[i-1]);
    for (unsigned i=4;i;--i) xr_xir_value_drop(&values[i-1]);
    xr_xir_view_origin_unpin(&input);xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    CHECK(!live);return status;
}
static XrXirValueStatus view_origin_sequence(XrCompileResources *resources) {
    XrXirValueStatus status=view_origin_deep_sequence(resources);
    return status==XR_XIR_VALUE_OK ? view_origin_dag_sequence(resources) : status;
}
int main(void) {
    XrCompileResourceLimits limits=compile_domain_limits();XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);size_t first=value_compile_calls;
    CHECK(view_origin_sequence(resources)==XR_XIR_VALUE_OK);size_t sites=value_compile_calls-first;
    XrCompileResourceStats measured=compile_domain_stats(resources);CHECK(measured.live_bytes==sizeof(*resources));
    xr_compile_resources_release(resources);resources=NULL;CHECK(!value_compile_live && !value_compile_bytes && sites>80 && sites<256);
    for (size_t at=0;at<sites;++at) {
        CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        value_compile_injected=false;value_compile_fail_at=value_compile_calls+at;
        CHECK(view_origin_sequence(resources)==XR_XIR_VALUE_OOM && value_compile_injected);value_compile_fail_at=SIZE_MAX;
        CHECK(compile_domain_stats(resources).live_bytes==sizeof(*resources));CHECK(view_origin_sequence(resources)==XR_XIR_VALUE_OK);
        xr_compile_resources_release(resources);resources=NULL;CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits cap=limits;
        if (!axis) cap.allocated_bytes=measured.allocated_bytes-minus;
        else if (axis==1) cap.live_bytes=measured.peak_bytes-minus;else cap.work=measured.work-minus;
        CHECK(xr_compile_resources_new(&cap,&resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(view_origin_sequence(resources)==(minus?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_OK));
        CHECK(compile_domain_stats(resources).live_bytes==sizeof(*resources));xr_compile_resources_release(resources);resources=NULL;
        CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    CHECK(!calls);printf("View origin: 80 retired ancestors, two distinct leaves, 2^69 logical leaves, %zu complete allocation failures, repeat axes and physical0 PASS\n",sites);
    return 0;
}
