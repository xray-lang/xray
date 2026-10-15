/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_value_path_replacement_cases.h - Private value edits preserve their source
 */
#ifndef XIR_VALUE_PATH_REPLACEMENT_CASES_H
#define XIR_VALUE_PATH_REPLACEMENT_CASES_H
static void value_path_replacement_nested(XrXirValueAdmission *admission,bool boxed,bool resources) {
    XrXirValue root=value_path_root(admission,boxed),output={0},replacement={XR_XIR_I64,0,99};
    XrXirValuePath path={value_path_steps+(boxed ? 1 : 0),boxed ? 3 : 4};XrXirFaultDetail fault={0};
    size_t blocks=live,begin=calls;uint64_t bytes=admission->domain->stats.live_bytes;admission->work=100000;
    CHECK(xir_value_path_replacement(&root,&path,&replacement,admission,&output,&fault)==XR_XIR_VALUE_OK);
    size_t sites=calls-begin;uint64_t work=100000-admission->work;
    CHECK(sites && output.payload!=root.payload && value_path_number(&output,&path,admission)==99);
    CHECK(value_path_number(&root,&path,admission)==11);xr_xir_value_drop(&output);
    for (size_t i=0;i<sites;++i) {
        if (!resources && i && i!=sites-1) continue;
        admission->work=100000;fail_at=calls+i;
        CHECK(xir_value_path_replacement(&root,&path,&replacement,admission,&output,&fault)==XR_XIR_VALUE_OOM);
        CHECK(calls>fail_at);fail_at=SIZE_MAX;
        CHECK(unit_value(&output) && live==blocks && admission->domain->stats.live_bytes==bytes);
        CHECK(admission->scratch_bytes==65536 && value_path_number(&root,&path,admission)==11);
    }
    for (uint64_t i=0;i<work;++i) {
        if (!resources && i && i!=work-1) continue;
        admission->work=i;
        CHECK(xir_value_path_replacement(&root,&path,&replacement,admission,&output,&fault)==XR_XIR_VALUE_LIMIT);
        CHECK(unit_value(&output) && live==blocks && admission->domain->stats.live_bytes==bytes && admission->scratch_bytes==65536);
    }
    admission->work=100000;admission->scratch_bytes=0;
    CHECK(xir_value_path_replacement(&root,&path,&replacement,admission,&output,&fault)==XR_XIR_VALUE_LIMIT);
    admission->scratch_bytes=65536;
    XrXirValuePathStep bad_steps[4];memcpy(bad_steps,path.steps,path.count*sizeof(*bad_steps));
    XrXirValuePath bad={bad_steps,path.count};bad_steps[path.count-2].selector=-1;admission->work=100000;
    CHECK(xir_value_path_replacement(&root,&bad,&replacement,admission,&output,&fault)==XR_XIR_VALUE_BOUNDS);
    CHECK(fault.code==430 && fault.index==-1 && fault.length==1);
    bad=(XrXirValuePath){bad_steps,boxed ? 1 : 2};bad_steps[bad.count-1].selector=1;
    CHECK(xir_value_path_replacement(&root,&bad,&replacement,admission,&output,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!fault.code && unit_value(&output) && value_path_number(&root,&path,admission)==11);
    CHECK(xir_value_path_replacement(&root,&path,&replacement,NULL,&output,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xir_value_path_replacement(&root,NULL,&replacement,admission,&output,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(live==blocks && admission->domain->stats.live_bytes==bytes);xr_xir_value_drop(&root);
    printf("Private nested replacement: boxed=%u sites=%zu work=%llu injection=%s PASS\n",boxed,sites,(unsigned long long)work,resources ? "ALL" : "FIRST_LAST_ONLY");
}
static void value_path_replacement_identity(XrXirDomain *domain) {
    XrXirTypeArena *arena=class_value_arena(domain);XrXirValueAdmission admission={arena,domain,NULL,NULL,100000,65536};
    XrXirValue text={0},first={0},next={0},array={0},output={0},replacement={XR_XIR_I64,0,99};
    CHECK(xr_xir_string_new(domain,"identity",8,&text)==XR_XIR_VALUE_OK);
    XrXirValue fields[]={{XR_XIR_I64,0,11},text,text};
    CHECK(xr_xir_class_new((XrXirType)256,fields,3,&admission,&first)==XR_XIR_VALUE_OK);
    fields[0].payload=37;CHECK(xr_xir_class_new((XrXirType)256,fields,3,&admission,&next)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)257,&first,1,&admission,&array)==XR_XIR_VALUE_OK);
    const XrXirValuePathStep steps[]={{XR_XIR_PATH_INDEX,(XrXirType)257,0},{XR_XIR_PATH_FIELD,(XrXirType)256,0}};
    XrXirValuePath path={steps,2};XrXirFaultDetail fault={0};size_t blocks=live;uint64_t bytes=domain->stats.live_bytes;
    XrXirDomain *observer=NULL;XrXirDomainBudgetControls observer_limits={65536,65536,65536,65536,65536};
    CHECK(xr_xir_domain_new_budgeted(65536,&observer_limits,&observer)==XR_XIR_VALUE_OK);
    XrXirValueAdmission observing={arena,observer,NULL,NULL,65536,65536};XrXirValue observed={0};
    CHECK(xr_xir_array_get(&array,0,&observing,&observed,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && unit_value(&observed));
    uint64_t observer_work=xr_xir_domain_budget_stats(observer).work;
    CHECK(xir_value_observe_child(&array,steps,&observing,&observed,&fault)==XR_XIR_VALUE_OK);
    CHECK(observed.type==first.type && observed.payload==first.payload && object_pointer(&observed)->domain==domain);
    CHECK(xr_xir_domain_budget_stats(observer).work>observer_work && xr_xir_domain_stats(observer).live_bytes==sizeof(*observer));
    CHECK(xr_xir_class_set(&observed,0,&replacement,&observing)==XR_XIR_VALUE_BAD_ARGUMENT);class_value_count(&first,11);
    XrXirValuePathStep missing=steps[0];missing.selector=-1;XrXirValue unavailable={0};
    CHECK(xir_value_observe_child(&array,&missing,&observing,&unavailable,&fault)==XR_XIR_VALUE_BOUNDS);
    CHECK(unit_value(&unavailable) && fault.code==430 && fault.index==-1 && fault.length==1);
    XrXirFaultDetail retained={430,0,7,9};fault=retained;observing.work=0;
    CHECK(xir_value_observe_child(&array,steps,&observing,&unavailable,&fault)==XR_XIR_VALUE_LIMIT);
    CHECK(unit_value(&unavailable) && !memcmp(&fault,&retained,sizeof(fault)));
    xr_xir_domain_drop(observer);class_value_count(&observed,11);xr_xir_value_drop(&observed);fault=(XrXirFaultDetail){0};
    CHECK(xir_value_path_replacement(&array,&path,&replacement,&admission,&output,&fault)==XR_XIR_VALUE_UNSUPPORTED);
    CHECK(live==blocks && domain->stats.live_bytes==bytes && unit_value(&output));class_value_count(&first,11);class_value_count(&next,37);
    path=(XrXirValuePath){steps+1,1};
    CHECK(xir_value_path_replacement(&first,&path,&replacement,&admission,&output,&fault)==XR_XIR_VALUE_UNSUPPORTED);
    class_value_count(&first,11);path=(XrXirValuePath){steps,1};
    CHECK(xir_value_path_replacement(&array,&path,&next,&admission,&output,&fault)==XR_XIR_VALUE_OK);
    XrXirValue old={0},changed={0};
    CHECK(xr_xir_array_get(&array,0,&admission,&old,&fault)==XR_XIR_VALUE_OK && old.payload==first.payload);
    CHECK(xr_xir_array_get(&output,0,&admission,&changed,&fault)==XR_XIR_VALUE_OK && changed.payload==next.payload);
    xr_xir_value_drop(&old);xr_xir_value_drop(&changed);xr_xir_value_drop(&output);xr_xir_value_drop(&array);
    xr_xir_value_drop(&first);xr_xir_value_drop(&next);xr_xir_value_drop(&text);xr_xir_compile_type_arena_drop(arena);
    puts("Observer child retains foreign Class identity, spends only observer ledger and grants no Class write; bounds/work preserved PASS");
}
static void value_path_replacement_cases(bool resources) {
    CHECK(!live);XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirTypeArena *arena=value_path_arena(domain);XrXirValueAdmission admission={arena,domain,NULL,NULL,100000,65536};
    value_path_replacement_nested(&admission,false,resources);value_path_replacement_nested(&admission,true,resources);
    xr_xir_compile_type_arena_drop(arena);value_path_replacement_identity(domain);
    CHECK(domain->stats.live_bytes==sizeof(*domain));xr_xir_domain_drop(domain);
    CHECK(!live && !value_compile_live && !value_compile_bytes);
    puts("Private path replacements preserve roots and reject identity traversal; physical=0 PASS");
}
#endif // XIR_VALUE_PATH_REPLACEMENT_CASES_H
