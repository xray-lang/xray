/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_parser_asi.c - Statement boundaries at line breaks (L-04)
 *
 * KEY CONCEPT:
 *   Xray ends statements at line breaks. The ambiguity only exists for tokens
 *   that carry BOTH a prefix and an infix role — `!` (logical not / force
 *   unwrap), `-` (negate / subtract), `/` (regex / divide), `++`, `--`, `(`,
 *   `[`. For those, a new line must start a new statement instead of silently
 *   extending the previous expression:
 *
 *       var x = a
 *       !b          // two statements, NOT `var x = a!`
 *
 *   Two things are verified here:
 *     1. Behaviour: every dual-role token starts a fresh statement, while
 *        continuation lines opening with a single-role infix token keep working.
 *     2. Closure: the case table covers EVERY dual-role token in the Pratt rules
 *        table. Adding a new dual-role token without a case fails this test —
 *        the guard that the narrow `(`/`[`-only predecessor of this rule lacked.
 */

#include "../test_framework.h"
#include <string.h>

#include "frontend/parser/xparse.h"
#include "frontend/parser/xparse_internal.h"
#include "frontend/parser/xast_api.h"
#include "frontend/parser/xast_types.h"
#include "frontend/parser/xast_nodes.h"
#include "xray.h"
#include "base/xarena.h"
#include "base/xmalloc.h"
#include "toolchain/xcompiler_session.h"

/* ========== Fixtures ========== */

static XrVMRuntime *g_vm = NULL;
static XrCompilerSession *g_session = NULL;

static void setup(void) {
    g_vm = xray_vm_new(NULL);
    ASSERT_NOT_NULL(g_vm);
    XrCompilerSessionConfig cfg = {.vm_host = g_vm};
    g_session = xr_compiler_session_new(&cfg);
    ASSERT_NOT_NULL(g_session);
    xr_compiler_session_attach_isolate(g_vm, g_session);
}

static void teardown(void) {
    if (g_session) {
        xr_compiler_session_delete(g_session);
        g_session = NULL;
    }
    if (g_vm) {
        xray_vm_delete(g_vm);
        g_vm = NULL;
    }
}

/* ========== Dual-role token cases ==========
 *
 * `second_line` opens with the token under test. The probe program is
 *
 *     var a = 1
 *     var b = 2
 *     var x = a
 *     <second_line>
 *
 * and the assertion is that `x`'s initializer is still the bare variable `a`.
 * If the line-break rule regresses, the initializer absorbs the next line and
 * becomes a ForceUnwrap / Binary / Call / IndexGet instead.
 */
typedef struct {
    XrTokenType token;
    const char *second_line;
} AsiCase;

static const AsiCase k_asi_cases[] = {
    {TK_NOT, "!b"},             // force unwrap vs logical not
    {TK_MINUS, "-b"},           // subtract vs negate
    {TK_SLASH, "/b/"},          // divide vs regex literal
    {TK_INC, "++b"},            // postfix vs prefix increment
    {TK_DEC, "--b"},            // postfix vs prefix decrement
    {TK_LPAREN, "(b, a) = t"},  // call vs tuple-destructure statement
    {TK_LBRACKET, "[1, 2]"},    // index vs array literal statement
};

#define ASI_CASE_COUNT ((int) (sizeof(k_asi_cases) / sizeof(k_asi_cases[0])))

/* Parse with error recovery so that E0208 on the (deliberately effectless)
 * second line still yields a full AST to inspect. The arena is caller-owned
 * and captured into *out_arena; AST nodes live inside it. */
static AstNode *parse_recoverable_src(const char *source, Parser *parser, XrArena **out_arena) {
    XrArena *arena = (XrArena *) xr_malloc(sizeof(XrArena));
    xr_arena_init(arena, XR_ARENA_SEGMENT_SIZE);
    *out_arena = arena;

    XrCompilerSessionScope parse_scope;
    if (!xr_compiler_session_push_arena(g_session, arena, "<asi>", &parse_scope)) {
        xr_arena_destroy(arena);
        xr_free(arena);
        *out_arena = NULL;
        return NULL;
    }
    xr_parser_init(parser, g_session, source, "<asi>", arena);
    AstNode *ast = xr_parse_recoverable(parser);
    xr_compiler_session_pop_arena(&parse_scope);
    return ast;
}

static void release_arena(XrArena *arena) {
    if (!arena)
        return;
    xr_arena_destroy(arena);
    xr_free(arena);
}

/* Build the probe program for one dual-role token case. */
static AstNode *parse_probe(const char *second_line, Parser *parser, XrArena **out_arena) {
    char source[512];
    snprintf(source, sizeof(source), "var t = (1, 2)\nvar a = 1\nvar b = 2\nvar x = a\n%s\n",
             second_line);
    return parse_recoverable_src(source, parser, out_arena);
}

/* The `var x = a` declaration is the 4th top-level statement of the probe.
 * Returns NULL when the program does not have that shape at all. */
static AstNode *probe_x_initializer(AstNode *program) {
    if (!program || program->type != AST_PROGRAM || program->as.program.count < 4)
        return NULL;
    AstNode *decl = program->as.program.statements[3];
    if (decl->type != AST_VAR_DECL)
        return NULL;
    return decl->as.var_decl.initializer;
}

TEST(line_break_ends_expr_for_every_dual_role_token) {
    setup();
    for (int i = 0; i < ASI_CASE_COUNT; i++) {
        XrArena *arena = NULL;
        Parser parser;
        AstNode *program = parse_probe(k_asi_cases[i].second_line, &parser, &arena);

        AstNode *init = probe_x_initializer(program);
        if (!init || init->type != AST_VARIABLE) {
            fprintf(stderr, "  FAIL: line starting with \"%s\" was glued onto the previous line\n",
                    k_asi_cases[i].second_line);
        }
        ASSERT_NOT_NULL(init);
        ASSERT_EQ_INT(init->type, AST_VARIABLE);

        /* The second line is its own statement. Whether it survives into the
         * AST depends on whether it is itself legal (`(b, a) = t` is; `!b` is
         * an E0208 that the recovery driver drops), so the load-bearing
         * assertion is the one above: `a` was not absorbed. */

        release_arena(arena);
    }
    teardown();
}

/* Fail-closed closure check: no dual-role token may escape the case table. */
TEST(every_dual_role_token_has_an_asi_case) {
    setup();
    int dual_role = 0;
    for (int t = 0; t <= (int) TK_ERROR; t++) {
        const ParseRule *rule = xr_get_rule((XrTokenType) t);
        if (!rule->prefix || !rule->infix)
            continue;
        dual_role++;

        bool covered = false;
        for (int i = 0; i < ASI_CASE_COUNT; i++) {
            if ((int) k_asi_cases[i].token == t) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            fprintf(stderr,
                    "  FAIL: token %d has both a prefix and an infix rule but no L-04 case.\n"
                    "        Add it to k_asi_cases[] and confirm a line break still ends the\n"
                    "        previous statement — see the L-04 comment in xparse.c.\n",
                    t);
        }
        ASSERT_TRUE(covered);
    }
    /* Sanity: the table is not vacuously satisfied by an empty rules scan. */
    ASSERT_EQ_INT(dual_role, ASI_CASE_COUNT);
    teardown();
}

/* Continuation lines opening with a single-role infix token must NOT break. */
TEST(single_role_infix_tokens_still_continue_the_line) {
    setup();
    static const char *continuations[] = {
        "var x = 1 +\n    2\n",  // operator parked at end of line
        "var x = 1\n    + 2\n",  // `+` has no prefix role -> continues
        "var x = true\n    && false\n", "var x = 1\n    ?? 2\n",
        "var s = \"a\"\n    .len()\n",  NULL,
    };
    for (int i = 0; continuations[i]; i++) {
        AstNode *program =
            xr_parse(xr_compiler_session_current_for_isolate(g_vm), continuations[i]);
        if (!program)
            fprintf(stderr, "  FAIL: continuation rejected: %s\n", continuations[i]);
        ASSERT_NOT_NULL(program);
        ASSERT_EQ_INT(program->as.program.count, 1);
        xr_program_destroy(program);
    }
    teardown();
}

/* Inside `(` / `[` no statement can begin, so line breaks are transparent. */
TEST(line_breaks_inside_groups_do_not_split) {
    setup();
    static const char *grouped[] = {
        "var x = (1\n    - 2)\n",
        "var x = [1,\n    -2]\n",
        "fn f(p: int) -> int { return p }\nvar y = f(\n    1\n)\n",
        NULL,
    };
    for (int i = 0; grouped[i]; i++) {
        AstNode *program = xr_parse(xr_compiler_session_current_for_isolate(g_vm), grouped[i]);
        if (!program)
            fprintf(stderr, "  FAIL: grouped continuation rejected: %s\n", grouped[i]);
        ASSERT_NOT_NULL(program);
        xr_program_destroy(program);
    }
    teardown();
}

/* Regression for the original defect: the misparse used to be silent. */
TEST(bang_on_new_line_is_not_a_force_unwrap) {
    setup();
    const char *src = "var a: int? = null\nvar b = true\nvar x = a\n!b\n";
    XrArena *arena = NULL;
    Parser parser;
    AstNode *program = parse_recoverable_src(src, &parser, &arena);

    ASSERT_NOT_NULL(program);
    ASSERT_TRUE(program->as.program.count >= 4);
    AstNode *decl = program->as.program.statements[2];
    ASSERT_EQ_INT(decl->type, AST_VAR_DECL);
    ASSERT_NOT_NULL(decl->as.var_decl.initializer);
    ASSERT_EQ_INT(decl->as.var_decl.initializer->type, AST_VARIABLE);
    /* And it is loud, not silent. */
    ASSERT_TRUE(parser.had_error);

    release_arena(arena);
    teardown();
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Parser statement boundaries (L-04)");
RUN_TEST(line_break_ends_expr_for_every_dual_role_token);
RUN_TEST(every_dual_role_token_has_an_asi_case);
RUN_TEST(single_role_infix_tokens_still_continue_the_line);
RUN_TEST(line_breaks_inside_groups_do_not_split);
RUN_TEST(bang_on_new_line_is_not_a_force_unwrap);
TEST_MAIN_END()
