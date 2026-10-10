/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_emit_checked_identity.h - Generated C transport identity assertions
 *
 * KEY CONCEPT:
 *   Generated units freeze their producer's reader and semantic identity.
 */
#ifndef XRAY_TEST_XIR_EMIT_CHECKED_IDENTITY_H
#define XRAY_TEST_XIR_EMIT_CHECKED_IDENTITY_H
#include "xir/xxir_checked.h"

static void emit_checked_identity_oracle(const char *text) {
    char expected[160];
    CHECK(strstr(text, "#include \"xir/xxir_checked.h\"\n"));
    int length = snprintf(expected, sizeof(expected),
        "_Static_assert(XR_XIR_CHECKED_SCHEMA == %uu, \"XIR Checked schema\");\n",
        XR_XIR_CHECKED_SCHEMA);
    CHECK(length > 0 && (size_t)length < sizeof(expected));
    CHECK(strstr(text, expected));
    length = snprintf(expected, sizeof(expected),
        "_Static_assert(XR_XIR_CHECKED_CONTRACT == %uu, \"XIR Checked contract\");\n",
        XR_XIR_CHECKED_CONTRACT);
    CHECK(length > 0 && (size_t)length < sizeof(expected));
    CHECK(strstr(text, expected));
}
#endif
