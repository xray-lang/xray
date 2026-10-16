/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_iterator_lifetime_producer.c - Real owned generators and pull consumers to portable C
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Iterator lifetime producer %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"

int main(int argc, char **argv) {
    CHECK(argc == 2);
    const XrXirCompileContext context = *effects_source_owner(67108864, 128000000);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_ITERATOR_LIFETIME_FIXTURES};
    XrXirSourceRequest request = {session, XR_ITERATOR_LIFETIME_FIXTURES "/root.xr", &authority,
        &context, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult source = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &source, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr, "Iterator lifetime Source status=%u at=%d:%d message=%s\n",
        status, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(source.snapshot);
    CHECK(view && view->complete && view->modules && view->module_count>=3);
    for (uint32_t m=0;m<view->module_count;++m) {
        CHECK(view->modules[m].identity && view->modules[m].path);
        printf("P20 Iterator Source input path=%s fingerprint=",view->modules[m].path);
        for (unsigned b=0;b<XR_FINGERPRINT_BYTES;++b) printf("%02x",view->modules[m].fingerprint.bytes[b]);
        putchar('\n');
    }
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(source.checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&source); xr_compile_session_free(session);
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xA5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(closed);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    uint32_t entries[4] = {UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    const char *names[] = {"make","next","has","nth"}; const uint32_t parameters[] = {0,1,1,2};
    for (unsigned i = 0; i < 4; ++i) {
        for (uint32_t f = 0; f < module->function_count; ++f) {
            const XrXirFunction *function = &module->functions[f];
            if (module->declarations->functions[f].module != module->declarations->root_module) continue;
            if (function->name_length != strlen(names[i]) || memcmp(function->name, names[i], function->name_length)) continue;
            CHECK(entries[i] == UINT32_MAX && function->parameter_count == parameters[i]);
            CHECK((module->declarations->functions[f].module == module->declarations->root_module) == true);
            entries[i] = f;
        }
        CHECK(entries[i] != UINT32_MAX);
    }
    XrXirCSource generated = {0};
    CHECK(xr_xir_compile_emit_c(lowered, "p20_iterator_lifetime_fixture", 4194304, &generated) == XR_XIR_OK);
    CHECK(generated.text && !strstr(generated.text, "({") && !strstr(generated.text, "xr_xir_vm"));
    FILE *file = fopen(argv[1], "wb"); CHECK(file);
    CHECK(fwrite(generated.text, 1, generated.length, file) == generated.length);
    CHECK(fprintf(file, "\nconst uint32_t p20_iterator_lifetime_entries[4] = {%uu,%uu,%uu,%uu};\n", entries[0], entries[1], entries[2], entries[3]) > 0);
    CHECK(fclose(file) == 0);
    printf("Iterator lifetime real Source modules=%u functions=%u slots=%u cbytes=%zu\n",
        module->declarations->module_count, module->function_count, module->declarations->slot_count, generated.length);
    xr_xir_compile_c_source_free(&generated); xr_xir_compile_artifact_free(lowered);
    effects_source_owners_free(); return 0;
}
