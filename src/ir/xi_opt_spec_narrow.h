/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_spec_narrow.h - Speculative type narrowing pass
 *
 * KEY CONCEPT:
 *   When a call site's IC metadata shows monomorphic receiver type,
 *   this pass inserts XI_GUARD_TYPE + re-types the guarded value so
 *   that downstream passes and lowering can use the narrow type for
 *   direct field access and devirtualization.
 *
 *   Requires XI_INV_IC_ATTACHED (from xi_ic_attach).
 *   In AOT mode or without IC data the pass is a no-op.
 */

#ifndef XI_OPT_SPEC_NARROW_H
#define XI_OPT_SPEC_NARROW_H

#include "xi_pass.h"

XR_FUNC XiPassChange xi_opt_spec_narrow(XiFunc *f);

#endif /* XI_OPT_SPEC_NARROW_H */
