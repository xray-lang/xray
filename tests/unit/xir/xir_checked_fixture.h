/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_checked_fixture.h - Declaration-rich packet ownership fixture
 *
 * KEY CONCEPT:
 *   Tests reconstruct Built views explicitly; the reader accepts only Checked.
 */
#ifndef XIR_CHECKED_FIXTURE_H
#define XIR_CHECKED_FIXTURE_H
#include "xir_program_fixture.h"
static XrXirArtifact *checked_fixture(void) {
    XrXirArtifact *lowered = program_fixture(0), *checked = NULL;
    XrXirModule built = *xr_xir_artifact_module(lowered);
    built.stage = XR_XIR_BUILT;
    XrXirFunction functions[9];
    memcpy(functions, built.functions, 8 * sizeof(*functions));
    const XrXirType parameters[] = {XR_XIR_I64, XR_XIR_STRING};
    const uint32_t operands[] = {0, 2};
    const XrXirInstruction ops[] = {
        {XR_XIR_COPY, XR_XIR_STRING, {1}, {0}, 0},
        {XR_XIR_PRINT, XR_XIR_UNIT, {0, 2}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0}
    };
    const XrXirBlock block = {0, 3};
    functions[8] = (XrXirFunction) {"args", 4, parameters, 2, XR_XIR_I64, &block, 1, ops, 3, operands, 2};
    XrXirFunctionIdentity identities[9];
    memcpy(identities, built.declarations->functions, 8 * sizeof(*identities));
    identities[8] = (XrXirFunctionIdentity) {0, 0};
    XrXirDeclarations declarations = *built.declarations;
    declarations.functions = identities;
    built.functions = functions; built.function_count = 9; built.declarations = &declarations;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(lowered);
    return checked;
}
#endif
