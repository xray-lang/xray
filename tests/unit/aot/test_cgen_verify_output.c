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

#include "base/xmalloc.h"
#define VERIFY_CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
/* Observe the actual resource allocator; the verifier remains the linked
 * production implementation. The fixed observer itself never allocates. */
typedef struct VerifyAllocation { void *pointer; size_t bytes; } VerifyAllocation;
static VerifyAllocation verify_allocations[32];
static size_t verify_live,verify_physical;
static XrCompileResources *verify_resources;
static XrCompileResourceStats verify_baseline;
static void *verify_counted_malloc(size_t bytes) {
    void *memory=xr_malloc(bytes);if (!memory) return NULL;
    for (size_t i=0;i<32;++i) if (!verify_allocations[i].pointer) {
        VERIFY_CHECK(bytes<=SIZE_MAX-verify_physical);
        verify_allocations[i]=(VerifyAllocation){memory,bytes};++verify_live;verify_physical+=bytes;return memory;
    }
    VERIFY_CHECK(false);return NULL;
}
static void verify_counted_free(void *memory) {
    if (!memory) return;
    for (size_t i=0;i<32;++i) if (verify_allocations[i].pointer==memory) {
        VERIFY_CHECK(verify_live && verify_physical>=verify_allocations[i].bytes);
        verify_physical-=verify_allocations[i].bytes;--verify_live;verify_allocations[i]=(VerifyAllocation){0};xr_free(memory);return;
    }
    VERIFY_CHECK(false);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) verify_counted_malloc(bytes)
#define xr_free(pointer) verify_counted_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
static void verifier_owner_new(void) {
    VERIFY_CHECK(!verify_resources && !verify_live && !verify_physical);
    const XrCompileResourceLimits limits={UINT64_C(2)*1024*1024,UINT64_C(1)*1024*1024,UINT64_C(2000000)};
    VERIFY_CHECK(xr_compile_resources_new(&limits,&verify_resources)==XR_COMPILE_RESOURCE_OK);
    VERIFY_CHECK(xr_compile_resources_stats(verify_resources,&verify_baseline)==XR_COMPILE_RESOURCE_OK);
    VERIFY_CHECK(verify_live==1 && verify_physical==verify_baseline.live_bytes);
}
static void verifier_owner_baseline(void) {
    XrCompileResourceStats current={0};
    VERIFY_CHECK(xr_compile_resources_stats(verify_resources,&current)==XR_COMPILE_RESOURCE_OK);
    VERIFY_CHECK(current.live_bytes==verify_baseline.live_bytes && verify_live==1 && verify_physical==verify_baseline.live_bytes);
    VERIFY_CHECK(current.allocated_bytes<=UINT64_C(2)*1024*1024 && current.peak_bytes<=UINT64_C(1)*1024*1024 && current.work<=UINT64_C(2000000));
}
static void verifier_owner_free(void) {
    verifier_owner_baseline();xr_compile_resources_release(verify_resources);verify_resources=NULL;
    VERIFY_CHECK(!verify_live && !verify_physical);
    puts("CGen original output goldens: finite cumulative owner, actual physical blocks/bytes=0/0");
}
static XiCgenVerifyResult verify(const char *src) {
    XiCgenVerifyResult r;memset(&r,0,sizeof(r));
    XiCgenVerifyStatus status=xr_compile_cgen_verify_output(verify_resources,src,strlen(src),&r);
    verifier_owner_baseline();
    VERIFY_CHECK(status==(r.category==XI_CGEN_VERIFY_OK?XI_CGEN_VERIFY_PASSED:XI_CGEN_VERIFY_MALFORMED));
    return r;
}
static XiCgenVerifyResult verify_c90(const char *src) {
    XiCgenVerifyResult r;memset(&r,0,sizeof(r));
    XiCgenVerifyStatus status=xr_compile_cgen_verify_c90_output(verify_resources,src,strlen(src),&r);
    verifier_owner_baseline();
    VERIFY_CHECK(status==(r.category==XI_CGEN_VERIFY_OK?XI_CGEN_VERIFY_PASSED:XI_CGEN_VERIFY_MALFORMED));
    return r;
}

/* ========== W1: brace / quote / comment balance ========== */

TEST(w1_unbalanced_braces) {
    const char *src = "void f(void) {\n"
                      "    i64 a = 1;\n"; /* missing closing brace */
    XiCgenVerifyResult r = verify(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_W1_BALANCE);
    ASSERT_TRUE(r.line > 0);
}

TEST(w1_stray_close_brace) {
    const char *src = "void f(void) {\n"
                      "    i64 a = 1;\n"
                      "}\n"
                      "}\n"; /* one extra close */
    XiCgenVerifyResult r = verify(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_W1_BALANCE);
}

TEST(w1_unterminated_string) {
    const char *src = "static const char *s = \"abc;\n"
                      "i64 x = 0;\n";
    XiCgenVerifyResult r = verify(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_W1_BALANCE);
}

/* ========== W2: identifier hygiene ========== */

TEST(w2_path_fragment) {
    /* a source/path fragment leaked into an emitted symbol position */
    const char *src = "static i64 broken = pkg/../oops;\n";
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
                      "    i64 a = v5;\n" /* v5 used here ... */
                      "    i64 v5 = 2;\n" /* ... but defined here */
                      "}\n";
    XiCgenVerifyResult r = verify(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_W4_FORWARD_REF);
    ASSERT_EQ_INT(r.line, 2);
}

/* ========== Well-formed inputs must pass ========== */

TEST(ok_simple_program) {
    const char *src = "#include <stdio.h>\n"
                      "static i64 add(i64 a, i64 b) {\n"
                      "    i64 v0 = a + b;\n"
                      "    return v0;\n"
                      "}\n"
                      "i64 main(void) {\n"
                      "    i64 v1 = add(2, 3);\n"
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
                      "    i64 v0 = 0;\n"
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
                      "i64 resume(void *raw) {\n"
                      "    frame *f = (frame *) raw;\n"
                      "#define v3 (f->v3)\n"
                      "#if defined(XRAY_AOT_DEBUG_LOCALS)\n"
                      "    int64_t dbg = (int64_t) v3;\n"
                      "#endif\n"
                      "    v3 = 7;\n"
                      "    return (i64) v3;\n"
                      "#undef v3\n"
                      "}\n";
    XiCgenVerifyResult r = verify(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_OK);
}

/* ========== Restricted C90 policy ========== */

TEST(c90_accepts_governed_kernel_shape) {
    const char *src = "#include \"xrt_c90.h\"\n"
                      "unsigned i64 hash(const void *data, size_t length) {\n"
                      "    unsigned i64 value;\n"
                      "    (void) data;\n"
                      "    value = (unsigned i64) length;\n"
                      "    return value;\n"
                      "}\n";
    XiCgenVerifyResult r = verify_c90(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_OK);
}

TEST(c90_rejects_compound_literal_and_runtime_residue) {
    XiCgenVerifyResult compound = verify_c90("void f(void) { S s = ((S){0}); }\n");
    XiCgenVerifyResult runtime = verify_c90("extern i64 xrt_builtins[4];\n");
    ASSERT_EQ_INT(compound.category, XI_CGEN_VERIFY_C90_RESTRICTED);
    ASSERT_TRUE(strstr(compound.message, "compound-literal") != NULL);
    ASSERT_EQ_INT(runtime.category, XI_CGEN_VERIFY_C90_RESTRICTED);
    ASSERT_TRUE(strstr(runtime.message, "builtin-table") != NULL);
}

TEST(c90_rejects_line_comments_but_ignores_literal_text) {
    XiCgenVerifyResult comment = verify_c90("i64 x; // not ISO C90\n");
    XiCgenVerifyResult literal = verify_c90("const char *s = \"// ({ _Atomic ...\";\n");
    ASSERT_EQ_INT(comment.category, XI_CGEN_VERIFY_C90_RESTRICTED);
    ASSERT_EQ_INT(literal.category, XI_CGEN_VERIFY_OK);
}

TEST(category_names_are_stable) {
    ASSERT_STR_EQ(xi_cgen_verify_category_name(XI_CGEN_VERIFY_W1_BALANCE), "W1_BALANCE");
    ASSERT_STR_EQ(xi_cgen_verify_category_name(XI_CGEN_VERIFY_W2_IDENTIFIER), "W2_IDENTIFIER");
    ASSERT_STR_EQ(xi_cgen_verify_category_name(XI_CGEN_VERIFY_W3_SCOPE), "W3_SCOPE");
    ASSERT_STR_EQ(xi_cgen_verify_category_name(XI_CGEN_VERIFY_W4_FORWARD_REF), "W4_FORWARD_REF");
    ASSERT_STR_EQ(xi_cgen_verify_category_name(XI_CGEN_VERIFY_C90_RESTRICTED), "C90_RESTRICTED");
}

TEST(indexed_masks_preserve_multiline_crlf) {
    const char *src = "/* masked { )\r\n"
                      "pkg/../hidden v9\r\n"
                      "*/\r\n"
                      "void f(void) {\r\n"
                      "    int v0 = 7;\r\n"
                      "    const char *s = \"} ( v99 pkg/../hidden\";\r\n"
                      "    (void) v0;\r\n"
                      "}\r\n";
    XiCgenVerifyResult r = verify(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_OK);
}
TEST(indexed_escaped_newline_preserves_temp_position) {
    const char *src = "\nvoid f(void) {\n"
                      "    const char *s = \"(\\\n"
                      ")\";\n"
                      "    // } ) v5\n"
                      "    int a = v5;\n"
                      "    int v5 = 7;\n"
                      "}\n";
    XiCgenVerifyResult r = verify(src);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_W4_FORWARD_REF);
    ASSERT_EQ_INT(r.line, 6);
}
TEST(indexed_eof_and_late_lexical_priority) {
    XiCgenVerifyResult eof = verify("void f(void) {\n int a = v12;\n}");
    ASSERT_EQ_INT(eof.category, XI_CGEN_VERIFY_W4_FORWARD_REF);
    ASSERT_EQ_INT(eof.line, 2);
    const char *src = "return 0;\nvoid f(void) {\n int a = v7;\n const char *s = \"unfinished";
    XiCgenVerifyResult late = verify(src);
    ASSERT_EQ_INT(late.category, XI_CGEN_VERIFY_W1_BALANCE);
    ASSERT_EQ_INT(late.line, 4);
}
TEST(indexed_growth_preserves_scope_position) {
    char source[512];
    memset(source, '\n', 260);
    memcpy(source + 260, "return 0;", sizeof("return 0;"));
    XiCgenVerifyResult r = verify(source);
    ASSERT_EQ_INT(r.category, XI_CGEN_VERIFY_W3_SCOPE);
    ASSERT_EQ_INT(r.line, 261);
}

TEST_MAIN_BEGIN()
verifier_owner_new();
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
RUN_TEST_SUITE("CGen output verifier — restricted C90");
RUN_TEST(c90_accepts_governed_kernel_shape);
RUN_TEST(c90_rejects_compound_literal_and_runtime_residue);
RUN_TEST(c90_rejects_line_comments_but_ignores_literal_text);
RUN_TEST(category_names_are_stable);
RUN_TEST_SUITE("CGen output verifier — owned lexical line spans");
RUN_TEST(indexed_masks_preserve_multiline_crlf);
RUN_TEST(indexed_escaped_newline_preserves_temp_position);
RUN_TEST(indexed_eof_and_late_lexical_priority);
RUN_TEST(indexed_growth_preserves_scope_position);
verifier_owner_free();
TEST_MAIN_END()
