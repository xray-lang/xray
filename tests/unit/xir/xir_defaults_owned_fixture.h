/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_defaults_owned_fixture.h - Independent default declaration and error-edge models
 */
#ifndef XIR_DEFAULTS_OWNED_FIXTURE_H
#define XIR_DEFAULTS_OWNED_FIXTURE_H
#include "xir_construction_fixture.h"
#include "xir_error_fixture.h"
static XrXirArtifact *defaults_owned_fixture(bool invoke) {
    const XrXirInstruction helper[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirInstruction init={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    const XrXirInstruction answer={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    const XrXirInstruction caller[]={
        {XR_XIR_INVOKE_DEFAULT,XR_XIR_I64,{2,0},{1,2},0,{0}},
        {XR_XIR_INVOKE_RESULT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}},
        {XR_XIR_INVOKE_ERROR,XR_XIR_ERROR,{0},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{4},{0},0,{0}}};
    const XrXirBlock one={0,1,0,0},two={0,2,0,0},branches[]={{0,1,0,0},{1,2,0,0},{3,3,0,0}};
    const XrXirType parameter=XR_XIR_I64;
    const XrXirFunction functions[]={
        {"$default",8,NULL,0,XR_XIR_I64,&two,1,helper,2,NULL,0,
            .error_type=invoke?XR_XIR_CONSTRUCTED_TYPE_BASE:XR_XIR_NEVER},
        {"$init",5,NULL,0,XR_XIR_UNIT,&one,1,&init,1,NULL,0,.error_type=XR_XIR_NEVER},
        {"answer",6,&parameter,1,XR_XIR_I64,&one,1,&answer,1,NULL,0,.error_type=XR_XIR_NEVER},
        {"caller",6,NULL,0,XR_XIR_I64,branches,3,caller,6,NULL,0,.error_type=XR_XIR_NEVER}};
    const XrXirSourceModule source={"alpha",5,NULL,0,1,0,XR_XIR_MODULE_FRESH_STORAGE};
    const XrXirFunctionIdentity identities[4]={{0},{0},{.exported=1},{0}};
    const XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .root_module=invoke?0:UINT32_MAX,.entry_function=invoke?3:UINT32_MAX};
    const XrXirDefaultBinding binding={XR_XIR_DEFAULT_PARAMETER,2,0,0};
    const XrXirDefaultTable defaults={&binding,1};
    ErrorFixture error;error_fixture_init(&error,false);
    const XrXirModule built={.stage=XR_XIR_BUILT,.functions=functions,.function_count=invoke?4:3,
        .declarations=&declarations,.types=invoke?&error.types:NULL,
        .linkage_kind=invoke?XR_XIR_PROGRAM:XR_XIR_LIBRARY,.defaults=&defaults};
    XrXirArtifact *owned=NULL;
    CHECK(xir_fixture_check(library_context,&built,&owned,NULL)==XR_XIR_OK);
    return owned;
}
#endif // XIR_DEFAULTS_OWNED_FIXTURE_H
