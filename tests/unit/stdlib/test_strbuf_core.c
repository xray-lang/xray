/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_strbuf_core.c - Unit tests for runtime-neutral StringBuilder helpers
 */

#include "../test_framework.h"
#include "shared/xr_strbuf_core.h"
#include <string.h>

static void assert_slice(XrStrbufCoreSlice slice, const char *expected) {
    ASSERT_EQ_INT((int64_t) slice.len, (int64_t) strlen(expected));
    ASSERT(memcmp(slice.data, expected, slice.len) == 0);
}

TEST(strbuf_core_literal_slice_formats_scalar_fallbacks) {
    assert_slice(xr_strbuf_core_literal_slice(XR_STRBUF_CORE_LITERAL_BOOL, true), "true");
    assert_slice(xr_strbuf_core_literal_slice(XR_STRBUF_CORE_LITERAL_BOOL, false), "false");
    assert_slice(xr_strbuf_core_literal_slice(XR_STRBUF_CORE_LITERAL_NULL, false), "null");
    assert_slice(xr_strbuf_core_literal_slice(XR_STRBUF_CORE_LITERAL_OBJECT, false), "<object>");
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("StringBuilder Core - Append Literals");
RUN_TEST(strbuf_core_literal_slice_formats_scalar_fallbacks);

TEST_MAIN_END()
