/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_array_packets.c - Source packet generation without running unit gates
 */
#define XR_SOURCE_ARRAY_CUSTOM_MAIN
#include "test_xir_source_arrays.c"
int main(int argc, char **argv) {
    CHECK(argc == 4); (void)source_array_main;
    source_array_original(argv[1], argv[2], false); source_array_reordering(argv[3], false, false);
    puts("Source Array packets published after independent Source/query ownership checks"); return 0;
}
