/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_access_runtime_axes.h - Exact six independent owner quotas
 */
#ifndef XIR_CLASS_ACCESS_RUNTIME_AXES_H
#define XIR_CLASS_ACCESS_RUNTIME_AXES_H
static void access_other_axes(const AccessRuntimeCost *cost,const XrXirInstanceConfig *config,unsigned axis) {
    for (unsigned pass=0;pass<2;++pass) {
        const XrXirDomainBudgetStats *b=&cost->budgets[pass];
        if (!b->bound) continue;
        if (axis!=0) CHECK(b->requested_bytes<b->requested_limit);
        if (axis!=1) CHECK(b->requested_call_bytes<b->requested_call_limit);
        if (axis!=2) CHECK(b->work<b->work_limit);
        if (axis!=3) CHECK(cost->values[pass].peak_bytes<config->value_limit);
        if (axis!=4) CHECK(b->call_peak<b->call_limit);
        if (axis!=5) CHECK(b->metadata_peak<b->metadata_limit);
    }
}
static void access_set_axis(XrXirInstanceConfig *config,unsigned axis,uint64_t cap) {
    if (axis==0) config->requested_value_limit=cap;
    else if (axis==1) config->requested_call_limit=cap;
    else if (axis==2) config->work_limit=cap;
    else if (axis==3) config->value_limit=cap;
    else if (axis==4) config->call_limit=cap;
    else config->metadata_limit=cap;
}
static uint64_t access_cost_axis(const AccessRuntimeCost *cost,unsigned pass,unsigned axis) {
    const XrXirDomainBudgetStats *b=&cost->budgets[pass];
    return axis==0?b->requested_bytes:axis==1?b->requested_call_bytes:axis==2?b->work:
        axis==3?cost->values[pass].peak_bytes:axis==4?b->call_peak:b->metadata_peak;
}
static uint64_t access_live_boundary(const AccessRuntimeTrial *trial,unsigned pass,unsigned axis,uint64_t peak) {
    CHECK(axis>=3 && peak>1);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    access_set_axis(&config,axis,peak);cell_runtime_fault_count=0;
    AccessRuntimeCost cost=access_trial(trial,&config,pass);
    CHECK(cost.complete && cost.failure==XR_XIR_CALL_READY && !cost.hits);access_other_axes(&cost,&config,axis);
    uint64_t exact=access_cost_axis(&cost,pass,axis);CHECK(exact>1 && exact<=peak);
    /* A cap-dependent Call reserve can shrink. Every accepted probe must
     * strictly lower the observed whole-operation live peak. */
    for (uint64_t remaining=peak;remaining;--remaining) {
        access_set_axis(&config,axis,exact-1);cost=access_trial(trial,&config,pass);
        CHECK(!cost.hits && access_cost_axis(&cost,pass,axis)<exact);access_other_axes(&cost,&config,axis);
        if (cost.failure==XR_XIR_CALL_LIMIT) return exact;
        CHECK(cost.complete && cost.failure==XR_XIR_CALL_READY);
        exact=access_cost_axis(&cost,pass,axis);CHECK(exact>1);
    }
    CHECK(false);return 0;
}
static void access_runtime_axes(const AccessRuntimeTrial *trial,const AccessRuntimeCost *baseline) {
    for (unsigned pass=0;pass<2;++pass) for (unsigned axis=0;axis<6;++axis) {
        uint64_t exact=access_cost_axis(baseline,pass,axis);CHECK(exact>1);
        if (axis>=3) exact=access_live_boundary(trial,pass,axis,exact);
        for (unsigned minus=0;minus<2;++minus) {
            XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
            access_set_axis(&config,axis,exact-minus);cell_runtime_fault_count=0;
            AccessRuntimeCost cost=access_trial(trial,&config,pass);
            printf("CLASS_ACCESS_RUNTIME_AXIS fixture=%s mode=%u owner=%u axis=%u minus=%u exact=%llu actual=%u sites=%zu physical=0/0\n",
                trial->fixture->name,trial->mode,pass,axis,minus,(unsigned long long)exact,(unsigned)cost.failure,cost.sites);
            CHECK(!cost.hits && cost.failure==(minus?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY));
            if (!minus) CHECK(cost.complete);
            const XrXirDomainBudgetStats *b=&cost.budgets[pass];
            if (b->bound) CHECK(b->requested_bytes<=b->requested_limit && b->requested_call_bytes<=b->requested_call_limit &&
                b->work<=b->work_limit && b->call_peak<=b->call_limit && b->metadata_peak<=b->metadata_limit);
            CHECK(cost.values[pass].peak_bytes<=config.value_limit);access_other_axes(&cost,&config,axis);
        }
    }
}
#endif // XIR_CLASS_ACCESS_RUNTIME_AXES_H
