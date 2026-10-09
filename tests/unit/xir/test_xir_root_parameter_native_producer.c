/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_parameter_native_producer.c - Real Source to portable C with closed high-order proof
 *
 * KEY CONCEPT:
 *   Source, packet and artifact owners die before generated C is consumed.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_output.h"
#include "toolchain/xcompiler_session.h"
#include "os/os_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "H1 native producer %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_root_parameter_oracles.h"

static void h1n_compiler_report(const RootParameterSourceOracle *oracle, const char *phase,
                               const XrXirCompileContext *context, size_t begin) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context->resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(instance_compile_attempts >= begin);
    printf("H1_NATIVE_COMPILER file=%s phase=%s status=0 attempt_begin=%zu attempt_end=%zu sites=%zu allocated=%llu live=%llu peak=%llu work=%llu "
        "physical_blocks=%zu physical_bytes=%zu\n", oracle->file, phase, begin, instance_compile_attempts, instance_compile_attempts - begin,
        (unsigned long long)stats.allocated_bytes, (unsigned long long)stats.live_bytes,
        (unsigned long long)stats.peak_bytes, (unsigned long long)stats.work,
        instance_compile_live, instance_compile_bytes);
}

static void h1n_ok(XrXirStatus status, const RootParameterSourceOracle *oracle, const char *phase) {
    if (status != XR_XIR_OK)
        fprintf(stderr, "H1 native producer compiler file=%s phase=%s status=%u\n", oracle->file, phase, (unsigned)status);
    CHECK(status == XR_XIR_OK);
}

static uint32_t h1n_export(const XrXirModule *module) {
    CHECK(module->declarations && module->declarations->functions);
    uint32_t found = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        const XrXirFunctionIdentity *id = &module->declarations->functions[f];
        if (fn->name_length != 3 || memcmp(fn->name, "run", 3) ||
            id->module != module->declarations->root_module || id->nominal_owner) continue;
        CHECK(found == UINT32_MAX && id->exported && !fn->parameter_count && fn->result == XR_XIR_I64);
        found = f;
    }
    CHECK(found != UINT32_MAX); return found;
}

static void h1n_facts(XrXirArtifact *artifact, const RootParameterSourceOracle *oracle, uint32_t run) {
    XrXirEffects *effects = NULL;
    h1n_ok(xr_xir_compile_effects_analyze(artifact, &effects), oracle, "closed_effects");
    const XrXirRootEffects *facts = xr_xir_effects_root(effects, run);
    CHECK(facts && facts->requires_root == oracle->root && facts->unresolved == oracle->unresolved);
    xr_xir_compile_effects_free(effects);
}

static void h1n_emit(const RootParameterSourceOracle *oracle, unsigned index, const char *output, FILE *header) {
    instance_compile_zero();
    CHECK(instance_compile_fail_at == SIZE_MAX);
    XrXirCompileContext context = {0};
    const XrCompileResourceLimits caps = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    size_t begin = instance_compile_attempts;
    CHECK(xr_compile_resources_new(&caps, &context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    char path[1024]; int n = snprintf(path, sizeof(path), "%s/%s", XR_ROOT_PARAMETER_NATIVE_FIXTURES, oracle->file);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    XrOsIoPolicy io = xr_compile_io_policy(context.resources);
    bool exists = false; XrOsIoStatus present = xr_os_io_exists(&io, path, &exists);
    CHECK(present == XR_OS_IO_OK || present == XR_OS_IO_NOT_FOUND);
    if (exists) CHECK(xr_os_io_remove(&io, path) == XR_OS_IO_OK);
    CHECK(xr_os_io_write_new_file_sync(&io, path, (const uint8_t *)oracle->text,
        strlen(oracle->text)) == XR_OS_IO_OK);
    h1n_compiler_report(oracle, "owner_source", &context, begin);
    XrCompilerSession *session = NULL;
    begin = instance_compile_attempts;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_ROOT_PARAMETER_NATIVE_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, &context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult source = {0}; XrXirSourceDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request, &source, &diagnostic, &failure);
    if (status != XR_XIR_OK)
        fprintf(stderr, "H1 native producer Source file=%s status=%u line=%d column=%d message=%s\n",
            oracle->file, (unsigned)status, diagnostic.line, diagnostic.column, diagnostic.message);
    h1n_ok(status, oracle, "Source"); CHECK(source.checked && source.snapshot && !failure);
    XrXirArtifact *checked = source.checked; source.checked = NULL;
    xr_xir_compile_source_result_free(&source); xr_compile_session_free(session); session = NULL;
    CHECK(xr_os_io_remove(&io, path) == XR_OS_IO_OK);
    h1n_compiler_report(oracle, "source_producers_dead", &context, begin);
    XrXirCheckedPacket packet = {0}; begin = instance_compile_attempts;
    h1n_ok(xr_xir_compile_checked_write(checked, &packet, NULL), oracle, "Checked_write");
    xr_xir_compile_artifact_free(checked); checked = NULL;
    h1n_compiler_report(oracle, "Checked_producer_dead", &context, begin);
    XrXirArtifact *read = NULL; begin = instance_compile_attempts;
    h1n_ok(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &read, NULL), oracle, "Checked_read");
    memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    h1n_compiler_report(oracle, "packet_dead", &context, begin);
    XrXirArtifact *closed = NULL; begin = instance_compile_attempts;
    h1n_ok(xr_xir_compile_specialize(read, &closed, NULL), oracle, "specialize");
    xr_xir_compile_artifact_free(read); read = NULL;
    h1n_ok(xr_xir_compile_artifact_verify(closed, NULL), oracle, "closed_reverify");
    const XrXirModule *cm = xr_xir_compile_artifact_module(closed);
    CHECK(cm->stage == XR_XIR_CHECKED && cm->provenance && cm->provenance->kind == XR_XIR_EVIDENCE_INSTANCE);
    uint32_t run = h1n_export(cm); h1n_facts(closed, oracle, run);
    h1n_compiler_report(oracle, "specialize_reverify", &context, begin);
    XrXirArtifact *lowered = NULL; begin = instance_compile_attempts;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    h1n_ok(xr_xir_compile_lower(closed, &target, &lowered, NULL), oracle, "Lower");
    xr_xir_compile_artifact_free(closed); closed = NULL;
    h1n_ok(xr_xir_compile_artifact_verify(lowered, NULL), oracle, "Lowered_reverify");
    const XrXirModule *lm = xr_xir_compile_artifact_module(lowered);
    CHECK(lm->stage == XR_XIR_LOWERED && h1n_export(lm) == run);
    const XrXirDeclarations *d = lm->declarations;
    CHECK(d->module_count == 1 && d->entry_function < lm->function_count && d->entry_function != run);
    CHECK(!lm->functions[d->entry_function].parameter_count && lm->functions[d->entry_function].result == XR_XIR_I64);
    CHECK(d->slot_count == (oracle->root ? 1u : 0u));
    if (d->slot_count) CHECK(d->slots[0].module == d->root_module && d->slots[0].mutable && d->slots[0].type == XR_XIR_I64);
    char symbol[64];
    CHECK(snprintf(symbol, sizeof(symbol), "h1native_%u", index) > 0);
    CHECK(fprintf(header, "XR_DATA const XrXirProgramSpec %s_program;\n", symbol) > 0);
    CHECK(fprintf(header, "static const uint32_t h1native_%u_run = %uu;\n", index, run) > 0);
    XrXirCSource generated = {0};
    h1n_ok(xr_xir_compile_emit_c(lowered, symbol, 4194304, &generated), oracle, "CGen_W1_W4");
    CHECK(generated.text && generated.length && generated.text[generated.length] == 0);
    CHECK(!strstr(generated.text, "({"));
    xr_xir_compile_artifact_free(lowered); lowered = NULL;
    FILE *file = fopen(output, "wb");
    CHECK(file && fwrite(generated.text, 1, generated.length, file) == generated.length && !fclose(file));
    printf("H1_NATIVE_GENERATED file=%s checkedRoundtrip=1 closedReverify=1 W1W4=1 bytes=%zu finalNul=1\n", oracle->file, generated.length);
    xr_xir_compile_c_source_free(&generated);
    XrCompileResourceStats final;
    CHECK(xr_compile_resources_stats(context.resources, &final) == XR_COMPILE_RESOURCE_OK);
    CHECK(final.live_bytes == sizeof(XrCompileResources));
    xr_compile_resources_release(context.resources);
    instance_compile_zero();
}

int main(int argc, char **argv) {
    CHECK(argc == 6);
    FILE *header = fopen(argv[5], "wb");
    CHECK(header);
    for (unsigned i = 0; i < 4; ++i) h1n_emit(&rps_oracles[i], i, argv[i+1], header);
    CHECK(!fclose(header));
    instance_compile_zero();
    return 0;
}
