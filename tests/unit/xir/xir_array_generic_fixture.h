/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_generic_fixture.h - Ordinary generic Array operations
 *
 * KEY CONCEPT:
 *   Shared abstract nodes retain their separate declaration substitution owner.
 */
#ifndef XIR_ARRAY_GENERIC_FIXTURE_H
#define XIR_ARRAY_GENERIC_FIXTURE_H
static XrXirArtifact *array_generic_fixture(const XrXirCompileContext *context) {
    XrXirType t = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE, array = (XrXirType) 256;
    XrXirTypeNode node = {XR_XIR_TYPE_ARRAY, t, NULL, 0, XR_XIR_UNIT, 0, 1, {0}};
    XrXirTypes types = {&node, 1, NULL, NULL};
    XrXirInstruction entry[] = {
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 7, {0}},
        {XR_XIR_CONST_INT, XR_XIR_U8, {0}, {0}, 9, {0}},
        {XR_XIR_CALL, XR_XIR_I64, {0, 1}, {0}, 1, {0, 1}},
        {XR_XIR_CALL, XR_XIR_U8, {1, 1}, {0}, 2, {1, 1}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2}, {0}, 0, {0}}};
    XrXirInstruction body[] = {
        {XR_XIR_ARRAY_NEW, array, {0, 1}, {0}, 0, {0}},
        {XR_XIR_LOCAL_NEW, array, {1}, {0}, 0, {0}},
        {XR_XIR_ARRAY_PUSH, XR_XIR_UNIT, {2, 0}, {0}, 0, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}},
        {XR_XIR_ARRAY_GET, t, {2, 4}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {5}, {0}, 0, {0}}};
    uint32_t entry_values[] = {0, 1}, value = 0;
    XrXirConstraint constraints[] = {{0}, {.markers = XR_XIR_CONSTRAINT_SENDABLE}};
    XrXirType arguments[] = {XR_XIR_I64, XR_XIR_U8};
    XrXirBlock blocks[] = {{0, 5, 0, 0}, {0, 6, 0, 0}};
    XrXirFunction functions[] = {
        {"root", 4, NULL, 0, XR_XIR_I64, &blocks[0], 1, entry, 5, entry_values, 2},
        {"first", 5, &t, 1, t, &blocks[1], 1, body, 6, &value, 1},
        {"second", 6, &t, 1, t, &blocks[1], 1, body, 6, &value, 1}};
    XrXirGeneric generics[] = {{NULL, 0, arguments, 2, NULL}, {&constraints[0], 1, NULL, 0, NULL}, {&constraints[1], 1, NULL, 0, NULL}};
    XrXirModule module = {XR_XIR_BUILT, functions, 3, NULL, generics, &types, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_compile_check(context, &module, &checked, NULL) == XR_XIR_OK);
    return checked;
}
#endif // XIR_ARRAY_GENERIC_FIXTURE_H
