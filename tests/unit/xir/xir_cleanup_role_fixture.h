/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cleanup_role_fixture.h - Nested cleanup bodies in two generic instances
 */
#ifndef XIR_CLEANUP_ROLE_FIXTURE_H
#define XIR_CLEANUP_ROLE_FIXTURE_H
static XrXirArtifact *cleanup_role_fixture(void) {
    XrXirInstruction done = {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    XrXirInstruction main_ops[] = {
        {XR_XIR_CALL, XR_XIR_UNIT, {0}, {0}, 2, {0, 1}},
        {XR_XIR_CALL, XR_XIR_UNIT, {0}, {0}, 2, {1, 1}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2}, {0}, 0, {0}}};
    XrXirBlock one = {0, 1, 0, 0}, four = {0, 4, 0, 0};
    XrXirBlock registration_blocks[] = {{0, 1, 0, 0}, {1, 1, 0, 1}};
    XrXirInstruction owner_ops[] = {
        {XR_XIR_CLEANUP_REGISTER, XR_XIR_UNIT, {0}, {1}, 3, {0, 1}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirInstruction cleanup_ops[] = {
        {XR_XIR_CLEANUP_REGISTER, XR_XIR_UNIT, {0}, {1}, 4, {0, 1}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirFunction functions[] = {
        {"init", 4, NULL, 0, XR_XIR_UNIT, &one, 1, &done, 1, NULL, 0},
        {"main", 4, NULL, 0, XR_XIR_I64, &four, 1, main_ops, 4, NULL, 0},
        {"owner", 5, NULL, 0, XR_XIR_UNIT, registration_blocks, 2, owner_ops, 2, NULL, 0},
        {"cleanup", 7, NULL, 0, XR_XIR_UNIT, registration_blocks, 2, cleanup_ops, 2, NULL, 0},
        {"nested", 6, NULL, 0, XR_XIR_UNIT, &one, 1, &done, 1, NULL, 0}};
    XrXirFunctionIdentity identities[5] = {{0}};
    identities[3].cleanup_owner = 3; identities[4].cleanup_owner = 4;
    XrXirSourceModule source = {"root", 4, NULL, 0, 0};
    XrXirDeclarations declarations = {&source, 1, identities, NULL, 0, NULL, 0, 0, 1};
    const XrXirConstraint constraint = {0}; const XrXirType arguments[] = {XR_XIR_I64, XR_XIR_U8};
    XrXirGeneric generics[5] = {{0}};
    generics[1] = (XrXirGeneric){NULL, 0, arguments, 2};
    for (uint32_t f = 2; f < 5; ++f) generics[f] = (XrXirGeneric){&constraint, 1, NULL, 0};
    const XrXirType argument = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    generics[2].arguments = &argument; generics[2].argument_count = 1;
    generics[3].arguments = &argument; generics[3].argument_count = 1;
    XrXirModule module = {XR_XIR_BUILT, functions, 5, &declarations, generics, NULL, NULL};
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_check(&module, NULL, &checked, NULL) == XR_XIR_OK);
    memset(identities, 0xCC, sizeof(identities));
    return checked;
}
#endif // XIR_CLEANUP_ROLE_FIXTURE_H
