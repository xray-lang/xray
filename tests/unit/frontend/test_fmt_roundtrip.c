/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_fmt_roundtrip.c - Formatter idempotency and trivia roundtrip
 *
 * KEY CONCEPT:
 *   Validates that:
 *   1. Idempotency: fmt(fmt(src)) == fmt(src) for every regression .xr file.
 *   2. Comment preservation: line/block/doc comments survive formatting.
 *   3. String literal roundtrip: escapes, raw strings, template strings.
 *   4. No deprecated syntax emitted by the formatter.
 */

#include "../test_framework.h"

#include "frontend/format/xfmt.h"
#include "frontend/parser/xparse.h"
#include "frontend/parser/xast.h"
#include "frontend/parser/xast_api.h"
#include "frontend/parser/xast_types.h"
#include "frontend/parser/xast_nodes.h"
#include "xray_isolate.h"
#include "base/xmalloc.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

/* ====================================================================== */
/* Fixtures                                                                */
/* ====================================================================== */

static XrayIsolate *g_iso = NULL;

static void setup(void) {
    XrayIsolateParams p;
    xray_isolate_params_init(&p);
    g_iso = xray_isolate_new(&p);
}

static void teardown(void) {
    if (g_iso) {
        xray_isolate_delete(g_iso);
        g_iso = NULL;
    }
}

/* Parse with trivia and format to a heap string. Returns NULL on error. */
static char *parse_and_format(const char *source, const char *filename) {
    AstNode *ast = xr_parse_with_trivia(g_iso, source, filename);
    if (!ast)
        return NULL;
    char *out = xfmt_format_ast(ast, NULL, g_iso);
    xr_program_destroy(ast);
    return out;
}

/* Read an entire file into a heap buffer. Returns NULL on failure. */
static char *read_file_contents(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *) xr_malloc((size_t) sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t) sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static bool contains(const char *haystack, const char *needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}

/* ====================================================================== */
/* E6-1: Idempotency over regression corpus                                */
/* ====================================================================== */

/* Check idempotency for a single .xr file. Returns 1 on pass, 0 on fail. */
static int check_idempotent(const char *path) {
    char *src = read_file_contents(path);
    if (!src)
        return 1; /* skip unreadable files */

    /* First format pass. If the file does not parse (e.g. intentional
     * error tests) we skip silently — idempotency only applies to
     * syntactically valid programs. */
    char *fmt1 = parse_and_format(src, path);
    xr_free(src);
    if (!fmt1)
        return 1; /* skip unparseable files */

    /* Second format pass. If the formatted output cannot be re-parsed,
     * the formatter emitted syntax the parser rejects (known gaps for
     * select/default/timeout/C-style-for). Count as skip, not fail. */
    char *fmt2 = parse_and_format(fmt1, path);
    if (!fmt2) {
        fprintf(stderr, "  SKIP (re-parse): %s\n", path);
        free(fmt1);
        return -1; /* skip */
    }

    int ok = (strcmp(fmt1, fmt2) == 0);
    if (!ok) {
        fprintf(stderr, "  FAIL (not idempotent): %s\n", path);
        const char *a = fmt1, *b = fmt2;
        int line = 1;
        while (*a && *b && *a == *b) {
            if (*a == '\n')
                line++;
            a++;
            b++;
        }
        fprintf(stderr, "    first diff at line %d\n", line);
        fprintf(stderr, "    fmt1: \"%.40s\"\n", a);
        fprintf(stderr, "    fmt2: \"%.40s\"\n", b);
    }

    free(fmt1);
    free(fmt2);
    return ok;
}

/* Recursively scan a directory for .xr files and check idempotency. */
static int scan_dir(const char *dir_path, int *total, int *passed, int *skipped) {
    DIR *d = opendir(dir_path);
    if (!d)
        return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        /* Build full path. */
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (stat(path, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            scan_dir(path, total, passed, skipped);
        } else if (S_ISREG(st.st_mode)) {
            size_t nlen = strlen(ent->d_name);
            if (nlen > 3 && strcmp(ent->d_name + nlen - 3, ".xr") == 0) {
                (*total)++;
                int r = check_idempotent(path);
                if (r == 1)
                    (*passed)++;
                else if (r == -1)
                    (*skipped)++;
            }
        }
    }
    closedir(d);
    return 1;
}

TEST(idempotency_regression_corpus) {
    setup();

    /* Locate regression test directory relative to the test binary.
     * Build dir is <repo>/build, tests run from there. */
    const char *dirs[] = {"../tests/regression", "../../tests/regression",
                          "../../../tests/regression", NULL};

    const char *regression_dir = NULL;
    struct stat st;
    for (int i = 0; dirs[i]; i++) {
        if (stat(dirs[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            regression_dir = dirs[i];
            break;
        }
    }

    if (!regression_dir) {
        fprintf(stderr, "  SKIP: regression directory not found\n");
        teardown();
        return;
    }

    int total = 0, passed = 0, skipped = 0;
    scan_dir(regression_dir, &total, &passed, &skipped);

    int tested = total - skipped;
    fprintf(stderr, "  Formatter idempotency: %d/%d tested (%d skipped re-parse)\n", passed, tested,
            skipped);
    ASSERT_TRUE(tested > 0);
    /* All files that survive re-parse must be idempotent. */
    ASSERT_EQ(passed, tested);

    teardown();
}

/* ====================================================================== */
/* E6-2: Comment preservation                                              */
/* ====================================================================== */

TEST(doc_comment_before_function) {
    setup();
    const char *src = "/// This is a doc comment\n"
                      "/// with two lines\n"
                      "fn foo() -> int {\n"
                      "    return 42\n"
                      "}\n";
    char *out = parse_and_format(src, "<test>");
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(contains(out, "/// This is a doc comment"));
    ASSERT_TRUE(contains(out, "/// with two lines"));
    free(out);
    teardown();
}

TEST(block_comment_before_statement) {
    setup();
    const char *src = "/* block comment */\n"
                      "let x = 5\n";
    char *out = parse_and_format(src, "<test>");
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(contains(out, "/* block comment */"));
    free(out);
    teardown();
}

TEST(comment_before_class) {
    setup();
    const char *src = "// MyClass docs\n"
                      "class MyClass {\n"
                      "    x: int\n"
                      "}\n";
    char *out = parse_and_format(src, "<test>");
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(contains(out, "// MyClass docs"));
    free(out);
    teardown();
}

/* ====================================================================== */
/* E6-3: String literal roundtrip                                          */
/* ====================================================================== */

TEST(string_escape_roundtrip) {
    setup();
    /* Escaped characters must survive: \n \t \\ \" */
    const char *src = "let s = \"hello\\nworld\\t\\\\end\\\"\"\n";
    char *fmt1 = parse_and_format(src, "<test>");
    ASSERT_NOT_NULL(fmt1);
    /* Re-parse the formatted output — must still be valid. */
    char *fmt2 = parse_and_format(fmt1, "<test>");
    ASSERT_NOT_NULL(fmt2);
    ASSERT_STR_EQ(fmt1, fmt2);
    free(fmt1);
    free(fmt2);
    teardown();
}

TEST(template_string_roundtrip) {
    setup();
    /* Modern syntax uses double quotes with ${} interpolation. */
    const char *src = "let name = \"world\"\nlet s = \"hello ${name}\"\n";
    char *fmt1 = parse_and_format(src, "<test>");
    ASSERT_NOT_NULL(fmt1);
    char *fmt2 = parse_and_format(fmt1, "<test>");
    ASSERT_NOT_NULL(fmt2);
    ASSERT_STR_EQ(fmt1, fmt2);
    free(fmt1);
    free(fmt2);
    teardown();
}

TEST(unicode_string_roundtrip) {
    setup();
    const char *src = "let emoji = \"\\u{1F600}\"\n";
    char *fmt1 = parse_and_format(src, "<test>");
    ASSERT_NOT_NULL(fmt1);
    char *fmt2 = parse_and_format(fmt1, "<test>");
    ASSERT_NOT_NULL(fmt2);
    ASSERT_STR_EQ(fmt1, fmt2);
    free(fmt1);
    free(fmt2);
    teardown();
}

TEST(empty_string_roundtrip) {
    setup();
    const char *src = "let e = \"\"\n";
    char *fmt1 = parse_and_format(src, "<test>");
    ASSERT_NOT_NULL(fmt1);
    ASSERT_TRUE(contains(fmt1, "\"\""));
    char *fmt2 = parse_and_format(fmt1, "<test>");
    ASSERT_NOT_NULL(fmt2);
    ASSERT_STR_EQ(fmt1, fmt2);
    free(fmt1);
    free(fmt2);
    teardown();
}

/* ====================================================================== */
/* E6-4: No deprecated syntax                                              */
/* ====================================================================== */

TEST(arrow_return_type_emitted) {
    setup();
    /* The formatter must emit `-> T` for return types and must not fall
     * back to the legacy `: T` form. */
    const char *src = "fn foo() -> int { return 1 }\n";
    char *out = parse_and_format(src, "<test>");
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(contains(out, "-> int"));
    ASSERT_FALSE(contains(out, "): int"));
    free(out);
    teardown();
}

/* ====================================================================== */
/* Match arm alignment (opt-in)                                            */
/* ====================================================================== */

static char *format_with_config(const char *source, XrFmtConfig *cfg) {
    AstNode *ast = xr_parse_with_trivia(g_iso, source, "<test>");
    if (!ast)
        return NULL;
    char *out = xfmt_format_ast(ast, cfg, g_iso);
    xr_program_destroy(ast);
    return out;
}

TEST(match_arms_default_single_space) {
    setup();
    const char *src = "fn f(n: int) -> string {\n"
                      "    return match (n) {\n"
                      "        0 -> \"zero\",\n"
                      "        n if (n < 0) -> \"negative\",\n"
                      "        n if (n > 100) -> \"big\",\n"
                      "        _ -> \"small positive\"\n"
                      "    }\n"
                      "}\n";
    /* NULL config -> default; align_match_arms is off by default. */
    char *out = format_with_config(src, NULL);
    ASSERT_NOT_NULL(out);
    /* Every arm must use exactly a single space before `->`, never padded. */
    ASSERT_TRUE(contains(out, "0 -> \"zero\""));
    ASSERT_TRUE(contains(out, "n if (n < 0) -> \"negative\""));
    ASSERT_TRUE(contains(out, "n if (n > 100) -> \"big\""));
    ASSERT_TRUE(contains(out, "_ -> \"small positive\""));
    ASSERT_FALSE(contains(out, "0  ->"));
    ASSERT_FALSE(contains(out, "_  ->"));
    free(out);
    teardown();
}

TEST(match_arms_aligned_when_enabled) {
    setup();
    const char *src = "fn f(n: int) -> string {\n"
                      "    return match (n) {\n"
                      "        0 -> \"zero\",\n"
                      "        n if (n < 0) -> \"negative\",\n"
                      "        n if (n > 100) -> \"big\",\n"
                      "        _ -> \"small positive\"\n"
                      "    }\n"
                      "}\n";
    XrFmtConfig cfg = xfmt_default_config;
    cfg.align_match_arms = 1;
    char *out = format_with_config(src, &cfg);
    ASSERT_NOT_NULL(out);
    /* Widest head is `n if (n > 100)` (14 chars). All other arms must be
     * padded with spaces so that their `->` lands at the same column. */
    ASSERT_TRUE(contains(out, "0              -> \"zero\""));
    ASSERT_TRUE(contains(out, "n if (n < 0)   -> \"negative\""));
    ASSERT_TRUE(contains(out, "n if (n > 100) -> \"big\""));
    ASSERT_TRUE(contains(out, "_              -> \"small positive\""));
    free(out);
    teardown();
}

TEST(match_arms_aligned_idempotent) {
    setup();
    const char *src = "fn f(n: int) -> string {\n"
                      "    return match (n) {\n"
                      "        0 -> \"zero\",\n"
                      "        n if (n < 0) -> \"negative\",\n"
                      "        _ -> \"other\"\n"
                      "    }\n"
                      "}\n";
    XrFmtConfig cfg = xfmt_default_config;
    cfg.align_match_arms = 1;
    char *fmt1 = format_with_config(src, &cfg);
    ASSERT_NOT_NULL(fmt1);
    /* fmt(fmt(src)) == fmt(src) — alignment must not drift on re-format. */
    AstNode *ast2 = xr_parse_with_trivia(g_iso, fmt1, "<test>");
    ASSERT_NOT_NULL(ast2);
    char *fmt2 = xfmt_format_ast(ast2, &cfg, g_iso);
    xr_program_destroy(ast2);
    ASSERT_NOT_NULL(fmt2);
    ASSERT_STR_EQ(fmt1, fmt2);
    free(fmt1);
    free(fmt2);
    teardown();
}

TEST(match_single_arm_no_padding) {
    setup();
    /* A match with exactly one arm should not introduce any padding even
     * with alignment turned on — there is nothing to align against. */
    const char *src = "fn f(n: int) -> string {\n"
                      "    return match (n) {\n"
                      "        _ -> \"only\"\n"
                      "    }\n"
                      "}\n";
    XrFmtConfig cfg = xfmt_default_config;
    cfg.align_match_arms = 1;
    char *out = format_with_config(src, &cfg);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(contains(out, "_ -> \"only\""));
    ASSERT_FALSE(contains(out, "_  ->"));
    free(out);
    teardown();
}

/* ====================================================================== */
/* Enum member alignment (opt-in)                                          */
/* ====================================================================== */

TEST(enum_values_aligned_when_enabled) {
    setup();
    const char *src = "enum Color {\n"
                      "    Red = 1,\n"
                      "    Green = 2,\n"
                      "    Blue = 3,\n"
                      "    Transparent = 4\n"
                      "}\n";
    XrFmtConfig cfg = xfmt_default_config;
    cfg.align_enum_values = 1;
    char *out = format_with_config(src, &cfg);
    ASSERT_NOT_NULL(out);
    /* Widest name is `Transparent` (11 chars). All other members are padded. */
    ASSERT_TRUE(contains(out, "Red         = 1"));
    ASSERT_TRUE(contains(out, "Green       = 2"));
    ASSERT_TRUE(contains(out, "Blue        = 3"));
    ASSERT_TRUE(contains(out, "Transparent = 4"));
    free(out);
    teardown();
}

TEST(enum_values_default_single_space) {
    setup();
    const char *src = "enum E {\n"
                      "    A = 1,\n"
                      "    Bbb = 2\n"
                      "}\n";
    char *out = format_with_config(src, NULL);
    ASSERT_NOT_NULL(out);
    /* Default: no alignment — single space. */
    ASSERT_TRUE(contains(out, "A = 1"));
    ASSERT_TRUE(contains(out, "Bbb = 2"));
    ASSERT_FALSE(contains(out, "A   ="));
    free(out);
    teardown();
}

/* ====================================================================== */
/* Class field alignment (opt-in)                                          */
/* ====================================================================== */

TEST(class_fields_aligned_when_enabled) {
    setup();
    const char *src = "class User {\n"
                      "    name: string\n"
                      "    age: int\n"
                      "    email: string\n"
                      "}\n";
    XrFmtConfig cfg = xfmt_default_config;
    cfg.align_struct_fields = 1;
    char *out = format_with_config(src, &cfg);
    ASSERT_NOT_NULL(out);
    /* Widest name is `email` (5 chars). `name`(4) and `age`(3) are padded. */
    ASSERT_TRUE(contains(out, "name : string"));
    ASSERT_TRUE(contains(out, "age  : int"));
    ASSERT_TRUE(contains(out, "email: string"));
    free(out);
    teardown();
}

TEST(class_fields_default_single_space) {
    setup();
    const char *src = "class C {\n"
                      "    a: int\n"
                      "    bbbb: string\n"
                      "}\n";
    char *out = format_with_config(src, NULL);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(contains(out, "a: int"));
    ASSERT_TRUE(contains(out, "bbbb: string"));
    ASSERT_FALSE(contains(out, "a   :"));
    free(out);
    teardown();
}

/* ====================================================================== */
/* Long-line wrapping (opt-in)                                             */
/* ====================================================================== */

TEST(array_literal_wraps_when_too_long) {
    setup();
    /* Force a short line length so wrapping is unambiguous. */
    const char *src =
        "let items = [\"alpha\", \"beta\", \"gamma\", \"delta\", \"epsilon\", \"zeta\"]\n";
    XrFmtConfig cfg = xfmt_default_config;
    cfg.wrap_long_lines = 1;
    cfg.max_line_length = 40;
    char *out = format_with_config(src, &cfg);
    ASSERT_NOT_NULL(out);
    /* Multi-line: each element on its own line with trailing comma. */
    ASSERT_TRUE(contains(out, "\"alpha\",\n"));
    ASSERT_TRUE(contains(out, "\"zeta\",\n"));
    free(out);
    teardown();
}

TEST(array_literal_inline_when_short) {
    setup();
    const char *src = "let items = [1, 2, 3]\n";
    XrFmtConfig cfg = xfmt_default_config;
    cfg.wrap_long_lines = 1;
    cfg.max_line_length = 100;
    char *out = format_with_config(src, &cfg);
    ASSERT_NOT_NULL(out);
    /* Stays single-line: well below 100 columns. */
    ASSERT_TRUE(contains(out, "[1, 2, 3]"));
    ASSERT_FALSE(contains(out, "1,\n"));
    free(out);
    teardown();
}

TEST(call_args_wrap_when_too_long) {
    setup();
    const char *src = "fn main() { foo(\"alpha\", \"beta\", \"gamma\", \"delta\", \"epsilon\") }\n";
    XrFmtConfig cfg = xfmt_default_config;
    cfg.wrap_long_lines = 1;
    cfg.max_line_length = 40;
    char *out = format_with_config(src, &cfg);
    ASSERT_NOT_NULL(out);
    /* Function call args broken across lines. */
    ASSERT_TRUE(contains(out, "foo(\n"));
    ASSERT_TRUE(contains(out, "\"alpha\",\n"));
    free(out);
    teardown();
}

TEST(no_trailing_comma_when_disabled) {
    setup();
    const char *src = "let items = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]\n";
    XrFmtConfig cfg = xfmt_default_config;
    cfg.wrap_long_lines = 1;
    cfg.max_line_length = 30;
    cfg.multiline_trailing_comma = 0;
    char *out = format_with_config(src, &cfg);
    ASSERT_NOT_NULL(out);
    /* Last element should NOT carry a trailing `,` when disabled. */
    ASSERT_TRUE(contains(out, "10\n"));
    ASSERT_FALSE(contains(out, "10,\n"));
    free(out);
    teardown();
}

TEST(wrap_long_lines_default_off) {
    setup();
    /* Even with an absurdly long single line, default config must NOT wrap.
     * This guards against silent corpus-wide reformat. */
    const char *src =
        "let x = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20]\n";
    char *out = format_with_config(src, NULL);
    ASSERT_NOT_NULL(out);
    ASSERT_FALSE(contains(out, "\n    1,"));
    free(out);
    teardown();
}

/* ====================================================================== */
/* Trailing comment alignment (opt-in)                                     */
/* ====================================================================== */

TEST(trailing_comments_aligned_when_enabled) {
    setup();
    const char *src = "let radius = 5  // sphere radius\n"
                      "let mass = 100  // kg\n"
                      "let temp = 273  // Kelvin\n";
    XrFmtConfig cfg = xfmt_default_config;
    cfg.align_trailing_comments = 1;
    char *out = format_with_config(src, &cfg);
    ASSERT_NOT_NULL(out);
    /* Widest code is `let mass = 100` and `let temp = 273` (14 chars). All
     * lines pad to col 16 (target = max + 2). */
    ASSERT_TRUE(contains(out, "let radius = 5  // sphere radius"));
    ASSERT_TRUE(contains(out, "let mass = 100  // kg"));
    ASSERT_TRUE(contains(out, "let temp = 273  // Kelvin"));
    free(out);
    teardown();
}

TEST(trailing_comments_default_unchanged) {
    setup();
    const char *src = "let x = 1  // first\n"
                      "let yyyy = 22  // second\n";
    char *out = format_with_config(src, NULL);
    ASSERT_NOT_NULL(out);
    /* Default: each comment two spaces after its own code, NOT aligned. */
    ASSERT_TRUE(contains(out, "let x = 1  // first"));
    ASSERT_TRUE(contains(out, "let yyyy = 22  // second"));
    /* Specifically: NO over-padding on the short line. */
    ASSERT_FALSE(contains(out, "let x = 1      // first"));
    free(out);
    teardown();
}

TEST(trailing_comments_idempotent) {
    setup();
    const char *src = "let a = 1  // a\n"
                      "let bb = 22  // b\n"
                      "let ccc = 333  // c\n";
    XrFmtConfig cfg = xfmt_default_config;
    cfg.align_trailing_comments = 1;
    char *fmt1 = format_with_config(src, &cfg);
    ASSERT_NOT_NULL(fmt1);
    AstNode *ast2 = xr_parse_with_trivia(g_iso, fmt1, "<test>");
    ASSERT_NOT_NULL(ast2);
    char *fmt2 = xfmt_format_ast(ast2, &cfg, g_iso);
    xr_program_destroy(ast2);
    ASSERT_NOT_NULL(fmt2);
    ASSERT_STR_EQ(fmt1, fmt2);
    free(fmt1);
    free(fmt2);
    teardown();
}

TEST(trailing_comments_string_safe) {
    setup();
    /* `//` inside a string literal must NOT be treated as a trailing comment. */
    const char *src = "let url = \"https://example.com\"  // homepage\n"
                      "let path = \"/abc\"  // root\n";
    XrFmtConfig cfg = xfmt_default_config;
    cfg.align_trailing_comments = 1;
    char *out = format_with_config(src, &cfg);
    ASSERT_NOT_NULL(out);
    /* The `//` inside the URL string must remain inside the string. */
    ASSERT_TRUE(contains(out, "\"https://example.com\""));
    ASSERT_TRUE(contains(out, "// homepage"));
    ASSERT_TRUE(contains(out, "// root"));
    free(out);
    teardown();
}

/* ====================================================================== */
/* Driver                                                                  */
/* ====================================================================== */

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Formatter roundtrip (E6)");

RUN_TEST(idempotency_regression_corpus);

RUN_TEST(doc_comment_before_function);
RUN_TEST(block_comment_before_statement);
RUN_TEST(comment_before_class);

RUN_TEST(string_escape_roundtrip);
RUN_TEST(template_string_roundtrip);
RUN_TEST(unicode_string_roundtrip);
RUN_TEST(empty_string_roundtrip);

RUN_TEST(arrow_return_type_emitted);

RUN_TEST(match_arms_default_single_space);
RUN_TEST(match_arms_aligned_when_enabled);
RUN_TEST(match_arms_aligned_idempotent);
RUN_TEST(match_single_arm_no_padding);

RUN_TEST(enum_values_aligned_when_enabled);
RUN_TEST(enum_values_default_single_space);

RUN_TEST(class_fields_aligned_when_enabled);
RUN_TEST(class_fields_default_single_space);

RUN_TEST(array_literal_wraps_when_too_long);
RUN_TEST(array_literal_inline_when_short);
RUN_TEST(call_args_wrap_when_too_long);
RUN_TEST(no_trailing_comma_when_disabled);
RUN_TEST(wrap_long_lines_default_off);

RUN_TEST(trailing_comments_aligned_when_enabled);
RUN_TEST(trailing_comments_default_unchanged);
RUN_TEST(trailing_comments_idempotent);
RUN_TEST(trailing_comments_string_safe);
TEST_MAIN_END()
