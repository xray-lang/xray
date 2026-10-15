/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_effects_enum_fixture.h - Effect-owned enum graphs and cross-module generic constraints
 */
#ifndef XIR_EFFECTS_ENUM_FIXTURE_H
#define XIR_EFFECTS_ENUM_FIXTURE_H
#include "xir_construction_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_nominal.h"
static XrXirArtifact *effect_enum_ops_checked(const XrXirCompileContext *context, unsigned invalid, bool wrong_variant) {
    XrXirNominalVariant variants[] = {{{"Empty",5},0,0},{{"Some",4},0,2}};
    XrXirNominalField fields[] = {{{"value",5},XR_XIR_I64,0},{{"note",4},XR_XIR_STRING,0}};
    XrXirNominalDeclaration declaration = {.module={"alpha",5},.name={"Choice",6},.exported=1,
        .constraints=NULL,.parameter_count=0,.fields=fields,.field_count=2,.kind=XR_XIR_NOMINAL_ENUM,
        .variants=variants,.variant_count=2,.flags=0,.native={.native_id=0,.source_fingerprint={0}}};
    XrXirNominalTable table = {&declaration,1,NULL};
    XrXirTypeNode node = {0}; node.kind = XR_XIR_TYPE_NOMINAL;
    XrXirTypes types = {&node,1,&table, NULL};
    XrXirInstruction init = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0, {0}};
    XrXirInstruction entry[] = {
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},23, {0}},
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},0, {0}},
        {XR_XIR_ENUM_NEW,(XrXirType)256,{0,2},{0},1, {0}},
        {XR_XIR_ENUM_TAG,XR_XIR_I64,{2},{0},0, {0}},
        {XR_XIR_ENUM_GET,XR_XIR_I64,{2,0},{0},1, {0}},
        {XR_XIR_ENUM_GET,XR_XIR_STRING,{2,1},{0},1, {0}},
        {XR_XIR_ENUM_NEW,(XrXirType)256,{0},{0},0, {0}},
        {XR_XIR_ENUM_TAG,XR_XIR_I64,{6},{0},0, {0}},
        {XR_XIR_PRINT,XR_XIR_UNIT,{2,4},{0},0, {0}},
        {XR_XIR_SUSPEND,XR_XIR_UNIT,{0},{0},0, {0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0, {0}}};
    uint32_t operands[] = {0,1,3,4,7,5};
    XrXirInstruction main_ops[] = {{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0, {0}},{XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0, {0}}};
    XrXirBlock one = {0,1, 0, 0}, two = {0,2, 0, 0}, block = {0,11, 0, 0};
    XrXirFunction functions[] = {{"init",4,NULL,0,XR_XIR_UNIT,&one,1,&init,1,NULL,0},
        {"entry",5,NULL,0,XR_XIR_I64,&two,1,main_ops,2,NULL,0},
        {"make",4,NULL,0,(XrXirType)256,&block,1,entry,11,operands,6}};
    XrXirSourceModule source = {"alpha",5,NULL,0,0};
    XrXirFunctionIdentity identities[] = {{0,0,0,0, 0, 0, XR_XIR_NON_MEMBER, 0, 0, 0},{0,1,0,0, 0, 0, XR_XIR_NON_MEMBER, 0, 0, 0},{0,1,0,0, 0, 0, XR_XIR_NON_MEMBER, 0, 0, 0}};
    XrXirLiteral literal = {"constructed",11};
    XrXirDeclarations declarations = {&source,1,identities,NULL,0,&literal,1,0,1, NULL, NULL};
    XrXirModule built = {XR_XIR_BUILT,functions,3,&declarations,NULL,&types,NULL, XR_XIR_PROGRAM, NULL};
    if (wrong_variant) {
        entry[2].immediate = 0; entry[2].args[1] = 0; entry[8].args[0] = 0;
        operands[0] = 3; operands[1] = 4; operands[2] = 7; operands[3] = 5; functions[2].operand_count = 4;
    }
    if (invalid == 1) entry[2].immediate = 2;
    if (invalid == 2) entry[4].immediate = -1;
    if (invalid == 3) entry[4].args[1] = 2;
    if (invalid == 4) entry[5].type = XR_XIR_BOOL;
    if (invalid == 5) entry[2].args[1] = 1;
    if (invalid == 6) operands[0] = 1;
    if (invalid == 7) entry[3].args[1] = 1;
    if (invalid == 8) entry[4].args[0] = 5;
    if (invalid == 9) entry[2].targets[0] = 1;
    if (invalid == 10) entry[3].immediate = 1;
    if (invalid == 11) entry[5].immediate = 0;
    if (invalid == 12) entry[3].args[0] = 0;
    if (invalid == 13) entry[2].op = XR_XIR_STRUCT_NEW;
    XrXirArtifact *checked = NULL; XrXirStatus status = xir_fixture_check(context, &built,&checked,NULL);
    if (invalid) CHECK(status != XR_XIR_OK && !checked);
    else CHECK(status == XR_XIR_OK && checked);
    return checked;
}
static XrXirArtifact *effect_enum_ops_lowered(const XrXirCompileContext *context, bool wrong_variant) {
    XrXirArtifact *checked = effect_enum_ops_checked(context, 0, wrong_variant), *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL) == XR_XIR_OK); effect_artifact_free(checked);
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length,&decoded,NULL) == XR_XIR_OK); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL) == XR_XIR_OK); effect_artifact_free(decoded);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target,&lowered,NULL) == XR_XIR_OK); effect_artifact_free(closed);
    return lowered;
}

#include "xir/xxir_nominal.h"
#include "xir/xxir_generic.h"
static XrXirArtifact *effect_enum_generic_checked(const XrXirCompileContext *context, unsigned mode) {
    XrXirType t = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE, concrete = XR_XIR_I64;
    XrXirConstraint nominal_constraint = {.markers = XR_XIR_CONSTRAINT_SENDABLE}, function_constraint = nominal_constraint;
    XrXirNominalVariant variants[] = {{{"None",4},0,0},{{"Some",4},0,1}};
    XrXirNominalField field = {{"value",5},t,0};
    XrXirNominalDeclaration declaration = {.module={"alpha",5},.name={"Choice",6},.exported=1,
        .constraints=&nominal_constraint,.parameter_count=1,.fields=&field,.field_count=1,
        .kind=XR_XIR_NOMINAL_ENUM,.variants=variants,.variant_count=2,.flags=0,
        .native={.native_id=0,.source_fingerprint={0}}};
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
    XrXirFunctionIdentity identities[] = {{0,0,0,0, 0, 0, XR_XIR_NON_MEMBER, 0, 0, 0},{1,1,0,0, 0, 0, XR_XIR_NON_MEMBER, 0, 0, 0},{0,1,0,0, 0, 0, XR_XIR_NON_MEMBER, 0, 0, 0},{1,0,0,0, 0, 0, XR_XIR_NON_MEMBER, 0, 0, 0}};
    XrXirDeclarations declarations = {modules,2,identities,NULL,0,NULL,0,1,1, NULL, NULL};
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
    XrXirStatus status = xir_fixture_check(context, &built,&checked,&diagnostic);
    if (mode) CHECK(status != XR_XIR_OK && !checked);
    else { if (status != XR_XIR_OK) fprintf(stderr,"enum generic status %u function %u instruction %u\n",status,diagnostic.function,diagnostic.instruction); CHECK(status == XR_XIR_OK && checked); }
    return checked;
}
static void effect_enum_generic_cases(const XrXirCompileContext *context) {
    for (unsigned mode = 1; mode <= 5; ++mode) effect_enum_generic_checked(context, mode);
    XrXirArtifact *checked = effect_enum_generic_checked(context, 0), *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL) == XR_XIR_OK); effect_artifact_free(checked);
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length,&decoded,NULL) == XR_XIR_OK); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL) == XR_XIR_OK); effect_artifact_free(decoded);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target,&lowered,NULL) == XR_XIR_OK); effect_artifact_free(closed);
    const XrXirTypes *types = xr_xir_compile_artifact_module(lowered)->types;
    CHECK(types && types->count == 1 && types->nodes[0].nominal.field_count == 1);
    CHECK(types->nodes[0].nominal.fields[0] == XR_XIR_I64 && types->nodes[0].nominal.arguments[0] == XR_XIR_I64);
    effect_artifact_free(lowered);
}

#endif // XIR_EFFECTS_ENUM_FIXTURE_H
