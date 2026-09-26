/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_capture_fixture.h - Generic capture prefixes and nested resumable closures
 *
 * KEY CONCEPT:
 *   Captured values cross Checked serialization before any runtime owner exists.
 */
#ifndef XIR_CAPTURE_FIXTURE_H
#define XIR_CAPTURE_FIXTURE_H
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
static XrXirArtifact *capture_checked(void) {
    XrXirType fn = (XrXirType) 256, t = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
    XrXirCallableParameter input = {XR_XIR_STRING,0};
    XrXirCallableSignature signature = {&input,1,XR_XIR_STRING,0,0};
    XrXirCallableTypes types = {&signature,1};
    XrXirInstruction init[] = {{XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0}};
    XrXirInstruction root[] = {{XR_XIR_CONST_I64,XR_XIR_I64,{0},{0},17},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0}};
    XrXirInstruction make[] = {{XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},0},
        {XR_XIR_FUNCTION_REF,fn,{0,1},{0,1},5},
        {XR_XIR_FUNCTION_REF,fn,{1,1},{0},3},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0}};
    XrXirInstruction wrap[] = {{XR_XIR_SUSPEND,XR_XIR_UNIT,{0},{0},0},
        {XR_XIR_CALL_INDIRECT,XR_XIR_STRING,{0,1},{0},0},
        {XR_XIR_CONCAT_STRING,XR_XIR_STRING,{3,1},{0},0},
        {XR_XIR_RETURN,XR_XIR_UNIT,{4},{0},0}};
    XrXirInstruction invoke[] = {{XR_XIR_CALL_INDIRECT,XR_XIR_STRING,{0,1},{0},0},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0}};
    XrXirInstruction target[] = {{XR_XIR_SUSPEND,XR_XIR_UNIT,{0},{0},0},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0}};
    XrXirType parameters[] = {fn,XR_XIR_STRING,t,XR_XIR_STRING}, concrete = XR_XIR_STRING;
    uint32_t captures[] = {0,1}, argument = 1, constraint = 0;
    XrXirBlock blocks[] = {{0,1},{0,2},{0,4}};
    XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,blocks,1,init,1,NULL,0},
        {"root",4,NULL,0,XR_XIR_I64,blocks+1,1,root,2,NULL,0},
        {"make",4,NULL,0,fn,blocks+2,1,make,4,captures,2},
        {"wrap",4,parameters,2,XR_XIR_STRING,blocks+2,1,wrap,4,&argument,1},
        {"invoke",6,parameters,2,XR_XIR_STRING,blocks+1,1,invoke,2,&argument,1},
        {"capture",7,parameters+2,2,t,blocks+1,1,target,2,NULL,0}
    };
    XrXirGeneric generics[] = {{0},{0},{NULL,0,&concrete,1},{0},{0},{&constraint,1,NULL,0}};
    XrXirSourceModule source = {"root",4,NULL,0,0};
    XrXirFunctionIdentity identities[] = {{0,0},{0,1},{0,1},{0,0},{0,1},{0,0}};
    XrXirLiteral literal = {"captured",8};
    XrXirDeclarations declarations = {&source,1,identities,NULL,0,&literal,1,0,1};
    XrXirModule built = {XR_XIR_BUILT,functions,6,&declarations,generics,&types};
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_check(&built,NULL,&checked,NULL) == XR_XIR_OK && checked);
    memset(make,0xcc,sizeof(make)); memset(captures,0xcc,sizeof(captures));
    return checked;
}
static XrXirArtifact *capture_fixture(void) {
    XrXirArtifact *checked = capture_checked(), *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length); xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_specialize(decoded,NULL,&closed,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    const XrXirModule *m = xr_xir_artifact_module(closed);
    CHECK(m->function_count == 6 && m->functions[2].instructions[1].immediate == 5);
    CHECK(m->functions[5].parameters[0] == XR_XIR_STRING && !m->generics);
    XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed,&target,NULL,&lowered,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed); return lowered;
}
#endif // XIR_CAPTURE_FIXTURE_H
