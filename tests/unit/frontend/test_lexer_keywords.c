/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lexer_keywords.c - L-01 / L-08 acceptance tests
 *
 * KEY CONCEPT:
 *   The lexer's keyword recognition was rewritten in Phase 1 from a
 *   270-line hand-written switch nest to an X-macro single-source-of-
 *   truth table (xkeywords.def). The hand-written version had a real
 *   bug (L-01): TK_IF's match path returned TK_IF without checking
 *   length, so identifiers like `iffy` / `ifElse` were mis-classified
 *   as TK_IF.
 *
 *   These tests pin down the contract:
 *
 *     1. Every spelling listed in xkeywords.def is recognised as
 *        its declared XrTokenType.
 *     2. NO spelling listed in the table is matched by a strict
 *        prefix that happens to itself be an identifier (regression
 *        guard for L-01: `iffy` is TK_NAME, not TK_IF).
 *     3. NO spelling listed is matched by a strict suffix appended
 *        to an identifier head (`xif`, `iff`, `ininin` etc.).
 *     4. Every identifier that DIFFERS from a keyword by case is
 *        TK_NAME (`Let`, `IF`, `True`).
 *     5. The single-character `r` keeps its identifier status so the
 *        raw-string lexer can detect it contextually (xkeywords.def
 *        has a comment to that effect).
 *
 *   This file uses the same Scanner-driven helpers as test_lexer.c,
 *   without depending on the parser.
 */

#include "../test_framework.h"
#include "frontend/lexer/xlex.h"

/* ====================================================================== */
/* Helpers                                                                 */
/* ====================================================================== */

// Scan exactly one token from `source`. Trivia collection is off
// (irrelevant for keyword recognition).
static Token scan_one(const char *source) {
    Scanner s;
    xr_scanner_init(&s, source);
    return xr_scanner_scan(&s);
}

// Scan two consecutive tokens; second one is read AFTER the first
// produced its result. Used for "token boundary" checks.
static void scan_two(const char *source, Token *a, Token *b) {
    Scanner s;
    xr_scanner_init(&s, source);
    *a = xr_scanner_scan(&s);
    *b = xr_scanner_scan(&s);
}

static bool token_text_eq(Token t, const char *expected) {
    int len = (int)strlen(expected);
    return t.length == len && memcmp(t.start, expected, (size_t)len) == 0;
}

/* ====================================================================== */
/* Per-keyword recognition matrix                                          */
/* ====================================================================== */

// Each entry: spelling, expected XrTokenType. List MUST mirror
// xkeywords.def -- adding a keyword requires a new row here AND a
// new prefix-identifier row in keyword_prefix_identifiers below.
typedef struct { const char *spelling; XrTokenType type; } KwExpect;

static const KwExpect kKnownKeywords[] = {
    /* Uppercase type names still owned by the lexer. BigInt / Bytes /
     * DateTime / Range / Regex / StringBuilder are now resolved through
     * the prelude registry as plain identifiers. */
    { "Array",       TK_TYPE_ARRAY    },
    { "Channel",     TK_TYPE_CHANNEL  },
    { "Json",        TK_TYPE_JSON     },
    { "Map",         TK_TYPE_MAP      },
    { "Set",         TK_TYPE_SET      },
    { "abstract",    TK_ABSTRACT      },
    { "as",          TK_AS            },
    { "await",       TK_AWAIT         },
    { "bool",        TK_BOOL          },
    { "break",       TK_BREAK         },
    { "catch",       TK_CATCH         },
    { "class",       TK_CLASS         },
    { "const",       TK_CONST         },
    { "constructor", TK_CONSTRUCTOR   },
    { "continue",    TK_CONTINUE      },
    { "defer",       TK_DEFER         },
    { "else",        TK_ELSE          },
    { "enum",        TK_ENUM          },
    { "export",      TK_EXPORT        },
    { "extends",     TK_EXTENDS       },
    { "false",       TK_FALSE         },
    { "final",       TK_FINAL         },
    { "finally",     TK_FINALLY       },
    { "float",       TK_FLOAT         },
    { "float32",     TK_FLOAT32       },
    { "float64",     TK_FLOAT64       },
    { "fn",          TK_FN            },
    { "for",         TK_FOR           },
    { "go",          TK_GO            },
    { "if",          TK_IF            },
    { "implements",  TK_IMPLEMENTS    },
    { "import",      TK_IMPORT        },
    { "in",          TK_IN            },
    { "int",         TK_INT           },
    { "int8",        TK_INT8          },
    { "int16",       TK_INT16         },
    { "int32",       TK_INT32         },
    { "int64",       TK_INT64         },
    { "interface",   TK_INTERFACE     },
    { "is",          TK_IS            },
    { "let",         TK_LET           },
    { "match",       TK_MATCH         },
    { "new",         TK_NEW           },
    { "null",        TK_NULL          },
    { "operator",    TK_OPERATOR      },
    { "override",    TK_OVERRIDE      },
    { "private",     TK_PRIVATE       },
    { "public",      TK_PUBLIC        },
    { "return",      TK_RETURN        },
    { "scope",       TK_SCOPE         },
    { "select",      TK_SELECT        },
    { "shared",      TK_SHARED        },
    { "static",      TK_STATIC        },
    { "string",      TK_STRING        },
    { "struct",      TK_STRUCT        },
    { "super",       TK_SUPER         },
    { "this",        TK_THIS          },
    { "throw",       TK_THROW         },
    { "true",        TK_TRUE          },
    { "try",         TK_TRY           },
    { "type",        TK_TYPE_ALIAS    },
    { "uint8",       TK_UINT8         },
    { "uint16",      TK_UINT16        },
    { "uint32",      TK_UINT32        },
    { "uint64",      TK_UINT64        },
    { "unknown",     TK_UNKNOWN       },
    { "void",        TK_VOID          },
    { "while",       TK_WHILE         },
    { "yield",       TK_YIELD         },
};

#define KW_COUNT ((int)(sizeof(kKnownKeywords) / sizeof(kKnownKeywords[0])))

TEST(every_keyword_recognised) {
    // Every spelling exercised in isolation must scan as its
    // declared XrTokenType, with token text equal to the spelling.
    for (int i = 0; i < KW_COUNT; i++) {
        Token t = scan_one(kKnownKeywords[i].spelling);
        ASSERT_EQ_INT(t.type, kKnownKeywords[i].type);
        ASSERT_TRUE(token_text_eq(t, kKnownKeywords[i].spelling));
    }
}

/* ====================================================================== */
/* Prefix-identifier regression matrix (L-01 guard)                        */
/* ====================================================================== */

// Each entry is `keyword || extra` -- an identifier that has a
// keyword as a strict ASCII prefix. None of these is a keyword;
// each must lex as TK_NAME with the FULL combined text.
//
// This is the canonical regression set for the bug L-01 ("TK_IF
// returned without length guard"). The list intentionally covers:
//   - 2-char keywords (if/in/fn/go/is/as): these were the most
//     vulnerable in the old switch nest.
//   - longer keywords (`letter`, `interface_thing`) to cover the
//     X-macro binary search's longer paths.
//   - keywords that share a prefix with each other (`int`,
//     `int8`, `int32`, `interface`).
//
// Adding a new keyword to xkeywords.def MUST be paired with a
// matching row here.
static const char *kPrefixIdentifiers[] = {
    "iffy", "ifElse", "ifs",
    "innermost", "inn", "into",
    "fno", "fnord", "fns",
    "got", "goto", "goes",
    "isnt", "issue", "ish",
    "asynchronous", "ask", "ascend",
    "letter", "lettuce",
    "constants", "constellation",
    "continueLater",
    "elses", "elsewhere",
    "enumerate", "enumeration",
    "imports", "importer",
    "matcher", "matched",
    "newer", "newline",
    "nullable",
    "publicly",
    "returns", "returner",
    "scopes",
    "scoped",
    "trying", "trycatch",
    "true_value", "truely",
    "false_value",
    "thisIsNotThis",
    "voids",
    "whileLoop",
    "yields", "yielder",
    "Arrays", "ArrayList",
    "Maps", "MapEntry",
    "Sets", "SetOfThings",
    "Channels",
    "Jsonish",
    // Same-prefix-different-keyword chains:
    "interfaces",  // covers `interface` prefix, NOT `int`
    "int_thing",   // identifier starting with `int_`, not int
    "int8x", "int32_t",  // `int8` and `int32` are keywords; with extra letters they are NAME
    "float_x", "float32x", "float64x",
    "uint8x", "uint16x",
    // Single-char `r` reserved in xkeywords.def comment as a raw-
    // string contextual prefix; here it must remain TK_NAME.
    "r",
    "rid", "rabbit",
};

#define PREFIX_COUNT ((int)(sizeof(kPrefixIdentifiers) / \
                            sizeof(kPrefixIdentifiers[0])))

TEST(keyword_prefix_identifiers_are_TK_NAME) {
    // L-01 / L-08 regression guard. None of these may match a
    // keyword XrTokenType -- they are user identifiers.
    for (int i = 0; i < PREFIX_COUNT; i++) {
        Token t = scan_one(kPrefixIdentifiers[i]);
        ASSERT_EQ_INT(t.type, TK_NAME);
        ASSERT_TRUE(token_text_eq(t, kPrefixIdentifiers[i]));
    }
}

/* ====================================================================== */
/* Case-sensitivity & ASCII-suffix regression                              */
/* ====================================================================== */

TEST(keywords_are_case_sensitive) {
    // Capitalised / upper-case versions of lowercase keywords must
    // be TK_NAME. The X-macro table is byte-equal lookup; this is
    // a property of `memcmp`, but we still want the contract pinned.
    static const char *kCaseShifted[] = {
        "Let", "IF", "True", "False", "Class", "Fn", "Return",
        "Match", "New", "Type", "Const", "Final",
    };
    int n = (int)(sizeof(kCaseShifted) / sizeof(kCaseShifted[0]));
    for (int i = 0; i < n; i++) {
        Token t = scan_one(kCaseShifted[i]);
        ASSERT_EQ_INT(t.type, TK_NAME);
        ASSERT_TRUE(token_text_eq(t, kCaseShifted[i]));
    }
}

TEST(keyword_with_underscore_suffix_is_identifier) {
    // `if_` / `let_` etc. are underscore-suffixed identifiers.
    // The X-macro table never lists them, so they must be TK_NAME.
    static const char *kUnderscored[] = {
        "if_", "let_", "for_", "while_", "match_", "fn_", "in_",
        "true_", "false_", "null_", "this_", "super_",
    };
    int n = (int)(sizeof(kUnderscored) / sizeof(kUnderscored[0]));
    for (int i = 0; i < n; i++) {
        Token t = scan_one(kUnderscored[i]);
        ASSERT_EQ_INT(t.type, TK_NAME);
        ASSERT_TRUE(token_text_eq(t, kUnderscored[i]));
    }
}

/* ====================================================================== */
/* Keyword followed by token boundary                                      */
/* ====================================================================== */

TEST(keyword_then_punctuation) {
    // The keyword scanner must stop at non-identifier characters.
    // Verifies tokenisation of common forms like `if(`, `let;`,
    // `return)`, `match{`.
    Token a, b;

    scan_two("if(", &a, &b);
    ASSERT_EQ_INT(a.type, TK_IF);
    ASSERT_EQ_INT(b.type, TK_LPAREN);

    scan_two("let;", &a, &b);
    ASSERT_EQ_INT(a.type, TK_LET);
    ASSERT_EQ_INT(b.type, TK_SEMICOLON);

    scan_two("return)", &a, &b);
    ASSERT_EQ_INT(a.type, TK_RETURN);
    ASSERT_EQ_INT(b.type, TK_RPAREN);

    scan_two("match{", &a, &b);
    ASSERT_EQ_INT(a.type, TK_MATCH);
    ASSERT_EQ_INT(b.type, TK_LBRACE);
}

/* ====================================================================== */
/* Driver                                                                  */
/* ====================================================================== */

TEST_MAIN_BEGIN()
    RUN_TEST_SUITE("L-01 / L-08 keyword recognition");
    RUN_TEST(every_keyword_recognised);
    RUN_TEST(keyword_prefix_identifiers_are_TK_NAME);
    RUN_TEST(keywords_are_case_sensitive);
    RUN_TEST(keyword_with_underscore_suffix_is_identifier);
    RUN_TEST(keyword_then_punctuation);
TEST_MAIN_END()
