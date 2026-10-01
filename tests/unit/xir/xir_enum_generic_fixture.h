/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_enum_generic_fixture.h - Definition constraints and cross-module enum authority
 */
#ifndef XIR_ENUM_GENERIC_FIXTURE_H
#define XIR_ENUM_GENERIC_FIXTURE_H
#include "xir/xxir_nominal.h"
#include "xir/xxir_generic.h"
static XrXirArtifact *enum_generic_checked(unsigned mode) {
    XrXirType t = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE, concrete = XR_XIR_I64;
    XrXirConstraint nominal_constraint = {.markers = XR_XIR_CONSTRAINT_SENDABLE}, function_constraint = nominal_constraint;
    XrXirNominalVariant variants[] = {{{"None",4},0,0},{{"Some",4},0,1}};
    XrXirNominalField field = {{"value",5},t,0};
    XrXirNominalDeclaration declaration = {{"alpha",5},{"Choice",6},1,&nominal_constraint,1,&field,1,
        XR_XIR_NOMINAL_ENUM,variants,2, 0};
    XrXirNominalTable table = {&declaration,1,NULL};
    XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,1,{0,&t,1,NULL,0}},
        {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{0,&concrete,1,NULL,0}}};
    XrXirTypes types = {nodes,2,&table, NULL};
    XrXirInstruction init = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0, {0}};
    XrXirInstruction entry[] = {{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},42, {0}},
        {XR_XIR_CALL,(XrXirType)257,{0,1},{0},2, {0,1}},
        {XR_XIR_ENUM_GET,XR_XIR_I64,{1,0},{0},1, {0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0, {0}}};
    XrXirInstruction make[] = {{XR_XIR_ENUM_NEW,(XrXirType)256,{0,1},{0},1, {0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0, {0}}};
    uint32_t operand = 0, dependency = 0;
    XrXirBlock one = {0,1, 0, 0}, four = {0,4, 0, 0}, two = {0,2, 0, 0};
    XrXirType parameter = t;
    XrXirFunction functions[] = {
        {"initA",5,NULL,0,XR_XIR_UNIT,&one,1,&init,1,NULL,0},
        {"entry",5,NULL,0,XR_XIR_I64,&four,1,entry,4,&operand,1},
        {"make",4,&parameter,1,(XrXirType)256,&two,1,make,2,&operand,1},
        {"initB",5,NULL,0,XR_XIR_UNIT,&one,1,&init,1,NULL,0}};
    XrXirSourceModule modules[] = {{"alpha",5,NULL,0,0},{"beta",4,&dependency,1,3}};
    XrXirFunctionIdentity identities[] = {{0,0,0,0, 0, 0, XR_XIR_NON_MEMBER},{1,1,0,0, 0, 0, XR_XIR_NON_MEMBER},{0,1,0,0, 0, 0, XR_XIR_NON_MEMBER},{1,0,0,0, 0, 0, XR_XIR_NON_MEMBER}};
    XrXirDeclarations declarations = {modules,2,identities,NULL,0,NULL,0,1,1, NULL};
    XrXirGeneric generics[] = {{0},{NULL,0,&concrete,1, NULL},{&function_constraint,1,NULL,0, NULL},{0}};
    XrXirModule built = {XR_XIR_BUILT,functions,4,&declarations,generics,&types,NULL, XR_XIR_PROGRAM, NULL};
    if (mode == 1 || mode == 5) function_constraint.markers = 0;
    if (mode == 2) declaration.exported = 0;
    if (mode == 3) { modules[1].dependencies = NULL; modules[1].dependency_count = 0; }
    if (mode == 4) parameter = XR_XIR_STRING;
    if (mode == 5) {
        entry[1] = (XrXirInstruction) {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0, {0}};
        four.count = 2; functions[1].instruction_count = 2;
        functions[1].operands = NULL; functions[1].operand_count = 0;
        generics[1].arguments = NULL; generics[1].argument_count = 0;
    }
    XrXirArtifact *checked = NULL; XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_check(&built,NULL,&checked,&diagnostic);
    if (mode) CHECK(status != XR_XIR_OK && !checked);
    else { if (status != XR_XIR_OK) fprintf(stderr,"enum generic status %u function %u instruction %u\n",status,diagnostic.function,diagnostic.instruction); CHECK(status == XR_XIR_OK && checked); }
    return checked;
}
static void enum_generic_cases(void) {
    for (unsigned mode = 1; mode <= 5; ++mode) enum_generic_checked(mode);
    XrXirArtifact *checked = enum_generic_checked(0), *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL) == XR_XIR_OK); xr_xir_artifact_free(checked);
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK); xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_specialize(decoded,NULL,&closed,NULL) == XR_XIR_OK); xr_xir_artifact_free(decoded);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed,&target,NULL,&lowered,NULL) == XR_XIR_OK); xr_xir_artifact_free(closed);
    const XrXirTypes *types = xr_xir_artifact_module(lowered)->types;
    CHECK(types && types->count == 1 && types->nodes[0].nominal.field_count == 1);
    CHECK(types->nodes[0].nominal.fields[0] == XR_XIR_I64 && types->nodes[0].nominal.arguments[0] == XR_XIR_I64);
    xr_xir_artifact_free(lowered);
}
#endif // XIR_ENUM_GENERIC_FIXTURE_H
