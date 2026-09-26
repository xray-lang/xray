/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_integer_admission_cases.h - Reject forged integer capabilities
 *
 * KEY CONCEPT:
 *   A numeric opcode never supplies a missing type or operand proof.
 */
#ifndef XIR_INTEGER_ADMISSION_CASES_H
#define XIR_INTEGER_ADMISSION_CASES_H
#include "xir/xxir_generic.h"
static void integer_ir_rejections(void) {
    for (unsigned test = 0; test < 12; ++test) {
        XrXirType parameters[] = {XR_XIR_I8, XR_XIR_I8};
        XrXirInstruction ops[] = {{XR_XIR_ADD_INT, XR_XIR_I8, {0, 1}, {0}, 0},
            {XR_XIR_RETURN, XR_XIR_UNIT, {2}, {0}, 0}};
        XrXirBlock block = {0, 2};
        XrXirFunction function = {"integer", 7, parameters, 2, XR_XIR_I8, &block, 1, ops, 2, NULL, 0};
        XrXirModule module = {XR_XIR_BUILT, &function, 1, NULL, NULL, NULL};
        uint32_t constraints = 0;
        XrXirGeneric generic = {&constraints, 1, NULL, 0};
        switch (test) {
        case 0: parameters[1] = XR_XIR_U8; break;
        case 1: parameters[1] = XR_XIR_I16; break;
        case 2: parameters[0] = XR_XIR_BOOL; break;
        case 3: ops[0].op = XR_XIR_CONST_INT; ops[0].args[1] = 0; ops[0].immediate = 128; break;
        case 4: ops[0].op = XR_XIR_CONST_INT; ops[0].args[1] = 0; ops[0].immediate = -129; break;
        case 5: ops[0].op = XR_XIR_CONST_INT; ops[0].args[1] = 0; ops[0].type = XR_XIR_U8; ops[0].immediate = -1; break;
        case 6: ops[0].op = XR_XIR_SHL_INT; parameters[1] = XR_XIR_BOOL; break;
        case 7: ops[0].op = XR_XIR_CONVERT_NUMBER; ops[0].args[1] = 0; parameters[0] = XR_XIR_STRING; break;
        case 8: ops[0].op = XR_XIR_CONVERT_NUMBER; ops[0].args[1] = 0; ops[0].type = XR_XIR_BOOL; break;
        case 9: ops[0].op = XR_XIR_EQ_INT; ops[0].type = XR_XIR_BOOL; parameters[1] = XR_XIR_U8; break;
        case 10: parameters[0] = (XrXirType) 14; break;
        default: parameters[0] = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE; module.generics = &generic; break;
        }
        function.result = ops[0].type;
        XrXirArtifact *checked = NULL;
        CHECK(xr_xir_check(&module, NULL, &checked, NULL) == XR_XIR_BAD_TYPE);
        CHECK(!checked);
    }
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    const XrXirType types[] = {XR_XIR_I8, XR_XIR_I16, XR_XIR_I32, XR_XIR_I64, XR_XIR_U8, XR_XIR_U16, XR_XIR_U32, XR_XIR_U64};
    for (unsigned i = 0; i < 8; ++i) {
        for (unsigned context = XR_XIR_LAYOUT_STORAGE; context <= XR_XIR_LAYOUT_FRAME; ++context) {
            XrXirLayout layout;
            CHECK(xr_xir_layout(types[i], &target, (XrXirLayoutContext) context, &layout) == XR_XIR_OK);
            uint32_t size = context == XR_XIR_LAYOUT_STORAGE ? xr_xir_integer_bits(types[i]) / 8 :
                context == XR_XIR_LAYOUT_FRAME || context == XR_XIR_LAYOUT_SSA ? 8 : 16;
            CHECK(layout.size == size && layout.alignment == (size > 8 ? 8 : size));
        }
    }
    puts("Typed integer admission: 12 forged IR cases and all eight layouts passed");
}
#endif // XIR_INTEGER_ADMISSION_CASES_H
