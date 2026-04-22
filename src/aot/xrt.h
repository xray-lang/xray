/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt.h - Umbrella header for AOT runtime
 *
 * KEY CONCEPT:
 *   Includes all layered sub-headers in dependency order:
 *     L0  xrt_value.h   - tags, boxing/unboxing, string helpers
 *     L1  xrt_arc.h     - ARC + bump allocator + str_alloc/str_concat
 *     L1  xrt_arith.h   - arithmetic, comparison, print
 *     L2  xrt_coll.h    - Array, Map, StringBuilder, Closure, index ops
 *     L3  xrt_method.h  - method dispatch, property access, toString
 *     L4  xrt_compat.h  - VM-native type aliases (XrValue, XR_TAG_*, etc.)
 *     L5  xrt_module.h  - module export tables, init sequencing
 *
 *   All runtime primitives are fully self-contained (no extern VM dependency).
 *   AOT-generated code includes only this header.
 *
 * RELATED MODULES:
 *   - xcgen.c: generates C code that includes this header
 */

#ifndef XRT_H
#define XRT_H

#include "xrt_value.h"   // L0: tags, boxing, unboxing, xrt_to_cstr
#include "xrt_arc.h"     // L1: ARC, bump alloc, xrt_str_alloc, xrt_str_concat
#include "xrt_arith.h"   // L1: add/sub/mul/div/mod/neg, eq/lt/le, print
#include "xrt_coll.h"    // L2: Array, Map, StringBuilder, Closure, index ops
#include "xrt_method.h"  // L3: method_0/1/2, getprop, tostring, symbol IDs
#include "xrt_compat.h"  // L4: XrValue alias, XR_TAG_*, XR_FROM_*, XrtContext
#include "xrt_module.h"  // L5: module export tables, init sequencing

#endif // XRT_H
