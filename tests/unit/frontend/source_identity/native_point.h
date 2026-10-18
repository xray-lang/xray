/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_point.h - Native layout witness shared with the manifest fixture
 */
#ifndef XR_TEST_ANALYZER_NATIVE_POINT_H
#define XR_TEST_ANALYZER_NATIVE_POINT_H
#include <stdint.h>
struct XrAnalyzerNativePoint {
    int64_t left;
    int64_t right;
};
#endif // XR_TEST_ANALYZER_NATIVE_POINT_H
