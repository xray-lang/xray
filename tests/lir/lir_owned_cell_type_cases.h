/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "lir/xlir_cell.h"

static void owned_cell_type_receivers(void) {
    XrLirModule *source = NULL, *target = NULL, *copy = NULL;
    OK(xr_lir_module_create(NULL, &source));
    OK(xr_lir_module_create(NULL, &target));
    OK(xr_lir_module_create(NULL, &copy));
    XrLirType callback = 0;
    XrLirCallableType signature = {.result=XR_LIR_I64};
    OK(xr_lir_callable_type(source, &signature, &callback));
    XrLirType cells[7] = {0};
    XrLirCellType shapes[] = {{XR_LIR_I64, 0}, {XR_LIR_STRING, 0}, {callback, 0},
        {XR_LIR_ARRAY, XR_LIR_I64}, {XR_LIR_ARRAY, XR_LIR_STRING},
        {XR_LIR_ARRAY, XR_LIR_U8}, {XR_LIR_ARRAY, XR_LIR_U64}};
    for (unsigned n = 0; n < 7; ++n) {
        OK(xr_lir_cell_type(source, shapes[n], &cells[n]));
        XrLirType duplicate = 0;
        OK(xr_lir_cell_type(source, shapes[n], &duplicate));
        CHECK(duplicate == cells[n]);
        for (unsigned p = 0; p < n; ++p) CHECK(cells[p] != cells[n]);
    }
    XrLirType distraction = 0;
    OK(xr_lir_fixed_array_type(target, XR_LIR_U8, 3, &distraction));
    XrLirType imported = 0;
    OK(xr_lir_import_type(target, source, cells[2], &imported));
    XrLirCellType received = {0};
    OK(xr_lir_cell_type_view(target, imported, &received));
    CHECK(received.payload != callback && received.array_element == 0);
    XrLirFunction root = test_function(source, "root", NULL, 0, XR_LIR_UNIT);
    XrLirBlock entry = test_block(source, root, NULL, 0);
    OK(xr_lir_set_entry(source, root, entry));
    test_return(source, entry, (XrLirValue){0});
    OK(xr_lir_module_finish(source, NULL));
    OK(xr_lir_copy_types(copy, source));
    XrLirModule *rebuilt = NULL;
    OK(xr_lir_rebuild(source, &rebuilt));
    xr_lir_module_destroy(source);
    OK(xr_lir_callable_type_view(target, received.payload, &signature));
    CHECK(signature.result == XR_LIR_I64 && signature.parameter_count == 0);
    for (unsigned n = 0; n < 7; ++n) {
        OK(xr_lir_cell_type_view(copy, cells[n], &received));
        CHECK(received.payload == shapes[n].payload && received.array_element == shapes[n].array_element);
        OK(xr_lir_cell_type_view(rebuilt, cells[n], &received));
        CHECK(received.payload == shapes[n].payload && received.array_element == shapes[n].array_element);
        XrLirTypeLayout layout = {17, 4, 5, false};
        CHECK(xr_lir_type_layout(rebuilt, cells[n], &layout) == XR_LIR_UNSUPPORTED && layout.size == 17);
    }
    OK(xr_lir_verify(rebuilt, NULL));
    xr_lir_module_destroy(rebuilt);
    xr_lir_module_destroy(copy);
    xr_lir_module_destroy(target);
}

static void owned_cell_type_rejections(void) {
    const XrLirCellType invalid[] = {{XR_LIR_PTR, 0}, {XR_LIR_I64_CELL, 0},
        {XR_LIR_ARRAY_CELL, XR_LIR_I64}, {XR_LIR_ARRAY_REF, XR_LIR_U8},
        {XR_LIR_STRING_CELL, 0}, {XR_LIR_STATE, 0}, {XR_LIR_STATE_REF, 0},
        {XR_LIR_VIEW_FRAME, 0}, {XR_LIR_VIEW_CELL, 0}, {XR_LIR_SLICE_I64, 0},
        {XR_LIR_ARRAY, 0}, {XR_LIR_ARRAY, XR_LIR_I1}, {XR_LIR_I64, XR_LIR_STRING}};
    for (unsigned n = 0; n < sizeof(invalid) / sizeof(*invalid); ++n) {
        XrLirModule *module = NULL;
        OK(xr_lir_module_create(NULL, &module));
        XrLirType output = XR_LIR_F64;
        CHECK(xr_lir_cell_type(module, invalid[n], &output) == XR_LIR_INVALID_ARGUMENT);
        CHECK(output == XR_LIR_F64 && module->type_count == 0);
        xr_lir_module_destroy(module);
    }
}
