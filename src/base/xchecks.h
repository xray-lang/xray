/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xchecks.h - Assertion and check macros (zero overhead in Release)
 *
 * KEY CONCEPT:
 *   Debug assertions (XR_DCHECK) compile to no-op in Release.
 *   Production checks (XR_CHECK) always execute.
 */

#ifndef XCHECKS_H
#define XCHECKS_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#if !defined(NDEBUG)
#define XR_DEBUG 1
#else
#define XR_DEBUG 0
#endif

#if XR_DEBUG
#define XR_DEBUG_ONLY(code) code
#else
#define XR_DEBUG_ONLY(code) ((void) 0)
#endif

#define XR_DCHECK_IS_ON() XR_DEBUG

// XR_CHECK - Always-on check (Debug + Release)
#define XR_CHECK(cond, msg)                                                                        \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "[FATAL] %s:%d: Check failed: %s\n", __FILE__, __LINE__, msg);         \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

// XR_CHECK_FMT - Always-on check with printf-style formatted message.
// Useful when the failure context needs to embed runtime values (an
// out-of-range index, the offending opcode name, etc.). The format
// string is concatenated with a fixed prefix at the call site, so the
// caller writes only the message portion. The GCC/Clang ##__VA_ARGS__
// extension is required so the macro works with zero variadic args.
#define XR_CHECK_FMT(cond, fmt, ...)                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "[FATAL] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);        \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

// XR_DCHECK - Debug-only check (removed in Release)
#if XR_DEBUG
#define XR_DCHECK(cond, msg) XR_CHECK(cond, msg)
#define XR_DCHECK_FMT(cond, fmt, ...) XR_CHECK_FMT(cond, fmt, ##__VA_ARGS__)
#else
#define XR_DCHECK(cond, msg) ((void) 0)
#define XR_DCHECK_FMT(cond, fmt, ...) ((void) 0)
#endif

// Comparison check macros
#if XR_DEBUG
#define XR_DCHECK_EQ(a, b, msg) XR_CHECK((a) == (b), msg)
#define XR_DCHECK_NE(a, b, msg) XR_CHECK((a) != (b), msg)
#define XR_DCHECK_LT(a, b, msg) XR_CHECK((a) < (b), msg)
#define XR_DCHECK_LE(a, b, msg) XR_CHECK((a) <= (b), msg)
#define XR_DCHECK_GT(a, b, msg) XR_CHECK((a) > (b), msg)
#define XR_DCHECK_GE(a, b, msg) XR_CHECK((a) >= (b), msg)
#else
#define XR_DCHECK_EQ(a, b, msg) ((void) 0)
#define XR_DCHECK_NE(a, b, msg) ((void) 0)
#define XR_DCHECK_LT(a, b, msg) ((void) 0)
#define XR_DCHECK_LE(a, b, msg) ((void) 0)
#define XR_DCHECK_GT(a, b, msg) ((void) 0)
#define XR_DCHECK_GE(a, b, msg) ((void) 0)
#endif

// Bounds check: 0 <= index < limit
#define XR_CHECK_BOUNDS(index, limit, msg)                                                         \
    XR_CHECK((int64_t) (index) >= 0 && (uint64_t) (index) < (uint64_t) (limit), msg)

#if XR_DEBUG
#define XR_DCHECK_BOUNDS(index, limit, msg) XR_CHECK_BOUNDS(index, limit, msg)
#else
#define XR_DCHECK_BOUNDS(index, limit, msg) ((void) 0)
#endif

// Compile-time assertions
#if defined(__cplusplus) && __cplusplus >= 201103L
#define XR_STATIC_ASSERT(expr, msg) static_assert(expr, msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#include <assert.h>
#define XR_STATIC_ASSERT(expr, msg) static_assert(expr, msg)
#else
#define XR_STATIC_ASSERT_CONCAT_(a, b) a##b
#define XR_STATIC_ASSERT_CONCAT(a, b) XR_STATIC_ASSERT_CONCAT_(a, b)
#define XR_STATIC_ASSERT(expr, msg)                                                                \
    typedef char XR_STATIC_ASSERT_CONCAT(static_assertion_failed_, __LINE__)[(expr) ? 1 : -1]
#endif

#define XR_STATIC_ASSERT_SIZE(type, expected_size)                                                 \
    XR_STATIC_ASSERT(sizeof(type) == (expected_size), "Type size mismatch")

#define XR_STATIC_ASSERT_SIZEOF(type1, type2)                                                      \
    XR_STATIC_ASSERT(sizeof(type1) == sizeof(type2), "Type size must match")

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define XR_STATIC_ASSERT_ALIGN(type, alignment)                                                    \
    XR_STATIC_ASSERT(_Alignof(type) == (alignment), "Type alignment mismatch")
#elif defined(__cplusplus) && __cplusplus >= 201103L
#define XR_STATIC_ASSERT_ALIGN(type, alignment)                                                    \
    XR_STATIC_ASSERT(alignof(type) == (alignment), "Type alignment mismatch")
#else
#define XR_STATIC_ASSERT_ALIGN(type, alignment)
#endif

// ========== Reductions Check for C Extensions ==========
//
// Optional macro for C functions to cooperate with the scheduler.
// Without this macro, sysmon + handoff provides preemption (10ms granularity).
// With this macro, microsecond-level scheduling with zero handoff overhead.
//
// Usage:
//   static XrCFuncResult my_func(XrayIsolate *X, ...) {
//       for (int i = 0; i < n; i++) {
//           do_work(i);
//           XR_CHECK_REDS(X, 1);
//       }
//       *result = xr_int(42);
//       return XR_CFUNC_DONE;
//   }

#define XR_CHECK_REDS(X, cost)                                                                     \
    do {                                                                                           \
        XrCoroutine *_c = xr_current_coro(X);                                                      \
        if (_c) {                                                                                  \
            _c->reductions -= (cost);                                                              \
            if (_c->reductions <= 0) {                                                             \
                _c->reductions = XR_CORO_REDUCTIONS;                                               \
                return XR_CFUNC_YIELD;                                                             \
            }                                                                                      \
        }                                                                                          \
    } while (0)

#define XR_CHECK_REDS_LOOP(X) XR_CHECK_REDS(X, 1)

// ========== VM Inline Cache Debug Assertions ==========
//
// Validates IC cache_index is within proto's instruction range,
// and detects stale cache slot reuse across different instructions.

#if XR_DEBUG

#define XR_VM_IC_ASSERT_INDEX(cache_index, proto)                                                  \
    XR_DCHECK((int) (cache_index) >= 0 && (int) (cache_index) < PROTO_CODE_COUNT(proto),           \
              "IC cache_index out of range")

// Record which instruction owns this cache slot (first use wins)
#define XR_VM_IC_METHOD_BIND(cache, expected_offset)                                               \
    do {                                                                                           \
        if ((cache)->debug_instruction_offset == -1) {                                             \
            (cache)->debug_instruction_offset = (expected_offset);                                 \
        } else if ((cache)->debug_instruction_offset != (expected_offset)) {                       \
            fprintf(stderr,                                                                        \
                    "[IC BUG] Method cache slot collision: "                                       \
                    "expected offset %d but got %d\n",                                             \
                    (cache)->debug_instruction_offset, (expected_offset));                         \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

#define XR_VM_IC_FIELD_BIND(cache, expected_offset)                                                \
    do {                                                                                           \
        if ((cache)->debug_instruction_offset == -1) {                                             \
            (cache)->debug_instruction_offset = (expected_offset);                                 \
        } else if ((cache)->debug_instruction_offset != (expected_offset)) {                       \
            fprintf(stderr,                                                                        \
                    "[IC BUG] Field cache slot collision: "                                        \
                    "expected offset %d but got %d\n",                                             \
                    (cache)->debug_instruction_offset, (expected_offset));                         \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

#else

#define XR_VM_IC_ASSERT_INDEX(cache_index, proto) ((void) 0)
#define XR_VM_IC_METHOD_BIND(cache, expected_offset) ((void) 0)
#define XR_VM_IC_FIELD_BIND(cache, expected_offset) ((void) 0)

#endif

// ========== API Boundary Defense (P2) ==========
//
// Validates embedder-provided arguments at public API entry points.
// Debug: assertion fires for immediate diagnosis.
// Release: graceful early return prevents undefined behavior.
//
// Usage:
//   void xray_foo(XrayIsolate *X, int n) {
//       xray_api_check(X != NULL, "xray_foo: NULL isolate");
//       xray_api_check(n >= 0, "xray_foo: negative n");
//       ...
//   }
//   XrArray* xray_bar(XrayIsolate *X) {
//       xray_api_checkr(X != NULL, "xray_bar: NULL isolate", NULL);
//       ...
//   }

// Void-returning API check
#define xray_api_check(cond, msg)                                                                  \
    do {                                                                                           \
        XR_DCHECK(cond, msg);                                                                      \
        if (XR_UNLIKELY(!(cond)))                                                                  \
            return;                                                                                \
    } while (0)

// Value-returning API check
#define xray_api_checkr(cond, msg, retval)                                                         \
    do {                                                                                           \
        XR_DCHECK(cond, msg);                                                                      \
        if (XR_UNLIKELY(!(cond)))                                                                  \
            return (retval);                                                                       \
    } while (0)

// Unreachable code marker
#ifdef __GNUC__
#define XR_UNREACHABLE() __builtin_unreachable()
#elif defined(_MSC_VER)
#define XR_UNREACHABLE() __assume(0)
#else
#define XR_UNREACHABLE() abort()
#endif

#endif  // XCHECKS_H
