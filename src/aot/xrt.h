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
 *     L0  xrt_hash.h    - content hash primitives (shared with xi_cgen)
 *     L0  xrt_value.h   - tags, boxing/unboxing, source-level aliases, XrtContext
 *     L1  xrt_arc.h     - bump allocator, str_alloc/str_concat
 *     L1  xrt_arith.h   - arithmetic, comparison, print
 *     L2  xrt_coll.h    - Array, Map, StringBuilder, Closure, index ops
 *     L3  xrt_method.h  - method dispatch, property access, toString
 *
 *   All runtime primitives are fully self-contained (no extern VM dependency).
 *   AOT-generated code includes only this header.
 *
 * RELATED MODULES:
 *   - xi_cgen.c: generates C code that includes this header
 */

#ifndef XRT_H
#define XRT_H

#include "xrt_value.h"      // L0: tags, boxing, unboxing, source-level aliases, XrtContext
#include "xrt_path.h"       // L0: freestanding path string helpers
#include "xrt_encoding.h"   // L0: freestanding encoding predicates/counts
#include "xrt_arc.h"        // L1: bump alloc, xrt_str_alloc, xrt_str_concat
#include "xrt_base64.h"     // L1: freestanding Base64 string helpers
#include "xrt_range.h"      // L1: lazy Range value
#include "xrt_arith.h"      // L1: add/sub/mul/div/mod/neg, eq/lt/le, print
#include "xrt_coll.h"       // L2: Array, Map, StringBuilder, Closure, index ops
#include "xrt_method.h"     // L3: method_0/1/2, getprop, tostring, symbol IDs
#include "xrt_exception.h"  // L4: setjmp/longjmp exception handling
#include "xrt_defer.h"      // L4: function-scoped deferred cleanup (uses L1/L2/L4)
#include "xrt_class.h"      // L5: ObjHeader, TypeInfo, type table, obj_alloc

#endif  // XRT_H
