/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_mcp_knowledge.c - Independent generated knowledge lookup tests
 *
 * KEY CONCEPT:
 *   Generated knowledge lookup and ranking use no compiler or executor.
 *   Their regression tests link only the production lookup and generated
 *   tables so language execution dependencies cannot obscure failures.
 */

#include "../../../src/app/mcp/xmcp_knowledge.h"
#include "../../../src/base/xmalloc.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) \
    do { \
        int previous_failures = tests_failed; \
        printf("  Testing %s... ", #name); \
        test_##name(); \
        if (tests_failed == previous_failures) { \
            printf("PASS\n"); \
            tests_passed++; \
        } \
    } while (0)
#define ASSERT(cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL at line %d: %s\n", __LINE__, #cond); \
            tests_failed++; \
            return; \
        } \
    } while (0)
#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_STR_EQ(a, b) \
    do { \
        const char *_assert_str_a = (a); \
        const char *_assert_str_b = (b); \
        ASSERT(_assert_str_a != NULL); \
        ASSERT(_assert_str_b != NULL); \
        ASSERT(strcmp(_assert_str_a, _assert_str_b) == 0); \
    } while (0)
#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)

/* =========================================================================
 * Knowledge base
 * ========================================================================= */

TEST(knowledge_new_and_load) {
    XmcpKnowledge *kb = xmcp_knowledge_new();
    ASSERT_NOT_NULL(kb);
    ASSERT_EQ(kb->topic_count, 0);
    ASSERT_EQ(kb->module_count, 0);

    xmcp_knowledge_load(kb);
    ASSERT(kb->topic_count > 0);
    ASSERT(kb->module_count > 0);

    xmcp_knowledge_free(kb);
}

TEST(knowledge_lookup_exact) {
    XmcpKnowledge *kb = xmcp_knowledge_new();
    ASSERT_NOT_NULL(kb);
    xmcp_knowledge_load(kb);

    const char *content = xmcp_knowledge_lookup_topic(kb, "channel");
    ASSERT_NOT_NULL(content);
    /* Content should mention Channel */
    ASSERT(strstr(content, "Channel") != NULL);

    xmcp_knowledge_free(kb);
}

TEST(knowledge_lookup_alias) {
    XmcpKnowledge *kb = xmcp_knowledge_new();
    ASSERT_NOT_NULL(kb);
    xmcp_knowledge_load(kb);

    /* "fn" is an alias for "functions" */
    const char *content = xmcp_knowledge_lookup_topic(kb, "fn");
    ASSERT_NOT_NULL(content);
    ASSERT(strstr(content, "Functions") != NULL || strstr(content, "fn") != NULL);

    xmcp_knowledge_free(kb);
}

TEST(knowledge_lookup_not_found) {
    XmcpKnowledge *kb = xmcp_knowledge_new();
    ASSERT_NOT_NULL(kb);
    xmcp_knowledge_load(kb);

    const char *content = xmcp_knowledge_lookup_topic(kb, "zzz_nonexistent_topic");
    ASSERT(content == NULL);

    xmcp_knowledge_free(kb);
}

TEST(knowledge_search_stdlib) {
    XmcpKnowledge *kb = xmcp_knowledge_new();
    ASSERT_NOT_NULL(kb);
    xmcp_knowledge_load(kb);

    XmcpStdlibSearchResult matches;
    xmcp_knowledge_search_stdlib_matches(kb, "http", NULL, &matches);
    ASSERT(matches.match_count > 0);
    ASSERT_STR_EQ(matches.matches[0].module->name, "http");
    ASSERT(matches.matches[0].symbol == NULL);

    int match_count = 0;
    char *result = xmcp_knowledge_search_stdlib(kb, "http", NULL, &match_count);
    ASSERT_NOT_NULL(result);
    ASSERT(match_count > 0);
    ASSERT(strstr(result, "Module: http") != NULL);
    xr_free(result);

    xmcp_knowledge_free(kb);
}

TEST(knowledge_search_stdlib_ranks_symbols) {
    XmcpKnowledge *kb = xmcp_knowledge_new();
    ASSERT_NOT_NULL(kb);
    xmcp_knowledge_load(kb);

    XmcpStdlibSearchResult result;
    xmcp_knowledge_search_stdlib_matches(kb, "parse", "csv", &result);
    ASSERT(result.match_count > 0);
    ASSERT_NOT_NULL(result.matches[0].module);
    ASSERT_STR_EQ(result.matches[0].module->name, "csv");
    ASSERT_NOT_NULL(result.matches[0].symbol);
    ASSERT_STR_EQ(result.matches[0].symbol->name, "parse");
    ASSERT(result.matches[0].score > 0);

    xmcp_knowledge_free(kb);
}

TEST(knowledge_search_stdlib_module_symbol_direct) {
    XmcpKnowledge *kb = xmcp_knowledge_new();
    ASSERT_NOT_NULL(kb);
    xmcp_knowledge_load(kb);

    XmcpStdlibSearchResult result;
    xmcp_knowledge_search_stdlib_matches(kb, "http.request", NULL, &result);
    ASSERT(result.match_count > 0);
    ASSERT_NOT_NULL(result.matches[0].module);
    ASSERT_STR_EQ(result.matches[0].module->name, "http");
    ASSERT_NOT_NULL(result.matches[0].symbol);
    ASSERT_STR_EQ(result.matches[0].symbol->name, "request");

    xmcp_knowledge_search_stdlib_matches(kb, "http.request", "csv", &result);
    ASSERT_EQ(result.match_count, 0);

    xmcp_knowledge_free(kb);
}

TEST(knowledge_search_stdlib_multi_token_module_context) {
    XmcpKnowledge *kb = xmcp_knowledge_new();
    ASSERT_NOT_NULL(kb);
    xmcp_knowledge_load(kb);

    XmcpStdlibSearchResult result;
    xmcp_knowledge_search_stdlib_matches(kb, "csv parse", NULL, &result);
    ASSERT(result.match_count > 0);
    ASSERT_NOT_NULL(result.matches[0].module);
    ASSERT_STR_EQ(result.matches[0].module->name, "csv");
    ASSERT_NOT_NULL(result.matches[0].symbol);
    ASSERT_STR_EQ(result.matches[0].symbol->name, "parse");

    xmcp_knowledge_free(kb);
}

TEST(knowledge_search_stdlib_no_match) {
    XmcpKnowledge *kb = xmcp_knowledge_new();
    ASSERT_NOT_NULL(kb);
    xmcp_knowledge_load(kb);

    int match_count = 0;
    char *result = xmcp_knowledge_search_stdlib(kb, "zzz_nonexistent", NULL, &match_count);
    ASSERT_NOT_NULL(result);
    ASSERT_EQ(match_count, 0);
    /* Should contain "No modules found" or module list */
    ASSERT(strstr(result, "No modules found") != NULL ||
           strstr(result, "Available modules") != NULL);
    xr_free(result);

    xmcp_knowledge_free(kb);
}

TEST(knowledge_get_cheatsheet) {
    const char *cs = xmcp_knowledge_get_cheatsheet();
    ASSERT_NOT_NULL(cs);
    ASSERT(strstr(cs, "Xray") != NULL);
    ASSERT(strlen(cs) > 100);
}

TEST(knowledge_get_concurrency) {
    const char *cm = xmcp_knowledge_get_concurrency();
    ASSERT_NOT_NULL(cm);
    ASSERT(strstr(cm, "Concurrency") != NULL);
}

TEST(knowledge_get_stdlib_list) {
    const char *sl = xmcp_knowledge_get_stdlib_list();
    ASSERT_NOT_NULL(sl);
    ASSERT(strstr(sl, "http") != NULL);
    ASSERT(strstr(sl, "| `json` |") == NULL);
    ASSERT(strstr(sl, "JSON.parse<T>()") != NULL);
}

int main(void) {
    xr_test_suppress_dialogs();
    printf("=== MCP Knowledge Tests ===\n");

    RUN_TEST(knowledge_new_and_load);
    RUN_TEST(knowledge_lookup_exact);
    RUN_TEST(knowledge_lookup_alias);
    RUN_TEST(knowledge_lookup_not_found);
    RUN_TEST(knowledge_search_stdlib);
    RUN_TEST(knowledge_search_stdlib_ranks_symbols);
    RUN_TEST(knowledge_search_stdlib_module_symbol_direct);
    RUN_TEST(knowledge_search_stdlib_multi_token_module_context);
    RUN_TEST(knowledge_search_stdlib_no_match);
    RUN_TEST(knowledge_get_cheatsheet);
    RUN_TEST(knowledge_get_concurrency);
    RUN_TEST(knowledge_get_stdlib_list);

    printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
