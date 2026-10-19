/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_override_checks.inc.c - Explicit inheritance override contracts
 */

TEST(analyzer_override_requires_declaration_and_matching_target) {
    static const struct { const char *source; unsigned expected; } cases[] = {
        {"class A { value() -> i64 { return 1 } }\nclass B extends A { override value() -> i64 { return 2 } }", 0},
        {"class A { value() -> i64 { return 1 } }\nclass B extends A { value() -> i64 { return 2 } }", 1},
        {"class A { override value() -> i64 { return 1 } }", 1},
        {"class A {}\nclass B extends A { override value() -> i64 { return 2 } }", 1},
        {"class A { value() -> i64 { return 1 } }\nclass B extends A { override value(x: i64) -> i64 { return x } }", 1},
        {"class A { ref value() {} }\nclass B extends A { override value() {} }", 1},
        {"class A { ref value() {} }\nclass B extends A { override ref value() {} }", 0},
        {"class A { move value() {} }\nclass B extends A { override move value() {} }", 0},
        {"class A { private value() {} }\nclass B extends A { override value() {} }", 1},
        {"class A { private value() {} }\nclass B extends A { value() {} }", 0},
        {"class A { value() {} }\nclass B extends A { private override value() {} }", 1},
        {"class A { protected value() {} }\nclass B extends A { protected override value() {} }", 0},
        {"class A { value() {} }\nclass B extends A {}\nclass C extends B { override value() {} }", 0},
        {"class A { value() {} }\nclass B extends A {}\nclass C extends B { value() {} }", 1},
        {"interface Value { value() -> i64 }\nclass A implements Value { value() -> i64 { return 1 } }", 0},
        {"interface Value { value() -> i64 }\nclass A implements Value { override value() -> i64 { return 1 } }", 1},
        {"class A { value(x: i64 = 1) {} }\nclass B extends A { override value(x: i64 = 2) {} }", 1},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        XaAnalyzer *analyzer = xa_analyzer_new(g_session);
        ASSERT(analyzer != NULL);
        AstNode *program = xr_parse(g_session, cases[i].source);
        ASSERT(program != NULL);
        xa_analyzer_analyze(analyzer, "override-contract.xr", program);
        int count = 0;
        unsigned overrides = 0;
        XaDiagnostic *diagnostic = xa_analyzer_get_diagnostics(analyzer, &count);
        for (; diagnostic; diagnostic = diagnostic->next) {
            if (diagnostic->code == XR_ERR_ANALYZE_OVERRIDE_MISMATCH)
                ++overrides;
            else
                fprintf(stderr, "override case %u: %s\n", i, diagnostic->message);
        }
        bool valid = overrides == cases[i].expected && (cases[i].expected || count == 0);
        if (!valid)
            fprintf(stderr, "override case %u: expected %u, actual %u, total %d\n",
                    i, cases[i].expected, overrides, count);
        xa_analyzer_free(analyzer);
        xr_program_destroy(program);
        setup_pool();
        ASSERT(valid);
    }
}
