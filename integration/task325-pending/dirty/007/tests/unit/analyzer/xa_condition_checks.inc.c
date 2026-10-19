/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_condition_checks.inc.c - Exact boolean control-flow admission
 */

TEST(analyzer_conditions_require_plain_bool) {
    static const char *const bodies[] = {
        "if (value) {}",
        "while (value) { break }",
        "for (var index = 0; value; index++) { break }",
        "var selected = value ? 1 : 0",
        "var selected = match (1) { _ if (value) -> 1\n _ -> 0 }",
    };
    static const char *const types[] = {
        "bool", "bool?", "i64?", "f64?", "string?", "Array<i64>?", "Holder?",
        "i64", "f64", "string", "Array<i64>", "Holder",
    };
    for (size_t type = 0; type < sizeof(types) / sizeof(types[0]); ++type) {
        for (size_t body = 0; body < sizeof(bodies) / sizeof(bodies[0]); ++body) {
            char source[512];
            snprintf(source, sizeof(source), "class Holder {}\nfn probe(value: %s) { %s }\n",
                     types[type], bodies[body]);
            XaAnalyzer *analyzer = xa_analyzer_new(g_session);
            ASSERT(analyzer != NULL);
            AstNode *program = xr_parse(g_session, source);
            ASSERT(program != NULL);
            xa_analyzer_analyze(analyzer, "condition-types.xr", program);
            int count = 0, conditions = 0;
            XaDiagnostic *diagnostic = xa_analyzer_get_diagnostics(analyzer, &count);
            for (; diagnostic; diagnostic = diagnostic->next)
                if (diagnostic->code == XR_ERR_ANALYZE_CONDITION_TYPE)
                    ++conditions;
            if (conditions != (type == 0 ? 0 : 1))
                fprintf(stderr, "condition case type=%s body=%s diagnostics=%d conditions=%d\n",
                        types[type], bodies[body], count, conditions);
            bool valid = conditions == (type == 0 ? 0 : 1) && (type != 0 || count == 0);
            xa_analyzer_free(analyzer);
            xr_program_destroy(program);
            setup_pool();
            ASSERT(valid);
        }
    }
}

TEST(analyzer_explicit_presence_narrows_and_for_condition_is_optional) {
    const char *source =
        "fn probe(value: string?, flag: bool?) -> i64 {\n"
        " if (value != null && len(value) > 0) { return len(value) }\n"
        " if (value == null || len(value) == 0) {}\n"
        " if (flag == true) {}\n if (flag != null) {}\n if (flag ?? false) {}\n"
        " var result = value != null ? len(value) : 0\n"
        " while (value != null) { result = len(value); break }\n"
        " for (var index = 0; ; index++) { break }\n"
        " return result\n}\n";
    XaAnalyzer *analyzer = xa_analyzer_new(g_session);
    ASSERT(analyzer != NULL);
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(analyzer, "explicit-conditions.xr", program);
    int count = 0;
    xa_analyzer_get_diagnostics(analyzer, &count);
    xa_analyzer_free(analyzer);
    xr_program_destroy(program);
    setup_pool();
    ASSERT(count == 0);
}
