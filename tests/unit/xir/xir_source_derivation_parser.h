/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_derivation_parser.h - Exact explicit intent and canonical formatting
 */
#ifndef XIR_SOURCE_DERIVATION_PARSER_H
#define XIR_SOURCE_DERIVATION_PARSER_H
#include "frontend/parser/xparse.h"
#include "frontend/parser/xast_walk.h"
#include "frontend/format/xfmt.h"
static void derive_parser_case(const char *source,bool valid) {
    const XrXirCompileContext *context=source_fixture_source_owner(67108864,128000000);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    AstNode *ast=NULL;XrParseStatus parsed=xr_compile_parse_with_trivia(session,source,"derive-parser",NULL,&ast);
    if (!valid) {CHECK(parsed==XR_PARSE_SYNTAX && !ast);xr_compile_session_free(session);source_fixture_source_owner_close(context);return;}
    CHECK(parsed==XR_PARSE_OK && ast && ast->as.program.count==1);
    AstNode *declaration=ast->as.program.statements[0];CHECK(declaration->type==AST_STRUCT_DECL);
    CHECK(declaration->as.struct_decl.attr_count==2);
    const XrDerivationSyntax *intent=declaration->as.struct_decl.attributes[0]->derivation;
    CHECK(intent && !strcmp(intent->generator_module,"makers") && !strcmp(intent->generator,"generate") &&
        intent->field_count==4 && !intent->construct && intent->fields[0].operation==XR_DERIVE_FIELD_WRITE);
    char before[8192],after[8192];CHECK(xr_ast_node_signature(declaration,before,sizeof(before)));
    XrFmtOutput output={0};CHECK(xr_compile_format_ast(xr_compile_session_compile_state(session),ast,NULL,&output)==XR_FMT_OK);
    CHECK(strstr(output.text,"describe: [\"a\", \"b\"], read: [\"b\"], write: [\"a\"]"));
    xr_program_destroy(ast);ast=NULL;xr_compile_session_free(session);session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    CHECK(xr_compile_parse_with_trivia(session,output.text,"derive-parser",NULL,&ast)==XR_PARSE_OK && ast);
    CHECK(xr_ast_node_signature(ast->as.program.statements[0],after,sizeof(after)) && !strcmp(before,after));
    XrFmtOutput again={0};CHECK(xr_compile_format_ast(xr_compile_session_compile_state(session),ast,NULL,&again)==XR_FMT_OK &&
        output.length==again.length && !memcmp(output.text,again.text,output.length));
    xr_compile_format_output_free(&again);xr_compile_format_output_free(&output);
    xr_program_destroy(ast);xr_compile_session_free(session);source_fixture_source_owner_close(context);
}
static void derive_parser_cases(void) {
    derive_parser_case("@derive(P<i64>, makers.generate, write:[\"a\"], read:[\"b\"], describe:[\"a\",\"b\"], construct:false)\n"
        "@derive(Q,g)\nstruct S {a:i64;b:i64}",true);
    const char *bad[]={
        "@derive(JSON) struct S{}",
        "@derive(Inspect, JSON, Eq, Hash, Clone) struct S{}",
        "@derive(P,g, read:[\"a\"], read:[\"b\"]) struct S{}",
        "@derive(P,g, unknown:[]) struct S{}",
        "@derive(P,g, describe:[a]) struct S{}",
        "@derive(P,g, describe:[\"\"]) struct S{}",
        "@derive(P,g, construct:1) struct S{}",
        "@derive(P,g) fn f(){}",
        "@derive(P,g) union U {a:i64}"
    };
    for (size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) derive_parser_case(bad[i],false);
}
#endif // XIR_SOURCE_DERIVATION_PARSER_H
