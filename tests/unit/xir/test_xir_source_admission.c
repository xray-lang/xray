/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_admission.c - Reject invalid source without a partial artifact
 *
 * KEY CONCEPT:
 *   Reusing a compiler session must not retain declarations from rejected input.
 */
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#include "module/xmodule_resolver.h"
#include "base/xmalloc.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void write_source(const char *path, const char *source) {
    FILE *file = fopen(path, "wb"); CHECK(file);
    size_t length = strlen(source);
    CHECK(fwrite(source, 1, length, file) == length && fclose(file) == 0);
}
static void stdlib_resolution(void) {
    XrModuleResolverConfig config = {XR_SOURCE_STDLIB, NULL};
    XrModuleResolver *resolver = xr_module_resolver_new(&config); CHECK(resolver);
    XrModuleId first = {0}, second = {0}; char *error = NULL;
    CHECK(xr_module_resolver_resolve(resolver, "std/io/output", NULL, NULL, &first, &error) == 0 && !error);
    CHECK(first.kind == XR_MOD_STDLIB && first.authority.kind == XR_MODULE_IDENTITY_STDLIB);
    CHECK(!strcmp(first.authority.namespace_id, "io") && !strcmp(first.logical_path, "io/output.xr"));
    CHECK(xr_module_identity_valid(first.canonical, NULL));
    CHECK(xr_module_resolver_resolve(resolver, "std/io/output", NULL, NULL, &second, &error) == 0 && !error);
    CHECK(!strcmp(first.canonical, second.canonical) && first.canonical != second.canonical);
    xr_module_id_cleanup(&first); xr_module_id_cleanup(&second);
    const char *invalid[] = {"std/io", "std/io/io", "std/io/../io/output", "std/io//output", "std/io/output.xr",
        "std/io/output/", "std/io/output/index", "std/io/output:bad", "std/io/0output", "std/unknown/output"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK(xr_module_resolver_resolve(resolver, invalid[i], NULL, NULL, &first, &error) != 0);
        CHECK(!first.canonical && !first.source_path && !first.authority.physical_root);
        xr_free(error); error = NULL;
    }
    xr_module_resolver_free(resolver);
    config.stdlib_path = NULL; resolver = xr_module_resolver_new(&config); CHECK(resolver);
    CHECK(xr_module_resolver_resolve(resolver, "std/io/output", NULL, NULL, &first, &error) != 0);
    CHECK(error && !first.canonical); xr_free(error); xr_module_resolver_free(resolver);
}
static void primitive_authority(XrCompilerSession *session, const char *directory) {
    char io[XR_TEST_PATH_MAX], output[XR_TEST_PATH_MAX], other[XR_TEST_PATH_MAX];
    CHECK(snprintf(io, sizeof(io), "%s/io", directory) > 0 && xr_test_mkdir(io) == 0);
    CHECK(snprintf(output, sizeof(output), "%s/output.xr", io) > 0);
    CHECK(snprintf(other, sizeof(other), "%s/other.xr", io) > 0);
    const char *body = "export fn emit(value: string) -> bool { return __writeStderr(value) }\n";
    write_source(output, body); write_source(other, body);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, directory};
    XrXirSourceRequest request = {session, output, &authority, NULL, XR_SOURCE_STDLIB};
    XrXirArtifact *artifact = NULL;
    CHECK(xr_xir_source_check(&request, &artifact, NULL) != XR_XIR_OK && !artifact);
    authority.kind = XR_MODULE_IDENTITY_STDLIB; authority.namespace_id = "math";
    CHECK(xr_xir_source_check(&request, &artifact, NULL) != XR_XIR_OK && !artifact);
    authority.namespace_id = "io"; request.entry_path = other;
    CHECK(xr_xir_source_check(&request, &artifact, NULL) != XR_XIR_OK && !artifact);
    request.entry_path = output;
    CHECK(xr_xir_source_check(&request, &artifact, NULL) == XR_XIR_OK && artifact);
    xr_xir_artifact_free(artifact); artifact = NULL;
    write_source(output, "export fn emit() -> bool { return __writeStderr(1) }\n");
    CHECK(xr_xir_source_check(&request, &artifact, NULL) != XR_XIR_OK && !artifact);
    write_source(output, "export fn emit() -> bool { return __writeStderr() }\n");
    CHECK(xr_xir_source_check(&request, &artifact, NULL) != XR_XIR_OK && !artifact);
    write_source(output, "fn __writeStderr(value: i64) -> i64 { return value }\nprint(__writeStderr(7))\n");
    CHECK(xr_xir_source_check(&request, &artifact, NULL) == XR_XIR_OK && artifact);
    xr_xir_artifact_free(artifact);
    CHECK(xr_test_unlink(output) == 0 && xr_test_unlink(other) == 0 && xr_test_rmdir(io) == 0);
}
static void shadowed_coro(const XrXirSourceRequest *request, const char *root) {
    write_source(root, "import \"./lib\" as Coro\nprint(Coro.visible())\n");
    XrXirArtifact *artifact = NULL;
    CHECK(xr_xir_source_check(request, &artifact, NULL) == XR_XIR_OK && artifact);
    const XrXirModule *module = xr_xir_artifact_module(artifact);
    for (uint32_t f = 0; f < module->function_count; ++f)
        for (uint32_t i = 0; i < module->functions[f].instruction_count; ++i)
            CHECK(module->functions[f].instructions[i].op != XR_XIR_SUSPEND);
    xr_xir_artifact_free(artifact);
}
static const char *const rejected[] = {
    "fn unused(f:fn(i64)->i64)->i64 { return f(true) }\n",
    "fn unused(f:fn(i64)->i64)->i64 { return f() }\n",
    "fn unused(f:fn(i64)->i64)->i64 { return f<i64>(1) }\n",
    "fn unused(f:fn(i64)->i64)->string { return f(1) }\n",
    "fn unused(f:fn(ref i64)->i64) {}\n",
    "fn unused(f:fn(move string)->string) {}\n",
    "fn unused<T>(f:fn(T)->T) {}\n",
    "fn f(x:i64)->i64 { return x } const g:fn(bool)->i64=f\n",
    "fn f<T>(x:T)->T { return x } const g=f\n",
    "import \"./lib\" as lib\nconst f=lib.hidden\n",
    "fn send<T:Sendable>(x:T)->T { return x } fn f()->i64 {return 1} const g=send<fn()->i64>(f)\n",

    "Coro.yield(1)\n",
    "Coro.yield<i64>()\n",
    "const x = Coro.yield()\n",
    "const Coro = Atomic(1)\nCoro.yield()\n",
    "fn unused(Coro:i64) { Coro.yield() }\n",
    "Coro.missing()\n",
    "const alias = Coro\nalias.yield()\n",
    "fn unused()->i64 { return Coro.yield() }\n",
    "fn unused<T>(value:T)->T { return true ? value : Coro.yield() }\n",

    "fn unused() -> i64 { return true }\n",
    "fn unused<T>(value: T) -> T { return value.missing() }\n",
    "fn value(x: i64) -> i64 { return x }\nvalue(true)\n",
    "fn value(x: i64) -> i64 { return x }\nvalue()\n",
    "const a = 1\na = 2\n",
    "const a: string = 1\n",
    "const a = 1\nconst a = 2\n",
    "print(Atomic(1), 2, 3)\n",
    "print(Atomic(1))\n",
    "__writeStderr(\"forbidden\")\n",
    "import { __writeStderr } from \"std/io/output\"\n",
    "import { writeStderr } from \"std/io/output\"\nwriteStderr(1)\n",
    "import \"std/io/output\" as output\noutput.__writeStdout(\"forbidden\")\n",
    "const a = Atomic(true)\n",
    "const a = Atomic(1)\na.fetchAdd(false)\n",
    "const a = Atomic(1)\na.load(1)\n",
    "print(missing)\n",
    "print(later)\nconst later = 1\n",
    "fn value() -> i64 {}\n",
    "fn value() { return 1 }\n",
    "fn value(x: i64, x: i64) -> i64 { return x }\n",
    "fn value(x: string) { x = \"modified\" }\n",
    "fn value() { var x = 1; x = true }\n",
    "fn value() { var x = 1; var x = 2 }\n",
    "import \"./lib\" as lib\nlib.hidden()\n",
    "import { hidden } from \"./lib\"\n",
    "import { missing } from \"./lib\"\n",
    "import \"./lib\" as lib\nconst lib = 1\n",
    "import \"./missing\" as missing\n",
    "import \"./lib.xr\" as lib\n",
    "const print = 1\nprint(1)\n",
    "const Atomic = 1\nAtomic(1)\n",
    "fn unused() { while true {} }\n",
    "fn unused() { if (1) {} }\n",
    "fn unused() { while (1) {} }\n",
    "fn unused(x: bool) -> i64 { if (x) { return 1 } }\n",
    "fn unused(x: bool) { if (x) { var hidden = 1 } print(hidden) }\n",
    "fn unused() { break }\n",
    "fn unused() { continue }\n",
    "fn unused() { while (true) { break; print(1) } }\n",
    "fn unused(x: bool) { if (x) { return } else { return } print(1) }\n",
    "if (true) { return }\n",
    "fn unused<T>(x:T) { if (true) { print(x) } }\n",
    "fn unused<T>(x:T) { var y=x; while (y < x) {} }\n",
    "fn unused<T>(x:T)->T { return ~x }\n",
    "fn unused<T>(x:T)->T { return x << 1 }\n",
    "fn unused<T>(x:T)->T { return x & x }\n",
    "fn unused()->i64 { return true & false }\n",
    "fn unused()->i64 { return 1 << false }\n",
    "fn unused()->i64 { return ~true }\n",
    "fn unused()->i64 { return \"x\" | \"y\" }\n",
    "fn unused() { const x=1; x <<= 1 }\n",
    "fn unused() { var x=true; x ^= false }\n",
    "fn unused<T>(x:T) { var y=x; y += x }\n",
    "fn unused(x:i64) { x &= 1 }\n",
    "fn unused() { var x=\"a\"; x -= \"b\" }\n",
    "fn unused() { var x=\"a\"; x += 1 }\n",
    "fn unused() { var x=1; x += true }\n",
    "fn unused() -> i64 { return 1 + true }\n",
    "fn unused() { print(!1) }\n",
    "fn unused() { print(false && 1) }\n",
    "fn unused() { print(true || missing) }\n",
    "fn unused<T>(x:T) { print(false && x) }\n",
    "fn unused() { for (; 1;) { break } }\n",
    "fn unused() { for (var i=0; i<1; i++) {} print(i) }\n",
    "fn unused() { for (var i=0; true; i=hidden) { var hidden=1; break } }\n",
    "fn unused() { for (;; missing()) { break } }\n",
    "fn unused<T>(x:T) { for (;; x+1) { return } }\n",
    "fn unused() { for (;;) { break; print(1) } }\n",
    "fn unused() { const i=1; i++ }\n",
    "fn unused() { var i=true; i++ }\n",
    "fn unused(i:i64) { i-- }\n",
    "fn unused() { var i=1; print(i++) }\n",
    "fn unused() { for (var i=1;; i++) { const i=true; i++ } }\n",
    "fn unused<T>(x:T)->T { return x * x }\n" ,
    "fn unused<T>(x:T)->T { return -x }\n" ,
    "fn unused<T>(x:T)->bool { return x >= x }\n" ,
    "fn unused()->i64 { return true - 1 }\n" ,
    "fn unused()->i64 { return 1 / false }\n" ,
    "fn unused()->i64 { return -true }\n" ,
    "fn unused()->i64 { return \"x\" % 2 }\n" ,
    "fn unused()->bool { return \"x\" <= \"y\" }\n" ,
    "const a = \"unterminated\n"
};
int main(void) {
    stdlib_resolution();

    char directory[XR_TEST_PATH_MAX] = "xir-source-admission-XXXXXX";
    CHECK(xr_test_mkdtemp(directory));
    char absolute[XR_TEST_PATH_MAX];
    CHECK(xr_test_realpath_buf(directory, absolute, sizeof(absolute)));
    char root[XR_TEST_PATH_MAX], library[XR_TEST_PATH_MAX];
    CHECK(snprintf(root, sizeof(root), "%s/root.xr", absolute) > 0);
    CHECK(snprintf(library, sizeof(library), "%s/lib.xr", absolute) > 0);
    write_source(library, "fn hidden() -> i64 { return 1 }\nexport fn visible() -> i64 { return 2 }\n");
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    primitive_authority(session, absolute);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, absolute};
    XrXirSourceRequest request = {session, root, &authority, NULL, XR_SOURCE_STDLIB};
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        write_source(root, rejected[i]);
        XrXirArtifact *artifact = NULL; XrXirSourceDiagnostic diagnostic;
        XrXirStatus status = xr_xir_source_check(&request, &artifact, &diagnostic);
        if (status == XR_XIR_OK) fprintf(stderr, "incorrectly admitted source case %zu\n", i);
        CHECK(status != XR_XIR_OK && !artifact && diagnostic.status == status && diagnostic.message[0]);
    }
    const char *valid = "import { visible } from \"./lib\"\nprint(visible(), true)\n";
    write_source(root, valid);
    for (unsigned mode = 0; mode < 5; ++mode) {
        XrXirBudget budget = xr_xir_default_budget();
        if (mode == 0) budget.metadata_bytes = 1;
        if (mode == 1) budget.work = 0;
        if (mode == 2) budget.instructions = 1;
        if (mode == 3) budget.functions = 1;
        if (mode == 4) budget.blocks = 0;
        request.budget = &budget;
        XrXirArtifact *artifact = NULL;
        CHECK(xr_xir_source_check(&request, &artifact, NULL) == XR_XIR_BUDGET && !artifact);
    }
    request.budget = NULL;
    write_source(library, "var forbidden = 1\nexport fn visible() -> i64 { return forbidden }\n");
    XrXirArtifact *artifact = NULL;
    CHECK(xr_xir_source_check(&request, &artifact, NULL) != XR_XIR_OK && !artifact);
    write_source(library, "import \"./root\" as root\nexport fn visible() -> i64 { return 1 }\n");
    CHECK(xr_xir_source_check(&request, &artifact, NULL) != XR_XIR_OK && !artifact);
    write_source(library, "export fn visible() -> i64 { return 2 }\n");
    CHECK(xr_xir_source_check(&request, &artifact, NULL) == XR_XIR_OK && artifact);
    xr_xir_artifact_free(artifact);
    shadowed_coro(&request, root);
    xr_compiler_session_delete(session);
    CHECK(xr_test_unlink(root) == 0 && xr_test_unlink(library) == 0 && xr_test_rmdir(directory) == 0);
    puts("Source declaration, visibility, type, graph and budget rejection passed");
    return 0;
}
