/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_access_runtime_cases.h - Real Class access resource registrations
 */
#ifndef XIR_CLASS_ACCESS_RUNTIME_CASES_H
#define XIR_CLASS_ACCESS_RUNTIME_CASES_H
#ifndef XR_CLASS_ACCESS_PROGRAM_NEW
#define XR_CLASS_ACCESS_PROGRAM_NEW cell_source_program
#endif
static void access_runtime_census(const AccessRuntimeTrial *trial,const AccessRuntimeCost *cost) {
    printf("CLASS_ACCESS_RUNTIME_CENSUS fixture=%s mode=%u sites=%zu prepare=%zu..%zu results=%u/%u getters=%u/%u loans=%u physical=0/0\n",
        trial->fixture->name,trial->mode,cost->sites,cost->prepare_begin,cost->prepare_end,
        cost->results[0],cost->results[1],cost->getters[0],cost->getters[1],cost->loans);
    for (unsigned pass=0;pass<2;++pass) {
        const XrXirDomainBudgetStats *b=&cost->budgets[pass];
        printf("CLASS_ACCESS_RUNTIME_COST fixture=%s mode=%u owner=%u value=%llu call=%llu work=%llu valuePeak=%llu callPeak=%llu metadataPeak=%llu\n",
            trial->fixture->name,trial->mode,pass,(unsigned long long)b->requested_bytes,
            (unsigned long long)b->requested_call_bytes,(unsigned long long)b->work,
            (unsigned long long)cost->values[pass].peak_bytes,(unsigned long long)b->call_peak,
            (unsigned long long)b->metadata_peak);
    }
}
static void access_runtime_cases(const char *operation) {
    for (unsigned i=0;i<ACCESS_SOURCE_CASE_COUNT;++i) {
        const AccessSourceCase *fixture=&access_source_cases[i];
        if (fixture->rejected) continue;
        access_physical_zero();CHECK(!cell_runtime_observer_live);
        SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
        XrXirProgram *program=NULL;CellSourceEntries entries={UINT32_MAX,UINT32_MAX,UINT32_MAX};
        CHECK(XR_CLASS_ACCESS_PROGRAM_NEW(&owner.context,fixture->name,&program,&entries,false)==XR_XIR_OK && program);
        CHECK(entries.run!=UINT32_MAX && entries.factory==UINT32_MAX);
        CHECK((entries.counter!=UINT32_MAX)==fixture->alias);
        for (unsigned mode=0;mode<(fixture->alias?1u:ACCESS_MODES);++mode) {
            AccessRuntimeTrial trial={program,&entries,fixture,mode};
            cell_runtime_fault_count=0;access_runtime_retry=false;
#ifdef XR_CLASS_ACCESS_TRIAL_BEGIN
            XR_CLASS_ACCESS_TRIAL_BEGIN();
#endif
            AccessRuntimeCost baseline=access_trial(&trial,NULL,0);
            CHECK(baseline.complete && baseline.failure==XR_XIR_CALL_READY && baseline.sites && !baseline.hits);
#ifdef XR_CLASS_ACCESS_TRIAL_END
            XR_CLASS_ACCESS_TRIAL_END(fixture,mode);
#endif
            access_runtime_census(&trial,&baseline);
            CellRuntimeAllocationEvent *events=cell_runtime_observer_new(baseline.sites*sizeof(*events));
            memcpy(events,cell_runtime_events,baseline.sites*sizeof(*events));access_runtime_runs=0;
            if (!strcmp(operation,"--runtime-fi")) {
                access_runtime_tree(&trial,0,NULL);
                access_runtime_retry=true;access_runtime_tree(&trial,0,NULL);access_runtime_retry=false;
            }
            else if (!strcmp(operation,"--runtime-axes")) access_runtime_axes(&trial,&baseline);
            else if (!strcmp(operation,"--runtime-prepare")) access_runtime_prepare(&trial,&baseline,events);
            else CHECK(!strcmp(operation,"--runtime-control"));
            printf("CLASS_ACCESS_RUNTIME_CLOSURE fixture=%s mode=%u originalSites=%zu failureRuns=%llu operation=%s\n",
                fixture->name,mode,baseline.sites,(unsigned long long)access_runtime_runs,operation);
            cell_runtime_observer_free(events);cell_runtime_fault_count=0;access_runtime_retry=false;
        }
        xr_xir_compile_program_drop(program);
#ifdef XR_CLASS_ACCESS_PROGRAM_DROP
        XR_CLASS_ACCESS_PROGRAM_DROP();
#endif
        source_fixture_owner_free(&owner);
        access_physical_zero();CHECK(!cell_runtime_observer_live);
    }
}
#endif // XIR_CLASS_ACCESS_RUNTIME_CASES_H
