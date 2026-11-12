/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_display_cases.h - Receiver-owned display qualification survives copies
 */
static void display_catalog_owners(void) {
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(NULL, &module));
    XrLirType nullable, unsigned_type, fixed, fixed_nullable;
    OK(xr_lir_nullable_type(module, XR_LIR_I64, &nullable));
    OK(xr_lir_nullable_type(module, XR_LIR_U64, &unsigned_type));
    OK(xr_lir_fixed_array_type(module, XR_LIR_U8, 17, &fixed));
    OK(xr_lir_nullable_type(module, fixed, &fixed_nullable));
    CHECK(xr_lir_display_type(module, nullable) && xr_lir_display_type(module, unsigned_type));
    CHECK(!xr_lir_display_type(module, fixed) && !xr_lir_display_type(module, fixed_nullable));
    OK(xr_lir_display_catalog_install(module, false, false));
    uint32_t role = 0;
    OK(xr_lir_display_operation(module, &role));
    XrLirFunction function = test_function(module, "display", NULL, 0, XR_LIR_STRING);
    XrLirBlock block = test_block(module, function, NULL, 0);
    OK(xr_lir_set_entry(module, function, block));
    XrLirValue value = {0}, text = {0};
    XrLirInstruction none = {.op=XR_LIR_NULLABLE_NONE, .type=nullable};
    OK(xr_lir_emit_value(module, block, &none, &value));
    XrLirInstruction display = {.op=XR_LIR_CALL_INTRINSIC, .type=XR_LIR_STRING,
        .intrinsic=role, .operands=&value, .operand_count=1};
    OK(xr_lir_emit_value(module, block, &display, &text));
    test_return(module, block, text);
    OK(xr_lir_module_finish(module, NULL));
    XrLirModule *copies[3] = {0};
    OK(xr_lir_rebuild(module, &copies[0]));
    OK(xr_lir_sccp(module, &copies[1]));
    OK(xr_lir_inline(module, &copies[2]));
    CHECK(copies[0]->intrinsics[0].operation == XR_LIR_INTRINSIC_DISPLAY);
    module->intrinsics[0].operation = 0;
    CHECK(xr_lir_verify(module, NULL) == XR_LIR_VERIFY_ERROR);
    xr_lir_module_destroy(module);
    for (uint32_t n = 0; n < 3; ++n) {
        OK(xr_lir_verify(copies[n], NULL));
        CHECK(xr_lir_display_type(copies[n], nullable));
        xr_lir_module_destroy(copies[n]);
    }
}
