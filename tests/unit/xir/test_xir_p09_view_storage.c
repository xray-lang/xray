/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_storage.c - Actual ranges, root loans and private COW publication
 *
 * KEY CONCEPT:
 *   Every allocation failure retires the real loan before its last root pin.
 */
#define XR_COMPILE_DOMAIN_ENTRY original_view_storage_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY

static XrXirValueStatus view_storage_fixture(XrCompileResources *resources,
    XrXirTypeArena **arena,XrXirDomain **domain) {
    const XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)258},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_U8},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64}
    };
    const XrXirTypes types={nodes,7,NULL,NULL};
    XrXirCompileContext c={.resources=resources,.limits=xr_xir_compile_default_limits()};
    XrXirValueStatus status=xr_xir_compile_type_arena_new(&c,&types,arena);
    if (status!=XR_XIR_VALUE_OK) return status;
    status=xr_xir_compile_domain_new(resources,65536,domain);
    if (status!=XR_XIR_VALUE_OK) return status;
    const XrXirDomainBudgetControls controls={65536,65536,1000000,65536,65536};
    return xr_xir_domain_budget_bind(*domain,&controls);
}
static XrXirValueStatus view_storage_sequence(XrCompileResources *resources) {
    XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL,*foreign=NULL;XrXirValue values[12]={{0}};
    XrXirViewStorage views[5]={{0}};XrXirValueStatus status=view_storage_fixture(resources,&arena,&domain);
    if (status!=XR_XIR_VALUE_OK) goto done;
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
    const XrXirCellAuthority authority={domain,arena,7},child={domain,views,8};
    const XrXirValue integers[]={{XR_XIR_I64,0,11},{XR_XIR_I64,0,22},{XR_XIR_I64,0,33},{XR_XIR_I64,0,44}};
    XrXirViewStorageSpec spec={authority,1,2,XR_XIR_TYPE_MUT_SLICE};XrXirFaultDetail fault={0};
    TRY_VALUE(xr_xir_array_new((XrXirType)256,integers,4,&admission,&values[0]));
    TRY_VALUE(xr_xir_cell_new(domain,arena,(XrXirType)257,&values[0],&admission,&values[1]));
    XirCell *cell=(XirCell *)object_pointer(&values[1]);int64_t original=cell->value.payload;
    status=xr_xir_view_storage_begin(&values[1],NULL,&spec,&admission,&views[0]);
    if (status!=XR_XIR_VALUE_OK) {
        CHECK(cell->value.payload==original && xr_xir_cell_unborrowed(&values[1]) && view_storage_empty(&views[0]));goto done;
    }
    CHECK(cell->value.payload!=original && atomic_load(&object_pointer(&cell->value)->references)==1);
    int64_t length=-1;TRY_VALUE(xr_xir_view_storage_len(&views[0],&authority,&admission,&length));CHECK(length==2);
    TRY_VALUE(xr_xir_view_storage_get(&views[0],&authority,0,&admission,&values[11],&fault));
    CHECK(values[11].type==XR_XIR_I64 && values[11].payload==22);xr_xir_value_drop(&values[11]);
    XrXirValue number={XR_XIR_I64,0,99};
    TRY_VALUE(xr_xir_view_storage_set(&views[0],&authority,1,&number,&admission,&fault));
    spec=(XrXirViewStorageSpec){authority,0,2,XR_XIR_TYPE_SLICE};
    TRY_VALUE(xr_xir_view_storage_reborrow(&views[0],&authority,&spec,&admission,&views[1]));
    TRY_VALUE(xr_xir_view_storage_reborrow(&views[0],&authority,&spec,&admission,&views[2]));
    TRY_VALUE(xr_xir_view_storage_get(&views[1],&authority,1,&admission,&values[11],&fault));
    CHECK(values[11].payload==99);xr_xir_value_drop(&values[11]);
    xr_xir_view_storage_end(&views[1]);xr_xir_view_storage_end(&views[2]);
    spec=(XrXirViewStorageSpec){child,1,1,XR_XIR_TYPE_MUT_SLICE};
    TRY_VALUE(xr_xir_view_storage_reborrow(&views[0],&authority,&spec,&admission,&views[3]));
    TRY_VALUE(xr_xir_view_storage_get(&views[3],&child,0,&admission,&values[11],&fault));
    CHECK(values[11].payload==99);xr_xir_value_drop(&values[11]);xr_xir_view_storage_end(&views[3]);
    TRY_VALUE(xr_xir_view_storage_copy((XrXirType)256,&views[0],&authority,&admission,&values[2]));
    xr_xir_view_storage_end(&views[0]);
    TRY_VALUE(xr_xir_array_get(&values[0],2,&admission,&values[11],&fault));
    CHECK(values[11].payload==33);xr_xir_value_drop(&values[11]);
    TRY_VALUE(xr_xir_string_new(domain,"a\xc3\xa9z",4,&values[3]));
    TRY_VALUE(xr_xir_cell_new(domain,arena,(XrXirType)260,&values[3],&admission,&values[4]));
    spec=(XrXirViewStorageSpec){authority,1,2,XR_XIR_TYPE_SLICE};
    TRY_VALUE(xr_xir_view_storage_begin(&values[4],NULL,&spec,&admission,&views[4]));
    TRY_VALUE(xr_xir_view_storage_get(&views[4],&authority,1,&admission,&values[11],&fault));
    CHECK(values[11].type==XR_XIR_U8 && values[11].payload==0xa9);xr_xir_value_drop(&values[11]);
    TRY_VALUE(xr_xir_view_storage_copy((XrXirType)261,&views[4],&authority,&admission,&values[5]));
    xr_xir_view_storage_end(&views[4]);
    const char owned_text[]="owned-view-element-with-independent-storage";
    TRY_VALUE(xr_xir_string_new(domain,owned_text,sizeof(owned_text)-1,&values[6]));
    TRY_VALUE(xr_xir_array_new((XrXirType)258,&values[6],1,&admission,&values[8]));
    TRY_VALUE(xr_xir_cell_new(domain,arena,(XrXirType)259,&values[8],&admission,&values[9]));
    spec=(XrXirViewStorageSpec){authority,0,1,XR_XIR_TYPE_MUT_SLICE};
    TRY_VALUE(xr_xir_view_storage_begin(&values[9],NULL,&spec,&admission,&views[0]));
    xr_xir_value_drop(&values[9]);
    TRY_VALUE(xr_xir_view_storage_copy((XrXirType)258,&views[0],&authority,&admission,&values[10]));
    TRY_VALUE(xr_xir_array_get(&values[10],0,&admission,&values[11],&fault));
    CHECK(values[11].payload==values[6].payload);xr_xir_value_drop(&values[11]);
    xr_xir_view_storage_end(&views[0]);
    TRY_VALUE(xr_xir_compile_domain_new(resources,65536,&foreign));
    const XrXirDomainBudgetControls controls={65536,65536,1000000,65536,65536};
    TRY_VALUE(xr_xir_domain_budget_bind(foreign,&controls));
    XrXirValueAdmission foreign_admission={arena,foreign,NULL,NULL,1000000,65536};
    TRY_VALUE(xr_xir_array_new((XrXirType)256,integers,2,&foreign_admission,&values[7]));
    TRY_VALUE(xr_xir_cell_new(domain,arena,(XrXirType)257,&values[7],&admission,&values[9]));
    xr_xir_value_drop(&values[7]);xr_xir_domain_drop(foreign);foreign=NULL;
    cell=(XirCell *)object_pointer(&values[9]);original=cell->value.payload;
    CHECK(object_pointer(&cell->value)->domain!=domain && atomic_load(&object_pointer(&cell->value)->references)==1);
    spec=(XrXirViewStorageSpec){authority,0,2,XR_XIR_TYPE_MUT_SLICE};
    status=xr_xir_view_storage_begin(&values[9],NULL,&spec,&admission,&views[0]);
    if (status!=XR_XIR_VALUE_OK) {
        CHECK(cell->value.payload==original && xr_xir_cell_unborrowed(&values[9]) && view_storage_empty(&views[0]));goto done;
    }
    CHECK(cell->value.payload!=original && object_pointer(&cell->value)->domain==domain);
done:
    for (unsigned i=5;i;--i) xr_xir_view_storage_end(&views[i-1]);
    for (unsigned i=12;i;--i) xr_xir_value_drop(&values[i-1]);
    xr_xir_domain_drop(foreign);xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);CHECK(!live);
    return status;
}
static void view_storage_guards(XrCompileResources *resources) {
    XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(view_storage_fixture(resources,&arena,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
    const XrXirCellAuthority a={domain,arena,31},b={domain,&admission,32},wrong={arena,domain,31};
    XrXirValue array={0},cell={0},result={0},number={XR_XIR_I64,0,5};
    const XrXirValue numbers[]={{XR_XIR_I64,0,5},{XR_XIR_I64,0,6},{XR_XIR_I64,0,7}};
    CHECK(xr_xir_array_new((XrXirType)256,numbers,3,&admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)257,&array,&admission,&cell)==XR_XIR_VALUE_OK);
    XrXirViewStorage view={0},shared[2]={{0}},nested={0};XrXirFaultDetail fault={0};
    XrXirViewStorageSpec spec={a,1,1,XR_XIR_TYPE_MUT_SLICE};
    CHECK(xr_xir_view_storage_begin(&cell,NULL,&spec,&admission,&view)==XR_XIR_VALUE_OK);
    int64_t length=77;
    CHECK(xr_xir_view_storage_len(&view,&wrong,&admission,&length)==XR_XIR_VALUE_BAD_ARGUMENT && length==77);
    CHECK(xr_xir_cell_read(&cell,&result)==XR_XIR_VALUE_BAD_ARGUMENT && unit_value(&result));
    CHECK(xr_xir_view_storage_begin(&cell,NULL,&spec,&admission,&view)==XR_XIR_VALUE_BAD_ARGUMENT);
    XrXirViewStorage forged=view;
    CHECK(xr_xir_view_storage_get(&forged,&a,0,&admission,&result,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    forged=(XrXirViewStorage){0};
    CHECK(xr_xir_view_storage_get(&view,&a,1,&admission,&result,&fault)==XR_XIR_VALUE_BOUNDS);
    CHECK(fault.code==430 && fault.index==1 && fault.length==1 && unit_value(&result));
    spec=(XrXirViewStorageSpec){b,0,2,XR_XIR_TYPE_MUT_SLICE};
    CHECK(xr_xir_view_storage_reborrow(&view,&a,&spec,&admission,&nested)==XR_XIR_VALUE_BAD_ARGUMENT && view_storage_empty(&nested));
    spec=(XrXirViewStorageSpec){a,0,1,XR_XIR_TYPE_SLICE};
    for (unsigned i=0;i<2;++i)
        CHECK(xr_xir_view_storage_reborrow(&view,&a,&spec,&admission,&shared[i])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_view_storage_set(&view,&a,0,&number,&admission,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_view_storage_set(&shared[0],&a,0,&number,&admission,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    spec.kind=XR_XIR_TYPE_MUT_SLICE;spec.authority=b;
    CHECK(xr_xir_view_storage_reborrow(&shared[0],&a,&spec,&admission,&nested)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_view_storage_end(&shared[0]);
    CHECK(xr_xir_view_storage_set(&view,&a,0,&number,&admission,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_view_storage_end(&shared[1]);
    spec.length=1;
    CHECK(xr_xir_cell_authorized_read(&cell,&a,&result)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_view_storage_set(&view,&a,0,&number,&admission,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_view_storage_reborrow(&view,&a,&spec,&admission,&nested)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_value_drop(&result);
    CHECK(xr_xir_view_storage_reborrow(&view,&a,&spec,&admission,&nested)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_view_storage_get(&view,&a,0,&admission,&result,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_view_storage_get(&nested,&wrong,0,&admission,&result,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_view_storage_end(&nested);
    CHECK(xr_xir_view_storage_set(&view,&a,0,&number,&admission,&fault)==XR_XIR_VALUE_OK);
    size_t before=value_compile_calls;closed_fail_all=true;value_compile_fail_at=before;
    xr_xir_view_storage_end(&view);closed_fail_all=false;value_compile_fail_at=SIZE_MAX;
    CHECK(value_compile_calls==before && xr_xir_cell_unborrowed(&cell));
    spec=(XrXirViewStorageSpec){a,INT64_MAX,INT64_MAX,XR_XIR_TYPE_SLICE};
    uint64_t work=xr_xir_domain_budget_stats(domain).work;
    CHECK(xr_xir_view_storage_begin(&cell,NULL,&spec,&admission,&view)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_domain_budget_stats(domain).work==work && view_storage_empty(&view));
    spec=(XrXirViewStorageSpec){a,0,1,XR_XIR_TYPE_SLICE};
    admission.work=27;
    CHECK(xr_xir_view_storage_begin(&cell,NULL,&spec,&admission,&view)==XR_XIR_VALUE_LIMIT && view_storage_empty(&view));
    admission.work=28;work=xr_xir_domain_budget_stats(domain).work;
    CHECK(xr_xir_view_storage_begin(&cell,NULL,&spec,&admission,&view)==XR_XIR_VALUE_OK && !admission.work);
    CHECK(xr_xir_domain_budget_stats(domain).work==work+29);xr_xir_view_storage_end(&view);
    uint64_t saved_limit=domain->budget.work_limit;work=xr_xir_domain_budget_stats(domain).work;
    admission.work=28;domain->budget.work_limit=work+28;
    CHECK(xr_xir_view_storage_begin(&cell,NULL,&spec,&admission,&view)==XR_XIR_VALUE_LIMIT && view_storage_empty(&view));
    work=xr_xir_domain_budget_stats(domain).work;domain->budget.work_limit=work+29;admission.work=28;
    CHECK(xr_xir_view_storage_begin(&cell,NULL,&spec,&admission,&view)==XR_XIR_VALUE_OK);
    xr_xir_view_storage_end(&view);domain->budget.work_limit=saved_limit;admission.work=1000000;
    XrXirCellLoan raw={0};
    CHECK(xr_xir_cell_loan_prepare(&cell,NULL,&raw)==XR_XIR_VALUE_OK);xr_xir_cell_loan_commit(&cell,&raw,&a);
    CHECK(xr_xir_view_storage_begin(&cell,&a,&spec,&admission,&view)==XR_XIR_VALUE_OK);
    xr_xir_cell_loan_release(&raw);
    CHECK(xr_xir_view_storage_get(&view,&a,0,&admission,&result,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_view_storage_end(&view);
    CHECK(xr_xir_view_storage_begin(&cell,NULL,&spec,&admission,&view)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&cell);xr_xir_value_drop(&array);xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    before=value_compile_calls;closed_fail_all=true;value_compile_fail_at=before;
    xr_xir_view_storage_end(&view);closed_fail_all=false;value_compile_fail_at=SIZE_MAX;
    CHECK(value_compile_calls==before);
}
static void view_storage_string_guards(XrCompileResources *resources) {
    XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(view_storage_fixture(resources,&arena,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
    const XrXirCellAuthority a={domain,arena,91};
    XrXirValue text={0},cell={0},value={0},copy={0};XrXirViewStorage view={0};XrXirFaultDetail fault={0};
    CHECK(xr_xir_string_new(domain,"a\0\xc3\xa9",4,&text)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)260,&text,&admission,&cell)==XR_XIR_VALUE_OK);
    XrXirViewStorageSpec spec={a,0,4,XR_XIR_TYPE_MUT_SLICE};
    CHECK(xr_xir_view_storage_begin(&cell,NULL,&spec,&admission,&view)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(view_storage_empty(&view) && xr_xir_cell_unborrowed(&cell));
    spec=(XrXirViewStorageSpec){a,4,0,XR_XIR_TYPE_SLICE};
    CHECK(xr_xir_view_storage_begin(&cell,NULL,&spec,&admission,&view)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_view_storage_get(&view,&a,0,&admission,&value,&fault)==XR_XIR_VALUE_BOUNDS);
    CHECK(fault.code==430 && !fault.index && !fault.length && unit_value(&value));
    CHECK(xr_xir_view_storage_copy((XrXirType)261,&view,&a,&admission,&copy)==XR_XIR_VALUE_OK);
    xr_xir_view_storage_end(&view);spec=(XrXirViewStorageSpec){a,0,4,XR_XIR_TYPE_SLICE};
    CHECK(xr_xir_view_storage_begin(&cell,NULL,&spec,&admission,&view)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_view_storage_get(&view,&a,1,&admission,&value,&fault)==XR_XIR_VALUE_OK);
    CHECK(value.type==XR_XIR_U8 && !value.payload);xr_xir_value_drop(&value);
    CHECK(xr_xir_view_storage_copy((XrXirType)256,&view,&a,&admission,&value)==XR_XIR_VALUE_BAD_ARGUMENT && unit_value(&value));
    XrXirValuePlace place={0};
    CHECK(xr_xir_cell_authorized_place(&cell,&a,&admission,&place)==XR_XIR_VALUE_BAD_ARGUMENT && !place.payload);
    xr_xir_view_storage_end(&view);xr_xir_value_drop(&cell);xr_xir_value_drop(&text);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);xr_xir_value_drop(&copy);
}
int main(void) {
    XrCompileResourceLimits limits=compile_domain_limits();XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    size_t first=value_compile_calls;CHECK(view_storage_sequence(resources)==XR_XIR_VALUE_OK);
    size_t sites=value_compile_calls-first;XrCompileResourceStats measured=compile_domain_stats(resources);
    CHECK(measured.live_bytes==sizeof(*resources) && sites>10 && sites<128);
    xr_compile_resources_release(resources);CHECK(!value_compile_live && !value_compile_bytes);
    for (size_t at=0;at<sites;++at) {
        resources=NULL;CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        value_compile_injected=false;value_compile_fail_at=value_compile_calls+at;
        CHECK(view_storage_sequence(resources)==XR_XIR_VALUE_OOM && value_compile_injected);value_compile_fail_at=SIZE_MAX;
        CHECK(compile_domain_stats(resources).live_bytes==sizeof(*resources));
        CHECK(view_storage_sequence(resources)==XR_XIR_VALUE_OK);
        xr_compile_resources_release(resources);CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits cap=limits;
        if (!axis) cap.allocated_bytes=measured.allocated_bytes-minus;
        else if (axis==1) cap.live_bytes=measured.peak_bytes-minus;
        else cap.work=measured.work-minus;
        resources=NULL;CHECK(xr_compile_resources_new(&cap,&resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(view_storage_sequence(resources)==(minus?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_OK));
        CHECK(compile_domain_stats(resources).live_bytes==sizeof(*resources));xr_compile_resources_release(resources);
        CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    resources=NULL;CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    view_storage_guards(resources);view_storage_string_guards(resources);
    CHECK(compile_domain_stats(resources).live_bytes==sizeof(*resources));
    xr_compile_resources_release(resources);CHECK(!value_compile_live && !value_compile_bytes && !live && !calls);
    printf("View storage: Array COW, byte ranges, exact reborrows, owned copies, %zu complete allocation failures, three axes and physical0 PASS\n",sites);
    return 0;
}
