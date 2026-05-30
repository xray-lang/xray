/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_spec_const.h - Constant-on-branch specialization
 *
 * KEY CONCEPT:
 *   When PGO data shows a parameter is nearly always a specific
 *   constant value (e.g., 95% of calls pass mode="fast"), this
 *   pass inserts a type/value guard at the function entry and
 *   propagates the constant through the guarded path, enabling
 *   downstream constant folding and dead branch elimination.
 *
 *   Unlike full function cloning, this pass works within a single
 *   function body using conditional specialization (guard + const
 *   propagation on the hot path).
 */

#ifndef XI_OPT_SPEC_CONST_H
#define XI_OPT_SPEC_CONST_H

#include "xi_pass.h"

XR_FUNC XiPassChange xi_opt_spec_const(XiFunc *f);

#endif /* XI_OPT_SPEC_CONST_H */
