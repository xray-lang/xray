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
int main(void) {
    static const char *const rejected[] = {
        "fn unused() -> i64 { return true }\n",
        "fn unused<T>(value: T) -> T { return value }\n",
        "fn value(x: i64) -> i64 { return x }\nvalue(true)\n",
        "fn value(x: i64) -> i64 { return x }\nvalue()\n",
        "const a = 1\na = 2\n",
        "const a: string = 1\n",
        "const a = 1\nconst a = 2\n",
        "print(1, 2, 3)\n",
        "print(Atomic(1))\n",
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
        "fn unused() -> i64 { return 1 + 2 }\n",
        "const a = \"unterminated\n"
    };
    char directory[XR_TEST_PATH_MAX] = "xir-source-admission-XXXXXX";
    CHECK(xr_test_mkdtemp(directory));
    char absolute[XR_TEST_PATH_MAX];
    CHECK(xr_test_realpath_buf(directory, absolute, sizeof(absolute)));
    char root[XR_TEST_PATH_MAX], library[XR_TEST_PATH_MAX];
    CHECK(snprintf(root, sizeof(root), "%s/root.xr", absolute) > 0);
    CHECK(snprintf(library, sizeof(library), "%s/lib.xr", absolute) > 0);
    write_source(library, "fn hidden() -> i64 { return 1 }\nexport fn visible() -> i64 { return 2 }\n");
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, absolute};
    XrXirSourceRequest request = {session, root, &authority, NULL};
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        write_source(root, rejected[i]);
        XrXirArtifact *artifact = NULL; XrXirSourceDiagnostic diagnostic;
        XrXirStatus status = xr_xir_source_check(&request, &artifact, &diagnostic);
        if (status == XR_XIR_OK) fprintf(stderr, "incorrectly admitted source case %zu\n", i);
        CHECK(status != XR_XIR_OK && !artifact && diagnostic.status == status && diagnostic.message[0]);
    }
    const char *valid = "import { visible } from \"./lib\"\nprint(visible(), true)\n";
    write_source(root, valid);
    for (unsigned mode = 0; mode < 4; ++mode) {
        XrXirBudget budget = xr_xir_default_budget();
        if (mode == 0) budget.metadata_bytes = 1;
        if (mode == 1) budget.work = 0;
        if (mode == 2) budget.instructions = 1;
        if (mode == 3) budget.functions = 1;
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
    xr_compiler_session_delete(session);
    CHECK(xr_test_unlink(root) == 0 && xr_test_unlink(library) == 0 && xr_test_rmdir(directory) == 0);
    puts("Source declaration, visibility, type, graph and budget rejection passed");
    return 0;
}
