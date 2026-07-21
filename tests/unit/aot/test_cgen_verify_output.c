/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cgen_verify_output.c - injection tests for the CGen well-formedness
 * verifier (task 218 defense line 3). Each W1-W4 category is exercised by
 * feeding a crafted malformed C fragment directly to the pure verifier, and a
 * set of realistic well-formed fragments must pass untouched.
 */

#include "../test_framework.h"
#include "aot/xi_cgen_verify_output.h"
#include <string.h>

static XiCgenVerifyResult verify(const char *src) {
    XiCgenVerifyResult r;
    memset(&r, 0, sizeof(r));
    xi_cgen_verify_output(src, strlen(src), &r);
    return r;
}

/* ========== W1: brace / quote / comment balance ========== */

TEST(w1_unbalanced_braces) {
    const char *src = "void f(void) {\n"
                      "    int a = 1;\n"; /* missing closing brace */
    XiCgenVerifyResult r = verify(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_W1_BALANCE);
    ASSERT_TRUE(r.line > 0);
}

TEST(w1_stray_close_brace) {
    const char *src = "void f(void) {\n"
                      "    int a = 1;\n"
                      "}\n"
                      "}\n"; /* one extra close */
    XiCgenVerifyResult r = verify(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_W1_BALANCE);
}

TEST(w1_unterminated_string) {
    const char *src = "static const char *s = \"abc;\n"
                      "int x = 0;\n";
    XiCgenVerifyResult r = verify(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_W1_BALANCE);
}

/* ========== W2: identifier hygiene ========== */

TEST(w2_path_fragment) {
    /* a source/path fragment leaked into an emitted symbol position */
    const char *src = "static int broken = pkg/../oops;\n";
    XiCgenVerifyResult r = verify(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_W2_IDENTIFIER);
}

TEST(w2_identifier_runs_into_source_fragment) {
    /* the historical `extern void * xr_ffi_ } else {` corruption, kept
     * brace-balanced so the identifier check (not W1) is what fires */
    const char *src = "void f(void) {\n"
                      "    if (c) { xr_ffi_ } else { }\n"
                      "}\n";
    XiCgenVerifyResult r = verify(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_W2_IDENTIFIER);
}

/* ========== W3: scope hygiene ========== */

TEST(w3_statement_at_file_scope) {
    /* a function-body statement spilled to file scope (brace depth 0) */
    const char *src = "void f(void) {\n"
                      "    return;\n"
                      "}\n"
                      "return 0;\n";
    XiCgenVerifyResult r = verify(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_W3_SCOPE);
    ASSERT_EQ_INT(r.line, 4);
}

TEST(w3_temp_assignment_at_file_scope) {
    const char *src = "void f(void) {\n"
                      "}\n"
                      "v3 = xrt_add(v1, v2);\n";
    XiCgenVerifyResult r = verify(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_W3_SCOPE);
}

/* ========== W4: forward reference ========== */

TEST(w4_use_before_def) {
    const char *src = "void f(void) {\n"
                      "    int a = v5;\n" /* v5 used here ... */
                      "    int v5 = 2;\n" /* ... but defined here */
                      "}\n";
    XiCgenVerifyResult r = verify(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_W4_FORWARD_REF);
    ASSERT_EQ_INT(r.line, 2);
}

/* ========== Well-formed inputs must pass ========== */

TEST(ok_simple_program) {
    const char *src = "#include <stdio.h>\n"
                      "static int add(int a, int b) {\n"
                      "    int v0 = a + b;\n"
                      "    return v0;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    int v1 = add(2, 3);\n"
                      "    return v1;\n"
                      "}\n";
    XiCgenVerifyResult r = verify(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_OK);
}

TEST(ok_strings_and_comments_with_braces) {
    /* braces/parens inside strings and comments must not be counted */
    const char *src = "/* a comment with { unbalanced braces )( */\n"
                      "static const char *j = \"{ \\\"k\\\": [1,2,3] }\";\n"
                      "void g(void) {\n"
                      "    // trailing } ) brace in a line comment\n"
                      "    int v0 = 0;\n"
                      "    (void) v0;\n"
                      "}\n";
    XiCgenVerifyResult r = verify(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_OK);
}

TEST(ok_coroutine_frame_macro_temps) {
    /* coroutine codegen: vN are frame fields aliased via #define, inside
     * #if debug islands; none of this is a forward reference */
    const char *src = "typedef struct frame {\n"
                      "    uint32_t state;\n"
                      "    int64_t v3;\n"
                      "} frame;\n"
                      "int resume(void *raw) {\n"
                      "    frame *f = (frame *) raw;\n"
                      "#define v3 (f->v3)\n"
                      "#if defined(XRAY_AOT_DEBUG_LOCALS)\n"
                      "    int64_t dbg = (int64_t) v3;\n"
                      "#endif\n"
                      "    v3 = 7;\n"
                      "    return (int) v3;\n"
                      "#undef v3\n"
                      "}\n";
    XiCgenVerifyResult r = verify(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_OK);
}

TEST(category_names_are_stable) {
    ASSERT_STR_EQ(xi_cgen_verify_category_name(XI_CGEN_VERIFY_W1_BALANCE), "W1_BALANCE");
    ASSERT_STR_EQ(xi_cgen_verify_category_name(XI_CGEN_VERIFY_W2_IDENTIFIER), "W2_IDENTIFIER");
    ASSERT_STR_EQ(xi_cgen_verify_category_name(XI_CGEN_VERIFY_W3_SCOPE), "W3_SCOPE");
    ASSERT_STR_EQ(xi_cgen_verify_category_name(XI_CGEN_VERIFY_W4_FORWARD_REF), "W4_FORWARD_REF");
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("CGen output verifier — W1 balance");
RUN_TEST(w1_unbalanced_braces);
RUN_TEST(w1_stray_close_brace);
RUN_TEST(w1_unterminated_string);
RUN_TEST_SUITE("CGen output verifier — W2 identifier hygiene");
RUN_TEST(w2_path_fragment);
RUN_TEST(w2_identifier_runs_into_source_fragment);
RUN_TEST_SUITE("CGen output verifier — W3 scope hygiene");
RUN_TEST(w3_statement_at_file_scope);
RUN_TEST(w3_temp_assignment_at_file_scope);
RUN_TEST_SUITE("CGen output verifier — W4 forward reference");
RUN_TEST(w4_use_before_def);
RUN_TEST_SUITE("CGen output verifier — well-formed inputs");
RUN_TEST(ok_simple_program);
RUN_TEST(ok_strings_and_comments_with_braces);
RUN_TEST(ok_coroutine_frame_macro_temps);
RUN_TEST(category_names_are_stable);
TEST_MAIN_END()
