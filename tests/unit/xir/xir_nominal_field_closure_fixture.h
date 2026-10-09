/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_nominal_field_closure_fixture.h - Field-driven generic instance closure
 */
#ifndef XIR_NOMINAL_FIELD_CLOSURE_FIXTURE_H
#define XIR_NOMINAL_FIELD_CLOSURE_FIXTURE_H
#include "xir_construction_fixture.h"
#include "xir_checked_fixture.h"
static XrXirArtifact *nominal_field_closure_fixture(const XrXirCompileContext *context, unsigned mode) {
    CHECK(mode < 3);
    XrXirArtifact *base = checked_fixture(context), *checked = NULL;
    XrXirModule built = *xr_xir_compile_artifact_module(base); built.stage = XR_XIR_BUILT;
    XrXirType t = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE, i64 = XR_XIR_I64;
    XrXirConstraint constraint = {0};
    XrXirNominalField fields[] = {{{"value",5},t,0}, {{"inner",5},(XrXirType)256,0}};
    XrXirNominalDeclaration definitions[] = {
        {{"alpha",5},{"Box",3},1,&constraint,1,fields,1, XR_XIR_NOMINAL_STRUCT, NULL, 0, 0, {0}},
        {{"alpha",5},{"Outer",5},1,&constraint,1,fields+1,1, XR_XIR_NOMINAL_STRUCT, NULL, 0, 0, {0}}};
    XrXirNominalTable table = {definitions,2,NULL};
    XrXirType arguments[] = {(XrXirType)256,(XrXirType)257};
    XrXirTypeNode nodes[4] = {0};
    if (!mode) {
        nodes[0] = (XrXirTypeNode){XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,1,{0,&t,1,NULL,0}};
        nodes[1] = (XrXirTypeNode){XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{1,&i64,1,NULL,0}};
    } else {
        uint32_t at = mode == 2 ? 1 : 0;
        if (at) nodes[0] = (XrXirTypeNode){XR_XIR_TYPE_ARRAY,t,NULL,0,XR_XIR_UNIT,0,1,{0}};
        nodes[at] = (XrXirTypeNode){XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,1,
            {1,at ? arguments : &t,1,NULL,0}};
        nodes[at+1] = (XrXirTypeNode){XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,1,
            {0,arguments+at,1,NULL,0}};
        nodes[at+2] = (XrXirTypeNode){XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{1,&i64,1,NULL,0}};
        fields[1].type = (XrXirType)(257+at);
    }
    XrXirTypes types = {nodes,mode+2,&table, NULL}; built.types = &types;
    CheckedAtomicPool atomic_pool;checked_atomic_pool(&built,&atomic_pool);
    CHECK(!built.generics);
    CHECK(xir_fixture_check(context, &built, &checked, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(base);base=NULL; return checked;
}
#endif // XIR_NOMINAL_FIELD_CLOSURE_FIXTURE_H
