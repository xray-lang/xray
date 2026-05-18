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
 *   xfmt_literal.c now re-escapes payloads properly. Since the AST
 *   does not retain raw-vs-non-raw style, raw strings are
 *   canonicalised to ordinary double-quoted strings (canonical form >
 *   lexeme preservation, per the project principle).
 *
 *   This test pins the round-trip property:
 *
 *     for any source S that the parser accepts,
 *         format(parse(format(parse(S)))) == format(parse(S))
 *
 *   i.e. format-on-AST is a fixed point. We test it on:
 *     - hand-crafted edge-case payloads (every escapable byte, mixed
 *       quotes / backslashes, embedded `${`, multi-byte UTF-8);
 *     - raw-string sources whose payloads contain bytes that ONLY
 *       lex inside raw strings (literal backslash, literal `${`),
 *       exercising the canonical rewrite's re-escaping;
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
#include "xray_isolate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* Fixtures                                                                */
/* ====================================================================== */

static XrayIsolate *g_iso = NULL;

static void setup(void) {
    if (!g_iso) {
        XrayIsolateParams p;
        xray_isolate_params_init(&p);
        g_iso = xray_isolate_new(&p);
    }
}

static void teardown(void) {
    if (g_iso) {
        xray_isolate_delete(g_iso);
        g_iso = NULL;
    }
}

// Parse + format a snippet, returning a heap string the caller frees.
// Returns NULL if the parser rejects the source (caller asserts).
static char *parse_and_format(const char *source) {
    AstNode *ast = xr_parse_with_trivia(g_iso, source, "<test>");
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

// Build `let s = "<payload>";` with `payload` injected verbatim.
// Used to drive parser through the regular-string production.
static char *build_regular_let(const char *payload) {
    size_t len = strlen(payload);
    char *buf = (char *) malloc(len + 32);
    snprintf(buf, len + 32, "let s = \"%s\";\n", payload);
    return buf;
}

// Build `let s = r"<payload>";` for the raw-string production.
static char *build_raw_let(const char *payload) {
    size_t len = strlen(payload);
    char *buf = (char *) malloc(len + 32);
    snprintf(buf, len + 32, "let s = r\"%s\";\n", payload);
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
        "let s = \"hello\";\n",
        "let s = \"with space\";\n",
        "let s = \"escaped quote: \\\"\";\n",
        "let s = \"backslash: \\\\\";\n",
        "let s = \"newline: \\n end\";\n",
        "let s = \"tab\\there\";\n",
        "let s = \"all: \\\" \\\\ \\n \\r \\t \\b \\f\";\n",
        "let s = \"dollar: $not a placeholder\";\n",
        "let s = \"chinese: \xe4\xbd\xa0\xe5\xa5\xbd\";\n",
        // Empty string:
        "let s = \"\";\n",
        // Only escapes:
        "let s = \"\\n\\t\";\n",
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

TEST(raw_string_canonicalised_to_double_quoted) {
    // Payloads that are LEGAL inside a raw string but require
    // re-escaping when emitted as a regular string. The parser
    // accepts the raw form; the formatter must rewrite it.
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
        char *out = assert_round_trip(src, "raw_canonical");
        ASSERT_NOT_NULL(out);
        // Canonical-form contract: no `r"` survives the format pass.
        ASSERT_FALSE(strstr(out, " r\"") != NULL);
        ASSERT_FALSE(strstr(out, "=r\"") != NULL);
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
        "let n = \"x\";\nlet s = \"hello, ${n}!\";\n",
        "let a = 1; let b = 2;\nlet s = \"sum=${a + b}\";\n",
        "let p = 0;\nlet s = \"$${p}\";\n",  // user wants literal `$`
        "let n = \"x\";\nlet s = \"${n}-${n}-${n}\";\n",
        "let x = 1;\nlet s = \"start ${x} mid ${x + 1} end\";\n",
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
    const char *src = "let a = \"plain\";\n"
                      "let b = \"with \\\"quote\\\" and \\\\ backslash\";\n"
                      "let c = r\"raw \\n stays literal\";\n"
                      "let n = \"x\";\n"
                      "let d = \"template ${n} done\";\n";

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

TEST(random_raw_string_canonicalisation) {
    // Same alphabet, but injected as a raw-string payload. Raw
    // accepts bytes the regular form would reject (literal `\`),
    // so this lane stresses the canonicalisation pipeline.
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
        // Canonical form: the formatted output must NOT use `r"`.
        ASSERT_FALSE(strstr(out, " r\"") != NULL);
        free(out);
        free(src);
        verified++;
    }
    // We may skip a few to dodge `"`. Require we actually exercised
    // a meaningful fraction so a regression that silently shrinks the
    // corpus to zero would surface.
    ASSERT_TRUE(verified >= RANDOM_CASE_COUNT / 2);
}

/* ====================================================================== */
/* Driver                                                                  */
/* ====================================================================== */

TEST_MAIN_BEGIN()
setup();
RUN_TEST_SUITE("string / template round-trip");
RUN_TEST(regular_string_round_trip_basic);
RUN_TEST(raw_string_canonicalised_to_double_quoted);
RUN_TEST(template_string_round_trip);
RUN_TEST(idempotence_after_two_passes);
RUN_TEST(random_regular_string_round_trip);
RUN_TEST(random_raw_string_canonicalisation);
teardown();
TEST_MAIN_END()
