/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_effect_analysis_owner.h - Owned compiler memory and physical failure ownership
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
    return (XrCompileResourceLimits){XR_COMPILE_MEMORY_ORDINARY,0,true};
}
static XrCompileResourceStats effect_stats(const XrXirCompileContext *c) {
    XrCompileResourceStats s = {0}; CHECK(xr_compile_resources_stats(c->resources, &s) == XR_COMPILE_RESOURCE_OK); return s;
}
static XrXirCompileContext effect_owner_new(XrCompileResourceLimits caps) {
    CHECK(caps.profile_allocations && (caps.memory_mode==XR_COMPILE_MEMORY_ORDINARY ?
        caps.memory_limit==0 : caps.memory_mode==XR_COMPILE_MEMORY_FINITE && caps.memory_limit>0));
    XrXirCompileContext c = {0};
    CHECK(xr_compile_resources_new(&caps, &c.resources) == XR_COMPILE_RESOURCE_OK);
    c.limits = xr_xir_compile_default_limits(); return c;
}
static void effect_owner_free(XrXirCompileContext *c, uint64_t baseline) {
    CHECK(effect_stats(c).held_bytes == baseline);
    xr_compile_resources_release(c->resources); *c = (XrXirCompileContext){0};
}
static void effect_case_begin(void) {
    CHECK(!live && !live_bytes && !effect_owned_summary);
    effect_context = effect_owner_new(effect_caps()); effect_baseline = effect_stats(&effect_context);
    effect_expected = effect_mark();
}
static void effect_case_end(void) {
    XrCompileResourceStats s = effect_stats(&effect_context);
    CHECK(!effect_owned_summary && s.held_bytes == effect_baseline.held_bytes);
    fprintf(stderr,"effect memory: allocated=%llu peak=%llu held=%llu allocations=%llu\n",
        (unsigned long long)s.allocated_bytes,(unsigned long long)s.peak_bytes,
        (unsigned long long)s.held_bytes,(unsigned long long)s.allocation_count);
    effect_owner_free(&effect_context, effect_baseline.held_bytes); CHECK(!live && !live_bytes);
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
/* Finite memory covers the complete probe lifetime and releases with owners. */
static void effect_memory_probe(const XrXirArtifact *source,uint64_t limit,bool exact) {
    EffectMark mark=effect_mark();
    XrCompileResourceLimits caps={XR_COMPILE_MEMORY_FINITE,limit,true};
    XrXirCompileContext c=effect_owner_new(caps);uint64_t baseline=effect_stats(&c).held_bytes;
    XrXirArtifact *copy=NULL;XrXirEffects *summary=NULL;
    XrXirStatus status=effect_reproduce(source,&c,&copy);
    if(status==XR_XIR_OK)status=xr_xir_compile_effects_analyze(copy,&summary);
    CHECK(status==(exact?XR_XIR_OK:XR_XIR_MEMORY_LIMIT));CHECK(exact?summary!=NULL:summary==NULL);
    xr_xir_compile_effects_free(summary);xr_xir_compile_artifact_free(copy);
    effect_owner_free(&c,baseline);effect_mark_check(mark);
}
static void effect_memory_boundaries(const XrXirArtifact *source) {
    EffectMark mark=effect_mark();XrXirCompileContext c=effect_owner_new(effect_caps());
    uint64_t baseline=effect_stats(&c).held_bytes;XrXirArtifact *copy=NULL;XrXirEffects *summary=NULL;
    CHECK(effect_reproduce(source,&c,&copy)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(copy,&summary)==XR_XIR_OK);
    XrCompileResourceStats measured=effect_stats(&c);CHECK(measured.peak_bytes>1);
    xr_xir_compile_effects_free(summary);xr_xir_compile_artifact_free(copy);
    effect_owner_free(&c,baseline);effect_mark_check(mark);
    effect_memory_probe(source,measured.peak_bytes,true);
    effect_memory_probe(source,measured.peak_bytes-1,false);
    effect_memory_probe(source,measured.peak_bytes+1,true);
    fprintf(stderr,"effect held memory exact/minus1/plus1=%llu\n",(unsigned long long)measured.peak_bytes);
}
static void effect_analysis_reuse(const XrXirArtifact *source) {
    EffectMark mark=effect_mark();XrXirCompileContext c=effect_owner_new(effect_caps());
    uint64_t baseline=effect_stats(&c).held_bytes;XrXirArtifact *copy=NULL;
    CHECK(effect_reproduce(source,&c,&copy)==XR_XIR_OK);
    uint64_t retained=effect_stats(&c).held_bytes;
    for(unsigned repeat=0;repeat<2;++repeat) {
        XrCompileResourceStats before=effect_stats(&c);XrXirEffects *summary=NULL;
        CHECK(xr_xir_compile_effects_analyze(copy,&summary)==XR_XIR_OK && summary);
        xr_xir_compile_effects_free(summary);
        XrCompileResourceStats after=effect_stats(&c);
        CHECK(after.held_bytes==retained && after.allocated_bytes>before.allocated_bytes);
    }
    xr_xir_compile_artifact_free(copy);effect_owner_free(&c,baseline);effect_mark_check(mark);
}
#endif // XIR_EFFECT_ANALYSIS_OWNER_H
