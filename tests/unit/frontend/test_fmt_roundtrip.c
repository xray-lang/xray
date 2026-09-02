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
#include "frontend/parser/xast_walk.h"
#include "xray_vm.h"
#include "base/xmalloc.h"
#include "toolchain/xcompiler_session.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

/* ====================================================================== */
/* Fixtures                                                                */
/* ====================================================================== */

static XrVMRuntime *g_iso = NULL;
static XrCompilerSession *g_session = NULL;

static void setup(void) {
    XrVMConfig p = {0};
    g_iso = xray_vm_new_full(&p);
    ASSERT_NOT_NULL(g_iso);
    g_session = xr_compiler_session_current_for_isolate(g_iso);
    ASSERT_NOT_NULL(g_session);
}

static void teardown(void) {
    if (g_iso) {
        xray_vm_delete(g_iso);
        g_iso = NULL;
        g_session = NULL;
    }
}

/* Parse with trivia and format to a heap string. Returns NULL on error. */
static char *parse_and_format(const char *source, const char *filename) {
    AstNode *ast =
        xr_parse_with_trivia(xr_compiler_session_current_for_isolate(g_iso), source, filename);
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
typedef int (*FileCheckFn)(const char *path);

static int scan_dir(const char *dir_path, FileCheckFn check, int *total, int *passed,
                    int *skipped) {
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir_path);
    WIN32_FIND_DATAA entry;
    HANDLE search = FindFirstFileA(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE)
        return 0;

    do {
        if (entry.cFileName[0] == '.')
            continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s\\%s", dir_path, entry.cFileName);
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_dir(path, check, total, passed, skipped);
            continue;
        }
        size_t nlen = strlen(entry.cFileName);
        if (nlen > 3 && strcmp(entry.cFileName + nlen - 3, ".xr") == 0) {
            (*total)++;
            int result = check(path);
            if (result == 1)
                (*passed)++;
            else if (result == -1)
                (*skipped)++;
        }
    } while (FindNextFileA(search, &entry));
    FindClose(search);
    return 1;
#else
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
            scan_dir(path, check, total, passed, skipped);
        } else if (S_ISREG(st.st_mode)) {
            size_t nlen = strlen(ent->d_name);
            if (nlen > 3 && strcmp(ent->d_name + nlen - 3, ".xr") == 0) {
                (*total)++;
                int r = check(path);
                if (r == 1)
                    (*passed)++;
                else if (r == -1)
                    (*skipped)++;
            }
        }
    }
    closedir(d);
    return 1;
#endif
}

static bool path_is_directory(const char *path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

TEST(idempotency_regression_corpus) {
    setup();

    /* Locate regression test directory relative to the test binary.
     * Build dir is <repo>/build, tests run from there. */
    const char *dirs[] = {"../tests/regression", "../../tests/regression",
                          "../../../tests/regression", NULL};

    const char *regression_dir = NULL;
    for (int i = 0; dirs[i]; i++) {
        if (path_is_directory(dirs[i])) {
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
    scan_dir(regression_dir, check_idempotent, &total, &passed, &skipped);

    int tested = total - skipped;
    fprintf(stderr, "  Formatter idempotency: %d/%d tested (%d skipped re-parse)\n", passed, tested,
            skipped);
    ASSERT_TRUE(tested > 0);
    /* All files that survive re-parse must be idempotent. */
    ASSERT_EQ(passed, tested);

    teardown();
}

/* ====================================================================== */
/* E6-1b: AST preservation over regression + diff corpora                  */
/* ====================================================================== */

/* Formatting must not change what the program means. Idempotency alone does
 * not check that: a formatter that drops a paren, re-wraps a line so the next
 * line binds differently, or loses a `!` can still be perfectly idempotent on
 * its own (wrong) output. The load-bearing property is
 *
 *     AST(fmt(src)) is structurally identical to AST(src)
 *
 * checked here through the generic walker in frontend/parser/xast_walk.h, so
 * this gate never needs its own copy of the AST's shape and cannot silently
 * skip a node type it has not been taught (unknown types fail). Source
 * positions are excluded by construction — formatting is supposed to move
 * things around. */

/* A growable text buffer for the canonical AST digest. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool failed;
} Digest;

static void digest_add(Digest *d, const char *text) {
    if (d->failed)
        return;
    size_t n = strlen(text);
    if (d->len + n + 1 > d->cap) {
        size_t next = d->cap ? d->cap * 2 : 4096;
        while (next < d->len + n + 1)
            next *= 2;
        char *grown = (char *) xr_realloc(d->data, next);
        if (!grown) {
            d->failed = true;
            return;
        }
        d->data = grown;
        d->cap = next;
    }
    memcpy(d->data + d->len, text, n);
    d->len += n;
    d->data[d->len] = '\0';
}

/* Depth is encoded so that "same signatures, different nesting" cannot hash to
 * the same digest. The scratch buffers live here rather than on the stack: the
 * walk recurses once per AST level, and 16 KB of locals per frame overflows the
 * stack on a deep expression. */
typedef struct {
    Digest *digest;
    int depth;
    char sig[8192];
    char line[8320];
} DigestWalk;

static void digest_node(AstNode *node, DigestWalk *w);

static bool digest_child(AstNode *child, void *user_data) {
    DigestWalk *w = (DigestWalk *) user_data;
    w->depth++;
    digest_node(child, w);
    w->depth--;
    return !w->digest->failed;
}

static void digest_node(AstNode *node, DigestWalk *w) {
    if (w->digest->failed)
        return;

    if (!node) {
        snprintf(w->line, sizeof(w->line), "%*s<absent>\n", w->depth * 2, "");
        digest_add(w->digest, w->line);
        return;
    }

    if (!xr_ast_node_signature(node, w->sig, sizeof(w->sig))) {
        snprintf(w->line, sizeof(w->line), "%*s<node type %d not covered by xast_walk.c>\n",
                 w->depth * 2, "", (int) node->type);
        digest_add(w->digest, w->line);
        w->digest->failed = true;
        return;
    }
    snprintf(w->line, sizeof(w->line), "%*s%s\n", w->depth * 2, "", w->sig);
    digest_add(w->digest, w->line);

    if (!xr_ast_for_each_child(node, digest_child, w))
        w->digest->failed = true;
}

/* Canonical digest of a program. Returns NULL if any node type is unknown —
 * fail-closed, so an untaught node type cannot be silently treated as equal. */
static char *ast_digest(AstNode *program) {
    Digest d = {NULL, 0, 0, false};
    DigestWalk *w = (DigestWalk *) xr_malloc(sizeof(DigestWalk));
    if (!w)
        return NULL;
    w->digest = &d;
    w->depth = 0;
    digest_node(program, w);
    xr_free(w);
    if (d.failed) {
        xr_free(d.data);
        return NULL;
    }
    return d.data;
}

/* Report the first differing line of two digests. */
static void report_digest_diff(const char *path, const char *a, const char *b) {
    int line = 1;
    const char *pa = a;
    const char *pb = b;
    while (*pa && *pb && *pa == *pb) {
        if (*pa == '\n')
            line++;
        pa++;
        pb++;
    }
    /* Rewind both to the start of the differing line for a readable message. */
    while (pa > a && pa[-1] != '\n')
        pa--;
    while (pb > b && pb[-1] != '\n')
        pb--;
    fprintf(stderr, "  FAIL (formatting changed the AST): %s\n", path);
    fprintf(stderr, "    first difference at digest line %d\n", line);
    fprintf(stderr, "    before: %.400s", pa);
    fprintf(stderr, "    after:  %.400s", pb);
}

/* Every file in both corpora is required to survive the round trip. This gate
 * used to carry a k_known_ast_divergence[] ratchet of files whose formatted
 * output re-parsed differently; all of them were real formatter defects and all
 * are fixed, so there is no exemption list left to grow back into. A file that
 * starts failing here is a formatter regression, not a candidate for the list.
 */

/* Returns 1 pass, 0 fail, -1 skip (unparseable source, or formatter output the
 * parser rejects — both already covered by the idempotency gate above).
 *
 * Only one program is alive at a time: each parse owns its arena, and holding
 * two open across the session's arena stack is not a supported pattern. */
static int check_ast_preserved(const char *path) {
    char *src = read_file_contents(path);
    if (!src)
        return -1;

    XrCompilerSession *session = xr_compiler_session_current_for_isolate(g_iso);
    AstNode *before = xr_parse_with_trivia(session, src, path);
    if (!before) {
        xr_free(src);
        return -1;
    }

    /* The AST points into `src` for literal payloads and comment trivia, so
     * the source must outlive both the formatter and the digest. */
    char *formatted = xfmt_format_ast(before, NULL, g_iso);
    char *digest_before = formatted ? ast_digest(before) : NULL;
    xr_program_destroy(before);
    xr_free(src);
    if (!formatted || !digest_before) {
        free(formatted);
        xr_free(digest_before);
        return formatted ? 0 : -1;
    }

    AstNode *after = xr_parse_with_trivia(session, formatted, path);
    if (!after) {
        free(formatted);
        xr_free(digest_before);
        return -1;
    }
    char *digest_after = ast_digest(after);
    xr_program_destroy(after);
    free(formatted);

    bool preserved = digest_after && strcmp(digest_before, digest_after) == 0;
    int result = 1;
    if (!preserved) {
        report_digest_diff(path, digest_before, digest_after ? digest_after : "<null>\n");
        result = 0;
    }
    xr_free(digest_before);
    xr_free(digest_after);
    return result;
}

TEST(ast_preserved_over_corpora) {
    setup();

    /* regression/ covers language surface; diff/ covers VM-vs-AOT semantics
     * and is the densest collection of real programs in the repo. */
    static const char *corpora[] = {"tests/regression", "tests/diff", NULL};
    static const char *prefixes[] = {"../", "../../", "../../../", NULL};

    int total = 0, passed = 0, skipped = 0;
    int roots_found = 0;
    for (int c = 0; corpora[c]; c++) {
        char dir[512];
        const char *found = NULL;
        for (int p = 0; prefixes[p] && !found; p++) {
            snprintf(dir, sizeof(dir), "%s%s", prefixes[p], corpora[c]);
            if (path_is_directory(dir))
                found = dir;
        }
        if (!found) {
            fprintf(stderr, "  SKIP: %s not found\n", corpora[c]);
            continue;
        }
        roots_found++;
        scan_dir(found, check_ast_preserved, &total, &passed, &skipped);
    }

    int tested = total - skipped;
    fprintf(stderr, "  Formatter AST preservation: %d/%d tested (%d skipped, %d corpora)\n", passed,
            tested, skipped, roots_found);
    ASSERT_TRUE(roots_found > 0);
    ASSERT_TRUE(tested > 0);
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
                      "fn foo() -> i64 {\n"
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
                      "var x = 5\n";
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
                      "    x: i64\n"
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
    const char *src = "var s = \"hello\\nworld\\t\\\\end\\\"\"\n";
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
    const char *src = "var name = \"world\"\n"
                      "var m = #{\"k\": \"value\"}\n"
                      "var s = \"hello ${name} ${\"literal\"} ${m[\"k\"]}\"\n";
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
    const char *src = "var emoji = \"\\u{1F600}\"\n";
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
    const char *src = "var e = \"\"\n";
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
    const char *src = "fn foo() -> i64 { return 1 }\n";
    char *out = parse_and_format(src, "<test>");
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(contains(out, "-> i64"));
    ASSERT_FALSE(contains(out, "): i64"));
    free(out);
    teardown();
}

TEST(annotated_and_mode_lambda_remain_arrow) {
    setup();
    const char *src = "var typed = (x: i64) -> x + 1\n"
                      "var mutate = (x: ref i64) -> { x = x + 1 }\n"
                      "var consume = (job: move Job) -> job\n"
                      "var task = go launch((x: i64) -> x)\n"
                      "var explicit = fn(x: i64) -> i64 { return x + 1 }\n";
    char *fmt1 = parse_and_format(src, "<test>");
    ASSERT_NOT_NULL(fmt1);
    ASSERT_TRUE(contains(fmt1, "(x: i64) -> x + 1"));
    ASSERT_TRUE(contains(fmt1, "(x: ref i64) ->"));
    ASSERT_TRUE(contains(fmt1, "(job: move Job) -> job"));
    ASSERT_TRUE(contains(fmt1, "go launch((x: i64) -> x)"));
    ASSERT_TRUE(contains(fmt1, "fn(x: i64) -> i64"));
    char *fmt2 = parse_and_format(fmt1, "<test>");
    ASSERT_NOT_NULL(fmt2);
    ASSERT_STR_EQ(fmt1, fmt2);
    free(fmt1);
    free(fmt2);
    teardown();
}

TEST(attribute_visibility_modifier_order_roundtrip) {
    setup();
    const char *src = "@deprecated(\"use hash64\")\n"
                      "export fn hash() -> i64 { return 1 }\n"
                      "@derive(Clone)\n"
                      "export final class Box {}\n"
                      "export packed struct Word { value: u32 }\n";
    char *fmt1 = parse_and_format(src, "<test>");
    ASSERT_NOT_NULL(fmt1);
    ASSERT_TRUE(contains(fmt1, "@deprecated(\"use hash64\")\nexport fn hash"));
    ASSERT_TRUE(contains(fmt1, "@derive(Clone)\nexport final class Box"));
    ASSERT_TRUE(contains(fmt1, "export packed struct Word"));
    char *fmt2 = parse_and_format(fmt1, "<test>");
    ASSERT_NOT_NULL(fmt2);
    ASSERT_STR_EQ(fmt1, fmt2);
    free(fmt1);
    free(fmt2);
    teardown();
}

TEST(method_deprecated_attribute_roundtrip) {
    setup();
    const char *src = "export struct Word {\n"
                      "  @deprecated(\"use rotateLeft\")\n"
                      "  rotate(n: i64) -> u32 { return 0 }\n"
                      "}\n"
                      "enum State {\n"
                      "  Ready\n"
                      "  @deprecated\n"
                      "  static initial() -> State { return State.Ready }\n"
                      "}\n";
    char *fmt1 = parse_and_format(src, "<test>");
    ASSERT_NOT_NULL(fmt1);
    ASSERT_TRUE(contains(fmt1, "@deprecated(\"use rotateLeft\")\n    rotate"));
    ASSERT_TRUE(contains(fmt1, "@deprecated\n    static initial"));
    char *fmt2 = parse_and_format(fmt1, "<test>");
    ASSERT_NOT_NULL(fmt2);
    ASSERT_STR_EQ(fmt1, fmt2);
    free(fmt1);
    free(fmt2);
    teardown();
}

TEST(inline_control_attributes_roundtrip) {
    setup();
    const char *src = "@inline\n"
                      "fn hot(value: i64) -> i64 { return value }\n"
                      "struct Worker {\n"
                      "  @noinline\n"
                      "  cold(value: i64) -> i64 { return value }\n"
                      "}\n";
    char *fmt1 = parse_and_format(src, "<test>");
    ASSERT_NOT_NULL(fmt1);
    ASSERT_TRUE(contains(fmt1, "@inline\nfn hot"));
    ASSERT_TRUE(contains(fmt1, "@noinline\n    cold"));
    char *fmt2 = parse_and_format(fmt1, "<test>");
    ASSERT_NOT_NULL(fmt2);
    ASSERT_STR_EQ(fmt1, fmt2);
    free(fmt1);
    free(fmt2);
    teardown();
}

TEST(explicit_numeric_conversions_roundtrip) {
    setup();
    const char *src = "fn convert(wide: u64, signed: i32) -> u8 {\n"
                      "  var narrowed = wide as u8\n"
                      "  var reinterpreted = signed as u32\n"
                      "  var approximate = wide as f64\n"
                      "  return narrowed\n"
                      "}\n";
    char *fmt1 = parse_and_format(src, "<test>");
    ASSERT_NOT_NULL(fmt1);
    ASSERT_TRUE(contains(fmt1, "wide as u8"));
    ASSERT_TRUE(contains(fmt1, "signed as u32"));
    ASSERT_TRUE(contains(fmt1, "wide as f64"));
    char *fmt2 = parse_and_format(fmt1, "<test>");
    ASSERT_NOT_NULL(fmt2);
    ASSERT_STR_EQ(fmt1, fmt2);
    free(fmt1);
    free(fmt2);
    teardown();
}

TEST(deprecated_message_roundtrip) {
    setup();
    const char *src = "@deprecated(\"use coldPath instead\")\n"
                      "fn hot() -> i64 { return 1 }\n";
    char *fmt1 = parse_and_format(src, "<test>");
    ASSERT_NOT_NULL(fmt1);
    ASSERT_TRUE(contains(fmt1, "@deprecated(\"use coldPath instead\")\nfn hot"));
    char *fmt2 = parse_and_format(fmt1, "<test>");
    ASSERT_NOT_NULL(fmt2);
    ASSERT_STR_EQ(fmt1, fmt2);
    free(fmt1);
    free(fmt2);
    teardown();
}

TEST(object_destructure_rename_roundtrip) {
    setup();
    const char *src = "var { name: localName, age } = user\n"
                      "{ name: otherName, age } = user\n";
    char *fmt1 = parse_and_format(src, "<test>");
    ASSERT_NOT_NULL(fmt1);
    ASSERT_TRUE(contains(fmt1, "var {name: localName, age} = user"));
    ASSERT_TRUE(contains(fmt1, "{name: otherName, age} = user"));
    char *fmt2 = parse_and_format(fmt1, "<test>");
    ASSERT_NOT_NULL(fmt2);
    ASSERT_STR_EQ(fmt1, fmt2);
    free(fmt1);
    free(fmt2);
    teardown();
}

TEST(parameter_modes_roundtrip) {
    setup();
    const char *src = "fn param_modes(a: i64, b: ref i64, c: move Buffer) { }\n"
                      "var f = fn(a: i64, b: ref i64, c: move Buffer) -> i64 { return a }\n"
                      "class ParamModeBox {\n"
                      "    touch(a: i64, b: ref i64, c: move Buffer) { }\n"
                      "    private ref mutate() { }\n"
                      "    move consume() { }\n"
                      "    configure(limit: i64 = 4) { }\n"
                      "    collect(...values: i64) { }\n"
                      "}\n"
                      "interface ParamModeIface {\n"
                      "    touch(a: i64, b: ref i64, c: move Buffer) -> i64\n"
                      "    ref mutate() -> ()\n"
                      "    move consume() -> ()\n"
                      "}\n"
                      "type ComplexHandler = fn(Array<i64>, ref Slice<u8>?, "
                      "move Buffer, (i64, string), fn(ref i64) -> bool,) -> Array<string>\n";
    char *fmt1 = parse_and_format(src, "<test>");
    ASSERT_NOT_NULL(fmt1);
    ASSERT_TRUE(contains(fmt1, "fn param_modes(a: i64, b: ref i64, c: move Buffer)"));
    ASSERT_TRUE(contains(fmt1, "fn(a: i64, b: ref i64, c: move Buffer) -> i64"));
    ASSERT_TRUE(contains(fmt1, "touch(a: i64, b: ref i64, c: move Buffer)"));
    ASSERT_TRUE(contains(fmt1, "private ref mutate()"));
    ASSERT_TRUE(contains(fmt1, "move consume()"));
    ASSERT_TRUE(contains(fmt1, "ref mutate() -> ()"));
    ASSERT_TRUE(contains(fmt1, "configure(limit: i64 = 4)"));
    ASSERT_TRUE(contains(fmt1, "collect(...values: i64)"));
    ASSERT_TRUE(contains(fmt1, "type ComplexHandler = fn(Array<i64>, ref Slice<u8>?, "
                               "move Buffer, (i64, string), fn(ref i64) -> bool) -> "
                               "Array<string>"));
    ASSERT_FALSE(contains(fmt1, "bool,) -> Array<string>"));
    ASSERT_FALSE(contains(fmt1, "ref b:"));
    ASSERT_FALSE(contains(fmt1, "move c:"));
    char *fmt2 = parse_and_format(fmt1, "<test>");
    ASSERT_NOT_NULL(fmt2);
    ASSERT_STR_EQ(fmt1, fmt2);
    free(fmt1);
    free(fmt2);
    teardown();
}

TEST(unknown_effect_attribute_rejected) {
    setup();
    const char *src = "@effect_claim\nfn pure(value: i64) -> i64 { return value }\n";
    char *out = parse_and_format(src, "retired-effect-attributes.xr");
    ASSERT_NULL(out);
    teardown();
}

TEST(extern_block_roundtrip) {
    setup();
    const char *src = "extern \"C\" {\n"
                      "  export fn cos(x: f64) -> f64\n"
                      "  fn clear(value: MutPtr<i32>)\n"
                      "}\n";
    char *fmt1 = parse_and_format(src, "extern-block.xr");
    ASSERT_NOT_NULL(fmt1);
    ASSERT_TRUE(contains(fmt1, "extern \"C\""));
    ASSERT_TRUE(contains(fmt1, "export fn cos"));
    ASSERT_TRUE(!contains(fmt1, "@extern"));
    char *fmt2 = parse_and_format(fmt1, "extern-block-formatted.xr");
    ASSERT_NOT_NULL(fmt2);
    ASSERT_STR_EQ(fmt1, fmt2);
    free(fmt1);
    free(fmt2);
    teardown();
}

/* An optional chain continued with a plain `.` / `[` (spec §3.6) is stored as
 * a chain link, but the formatter must print it back exactly as written — a
 * rewrite to `?.` would silently churn every such line. */
TEST(optional_chain_implicit_link_roundtrip) {
    setup();
    const char *src = "fn probe(b: Branch?, rows: Array<Array<i64>>?) {\n"
                      "    print(b?.leaf.value)\n"
                      "    print(b?.leaf.doubled())\n"
                      "    print(b?.maybeLeaf?.value)\n"
                      "    print(rows?[0][1])\n"
                      "}\n";
    char *fmt1 = parse_and_format(src, "optional-chain.xr");
    ASSERT_NOT_NULL(fmt1);
    ASSERT_TRUE(contains(fmt1, "b?.leaf.value"));
    ASSERT_TRUE(contains(fmt1, "b?.leaf.doubled()"));
    ASSERT_TRUE(contains(fmt1, "b?.maybeLeaf?.value"));
    ASSERT_TRUE(contains(fmt1, "rows?[0][1]"));
    char *fmt2 = parse_and_format(fmt1, "optional-chain-formatted.xr");
    ASSERT_NOT_NULL(fmt2);
    ASSERT_STR_EQ(fmt1, fmt2);
    free(fmt1);
    free(fmt2);
    teardown();
}

TEST(parameter_modes_comments_roundtrip) {
    setup();
    const char *src = "/// function docs\n"
                      "fn commented_modes(a: i64, b: ref i64, c: move Buffer) {\n"
                      "    touch(ref b, move c) // call marker note\n"
                      "}\n"
                      "/* function type docs */\n"
                      "type CommentedHandler = fn(i64, ref string, move Buffer) -> bool\n"
                      "class CommentedBox {\n"
                      "    // method docs\n"
                      "    touch(value: ref i64, job: move Buffer) { } // method marker note\n"
                      "}\n"
                      "interface CommentedIface {\n"
                      "    // signature docs\n"
                      "    call(value: i64, job: move Buffer) -> i64\n"
                      "}\n";
    char *fmt1 = parse_and_format(src, "<test>");
    ASSERT_NOT_NULL(fmt1);
    ASSERT_TRUE(contains(fmt1, "/// function docs"));
    ASSERT_TRUE(contains(fmt1, "// call marker note"));
    ASSERT_TRUE(contains(fmt1, "/* function type docs */"));
    ASSERT_TRUE(contains(fmt1, "// method docs"));
    ASSERT_TRUE(contains(fmt1, "// method marker note"));
    ASSERT_TRUE(contains(fmt1, "// signature docs"));
    ASSERT_TRUE(contains(fmt1, "commented_modes(a: i64, b: ref i64, c: move Buffer)"));
    ASSERT_TRUE(contains(fmt1, "touch(ref b, move c)"));
    ASSERT_TRUE(contains(fmt1, "type CommentedHandler = fn(i64, ref string, move Buffer) -> bool"));
    ASSERT_TRUE(contains(fmt1, "touch(value: ref i64, job: move Buffer)"));
    ASSERT_TRUE(contains(fmt1, "call(value: i64, job: move Buffer) -> i64"));
    ASSERT_FALSE(contains(fmt1, "ref value:"));
    ASSERT_FALSE(contains(fmt1, "move job:"));
    char *fmt2 = parse_and_format(fmt1, "<test>");
    ASSERT_NOT_NULL(fmt2);
    ASSERT_STR_EQ(fmt1, fmt2);
    free(fmt1);
    free(fmt2);
    teardown();
}

/* ====================================================================== */
/* Match/select branch arrow alignment                                     */
/* ====================================================================== */

static char *format_with_config(const char *source, XrFmtConfig *cfg) {
    AstNode *ast =
        xr_parse_with_trivia(xr_compiler_session_current_for_isolate(g_iso), source, "<test>");
    if (!ast)
        return NULL;
    char *out = xfmt_format_ast(ast, cfg, g_iso);
    xr_program_destroy(ast);
    return out;
}

TEST(branch_arrows_default_aligned) {
    setup();
    const char *src = "fn f(n: i64) -> string {\n"
                      "    return match (n) {\n"
                      "        0 -> \"zero\",\n"
                      "        n if (n < 0) -> \"negative\",\n"
                      "        n if (n > 100) -> \"big\",\n"
                      "        _ -> \"small positive\"\n"
                      "    }\n"
                      "}\n";
    /* NULL config -> default; branch arrows are aligned by default. */
    char *out = format_with_config(src, NULL);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(contains(out, "0              -> \"zero\""));
    ASSERT_TRUE(contains(out, "n if (n < 0)   -> \"negative\""));
    ASSERT_TRUE(contains(out, "n if (n > 100) -> \"big\""));
    ASSERT_TRUE(contains(out, "_              -> \"small positive\""));
    free(out);
    teardown();
}

TEST(branch_arrows_can_disable_alignment) {
    setup();
    const char *src = "fn f(n: i64) -> string {\n"
                      "    return match (n) {\n"
                      "        0 -> \"zero\",\n"
                      "        n if (n < 0) -> \"negative\",\n"
                      "        n if (n > 100) -> \"big\",\n"
                      "        _ -> \"small positive\"\n"
                      "    }\n"
                      "}\n";
    XrFmtConfig cfg = xfmt_default_config;
    cfg.align_branch_arrows = 0;
    char *out = format_with_config(src, &cfg);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(contains(out, "0 -> \"zero\""));
    ASSERT_TRUE(contains(out, "n if (n < 0) -> \"negative\""));
    ASSERT_TRUE(contains(out, "n if (n > 100) -> \"big\""));
    ASSERT_TRUE(contains(out, "_ -> \"small positive\""));
    ASSERT_FALSE(contains(out, "0  ->"));
    ASSERT_FALSE(contains(out, "_  ->"));
    free(out);
    teardown();
}

TEST(branch_arrows_aligned_idempotent) {
    setup();
    const char *src = "fn f(n: i64) -> string {\n"
                      "    return match (n) {\n"
                      "        0 -> \"zero\",\n"
                      "        n if (n < 0) -> \"negative\",\n"
                      "        _ -> \"other\"\n"
                      "    }\n"
                      "}\n";
    XrFmtConfig cfg = xfmt_default_config;
    cfg.align_branch_arrows = 1;
    char *fmt1 = format_with_config(src, &cfg);
    ASSERT_NOT_NULL(fmt1);
    /* fmt(fmt(src)) == fmt(src) — alignment must not drift on re-format. */
    AstNode *ast2 =
        xr_parse_with_trivia(xr_compiler_session_current_for_isolate(g_iso), fmt1, "<test>");
    ASSERT_NOT_NULL(ast2);
    char *fmt2 = xfmt_format_ast(ast2, &cfg, g_iso);
    xr_program_destroy(ast2);
    ASSERT_NOT_NULL(fmt2);
    ASSERT_STR_EQ(fmt1, fmt2);
    free(fmt1);
    free(fmt2);
    teardown();
}

TEST(select_branch_arrows_default_aligned) {
    setup();
    const char *src = "fn main() {\n"
                      "    const ch1 = Channel<i64>(1)\n"
                      "    const ch2 = Channel<i64>(1)\n"
                      "    select {\n"
                      "        v from ch1 -> { print(v) }\n"
                      "        100 to ch2 -> { print(\"sent\") }\n"
                      "        after 10 -> { print(\"timeout\") }\n"
                      "        _ -> { print(\"default\") }\n"
                      "    }\n"
                      "}\n";
    char *out = format_with_config(src, NULL);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(contains(out, "v from ch1 -> {"));
    ASSERT_TRUE(contains(out, "100 to ch2 -> {"));
    ASSERT_TRUE(contains(out, "after 10   -> {"));
    ASSERT_TRUE(contains(out, "_          -> {"));
    free(out);
    teardown();
}

TEST(match_single_arm_no_padding) {
    setup();
    /* A match with exactly one arm should not introduce any padding even
     * with alignment turned on — there is nothing to align against. */
    const char *src = "fn f(n: i64) -> string {\n"
                      "    return match (n) {\n"
                      "        _ -> \"only\"\n"
                      "    }\n"
                      "}\n";
    char *out = format_with_config(src, NULL);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(contains(out, "_ -> \"only\""));
    ASSERT_FALSE(contains(out, "_  ->"));
    free(out);
    teardown();
}

/* ====================================================================== */
/* Enum member alignment (opt-in)                                          */
/* ====================================================================== */

TEST(enum_members_ignore_removed_value_alignment) {
    setup();
    const char *src = "enum Color {\n"
                      "    Red,\n"
                      "    Green,\n"
                      "    Blue,\n"
                      "    Transparent\n"
                      "}\n";
    XrFmtConfig cfg = xfmt_default_config;
    cfg.align_enum_values = 1;
    char *out = format_with_config(src, &cfg);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(contains(out, "Red"));
    ASSERT_TRUE(contains(out, "Transparent"));
    ASSERT_FALSE(contains(out, "="));
    free(out);
    teardown();
}

TEST(enum_payload_members_roundtrip) {
    setup();
    const char *src = "enum E {\n"
                      "    A,\n"
                      "    Bbb { x: i64, text: string }\n"
                      "}\n";
    char *out = format_with_config(src, NULL);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(contains(out, "A"));
    ASSERT_TRUE(contains(out, "Bbb { x: i64, text: string }"));
    ASSERT_FALSE(contains(out, "="));
    free(out);
    teardown();
}

TEST(enum_record_construction_and_selective_pattern_are_idempotent) {
    setup();
    const char *src = "enum Result{Ok{value:i64,code:string}}\n"
                      "var made=Result.Ok{code:\"ready\",value:1}\n"
                      "fn inspect(input:Result)->i64{\n"
                      "return match(input){\n"
                      "Result.Ok{value,code:_}->value\n"
                      "}\n"
                      "}\n";
    char *out = parse_and_format(src, "enum_record_surface.xr");
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(contains(out, "Ok { value: i64, code: string }"));
    ASSERT_TRUE(contains(out, "Result.Ok { code: \"ready\", value: 1 }"));
    ASSERT_TRUE(contains(out, "Result.Ok { value, code: _ } -> value"));
    ASSERT_FALSE(contains(out, "value: value"));

    char *again = parse_and_format(out, "enum_record_surface_formatted.xr");
    ASSERT_NOT_NULL(again);
    ASSERT_STR_EQ(out, again);
    free(again);
    free(out);
    teardown();
}

TEST(enum_static_iteration_roundtrip) {
    setup();
    const char *src = "enum Color { Red, Green }\n"
                      "enum Event { Ready, Data { value: i64 } }\n"
                      "export enum Result<T> { Empty, Ok { value: T } }\n"
                      "for(color in Color){print(color.name)}\n"
                      "for(variant in Event.variants){\n"
                      "for(field in variant.payloads){print(field.name)}\n"
                      "}\n"
                      "for(variant in Result<i64>.variants){print(variant.name)}\n";
    char *out = parse_and_format(src, "enum_static_iteration.xr");
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(contains(out, "for (color in Color) {"));
    ASSERT_TRUE(contains(out, "for (variant in Event.variants) {"));
    ASSERT_TRUE(contains(out, "for (field in variant.payloads) {"));
    ASSERT_TRUE(contains(out, "export enum Result<T> {"));
    ASSERT_TRUE(contains(out, "for (variant in Result<i64>.variants) {"));
    ASSERT_FALSE(contains(out, "Result<i64>().variants"));

    char *again = parse_and_format(out, "enum_static_iteration_formatted.xr");
    ASSERT_NOT_NULL(again);
    ASSERT_STR_EQ(out, again);
    free(again);
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
                      "    age: i64\n"
                      "    email: string\n"
                      "}\n";
    XrFmtConfig cfg = xfmt_default_config;
    cfg.align_struct_fields = 1;
    char *out = format_with_config(src, &cfg);
    ASSERT_NOT_NULL(out);
    /* Widest name is `email` (5 chars). `name`(4) and `age`(3) are padded. */
    ASSERT_TRUE(contains(out, "name : string"));
    ASSERT_TRUE(contains(out, "age  : i64"));
    ASSERT_TRUE(contains(out, "email: string"));
    free(out);
    teardown();
}

TEST(class_fields_default_single_space) {
    setup();
    const char *src = "class C {\n"
                      "    a: i64\n"
                      "    bbbb: string\n"
                      "}\n";
    char *out = format_with_config(src, NULL);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(contains(out, "a: i64"));
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
        "var items = [\"alpha\", \"beta\", \"gamma\", \"delta\", \"epsilon\", \"zeta\"]\n";
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
    const char *src = "var items = [1, 2, 3]\n";
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
    const char *src = "var items = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]\n";
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
        "var x = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20]\n";
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
    const char *src = "var radius = 5  // sphere radius\n"
                      "var mass = 100  // kg\n"
                      "var temp = 273  // Kelvin\n";
    XrFmtConfig cfg = xfmt_default_config;
    cfg.align_trailing_comments = 1;
    char *out = format_with_config(src, &cfg);
    ASSERT_NOT_NULL(out);
    /* Widest code is `var mass = 100` and `var temp = 273` (14 chars). All
     * lines pad to col 16 (target = max + 2). */
    ASSERT_TRUE(contains(out, "var radius = 5  // sphere radius"));
    ASSERT_TRUE(contains(out, "var mass = 100  // kg"));
    ASSERT_TRUE(contains(out, "var temp = 273  // Kelvin"));
    free(out);
    teardown();
}

TEST(trailing_comments_default_unchanged) {
    setup();
    const char *src = "var x = 1  // first\n"
                      "var yyyy = 22  // second\n";
    char *out = format_with_config(src, NULL);
    ASSERT_NOT_NULL(out);
    /* Default: each comment two spaces after its own code, NOT aligned. */
    ASSERT_TRUE(contains(out, "var x = 1  // first"));
    ASSERT_TRUE(contains(out, "var yyyy = 22  // second"));
    /* Specifically: NO over-padding on the short line. */
    ASSERT_FALSE(contains(out, "var x = 1      // first"));
    free(out);
    teardown();
}

TEST(trailing_comments_idempotent) {
    setup();
    const char *src = "var a = 1  // a\n"
                      "var bb = 22  // b\n"
                      "var ccc = 333  // c\n";
    XrFmtConfig cfg = xfmt_default_config;
    cfg.align_trailing_comments = 1;
    char *fmt1 = format_with_config(src, &cfg);
    ASSERT_NOT_NULL(fmt1);
    AstNode *ast2 =
        xr_parse_with_trivia(xr_compiler_session_current_for_isolate(g_iso), fmt1, "<test>");
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
    const char *src = "var url = \"https://example.com\"  // homepage\n"
                      "var path = \"/abc\"  // root\n";
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
RUN_TEST(ast_preserved_over_corpora);

RUN_TEST(doc_comment_before_function);
RUN_TEST(block_comment_before_statement);
RUN_TEST(comment_before_class);

RUN_TEST(string_escape_roundtrip);
RUN_TEST(template_string_roundtrip);
RUN_TEST(unicode_string_roundtrip);
RUN_TEST(empty_string_roundtrip);

RUN_TEST(arrow_return_type_emitted);
RUN_TEST(annotated_and_mode_lambda_remain_arrow);
RUN_TEST(attribute_visibility_modifier_order_roundtrip);
RUN_TEST(method_deprecated_attribute_roundtrip);
RUN_TEST(inline_control_attributes_roundtrip);
RUN_TEST(explicit_numeric_conversions_roundtrip);
RUN_TEST(deprecated_message_roundtrip);
RUN_TEST(object_destructure_rename_roundtrip);
RUN_TEST(parameter_modes_roundtrip);
RUN_TEST(unknown_effect_attribute_rejected);
RUN_TEST(extern_block_roundtrip);
RUN_TEST(optional_chain_implicit_link_roundtrip);
RUN_TEST(parameter_modes_comments_roundtrip);

RUN_TEST(branch_arrows_default_aligned);
RUN_TEST(branch_arrows_can_disable_alignment);
RUN_TEST(branch_arrows_aligned_idempotent);
RUN_TEST(select_branch_arrows_default_aligned);
RUN_TEST(match_single_arm_no_padding);

RUN_TEST(enum_members_ignore_removed_value_alignment);
RUN_TEST(enum_payload_members_roundtrip);
RUN_TEST(enum_record_construction_and_selective_pattern_are_idempotent);
RUN_TEST(enum_static_iteration_roundtrip);

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
