/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_jit_profile.h - JIT/OSR profile input for Xi speculative passes
 *
 * Bridges VM inline-cache snapshots and optional block-frequency data
 * into the Xi IR layer before Tier-2 recompilation.  Keeps the stored
 * proto->xi_func slot-map stable by running only speculative passes
 * that insert guards or rewrite call ops in-place without removing
 * deopt-relevant values.
 */

#ifndef XI_JIT_PROFILE_H
#define XI_JIT_PROFILE_H

#include "xi.h"
#include "xi_pass.h"
#include <stdint.h>
#include <stdbool.h>

struct XrICFieldTable;
struct XrICMethodTable;

typedef struct XiJitProfileInput {
    struct XrICFieldTable *ic_fields;
    struct XrICMethodTable *ic_methods;
    /* Optional per-block execution counts, indexed by XiBlock.id.
     * Used for frequency annotation only (no block reorder on stored IR). */
    const uint32_t *block_freq;
    uint32_t block_freq_count;
    /* OSR hint: estimated remaining loop iterations at tier-up site. */
    uint32_t loop_trip_estimate;
} XiJitProfileInput;

/* Attach IC metadata and run speculative Xi passes suitable for JIT
 * recompile.  Mutates f in place; caller must ensure no concurrent
 * readers of f during this call.
 *
 * Returns true if any pass made a change. */
XR_FUNC bool xi_jit_apply_profile(XiFunc *f, const XiJitProfileInput *profile);

#endif /* XI_JIT_PROFILE_H */
