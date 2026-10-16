/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_values.c - Scoped physical values and affine aggregate transfer
 *
 * KEY CONCEPT:
 *   Live binding authority is independent of an otherwise valid physical handle.
 */
#define XR_COMPILE_DOMAIN_ENTRY original_view_values_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY

typedef struct ViewBinding {int64_t payload;XrXirCellAuthority authority;} ViewBinding;
typedef struct ViewScope {bool active,wrong;uint32_t count,calls;ViewBinding bindings[16];} ViewScope;
static void view_bind(ViewScope *scope,const XrXirValue *value,XrXirCellAuthority authority) {
    for (uint32_t i=0;i<scope->count;++i) if (scope->bindings[i].payload==value->payload) {
        scope->bindings[i].authority=authority;return;
    }
    CHECK(scope->count<16);scope->bindings[scope->count++]=(ViewBinding){value->payload,authority};
}
static XrXirValueStatus view_authorize(void *context,const XrXirValue *value,bool writable,
    XrXirValueAdmission *admission,XrXirCellAuthority *output) {
    ViewScope *scope=context;(void)writable;(void)admission;++scope->calls;
    if (!scope->active) return XR_XIR_VALUE_BAD_ARGUMENT;
    for (uint32_t i=0;i<scope->count;++i) if (scope->bindings[i].payload==value->payload) {
        *output=scope->bindings[i].authority;if (scope->wrong) ++output->epoch;return XR_XIR_VALUE_OK;
    }
    return XR_XIR_VALUE_BAD_ARGUMENT;
}
static XrXirValueStatus view_values_fixture(XrCompileResources *resources,XrXirTypeArena **arena,
    XrXirDomain **domain) {
    const XrXirCallableParameter mutable_fields[]={{(XrXirType)259,0},{XR_XIR_I64,0}};
    const XrXirCallableParameter shared_fields[]={{(XrXirType)258,0},{(XrXirType)258,0}};
    XrXirTypeNode nodes[80]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)259},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=mutable_fields,.parameter_count=2},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=shared_fields,.parameter_count=2},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)261},
        {.kind=XR_XIR_TYPE_CALLABLE,.flags=XR_XIR_CALLABLE_ROOT_NONE},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_U8},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_U8}
    };
    XrXirCallableParameter dag_fields[68][2];
    for (uint32_t i=12;i<80;++i) {
        dag_fields[i-12][0]=(XrXirCallableParameter){(XrXirType)(i==12 ? 262 : 256+i-1),0};
        dag_fields[i-12][1]=dag_fields[i-12][0];
        nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=dag_fields[i-12],.parameter_count=2};
    }
    const XrXirTypes types={nodes,80,NULL,NULL};
    XrXirCompileContext c={.resources=resources,.limits=xr_xir_compile_default_limits()};
    XrXirValueStatus status=xr_xir_compile_type_arena_new(&c,&types,arena);if (status!=XR_XIR_VALUE_OK) return status;
    status=xr_xir_compile_domain_new(resources,65536,domain);if (status!=XR_XIR_VALUE_OK) return status;
    const XrXirDomainBudgetControls controls={65536,65536,1000000,65536,65536};
    return xr_xir_domain_budget_bind(*domain,&controls);
}
static XrXirValueStatus view_values_sequence(XrCompileResources *resources) {
    XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;XrXirValue values[12]={{0}},fields[2]={{0}};
    XrXirValueStatus status=view_values_fixture(resources,&arena,&domain);if (status!=XR_XIR_VALUE_OK) goto done;
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
    ViewScope scope={.active=true};XrXirViewAccess access={&admission,view_authorize,&scope};
    const XrXirCellAuthority a={domain,arena,17},b={domain,&scope,18},c={domain,arena,19};
    const XrXirValue items[]={{XR_XIR_I64,0,11},{XR_XIR_I64,0,22},{XR_XIR_I64,0,33},{XR_XIR_I64,0,44}};
    TRY_VALUE(xr_xir_array_new((XrXirType)256,items,4,&admission,&values[0]));
    TRY_VALUE(xr_xir_cell_new(domain,arena,(XrXirType)257,&values[0],&admission,&values[1]));
    XrXirViewStorageSpec spec={a,1,2,XR_XIR_TYPE_MUT_SLICE};XrXirFaultDetail fault={0};
    TRY_VALUE(xr_xir_view_new((XrXirType)259,&values[1],NULL,&spec,&admission,&values[2]));view_bind(&scope,&values[2],a);
    int64_t payload=values[2].payload;
    TRY_VALUE(xr_xir_value_move(&values[2],&values[3]));CHECK(unit_value(&values[2]) && values[3].payload==payload);
    spec=(XrXirViewStorageSpec){b,1,1,XR_XIR_TYPE_MUT_SLICE};
    TRY_VALUE(xr_xir_view_reborrow((XrXirType)259,&values[3],&spec,&access,&values[4]));view_bind(&scope,&values[4],b);
    xr_xir_value_drop(&values[3]);
    TRY_VALUE(xr_xir_view_get(&values[4],0,&access,&values[11],&fault));CHECK(values[11].payload==33);
    xr_xir_value_drop(&values[11]);xr_xir_value_drop(&values[4]);CHECK(xr_xir_cell_unborrowed(&values[1]));
    spec=(XrXirViewStorageSpec){c,1,2,XR_XIR_TYPE_MUT_SLICE};
    TRY_VALUE(xr_xir_view_new((XrXirType)259,&values[1],NULL,&spec,&admission,&values[2]));view_bind(&scope,&values[2],c);
    XrXirValue number={XR_XIR_I64,0,99};
    TRY_VALUE(xr_xir_view_set(&values[2],1,&number,&access,&fault));
    spec=(XrXirViewStorageSpec){c,0,1,XR_XIR_TYPE_SLICE};
    TRY_VALUE(xr_xir_view_reborrow((XrXirType)258,&values[2],&spec,&access,&values[3]));view_bind(&scope,&values[3],c);
    TRY_VALUE(xr_xir_value_copy(&values[3],&values[4]));
    TRY_VALUE(xr_xir_value_move(&values[2],&fields[0]));fields[1]=(XrXirValue){XR_XIR_I64,0,55};
    XrXirValue before=fields[0];
    status=xr_xir_borrowed_tuple_new_move((XrXirType)261,fields,2,&access,&values[5]);
    if (status!=XR_XIR_VALUE_OK) {CHECK(fields[0].payload==before.payload && fields[1].payload==55 && unit_value(&values[5]));goto done;}
    CHECK(unit_value(&fields[0]) && unit_value(&fields[1]));before=values[5];
    status=xr_xir_borrowed_some_new_move((XrXirType)263,&values[5],&access,&values[6]);
    if (status!=XR_XIR_VALUE_OK) {CHECK(values[5].payload==before.payload && unit_value(&values[6]));goto done;}
    CHECK(unit_value(&values[5]));
    TRY_VALUE(xr_xir_borrowed_some_take(&values[6],&access,&values[5]));CHECK(unit_value(&values[6]));
    TRY_VALUE(xr_xir_borrowed_tuple_unpack_move(&values[5],fields,2,&access));
    CHECK(unit_value(&values[5]) && fields[0].type==259 && fields[1].payload==55);
    xr_xir_value_drop(&fields[0]);xr_xir_value_drop(&fields[1]);xr_xir_value_drop(&values[3]);
    TRY_VALUE(xr_xir_view_get(&values[4],0,&access,&values[11],&fault));CHECK(values[11].payload==22);xr_xir_value_drop(&values[11]);
    TRY_VALUE(xr_xir_view_copy((XrXirType)256,&values[4],&access,&values[7]));
    xr_xir_value_drop(&values[4]);CHECK(xr_xir_cell_unborrowed(&values[1]));
    TRY_VALUE(xr_xir_nullable_new((XrXirType)260,NULL,&admission,&values[8]));
    TRY_VALUE(xr_xir_borrowed_value_admit(&values[8],(XrXirType)260,&access));
    TRY_VALUE(xr_xir_string_new(domain,"a\0\xc3\xa9",4,&values[9]));
    TRY_VALUE(xr_xir_cell_new(domain,arena,(XrXirType)266,&values[9],&admission,&values[10]));
    spec=(XrXirViewStorageSpec){a,1,3,XR_XIR_TYPE_SLICE};
    TRY_VALUE(xr_xir_view_new((XrXirType)267,&values[10],NULL,&spec,&admission,&values[3]));view_bind(&scope,&values[3],a);
    TRY_VALUE(xr_xir_view_copy((XrXirType)265,&values[3],&access,&values[4]));
    xr_xir_value_drop(&values[3]);xr_xir_value_drop(&values[4]);
    spec=(XrXirViewStorageSpec){a,0,4,XR_XIR_TYPE_SLICE};
    TRY_VALUE(xr_xir_view_new((XrXirType)258,&values[1],NULL,&spec,&admission,&values[3]));view_bind(&scope,&values[3],a);
    XrXirType dag_type=(XrXirType)258;
    for (uint32_t layer=0;layer<69;++layer) {
        TRY_VALUE(xr_xir_value_copy(&values[3],&fields[0]));TRY_VALUE(xr_xir_value_copy(&values[3],&fields[1]));
        dag_type=(XrXirType)(layer ? 256+11+layer : 262);
        TRY_VALUE(xr_xir_borrowed_tuple_new_move(dag_type,fields,2,&access,&values[4]));
        xr_xir_value_drop(&values[3]);TRY_VALUE(xr_xir_value_move(&values[4],&values[3]));
    }
    uint32_t before_calls=scope.calls;uint64_t work=xr_xir_domain_budget_stats(domain).work;
    TRY_VALUE(xr_xir_borrowed_value_admit(&values[3],dag_type,&access));
    CHECK(scope.calls==before_calls+1 && xr_xir_domain_budget_stats(domain).work-work<20000);
done:
    for (unsigned i=2;i;--i) xr_xir_value_drop(&fields[i-1]);
    for (unsigned i=12;i;--i) xr_xir_value_drop(&values[i-1]);
    xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);CHECK(!live);return status;
}
static unsigned view_native_calls;
static XrXirAction view_native(XrXirCallView *view) {
    (void)view;++view_native_calls;return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,1}};
}
static void view_values_guards(XrCompileResources *resources) {
    XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;CHECK(view_values_fixture(resources,&arena,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
    const XrXirCellAuthority a={domain,arena,29};ViewScope scope={.active=true};XrXirViewAccess access={&admission,view_authorize,&scope};
    XrXirValue array={0},cell={0},value={0},copy={0},some={0};XrXirFaultDetail fault={0};
    XrXirValue item={XR_XIR_I64,0,7};
    CHECK(xr_xir_array_new((XrXirType)256,&item,1,&admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)257,&array,&admission,&cell)==XR_XIR_VALUE_OK);
    XrXirViewStorageSpec spec={a,0,1,XR_XIR_TYPE_MUT_SLICE};
    CHECK(xr_xir_view_new((XrXirType)259,&cell,NULL,&spec,&admission,&value)==XR_XIR_VALUE_OK);view_bind(&scope,&value,a);
    CHECK(xr_xir_value_copy(&value,&copy)==XR_XIR_VALUE_BAD_ARGUMENT && unit_value(&copy));
    CHECK(xr_xir_value_admit(&value,(XrXirType)259,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_view_get(&value,0,NULL,&copy,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && unit_value(&copy));
    scope.active=false;CHECK(xr_xir_value_valid(&value));
    CHECK(xr_xir_view_get(&value,0,&access,&copy,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && unit_value(&copy));
    scope.active=true;scope.wrong=true;
    CHECK(xr_xir_view_get(&value,0,&access,&copy,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && unit_value(&copy));scope.wrong=false;
    CHECK(xr_xir_borrowed_some_new_move((XrXirType)260,&value,&access,&some)==XR_XIR_VALUE_OK && unit_value(&value));
    CHECK(xr_xir_value_copy(&some,&copy)==XR_XIR_VALUE_BAD_ARGUMENT && unit_value(&copy));
    XrXirCallConfig config={0};XrXirCallAccounting accounting={0};XrXirCall *call=NULL;
    const XrXirCallEntry entry={.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_I64,.resume=view_native};
    CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=&entry;config.entry_count=1;config.accounting=&accounting;config.admission=admission;
    CHECK(xr_xir_call_new(&config,0,NULL,0,&call)==XR_XIR_CALL_BAD_ARGUMENT && !call && !view_native_calls);
    xr_xir_value_drop(&some);xr_xir_value_drop(&cell);xr_xir_value_drop(&array);xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    const unsigned char retired25[8]={0x58,0x52,0x44,0x4d,0x19,0,0,1};
    CHECK(!xr_xir_domain_current((const XrXirDomain *)retired25));
}
int main(void) {
    XrCompileResourceLimits limits=compile_domain_limits();XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);size_t first=value_compile_calls;
    CHECK(view_values_sequence(resources)==XR_XIR_VALUE_OK);size_t sites=value_compile_calls-first;
    XrCompileResourceStats measured=compile_domain_stats(resources);CHECK(measured.live_bytes==sizeof(*resources));
    xr_compile_resources_release(resources);CHECK(!value_compile_live && !value_compile_bytes && sites>20 && sites<128);
    for (size_t at=0;at<sites;++at) {
        resources=NULL;CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        value_compile_injected=false;value_compile_fail_at=value_compile_calls+at;
        CHECK(view_values_sequence(resources)==XR_XIR_VALUE_OOM && value_compile_injected);value_compile_fail_at=SIZE_MAX;
        CHECK(compile_domain_stats(resources).live_bytes==sizeof(*resources));CHECK(view_values_sequence(resources)==XR_XIR_VALUE_OK);
        xr_compile_resources_release(resources);CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits cap=limits;
        if (!axis) cap.allocated_bytes=measured.allocated_bytes-minus;else if (axis==1) cap.live_bytes=measured.peak_bytes-minus;
        else cap.work=measured.work-minus;
        resources=NULL;CHECK(xr_compile_resources_new(&cap,&resources)==XR_COMPILE_RESOURCE_OK);
        XrXirValueStatus actual=view_values_sequence(resources);
        if (actual!=(minus?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_OK)) fprintf(stderr,"view axis%u minus%u status%u work%llu expected%llu\n",
            axis,minus,(unsigned)actual,(unsigned long long)compile_domain_stats(resources).work,(unsigned long long)measured.work);
        CHECK(actual==(minus?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_OK));
        CHECK(compile_domain_stats(resources).live_bytes==sizeof(*resources));xr_compile_resources_release(resources);
        CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    resources=NULL;CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);view_values_guards(resources);
    CHECK(compile_domain_stats(resources).live_bytes==sizeof(*resources));xr_compile_resources_release(resources);
    CHECK(!value_compile_live && !value_compile_bytes && !live && !calls);
    printf("View Value26: scope authority, affine move/aggregate, child pins, %zu allocation failures, exact axes and physical0 PASS\n",sites);return 0;
}
