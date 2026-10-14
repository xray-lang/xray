/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_string_append_performance.c - Declared finite long-loop policy
 */
#define XR_P09_APPEND_PERFORMANCE_POLICY
#define XR_P09_APPEND_PROBE_ENTRY original_append_probe_main
#include "test_xir_p09_string_append_probe.c"
int main(int argc, char **argv) { return original_append_probe_main(argc, argv); }
