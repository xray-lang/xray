/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_callable_type_cases.h - Exact signature identity and owner retirement
 */
static void callable_type_owners(void) {
    XrLirModule *source = NULL, *target = NULL, *copy = NULL;
    OK(xr_lir_module_create(NULL, &source));
    OK(xr_lir_module_create(NULL, &target));
    OK(xr_lir_module_create(NULL, &copy));
    XrLirCallableParameter parameter = {.type=XR_LIR_I64, .mode=XR_PARAM_READ};
    XrLirCallableType shape = {.parameters=&parameter, .parameter_count=1, .result=XR_LIR_I64};
    XrLirType ordinary = 0, again = 0, restricted = 0, escaping = 0, nested = 0;
    OK(xr_lir_callable_type(source, &shape, &ordinary));
    OK(xr_lir_callable_type(source, &shape, &again));
    CHECK(again == ordinary);
    shape.flags = XR_LIR_CALLABLE_NO_SUSPEND;
    OK(xr_lir_callable_type(source, &shape, &restricted));
    CHECK(restricted != ordinary);
    shape.flags = 0;
    parameter.type = ordinary;
    parameter.escaping = true;
    OK(xr_lir_callable_type(source, &shape, &escaping));
    CHECK(escaping != ordinary);
    shape.result = restricted;
    OK(xr_lir_callable_type(source, &shape, &nested));
    parameter.type = XR_LIR_U64;
    parameter.escaping = false;
    XrLirCallableType view = {0};
    OK(xr_lir_callable_type_view(source, ordinary, &view));
    CHECK(view.parameters[0].type == XR_LIR_I64 && !view.parameters[0].escaping);
    XrLirType different = 0;
    shape.result = XR_LIR_U64;
    OK(xr_lir_callable_type(target, &shape, &different));
    CHECK(different == ordinary);
    XrLirType imported = 0;
    OK(xr_lir_import_type(target, source, nested, &imported));
    CHECK(imported != different);
    OK(xr_lir_callable_type_view(target, imported, &view));
    XrLirType received_parameter = view.parameters[0].type, received_result = view.result;
    CHECK(view.parameters[0].escaping);
    XrLirFunction f = test_function(source, "entry", NULL, 0, XR_LIR_UNIT);
    XrLirBlock block = test_block(source, f, NULL, 0);
    OK(xr_lir_set_entry(source, f, block));
    test_return(source, block, (XrLirValue){0});
    XrLirFunction receiver = test_function(source, "receiver", &nested, 1, XR_LIR_UNIT);
    XrLirBlock receiver_block = test_block(source, receiver, NULL, 0);
    OK(xr_lir_set_entry(source, receiver, receiver_block));
    test_return(source, receiver_block, (XrLirValue){0});
    OK(xr_lir_module_finish(source, NULL));
    OK(xr_lir_copy_types(copy, source));
    XrLirModule *rebuilt = NULL, *inlined = NULL, *propagated = NULL;
    OK(xr_lir_rebuild(source, &rebuilt));
    OK(xr_lir_inline(source, &inlined));
    OK(xr_lir_sccp(source, &propagated));
    xr_lir_module_destroy(source);
    XrLirModule *transforms[] = {rebuilt, inlined, propagated};
    for (unsigned i = 0; i < 3; ++i) {
        OK(xr_lir_verify(transforms[i], NULL));
        OK(xr_lir_callable_type_view(transforms[i], nested, &view));
        CHECK(view.parameters[0].type == ordinary && view.result == restricted);
        xr_lir_module_destroy(transforms[i]);
    }
    OK(xr_lir_callable_type_view(target, received_parameter, &view));
    CHECK(view.parameters[0].type == XR_LIR_I64 && !view.flags);
    OK(xr_lir_callable_type_view(target, received_result, &view));
    CHECK(view.flags == XR_LIR_CALLABLE_NO_SUSPEND);
    OK(xr_lir_callable_type_view(copy, nested, &view));
    CHECK(view.parameters[0].type == ordinary && view.result == restricted);
    XrLirTypeLayout layout = {.size=123};
    CHECK(xr_lir_type_layout(copy, nested, &layout) == XR_LIR_UNSUPPORTED && layout.size == 123);
    xr_lir_module_destroy(target);
    OK(xr_lir_callable_type_view(copy, ordinary, &view));
    CHECK(view.parameters[0].type == XR_LIR_I64);
    xr_lir_module_destroy(copy);
}

static void callable_type_rejections(void) {
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirCallableParameter parameter = {.type=XR_LIR_I64, .mode=XR_PARAM_READ};
        XrLirCallableType shape = {.parameters=&parameter, .parameter_count=1, .result=XR_LIR_I64};
        switch (attempt) {
        case 0: shape.parameters = NULL; break;
        case 1: shape.flags = 128; break;
        case 2: shape.flags = XR_LIR_CALLABLE_C_ABI; break;
        case 3: parameter.mode = (XrParamMode)99; break;
        case 4: parameter.type = XR_LIR_TYPE_COUNT; break;
        case 5: parameter.array_element = XR_LIR_I64; break;
        case 6: shape.result = XR_LIR_ARRAY; break;
        default: shape.throws = true; break;
        }
        XrLirType output = 0;
        XrLirStatus expected = attempt == 7 ? XR_LIR_UNSUPPORTED : XR_LIR_INVALID_ARGUMENT;
        CHECK(xr_lir_callable_type(m, &shape, &output) == expected && !output);
        xr_lir_module_destroy(m);
    }
}
