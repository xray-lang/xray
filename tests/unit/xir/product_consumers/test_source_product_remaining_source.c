/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_remaining_source.c - Original pending source execution
 *
 * KEY CONCEPT:
 *   The original source and fixed result survive parse and product death in
 *   VM, compiled C, and mixed execution through two independent instances.
 */
#include "source_product_consumer.h"
#include "xir/xxir_checked.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Actual Checked identity");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22u && XR_XIR_CALL_ABI_VERSION == 28u &&
    XR_XIR_PROGRAM_ABI_VERSION == 29u, "Exact runtime identity");
#ifdef XR_REMAINING_SOURCE_NATIVE
XR_DATA const XrXirProgramSpec product_consumer_program;
XR_DATA const char product_consumer_c_sha[65];
#endif
int main(int argc, char **argv) {
    const SourceProductConsumerFixture fixture = {
        XR_REMAINING_SOURCE_NAME, XR_REMAINING_SOURCE_ROOT, XR_REMAINING_SOURCE_ROOT "/root.xr", XR_REMAINING_SOURCE_EXPECTED,
#ifdef XR_REMAINING_SOURCE_NATIVE
        &product_consumer_program, product_consumer_c_sha
#else
        NULL, NULL
#endif
    };
    return xr_source_product_consumer_main(&fixture, argc, argv);
}
