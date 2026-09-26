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
#include "../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void write_generic_source(const char *path, const char *text) {
    FILE *file = fopen(path, "wb"); CHECK(file);
    CHECK(fwrite(text, 1, strlen(text), file) == strlen(text) && fclose(file) == 0);
}
int main(void) {
    const char *rejected[] = {
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
    }
    write_generic_source(root,
        "import \"./lib\" as lib\n"
        "fn relay<T>(value:T)->T where T:Sendable { return lib.required<T>(value) }\n"
        "fn unused<T,U>(left:T,right:U)->T { const copy = left; return copy }\n"
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
    xr_xir_artifact_free(closed);
    printf("Source generics: %zu definition, forwarding and call rejections; four concrete domains admitted\n",
        sizeof(rejected) / sizeof(rejected[0]));
    return 0;
}
