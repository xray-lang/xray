/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_interface_access_cases.h - Canonical owners and nested naming authority
 */
#ifndef XIR_INTERFACE_ACCESS_CASES_H
#define XIR_INTERFACE_ACCESS_CASES_H
#include "xir/xxir_nominal.h"

static void interface_access_cases(void) {
    for (unsigned attack = 0; attack < 11; ++attack) {
        XrXirInstruction init = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
        XrXirInstruction answer[] = {
            {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
            {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}
        };
        XrXirBlock init_block = {0,1,0,0}, answer_block = {0,2,0,0};
        XrXirFunction functions[] = {
            {"alpha_init",10,NULL,0,XR_XIR_UNIT,&init_block,1,&init,1,NULL,0},
            {"other_init",10,NULL,0,XR_XIR_UNIT,&init_block,1,&init,1,NULL,0},
            {"entry",5,NULL,0,XR_XIR_I64,&answer_block,1,answer,2,NULL,0}
        };
        uint32_t dependency = 1;
        XrXirSourceModule modules[] = {{"alpha",5,&dependency,1,0},{"other",5,NULL,0,1}};
        XrXirFunctionIdentity identities[] = {{0},{1,0,0,0,0,0, XR_XIR_NON_MEMBER, 0, 0},{0}};
        XrXirDeclarations declarations = {modules,2,identities,NULL,0,NULL,0,0,2, NULL};
        XrXirNominalDeclaration nominal = {{"other",5},{"Payload",7},1,NULL,0,NULL,0,XR_XIR_NOMINAL_STRUCT,NULL,0, 0};
        XrXirNominalTable nominals = {&nominal,1,NULL};
        XrXirTypeNode nodes[3] = {0};
        nodes[0].kind = XR_XIR_TYPE_NOMINAL;
        nodes[1].kind = XR_XIR_TYPE_CALLABLE; nodes[1].result = (XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
        nodes[2].kind = XR_XIR_TYPE_CALLABLE; nodes[2].result = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + 1);
        XrXirType argument = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + 1);
        XrXirConstraint constraint = {0};
        XrXirInterfaceMethod method = {{"get",3},(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + 2),0,0,NULL};
        XrXirInterfaceApplication parent = {1,&argument,1};
        XrXirInterfaceDeclaration interfaces[] = {
            {{"alpha",5},{"Child",5},1,NULL,0,&parent,1,&method,1},
            {{"other",5},{"Base",4},1,&constraint,1,NULL,0,NULL,0}
        };
        XrXirInterfaceTable table = {interfaces,2};
        XrXirTypes types = {nodes,3,&nominals,&table};
        XrXirModule module = {XR_XIR_BUILT,functions,3,&declarations,NULL,&types,NULL, XR_XIR_PROGRAM, NULL};
        if (attack == 1) { module.declarations = NULL; types.nominals = NULL; types.count = 0; types.nodes = NULL;
            interfaces[0].methods = NULL; interfaces[0].method_count = 0; argument = XR_XIR_I64; }
        if (attack == 2) interfaces[0].module = (XrXirLiteral){"ghost",5};
        if (attack == 3 || attack == 8 || attack == 9 || attack == 10) {
            modules[0].dependencies = NULL; modules[0].dependency_count = 0;
        }
        if (attack == 4) interfaces[1].exported = 0;
        if (attack == 5 || attack == 6 || attack == 7 || attack == 8) nominal.exported = 0;
        if (attack == 6 || attack == 9) { interfaces[0].parents = NULL; interfaces[0].parent_count = 0; }
        if (attack == 7 || attack == 10) { interfaces[0].methods = NULL; interfaces[0].method_count = 0; }
        if (attack == 8) {
            nominal.module = interfaces[0].module; interfaces[1].module = interfaces[0].module;
            interfaces[1].exported = 0;
        }
        XrXirStatus expected = attack == 0 || attack == 8 ? XR_XIR_OK :
            attack == 1 || attack == 2 ? XR_XIR_BAD_STRUCTURE : XR_XIR_BAD_TYPE;
        XrXirArtifact *checked = NULL;
        XrXirStatus status = xr_xir_compile_check(interface_context_pointer_default(), &module, &checked, NULL);
        if (status != expected) fprintf(stderr,"interface access case %u: status=%u expected=%u\n",attack,status,expected);
        CHECK(status == expected && (checked != NULL) == (expected == XR_XIR_OK));
        xr_xir_compile_artifact_free(checked); CHECK(interface_live == stage_owner_count);
    }
}
#endif // XIR_INTERFACE_ACCESS_CASES_H
