/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_fixture.h - Polymorphic calls with repeated managed instances
 *
 * KEY CONCEPT:
 *   One checked definition serves concrete scalar and managed arguments.
 */
#ifndef XIR_GENERIC_FIXTURE_H
#define XIR_GENERIC_FIXTURE_H
#include "xir/xxir_generic.h"
static XrXirArtifact *generic_fixture(void) {
    const XrXirType t = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
    const uint32_t sendable = XR_XIR_CONSTRAINT_SENDABLE;
    const XrXirType parameters[] = {XR_XIR_I64, XR_XIR_STRING};
    const uint32_t operands[] = {0, 1, 3};
    const XrXirType types[] = {XR_XIR_I64, XR_XIR_STRING, XR_XIR_STRING};
    const XrXirInstruction caller[] = {
        {XR_XIR_CALL, XR_XIR_I64, {0, 1}, {0, 1}, 1},
        {XR_XIR_CALL, XR_XIR_STRING, {1, 1}, {1, 1}, 1},
        {XR_XIR_CALL, XR_XIR_STRING, {2, 1}, {2, 1}, 1},
        {XR_XIR_RETURN, XR_XIR_UNIT, {4}, {0}, 0}
    };
    const XrXirInstruction body[] = {{XR_XIR_COPY, t, {0}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1}, {0}, 0}};
    const XrXirBlock blocks[] = {{0, 4}, {0, 2}};
    const XrXirFunction functions[] = {
        {"caller", 6, parameters, 2, XR_XIR_STRING, blocks, 1, caller, 4, operands, 3},
        {"id", 2, &t, 1, t, blocks + 1, 1, body, 2, NULL, 0}
    };
    const XrXirGeneric generics[] = {{NULL, 0, types, 3}, {&sendable, 1, NULL, 0}};
    const XrXirModule built = {XR_XIR_BUILT, functions, 2, NULL, generics, NULL};
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    return checked;
}
#endif
