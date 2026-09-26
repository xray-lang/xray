/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_generics.c - Source definition and forwarding constraint matrix
 *
 * KEY CONCEPT:
 *   Even an unused or concretely callable template must justify its own body.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "toolchain/xcompiler_session.h"
#include "frontend/parser/xparse.h"
#include "frontend/parser/xast_walk.h"
#include "frontend/format/xfmt.h"
#include "base/xmalloc.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void write_generic_source(const char *path, const char *text) {
    FILE *file = fopen(path, "wb"); CHECK(file);
    CHECK(fwrite(text, 1, strlen(text), file) == strlen(text) && fclose(file) == 0);
}
static bool reference_child(AstNode *child, void *user) {
    CHECK(child && child->type == AST_VARIABLE); ++*(unsigned *) user; return true;
}
static void reference_syntax(void) {
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    const char *sources[] = {"const f = identity<string>\n", "const f = mod.identity<fn(i64)->string>\n",
        "consume(identity<string>, true)\n", "const f = true ? identity<i64> : identity<i64>\n",
        "const f = (identity<i64>)\n", "(identity<i64>)(7)\n", "identity<i64>(7)\n", "const result = a<b>c\n", "const result = a < b\n"};
    for (unsigned i = 0; i < sizeof(sources)/sizeof(sources[0]); ++i) {
        AstNode *ast = xr_parse(session,sources[i]); CHECK(ast);
        AstNode *statement = ast->as.program.statements[0];
        if (i == 0) {
            AstNode *ref = statement->as.var_decl.initializer;
            CHECK(ref->type == AST_FUNCTION_REF && ref->as.function_ref.type_arg_count == 1);
            CHECK(xr_ast_node_is_known(ref)); unsigned children = 0;
            CHECK(xr_ast_for_each_child(ref,reference_child,&children) && children == 1);
            char signature[512]; CHECK(xr_ast_node_signature(ref,signature,sizeof(signature)));
            CHECK(strstr(signature,"targs=1"));
        }
        if (i == 6) CHECK(statement->as.expr_stmt->type == AST_CALL_EXPR);
        if (i == 7) CHECK(statement->as.var_decl.initializer->type == AST_BINARY_GT);
        if (i == 8) CHECK(statement->as.var_decl.initializer->type == AST_BINARY_LT);
        char *formatted = xfmt_format_ast(ast,NULL,NULL); CHECK(formatted);
        xr_program_destroy(ast); ast = xr_parse(session,formatted); CHECK(ast);
        char *again = xfmt_format_ast(ast,NULL,NULL); CHECK(again && !strcmp(formatted,again));
        if (i == 0) CHECK(strstr(formatted,"identity<string>") && !strstr(formatted,"identity<string>()"));
        xr_free(again); xr_free(formatted); xr_program_destroy(ast);
    }
    CHECK(!xr_parse(session,"identity<i64>\n"));
    xr_compiler_session_delete(session);
}
int main(void) {
    reference_syntax();
    const char *rejected[] = {
        "fn id<T>(x:T)->T { return x }\nconst f = id<i64,string>\n",
        "fn id(x:i64)->i64 { return x }\nconst f = id<i64>\n",
        "fn id<T>(x:T)->T { return x }\nconst f:fn(bool)->bool = id<i64>\n",
        "fn id<T>(x:T)->T { return x }\nconst f = id<Missing>\n",
        "fn id<T>(x:T)->T { return x }\nconst f = id<()>\n",
        "const id = 1; const f = id<i64>\n",
        "const f = Atomic<i64>\n",
        "import \"./lib\" as lib\nconst f = lib.hidden<i64>\n",
        "import { hidden } from \"./lib\"\nconst f = hidden<i64>\n",
        "import \"./lib\" as lib\nfn unused<T>()->fn(T)->T { return lib.required<T> }\n",
        "import \"./lib\" as lib\nconst f = lib.required<fn(i64)->i64>\n",
        "fn unused<T>(x:T)->T { return true ? x : 0 }\n",
        "fn unused<T,U>(x:T,y:U)->T { return true ? x : y }\n",
        "fn unused<T>(x:T)->T { return true ? x : x + x }\n",
        "fn unused<T>(x:T)->T { return 1 ? x : x }\n",
        "const x = true ? 1 : false\n",
        "const x = true ? 1 : missing\n",
        "fn unused()->i64 { return false ? print() : 1 }\n",
        "import \"./lib\" as lib\nfn unused<T>(x:T)->T { return true ? x : lib.required<T>(x) }\n",

        "fn unused<T>(x:T)->T { return true }\n",
        "fn unused<T>(x:T) { print(x) }\n",
        "fn unused<T:Sendable>(x:T) { print(x) }\n",
        "fn unused<T>(x:T)->T { return x.missing() }\n",
        "fn unused<T:Sendable>(x:T)->T { return x.load() }\n",
        "fn unused<T>(x:T)->T { return x + x }\n",
        "fn unused<T>(x:T)->T { return 0 }\n",
        "fn unused<T:Comparable>(x:T)->T { return x }\n",
        "fn unused<T,T>(x:T)->T { return x }\n",
        "fn unused<T>(x:U)->T { return x }\n",
        "fn id<T>(x:T)->T { return x }\nid<i64>(true)\n",
        "fn id<T>(x:T)->T { return x }\nid(1)\n",
        "fn id<T>(x:T)->T { return x }\nid<i64,string>(1)\n",
        "fn id(x:i64)->i64 { return x }\nid<i64>(1)\n",
        "import \"./lib\" as lib\nfn open<T>(x:T)->T { return lib.required<T>(x) }\n",
        "import \"./lib\" as lib\nfn open<T>(x:T)->T { return lib.required<T>(x) }\nopen<i64>(1)\n",
        "import { hidden } from \"./lib\"\nhidden<string>(\"x\")\n",
        "fn id<T>(x:T)->T { return x }\nid<()>(print())\n"
    };
    char directory[XR_TEST_PATH_MAX] = "xir-source-generics-XXXXXX";
    CHECK(xr_test_mkdtemp(directory));
    char absolute[XR_TEST_PATH_MAX], root[XR_TEST_PATH_MAX], library[XR_TEST_PATH_MAX];
    CHECK(xr_test_realpath_buf(directory, absolute, sizeof(absolute)));
    CHECK(snprintf(root, sizeof(root), "%s/root.xr", absolute) > 0);
    CHECK(snprintf(library, sizeof(library), "%s/lib.xr", absolute) > 0);
    write_generic_source(library,
        "export fn required<T:Sendable>(x:T)->T { return x }\n"
        "fn hidden<T>(x:T)->T { return x }\n");
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, absolute};
    XrXirSourceRequest request = {session, root, &authority, NULL, NULL};
    for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        write_generic_source(root, rejected[i]);
        XrXirArtifact *artifact = NULL; XrXirSourceDiagnostic diagnostic;
        XrXirStatus status = xr_xir_source_check(&request, &artifact, &diagnostic);
        if (status == XR_XIR_OK) fprintf(stderr, "incorrectly accepted generic case %u\n", i);
        CHECK(status != XR_XIR_OK && !artifact && diagnostic.status == status && diagnostic.message[0]);
        if (i < 7 || i == 9 || i == 10) CHECK(status == XR_XIR_BAD_TYPE);
    }
    write_generic_source(root,
        "import \"./lib\" as lib\n"
        "fn relay<T>(value:T)->T where T:Sendable { const f = lib.required<T>; return f(value) }\n"
        "fn unused<T,U>(left:T,right:U)->T { const copy = left; return true ? copy : left }\n"
        "const text = relay<string>(\"yes\")\nconst number = relay<i64>(5)\n"
        "const flag = relay<bool>(true)\nconst atomic = relay<Atomic<i64>>(Atomic(7))\n");
    XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL;
    XrXirSourceDiagnostic diagnostic;
    XrXirStatus status = xr_xir_source_check(&request, &checked, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "%d:%d %s\n", diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK && checked);
    xr_compiler_session_delete(session);
    CHECK(xr_test_unlink(root) == 0 && xr_test_unlink(library) == 0 && xr_test_rmdir(directory) == 0);
    CHECK(xr_xir_artifact_module(checked)->generics);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_specialize(decoded, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    CHECK(!xr_xir_artifact_module(closed)->generics);
    CHECK(xr_xir_artifact_verify(closed, NULL, NULL) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_artifact_module(closed);
    unsigned reference_instances = 0, references = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (function->name_length > 9 && !memcmp(function->name,"required$",9)) ++reference_instances;
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *op = &function->instructions[i];
            if (op->op == XR_XIR_FUNCTION_REF) { ++references; CHECK(!op->targets[0] && !op->targets[1]); }
            if (op->op == XR_XIR_CALL) {
                const XrXirFunction *target = &module->functions[op->immediate];
                CHECK(target->name_length < 9 || memcmp(target->name,"required$",9));
            }
        }
    }
    CHECK(reference_instances == 4 && references == 4);
    xr_xir_artifact_free(closed);
    printf("Source generics: %zu definition, forwarding and call rejections; four concrete domains admitted\n",
        sizeof(rejected) / sizeof(rejected[0]));
    return 0;
}
