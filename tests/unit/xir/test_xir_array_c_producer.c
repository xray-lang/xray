/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_c_producer.c - Independent packet checking and portable C generation
 */
#define XR_ARRAY_PACKET_CUSTOM_MAIN
#include "test_xir_array_packet_vm.c"
int main(int argc, char **argv) {
    CHECK(argc == 3); (void)array_packet_vm_main;
    source_array_original_packet(argv[1], false); source_array_reordering_packet(argv[2], false);
    puts("Source Array C emitted from independently checked and lowered packets"); return 0;
}
