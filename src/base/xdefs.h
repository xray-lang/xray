/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdefs.h - Common definitions: visibility, attributes, compiler hints
 *
 * KEY CONCEPT:
 *   This is the lowest-level header in xray's internal hierarchy.
 *   It defines visibility macros (XRAY_API, XR_FUNC, XR_DATA) and
 *   compiler attribute helpers. Every internal .h should include this.
 *
 * WHY THIS DESIGN:
 *   - Visibility system with three tiers:
 *   - XRAY_API: public API symbols (visible to embedders)
 *   - XR_FUNC: internal cross-module functions (static in amalgamated build)
 *   - XR_DATA: internal cross-module data (static in amalgamated build)
 *   - All other functions should be 'static' (file-private)
 *
 * RELATED MODULES:
 *   - xchecks.h: assertion macros (includes this header)
 *   - include/xray.h: public API (uses XRAY_API)
 */

#ifndef XDEFS_H
#define XDEFS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* === Compiler Detection ===  */

#if defined(__GNUC__) || defined(__clang__)
#define XR_GCC_COMPAT 1
#endif

/* === Visibility Macros ===
 *
 * Four visibility levels (from most visible to least):
 *
 *   XRAY_API    - Public API for embedders. Exported in shared library builds.
 *   XR_FUNC     - Internal cross-module function. Becomes 'static' in
 *                 amalgamated builds, allowing the compiler to inline freely.
 *   XR_DATA     - Internal cross-module data (same visibility as XR_FUNC).
 *   static      - File-private function. Default choice for new functions.
 *
 * Rule: if a function is NOT called from another .c file, it MUST be static.
 */

// Public API: visible to embedders
#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef XRAY_BUILD_DLL
    #define XRAY_API __declspec(dllexport)
  #elif defined(XRAY_USE_DLL)
    #define XRAY_API __declspec(dllimport)
  #else
    #define XRAY_API extern
  #endif
#elif defined(XR_GCC_COMPAT)
  #define XRAY_API __attribute__((visibility("default"))) extern
#else
  #define XRAY_API extern
#endif // Internal cross-module functions/data
#if defined(xray_amalg_c)
  // Amalgamated build: all internal symbols become static
  #define XR_FUNC   static
  #define XR_DATA   static
#else
  #define XR_FUNC   extern
  #define XR_DATA   extern
#endif // For function definitions (in .c files) that match XR_FUNC declarations
#if defined(xray_amalg_c)
  #define XR_FUNCDEF  static
  #define XR_DATADEF  static
#else
  #define XR_FUNCDEF
  #define XR_DATADEF
#endif // === Visibility Macros ===

/* === Function Attributes === */

#ifdef XR_GCC_COMPAT
  #define XR_NORET      __attribute__((noreturn))
  #define XR_AINLINE     inline __attribute__((always_inline))
  #define XR_NOINLINE   __attribute__((noinline))
  #define XR_UNUSED     __attribute__((unused))
  #define XR_LIKELY(x)   __builtin_expect(!!(x), 1)
  #define XR_UNLIKELY(x) __builtin_expect(!!(x), 0)
  #define XR_PRINTF_FMT(fmtarg, firstvararg) \
      __attribute__((format(printf, fmtarg, firstvararg)))
#elif defined(_MSC_VER)
  #define XR_NORET      __declspec(noreturn)
  #define XR_AINLINE     __forceinline
  #define XR_NOINLINE   __declspec(noinline)
  #define XR_UNUSED
  #define XR_LIKELY(x)   (x)
  #define XR_UNLIKELY(x) (x)
  #define XR_PRINTF_FMT(fmtarg, firstvararg)
#else
  #define XR_NORET
  #define XR_AINLINE     inline
  #define XR_NOINLINE
  #define XR_UNUSED
  #define XR_LIKELY(x)   (x)
  #define XR_UNLIKELY(x) (x)
  #define XR_PRINTF_FMT(fmtarg, firstvararg)
#endif // Combined: noreturn + visibility
#define XR_FUNC_NORET   XR_FUNC XR_NORET
#define XRAY_API_NORET  XRAY_API XR_NORET

/* === Function Attributes === */

/* === Alignment & Packing === */

#ifdef XR_GCC_COMPAT
  #define XR_ALIGN(n)   __attribute__((aligned(n)))
  #define XR_PACKED      __attribute__((packed))
#elif defined(_MSC_VER)
  #define XR_ALIGN(n)   __declspec(align(n))
  #define XR_PACKED
#else
  #define XR_ALIGN(n)
  #define XR_PACKED
#endif // === Alignment & Packing ===

/* === Basic Helpers === */

#define XR_UNUSED_VAR(x) ((void)(x))

// Array element count
#define XR_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

// Min/Max (safe: evaluate arguments exactly once)
#ifdef XR_GCC_COMPAT
  #define XR_MIN(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
  #define XR_MAX(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#else
  #define XR_MIN(a, b) ((a) < (b) ? (a) : (b))
  #define XR_MAX(a, b) ((a) > (b) ? (a) : (b))
#endif // === Basic Helpers ===

#endif // XDEFS_H
