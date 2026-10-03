/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_compile_program_fixture.h - Owned compiler input with an independent language outcome
 */
#ifndef XIR_COMPILE_PROGRAM_FIXTURE_H
#define XIR_COMPILE_PROGRAM_FIXTURE_H
static XrXirArtifact *owner_lowered(const XrXirCompileContext *context) {
    XrXirInstruction init = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirInstruction main_code[] = {
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},42,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction text[] = {
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirBlock blocks[] = {{0,1,0,0},{0,2,0,0}};
    const XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,blocks,1,&init,1,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,blocks+1,1,main_code,2,NULL,0},
        {"text",4,NULL,0,XR_XIR_STRING,blocks+1,1,text,2,NULL,0}};
    const XrXirSourceModule source = {"root",4,NULL,0,0};
    const XrXirFunctionIdentity identities[] = {
        {0,0,0,0,0,0,XR_XIR_NON_MEMBER, 0, 0},
        {0,0,0,0,0,0,XR_XIR_NON_MEMBER, 0, 0},
        {0,1,0,0,0,0,XR_XIR_NON_MEMBER, 0, 0}};
    const XrXirLiteral literal = {"A\0\xe4\xb8\xad",5};
    const XrXirDeclarations declarations = {&source,1,identities,NULL,0,&literal,1,0,1,NULL};
    const XrXirModule module = {XR_XIR_BUILT,functions,3,&declarations,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_check(context,&module,&checked,NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); xr_xir_compile_artifact_free(closed);
    return lowered;
}
#endif // XIR_COMPILE_PROGRAM_FIXTURE_H
