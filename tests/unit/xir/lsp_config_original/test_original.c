/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
/* The generated translation unit retains the original independent test bodies
 * byte for byte. The full original executable also owns the server-factory test;
 * this entry point makes no claim about that separately required test. */

int main(void) {
    xr_test_suppress_dialogs();
    RUN_TEST(ignore_add_pattern);
    RUN_TEST(ignore_glob_pattern);
    RUN_TEST(ignore_should_ignore_hidden);
    RUN_TEST(ignore_exact_match);
    RUN_TEST(ignore_glob_match);
    RUN_TEST(ignore_multiple_patterns);
    RUN_TEST(ignore_capacity_growth);
    RUN_TEST(ignore_null_safety);
    RUN_TEST(toml_load_basic);
    RUN_TEST(toml_load_ignore_array);
    RUN_TEST(toml_load_inlay_hints);
    RUN_TEST(toml_load_missing_file);
    RUN_TEST(toml_load_no_lsp_section);
    RUN_TEST(toml_load_partial_config);
    RUN_TEST(toml_null_safety);
    printf("Original independent config helpers: %d executed, %d failed; server defaults NOT_RUN\n",
        tests_passed, tests_failed);
    return tests_failed || tests_passed != 15 ? 1 : 0;
}
