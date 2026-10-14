/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu
 * Licensed under the MIT License
 *
 * Actual whole-operation fault ordinals partition at genuine Program sealing.
 * This is a test coverage map, never a compiler permission or staged success.
 */
#ifndef XIR_ALLOCATION_PHASE_MAP_H
#define XIR_ALLOCATION_PHASE_MAP_H

typedef struct AllocationPhaseMap {
    const char *family;
    unsigned scalar,mode;
    size_t whole,prefix,runtime,compiler_visited,runtime_visited;
} AllocationPhaseMap;
typedef struct AllocationPhasePhysical { size_t blocks,bytes; } AllocationPhasePhysical;
enum { ALLOCATION_PHASE_COMPILER=1,ALLOCATION_PHASE_RUNTIME=2,ALLOCATION_PHASE_BRIDGE=4 };
static unsigned allocation_phase_selection=7;
static size_t allocation_phase_ordinals,allocation_phase_cases;
static size_t allocation_phase_compiler_ordinals,allocation_phase_runtime_ordinals,allocation_phase_bridge_ordinals;

static AllocationPhaseMap allocation_phase_map(const char *family,unsigned scalar,unsigned mode,
    size_t whole,size_t prefix) {
    CHECK(prefix && prefix<whole && whole<10000);
    allocation_request_equal(&allocation_whole_requests,0,&allocation_prefix_requests,0,prefix);
    return (AllocationPhaseMap){family,scalar,mode,whole,prefix,whole-prefix,0,0};
}
static void allocation_phase_compile_begin(AllocationPhaseMap *map,size_t ordinal) {
    CHECK(!live && !class_live_bytes && ordinal==map->compiler_visited && ordinal<map->prefix);
    allocation_calls=0;fail_at=ordinal;
}
static void allocation_phase_compile_end(AllocationPhaseMap *map,size_t ordinal) {
    CHECK(ordinal==map->compiler_visited && allocation_calls>=ordinal+1 && !live && !class_live_bytes);
    fail_at=SIZE_MAX;++map->compiler_visited;
}
static AllocationPhasePhysical allocation_phase_physical(void) {
    return (AllocationPhasePhysical){live,class_live_bytes};
}
static void allocation_phase_restore(XrXirProgram *program,AllocationPhasePhysical physical,
    const XrCompileResourceStats *prefix) {
    CHECK(program && atomic_load(&program->references)==1);
    CHECK(live==physical.blocks && class_live_bytes==physical.bytes);
    XrCompileResourceStats stats=native_fixture_stats(&program->context);
    CHECK(stats.work==prefix->work && stats.allocated_bytes==prefix->allocated_bytes &&
        stats.live_bytes==prefix->live_bytes);
}
static void allocation_phase_runtime_begin(AllocationPhaseMap *map,size_t ordinal,
    XrXirProgram *program) {
    CHECK(ordinal==map->runtime_visited && ordinal<map->runtime);
    CHECK(map->prefix+ordinal<map->whole && atomic_load(&program->references)==1);
    allocation_calls=0;fail_at=ordinal;
    allocation_request_begin(&allocation_runtime_requests);
    CHECK(xr_xir_compile_program_retain(program));
}
static void allocation_phase_runtime_end(AllocationPhaseMap *map,size_t ordinal,
    XrXirProgram *program,AllocationPhasePhysical physical,const XrCompileResourceStats *prefix) {
    allocation_request_end();
    CHECK(ordinal==map->runtime_visited && allocation_calls>=ordinal+1);
    if(allocation_phase_selection&ALLOCATION_PHASE_BRIDGE) {
        CHECK(allocation_reference_requests.count>=map->prefix &&
            allocation_runtime_requests.count==allocation_reference_requests.count-map->prefix);
        allocation_request_equal(&allocation_reference_requests,map->prefix,&allocation_runtime_requests,0,
            allocation_reference_requests.count-map->prefix);
    }
    allocation_phase_restore(program,physical,prefix);
    fail_at=SIZE_MAX;++map->runtime_visited;
}
static void allocation_phase_complete(AllocationPhaseMap *map) {
    CHECK(map->compiler_visited==((allocation_phase_selection&ALLOCATION_PHASE_COMPILER)?map->prefix:0));
    CHECK(map->runtime_visited==((allocation_phase_selection&ALLOCATION_PHASE_RUNTIME)?map->runtime:0));
    CHECK(!live && !class_live_bytes);
    allocation_phase_compiler_ordinals+=map->compiler_visited;
    allocation_phase_runtime_ordinals+=map->runtime_visited;
    allocation_phase_ordinals+=map->whole;++allocation_phase_cases;
    printf("FI map %s scalar%u mode%u: whole=%zu compiler=[0,%zu) runtime=[%zu,%zu) local=[0,%zu)\n",
        map->family,map->scalar,map->mode,map->whole,map->prefix,map->prefix,map->whole,map->runtime);
}
#endif // XIR_ALLOCATION_PHASE_MAP_H
