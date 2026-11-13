/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static void test_raw_callable_contracts(void) {
    XrRbcCallableType types[] = {{.result={XR_RBC_CALLABLE_SCALAR, XR_POD_I64}, .flags=1},
        {.result={XR_RBC_CALLABLE_SCALAR, XR_POD_I64}}};
    uint32_t root_code[] = {fixed_call_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 0, 0),
        fixed_call_word(XR_RBC_MOV, 2, 0, 0, 0), fixed_call_word(XR_RBC_CALL, 1, 0, 0, 2),
        fixed_call_word(XR_RBC_RET, 1, 0, 0, 0)};
    uint32_t leaf_code[] = {fixed_call_word(XR_RBC_LOADI, 0, 0, 0, 42),
        fixed_call_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 0, 0)};
    uint32_t helper_code[] = {fixed_call_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 0, 0),
        fixed_call_word(XR_RBC_RET, 1, 0, 0, 0)};
    XrRbcRegisterKind root_kinds[] = {XR_RBC_FUNCTION, XR_RBC_CALL_WINDOW, XR_RBC_CALL_WINDOW};
    XrRbcRegisterKind helper_kinds[] = {XR_RBC_FUNCTION, XR_RBC_CALL_WINDOW};
    uint32_t root_signatures[] = {1,0,0}, helper_signatures[] = {2,0};
    XrRbcAggregateAccess reference = {.operation=XR_RBC_FUNCTION_REFERENCE, .callee=1, .slot=1};
    XrRbcAggregateAccess returned = {.operation=XR_RBC_FUNCTION_RETURN, .slot=1, .count=1};
    XrRbcAggregateAccess call = {.operation=XR_RBC_FUNCTION_CALL, .slot=2, .window=1};
    XrRbcFunction functions[] = {
        {.code=root_code, .code_count=4, .register_count=3, .register_kinds=root_kinds, .register_kind_count=3,
            .register_callables=root_signatures, .register_callable_count=3,
            .aggregate_accesses=&reference, .aggregate_access_count=1},
        {.code=leaf_code, .code_count=2, .register_count=1, .callable=1,
            .aggregate_accesses=&returned, .aggregate_access_count=1},
        {.code=helper_code, .code_count=2, .register_count=2, .parameter_count=1,
            .register_kinds=helper_kinds, .register_kind_count=2,
            .register_callables=helper_signatures, .register_callable_count=2,
            .aggregate_accesses=&call, .aggregate_access_count=1}
    };
    XrRbcModule module = {.functions=functions, .function_count=3, .callable_types=types, .callable_type_count=2};
    uint64_t bits = 0;
    CHECK(execute(&module, 0, NULL, 0, &bits) == XR_RBC_OK && bits == 42);
    reference.slot = root_signatures[0] = 2;
    CHECK(execute(&module, 0, NULL, 0, &bits) == XR_RBC_OK && bits == 42);
    reference.slot = root_signatures[0] = 1;
    types[0].flags = 0; types[1].flags = 1;
    reject_prepare(&module, 0, NULL, 0);
    reference.slot = root_signatures[0] = 2;
    reject_prepare(&module, 0, NULL, 0);
}
