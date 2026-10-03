/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_method_access_cases.h - Method constraints preserve naming authority
 *
 * KEY CONCEPT:
 *   A method constraint can name a type without acquiring its construction rights.
 */
#ifndef XIR_GENERIC_METHOD_ACCESS_CASES_H
#define XIR_GENERIC_METHOD_ACCESS_CASES_H
#include "xir/xxir_nominal.h"
static void generic_method_access_cases(void) {
    for (unsigned attack = 0; attack < 8; ++attack) {
        XrXirInstruction init = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
        XrXirInstruction answer[] = {
            {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
            {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}
        };
        XrXirInstruction construct[] = {
            {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
            {XR_XIR_STRUCT_NEW,(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,{0,1},{0},0,{0}},
            {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}
        };
        uint32_t operand = 0;
        XrXirBlock init_block = {0,1,0,0}, answer_block = {0,2,0,0}, construct_block = {0,3,0,0};
        XrXirFunction functions[] = {
            {"alpha_init",10,NULL,0,XR_XIR_UNIT,&init_block,1,&init,1,NULL,0},
            {"other_init",10,NULL,0,XR_XIR_UNIT,&init_block,1,&init,1,NULL,0},
            {"entry",5,NULL,0,XR_XIR_I64,&answer_block,1,answer,2,NULL,0}
        };
        uint32_t dependency = 1;
        XrXirSourceModule modules[] = {{"alpha",5,&dependency,1,0},{"other",5,NULL,0,1}};
        XrXirFunctionIdentity identities[] = {{0},{1,0,0,0,0,0,XR_XIR_NON_MEMBER, 0, 0},{0}};
        XrXirDeclarations declarations = {modules,2,identities,NULL,0,NULL,0,0,2,NULL};
        XrXirNominalField field = {{"secret",6},XR_XIR_I64,XR_XIR_FIELD_PRIVATE};
        XrXirNominalDeclaration nominal = {{"other",5},{"Payload",7},1,NULL,0,&field,1,XR_XIR_NOMINAL_STRUCT,NULL,0, 0};
        XrXirNominalTable nominals = {&nominal,1,NULL};
        XrXirTypeNode nodes[4] = {0};
        nodes[0].kind = XR_XIR_TYPE_NOMINAL;
        nodes[1].kind = XR_XIR_TYPE_ARRAY; nodes[1].element = (XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
        nodes[2].kind = XR_XIR_TYPE_CALLABLE; nodes[2].result = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
        nodes[3].kind = XR_XIR_TYPE_CALLABLE; nodes[3].result = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
        nodes[3].parameter_span = 1;
        XrXirType argument = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2);
        XrXirInterfaceApplication application = {1,&argument,1};
        XrXirConstraint own = {0,&application,1}, outer = {0};
        XrXirInterfaceMethod method = {{"get",3},(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+3),0,1,&own};
        XrXirInterfaceDeclaration interfaces[] = {
            {{"alpha",5},{"Child",5},1,NULL,0,NULL,0,&method,1},
            {{"other",5},{"Base",4},1,&outer,1,NULL,0,NULL,0}
        };
        XrXirInterfaceTable table = {interfaces,2};
        XrXirTypes types = {nodes,4,&nominals,&table};
        XrXirModule module = {XR_XIR_BUILT,functions,3,&declarations,NULL,&types,NULL, XR_XIR_PROGRAM, NULL};
        if (attack == 1 || attack == 4 || attack == 5) {
            modules[0].dependencies = NULL; modules[0].dependency_count = 0;
        }
        if (attack == 2) interfaces[1].exported = 0;
        if (attack == 3) nominal.exported = 0;
        if (attack == 4 || attack == 5) interfaces[1].module = interfaces[0].module;
        if (attack == 5) {
            nominal.module = interfaces[0].module; nominal.exported = 0; interfaces[1].exported = 0;
        }
        if (attack == 6 || attack == 7) {
            functions[2].blocks = &construct_block; functions[2].instructions = construct;
            functions[2].instruction_count = 3; functions[2].operands = &operand; functions[2].operand_count = 1;
            if (attack == 6) field.flags = 0;
        }
        XrXirStatus expected = attack == 0 || attack == 5 || attack == 6 ? XR_XIR_OK : XR_XIR_BAD_TYPE;
        XrXirArtifact *checked = NULL;
        XrXirStatus status = xr_xir_check(&module,NULL,&checked,NULL);
        if (status != expected) fprintf(stderr,"generic method access %u: actual=%u expected=%u\n",attack,status,expected);
        CHECK(status == expected && (checked != NULL) == (expected == XR_XIR_OK));
        xr_xir_artifact_free(checked); CHECK(!live);
    }
}
#endif // XIR_GENERIC_METHOD_ACCESS_CASES_H
