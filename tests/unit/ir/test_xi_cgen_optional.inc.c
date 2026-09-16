/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_cgen_optional.inc.c - Source-backed optional C emission regression
 */

static const char *g_optional_c_output = NULL;

TEST(cgen_optional_injection_storage) {
    XrType scalar_type = {
        .kind = XR_KIND_INT, .id = 1700, .scalar_rep = XR_NATIVE_I64, .frozen = true};
    XrType optional_type = scalar_type;
    optional_type.id = 1701;
    optional_type.is_nullable = true;
    XrType bool_type = {
        .kind = XR_KIND_BOOL, .id = 1702, .scalar_rep = XR_SCALAR_REP_NONE, .frozen = true};
    for (int scenario = 0; scenario < 9; ++scenario) {
        bool none = scenario == 0 || scenario == 5 || scenario == 7;
        XrType *return_type = scenario < 3   ? &optional_type
                              : scenario < 5 ? &scalar_type
                                             : &bool_type;
        XiFunc *ir = xi_func_new("optional_storage", return_type);
        XiBlock *entry = ir ? xi_block_new(ir) : NULL;
        TEST_REQUIRE(entry != NULL, "optional storage fixture must allocate");
        entry->sealed = true;
        XiValue *payload =
            none ? NULL
                 : xi_const_int(ir, entry, scenario == 2 || scenario == 4 ? -7 : 0, &scalar_type);
        XiValue *injection = xi_value_new(ir, entry, XI_SUM_INJECT, &optional_type, none ? 0 : 1);
        TEST_REQUIRE(injection != NULL && (none || payload != NULL),
                     "optional storage values must allocate");
        injection->aux_int = none ? 0 : 1;
        if (payload)
            injection->args[0] = payload;
        XiValue *result = injection;
        if (scenario >= 3) {
            result = xi_value_new(ir, entry, scenario < 5 ? XI_VARIANT_PROJECT : XI_VARIANT_TEST,
                                  return_type, 1);
            TEST_REQUIRE(result != NULL, "optional observation must allocate");
            result->args[0] = injection;
            result->aux_int = scenario < 5    ? xi_variant_pack_projection(1u, 0u)
                              : scenario >= 7 ? 1
                                              : 0;
        }
        xi_block_set_return(entry, result);
        bool had_error = false;
        char *code = generate_c_with_status(ir, "optional_storage", &had_error);
        TEST_REQUIRE(code && !had_error, "optional storage must generate verified C");
        const char *fn = find_static_function_definition(code, "optional_storage");
        const char *end = fn ? strstr(fn, "\n}\n") : NULL;
        TEST_REQUIRE(end != NULL, "optional storage function must be emitted");
        TEST_REQUIRE(contains_between(fn, end, none ? "XR_NULL_VAL" : "XR_FROM_INT("),
                     "None and Some must keep distinct runtime tags");
        if (g_optional_c_output) {
            char path[1024];
            int length = snprintf(path, sizeof(path), "%s.%d.c", g_optional_c_output, scenario);
            TEST_REQUIRE(length > 0 && (size_t) length < sizeof(path), "optional path must fit");
            FILE *output = fopen(path, "wb");
            size_t size = strlen(code);
            TEST_REQUIRE(output && fwrite(code, 1, size, output) == size && fclose(output) == 0,
                         "optional storage C output must be complete");
        }
        xr_free(code);
        test_func_free(ir);
    }
}

TEST(cgen_optional_injection_preserves_none_and_some) {
    const char *source = "fn scalar(present: bool, value: i64) -> i64? {\n"
                         "    if (present) { return value }\n"
                         "    return null\n"
                         "}\n"
                         "fn bytes(present: bool) -> Array<i64>? {\n"
                         "    if (present) { return [7, 9] }\n"
                         "    return null\n"
                         "}\n"
                         "print(scalar(false, 0) == null)\n"
                         "print(scalar(true, 0) == null)\n"
                         "print(scalar(true, -7) ?? 99)\n"
                         "print(bytes(false) == null)\n"
                         "print(bytes(true) == null)\n"
                         "fn cleanup_none() {\n"
                         "    var pending: Channel<string>? = null\n"
                         "    defer { print(pending == null) }\n"
                         "}\n"
                         "cleanup_none()\n";
    XiFunc *ir = compile_to_ir(source);
    TEST_REQUIRE(ir != NULL, "optional source must reach backend IR");
    TEST_REQUIRE(count_op_in_func(ir, XI_SUM_INJECT) >= 4,
                 "both scalar and managed optional injections must reach C emission");
    bool had_error = false;
    char *code = generate_c_with_status(ir, "optional_injection", &had_error);
    TEST_REQUIRE(code != NULL && !had_error, "optional source must generate verified C");
    TEST_REQUIRE(!contains(code, "({"), "optional C must not contain GNU statement expressions");
    if (g_optional_c_output) {
        FILE *output = fopen(g_optional_c_output, "wb");
        size_t length = strlen(code);
        TEST_REQUIRE(output != NULL, "optional generated C output must open");
        TEST_REQUIRE(fwrite(code, 1, length, output) == length && fclose(output) == 0,
                     "optional generated C output must be complete");
    }
    xr_free(code);
    test_func_free(ir);
}
