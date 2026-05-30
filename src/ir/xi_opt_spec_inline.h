/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_spec_inline.h - Speculative inlining via IC metadata
 *
 * KEY CONCEPT:
 *   For monomorphic XI_CALL_METHOD sites where spec_narrow has already
 *   placed a type guard, convert the virtual call to a direct call
 *   (XI_CALL_METHOD_DIRECT).  This enables downstream inlining by
 *   the standard inline pass.
 *
 *   Mono case: guard_type(recv, Foo) + call_method(recv, "bar")
 *     → guard_type(recv, Foo) + call_method_direct(recv, Foo::bar)
 *
 *   Graceful degradation: without IC metadata (AOT / cold),
 *   the pass is a no-op.
 */

#ifndef XI_OPT_SPEC_INLINE_H
#define XI_OPT_SPEC_INLINE_H

#include "xi_pass.h"

XR_FUNC XiPassChange xi_opt_spec_inline(XiFunc *f);

#endif /* XI_OPT_SPEC_INLINE_H */
