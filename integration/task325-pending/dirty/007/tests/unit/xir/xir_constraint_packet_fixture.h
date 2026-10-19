/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_constraint_packet_fixture.h - Three declaration owners of interface constraints
 */
#ifndef XIR_CONSTRAINT_PACKET_FIXTURE_H
#define XIR_CONSTRAINT_PACKET_FIXTURE_H
#include "xir_construction_fixture.h"
#include "xir/xxir_constraints.h"
#include "xir/xxir_nominal.h"

static XrXirStatus constraint_packet_fixture(const XrXirCompileContext *context,uint32_t markers,XrXirArtifact **output) {
    XrXirType parameter = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirInterfaceApplication requirement = {0,&parameter,1};
    XrXirConstraint unconstrained = {0}, constraint = {markers,&requirement,1};
    XrXirInterfaceDeclaration interfaces[] = {
        {{"alpha",5},{"Base",4},1,&unconstrained,1,NULL,0,NULL,0},
        {{"alpha",5},{"Needs",5},1,&constraint,1,NULL,0,NULL,0}};
    XrXirInterfaceTable interface_table = {interfaces,2};
    XrXirNominalDeclaration nominal = {
        {"alpha",5},{"Box",3},1,&constraint,1,NULL,0,XR_XIR_NOMINAL_STRUCT,NULL,0, 0, {0}};
    XrXirNominalTable nominal_table = {&nominal,1,NULL};
    XrXirTypes types = {NULL,0,&nominal_table,&interface_table};
    XrXirInstruction init = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirInstruction answer[] = {
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirBlock init_block = {0,1,0,0}, answer_block = {0,2,0,0};
    XrXirFunction functions[] = {
        {"entry",5,NULL,0,XR_XIR_I64,&answer_block,1,answer,2,NULL,0},
        {"init",4,NULL,0,XR_XIR_UNIT,&init_block,1,&init,1,NULL,0},
        {"generic",7,NULL,0,XR_XIR_UNIT,&init_block,1,&init,1,NULL,0}};
    XrXirGeneric generics[] = {{0},{0},{&constraint,1,NULL,0, NULL}};
    XrXirSourceModule source = {"alpha",5,NULL,0,1};
    XrXirFunctionIdentity identities[] = {{0},{0},{0}};
    XrXirDeclarations declarations = {&source,1,identities,NULL,0,NULL,0,0,0, NULL};
    XrXirModule module = {XR_XIR_BUILT,functions,3,&declarations,generics,&types,NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *artifact = NULL;
    XrXirStatus status=xir_fixture_check(context, &module, &artifact, NULL);
    if(status!=XR_XIR_OK){CHECK(!artifact);return status;}
    parameter = XR_XIR_UNIT; requirement.declaration = UINT32_MAX; constraint.markers = UINT32_MAX;
    status=xr_xir_compile_artifact_verify(artifact,NULL);
    if(status==XR_XIR_OK)*output=artifact;else xr_xir_compile_artifact_free(artifact);artifact=NULL;
    return status;
}
static void constraint_packet_owned(const XrXirArtifact *artifact, uint32_t markers) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    const XrXirConstraint *owners[] = {module->generics[2].constraints,
        module->types->nominals->declarations[0].constraints,
        module->types->interfaces->declarations[1].constraints};
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(owners[i] && owners[i]->markers == markers && owners[i]->interface_count == 1);
        CHECK(owners[i]->interfaces[0].declaration == 0 && owners[i]->interfaces[0].argument_count == 1);
        CHECK(owners[i]->interfaces[0].arguments[0] == XR_XIR_TYPE_PARAMETER_BASE);
    }
}
#endif // XIR_CONSTRAINT_PACKET_FIXTURE_H
