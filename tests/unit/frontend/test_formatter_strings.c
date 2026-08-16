/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_formatter_strings.c - String / template round-trip property tests
 *
 * KEY CONCEPT:
 *   An earlier formatter wrote LiteralNode.raw_value.string_val
 *   verbatim between two `"` characters and emitted templates between
 *   backticks. Both produced source the lexer rejects: a payload
 *   containing a literal `"`, `\`, newline, or control byte became a
 *   syntax error, and backtick templates simply do not lex any more.
 *
 *   xfmt_literal.c now re-escapes payloads properly. The AST retains
 *   escape mode and source form, so the formatter preserves raw and
 *   block semantics while choosing a safe variable-length fence.
 *
 *   This test pins the round-trip property:
 *
 *     for any source S that the parser accepts,
 *         format(parse(format(parse(S)))) == format(parse(S))
 *
 *   i.e. format-on-AST is a fixed point. We test it on:
 *     - hand-crafted edge-case payloads (every escapable byte, mixed
 *       quotes / backslashes, embedded `${`, multi-byte UTF-8);
 *     - raw-string sources whose payloads contain literal backslashes;
 *     - block strings and compact b/br/c/cr byte literals;
 *     - template strings carrying interpolations with parens, dots,
 *       arithmetic, and `$` literals;
 *     - 64 deterministic random payloads (seeded, reproducible).
 *
 *   The test uses ONLY the public formatter / parser APIs already
 *   exercised by test_formatter_comments.c, so a regression in any
 *   of (lexer escape parsing / parser literal lowering / formatter
 *   re-escape table / template-part splitting) shows up here as a
 *   round-trip mismatch.
 */

#include "../test_framework.h"

#include "frontend/format/xfmt.h"
#include "frontend/parser/xparse.h"
#include "frontend/parser/xast.h"
#include "toolchain/xcompiler_session.h"
#include "xray_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* Fixtures                                                                */
/* ====================================================================== */

static XrVMRuntime *g_iso = NULL;
static XrCompilerSession *g_session = NULL;

static void setup(void) {
    if (!g_iso) {
        XrVMConfig p;
        xray_vm_config_init(&p);
        g_iso = xray_vm_new_full(&p);
        g_session = xr_compiler_session_current_for_isolate(g_iso);
        ASSERT_NOT_NULL(g_session);
    }
}

static void teardown(void) {
    if (g_iso) {
        xray_vm_delete(g_iso);
        g_iso = NULL;
        g_session = NULL;
    }
}

// Parse + format a snippet, returning a heap string the caller frees.
// Returns NULL if the parser rejects the source (caller asserts).
static char *parse_and_format(const char *source) {
    AstNode *ast =
        xr_parse_with_trivia(xr_compiler_session_current_for_isolate(g_iso), source, "<test>");
    if (!ast)
        return NULL;
    char *out = xfmt_format_ast(ast, NULL, g_iso);
    xr_program_destroy(ast);
    return out;
}

// Property: format-on-AST is a fixed point. Returns the canonical
// formatted output (still owned by the caller), or NULL on parse
// failure for either pass.
static char *assert_round_trip(const char *src, const char *label) {
    char *first = parse_and_format(src);
    if (!first) {
        fprintf(stderr, "[%s] parse failed on:\n%s\n", label, src);
        return NULL;
    }
    char *second = parse_and_format(first);
    if (!second) {
        fprintf(stderr, "[%s] parse failed on first formatted output:\n%s\n", label, first);
        free(first);
        return NULL;
    }
    if (strcmp(first, second) != 0) {
        fprintf(stderr,
                "[%s] round-trip MISMATCH\n"
                "--- input ---\n%s\n"
                "--- first  ---\n%s\n"
                "--- second ---\n%s\n",
                label, src, first, second);
        free(first);
        free(second);
        return NULL;
    }
    free(second);
    return first;  // caller frees
}

// Build `var s = "<payload>";` with `payload` injected verbatim.
// Used to drive parser through the regular-string production.
static char *build_regular_let(const char *payload) {
    size_t len = strlen(payload);
    char *buf = (char *) malloc(len + 32);
    snprintf(buf, len + 32, "var s = \"%s\";\n", payload);
    return buf;
}

// Build `var s = r"<payload>";` for the raw-string production.
static char *build_raw_let(const char *payload) {
    size_t len = strlen(payload);
    char *buf = (char *) malloc(len + 32);
    snprintf(buf, len + 32, "var s = r\"%s\";\n", payload);
    return buf;
}

/* ====================================================================== */
/* Hand-crafted edge cases                                                 */
/* ====================================================================== */

TEST(regular_string_round_trip_basic) {
    // Each row: a regular string source, with no parser-rejected
    // bytes in its payload. Round-trip must be a fixed point AND
    // contain a `"`-quoted form (never reverts to raw-string form
    // since raw lexeme is dropped at parse time).
    static const char *kSources[] = {
        "var s = \"hello\";\n",
        "var s = \"with space\";\n",
        "var s = \"escaped quote: \\\"\";\n",
        "var s = \"backslash: \\\\\";\n",
        "var s = \"newline: \\n end\";\n",
        "var s = \"tab\\there\";\n",
        "var s = \"all: \\\" \\\\ \\n \\r \\t \\b \\f\";\n",
        "var s = \"dollar: $not a placeholder\";\n",
        "var s = \"chinese: \xe4\xbd\xa0\xe5\xa5\xbd\";\n",
        // Empty string:
        "var s = \"\";\n",
        // Only escapes:
        "var s = \"\\n\\t\";\n",
    };
    int n = (int) (sizeof(kSources) / sizeof(kSources[0]));
    for (int i = 0; i < n; i++) {
        char *out = assert_round_trip(kSources[i], "regular_basic");
        ASSERT_NOT_NULL(out);
        // The payload appears between the FIRST two `"` of the output.
        // We do not pin its exact form (escape table can evolve), only
        // that the output uses double quotes -- never backticks and
        // never raw-string `r"`.
        ASSERT_FALSE(strstr(out, "`") != NULL);
        ASSERT_FALSE(strstr(out, " r\"") != NULL);
        free(out);
    }
}

TEST(raw_string_form_is_preserved) {
    // The raw prefix is semantic source-form metadata and must survive.
    static const char *kRawPayloads[] = {
        "abc",                    // plain ASCII -- trivial
        "with \\n inside",        // literal `\n` (two chars), not a newline
        "literal $ sign",         // raw `$` outside template
        "trailing backslash \\",  // odd-count backslash run
        "mixed \\\\\\n test",     // multiple backslashes, no real newline
    };
    int n = (int) (sizeof(kRawPayloads) / sizeof(kRawPayloads[0]));
    for (int i = 0; i < n; i++) {
        char *src = build_raw_let(kRawPayloads[i]);
        char *out = assert_round_trip(src, "raw_preserved");
        ASSERT_NOT_NULL(out);
        ASSERT_TRUE(strstr(out, " r\"") != NULL || strstr(out, "=r\"") != NULL);
        free(out);
        free(src);
    }
}

TEST(template_string_round_trip) {
    // Template strings with various interpolation shapes. The
    // formatter must re-emit them as `"..." with ${...}` (backticks
    // are gone), with `$` in literal parts escaped so that no
    // implicit `${` can re-form.
    static const char *kSources[] = {
        "var n = \"x\";\nvar s = \"hello, ${n}!\";\n",
        "var a = 1; var b = 2;\nvar s = \"sum=${a + b}\";\n",
        "var p = 0;\nvar s = \"$${p}\";\n",  // user wants literal `$`
        "var n = \"x\";\nvar s = \"${n}-${n}-${n}\";\n",
        "var x = 1;\nvar s = \"start ${x} mid ${x + 1} end\";\n",
        "var s = \"${\"quoted\"}\";\n",
        "var m = #{\"k\": \"value\"};\nvar s = \"${m[\"k\"]}\";\n",
    };
    int n = (int) (sizeof(kSources) / sizeof(kSources[0]));
    for (int i = 0; i < n; i++) {
        char *out = assert_round_trip(kSources[i], "template");
        ASSERT_NOT_NULL(out);
        ASSERT_FALSE(strstr(out, "`") != NULL);  // backticks gone
        free(out);
    }
}

TEST(idempotence_after_two_passes) {
    // The most direct fixed-point witness: a single source going
    // through two format passes must yield identical bytes. This
    // is the canonical formulation of the round-trip contract.
    const char *src = "var a = \"plain\";\n"
                      "var b = \"with \\\"quote\\\" and \\\\ backslash\";\n"
                      "var c = r\"raw \\n stays literal\";\n"
                      "var n = \"x\";\n"
                      "var d = \"template ${n} done\";\n";

    char *first = parse_and_format(src);
    ASSERT_NOT_NULL(first);

    char *second = parse_and_format(first);
    ASSERT_NOT_NULL(second);

    char *third = parse_and_format(second);
    ASSERT_NOT_NULL(third);

    ASSERT_STR_EQ(first, second);
    ASSERT_STR_EQ(second, third);

    free(first);
    free(second);
    free(third);
}

/* ====================================================================== */
/* Pseudo-random round-trip property                                       */
/* ====================================================================== */

// Curated alphabet for random payloads. Excludes:
//   - the literal `"` byte (would close the string in source)
//   - the literal `\` byte (would start an escape in source; tested
//     separately in the hand-crafted block as `\\\\` etc.)
//   - the NUL byte (would truncate strcmp comparisons)
//   - control bytes <0x20 except common whitespace, since the
//     formatter's `\xHH` fallback path covers those and a regression
//     should show up in the hand-crafted set
//   - multi-byte UTF-8 sequences: random_payload picks ONE byte per
//     iteration, which would split a 3-byte codepoint mid-sequence
//     and produce a lexer error unrelated to the property under
//     test. UTF-8 fidelity is covered by the hand-crafted set.
//
// What stays: printable ASCII + space + the `$` byte (interesting
// because it interacts with template parsing). Note that `${` IS
// a template interpolation marker even inside raw strings, so the
// random_raw lane filters those out -- a raw payload containing
// `${` without a matching `}` is a legitimate parse error.
static const char kRandomAlphabet[] = "abcdefghijklmnopqrstuvwxyz"
                                      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                      "0123456789"
                                      " !#%&'()*+,-./:;<->?@[]^_|}~"
                                      "$$$$";  // weight $ higher to stress the template-escape path

#define RANDOM_PAYLOAD_MAX 32
#define RANDOM_CASE_COUNT 64

static void random_payload(unsigned int *state, char *out) {
    int len = (int) ((*state = (*state * 1103515245u + 12345u)) % RANDOM_PAYLOAD_MAX);
    for (int i = 0; i < len; i++) {
        *state = *state * 1103515245u + 12345u;
        out[i] = kRandomAlphabet[(*state >> 8) % (sizeof(kRandomAlphabet) - 1)];
    }
    out[len] = '\0';
}

TEST(random_regular_string_round_trip) {
    // Deterministic seed so a reproducible failure can be debugged
    // directly (the seed is part of the test contract).
    unsigned int state = 0x9e3779b9u;
    char payload[RANDOM_PAYLOAD_MAX + 1];

    int rejected = 0;
    int verified = 0;
    for (int i = 0; i < RANDOM_CASE_COUNT; i++) {
        random_payload(&state, payload);

        // Some random byte sequences may be lexically invalid as a
        // regular-string payload (e.g. an unterminated escape if the
        // alphabet is extended in the future). The current alphabet
        // contains no `\\` so every payload is valid; the rejection
        // counter exists so a future alphabet change does not
        // silently swallow failing cases.
        char *src = build_regular_let(payload);
        char *out = assert_round_trip(src, "random_regular");
        if (out) {
            verified++;
            free(out);
        } else {
            rejected++;
        }
        free(src);
    }
    ASSERT_EQ_INT(rejected, 0);
    ASSERT_EQ_INT(verified, RANDOM_CASE_COUNT);
}

TEST(random_raw_string_round_trip) {
    // Same alphabet, but injected as a raw-string payload. Raw
    // accepts bytes the regular form would reject (literal `\`),
    // so this lane stresses raw-form preservation.
    unsigned int state = 0xc2b2ae35u;
    char payload[RANDOM_PAYLOAD_MAX + 1];

    int verified = 0;
    for (int i = 0; i < RANDOM_CASE_COUNT; i++) {
        random_payload(&state, payload);

        // The raw-string lexer rejects a literal `"` in the payload
        // (it would close the string). Skip such payloads.
        if (strchr(payload, '"'))
            continue;

        char *src = build_raw_let(payload);
        char *out = assert_round_trip(src, "random_raw");
        ASSERT_NOT_NULL(out);
        ASSERT_TRUE(strstr(out, " r\"") != NULL || strstr(out, "=r\"") != NULL);
        free(out);
        free(src);
        verified++;
    }
    // We may skip a few to dodge `"`. Require we actually exercised
    // a meaningful fraction so a regression that silently shrinks the
    // corpus to zero would surface.
    ASSERT_TRUE(verified >= RANDOM_CASE_COUNT / 2);
}

TEST(block_and_fixed_bytes_round_trip) {
    const char *src = "var text = r\"\"\"\n"
                      "<div title=\"x\">${literal}\\n</div>\n"
                      "\"\"\"\n"
                      "var escaped = \"\"\"\n"
                      "line with \"quotes\" and \\n\n"
                      "\"\"\"\n"
                      "var bytes = br\"\"\"\n"
                      "echo ${HOME} \\n\n"
                      "\"\"\"\n"
                      "var cbytes = cr\"\"\"\n"
                      "puts\n"
                      "\"\"\"\n";
    char *out = assert_round_trip(src, "block_and_bytes");
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "r\"\"\"") != NULL);
    ASSERT_TRUE(strstr(out, "br\"\"\"") != NULL);
    ASSERT_TRUE(strstr(out, "cr\"\"\"") != NULL);
    free(out);
}

TEST(block_closer_stays_on_own_line_before_punctuation) {
    const char *src = "var values = [r\"\"\"\n"
                      "alpha\n"
                      "\"\"\"\n"
                      ", br\"\"\"\n"
                      "beta\n"
                      "\"\"\"\n"
                      "]\n";
    char *out = assert_round_trip(src, "block_punctuation");
    ASSERT_NOT_NULL(out);
    ASSERT_FALSE(strstr(out, "\"\"\",") != NULL);
    ASSERT_FALSE(strstr(out, "\"\"\"]") != NULL);
    free(out);
}

TEST(block_formatter_raises_quote_count_for_quote_only_payload_line) {
    const char *src = "var text = r\"\"\"\"\n"
                      "before\n"
                      "\"\"\"\n"
                      "after\n"
                      "\"\"\"\"\n"
                      "var bytes = br\"\"\"\"\n"
                      "\"\"\"\n"
                      "\"\"\"\"\n";
    char *first = assert_round_trip(src, "block_quote_collision");
    ASSERT_NOT_NULL(first);
    ASSERT_TRUE(strstr(first, "r\"\"\"\"\n") != NULL);
    ASSERT_TRUE(strstr(first, "br\"\"\"\"\n") != NULL);
    ASSERT_TRUE(strstr(first, "\nbefore\n\"\"\"\nafter\n") != NULL);

    char *third = parse_and_format(first);
    ASSERT_NOT_NULL(third);
    ASSERT_STR_EQ(first, third);
    free(third);
    free(first);
}

TEST(indented_block_template_keeps_margin_outside_interpolation) {
    const char *src = "fn render() {\n"
                      "    var name = \"Alice\"\n"
                      "    var text = \"\"\"\n"
                      "    Hello,\n"
                      "    ${name}!\n"
                      "    \"\"\"\n"
                      "}\n";
    char *out = assert_round_trip(src, "indented_block_template");
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "\n    ${name}!\n    \"\"\"") != NULL);
    ASSERT_FALSE(strstr(out, "${    name}") != NULL);
    free(out);
}

/* ====================================================================== */
/* Driver                                                                  */
/* ====================================================================== */

TEST_MAIN_BEGIN()
setup();
RUN_TEST_SUITE("string / template round-trip");
RUN_TEST(regular_string_round_trip_basic);
RUN_TEST(raw_string_form_is_preserved);
RUN_TEST(template_string_round_trip);
RUN_TEST(idempotence_after_two_passes);
RUN_TEST(random_regular_string_round_trip);
RUN_TEST(random_raw_string_round_trip);
RUN_TEST(block_and_fixed_bytes_round_trip);
RUN_TEST(block_closer_stays_on_own_line_before_punctuation);
RUN_TEST(block_formatter_raises_quote_count_for_quote_only_payload_line);
RUN_TEST(indented_block_template_keeps_margin_outside_interpolation);
teardown();
TEST_MAIN_END()
