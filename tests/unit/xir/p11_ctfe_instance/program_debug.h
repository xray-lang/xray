/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * program_debug.h - Literal observations survive both Source owners
 */
static void program_debug_observe(const XrXirSourceSnapshot *snapshot,const XrXirModule *module,uint64_t counts[4]) {
    const XrXirSourceDebugView *debug=xr_xir_compile_source_snapshot_debug(snapshot);
    CHECK(debug && (!module || debug->function_count==module->function_count));
    memset(counts,0,4*sizeof(*counts));counts[0]=debug->function_count;
    for (uint32_t f=0;f<debug->function_count;++f) {
        const XrXirSourceDebugFunction *function=&debug->functions[f];
        CHECK(function->function==f);
        if (module) CHECK(function->parameter_count==module->functions[f].parameter_count &&
            function->instruction_count==module->functions[f].instruction_count);
        counts[1]+=function->instruction_count;counts[3]+=function->binding_count;
        for (uint32_t i=0;function->points && i<function->instruction_count;++i) {
            const XrXirSourceDebugPoint *point=&function->points[i];
            counts[2]+=point->statement;
            if (point->statement) CHECK(point->range.line>0 && point->range.column>0);
        }
    }
    const XrXirConstruction *construction=xr_xir_compile_source_snapshot_construction(snapshot);
    const XrXirComptimeTable *table=xr_xir_compile_construction_comptime(construction);
    CHECK(table);
    for (uint32_t b=0;b<table->binding_count;++b) {
        CHECK(table->bindings[b].provider<debug->function_count);
        const XrXirSourceDebugFunction *provider=&debug->functions[table->bindings[b].provider];
        CHECK(!provider->declaration && !provider->parameter_count && provider->points &&
            !provider->binding_count && !provider->throw_count && !provider->handler_count && !provider->route_count);
        for (uint32_t i=0;i<provider->instruction_count;++i)
            CHECK(!provider->points[i].statement && !provider->points[i].flags && !provider->points[i].environment);
    }
    uint32_t materializations=xr_xir_compile_construction_materialization_count(construction);
    for (uint32_t m=0;m<materializations;++m) {
        const XrXirMaterializationBinding *binding=xr_xir_compile_construction_materialization(construction,m);
        CHECK(binding && binding->function<debug->function_count && debug->functions[binding->function].points);
    }
}
