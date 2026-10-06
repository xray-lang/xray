/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_consumer.h - Independent inputs for a shared test driver
 *
 * KEY CONCEPT:
 *   Only observer code is shared; every fixture owns its source and native code.
 */
#ifndef SOURCE_PRODUCT_CONSUMER_H
#define SOURCE_PRODUCT_CONSUMER_H
#include "xir/xxir_program.h"
typedef struct SourceProductConsumerFixture {
    const char *name, *root, *file;
    int64_t expected;
    const XrXirProgramSpec *native_program;
    const char *native_c_sha;
} SourceProductConsumerFixture;
XR_FUNC int xr_source_product_consumer_main(const SourceProductConsumerFixture *fixture, int argc, char **argv);
#endif // SOURCE_PRODUCT_CONSUMER_H
