/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_symbol.c - Unit tests for symbol table
 */

#include "../test_framework.h"
#include "runtime/symbol/xsymbol_table.h"
#include <string.h>

/* ========== Basic Operations ========== */

TEST(symbol_table_create_destroy) {
    XrSymbolTable *table = xr_symbol_table_create();
    ASSERT_NOT_NULL(table);
    ASSERT_EQ_INT(table->count, 0);
    ASSERT_EQ_INT(table->builtin_count, 0);
    xr_symbol_table_destroy(table);
}

TEST(symbol_register_single) {
    XrSymbolTable *table = xr_symbol_table_create();

    SymbolId id = xr_symbol_register_in_table(table, "test");
    ASSERT_TRUE(id != SYMBOL_INVALID);
    ASSERT_EQ_INT(table->count, 1);

    xr_symbol_table_destroy(table);
}

TEST(symbol_register_multiple) {
    XrSymbolTable *table = xr_symbol_table_create();

    SymbolId id1 = xr_symbol_register_in_table(table, "foo");
    SymbolId id2 = xr_symbol_register_in_table(table, "bar");
    SymbolId id3 = xr_symbol_register_in_table(table, "baz");

    ASSERT_TRUE(id1 != SYMBOL_INVALID);
    ASSERT_TRUE(id2 != SYMBOL_INVALID);
    ASSERT_TRUE(id3 != SYMBOL_INVALID);

    // IDs should be unique
    ASSERT_TRUE(id1 != id2);
    ASSERT_TRUE(id2 != id3);
    ASSERT_TRUE(id1 != id3);

    ASSERT_EQ_INT(table->count, 3);

    xr_symbol_table_destroy(table);
}

TEST(symbol_register_duplicate) {
    XrSymbolTable *table = xr_symbol_table_create();

    SymbolId id1 = xr_symbol_register_in_table(table, "same");
    SymbolId id2 = xr_symbol_register_in_table(table, "same");

    // Same name should return same ID
    ASSERT_EQ_INT(id1, id2);
    ASSERT_EQ_INT(table->count, 1);

    xr_symbol_table_destroy(table);
}

/* ========== Lookup Operations ========== */

TEST(symbol_lookup_exists) {
    XrSymbolTable *table = xr_symbol_table_create();

    SymbolId registered = xr_symbol_register_in_table(table, "mySymbol");
    SymbolId looked_up = xr_symbol_lookup_in_table(table, "mySymbol");

    ASSERT_EQ_INT(registered, looked_up);

    xr_symbol_table_destroy(table);
}

TEST(symbol_lookup_not_exists) {
    XrSymbolTable *table = xr_symbol_table_create();

    SymbolId id = xr_symbol_lookup_in_table(table, "nonexistent");
    ASSERT_EQ_INT(id, SYMBOL_INVALID);

    xr_symbol_table_destroy(table);
}

TEST(symbol_lookup_after_multiple) {
    XrSymbolTable *table = xr_symbol_table_create();

    SymbolId id_a = xr_symbol_register_in_table(table, "alpha");
    SymbolId id_b = xr_symbol_register_in_table(table, "beta");
    SymbolId id_c = xr_symbol_register_in_table(table, "gamma");

    ASSERT_EQ_INT(xr_symbol_lookup_in_table(table, "alpha"), id_a);
    ASSERT_EQ_INT(xr_symbol_lookup_in_table(table, "beta"), id_b);
    ASSERT_EQ_INT(xr_symbol_lookup_in_table(table, "gamma"), id_c);
    ASSERT_EQ_INT(xr_symbol_lookup_in_table(table, "delta"), SYMBOL_INVALID);

    xr_symbol_table_destroy(table);
}

/* ========== Get Name Operations ========== */

TEST(symbol_get_name) {
    XrSymbolTable *table = xr_symbol_table_create();

    SymbolId id = xr_symbol_register_in_table(table, "testName");
    const char *name = xr_symbol_get_name_in_table(table, id);

    ASSERT_NOT_NULL(name);
    ASSERT_STR_EQ(name, "testName");

    xr_symbol_table_destroy(table);
}

TEST(symbol_get_name_invalid_id) {
    XrSymbolTable *table = xr_symbol_table_create();

    const char *name = xr_symbol_get_name_in_table(table, SYMBOL_INVALID);
    ASSERT_TRUE(name == NULL);

    const char *name2 = xr_symbol_get_name_in_table(table, 999);
    ASSERT_TRUE(name2 == NULL);

    xr_symbol_table_destroy(table);
}

TEST(symbol_get_name_multiple) {
    XrSymbolTable *table = xr_symbol_table_create();

    SymbolId id1 = xr_symbol_register_in_table(table, "first");
    SymbolId id2 = xr_symbol_register_in_table(table, "second");
    SymbolId id3 = xr_symbol_register_in_table(table, "third");

    ASSERT_STR_EQ(xr_symbol_get_name_in_table(table, id1), "first");
    ASSERT_STR_EQ(xr_symbol_get_name_in_table(table, id2), "second");
    ASSERT_STR_EQ(xr_symbol_get_name_in_table(table, id3), "third");

    xr_symbol_table_destroy(table);
}

/* ========== Builtin Symbols ========== */

TEST(symbol_init_builtins) {
    XrSymbolTable *table = xr_symbol_table_create();

    bool success = xr_symbol_table_init_builtins(table);
    ASSERT_TRUE(success);
    ASSERT_TRUE(table->builtin_count > 0);
    ASSERT_EQ_INT(table->count, table->builtin_count);

    xr_symbol_table_destroy(table);
}

TEST(symbol_builtin_lookup) {
    XrSymbolTable *table = xr_symbol_table_create();
    xr_symbol_table_init_builtins(table);

    // Common method symbols should exist
    SymbolId has_id = xr_symbol_lookup_in_table(table, "has");
    SymbolId length_id = xr_symbol_lookup_in_table(table, "length");
    SymbolId push_id = xr_symbol_lookup_in_table(table, "push");

    ASSERT_TRUE(has_id != SYMBOL_INVALID);
    ASSERT_TRUE(length_id != SYMBOL_INVALID);
    ASSERT_TRUE(push_id != SYMBOL_INVALID);

    xr_symbol_table_destroy(table);
}

TEST(symbol_builtin_names) {
    XrSymbolTable *table = xr_symbol_table_create();
    xr_symbol_table_init_builtins(table);

    // Verify some builtin symbol names
    SymbolId id = xr_symbol_lookup_in_table(table, "toString");
    ASSERT_TRUE(id != SYMBOL_INVALID);
    ASSERT_STR_EQ(xr_symbol_get_name_in_table(table, id), "toString");

    xr_symbol_table_destroy(table);
}

TEST(symbol_user_after_builtin) {
    XrSymbolTable *table = xr_symbol_table_create();
    xr_symbol_table_init_builtins(table);

    int builtin_count = table->builtin_count;

    // Register user symbol
    SymbolId user_id = xr_symbol_register_in_table(table, "myUserSymbol");
    ASSERT_TRUE(user_id != SYMBOL_INVALID);
    ASSERT_EQ_INT(table->count, builtin_count + 1);
    ASSERT_EQ_INT(table->count - table->builtin_count, 1);

    // User symbol should be findable
    ASSERT_EQ_INT(xr_symbol_lookup_in_table(table, "myUserSymbol"), user_id);
    ASSERT_STR_EQ(xr_symbol_get_name_in_table(table, user_id), "myUserSymbol");

    xr_symbol_table_destroy(table);
}

/* ========== Edge Cases ========== */

TEST(symbol_empty_string) {
    XrSymbolTable *table = xr_symbol_table_create();

    SymbolId id = xr_symbol_register_in_table(table, "");
    ASSERT_TRUE(id != SYMBOL_INVALID);
    ASSERT_STR_EQ(xr_symbol_get_name_in_table(table, id), "");

    xr_symbol_table_destroy(table);
}

TEST(symbol_long_name) {
    XrSymbolTable *table = xr_symbol_table_create();

    const char *long_name =
        "this_is_a_very_long_symbol_name_that_might_cause_issues_in_some_implementations";
    SymbolId id = xr_symbol_register_in_table(table, long_name);

    ASSERT_TRUE(id != SYMBOL_INVALID);
    ASSERT_STR_EQ(xr_symbol_get_name_in_table(table, id), long_name);

    xr_symbol_table_destroy(table);
}

TEST(symbol_special_chars) {
    XrSymbolTable *table = xr_symbol_table_create();

    // Operator symbols
    SymbolId plus = xr_symbol_register_in_table(table, "+");
    SymbolId minus = xr_symbol_register_in_table(table, "-");
    SymbolId eq = xr_symbol_register_in_table(table, "==");

    ASSERT_TRUE(plus != SYMBOL_INVALID);
    ASSERT_TRUE(minus != SYMBOL_INVALID);
    ASSERT_TRUE(eq != SYMBOL_INVALID);

    ASSERT_STR_EQ(xr_symbol_get_name_in_table(table, plus), "+");
    ASSERT_STR_EQ(xr_symbol_get_name_in_table(table, minus), "-");
    ASSERT_STR_EQ(xr_symbol_get_name_in_table(table, eq), "==");

    xr_symbol_table_destroy(table);
}

TEST(symbol_null_params) {
    XrSymbolTable *table = xr_symbol_table_create();

    // NULL table
    ASSERT_EQ_INT(xr_symbol_register_in_table(NULL, "test"), SYMBOL_INVALID);
    ASSERT_EQ_INT(xr_symbol_lookup_in_table(NULL, "test"), SYMBOL_INVALID);
    ASSERT_TRUE(xr_symbol_get_name_in_table(NULL, 1) == NULL);

    // NULL name
    ASSERT_EQ_INT(xr_symbol_register_in_table(table, NULL), SYMBOL_INVALID);
    ASSERT_EQ_INT(xr_symbol_lookup_in_table(table, NULL), SYMBOL_INVALID);

    xr_symbol_table_destroy(table);
}

/* ========== Stress Tests ========== */

TEST(symbol_stress_many_symbols) {
    XrSymbolTable *table = xr_symbol_table_create();

    char name[64];
    SymbolId ids[500];

    // Register many symbols
    for (int i = 0; i < 500; i++) {
        snprintf(name, sizeof(name), "symbol_%d", i);
        ids[i] = xr_symbol_register_in_table(table, name);
        ASSERT_TRUE(ids[i] != SYMBOL_INVALID);
    }

    ASSERT_EQ_INT(table->count, 500);

    // Verify all lookups
    for (int i = 0; i < 500; i++) {
        snprintf(name, sizeof(name), "symbol_%d", i);
        ASSERT_EQ_INT(xr_symbol_lookup_in_table(table, name), ids[i]);
    }

    xr_symbol_table_destroy(table);
}

TEST(symbol_stress_with_builtins) {
    XrSymbolTable *table = xr_symbol_table_create();
    xr_symbol_table_init_builtins(table);

    int initial_count = table->count;
    char name[64];

    // Add user symbols
    for (int i = 0; i < 200; i++) {
        snprintf(name, sizeof(name), "user_symbol_%d", i);
        SymbolId id = xr_symbol_register_in_table(table, name);
        ASSERT_TRUE(id != SYMBOL_INVALID);
    }

    ASSERT_EQ_INT(table->count, initial_count + 200);
    ASSERT_EQ_INT(table->count - table->builtin_count, 200);

    // Builtins should still work
    ASSERT_TRUE(xr_symbol_lookup_in_table(table, "has") != SYMBOL_INVALID);
    ASSERT_TRUE(xr_symbol_lookup_in_table(table, "push") != SYMBOL_INVALID);

    xr_symbol_table_destroy(table);
}

/* ========== Main ========== */

static void run_all_tests(void) {
    RUN_TEST_SUITE("Basic Operations");
    RUN_TEST(symbol_table_create_destroy);
    RUN_TEST(symbol_register_single);
    RUN_TEST(symbol_register_multiple);
    RUN_TEST(symbol_register_duplicate);

    RUN_TEST_SUITE("Lookup Operations");
    RUN_TEST(symbol_lookup_exists);
    RUN_TEST(symbol_lookup_not_exists);
    RUN_TEST(symbol_lookup_after_multiple);

    RUN_TEST_SUITE("Get Name Operations");
    RUN_TEST(symbol_get_name);
    RUN_TEST(symbol_get_name_invalid_id);
    RUN_TEST(symbol_get_name_multiple);

    RUN_TEST_SUITE("Builtin Symbols");
    RUN_TEST(symbol_init_builtins);
    RUN_TEST(symbol_builtin_lookup);
    RUN_TEST(symbol_builtin_names);
    RUN_TEST(symbol_user_after_builtin);

    RUN_TEST_SUITE("Edge Cases");
    RUN_TEST(symbol_empty_string);
    RUN_TEST(symbol_long_name);
    RUN_TEST(symbol_special_chars);
    RUN_TEST(symbol_null_params);

    RUN_TEST_SUITE("Stress Tests");
    RUN_TEST(symbol_stress_many_symbols);
    RUN_TEST(symbol_stress_with_builtins);
}

TEST_MAIN_BEGIN()
printf("=== xray Symbol Table Unit Tests ===\n");
run_all_tests();
TEST_MAIN_END()
