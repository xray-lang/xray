/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_closure_cases.h - Lexical captures and definition-time admission
 *
 * KEY CONCEPT:
 *   Unused mutable bindings do not leak into immutable environments.
 */
#ifndef XR_XIR_SOURCE_CLOSURE_CASES_H
#define XR_XIR_SOURCE_CLOSURE_CASES_H
static void source_closure_cases(const XrXirSourceRequest *request) {
    const char *rejected[] = {
        "fn make() { var x = 1; const f = fn()->i64 { return x } }\n",
        "fn make() { var x = 1; const f = fn() { x = 2 } }\n",
        "fn make() { var x = 1; const f = fn() { x += 2 } }\n",
        "fn make() { var x = 1; const f = fn() { x++ } }\n",
        "fn make() { var x = 1; const f = fn() { const g = fn()->i64 { return x } } }\n",
        "fn make() { const x = 1; const f = fn() { x = 2 } }\n",
        "const f = fn(x) { return x }\n",
        "const f = (x) -> x\n",
        "const f = fn(x:ref i64) { return x }\n",
        "const f = fn(x:move string) { return x }\n",
        "const f = fn<T>(x:T)->T { return x }\n",
        "const f = fn(x:i64)->bool { return x }\n",
        "const f = fn(x:bool) { if (x) { return 1 } else { return true } }\n",
        "const f = fn(x:bool) { if (x) { return 1 } }\n",
        "const f = fn(x:bool) { if (x) { return } else { return 1 } }\n",
        "fn unused<T>(x:T) { const f = fn() { return x + x } }\n",
        "import \"./lib\" as lib\nfn unused<T>(x:T) { const f = fn() { return lib.required<T>(x) } }\n",
        "import \"./lib\" as lib\nconst f = fn() { return lib.hidden<i64>(1) }\n",
        "fn make() { const f = fn() { return missing } }\n",
        "fn make() { const f = fn() { return later }; const later = 1 }\n",
        "const f = fn(x:i64,x:i64) { return x }\n",
        "const f = fn() { break }\n"
    };
    for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        write_generic_source(request->entry_path, rejected[i]);
        XrXirArtifact *checked = NULL; XrXirSourceDiagnostic diagnostic;
        XrXirStatus status = xr_xir_source_check(request, &checked, &diagnostic);
        if (status == XR_XIR_OK) fprintf(stderr, "incorrectly accepted closure case %u\n", i);
        CHECK(status != XR_XIR_OK && !checked && diagnostic.status == status);
        if (i < 5) {
            if (status != XR_XIR_BAD_TYPE || !strstr(diagnostic.message, "shared cell"))
                fprintf(stderr, "closure rejection %u: %s (%u)\n", i, diagnostic.message, status);
            CHECK(status == XR_XIR_BAD_TYPE && strstr(diagnostic.message, "shared cell"));
        }
    }
    const char *accepted[] = {
        "fn make() { var unused = 1; const f = fn()->i64 { return 7 } }\n",
        "fn make(x:i64) { const f = (x:i64) -> x + 1 }\n",
        "fn make() { var x = 1; const f = fn() { const x = 2; return x } }\n",
        "fn make(x:i64) { const f = fn() { const x = x + 1; return x } }\n",
        "fn make(x:i64) { const f = fn() { return x + x } }\n",
        "fn make(x:i64) { const f = fn() { const g = fn() { return x }; return g() } }\n",
        "fn make() { var x = 1; const f = fn() { for (var x = 0; x < 1; x++) {} } }\n",
        "fn make(x:i64) { const f = fn() { { const x = 2 }; return x } }\n",
        "var x = 1; const f = fn() { return x }\n",
        ("import \"./lib\" as lib\nfn make<T:Sendable>(x:T)->fn()->T { return fn() { return lib.required<T>(x) } }\n"
        "const f = make<string>(\"owned\"); const n = make<i64>(7)\n"),
        "fn make() { const f = fn() { return }; const g = () -> 7 }\n"
    };
    const unsigned captures[] = {0, 0, 0, 1, 1, 2, 0, 1, 0, 1, 0};
    for (unsigned i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
        write_generic_source(request->entry_path, accepted[i]);
        XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL;
        XrXirSourceDiagnostic diagnostic;
        XrXirStatus status = xr_xir_source_check(request, &checked, &diagnostic);
        if (status != XR_XIR_OK) fprintf(stderr, "closure case %u: %s (%u)\n", i, diagnostic.message, status);
        CHECK(status == XR_XIR_OK && checked);
        const XrXirModule *module = xr_xir_artifact_module(checked);
        unsigned count = 0;
        for (uint32_t f = 0; f < module->function_count; ++f)
            for (uint32_t n = 0; n < module->functions[f].instruction_count; ++n) {
                const XrXirInstruction *op = &module->functions[f].instructions[n];
                if (op->op == XR_XIR_FUNCTION_REF) count += op->args[1];
            }
        CHECK(count == captures[i]);
        XrXirCheckedPacket packet = {0};
        CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
        xr_xir_artifact_free(checked);
        CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
        xr_xir_checked_packet_free(&packet);
        CHECK(xr_xir_specialize(decoded, NULL, &closed, NULL) == XR_XIR_OK);
        xr_xir_artifact_free(decoded);
        CHECK(xr_xir_artifact_verify(closed, NULL, NULL) == XR_XIR_OK);
        xr_xir_artifact_free(closed);
    }
    puts("Source closures: 22 fail-closed cases and 11 exact lexical capture/Checked round-trips passed");
}
#endif
