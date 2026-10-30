/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_effect_analysis_owner.h - Finite compiler ledgers and physical failure ownership
 */
#ifndef XIR_EFFECT_ANALYSIS_OWNER_H
#define XIR_EFFECT_ANALYSIS_OWNER_H
#include "xir_construction_fixture.h"
#include "xir/xxir_internal.h"
#include "base/xmalloc.h"
#include "base/xhash.h"

typedef struct EffectAllocation { void *pointer; size_t bytes; } EffectAllocation;
typedef struct EffectMark { size_t blocks, bytes; } EffectMark;
static EffectAllocation effect_blocks[32768];
/* Fixed observer storage cannot add compiler allocation or fault sites. Values
 * locate dense physical records; every lookup still compares the full pointer. */
static uint32_t effect_block_index[65536];
static size_t live, live_bytes, attempts, fail_at = SIZE_MAX;
static bool injected;
static XrXirCompileContext effect_context;
static XrCompileResourceStats effect_baseline;
static EffectMark effect_expected;
static XrXirEffects *effect_owned_summary;
static EffectMark effect_mark(void) { return (EffectMark){live, live_bytes}; }
static void effect_mark_check(EffectMark mark) { CHECK(live == mark.blocks && live_bytes == mark.bytes); }
static size_t effect_index_home(const void *pointer) {
    return (size_t)xr_hash_int((int64_t)(uintptr_t)pointer) & 65535;
}
static size_t effect_index_find(const void *pointer) {
    size_t slot = effect_index_home(pointer);
    for (size_t scanned = 0; ; ++scanned) {
        CHECK(scanned < 65536);
        uint32_t record = effect_block_index[slot];
        if (!record) return slot;
        CHECK(record <= live);
        if (effect_blocks[record - 1].pointer == pointer) return slot;
        slot = (slot + 1) & 65535;
    }
}
/* Backshift deletion leaves no tombstones across successive finite owners. */
static void effect_index_erase(size_t hole) {
    CHECK(hole < 65536 && effect_block_index[hole]);
    for (size_t next = (hole + 1) & 65535, scanned = 0; effect_block_index[next];
         next = (next + 1) & 65535, ++scanned) {
        CHECK(scanned < 65536 && effect_block_index[next] <= live);
        size_t home = effect_index_home(effect_blocks[effect_block_index[next] - 1].pointer);
        bool stays = hole <= next ? hole < home && home <= next : hole < home || home <= next;
        if (!stays) { effect_block_index[hole] = effect_block_index[next]; hole = next; }
    }
    effect_block_index[hole] = 0;
}
static void *effect_malloc(size_t bytes) {
    if (attempts++ == fail_at) { injected = true; return NULL; }
    void *p = xr_malloc(bytes);
    if (p) {
        CHECK(live < 32768 && bytes <= SIZE_MAX - live_bytes);
        size_t slot = effect_index_find(p); CHECK(!effect_block_index[slot]);
        effect_blocks[live] = (EffectAllocation){p, bytes};
        effect_block_index[slot] = (uint32_t)live + 1; ++live; live_bytes += bytes;
    }
    return p;
}
static void effect_free(void *p) {
    if (!p) return;
    size_t slot = effect_index_find(p); uint32_t record = effect_block_index[slot];
    CHECK(record && record <= live);
    size_t i = (size_t)record - 1;
    CHECK(effect_blocks[i].bytes <= live_bytes);
    effect_index_erase(slot); live_bytes -= effect_blocks[i].bytes;
    if (i != live - 1) {
        size_t moved = effect_index_find(effect_blocks[live - 1].pointer);
        CHECK(effect_block_index[moved] == live);
        effect_blocks[i] = effect_blocks[live - 1]; effect_block_index[moved] = (uint32_t)i + 1;
    }
    --live; xr_free(p);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) effect_malloc(bytes)
#define xr_free(p) effect_free(p)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

static XrCompileResourceLimits effect_caps(void) {
    return (XrCompileResourceLimits){UINT64_C(64)*1024*1024, UINT64_C(8)*1024*1024, UINT64_C(128000000)};
}
static XrCompileResourceStats effect_stats(const XrXirCompileContext *c) {
    XrCompileResourceStats s = {0}; CHECK(xr_compile_resources_stats(c->resources, &s) == XR_COMPILE_RESOURCE_OK); return s;
}
static XrXirCompileContext effect_owner_new(XrCompileResourceLimits caps) {
    XrCompileResourceLimits bound = effect_caps();
    CHECK(caps.allocated_bytes <= bound.allocated_bytes && caps.live_bytes <= bound.live_bytes && caps.work <= bound.work);
    XrXirCompileContext c = {0};
    CHECK(xr_compile_resources_new(&caps, &c.resources) == XR_COMPILE_RESOURCE_OK);
    c.limits = xr_xir_compile_default_limits(); return c;
}
static void effect_owner_free(XrXirCompileContext *c, uint64_t baseline) {
    CHECK(effect_stats(c).live_bytes == baseline);
    xr_compile_resources_release(c->resources); *c = (XrXirCompileContext){0};
}
static void effect_case_begin(void) {
    CHECK(!live && !live_bytes && !effect_owned_summary);
    effect_context = effect_owner_new(effect_caps()); effect_baseline = effect_stats(&effect_context);
    effect_expected = effect_mark();
}
static void effect_case_end(void) {
    XrCompileResourceStats s = effect_stats(&effect_context);
    CHECK(!effect_owned_summary && s.live_bytes == effect_baseline.live_bytes);
    fprintf(stderr,"effect ledger: allocated=%llu peak=%llu work=%llu allocations=%llu\n",
        (unsigned long long)s.allocated_bytes,(unsigned long long)s.peak_bytes,
        (unsigned long long)s.work,(unsigned long long)s.allocation_count);
    effect_owner_free(&effect_context, effect_baseline.live_bytes); CHECK(!live && !live_bytes);
}
/* A summary may outlive its producer; producer releases adjust the retained
 * baseline, while final owner release independently checks all artifact bytes. */
static XrXirStatus effect_analyze(const XrXirArtifact *artifact, XrXirEffects **output) {
    CHECK(!effect_owned_summary && output && !*output);
    effect_expected = effect_mark();
    XrXirStatus status = xr_xir_compile_effects_analyze(artifact, output);
    if (status == XR_XIR_OK) { CHECK(*output); effect_owned_summary = *output; }
    else { CHECK(!*output); effect_mark_check(effect_expected); }
    return status;
}
static void effect_summary_free(XrXirEffects *summary) {
    CHECK(!summary || summary == effect_owned_summary);
    xr_xir_compile_effects_free(summary);
    if (summary) { effect_owned_summary = NULL; effect_mark_check(effect_expected); }
}
static void effect_artifact_free(XrXirArtifact *artifact) {
    EffectMark before = effect_mark(); xr_xir_compile_artifact_free(artifact);
    CHECK(live <= before.blocks && live_bytes <= before.bytes);
    if (effect_owned_summary) {
        CHECK(before.blocks-live <= effect_expected.blocks && before.bytes-live_bytes <= effect_expected.bytes);
        effect_expected.blocks -= before.blocks-live; effect_expected.bytes -= before.bytes-live_bytes;
    } else effect_expected = effect_mark();
}
static bool effect_balanced(void) {
    effect_mark_check(effect_expected); return true;
}
/* A new probe begins at the same semantic graph, through real reverification.
 * Lowered instances are reconstructed from their sealed Checked packet. */
static XrXirStatus effect_reproduce(const XrXirArtifact *source, const XrXirCompileContext *c, XrXirArtifact **out) {
    const XrXirModule *m = xr_xir_compile_artifact_module(source);
    if (m->stage == XR_XIR_CHECKED) return xr_xir_compile_recheck_v2(c,m,xr_xir_compile_artifact_construction(source), out, NULL, NULL);
    CHECK(m->stage == XR_XIR_LOWERED);
    XrXirArtifact *checked = NULL;
    XrXirStatus status = xr_xir_compile_checked_read(c, source->checked_packet.bytes, source->checked_packet.length, &checked, NULL);
    if (status == XR_XIR_OK) status = xr_xir_compile_lower(checked, &source->target, NULL, out, NULL);
    xr_xir_compile_artifact_free(checked); return status;
}
/* Limits apply to the complete probe lifetime, never a replacement stage budget. */
static void effect_budget_probe(const XrXirArtifact *source, XrCompileResourceLimits caps, bool exact) {
    EffectMark mark = effect_mark();
    XrXirCompileContext c = effect_owner_new(caps); uint64_t baseline = effect_stats(&c).live_bytes;
    XrXirArtifact *copy = NULL; XrXirEffects *summary = NULL;
    XrXirStatus status = effect_reproduce(source, &c, &copy);
    if (status == XR_XIR_OK) status = xr_xir_compile_effects_analyze(copy, &summary);
    CHECK(status == (exact ? XR_XIR_OK : XR_XIR_BUDGET)); CHECK(exact ? summary != NULL : summary == NULL);
    xr_xir_compile_effects_free(summary); xr_xir_compile_artifact_free(copy);
    effect_owner_free(&c, baseline); effect_mark_check(mark);
}
static void effect_budget_boundaries(const XrXirArtifact *source) {
    EffectMark mark = effect_mark(); XrXirCompileContext c = effect_owner_new(effect_caps());
    uint64_t baseline = effect_stats(&c).live_bytes; XrXirArtifact *copy = NULL; XrXirEffects *summary = NULL;
    CHECK(effect_reproduce(source,&c,&copy) == XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(copy,&summary) == XR_XIR_OK);
    XrCompileResourceStats measured = effect_stats(&c);
    xr_xir_compile_effects_free(summary); xr_xir_compile_artifact_free(copy); effect_owner_free(&c,baseline); effect_mark_check(mark);
    fprintf(stderr, "effect exact boundaries: allocated=%llu live=%llu work=%llu\n",
        (unsigned long long)measured.allocated_bytes, (unsigned long long)measured.peak_bytes,
        (unsigned long long)measured.work);
    for (unsigned axis = 0; axis < 3; ++axis) {
        XrCompileResourceLimits caps = effect_caps();
        uint64_t *field = axis == 0 ? &caps.allocated_bytes : axis == 1 ? &caps.live_bytes : &caps.work;
        *field = axis == 0 ? measured.allocated_bytes : axis == 1 ? measured.peak_bytes : measured.work;
        effect_budget_probe(source,caps,true); CHECK(*field > 1); --*field; effect_budget_probe(source,caps,false);
    }
}
static void effect_analysis_work_cut(const XrXirArtifact *source, uint64_t units, bool exact_analyze) {
    EffectMark mark = effect_mark(); XrXirCompileContext c = effect_owner_new(effect_caps());
    uint64_t baseline = effect_stats(&c).live_bytes; XrXirArtifact *copy = NULL;
    CHECK(effect_reproduce(source,&c,&copy) == XR_XIR_OK);
    uint64_t setup_work = effect_stats(&c).work, analyze_work = 0;
    if (exact_analyze) {
        XrXirEffects *summary=NULL;
        CHECK(xr_xir_compile_effects_analyze(copy,&summary)==XR_XIR_OK && summary);
        analyze_work=effect_stats(&c).work-setup_work;CHECK(analyze_work>1);
        xr_xir_compile_effects_free(summary);
    }
    xr_xir_compile_artifact_free(copy); effect_owner_free(&c,baseline); effect_mark_check(mark);
    XrCompileResourceLimits caps = effect_caps();
    CHECK(units <= caps.work - setup_work && analyze_work <= caps.work - setup_work);
    caps.work = setup_work + (exact_analyze ? analyze_work : units);
    if (exact_analyze) { effect_budget_probe(source,caps,true);--caps.work; }
    c = effect_owner_new(caps); baseline = effect_stats(&c).live_bytes; copy = NULL;
    CHECK(effect_reproduce(source,&c,&copy) == XR_XIR_OK);
    attempts = 0; XrXirEffects *summary = NULL;
    CHECK(xr_xir_compile_effects_analyze(copy,&summary) == XR_XIR_BUDGET && !summary);
    xr_xir_compile_artifact_free(copy); effect_owner_free(&c,baseline); effect_mark_check(mark);
}
#endif // XIR_EFFECT_ANALYSIS_OWNER_H
