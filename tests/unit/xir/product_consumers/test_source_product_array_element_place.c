/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_array_element_place.c - Six exact Array element place cases
 *
 * KEY CONCEPT:
 *   The default Array fixture keeps its original six count inputs in independent
 *   backends after the source owners and caller Program reference die.
 */
#include "source_product_consumer.h"
#include "xir/xxir_checked.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Actual Checked identity");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22u && XR_XIR_CALL_ABI_VERSION == 28u &&
    XR_XIR_PROGRAM_ABI_VERSION == 29u, "Exact runtime identity");
#ifdef XR_ARRAY_ELEMENT_PLACE_NATIVE
XR_DATA const XrXirProgramSpec product_consumer_program;
XR_DATA const char product_consumer_c_sha[65];
#endif
int main(int argc, char **argv) {
    const SourceProductConsumerFixture fixture = {
        "array_element_place", XR_ARRAY_ELEMENT_PLACE_ROOT, XR_ARRAY_ELEMENT_PLACE_ROOT "/root.xr", 42,
#ifdef XR_ARRAY_ELEMENT_PLACE_NATIVE
        &product_consumer_program, product_consumer_c_sha
#else
        NULL, NULL
#endif
    };
    return xr_source_product_consumer_main(&fixture, argc, argv);
}
