/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_owner.c - Typed parse publication and exact arena ownership
 */
#include "xparse_internal.h"
#include "../../toolchain/xcompiler_arena_backing.h"
#include "../xdiag_fmt.h"

typedef struct XrParserScopeOwner {
    XrTypeScope *scope;
    struct XrParserScopeOwner *next;
} XrParserScopeOwner;

struct XrParserOwner {
    XrCompilerSessionScope session_scope;
    XrParserScopeOwner *scopes;
    bool closed;
    XrParseStatus diagnostic_failure;
};

XrParseStatus xr_parser_diagnostic_status(const Parser *parser) {
    return parser && parser->owner ? parser->owner->diagnostic_failure : XR_PARSE_OK;
}

void xr_parser_diagnostic_fail(Parser *parser, XrParseStatus status) {
    if (parser && parser->owner && parser->owner->diagnostic_failure == XR_PARSE_OK)
        parser->owner->diagnostic_failure = status;
}

static XrParseStatus parse_resource_status(XrCompileState *state) {
    switch (xr_compile_state_status(state)) {
        case XR_COMPILE_RESOURCE_OK: return XR_PARSE_OK;
        case XR_COMPILE_RESOURCE_BUDGET: return XR_PARSE_BUDGET;
        case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XR_PARSE_OUT_OF_MEMORY;
        default: return XR_PARSE_BAD_ARGUMENT;
    }
}

XrParseStatus xr_compile_parser_status(const Parser *parser) {
    if (!parser) return XR_PARSE_BAD_ARGUMENT;
    XrParseStatus status = parse_resource_status(parser->state);
    if (status != XR_PARSE_OK) return status;
    status = xr_parser_diagnostic_status(parser);
    return status != XR_PARSE_OK ? status : parser->had_error ? XR_PARSE_SYNTAX : XR_PARSE_OK;
}

XrTypeScope *xr_parser_type_scope_new(Parser *parser, XrTypeScope *parent) {
    if (!xr_parser_healthy(parser) || !parser->owner || parser->owner->closed) return NULL;
    XrParserScopeOwner *entry = ast_alloc(parser->compiler_session, sizeof(*entry));
    if (!entry) return NULL;
    XrTypeScope *scope = NULL;
    if (xr_compile_type_scope_open(parser->state, parent, &scope) != XR_COMPILE_RESOURCE_OK) return NULL;
    entry->scope = scope;
    entry->next = parser->owner->scopes;
    parser->owner->scopes = entry;
    return scope;
}

void xr_parser_type_scope_free(Parser *parser, XrTypeScope *scope) {
    if (!parser || !parser->owner || !scope) return;
    for (XrParserScopeOwner *entry = parser->owner->scopes; entry; entry = entry->next) {
        if (entry->scope == scope) {
            entry->scope = NULL;
            xr_type_scope_free(scope);
            return;
        }
    }
}

XrTypeAlias *xr_parser_type_scope_define(Parser *parser, XrTypeScope *scope,
                                          const char *name, XrTypeRef *type) {
    if (!xr_parser_healthy(parser)) return NULL;
    XrTypeAlias *alias = NULL;
    XrTypeScopeStatus status = xr_compile_type_scope_define(scope, name, type, &alias);
    if (status == XR_TYPE_SCOPE_DUPLICATE) xr_parser_error(parser, "duplicate type alias definition");
    return alias;
}

void xr_compile_parser_close(Parser *parser) {
    if (!parser || !parser->owner || parser->owner->closed) return;
    struct XrParserOwner *owner = parser->owner;
    (void) xr_compile_session_pop_arena(&owner->session_scope);
    if (owner->session_scope.active) return;
    owner->closed = true;
    for (XrParserScopeOwner *entry = owner->scopes; entry; entry = entry->next) {
        xr_type_scope_free(entry->scope);
        entry->scope = NULL;
    }
    parser->type_scope = NULL;
}

static XrParseStatus parser_init(Parser *output, XrCompilerSession *session, const char *source,
                                 const char *source_file, XrArena *arena, bool trivia) {
    XrCompileState *state = xr_compile_session_compile_state(session);
    if (!output || output->owner || !source || !xr_compiler_arena_matches_state(arena, state))
        return XR_PARSE_BAD_ARGUMENT;
    if (parse_resource_status(state) != XR_PARSE_OK) return parse_resource_status(state);
    Parser local = {0};
    local.state = state;
    local.compiler_session = session;
    local.arena = arena;
    local.source_file = source_file;
    local.current.type = local.previous.type = TK_ERROR;
    local.owner = xr_arena_alloc(arena, sizeof(*local.owner));
    if (xr_compiler_arena_capture_status(arena, state) != XR_COMPILE_RESOURCE_OK)
        return parse_resource_status(state);
    if (xr_compile_session_push_arena(session, arena,
                                       &local.owner->session_scope) != XR_COMPILE_RESOURCE_OK)
        return parse_resource_status(state);
    local.type_scope = xr_parser_type_scope_new(&local, NULL);
    if (local.type_scope &&
        xr_compile_scanner_open_with_trivia(&local.scanner, state, arena, source, trivia) == XR_COMPILE_RESOURCE_OK &&
        xr_compile_state_copy(state, output, &local, sizeof(local)) == XR_COMPILE_RESOURCE_OK)
        return XR_PARSE_OK;
    xr_compile_parser_close(&local);
    return parse_resource_status(state);
}

XrParseStatus xr_compile_parser_open(Parser *parser, XrCompilerSession *session, const char *source,
                              const char *source_file, XrArena *arena) {
    return parser_init(parser, session, source, source_file, arena, false);
}

static AstNode *parse_body(Parser *parser, bool expression, bool trivia) {
    if (!xr_parser_healthy(parser)) return NULL;
    AstNode *program = xr_ast_program(parser->compiler_session);
    if (!program) return NULL;
    xr_parser_advance(parser);
    if (!xr_parser_healthy(parser)) return NULL;
    if (expression) {
        AstNode *value = xr_parse_expression(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!value) { parser->had_error = 1; return NULL; }
        xr_ast_program_add(parser->compiler_session, program, value);
        return xr_parser_healthy(parser) ? program : NULL;
    }
    if (trivia) {
        program->leading_comments = parser->current.leading_trivia;
        parser->current.leading_trivia = NULL;
    }
    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_EOF)) {
        if (parser->max_errors > 0 && parser->error_count >= parser->max_errors) break;
        if (xr_compile_state_work(parser->state, 1) != XR_COMPILE_RESOURCE_OK) return NULL;
        if (parser->panic_mode) {
            xr_parser_synchronize(parser);
            if (!xr_parser_healthy(parser) || xr_parser_check(parser, TK_EOF)) break;
        }
        int line = parser->current.line;
        const char *start = parser->current.start;
        XrTrivia *leading = trivia ? parser->current.leading_trivia : NULL;
        if (trivia) parser->current.leading_trivia = NULL;
        AstNode *declaration = xr_parse_declaration(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        if (declaration && leading) declaration->leading_comments = leading;
        if (xr_parser_check(parser, TK_SEMICOLON)) xr_parser_advance(parser);
        else if (!xr_parser_check(parser, TK_EOF) && parser->current.line == line &&
                 parser->current.type != TK_QUESTION && parser->current.type != TK_COLON) {
            if (!xr_parser_check_asi_hint(parser))
                xr_parser_error_at_current(parser, "multiple statements on same line must be separated by semicolon");
        }
        if (!xr_parser_healthy(parser)) return NULL;
        if (declaration) {
            if (trivia && parser->previous.trailing_trivia) {
                declaration->trailing_comments = parser->previous.trailing_trivia;
                parser->previous.trailing_trivia = NULL;
            }
            xr_ast_program_add(parser->compiler_session, program, declaration);
        }
        if (parser->current.start == start && !declaration && !parser->panic_mode &&
            !xr_parser_check(parser, TK_EOF) && !xr_parser_check(parser, TK_RBRACE))
            xr_parser_advance(parser);
    }
    return xr_parser_healthy(parser) ? program : NULL;
}

static XrParseStatus parse_owned(XrCompilerSession *session, const char *source,
                                  const char *file, bool observed, bool trivia,
                                  bool expression, AstNode **output) {
    if (!session || !source || !output || *output) return XR_PARSE_BAD_ARGUMENT;
    XrCompileState *state = xr_compile_session_compile_state(session);
    XrParseStatus status = parse_resource_status(state);
    if (status != XR_PARSE_OK) return status;
    void *memory = NULL;
    if (xr_compile_state_calloc(state, 1, sizeof(XrArena), &memory) != XR_COMPILE_RESOURCE_OK)
        return parse_resource_status(state);
    XrArena *arena = memory;
    XrArenaBacking backing;
    (void) xr_compiler_arena_state_backing(state, &backing);
    (void) xr_arena_open(arena, XR_ARENA_SEGMENT_SIZE, &backing);
    if (xr_compiler_arena_capture_status(arena, state) != XR_COMPILE_RESOURCE_OK) {
        status = parse_resource_status(state);
        goto failure;
    }
    Parser parser = {0};
    status = parser_init(&parser, session, source, file, arena, trivia);
    if (status != XR_PARSE_OK) goto failure;
    parser.expr_value_observed = observed;
    parser.max_errors = 20;
    AstNode *program = parse_body(&parser, expression, trivia);
    status = xr_compile_parser_status(&parser);
    if (status == XR_PARSE_OK && !program) status = XR_PARSE_SYNTAX;
    if (status == XR_PARSE_SYNTAX && !trivia && !expression && parser.error_count)
        xr_parser_diagnostic_summary(&parser);
    status = xr_compile_parser_status(&parser);
    xr_compile_parser_close(&parser);
    status = xr_compile_parser_status(&parser);
    if (status == XR_PARSE_OK && !program) status = XR_PARSE_SYNTAX;
    if (status != XR_PARSE_OK) goto failure;
    program->as.program.arena = arena;
    program->as.program.owns_arena = true;
    *output = program;
    return XR_PARSE_OK;
failure:
    xr_arena_destroy(arena);
    xr_compile_state_free(arena);
    return status;
}

XrParseStatus xr_compile_parse(XrCompilerSession *session, const char *source, AstNode **output) {
    return parse_owned(session, source, NULL, false, false, false, output);
}

XrParseStatus xr_compile_parse_repl_unit(XrCompilerSession *session, const char *source, AstNode **output) {
    return parse_owned(session, source, "<repl>", true, false, false, output);
}

XrParseStatus xr_compile_parse_with_source(XrCompilerSession *session, const char *source,
                                   const char *file, AstNode **output) {
    return parse_owned(session, source, file, false, false, false, output);
}

XrParseStatus xr_compile_parse_with_trivia(XrCompilerSession *session, const char *source,
                                   const char *file, AstNode **output) {
    return parse_owned(session, source, file, false, true, false, output);
}

XrParseStatus xr_compile_parse_expression_string(XrCompilerSession *session, const char *source,
                                          const char *file, AstNode **output) {
    return parse_owned(session, source, file, false, false, true, output);
}

XrParseStatus xr_compile_parse_recoverable(Parser *parser, AstNode **output) {
    if (!parser || !output || *output || !parser->owner || parser->owner->closed)
        return XR_PARSE_BAD_ARGUMENT;
    AstNode *program = parse_body(parser, false, parser->scanner.collect_trivia);
    xr_compile_parser_close(parser);
    XrParseStatus status = xr_compile_parser_status(parser);
    if (status != XR_PARSE_OK && status != XR_PARSE_SYNTAX) return status;
    if (!program) return XR_PARSE_SYNTAX;
    program->as.program.arena = parser->arena;
    program->as.program.owns_arena = false;
    *output = program;
    return status == XR_PARSE_SYNTAX ? XR_PARSE_RECOVERED : XR_PARSE_OK;
}
