/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "lir/xlir_range.h"
static void range_catalog_receivers(void) {
    for (unsigned n = 0; n < 5; ++n) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirType parameters[] = {XR_LIR_I64, XR_LIR_I64, XR_LIR_I64};
        XrLirIntrinsic entry = {901, parameters, 3, XR_LIR_UNIT, true, XR_LIR_RANGE_OP_CHECK};
        if (n == 1) parameters[2] = XR_LIR_U8;
        if (n == 2) entry.parameter_count = 2;
        if (n == 3) entry.result = XR_LIR_I64;
        if (n == 4) entry.scalar_sync = false;
        XrLirRegistry registry = {&entry, 1};
        XrLirStatus status = xr_lir_set_registry(m, &registry);
        if (n) {
            CHECK(status == XR_LIR_UNSUPPORTED);
        }
        else {
            OK(status);
            parameters[2] = XR_LIR_U8;
            uint32_t id = 0;
            OK(xr_lir_range_operation(m, &id)); CHECK(id == 901);
            XrLirFunction f = test_function(m, "range_check", NULL, 0, XR_LIR_UNIT);
            XrLirBlock block = test_block(m, f, NULL, 0);
            OK(xr_lir_set_entry(m, f, block));
            XrLirValue values[] = {test_constant(m, block, XR_LIR_I64, 0),
                test_constant(m, block, XR_LIR_I64, 1), test_constant(m, block, XR_LIR_I64, 3)};
            XrLirInstruction op = {.op=XR_LIR_CALL_INTRINSIC, .type=XR_LIR_UNIT,
                .intrinsic=id, .operands=values, .operand_count=3};
            OK(xr_lir_emit_effect(m, block, &op));
            test_return(m, block, (XrLirValue){0});
            OK(xr_lir_module_finish(m, NULL));
            XrLirIntrinsic received = {0};
            OK(xr_lir_intrinsic_view(m, id, &received));
            CHECK(xr_lir_range_intrinsic_valid(&received) && received.parameters[2] == XR_LIR_I64);
        }
        xr_lir_module_destroy(m);
    }
}
