/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_slp.h - Superword Level Parallelism (SLP) vectorization
 *
 * KEY CONCEPT:
 *   Identifies isomorphic operations on adjacent memory locations
 *   within a single basic block and packs them into vector ops:
 *     a[0] = b[0] + c[0]     →  vb = VEC_LOAD(b, VF=4)
 *     a[1] = b[1] + c[1]         vc = VEC_LOAD(c, VF=4)
 *     a[2] = b[2] + c[2]         va = VEC_ADD(vb, vc, VF=4)
 *     a[3] = b[3] + c[3]         VEC_STORE(a, va, VF=4)
 *
 *   The pass operates on straight-line code only (no control flow).
 *   Vector ops are lowered by the backend to SSE/AVX (x64),
 *   NEON (arm64), or scalar fallback.
 */

#ifndef XI_OPT_SLP_H
#define XI_OPT_SLP_H

#include "xi_pass.h"

XR_FUNC XiPassChange xi_opt_slp(XiFunc *f);

#endif /* XI_OPT_SLP_H */
