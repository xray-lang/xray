/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_final_retirement_cases.h - Physical solver retirement and owned Final reads
 *
 * KEY CONCEPT: A free callback observes real retired storage before any final
 * node, formula or cause query; all original allocation faults retain charges.
 */
#ifndef XIR_INVOCATION_FINAL_RETIREMENT_CASES_H
#define XIR_INVOCATION_FINAL_RETIREMENT_CASES_H
static void invocation_final_retirement_literals(void) {
    for (uint32_t mode=0;mode<4;++mode) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(conditional_invocation_core(&context,mode,true,false)==XR_XIR_OK);
        CHECK(!rp_release_observer);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    puts("Final reads and owned trace after four real solver/producer retirements");
}
static void invocation_final_retirement_resources(void) {
    for (uint32_t which=25;which<29;++which) {
        size_t sites=0;XrCompileResourceStats single={0};
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
            XrCompileResourceStats entry=rp_stats(&context);XrCompileResources *identity=context.resources;
            RootParameterMark retained=rp_mark();
            rp_attempts=0;rp_injected=false;rp_fail_at=pass?pass-1:SIZE_MAX;
            XrXirStatus status=conditional_resource_case(&context,which);
            rp_fail_at=SIZE_MAX;
            if (!pass) { CHECK(status==XR_XIR_OK);sites=rp_attempts;single=rp_stats(&context); }
            else CHECK(rp_injected && status==XR_XIR_OUT_OF_MEMORY);
            ConditionalRetryTrial retry={.which=which,.ordinal=pass?pass-1:SIZE_MAX,.sites=sites,
                .resources=identity,.initial=entry,.single=single,.retained=retained,.first=status};
            if (pass) conditional_retry_operation(&context,&retry);
            rp_owner_free(&context,entry.live_bytes);rp_balanced(physical);
        }
        printf("Final retirement case%u complete allocation census %zu\n",which-25,sites);
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        XrCompileResourceStats entry=rp_stats(&context);
        size_t census_attempts=rp_attempts;
        CHECK(conditional_resource_case(&context,which)==XR_XIR_OK);
        size_t census_sites=rp_attempts-census_attempts;
        XrCompileResourceStats census=rp_stats(&context);
        rp_owner_free(&context,entry.live_bytes);rp_balanced(physical);
        for (uint32_t axis=0;axis<3;++axis) for (uint32_t minus=0;minus<2;++minus) {
            XrCompileResourceLimits caps=rp_caps();
            if (!axis) caps.allocated_bytes=census.allocated_bytes-minus;
            else if (axis==1) caps.live_bytes=census.peak_bytes-minus;
            else caps.work=census.work-minus;
            physical=rp_mark();context=rp_owner(caps);entry=rp_stats(&context);
            RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
            XrXirStatus status=conditional_resource_case(&context,which);
            CHECK(status==(minus?XR_XIR_BUDGET:XR_XIR_OK));
            ConditionalRetryTrial retry={.which=which,.ordinal=SIZE_MAX,.sites=census_sites,
                .resources=identity,.initial=entry,.single=census,.retained=retained,.first=status};
            conditional_retry_operation(&context,&retry);
            rp_owner_free(&context,entry.live_bytes);rp_balanced(physical);
        }
    }
}
#endif // XIR_INVOCATION_FINAL_RETIREMENT_CASES_H
