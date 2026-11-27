/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_test_session.h - Private synchronous file test execution
 */
#ifndef XNATIVE_TEST_SESSION_H
#define XNATIVE_TEST_SESSION_H
#include "xray_native_state.h"

/* Private output from the C emitter; callbacks use exact Unit signatures. */
typedef struct XrtNativeTestFile {
    const XrtSourceMap *sources;
    const XrtModuleLayoutSpec *state;
    XrtSynchronousStateRoot initializer;
    const XrtSynchronousStateRoot *actions;
    uint32_t action_count;
} XrtNativeTestFile;

#endif // XNATIVE_TEST_SESSION_H
