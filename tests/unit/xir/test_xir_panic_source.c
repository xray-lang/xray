/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_panic_source.c - Source panic regions through Checked and Lowered
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_checked.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_panic_cases.h"
static void panic_find(const XrXirModule *module, uint32_t *functions) {
    const char *names[] = {"direct", "bound", "bounds", "nested", "channels", "throughOrdinary",
        "localState", "indirect", "suspended", "suspendedHandler", "information", "unmatched", "uncaught"};
    for (unsigned n = 0; n < PANIC_ENTRY; ++n) {
        functions[n] = UINT32_MAX;
        for (uint32_t i = 0; i < module->function_count; ++i) {
            const XrXirFunction *f = &module->functions[i];
            if (f->name_length == strlen(names[n]) && !memcmp(f->name, names[n], f->name_length)) {
                CHECK(functions[n] == UINT32_MAX); functions[n] = i;
            }
        }
        CHECK(functions[n] != UINT32_MAX);
    }
    functions[PANIC_ENTRY] = module->declarations->entry_function;
}
static void panic_emit(XrXirArtifact *lowered, const uint32_t *functions, const char *path) {
    XrXirCSource source = {0};
    CHECK(xr_xir_compile_emit_c(lowered, functions ? "panic_source" : "panic_bad", 4194304, &source) == XR_XIR_OK);
    CHECK(!strstr(source.text, "({"));
    if (path) {
        FILE *file = fopen(path, functions ? "wb" : "ab"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        if (functions) {
            CHECK(fprintf(file, "\nconst uint32_t panic_source_functions[%u] = {", PANIC_FUNCTIONS) > 0);
            for (unsigned i = 0; i < PANIC_FUNCTIONS; ++i)
                CHECK(fprintf(file, "%s%uu", i ? "," : "", functions[i]) > 0);
            CHECK(fputs("};\n", file) >= 0);
        }
        CHECK(fclose(file) == 0);
    }
    xr_xir_compile_c_source_free(&source);
}
static XrXirArtifact *panic_source_lower(const char *path) {
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILER_SESSION_OK && session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_PANIC_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, &context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr, "panic source %u at %u:%d:%d %s\n", status,
        diagnostic.module, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(result.checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(closed);
    return lowered;
}
int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 2);
    XrXirArtifact *lowered = panic_source_lower(XR_PANIC_FIXTURES "/root.xr");
    uint32_t functions[PANIC_FUNCTIONS]; panic_find(xr_xir_compile_artifact_module(lowered), functions);
    panic_emit(lowered, functions, argc == 2 ? argv[1] : NULL);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK);
    CHECK(!lowered); panic_cases(program, functions); program = NULL;
    lowered = panic_source_lower(XR_PANIC_FIXTURES "/initialization.xr");
    uint32_t entry = xr_xir_compile_artifact_module(lowered)->declarations->entry_function;
    panic_emit(lowered, NULL, argc == 2 ? argv[1] : NULL);
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK);
    CHECK(!lowered); panic_sticky(program, entry);
    effects_source_owners_free();
    puts("Panic source VM matched independent values, channels and physical ownership");
    return 0;
}
