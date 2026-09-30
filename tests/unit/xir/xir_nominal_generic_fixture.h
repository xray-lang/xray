/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_nominal_generic_fixture.h - Shared declaration and function substitutions
 *
 * KEY CONCEPT:
 *   Equal field and body types share nodes without sharing parameter contexts.
 */
#ifndef XIR_NOMINAL_GENERIC_FIXTURE_H
#define XIR_NOMINAL_GENERIC_FIXTURE_H
#include "xir_array_generic_fixture.h"
#include "xir_nominal_fixture.h"
static XrXirArtifact *nominal_generic_fixture(bool expressions) {
    XrXirArtifact *base = array_generic_fixture(), *checked = NULL;
    XrXirModule built = *xr_xir_artifact_module(base); built.stage = XR_XIR_BUILT;
    XrXirFunction functions[4]; memcpy(functions, built.functions, 3 * sizeof(*functions));
    XrXirGeneric generics[4] = {0}; memcpy(generics, built.generics, 3 * sizeof(*generics));
    XrXirInstruction init = {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    XrXirBlock block = {0, 1, 0, 0};
    functions[3] = (XrXirFunction) {"init", 4, NULL, 0, XR_XIR_UNIT, &block, 1, &init, 1, NULL, 0};
    XrXirSourceModule module = {"alpha", 5, NULL, 0, 3};
    XrXirFunctionIdentity identities[4] = {{0, 1, 0, 0, 0, 0, XR_XIR_NON_MEMBER}, {0, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER}, {0, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER}, {0, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER}};
    identities[1].nominal_owner = 1; identities[1].method_kind = XR_XIR_MEMBER_HELPER;
    XrXirDeclarations declarations = {&module, 1, identities, NULL, 0, NULL, 0, 0, 0, NULL};
    NominalFixture f; nominal_fixture(&f); f.table.count = 1;
    XrXirType arguments[] = {XR_XIR_I64, XR_XIR_U8};
    XrXirTypeNode nodes[] = {built.types->nodes[0],
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {0, arguments, 1, NULL, 0}},
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {0, arguments + 1, 1, NULL, 0}}};
    if (expressions) f.fields[0].type = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirTypes types = {nodes, expressions ? 3 : 1, &f.table, NULL};
    built.functions = functions; built.function_count = 4; built.generics = generics;
    built.declarations = &declarations; built.types = &types;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(base);
    memset(&f, 0xCC, sizeof(f)); memset(nodes, 0xCC, sizeof(nodes));
    return checked;
}
static inline XrXirArtifact *nominal_generic_lowered(void) {
    XrXirArtifact *checked = nominal_generic_fixture(true), *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    return lowered;
}
#endif // XIR_NOMINAL_GENERIC_FIXTURE_H
