/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_guard_combine.h - Redundant guard elimination + guard strengthening
 *
 * KEY CONCEPT:
 *   Eliminates duplicate XI_GUARD_TYPE instructions within each block.
 *   When the same (receiver, expected_type) pair is guarded multiple
 *   times, subsequent guards are redundant and can be removed.
 *
 *   Also performs guard strengthening: when multiple guards on the
 *   same receiver with the same type exist across dominator chains,
 *   the dominated guard can be eliminated.
 */

#ifndef XI_OPT_GUARD_COMBINE_H
#define XI_OPT_GUARD_COMBINE_H

#include "xi_pass.h"

XR_FUNC XiPassChange xi_opt_guard_combine(XiFunc *f);

#endif /* XI_OPT_GUARD_COMBINE_H */
