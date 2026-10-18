/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_value_observer_wrapper_cases.h - Owned immutable wrapper projections
 */
#ifndef XIR_VALUE_OBSERVER_WRAPPER_CASES_H
#define XIR_VALUE_OBSERVER_WRAPPER_CASES_H
static void value_observer_unit_case(XrXirTypeArena *arena,XrXirDomain *domain,XrXirDomain *observer) {
    XrXirValueAdmission owner={arena,domain,NULL,NULL,65536,65536};
    XrXirValue empty={0},value={0};
    CHECK(!xr_xir_value_argument(&empty,arena,XR_XIR_UNIT));
    CHECK(xr_xir_tuple_new((XrXirType)260,&empty,1,&owner,&value)==XR_XIR_VALUE_OK);
    XrXirValuePathStep step={XR_XIR_PATH_TUPLE_FIELD,(XrXirType)260,0};
    XrXirValueAdmission observing={arena,observer,NULL,NULL,65536,65536};
    XrXirValue result={0};XrXirFaultDetail fault={0};size_t allocations=calls;
    CHECK(xir_value_observe_child(&value,&step,&observing,&result,&fault)==XR_XIR_VALUE_OK);
    uint64_t spent=65536-observing.work;
    CHECK(spent && unit_value(&result) && calls==allocations);
    for (uint64_t work=0;work<spent;++work) {
        observing.work=work;XrXirFaultDetail kept={430,0,7,9};fault=kept;
        CHECK(xir_value_observe_child(&value,&step,&observing,&result,&fault)==XR_XIR_VALUE_LIMIT);
        CHECK(unit_value(&result) && !memcmp(&kept,&fault,sizeof(kept)) && calls==allocations);
    }
    XirTuple *tuple=(XirTuple *)object_pointer(&value);tuple->fields[0].payload=1;observing.work=65536;
    CHECK(xir_value_observe_child(&value,&step,&observing,&result,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(unit_value(&result) && calls==allocations);tuple->fields[0].payload=0;xr_xir_value_drop(&value);
    puts("Unit child observation authenticates its parent, spends exact finite work, allocates nothing and grants no argument admission PASS");
}
static void value_observer_wrapper_cases(XrXirDomain *domain) {
    XrXirNominalFieldIdentity field={ {"value",5},XR_XIR_FIELD_MUTABLE },payload={ {"object",6},0 };
    XrXirNominalVariant variants[]={{{"Full",4},0,1},{{"Empty",5},1,0}};
    XrXirNominalIdentity identities[]={
        {{"wrapper",7},{"Box",3},1,0,&field,1,XR_XIR_NOMINAL_CLASS,NULL,0,XR_XIR_NOMINAL_FINAL,{0}},
        {{"wrapper",7},{"Choice",6},1,0,&payload,1,XR_XIR_NOMINAL_ENUM,variants,2,0,{0}}};
    XrXirNominalTable table={NULL,2,identities};XrXirType scalar[]={XR_XIR_I64},object[]={(XrXirType)256};
    XrXirCallableParameter unit_member[]={{XR_XIR_UNIT,XR_PARAM_READ}};
    XrXirCallableParameter members[]={{(XrXirType)256,XR_PARAM_READ},{XR_XIR_I64,XR_PARAM_READ}};
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,scalar,1}},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=members,.parameter_count=2},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={1,NULL,0,object,1}},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=unit_member,.parameter_count=1}};
    XrXirTypes types={nodes,5,&table,NULL};XrXirTypeArena *arena=NULL;
    XrCompileResourceLimits limits=value_compile_limits(1048576,1048576,1000000);
    CHECK(value_compile_arena(&types,100,limits,&arena)==XR_XIR_VALUE_OK);
    memset(nodes,0xcc,sizeof(nodes));memset(identities,0xcc,sizeof(identities));memset(variants,0xcc,sizeof(variants));
    XrXirValueAdmission owner={arena,domain,NULL,NULL,100000,65536};XrXirValue box={0},values[5]={{0}};
    XrXirValue number={XR_XIR_I64,0,11};CHECK(xr_xir_class_new((XrXirType)256,&number,1,&owner,&box)==XR_XIR_VALUE_OK);
    XrXirValue fields[]={box,{XR_XIR_I64,0,13}};
    CHECK(xr_xir_tuple_new((XrXirType)257,fields,2,&owner,&values[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)258,&box,&owner,&values[1])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_enum_new((XrXirType)259,0,&box,1,&owner,&values[2])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)258,NULL,&owner,&values[3])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_enum_new((XrXirType)259,1,NULL,0,&owner,&values[4])==XR_XIR_VALUE_OK);
    XrXirDomain *observer=NULL;XrXirDomainBudgetControls observer_limits={65536,65536,65536,65536,65536};
    CHECK(xr_xir_domain_new_budgeted(65536,&observer_limits,&observer)==XR_XIR_VALUE_OK);
    value_observer_unit_case(arena,domain,observer);
    XrXirValueAdmission observing={arena,observer,NULL,NULL,65536,65536};XrXirValue retained[3]={{0}};
    const XrXirValuePathKind kinds[]={XR_XIR_PATH_TUPLE_FIELD,XR_XIR_PATH_NULLABLE_VALUE,XR_XIR_PATH_ENUM_FIELD};
    for (uint32_t i=0;i<5;++i) {
        uint32_t family=i<3 ? i : i-2;XrXirValuePathStep step={kinds[family],(XrXirType)values[i].type,0};XrXirFaultDetail fault={0};XrXirValue result={0};
        uint64_t work=xr_xir_domain_budget_stats(observer).work,available=observing.work;size_t allocations=calls;
        XrXirValueStatus status=xir_value_observe_child(&values[i],&step,&observing,&result,&fault);
        CHECK(calls==allocations && xr_xir_domain_budget_stats(observer).work>work);
        uint64_t spent=available-observing.work;CHECK(spent);
        for (uint64_t limit=0;limit<spent;++limit) {
            XrXirValue unavailable={0};XrXirFaultDetail kept={430,0,7,9},failed=kept;observing.work=limit;
            CHECK(xir_value_observe_child(&values[i],&step,&observing,&unavailable,&failed)==XR_XIR_VALUE_LIMIT);
            CHECK(unit_value(&unavailable) && !memcmp(&kept,&failed,sizeof(kept)) && calls==allocations);
        }
        observing.work=65536;
        if (i>=3) {CHECK(status==XR_XIR_VALUE_BOUNDS && !fault.length && unit_value(&result));continue;}
        CHECK(status==XR_XIR_VALUE_OK && result.type==box.type && result.payload==box.payload && object_pointer(&result)->domain==domain);
        CHECK(xr_xir_class_set(&result,0,&number,&observing)==XR_XIR_VALUE_BAD_ARGUMENT);retained[i]=result;
        XrXirValuePath path={&step,1};XrXirValue candidate={0};owner.work=100000;size_t blocks=live;
        CHECK(xir_value_path_replacement(&values[i],&path,&box,&owner,&candidate,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(unit_value(&candidate) && live==blocks);
        step.selector=-1;CHECK(xir_value_observe_child(&values[i],&step,&observing,&candidate,&fault)==XR_XIR_VALUE_BOUNDS && fault.index==-1);
        step.selector=0;step.container=XR_XIR_I64;
        CHECK(xir_value_observe_child(&values[i],&step,&observing,&candidate,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && unit_value(&candidate));
        step.container=(XrXirType)values[i].type;step.kind=kinds[(family+1)%3];
        CHECK(xir_value_observe_child(&values[i],&step,&observing,&candidate,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && unit_value(&candidate));
    }
    xr_xir_domain_drop(observer);
    for (uint32_t i=0;i<3;++i) {class_value_count(&retained[i],11);xr_xir_value_drop(&retained[i]);}
    for (uint32_t i=0;i<5;++i) xr_xir_value_drop(&values[i]);xr_xir_value_drop(&box);xr_xir_compile_type_arena_drop(arena);
    puts("Immutable wrapper observation: exact Class identity/observer work/no allocations/empty and malformed rejection/no value mutation/owner lifetime PASS");
}
#endif // XIR_VALUE_OBSERVER_WRAPPER_CASES_H
