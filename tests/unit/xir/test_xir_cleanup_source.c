/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_cleanup_source.c - Source cleanup through packets, specialization and VM execution
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_nominal.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_runtime_allocations.h"
#include "xir_cleanup_source_cases.h"
static void cleanup_member_queries(const XrXirSourceView *view) {
    unsigned fields = 0, root = 0;
    CHECK(view && view->complete);
    for (uint32_t i = 0; i < view->reference_count; ++i) {
        const XrXirSourceReference *ref = &view->references[i];
        CHECK(ref->target && ref->target <= view->declaration_count);
        if (ref->access != XR_XIR_SOURCE_READ_WRITE) continue;
        const XrXirSourceDeclaration *decl = &view->declarations[ref->target - 1];
        if (decl->kind == XR_XIR_SOURCE_MEMBER && !strcmp(decl->name, "value")) ++fields;
        if (decl->kind == XR_XIR_SOURCE_BINDING && !strcmp(decl->name, "compoundState")) ++root;
    }
    CHECK(fields >= 7 && root >= 5);
}
static XrXirArtifact *cleanup_source_lower(const char *path) {
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_CLEANUP_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, NULL, NULL, NULL, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_source_check(&request, &result, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "cleanup source %u at %u:%d:%d %s\n", status,
        diagnostic.module, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK);
    cleanup_member_queries(xr_xir_source_snapshot_view(result.snapshot));
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(result.checked, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_source_result_free(&result); xr_compiler_session_delete(session);
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &checked, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK); xr_xir_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK); xr_xir_artifact_free(closed);
    return lowered;
}
static void cleanup_constructor_storage(const XrXirModule *module) {
    unsigned ordinary = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (function->name_length != 11 || memcmp(function->name, "constructor", 11)) continue;
        const XrXirTypeNode *type = xr_xir_type_node(module->types, function->result);
        CHECK(type && type->kind == XR_XIR_TYPE_NOMINAL);
        CHECK(module->types->nominals->identities && !module->types->nominals->declarations);
        XrXirLiteral name = module->types->nominals->identities[type->nominal.declaration].name;
        bool plain = name.length == 16 && !memcmp(name.bytes, "PlainConstructor", 16);
        bool unrelated = name.length == 21 && !memcmp(name.bytes, "UncapturedConstructor", 21);
        if (!plain && !unrelated) continue;
        ++ordinary;
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *op = &function->instructions[i];
            CHECK(op->op != XR_XIR_CELL_NEW && op->op != XR_XIR_CELL_LOCAL_WRITE);
        }
    }
    CHECK(ordinary == 2);
}
int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 2 || (argc == 3 && !strcmp(argv[1], "--fatal")));
    XrXirArtifact *lowered = cleanup_source_lower(XR_CLEANUP_FIXTURES "/root.xr");
    const XrXirModule *module = xr_xir_artifact_module(lowered);
    cleanup_constructor_storage(module);
    const char *names[] = {"scopes","loops","errors","panic","cancelled","generics","snapshot","constructorLate","constructorNested","constructorBare","constructorUncaptured","memberCompound","memberOperators","memberResume","memberCancel","memberFailure","fatalReturn","fatalError","fatalPanic","fatalCancel"};
    uint32_t functions[CLEANUP_SOURCE_FUNCTIONS + 4];
    for (unsigned n = 0; n < CLEANUP_SOURCE_FUNCTIONS + 4; ++n) {
        functions[n] = UINT32_MAX;
        for (uint32_t f = 0; f < module->function_count; ++f)
            if (module->functions[f].name_length == strlen(names[n]) &&
                !memcmp(module->functions[f].name, names[n], strlen(names[n]))) {
                CHECK(functions[n] == UINT32_MAX); functions[n] = f;
            }
        CHECK(functions[n] != UINT32_MAX);
    }
    XrXirCSource source = {0};
    CHECK(xr_xir_emit_c(lowered, "cleanup_source", 4194304, &source) == XR_XIR_OK);
    CHECK(!strstr(source.text, "({") && !strstr(source.text, "xr_xir_vm"));
    if (argc == 2) {
        FILE *file = fopen(argv[1], "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fprintf(file, "\nconst uint32_t cleanup_source_functions[%u] = {", CLEANUP_SOURCE_FUNCTIONS) > 0);
        for (unsigned i = 0; i < CLEANUP_SOURCE_FUNCTIONS; ++i) CHECK(fprintf(file, "%s%uu", i ? "," : "", functions[i]) > 0);
        CHECK(fputs("};\nconst uint32_t cleanup_source_fatal_functions[4] = {", file) >= 0);
        for (unsigned i = 0; i < 4; ++i) CHECK(fprintf(file, "%s%uu", i ? "," : "", functions[CLEANUP_SOURCE_FUNCTIONS+i]) > 0);
        CHECK(fputs("};\n", file) >= 0 && fclose(file) == 0);
    }
    xr_xir_c_source_free(&source);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_vm_program_take(&lowered, (XrXirProgramBudget){16777216, 64000000}, &program) == XR_XIR_OK);
    if (argc == 3) {
        unsigned mode = (unsigned)atoi(argv[2]); CHECK(mode < 4);
        cleanup_source_fatal(program, functions[CLEANUP_SOURCE_FUNCTIONS + mode]);
    }
    cleanup_source_allocations(program, functions);
    cleanup_source_cases(program, functions);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Source cleanup VM: lexical exits, late reads, nested bodies and independent results passed");
    return 0;
}
