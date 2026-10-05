/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * fixtures.h - Borrowed programs with independent nominal and interface authority
 */
#include "xir/xxir_nominal.h"
static XrXirStatus generic_built(const XrXirCompileContext *context, XrXirArtifact **output) {
    const XrXirType t = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
    const XrXirConstraint sendable = {.markers = XR_XIR_CONSTRAINT_SENDABLE};
    const XrXirType parameters[] = {XR_XIR_I64, XR_XIR_STRING};
    const uint32_t operands[] = {0, 1, 3};
    const XrXirType types[] = {XR_XIR_I64, XR_XIR_STRING, XR_XIR_STRING};
    const XrXirInstruction caller[] = {
        {XR_XIR_CALL, XR_XIR_I64, {0, 1}, {0}, 1, {0, 1}},
        {XR_XIR_CALL, XR_XIR_STRING, {1, 1}, {0}, 1, {1, 1}},
        {XR_XIR_CALL, XR_XIR_STRING, {2, 1}, {0}, 1, {2, 1}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {4}, {0}, 0, {0}}
    };
    const XrXirInstruction body[] = {{XR_XIR_COPY, t, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1}, {0}, 0, {0}}};
    const XrXirBlock blocks[] = {{0, 4, 0, 0}, {0, 2, 0, 0}};
    const XrXirFunction functions[] = {
        {"caller", 6, parameters, 2, XR_XIR_STRING, blocks, 1, caller, 4, operands, 3},
        {"id", 2, &t, 1, t, blocks + 1, 1, body, 2, NULL, 0}
    };
    const XrXirGeneric generics[] = {{NULL, 0, types, 3, NULL}, {&sendable, 1, NULL, 0, NULL}};
    const XrXirModule built = {XR_XIR_BUILT, functions, 2, NULL, generics, NULL, NULL, XR_XIR_PROGRAM, NULL};
    return xr_xir_compile_check(context, &built, output, NULL);
}
static XrXirStatus nominal_built(const XrXirCompileContext *context, XrXirArtifact **output) {
    XrXirType t = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
    XrXirType box = (XrXirType)256, outer = (XrXirType)257;
    XrXirConstraint constraint = {0};
    XrXirNominalField fields[] = {{{"value", 5}, t, 0}, {{"inner", 5}, box, 0}};
    XrXirNominalDeclaration definitions[] = {
        {{"alpha", 5}, {"Box", 3}, 1, &constraint, 1, fields, 1, XR_XIR_NOMINAL_STRUCT, NULL, 0, 0,{0}},
        {{"alpha", 5}, {"Outer", 5}, 1, &constraint, 1, fields + 1, 1, XR_XIR_NOMINAL_STRUCT, NULL, 0, 0,{0}}};
    XrXirNominalTable table = {definitions, 2, NULL};
    XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 1, {0, &t, 1, NULL, 0}},
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 1, {1, &t, 1, NULL, 0}}};
    XrXirTypes types = {nodes, 2, &table, NULL};
    XrXirInstruction entry[] = {
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 7, {0}},
        {XR_XIR_CALL, XR_XIR_I64, {0, 1}, {0}, 1, {0, 1}},
        {XR_XIR_CONST_INT, XR_XIR_U8, {0}, {0}, 9, {0}},
        {XR_XIR_CALL, XR_XIR_U8, {1, 1}, {0}, 1, {1, 1}},
        {XR_XIR_CONST_STRING, XR_XIR_STRING, {0}, {0}, 0, {0}},
        {XR_XIR_CALL, XR_XIR_STRING, {2, 1}, {0}, 1, {2, 1}},
        {XR_XIR_PRINT, XR_XIR_UNIT, {3, 3}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1}, {0}, 0, {0}}};
    XrXirInstruction body[] = {
        {XR_XIR_STRUCT_NEW, box, {0, 1}, {0}, 0, {0}},
        {XR_XIR_STRUCT_NEW, outer, {1, 1}, {0}, 0, {0}},
        {XR_XIR_STRUCT_GET, box, {2}, {0}, 0, {0}},
        {XR_XIR_STRUCT_GET, t, {3}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {4}, {0}, 0, {0}}};
    XrXirInstruction init = {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    XrXirInstruction escape[] = {
        {XR_XIR_CONST_STRING, XR_XIR_STRING, {0}, {0}, 0, {0}},
        {XR_XIR_CALL, XR_XIR_STRING, {0, 1}, {0}, 1, {0, 1}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1}, {0}, 0, {0}}};
    uint32_t operands[] = {0, 2, 4, 1, 3, 5}, body_operands[] = {0, 1}, escape_operand = 0;
    XrXirBlock blocks[] = {{0, 8, 0, 0}, {0, 5, 0, 0}, {0, 1, 0, 0}, {0, 3, 0, 0}};
    XrXirFunction functions[] = {
        {"root", 4, NULL, 0, XR_XIR_I64, blocks, 1, entry, 8, operands, 6},
        {"wrap", 4, &t, 1, t, blocks + 1, 1, body, 5, body_operands, 2},
        {"init", 4, NULL, 0, XR_XIR_UNIT, blocks + 2, 1, &init, 1, NULL, 0},
        {"escape", 6, NULL, 0, XR_XIR_STRING, blocks + 3, 1, escape, 3, &escape_operand, 1}};
    XrXirType arguments[] = {XR_XIR_I64, XR_XIR_U8, XR_XIR_STRING};
    XrXirGeneric generics[] = {{NULL, 0, arguments, 3, NULL}, {&constraint, 1, NULL, 0, NULL}, {0}, {NULL,0,arguments+2,1, NULL}};
    XrXirSourceModule source = {"alpha", 5, NULL, 0, 2};
    XrXirFunctionIdentity identities[4] = {{0}}; identities[3].exported = 1;
    XrXirLiteral literal = {"generic",7};
    XrXirDeclarations declarations = {&source, 1, identities, NULL, 0, &literal, 1, 0, 0, NULL};
    XrXirModule built = {XR_XIR_BUILT, functions, 4, &declarations, generics, &types, NULL, XR_XIR_PROGRAM, NULL};
    return xr_xir_compile_check(context, &built, output, NULL);
}

#include "xir/xxir_implementation.h"
static XrXirStatus implementation_built(const XrXirCompileContext *context, XrXirArtifact **output) {
    XrXirType receiver = (XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE, argument = XR_XIR_I64;
    XrXirTypeNode nodes[2] = {0};
    nodes[0].kind = XR_XIR_TYPE_NOMINAL;
    nodes[1].kind = XR_XIR_TYPE_CALLABLE;
    nodes[1].result = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE; nodes[1].parameter_span = 1;
    XrXirNominalDeclaration nominal = {{"alpha",5},{"Meter",5},1,NULL,0,NULL,0,XR_XIR_NOMINAL_STRUCT,NULL,0, 0,{0}};
    XrXirNominalTable nominals = {&nominal,1,NULL};
    XrXirConstraint constraint = {0};
    XrXirInterfaceMethod method = {{"measure",7},(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1),0,0,NULL};
    XrXirInterfaceDeclaration interface = {{"alpha",5},{"Measure",7},1,&constraint,1,NULL,0,&method,1};
    XrXirInterfaceTable interfaces = {&interface,1};
    XrXirTypes types = {nodes,2,&nominals,&interfaces};
    XrXirInstruction init = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirInstruction answer[] = {{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction measure[] = {{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    XrXirBlock one = {0,1,0,0}, two = {0,2,0,0};
    XrXirFunction functions[] = {
        {"entry",5,NULL,0,XR_XIR_I64,&two,1,answer,2,NULL,0},
        {"init",4,NULL,0,XR_XIR_UNIT,&one,1,&init,1,NULL,0},
        {"measure",7,&receiver,1,XR_XIR_I64,&two,1,measure,2,NULL,0}};
    XrXirSourceModule source_module = {"alpha",5,NULL,0,1};
    XrXirFunctionIdentity identities[] = {{0},{0},{0,1,1,0,0,0,XR_XIR_READ_METHOD, 0, 0}};
    XrXirImplementationBinding binding = {{0,&argument,1},0,2};
    XrXirImplementation implementation = {0,{0,&argument,1},&binding,1};
    XrXirImplementationTable table = {&implementation,1};
    XrXirDeclarations declarations = {&source_module,1,identities,NULL,0,NULL,0,0,0,&table};
    XrXirModule built = {XR_XIR_BUILT,functions,3,&declarations,NULL,&types,NULL, XR_XIR_PROGRAM, NULL};
    return xr_xir_compile_check(context,&built,output,NULL);
}

static XrXirStatus generic_error_built(const XrXirCompileContext *context, XrXirArtifact **output) {
    XrXirType t = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE, concrete = XR_XIR_I64;
    XrXirConstraint nominal_constraint = {.markers = XR_XIR_CONSTRAINT_SENDABLE}, function_constraint = nominal_constraint;
    XrXirNominalVariant variants[] = {{{"None",4},0,0},{{"Some",4},0,1}};
    XrXirNominalField field = {{"value",5},t,0};
    XrXirNominalDeclaration declaration = {{"alpha",5},{"Choice",6},1,&nominal_constraint,1,&field,1,
        XR_XIR_NOMINAL_ENUM,variants,2, 0,{0}};
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
    XrXirFunctionIdentity identities[] = {{0,0,0,0, 0, 0, XR_XIR_NON_MEMBER, 0, 0},{1,1,0,0, 0, 0, XR_XIR_NON_MEMBER, 0, 0},{0,1,0,0, 0, 0, XR_XIR_NON_MEMBER, 0, 0},{1,0,0,0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}};
    XrXirDeclarations declarations = {modules,2,identities,NULL,0,NULL,0,1,1, NULL};
    XrXirGeneric generics[] = {{0},{NULL,0,&concrete,1, NULL},{&function_constraint,1,NULL,0, NULL},{0}};
    XrXirModule built = {XR_XIR_BUILT,functions,4,&declarations,generics,&types,NULL, XR_XIR_PROGRAM, NULL};
    make[1] = (XrXirInstruction){XR_XIR_THROW,XR_XIR_UNIT,{1},{0},0,{0}};
    return xr_xir_compile_check(context,&built,output,NULL);
}
