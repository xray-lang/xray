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
 *     L1  xrt_range.h   - lazy Range value
 *     L2  xrt_coll.h    - Array, Map, Json, StringBuilder, Closure, index ops
 *     L2  xrt_path.h    - freestanding path helpers (parse/format use Json)
 *     L2  xrt_url.h     - freestanding URL helpers (parse/query use Json)
 *     L2  xrt_compress.h - freestanding checksum helpers
 *     L2  xrt_crypto.h  - freestanding crypto utilities
 *     L2  xrt_regex.h   - freestanding regex utilities
 *     L2  xrt_math.h    - freestanding math system helpers
 *     L2  xrt_time.h    - freestanding time query helpers
 *     L2  xrt_os.h      - freestanding OS query helpers
 *     L2  xrt_arith.h   - arithmetic, comparison, print
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
#include "xrt_arc.h"        // L1: bump alloc, xrt_str_alloc, xrt_str_concat
#include "xrt_range.h"      // L1: lazy Range value
#include "xrt_coll.h"       // L2: Array, Map, Json, StringBuilder, Closure, index ops
#include "xrt_path.h"       // L2: freestanding path helpers
#include "xrt_url.h"        // L2: freestanding URL helpers
#include "xrt_arith.h"      // L2: add/sub/mul/div/mod/neg, eq/lt/le, print
#include "xrt_encoding.h"   // L2: freestanding encoding string/Bytes helpers
#include "xrt_base64.h"     // L2: freestanding Base64 string/Bytes helpers
#include "xrt_compress.h"   // L2: freestanding checksum helpers
#include "xrt_crypto.h"     // L2: freestanding crypto helpers
#include "xrt_regex.h"      // L2: freestanding regex helpers
#include "xrt_math.h"       // L2: freestanding math helpers
#include "xrt_time.h"       // L2: freestanding time query helpers
#include "xrt_os.h"         // L2: freestanding OS query helpers
#include "xrt_method.h"     // L3: method_0/1/2, getprop, tostring, symbol IDs
#include "xrt_exception.h"  // L4: setjmp/longjmp exception handling
#include "xrt_defer.h"      // L4: function-scoped deferred cleanup (uses L1/L2/L4)
#include "xrt_class.h"      // L5: ObjHeader, TypeInfo, type table, obj_alloc

#endif  // XRT_H
