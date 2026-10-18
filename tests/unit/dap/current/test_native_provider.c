/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native_provider.c - Real native-only registration without unused VM entries
 */
#define main native_route_reference_main
#include "test_native_routes.c"
#undef main
int main(void) {
    for(uint32_t filters=0;filters<8;++filters) for(unsigned example=0;example<6;++example)
        native_route_case(5,filters,example);
    puts("Native-only actual Source registration and filters: PASS");return 0;
}
