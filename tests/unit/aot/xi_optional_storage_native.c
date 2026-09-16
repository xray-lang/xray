/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_optional_storage_native.c - Execute a generated optional scalar result
 */

#define main optional_storage_generated_main
#include XR_OPTIONAL_GENERATED_C
#undef main

int main(void) {
#if XR_OPTIONAL_SCENARIO >= 3
    int64_t expected = XR_OPTIONAL_SCENARIO == 4                                ? -7
                       : XR_OPTIONAL_SCENARIO == 5 || XR_OPTIONAL_SCENARIO == 8 ? 1
                                                                                : 0;
    return optional_storage_optional_storage_1(NULL) == expected ? 0 : 1;
#else
    XrValue result = optional_storage_optional_storage_1(NULL);
#if XR_OPTIONAL_SCENARIO == 0
    return XR_IS_NULL(result) ? 0 : 1;
#else
    int64_t expected = XR_OPTIONAL_SCENARIO == 1 ? 0 : -7;
    return XR_IS_INT(result) && XR_TO_INT(result) == expected ? 0 : 1;
#endif
#endif
}
