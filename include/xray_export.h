/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_export.h - Public API visibility macros
 *
 * KEY CONCEPT:
 *   Defines XRAY_API for public headers in include/.
 *   Internal code uses src/base/xdefs.h which mirrors these definitions
 *   and adds XR_FUNC / XR_DATA for internal cross-module visibility.
 */

#ifndef XRAY_EXPORT_H
#define XRAY_EXPORT_H

#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef XRAY_BUILD_DLL
    #define XRAY_API __declspec(dllexport)
  #elif defined(XRAY_USE_DLL)
    #define XRAY_API __declspec(dllimport)
  #else
    #define XRAY_API extern
  #endif
#elif defined(__GNUC__) || defined(__clang__)
  #define XRAY_API __attribute__((visibility("default"))) extern
#else
  #define XRAY_API extern
#endif

#endif // XRAY_EXPORT_H
