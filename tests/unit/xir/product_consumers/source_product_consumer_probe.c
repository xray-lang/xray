/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_consumer_probe.c - Public fixture admission diagnostics
 *
 * KEY CONCEPT:
 *   Unsupported old positive inputs stay failures with their fixed expectation.
 */
#include "source_product_consumer.h"
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 6 || argc > 7)
        return 2;
    const SourceProductConsumerFixture fixture = {
        argv[1], argv[2], argv[3], (int64_t)strtoll(argv[4], NULL, 10), NULL, NULL};
    return xr_source_product_consumer_main(&fixture, argc - 4, argv + 4);
}
