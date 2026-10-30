/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_string_timing.c - Time actual library execution without observers
 *
 * KEY CONCEPT:
 *   Source, independent packet replay, Lowered and the native proof are real.
 *   Copy and allocator observation belongs to the separate ownership fixture.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_tuple.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#if defined(XR_STRING_TIMING_NATIVE)
XR_DATA const XrXirProgramSpec string_timing_program;
#endif
typedef struct TimingProgram {
    SourceFixtureOwner owner;
    XrXirArtifact *lowered;
    const XrXirModule *module;
    XrXirProgram *program;
} TimingProgram;
static void timing_prepare(TimingProgram *run, unsigned mode, const char *emit) {
    source_fixture_owner_new(&run->owner);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(run->owner.context.resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_TIMING_FIXTURES};
    XrXirSourceRequest request = {session, XR_TIMING_FIXTURES "/string_build/root.xr", &authority,
        &run->owner.context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult source = {0}; XrXirSourceDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_source_check(&request, &source, &diagnostic, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(source.checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&source); xr_compile_session_free(session);
    XrXirArtifact *checked = NULL, *specialized = NULL;
    CHECK(xr_xir_compile_checked_read(&run->owner.context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked, &specialized, NULL, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(specialized, &target, NULL, &run->lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(specialized);
    run->module = xr_xir_compile_artifact_module(run->lowered);
    if (emit) {
        XrXirCSource output = {0};
        CHECK(xr_xir_compile_emit_c(run->lowered, "string_timing", 1048576, &output) == XR_XIR_OK);
        FILE *file = fopen(emit, "wb"); CHECK(file);
        CHECK(fwrite(output.text, 1, output.length, file) == output.length && fclose(file) == 0);
        xr_xir_compile_c_source_free(&output); return;
    }
    if (!mode) CHECK(xr_xir_compile_vm_program_take(&run->lowered, &run->program) == XR_XIR_OK);
    else {
#if defined(XR_STRING_TIMING_NATIVE)
        XrXirProgramProof proof = xr_xir_compile_program_proof(run->lowered);
        CHECK(proof.length == string_timing_program.proof.length &&
            !memcmp(proof.identity, string_timing_program.proof.identity, 32) &&
            !memcmp(proof.bytes, string_timing_program.proof.bytes, proof.length));
        CHECK(xr_xir_compile_program_seal(&run->owner.context, &string_timing_program, &run->program) == XR_XIR_OK);
#else
        CHECK(false);
#endif
    }
}
static double timing_wall(void) {
    struct timespec value; CHECK(timespec_get(&value, TIME_UTC) == TIME_UTC);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}
static XrXirCallStatus timing_poll(XrXirInstance *instance) {
    XrXirCallStatus status = XR_XIR_CALL_READY;
    while (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
    return status;
}
static void timing_text(const XrXirValue *value, size_t length, bool digits) {
    const char *bytes = NULL; size_t actual = 0;
    CHECK(value->type == XR_XIR_STRING && xr_xir_string_view(value, &bytes, &actual));
    CHECK(actual == length && bytes[length] == 0);
    for (size_t i = 0; i < length; ++i) CHECK(bytes[i] == (digits ? (char)('0' + i % 10) : 'x'));
}
static void timing_result(const XrXirValue *value, size_t length, bool digits, bool snapshot) {
    if (!snapshot) { timing_text(value, length, digits); return; }
    XrXirValue old = {0}, fresh = {0};
    CHECK(xr_xir_tuple_get(value, 0, &old) == XR_XIR_VALUE_OK && xr_xir_tuple_get(value, 1, &fresh) == XR_XIR_VALUE_OK);
    timing_text(&old, 4096, false); timing_text(&fresh, length, false);
    xr_xir_value_drop(&fresh); xr_xir_value_drop(&old);
}
static void timing_scenario(TimingProgram *run, unsigned mode, const char *name) {
    bool digits = !strcmp(name, "digits"), chunks = !strcmp(name, "chunks");
    bool join = !strcmp(name, "join"), snapshot = !strcmp(name, "snapshot");
    CHECK(digits || chunks || join || snapshot || !strcmp(name, "short"));
    size_t count = chunks ? 64 : join ? 1000 : snapshot ? 4096 : 1000000;
    size_t piece_length = chunks ? 16384 : join ? 1000 : 1, length = count * piece_length + (snapshot ? 64 : 0);
    uint32_t function = UINT32_MAX;
    for (uint32_t f = 0; f < run->module->function_count; ++f)
        if (run->module->declarations->functions[f].module == run->module->declarations->root_module &&
            run->module->functions[f].name_length == strlen(name) && !memcmp(run->module->functions[f].name, name, strlen(name))) {
            CHECK(function == UINT32_MAX); function = f;
        }
    CHECK(function != UINT32_MAX);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.poll_limit = UINT64_C(64) * count + 4096;
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(run->program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, run->module->declarations->entry_function, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(timing_poll(instance) == XR_XIR_CALL_RETURNED);
    XrXirValue initialized = {0}; CHECK(xr_xir_instance_take_result(instance, &initialized) == XR_XIR_CALL_RETURNED);
    CHECK(initialized.type == XR_XIR_I64 && !initialized.payload); xr_xir_value_drop(&initialized);
    XrXirDomain *input_domain = NULL;
    CHECK(xr_xir_domain_new(65536, &input_domain) == XR_XIR_VALUE_OK);
    char piece[16384]; memset(piece, 'x', piece_length);
    XrXirValue arguments[] = {{XR_XIR_I64, 0, (int64_t)count}, {0}};
    CHECK(xr_xir_string_new(input_domain, piece, piece_length, &arguments[1]) == XR_XIR_VALUE_OK);
    clock_t start = clock(); double wall_start = timing_wall();
    CHECK(xr_xir_instance_start(instance, function, arguments, 2) == XR_XIR_CALL_READY);
    CHECK(timing_poll(instance) == XR_XIR_CALL_RETURNED);
    double wall = timing_wall() - wall_start, cpu = (double)(clock() - start) / CLOCKS_PER_SEC;
    XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    xr_xir_value_drop(&arguments[1]); xr_xir_domain_drop(input_domain);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    xr_xir_compile_artifact_free(run->lowered); run->lowered = NULL;
    xr_xir_compile_program_drop(run->program); run->program = NULL; run->module = NULL;
    timing_result(&value, length, digits, snapshot); xr_xir_value_drop(&value);
    printf("{\"scenario\":\"%s\",\"mode\":%u,\"result_bytes\":%zu,\"wall_seconds\":%.9f,"
        "\"cpu_seconds\":%.9f,\"copy_observer\":false,\"allocator_observer\":false,"
        "\"qualification\":\"TIMING_AND_CONTENT_ONLY\"}\n", name, mode, length, wall, cpu);
}
int main(int argc, char **argv) {
    CHECK(argc == 3); TimingProgram run = {0};
    if (!strcmp(argv[1], "--generate")) timing_prepare(&run, 0, argv[2]);
    else {
        char *end = NULL; unsigned long mode = strtoul(argv[1], &end, 10);
        CHECK(end && !*end && mode <= 1); timing_prepare(&run, (unsigned)mode, NULL);
        timing_scenario(&run, (unsigned)mode, argv[2]);
    }
    xr_xir_compile_artifact_free(run.lowered); xr_xir_compile_program_drop(run.program);
    source_fixture_owner_free(&run.owner); return 0;
}
