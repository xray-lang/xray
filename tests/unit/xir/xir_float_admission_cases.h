/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_float_admission_cases.h - Canonical floating type and value admission
 *
 * KEY CONCEPT:
 *   Forged metadata and noncanonical values cannot acquire numeric capabilities.
 */
#ifndef XIR_FLOAT_ADMISSION_CASES_H
#define XIR_FLOAT_ADMISSION_CASES_H
#include "xir/xxir_type_arena.h"
static void floating_ir_rejections(void) {
    for (unsigned test = 0; test < 17; ++test) {
        XrXirType types[] = {XR_XIR_F32, XR_XIR_F32};
        XrXirInstruction ops[] = {{XR_XIR_EQ_FLOAT, XR_XIR_BOOL, {0, 1}, {0}, 0, {0}},
            {XR_XIR_RETURN, XR_XIR_UNIT, {2}, {0}, 0, {0}}};
        XrXirBlock block = {0, 2, 0, 0};
        XrXirFunction function = {"floating", 8, types, 2, XR_XIR_BOOL, &block, 1, ops, 2, NULL, 0};
        XrXirModule module = {XR_XIR_BUILT, &function, 1, NULL, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
        XrXirConstraint constraint = {0}; XrXirGeneric generic = {&constraint, 1, NULL, 0, NULL};
        switch (test) {
        case 0: types[1] = XR_XIR_F64; break;
        case 1: types[0] = XR_XIR_I32; break;
        case 2: ops[0].op = XR_XIR_EQ_INT; break;
        case 3: ops[0].op = XR_XIR_NEG_FLOAT; ops[0].args[1] = 0; ops[0].type = XR_XIR_I32; break;
        case 4: ops[0].op = XR_XIR_CONST_FLOAT; ops[0].args[1] = 0; ops[0].type = XR_XIR_F32; ops[0].immediate = INT64_C(0x100000000); break;
        case 5: ops[0].op = XR_XIR_CONST_FLOAT; ops[0].args[1] = 0; ops[0].type = XR_XIR_F32; ops[0].immediate = INT64_C(0x7fc00001); break;
        case 6: ops[0].op = XR_XIR_CONST_FLOAT; ops[0].args[1] = 0; ops[0].type = XR_XIR_F64; ops[0].immediate = INT64_C(0x7ff0000000000001); break;
        case 7: ops[0].op = XR_XIR_CONVERT_NUMBER; ops[0].args[1] = 0; ops[0].type = XR_XIR_STRING; break;
        case 8: ops[0].op = XR_XIR_CONVERT_NUMBER; ops[0].args[1] = 0; ops[0].type = XR_XIR_F32; types[0] = XR_XIR_BOOL; break;
        case 9: ops[0].op = XR_XIR_OUTPUT; ops[0].type = XR_XIR_UNIT; ops[0].args[1] = 0;
            ops[0].immediate = 1; ops[1].args[0] = 0; types[0] = XR_XIR_ATOMIC_I64; break;
        case 10: ops[0].op = XR_XIR_EQ_FLOAT; types[0] = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE; module.generics = &generic; break;
        case 11: types[0] = (XrXirType) 15; break;
        case 12: ops[0].op = XR_XIR_ADD_FLOAT; ops[0].type = XR_XIR_F32; types[1] = XR_XIR_F64; break;
        case 13: ops[0].op = XR_XIR_SUB_FLOAT; ops[0].type = XR_XIR_I32; break;
        case 14: ops[0].op = XR_XIR_MUL_FLOAT; ops[0].type = XR_XIR_F32; types[0] = XR_XIR_I32; break;
        case 15: ops[0].op = XR_XIR_DIV_FLOAT; ops[0].type = XR_XIR_F32;
            types[0] = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE; module.generics = &generic; break;
        default: ops[0].op = XR_XIR_CONST_INT; ops[0].args[1] = 0; ops[0].type = XR_XIR_F32; break;
        }
        function.result = ops[0].type;
        XrXirArtifact *artifact = NULL;
        CHECK(xr_xir_compile_check(&scalar_owner.context, &module, &artifact, NULL) == XR_XIR_BAD_TYPE && !artifact);
    }
}
static void floating_value_admission(void) {
    const struct { XrXirType type; uint64_t bits; bool valid; } cases[] = {
        {XR_XIR_F32, 0, true}, {XR_XIR_F32, UINT64_C(0x80000000), true},
        {XR_XIR_F32, 1, true}, {XR_XIR_F32, UINT64_C(0x7f800000), true},
        {XR_XIR_F32, UINT64_C(0xff800000), true}, {XR_XIR_F32, UINT64_C(0x7fc00000), true},
        {XR_XIR_F32, UINT64_C(0x100000000), false}, {XR_XIR_F32, UINT64_C(0xffc00000), false},
        {XR_XIR_F32, UINT64_C(0x7f800001), false}, {XR_XIR_F64, UINT64_C(0x8000000000000000), true},
        {XR_XIR_F64, 1, true}, {XR_XIR_F64, UINT64_C(0xfff0000000000000), true},
        {XR_XIR_F64, UINT64_C(0x7ff8000000000000), true}, {XR_XIR_F64, UINT64_C(0xfff8000000000000), false},
        {XR_XIR_F64, UINT64_C(0x7ff0000000000001), false}
    };
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(4096, &domain) == XR_XIR_VALUE_OK);
    uint64_t baseline = xr_xir_domain_stats(domain).live_bytes;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        XrXirTypeNode node = {XR_XIR_TYPE_CELL,cases[i].type,NULL,0,XR_XIR_UNIT,0,0, {0}};
        XrXirTypes types = {&node,1, NULL, NULL};
        ScalarCompileOwner owner = {0};
        XrXirCompileLimits structural = xr_xir_compile_default_limits();
        structural.parameters = 16;
        /* Type copying charges actual byte work independently of metadata bytes. */
        scalar_compile_owner_new(&owner, (XrCompileResourceLimits) {4096, 4096, 16384}, structural);
        XrXirTypeArena *arena = NULL;
        CHECK(xr_xir_compile_type_arena_new(&owner.context,&types,&arena) == XR_XIR_VALUE_OK);
        XrXirValueAdmission admission = {arena,domain,NULL,NULL,16,0};
        XrXirValue value = floating_argument(cases[i].type, cases[i].bits), copy = {0}, cell = {0}, read = {0};
        CHECK(xr_xir_value_argument(&value, NULL, cases[i].type) == cases[i].valid);
        CHECK(xr_xir_value_copy(&value, &copy) == (cases[i].valid ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT));
        CHECK(xr_xir_cell_new(domain,arena,(XrXirType)256,&value,&admission,&cell) ==
            (cases[i].valid ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT));
        if (cases[i].valid) {
            CHECK(copy.type == value.type && copy.payload == value.payload && !copy.reserved);
            CHECK(xr_xir_value_argument(&cell,arena,(XrXirType)256));
            CHECK(xr_xir_cell_read(&cell, &read) == XR_XIR_VALUE_OK && read.type == value.type && read.payload == value.payload);
        } else CHECK(!copy.type && !copy.payload && !cell.type && !cell.payload);
        xr_xir_value_drop(&copy); xr_xir_value_drop(&cell); xr_xir_value_drop(&read);
        xr_xir_compile_type_arena_drop(arena);
        scalar_compile_owner_free(&owner);
        CHECK(xr_xir_domain_stats(domain).live_bytes == baseline);
    }
    xr_xir_domain_drop(domain);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    for (unsigned width = 0; width < 2; ++width) for (unsigned context = XR_XIR_LAYOUT_STORAGE; context <= XR_XIR_LAYOUT_FRAME; ++context) {
        XrXirType type = width ? XR_XIR_F64 : XR_XIR_F32;
        uint32_t size = context == XR_XIR_LAYOUT_STORAGE ? (width ? 8 : 4) :
            context == XR_XIR_LAYOUT_FRAME || context == XR_XIR_LAYOUT_SSA ? 8 : 16;
        XrXirLayout layout;
        CHECK(xr_xir_builtin_layout(type, &target, (XrXirLayoutContext) context, &layout) == XR_XIR_OK);
        CHECK(layout.size == size && layout.alignment == (size > 8 ? 8 : size));
    }
    floating_ir_rejections();
    puts("Floating admission: 17 forged IR cases, 15 canonical value/cell cases and both layouts passed");
}
#endif // XIR_FLOAT_ADMISSION_CASES_H
