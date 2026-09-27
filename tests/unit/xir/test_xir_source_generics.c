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
#include "xir/xxir_internal.h"
#include "xir/xxir_types.h"
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
#include "xir_source_closure_cases.h"
/* Characterize the open specialization authority gap without granting access. */
static void nominal_specialization_authority(XrXirSourceRequest *request, const char *library) {
    write_generic_source(library,
        "struct LibraryPrivate{value:i64}\n"
        "fn privateHelper()->i64{return LibraryPrivate{value:7}.value}\n"
        "export fn identity<T>(x:T)->T{privateHelper();return x}\n");
    const char *sources[] = {
        "struct Private{value:i64}\nfn identity<T>(x:T)->T{return x}\nconst p=identity<Private>(Private{value:7})\n",
        "import \"./lib\" as lib\nstruct Private{value:i64}\nconst p=lib.identity<Private>(Private{value:7})\n",
        "import \"./lib\" as lib\nexport struct Public{value:i64}\nconst p=lib.identity<Public>(Public{value:7})\n",
        "import \"./lib\" as lib\nconst p=lib.identity<i64>(7)\n"
    };
    for (unsigned mode = 0; mode < 4; ++mode) {
        write_generic_source(request->entry_path, sources[mode]);
        XrXirSourceResult result = {0};
        CHECK(xr_xir_source_check(request, &result, NULL) == XR_XIR_OK && result.checked);
        XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL, *closed = NULL;
        CHECK(xr_xir_checked_write(result.checked, NULL, &packet, NULL) == XR_XIR_OK);
        xr_xir_source_result_free(&result);
        CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
        xr_xir_checked_packet_free(&packet);
        XrXirDiagnostic diagnostic = {0};
        XrXirStatus status = xr_xir_specialize(decoded, NULL, &closed, &diagnostic);
        {
            CHECK(status == XR_XIR_OK && closed);
            if (mode) {
                const XrXirModule *module = xr_xir_artifact_module(closed);
                bool instance = false, helper = false;
                for (uint32_t f = 0; f < module->function_count; ++f) {
                    const XrXirFunction *function = &module->functions[f];
                    bool identity = function->name_length > 9 && !memcmp(function->name,"identity$",9);
                    bool private_helper = function->name_length == 13 && !memcmp(function->name,"privateHelper",13);
                    if (!identity && !private_helper) continue;
                    const XrXirFunctionIdentity *scope = &module->declarations->functions[f];
                    const XrXirSourceModule *owner = &module->declarations->modules[scope->module];
                    CHECK(owner->name_length >= 6 && !memcmp(owner->name + owner->name_length - 6,"lib.xr",6));
                    if (identity) instance = true;
                    if (private_helper) { CHECK(!scope->exported); helper = true; }
                }
                CHECK(instance && helper);
            }
        }
        if (mode == 1 || mode == 2) {
            XrXirProvenance *proof = (XrXirProvenance *)closed->module.provenance;
            CHECK(proof && proof->source);
            closed->module.provenance = NULL;
            CHECK(xr_xir_artifact_verify(closed, NULL, NULL) == XR_XIR_BAD_TYPE);
            XrXirCheckedPacket stripped = {0};
            CHECK(xr_xir_checked_write(closed, NULL, &stripped, NULL) == XR_XIR_BAD_TYPE && !stripped.bytes);
            closed->module.provenance = proof;
            uint32_t instance = UINT32_MAX;
            for (uint32_t f = 0; f < closed->module.function_count; ++f)
                if (closed->module.functions[f].name_length > 9 &&
                    !memcmp(closed->module.functions[f].name, "identity$", 9)) instance = f;
            CHECK(instance != UINT32_MAX);
            uint32_t original = proof->origins[instance].function;
            proof->origins[instance].function = UINT32_MAX;
            CHECK(xr_xir_artifact_verify(closed, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
            proof->origins[instance].function = original;
            const XrXirTypes *types = proof->source->module.types;
            XrXirType opaque = XR_XIR_UNIT;
            const char *name = mode == 1 ? "Private" : "Public";
            for (uint32_t t = 0; t < types->count; ++t) {
                const XrXirTypeNode *node = &types->nodes[t];
                if (node->kind != XR_XIR_TYPE_NOMINAL || node->parameter_span) continue;
                XrXirLiteral declared = types->nominals->declarations[node->nominal.declaration].name;
                if (declared.length == strlen(name) && !memcmp(declared.bytes, name, declared.length))
                    opaque = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + t);
            }
            CHECK(opaque != XR_XIR_UNIT);
            XrXirFunction *definition = (XrXirFunction *)&proof->source->module.functions[original];
            XrXirType saved = definition->result; definition->result = opaque;
            CHECK(xr_xir_artifact_verify(closed, NULL, NULL) == XR_XIR_BAD_TYPE);
            definition->result = saved;
            CHECK(xr_xir_artifact_verify(closed, NULL, NULL) == XR_XIR_OK);
        }
        xr_xir_artifact_free(closed); xr_xir_artifact_free(decoded);
    }
}
static void member_call_authority(XrXirSourceRequest *request) {
    write_generic_source(request->entry_path,
        "struct S<T>{value:T;private hidden()->T{return this.value};get()->T{return this.hidden()}}\n"
        "const s=S<i64>{value:7};const n=s.get()\n");
    XrXirSourceResult result = {0};
    CHECK(xr_xir_source_check(request, &result, NULL) == XR_XIR_OK);
    XrXirModule *module = &result.checked->module;
    uint32_t hidden = UINT32_MAX, public_method = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if (module->functions[f].name_length == 6 && !memcmp(module->functions[f].name, "hidden", 6)) hidden = f;
        if (module->functions[f].name_length == 3 && !memcmp(module->functions[f].name, "get", 3)) public_method = f;
    }
    CHECK(hidden != UINT32_MAX && public_method != UINT32_MAX);
    XrXirFunctionIdentity *identity = (XrXirFunctionIdentity *)&module->declarations->functions[hidden];
    CHECK(identity->member_access == XR_XIR_MEMBER_PRIVATE && !identity->exported && identity->nominal_owner);
    uint32_t owner = identity->nominal_owner;
    identity->member_access = 3;
    CHECK(xr_xir_artifact_verify(result.checked, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    identity->member_access = XR_XIR_MEMBER_PRIVATE; identity->exported = 1;
    CHECK(xr_xir_artifact_verify(result.checked, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    identity->exported = 0; identity->nominal_owner = 0;
    CHECK(xr_xir_artifact_verify(result.checked, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    identity->nominal_owner = owner;
    bool attacked = false;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if (module->declarations->functions[f].nominal_owner) continue;
        for (uint32_t i = 0; i < module->functions[f].instruction_count; ++i) {
            XrXirInstruction *op = (XrXirInstruction *)&module->functions[f].instructions[i];
            if (op->op != XR_XIR_CALL || op->immediate != public_method) continue;
            op->immediate = hidden;
            CHECK(xr_xir_artifact_verify(result.checked, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
            XrXirCheckedPacket bad = {0};
            CHECK(xr_xir_checked_write(result.checked, NULL, &bad, NULL) == XR_XIR_BAD_STRUCTURE && !bad.bytes);
            op->immediate = public_method; attacked = true;
        }
    }
    CHECK(attacked && xr_xir_artifact_verify(result.checked, NULL, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL, *closed = NULL;
    CHECK(xr_xir_checked_write(result.checked, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_source_result_free(&result);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_specialize(decoded, NULL, &closed, NULL) == XR_XIR_OK);
    bool specialized = false;
    for (uint32_t f = 0; f < closed->module.function_count; ++f) {
        identity = (XrXirFunctionIdentity *)&closed->module.declarations->functions[f];
        if (identity->member_access != XR_XIR_MEMBER_PRIVATE) continue;
        identity->member_access = XR_XIR_MEMBER_PUBLIC;
        CHECK(xr_xir_artifact_verify(closed, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
        identity->member_access = XR_XIR_MEMBER_PRIVATE; specialized = true;
    }
    CHECK(specialized && xr_xir_artifact_verify(closed, NULL, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed); xr_xir_artifact_free(decoded);
}
static void member_reference_authority(XrXirSourceRequest *request, const char *library) {
    write_generic_source(request->entry_path,
        "struct S{get()->fn()->i64{return fn()->i64{return 1}}}\n"
        "const s=S();const f=s.get();const outsider=fn()->i64{return 2}\n");
    XrXirSourceResult result = {0};
    CHECK(xr_xir_source_check(request, &result, NULL) == XR_XIR_OK);
    XrXirModule *module = &result.checked->module;
    unsigned allowed = 0, denied = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        for (uint32_t i = 0; i < module->functions[f].instruction_count; ++i) {
            const XrXirInstruction *op = &module->functions[f].instructions[i];
            if (op->op != XR_XIR_FUNCTION_REF) continue;
            XrXirFunctionIdentity *id = (XrXirFunctionIdentity *)&module->declarations->functions[op->immediate];
            XrXirFunctionIdentity saved = *id;
            id->nominal_owner = 1; id->exported = 0; id->member_access = XR_XIR_MEMBER_PRIVATE;
            bool same_owner = module->declarations->functions[f].nominal_owner == 1;
            CHECK(xr_xir_artifact_verify(result.checked, NULL, NULL) ==
                (same_owner ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE));
            if (same_owner) ++allowed; else ++denied;
            *id = saved;
        }
    }
    CHECK(allowed == 1 && denied == 1);
    xr_xir_source_result_free(&result);
    write_generic_source(library, "export struct S{private secret()->i64{return 7};protected guarded()->i64{return 8}}\n");
    const char *sources[] = {"import \"./lib\" as lib\nlib.S().secret()\n",
        "import \"./lib\" as lib\nlib.S().guarded()\n"};
    for (unsigned i = 0; i < 2; ++i) {
        write_generic_source(request->entry_path, sources[i]);
        CHECK(xr_xir_source_check(request, &result, NULL) == XR_XIR_BAD_TYPE && !result.checked);
        xr_xir_source_result_free(&result);
    }
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
        "fn id<T>(x:T)->T { return x }\nid<()>(print())\n",
        "struct S<T>{value:T=0}\n",
        "struct S<T>{value:T=0}\nconst s=S<i64>{value:7}\n",
        "fn required<T:Sendable>(x:T)->T{return x}\nstruct S<T>{f:fn(T)->T=required<T>}\n",
        "struct S<T>{value:T=caller}\nfn make()->S<i64>{const caller=7;return S<i64>{}}\n",
        "struct S<T>{private value:i64=7}\nconst s=S<string>{}\n",
        "struct S<T>{value:T}\nconst s=S<i64>()\n",
        "struct S<T>{value:T}\nfn make<T>()->S<T>{return S<T>()}\n",
        "struct S<T>{count:i64}\nconst s=S()\n",
        "struct S{x:i64;bad(){this.x=1}}\n",
        "struct S<T>{x:T;bad()->T{return 0}}\n",
        "struct S{x:i64;get()->i64{return missing}}\n",
        "struct S{x:i64;x()->i64{return 1}}\n",
        "struct S{get(x:i64)->i64{return x}}\nconst s=S();s.get(true)\n",
        "struct S{get()->i64{return 1}}\nconst s=S();s.get<i64>()\n",
        "struct S{private get()->i64{return 1}}\nconst s=S();s.get()\n",
        "struct S{get()->i64{return 1}}\nconst s=S();const f=s.get\n",
        "struct S{protected get()->i64{return 1}}\nS().get()\n",
        "struct S{private get()->i64{return 1}}\nstruct U{read(s:S)->i64{return s.get()}}\n",
        "struct S<T>{private get()->T{return missing}}\n"
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
    member_reference_authority(&request, library);
    member_call_authority(&request);
    nominal_specialization_authority(&request, library);
    write_generic_source(library,
        "export fn required<T:Sendable>(x:T)->T { return x }\n"
        "fn hidden<T>(x:T)->T { return x }\n");
    source_closure_cases(&request);
    for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        write_generic_source(root, rejected[i]);
        XrXirArtifact *artifact = NULL; XrXirSourceDiagnostic diagnostic;
        XrXirSourceResult query_result_1 = {0};
        XrXirStatus query_status_1 = xr_xir_source_check(&request, &query_result_1, &diagnostic);
        artifact = query_result_1.checked; query_result_1.checked = NULL;
        xr_xir_source_result_free(&query_result_1);
        XrXirStatus status = query_status_1;
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
    XrXirSourceResult query_result_2 = {0};
    XrXirStatus query_status_2 = xr_xir_source_check(&request, &query_result_2, &diagnostic);
    checked = query_result_2.checked; query_result_2.checked = NULL;
    xr_xir_source_result_free(&query_result_2);
    XrXirStatus status = query_status_2;
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
