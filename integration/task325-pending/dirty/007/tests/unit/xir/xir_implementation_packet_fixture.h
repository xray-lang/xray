/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XIR_IMPLEMENTATION_PACKET_FIXTURE_H
#define XIR_IMPLEMENTATION_PACKET_FIXTURE_H
#include "xir_construction_fixture.h"
#include "xir/xxir_implementation.h"
#include "xir/xxir_nominal.h"
static XrXirStatus implementation_packet_fixture(const XrXirCompileContext *context,XrXirArtifact **output) {
    XrXirType receiver = (XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE, argument = XR_XIR_I64;
    XrXirTypeNode nodes[2] = {0};
    nodes[0].kind = XR_XIR_TYPE_NOMINAL;
    nodes[1].kind = XR_XIR_TYPE_CALLABLE; nodes[1].flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    nodes[1].result = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE; nodes[1].parameter_span = 1;
    XrXirNominalDeclaration nominal = {{"alpha",5},{"Meter",5},1,NULL,0,NULL,0,XR_XIR_NOMINAL_STRUCT,NULL,0, 0, {0}};
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
    XrXirSourceModule module = {"alpha",5,NULL,0,1};
    XrXirFunctionIdentity identities[] = {{0},{0},{0,1,1,0,0,0,XR_XIR_READ_METHOD, 0, 0}};
    XrXirImplementationBinding binding = {{0,&argument,1},0,2};
    XrXirImplementation implementation = {0,{0,&argument,1},&binding,1};
    XrXirImplementationTable table = {&implementation,1};
    XrXirDeclarations declarations = {&module,1,identities,NULL,0,NULL,0,0,0,&table};
    XrXirModule built = {XR_XIR_BUILT,functions,3,&declarations,NULL,&types,NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL;
    XrXirStatus status=xir_fixture_check(context, &built, &checked, NULL);
    if(status!=XR_XIR_OK){CHECK(!checked);return status;}
    argument = XR_XIR_BOOL; binding.function = UINT32_MAX;
    memset(&implementation,0xcc,sizeof(implementation));
    status=xr_xir_compile_artifact_verify(checked,NULL);
    if(status==XR_XIR_OK)*output=checked;else xr_xir_compile_artifact_free(checked);checked=NULL;
    return status;
}
static void implementation_packet_owned(const XrXirArtifact *artifact) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    const XrXirImplementationTable *table = module->declarations->implementations;
    CHECK(table && table->count == 1 && table->records[0].nominal_declaration == 0);
    CHECK(table->records[0].interface.argument_count == 1 && table->records[0].interface.arguments[0] == XR_XIR_I64);
    CHECK(table->records[0].binding_count == 1 && table->records[0].bindings[0].function == 2);
    CHECK(table->records[0].bindings[0].requirement.arguments[0] == XR_XIR_I64);
    CHECK(module->declarations->functions[2].method_kind == XR_XIR_READ_METHOD);
    CHECK(module->functions[0].instructions[0].immediate == 41);
    CHECK(module->types->nodes[1].flags == 8u);
}
#endif // XIR_IMPLEMENTATION_PACKET_FIXTURE_H
