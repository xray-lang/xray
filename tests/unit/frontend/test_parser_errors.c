/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_parser_errors.c - Error syntax survives parse and format boundaries
 */
#include "frontend/parser/xparse.h"
#include "frontend/parser/xtype_ref.h"
#include "frontend/parser/xast_walk.h"
#include "frontend/format/xfmt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)

typedef struct Children { AstNode *nodes[64]; size_t count; } Children;
static bool child(AstNode *node, void *pointer) {
    Children *children = pointer;
    CHECK(children->count < 64);
    children->nodes[children->count++] = node;
    return true;
}
static void same_ast(AstNode *left, AstNode *right) {
    if (!left || !right) { CHECK(left == right); return; }
    char a[2048], b[2048];
    CHECK(xr_ast_node_signature(left, a, sizeof(a)));
    CHECK(xr_ast_node_signature(right, b, sizeof(b)));
    if (strcmp(a,b)) { fprintf(stderr,"before: %s\nafter: %s\n",a,b); exit(1); }
    Children x = {0}, y = {0};
    CHECK(xr_ast_for_each_child(left,child,&x));
    CHECK(xr_ast_for_each_child(right,child,&y));
    CHECK(x.count == y.count);
    for (size_t i = 0; i < x.count; ++i) same_ast(x.nodes[i],y.nodes[i]);
}
static AstNode *parse(XrCompilerSession *session, const char *source) {
    AstNode *program = NULL;
    XrParseStatus status = xr_compile_parse(session,source,&program);
    if (status != XR_PARSE_OK) fprintf(stderr,"source: %s\n",source);
    CHECK(status == XR_PARSE_OK && program && program->type == AST_PROGRAM);
    return program;
}
static void roundtrip(XrCompilerSession *session, const char *source) {
    AstNode *first = parse(session,source);
    XrFmtOutput one = {0}, two = {0};
    XrCompileState *state = xr_compile_session_compile_state(session);
    CHECK(xr_compile_format_ast(state,first,&xfmt_default_config,&one) == XR_FMT_OK);
    AstNode *second = parse(session,one.text);
    same_ast(first,second);
    CHECK(xr_compile_format_ast(state,second,&xfmt_default_config,&two) == XR_FMT_OK);
    CHECK(one.length == two.length && !memcmp(one.text,two.text,one.length));
    xr_program_destroy(first);
    xr_program_destroy(second);
    xr_compile_format_output_free(&one);
    xr_compile_format_output_free(&two);
}
static void clause_states(XrCompilerSession *session) {
    const char *sources[] = {"fn f() {}", "fn f() throws {}", "fn f() throws(Never) {}"};
    const XrThrowsSyntax states[] = {XR_THROWS_OMITTED,XR_THROWS_ERASED,XR_THROWS_TYPED};
    for (size_t i = 0; i < 3; ++i) {
        AstNode *program = parse(session,sources[i]);
        XrThrowsClause clause = program->as.program.statements[0]->as.function_decl.throws_clause;
        CHECK(clause.syntax == states[i]);
        if (i) CHECK(clause.line == 1 && clause.column == 8);
        if (i == 2) CHECK(clause.type && !strcmp(clause.type->name,"Never"));
        else CHECK(!clause.type);
        xr_program_destroy(program);
    }
}
static void expression_modes(XrCompilerSession *session) {
    const char *sources[] = {"var x = try f() + g() * 2", "var x = try? f()", "var x = try! f()"};
    for (unsigned i = 0; i < 3; ++i) {
        AstNode *program = parse(session,sources[i]);
        AstNode *expr = program->as.program.statements[0]->as.var_decl.initializer;
        CHECK(expr && expr->type == AST_TRY_EXPR && (unsigned)expr->as.try_expr.mode == i);
        CHECK(expr->line == 1 && expr->column == 9);
        CHECK(expr->as.try_expr.operand->type == (i ? AST_CALL_EXPR : AST_BINARY_ADD));
        xr_program_destroy(program);
    }
    AstNode *program = parse(session,"var x = try ready ? f() : g()");
    CHECK(program->as.program.statements[0]->as.var_decl.initializer->as.try_expr.operand->type == AST_TERNARY);
    xr_program_destroy(program);
}

typedef struct RegexDiagnostic { unsigned count; int line, column; const char *expected; } RegexDiagnostic;
static void regex_diagnostic(void *context, int line, int column, int end_line,
                              int end_column, const char *message) {
    RegexDiagnostic *diagnostic = context;
    CHECK(message && *message && end_line >= line && end_column > 0);
    if (!diagnostic->count) {
        diagnostic->line = line; diagnostic->column = column;
        if (diagnostic->expected) CHECK(!strcmp(message,diagnostic->expected));
    }
    diagnostic->count++;
}

static void regex_syntax(XrCompilerSession *session) {
    AstNode *program = parse(session, "var re = 8\nvar r = re\"(?i)\\d+/${literal}\"\nvar q = re\n/ 2");
    CHECK(program->as.program.count == 3);
    AstNode *literal = program->as.program.statements[1]->as.var_decl.initializer;
    CHECK(literal->type == AST_LITERAL_REGEX && literal->line == 2 && literal->column == 9);
    CHECK(!strcmp(literal->as.literal.raw_value.regex.pattern, "(?i)\\d+/${literal}"));
    CHECK(!strcmp(literal->as.literal.raw_value.regex.flags, ""));
    CHECK(literal->as.literal.escape_mode == XR_LITERAL_RAW);
    CHECK(program->as.program.statements[2]->as.var_decl.initializer->type == AST_BINARY_DIV);
    xr_program_destroy(program);

    const char *bad[] = {"var r = /a/i", "var r = /\\d+/", "var r = re\"a\"g",
        "var r = re \"a\"", "var r = re\"a\n\"", "var r = re\"a", "var r = re\"\xc0\xaf\""};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        RegexDiagnostic diagnostic = {0};
        XrParseDiagnostics options = {regex_diagnostic, &diagnostic, 4};
        program = NULL;
        CHECK(xr_compile_parse_with_diagnostics(session, bad[i], "<regex>", &options, &program) == XR_PARSE_SYNTAX);
        CHECK(!program && diagnostic.count && diagnostic.line == 1 && diagnostic.column >= 9);
        CHECK(xr_compile_session_resource_status(session) == XR_COMPILE_RESOURCE_OK);
    }
}

static void explicit_methods(XrCompilerSession *session) {
    AstNode *program=parse(session,"class Data { value:i64; read()->i64{return this.value} }\ninterface I { read()->i64 }\n");
    CHECK(program->as.program.count==2);
    ClassDeclNode *data=&program->as.program.statements[0]->as.class_decl;
    CHECK(data->field_count==1 && data->method_count==1);
    CHECK(!data->methods[0]->as.method_decl.is_getter && !data->methods[0]->as.method_decl.is_setter);
    InterfaceDeclNode *interface=&program->as.program.statements[1]->as.interface_decl;
    CHECK(interface->method_count==1 && interface->property_count==0 && !interface->properties);
    xr_program_destroy(program);
    const char *bad[]={
        "class C {\n value: i64 { fn() { return 1 } }\n}",
        "class C {\n value: i64 { fn(v) {} }\n}",
        "struct C {\n value: i64 { fn() { return 1 } }\n}",
        "interface I {\n value: i64\n}",
        "interface I {\n const value: i64\n}"
    };
    const int columns[]={13,13,13,7,13};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        RegexDiagnostic diagnostic={.expected=i<3?
            "computed accessors were removed; use an explicit method":
            "interface properties were removed; declare an explicit method"};
        XrParseDiagnostics options={regex_diagnostic,&diagnostic,4};program=NULL;
        CHECK(xr_compile_parse_with_diagnostics(session,bad[i],"<removed-accessor>",&options,&program)==XR_PARSE_SYNTAX);
        CHECK(!program && diagnostic.count && diagnostic.line==2 && diagnostic.column==columns[i]);
        CHECK(xr_compile_session_resource_status(session)==XR_COMPILE_RESOURCE_OK);
    }
}

int main(void) {
    const XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_FINITE, 8 * 1024 * 1024, true};
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline = {0};
    CHECK(xr_compile_resources_stats(resources,&baseline) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_new(resources,&session) == XR_COMPILE_RESOURCE_OK);
    clause_states(session);
    expression_modes(session);
    regex_syntax(session);
    explicit_methods(session);
    const char *sources[] = {
        "fn throws() {}\nvar throws = 1\nvar value: Never\n",
        "fn f<T,E:Error>(value:T) throws(E | IoErr) -> T { return value }\n",
        "interface I { run() throws(E) -> i64 }\nclass C implements I { init() throws(E) {} run() throws(E) -> i64 { return 1 } }",
        "class C { static init() {} }",
        "class C { static init() throws {} }",
        "interface I { operator len() throws(E) -> i64 }",
        "class C { operator len() throws(E) -> i64 { return 1 } }",
        "extern \"C\" { fn f() throws(E) -> i64 }",
        "var f = fn() throws(E) {}",
        "var f = fn<E:Error>() throws(E) -> i64 { return 1 }",
        "type Callback = fn(ref i64, fn() throws(E)) throws(Error) -> fn() throws",
        "type Callback = fn() throws(Never)",
        "try f()\ntry? f()\ntry! f()\n",
        "var x = try f() + g() * 2\nvar y = try ready ? f() : g()",
        "var x = (try? f()) ?? 1\nvar y = try? f(a ?? b)",
        "var value = try await task\n",
        "try { f() } catch (e) { print(1) }",
        "fn f() { var g = fn() throws(E) {}\ntry g() }",
        "var r = re\"(?ims)\\d+/[a-z]+${literal}\"\nvar e = re\"\"",
        "var r = re\"\"\"\n    (?m)^a\\d+\"${literal}\n    \"\"\"\n",
        "var r = [re\"a\", re\"(?i)b\"]\nvar q = 20\n/ 5\n",
        "var text = \"${re\"${not_interpolated}\"}\"",
        "var text = \"${re\"${\"}\"",
        "var text = \"${re\"\\\"}\"",
        "var r = re\"中😀+\\p{L}\""
    };
    for (size_t i = 0; i < sizeof(sources)/sizeof(sources[0]); ++i) roundtrip(session,sources[i]);
    xr_compile_session_free(session);
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(resources,&stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.held_bytes == baseline.held_bytes);
    xr_compile_resources_release(resources);
    printf("error clause states, propagation modes and %zu format roundtrips passed\n",sizeof(sources)/sizeof(sources[0]));
    return 0;
}
