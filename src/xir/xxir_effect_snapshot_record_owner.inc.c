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
