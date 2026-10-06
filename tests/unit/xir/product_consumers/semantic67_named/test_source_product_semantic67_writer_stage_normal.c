/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_semantic67_writer_stage_normal.c - Public writer charging observations
 *
 * KEY CONCEPT:
 *   Stage deltas and cumulative peaks use one finite owner without resource resets.
 */
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"

_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u,
    "Public writer observations require the frozen current packet contract");

/* Exact legal vector from semantic67_public_writer_goldens.h:writer_named67_0. */
static const uint8_t writer_normal_golden[] = {
    0x58,0x52,0x43,0x48,0x4b,0x00,0x00,0x00,0x19,0x00,0x00,0x00,
    0x43,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xa0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x5d,0xc7,0x65,0x68,
    0x80,0xca,0xf9,0xc2,0xbb,0xe1,0x77,0xea,0x5b,0xc0,0x4b,0x20,
    0xe6,0x13,0xac,0xf6,0x73,0xcd,0x89,0x7b,0xe0,0x15,0x43,0xfb,
    0x90,0x4c,0xf2,0x16,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x6d,0x61,0x69,0x6e,
    0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x2a,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x21,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
_Static_assert(sizeof(writer_normal_golden) == 224, "Complete legal Checked packet");

typedef struct WriterObservation {
    XrCompileResourceStats ledger;
    size_t attempts, physical_blocks, physical_bytes, physical_peak_blocks, physical_peak_bytes;
} WriterObservation;

static WriterObservation writer_observe(const XrXirCompileContext *context) {
    WriterObservation result = {0};
    if (context) {
        CHECK(context->resources);
        CHECK(xr_compile_resources_stats(context->resources, &result.ledger) == XR_COMPILE_RESOURCE_OK);
    }
    result.attempts = source_program_compile_attempts;
    result.physical_blocks = source_program_compile_live;
    result.physical_bytes = source_program_compile_bytes;
    result.physical_peak_blocks = source_program_compile_peak_live;
    result.physical_peak_bytes = source_program_compile_peak_bytes;
    return result;
}

static void writer_stage(const char *phase, const WriterObservation *before,
                         const WriterObservation *after) {
    CHECK(after->attempts >= before->attempts);
    CHECK(after->ledger.allocation_count >= before->ledger.allocation_count);
    CHECK(after->ledger.allocated_bytes >= before->ledger.allocated_bytes);
    CHECK(after->ledger.work >= before->ledger.work);
    printf("writer-normal phase=%s site_begin=%zu site_end=%zu sites=%zu\n", phase,
        before->attempts, after->attempts, after->attempts - before->attempts);
    printf("writer-charge phase=%s count=%llu allocated=%llu work=%llu\n", phase,
        (unsigned long long)(after->ledger.allocation_count - before->ledger.allocation_count),
        (unsigned long long)(after->ledger.allocated_bytes - before->ledger.allocated_bytes),
        (unsigned long long)(after->ledger.work - before->ledger.work));
    printf("writer-ledger phase=%s allocated_total=%llu live_total=%llu peak_total=%llu work_total=%llu\n",
        phase, (unsigned long long)after->ledger.allocated_bytes,
        (unsigned long long)after->ledger.live_bytes, (unsigned long long)after->ledger.peak_bytes,
        (unsigned long long)after->ledger.work);
    printf("writer-physical phase=%s blocks=%zu bytes=%zu peak_blocks=%zu peak_bytes=%zu\n",
        phase, after->physical_blocks, after->physical_bytes,
        after->physical_peak_blocks, after->physical_peak_bytes);
}

static XrXirArtifact *writer_check(const XrXirCompileContext *context) {
    char name[] = {'m', 'a', 'i', 'n'};
    const XrXirInstruction operations[] = {
        {.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 42},
        {.op = XR_XIR_RETURN, .type = XR_XIR_UNIT}
    };
    const XrXirBlock block = {.count = 2};
    const XrXirFunction function = {.name = name, .name_length = 4, .result = XR_XIR_I64,
        .blocks = &block, .block_count = 1, .instructions = operations, .instruction_count = 2};
    const XrXirModule built = {.stage = XR_XIR_BUILT, .functions = &function,
        .function_count = 1, .linkage_kind = XR_XIR_PROGRAM};
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_compile_check(context, &built, &checked, NULL) == XR_XIR_OK && checked);
    memset(name, 0xcc, sizeof(name));
    const XrXirModule *owned = xr_xir_compile_artifact_module(checked);
    CHECK(owned && owned->stage == XR_XIR_CHECKED && owned->function_count == 1);
    CHECK(owned->functions[0].name != name && owned->functions[0].name_length == 4);
    CHECK(!memcmp(owned->functions[0].name, "main", 4));
    return checked;
}

static int writer_normal(void) {
    CHECK(!source_program_compile_attempts && !source_program_compile_live && !source_program_compile_bytes);
    WriterObservation before = writer_observe(NULL);
    const XrXirCompileContext *context = source_program_owner(UINT64_C(67108864), UINT64_C(128000000));
    WriterObservation after = writer_observe(context);
    writer_stage("owner_new", &before, &after);
    CHECK(after.attempts > before.attempts);
    const uint64_t owner_live = after.ledger.live_bytes;
    before = after;
    XrXirArtifact *checked = writer_check(context);
    after = writer_observe(context);
    writer_stage("built_check", &before, &after);
    CHECK(after.attempts > before.attempts);
    before = after;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    after = writer_observe(context);
    writer_stage("checked_write", &before, &after);
    CHECK(after.attempts > before.attempts && packet.bytes && packet.length == sizeof(writer_normal_golden));
    CHECK(!memcmp(packet.bytes, writer_normal_golden, packet.length));
    const WriterObservation operation = after;
    before = after;
    xr_xir_compile_artifact_free(checked);
    checked = NULL;
    after = writer_observe(context);
    writer_stage("checked_drop", &before, &after);
    CHECK(!memcmp(packet.bytes, writer_normal_golden, packet.length));
    before = after;
    xr_xir_compile_checked_packet_free(&packet);
    after = writer_observe(context);
    writer_stage("packet_drop", &before, &after);
    CHECK(!packet.bytes && !packet.length && after.ledger.live_bytes == owner_live);
    const size_t release_begin = after.attempts;
    source_program_owners_free();
    after = writer_observe(NULL);
    CHECK(!after.physical_blocks && !after.physical_bytes && !source_program_compile_allocations);
    CHECK(after.attempts >= release_begin);
    printf("writer-release phase=owner_drop site_begin=%zu site_end=%zu sites=%zu ledger=destroyed physical=0/0\n",
        release_begin, after.attempts, after.attempts - release_begin);
    printf("writer-normal-summary scope=owner_check_write sites=%zu allocated=%llu peak=%llu work=%llu\n",
        operation.attempts, (unsigned long long)operation.ledger.allocated_bytes,
        (unsigned long long)operation.ledger.peak_bytes, (unsigned long long)operation.ledger.work);
    puts("Public writer normal measurements only; private codec sizing and all FI remain OPEN");
    return 0;
}


typedef struct WriterPacketFault {
    XrXirCompileContext context;
    XrXirArtifact *checked;
    XrXirCheckedPacket packet;
    XrXirDiagnostic diagnostic;
    XrCompileResourceStats charged;
    XrCompileResourceStatus owner_status, stats_status;
    XrXirStatus check_status, write_status;
    uint64_t owner_live;
    size_t normal_sites, write_begin, write_end;
    bool hit, unchanged, owner_baseline_known;
} WriterPacketFault;

/* Status returns leave every published allocation available to the caller cleanup. */
static XrXirStatus writer_fault_check(const XrXirCompileContext *context,
                                    XrXirArtifact **output) {
    char name[] = {'m', 'a', 'i', 'n'};
    const XrXirInstruction operations[] = {
        {.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 42},
        {.op = XR_XIR_RETURN, .type = XR_XIR_UNIT}
    };
    const XrXirBlock block = {.count = 2};
    const XrXirFunction function = {.name = name, .name_length = 4, .result = XR_XIR_I64,
        .blocks = &block, .block_count = 1, .instructions = operations, .instruction_count = 2};
    const XrXirModule built = {.stage = XR_XIR_BUILT, .functions = &function,
        .function_count = 1, .linkage_kind = XR_XIR_PROGRAM};
    XrXirStatus status = xr_xir_compile_check(context, &built, output, NULL);
    if (status != XR_XIR_OK || !*output) return status;
    memset(name, 0xcc, sizeof(name));
    const XrXirModule *owned = xr_xir_compile_artifact_module(*output);
    const XrXirCompileContext *owner = xr_xir_compile_artifact_context(*output);
    if (!owned || owned->stage != XR_XIR_CHECKED || owned->function_count != 1 ||
        !owned->functions || !owned->functions[0].name || owned->functions[0].name == name ||
        owned->functions[0].name_length != 4 || memcmp(owned->functions[0].name, "main", 4) ||
        !owner || owner->resources != context->resources) return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}

static bool writer_fault_cleanup(WriterPacketFault *fault) {
    bool baseline_ok = !fault->context.resources;
    xr_xir_compile_artifact_free(fault->checked);
    fault->checked = NULL;
    xr_xir_compile_checked_packet_free(&fault->packet);
    if (fault->context.resources) {
        XrCompileResourceStats final = {0};
        XrCompileResourceStatus status = xr_compile_resources_stats(fault->context.resources, &final);
        baseline_ok = status == XR_COMPILE_RESOURCE_OK && fault->owner_baseline_known &&
            final.live_bytes == fault->owner_live;
        if (status == XR_COMPILE_RESOURCE_OK)
            printf("writer-packet-oom-cleanup live=%llu owner_live=%llu allocated=%llu peak=%llu work=%llu\n",
                (unsigned long long)final.live_bytes, (unsigned long long)fault->owner_live,
                (unsigned long long)final.allocated_bytes, (unsigned long long)final.peak_bytes,
                (unsigned long long)final.work);
        xr_compile_resources_release(fault->context.resources);
        fault->context.resources = NULL;
    }
    bool physical_ok = !source_program_compile_live && !source_program_compile_bytes;
    printf("writer-packet-oom-release ledger=destroyed blocks=%zu bytes=%zu peak_blocks=%zu peak_bytes=%zu\n",
        source_program_compile_live, source_program_compile_bytes,
        source_program_compile_peak_live, source_program_compile_peak_bytes);
    /* Observer storage is outside the compiler fault domain and has its own actual free. */
    xr_free(source_program_compile_allocations);
    source_program_compile_allocations = NULL;
    source_program_compile_capacity = 0;
    return baseline_ok && physical_ok && !source_program_compile_allocations;
}

static bool writer_normal_site_argument(const char *text, size_t *output) {
    size_t value = 0;
    if (!text || !*text) return false;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        size_t digit = (size_t)(*p - '0');
        if (value > (SIZE_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (!value || value == SIZE_MAX) return false;
    *output = value;
    return true;
}

static int writer_packet_oom(size_t normal_sites) {
    WriterPacketFault fault = {0};
    const XrCompileResourceLimits caps = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    unsigned char original[sizeof(fault.packet)];
    bool passed = false;
    fault.normal_sites = normal_sites;
    fault.owner_status = XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    fault.stats_status = XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    fault.check_status = XR_XIR_BAD_STRUCTURE;
    fault.write_status = XR_XIR_BAD_STRUCTURE;
    fault.diagnostic = (XrXirDiagnostic) {XR_XIR_BAD_STRUCTURE, 0, 0, 0, XR_XIR_DIAGNOSTIC_NONE};
    memset(&fault.packet, 0xa5, sizeof(fault.packet));
    fault.packet.bytes = NULL;
    fault.packet.length = 0;
    memcpy(original, &fault.packet, sizeof(original));
    if (source_program_compile_attempts || source_program_compile_live || source_program_compile_bytes ||
        source_program_compile_allocations || source_program_owner_count || source_program_compile_injected)
        goto cleanup;
    fault.owner_status = xr_compile_resources_new(&caps, &fault.context.resources);
    if (fault.owner_status != XR_COMPILE_RESOURCE_OK || !fault.context.resources) goto cleanup;
    fault.context.limits = xr_xir_compile_default_limits();
    fault.stats_status = xr_compile_resources_stats(fault.context.resources, &fault.charged);
    if (fault.stats_status != XR_COMPILE_RESOURCE_OK) goto cleanup;
    fault.owner_live = fault.charged.live_bytes;
    fault.owner_baseline_known = true;
    fault.check_status = writer_fault_check(&fault.context, &fault.checked);
    if (fault.check_status != XR_XIR_OK || !fault.checked) goto cleanup;
    fault.write_begin = source_program_compile_attempts;
    if (normal_sites - 1 < fault.write_begin) goto cleanup;
    source_program_compile_fail_at = normal_sites - 1;
    fault.write_status = xr_xir_compile_checked_write(fault.checked, &fault.packet, &fault.diagnostic);
    fault.write_end = source_program_compile_attempts;
    fault.hit = source_program_compile_injected;
    source_program_compile_fail_at = SIZE_MAX;
    fault.unchanged = !memcmp(original, &fault.packet, sizeof(original));
    fault.stats_status = xr_compile_resources_stats(fault.context.resources, &fault.charged);
    passed = fault.write_status == XR_XIR_OUT_OF_MEMORY && fault.hit && fault.write_end == normal_sites &&
        fault.unchanged && !fault.packet.bytes && !fault.packet.length &&
        fault.stats_status == XR_COMPILE_RESOURCE_OK && fault.diagnostic.status == XR_XIR_OUT_OF_MEMORY &&
        fault.diagnostic.function == UINT32_MAX && fault.diagnostic.block == UINT32_MAX &&
        fault.diagnostic.instruction == UINT32_MAX && fault.diagnostic.reason == XR_XIR_DIAGNOSTIC_NONE;
cleanup:
    source_program_compile_fail_at = SIZE_MAX;
    printf("writer-packet-oom N=%zu site=%zu write_begin=%zu write_end=%zu hit=%u status=%d owner_status=%d check_status=%d unchanged=%u\n",
        normal_sites, normal_sites - 1, fault.write_begin, fault.write_end, (unsigned)fault.hit,
        (int)fault.write_status, (int)fault.owner_status, (int)fault.check_status, (unsigned)fault.unchanged);
    if (!writer_fault_cleanup(&fault)) passed = false;
    if (!passed) {
        fputs("Public packet allocation OOM replay failed after cleanup\n", stderr);
        return 1;
    }
    puts("Public empty packet allocation OOM hit1; all other FI and private codec obligations remain OPEN");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 1) return writer_normal();
    size_t normal_sites = 0;
    if (argc == 3 && !strcmp(argv[1], "--packet-oom-last") &&
        writer_normal_site_argument(argv[2], &normal_sites)) return writer_packet_oom(normal_sites);
    fputs("Usage: writer-stage-normal [--packet-oom-last frozen-normal-N]\n", stderr);
    return 2;
}
