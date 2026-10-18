/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_value_reference.c - Independent source value and rejection examples
 *
 * KEY CONCEPT:
 *   Literal results remain independent of backend output, and rejection must
 *   identify the intended source rule before an executable is published.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "../xir_source_fixture_owner.h"

static XrXirArtifact *reference_lower(XrXirArtifact *checked) {
    XrXirArtifact *specialized = NULL, *lowered = NULL;
    XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_specialize(checked, &specialized, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "specialize %u f%u i%u\n",
        (unsigned)status, diagnostic.function, diagnostic.instruction);
    CHECK(status == XR_XIR_OK && specialized);
    xr_xir_compile_artifact_free(checked);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(specialized, &target, &lowered, &diagnostic) == XR_XIR_OK && lowered);
    xr_xir_compile_artifact_free(specialized);
    return lowered;
}

static uint32_t reference_entry(const XrXirArtifact *artifact) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    uint32_t entry = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (function->name_length == 9 && !memcmp(function->name, "p09Answer", 9)) {
            CHECK(entry == UINT32_MAX); entry = f;
        }
    }
    CHECK(entry != UINT32_MAX);
    return entry;
}

static void reference_run(XrXirProgram *program, uint32_t entry, int64_t expected) {
    XrXirInstance *instances[2] = {NULL, NULL};
    XrXirValue escaped[2] = {{0}, {0}};
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    for (uint32_t i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instances[i], entry, NULL, 0) == XR_XIR_CALL_READY);
    }
    for (uint32_t i = 0; i < 2; ++i) {
        XrXirInstanceResult polled = xr_xir_instance_poll_bounded(instances[i], UINT64_MAX);
        if (polled.outcome.status != XR_XIR_CALL_RETURNED)
            fprintf(stderr, "instance %u outcome %u\n", i, (unsigned)polled.outcome.status);
        CHECK(polled.outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instances[i], &escaped[i]) == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for (uint32_t i = 0; i < 2; ++i) {
        if (escaped[i].payload != expected) fprintf(stderr, "expected %lld actual %lld\n",
            (long long)expected, (long long)escaped[i].payload);
        CHECK(escaped[i].type == XR_XIR_I64 && escaped[i].payload == expected);
        xr_xir_value_drop(&escaped[i]);
    }
}

int main(int argc, char **argv) {
    CHECK((argc == 4 && !strcmp(argv[1], "--accept")) ||
          (argc == 5 && !strcmp(argv[1], "--reject")));
    char path[1024];
    int bytes = snprintf(path, sizeof(path), "%s/%s.xr", XR_REFERENCE_FIXTURES, argv[2]);
    CHECK(bytes > 0 && (size_t)bytes < sizeof(path));
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(owner.context.resources, &session) == XR_COMPILER_SESSION_OK && session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_REFERENCE_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, &owner.context,
        XR_REFERENCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL,NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, NULL);
    printf("%s source status=%u location=%u:%d:%d message=%s\n", argv[2],
        (unsigned)status, diagnostic.module, diagnostic.line, diagnostic.column, diagnostic.message);
    if (!strcmp(argv[1], "--reject")) {
        CHECK(status == XR_XIR_BAD_TYPE && diagnostic.status == status);
        CHECK(diagnostic.line == atoi(argv[3]) && strstr(diagnostic.message, argv[4]));
        CHECK(!result.checked && !result.snapshot);
        xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
        source_fixture_owner_free(&owner);
        return 0;
    }
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(result.checked, &packet, NULL) == XR_XIR_OK);
    /* The consumer reconstructs from owned bytes after every source producer dies. */
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    XrXirArtifact *decoded = NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    XrXirArtifact *lowered = reference_lower(decoded);
    uint32_t entry = reference_entry(lowered);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    char *end = NULL;
    int64_t expected = (int64_t)strtoll(argv[3], &end, 10);
    CHECK(end && end != argv[3] && !*end);
    reference_run(program, entry, expected);
    source_fixture_owner_free(&owner);
    printf("%s packet/VM independent result=%lld two_instances=2\n", argv[2], (long long)expected);
    return 0;
}
