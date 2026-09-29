/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_effects_source.c - Control facts after source destruction and specialization
 */
#include "xir/xxir_effects.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void source_effects(const XrXirArtifact *artifact) {
    const struct { const char *name; XrXirEffect suspend, throws; } expected[] = {
        {"pure", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_NONE},
        {"sleeper", XR_XIR_EFFECT_MAY, XR_XIR_EFFECT_NONE},
        {"relay", XR_XIR_EFFECT_MAY, XR_XIR_EFFECT_NONE},
        {"instantiated", XR_XIR_EFFECT_MAY, XR_XIR_EFFECT_NONE},
        {"fail", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"forward", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"handled", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_NONE},
        {"rethrow", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"dynamic", XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_UNKNOWN},
        {"dynamicHandled", XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_NONE},
        {"panicOnly", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_NONE}};
    XrXirEffects *effects = NULL;
    CHECK(xr_xir_effects_analyze(artifact, NULL, &effects) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_artifact_module(artifact);
    for (size_t e = 0; e < sizeof(expected) / sizeof(expected[0]); ++e) {
        bool found = false;
        for (uint32_t f = 0; f < module->function_count; ++f) {
            const XrXirFunction *function = &module->functions[f];
            size_t length = strlen(expected[e].name);
            if (function->name_length < length || memcmp(function->name, expected[e].name, length) ||
                (function->name_length != length && function->name[length] != '$')) continue;
            const XrXirFunctionEffects *fact = xr_xir_effects_function(effects, f);
            CHECK(fact);
            if (fact->suspend != expected[e].suspend || fact->throws != expected[e].throws)
                fprintf(stderr, "effect %s: suspend=%u throws=%u\n", expected[e].name, fact->suspend, fact->throws);
            CHECK(fact->suspend == expected[e].suspend && fact->throws == expected[e].throws); found = true;
        }
        if (!found) fprintf(stderr, "missing effect function %s at stage %u\n", expected[e].name, module->stage);
        CHECK(found);
    }
    xr_xir_effects_free(effects);
}
int main(void) {
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_EFFECT_FIXTURES};
    XrXirSourceRequest request = {session, XR_EFFECT_FIXTURES "/root.xr", &authority, NULL, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_source_check(&request, &result, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "%u at %d:%d: %s\n", status,
        diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK); source_effects(result.checked);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(result.checked, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_source_result_free(&result); xr_compiler_session_delete(session);
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &checked, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_checked_packet_free(&packet); source_effects(checked);
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked); source_effects(closed);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed); source_effects(lowered); xr_xir_artifact_free(lowered);
    puts("Source control effects survived packets, specialization and lowering");
    return 0;
}
