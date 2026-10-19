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
#include "xir_construction_fixture.h"
#include "xir_array_generic_fixture.h"
#include "xir_nominal_fixture.h"
static XrXirArtifact *nominal_generic_fixture(const XrXirCompileContext *context, bool expressions) {
    XrXirArtifact *base = array_generic_fixture(context), *checked = NULL;
    XrXirModule built = *xr_xir_compile_artifact_module(base); built.stage = XR_XIR_BUILT;
    XrXirFunction functions[4]; memcpy(functions, built.functions, 3 * sizeof(*functions));
    XrXirGeneric generics[4] = {0}; memcpy(generics, built.generics, 3 * sizeof(*generics));
    XrXirInstruction init = {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    XrXirBlock block = {0, 1, 0, 0};
    functions[3] = (XrXirFunction) {"init", 4, NULL, 0, XR_XIR_UNIT, &block, 1, &init, 1, NULL, 0};
    XrXirSourceModule module = {"alpha", 5, NULL, 0, 3};
    XrXirFunctionIdentity identities[4] = {{0, 1, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}, {0, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}, {0, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}, {0, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}};
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
    CHECK(xir_fixture_check(context, &built, &checked, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(base);base=NULL;
    memset(&f, 0xCC, sizeof(f)); memset(nodes, 0xCC, sizeof(nodes));
    return checked;
}
static inline XrXirArtifact *nominal_generic_lowered(const XrXirCompileContext *context) {
    XrXirArtifact *checked = nominal_generic_fixture(context, true), *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);closed=NULL;
    return lowered;
}
#endif // XIR_NOMINAL_GENERIC_FIXTURE_H
