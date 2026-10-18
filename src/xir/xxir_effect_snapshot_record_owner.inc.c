/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_snapshot_record_owner.inc.c - Independent physical record lifetime
 *
 * KEY CONCEPT:
 *   References retain only pointer-free instruction encoding. Metadata and
 *   semantic proof owners can die independently of these exact physical bytes.
 */
#include "../base/xchecks.h"

static void effect_body_runs_release(EffectBodyRun *runs) {
    if (!runs) return;
    EffectBodyRunStorage *owner=((EffectBodyRunStorage *)runs)-1;
    XR_CHECK(owner->resources && owner->references,"physical record storage must be live");
    if (!--owner->references) xr_compile_resources_free(owner);
}

static XrXirStatus effect_body_runs_retain(const XrXirCompileContext *work,
    const EffectBodyFunction *function) {
    if (!xir_compile_context_valid(work) || !function || !function->runs ||
        !function->run_count || function->run_count>function->run_capacity)
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyRunStorage *owner=((EffectBodyRunStorage *)function->runs)-1;
    if (owner->resources!=work->resources || !owner->references ||
        function->run_capacity>owner->capacity) return XR_XIR_BAD_STRUCTURE;
    if (owner->references==UINT32_MAX) return XR_XIR_BUDGET;
    /* Prepay the acquisition and its unavoidable later release before
     * publishing any alias. Neither cumulative bytes nor work are refunded. */
    if (!xir_compile_work(work,5*sizeof(owner->references)+sizeof(void *)+3)) return XR_XIR_BUDGET;
    ++owner->references;return XR_XIR_OK;
}
