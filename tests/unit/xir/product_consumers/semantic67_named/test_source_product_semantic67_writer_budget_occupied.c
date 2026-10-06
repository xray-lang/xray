/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_semantic67_writer_budget_occupied.c - Public writer budgets and retained packets
 *
 * KEY CONCEPT:
 *   Whole-graph charges belong to one finite ledger, including retained output prefixes.
 */
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"

_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u,
    "Public writer budgets require the frozen packet contract");
static const uint8_t writer_budget_golden[] = {
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
_Static_assert(sizeof(writer_budget_golden) == 224, "Complete independently framed Checked packet");

typedef enum WriterMode { WRITER_EMPTY_NORMAL, WRITER_OCCUPIED_NORMAL,
    WRITER_AXIS_EXACT, WRITER_AXIS_MINUS1 } WriterMode;
typedef enum WriterPhase { WRITER_OWNER, WRITER_CHECK, WRITER_WRITE1, WRITER_WRITE2 } WriterPhase;
typedef struct WriterSample {
    XrCompileResourceStats ledger;
    size_t sites, blocks, bytes, peak_blocks, peak_bytes;
    bool valid;
} WriterSample;
typedef struct WriterRun {
    XrXirCompileContext context;
    XrXirArtifact *checked;
    XrXirCheckedPacket output, prefix;
    XrXirDiagnostic diagnostic;
    XrCompileResourceLimits caps;
    XrCompileResourceStatus owner_status;
    XrXirStatus check_status, write1_status, write2_status;
    WriterSample constructor, operation, before_second;
    unsigned char original[sizeof(XrXirCheckedPacket)], occupied[sizeof(XrXirCheckedPacket)];
    uint64_t owner_live;
    WriterMode mode;
    WriterPhase phase;
    unsigned packet_frees;
    bool owner_baseline, accounting_ok, output_preserved, prefix_ready;
    bool bytes_before, bytes_after, distinct, cleanup_ok;
} WriterRun;

static const char *writer_phase_name(WriterPhase phase) {
    static const char *const names[] = {"owner_new", "check", "write1", "write2"};
    return names[(unsigned)phase];
}

static WriterSample writer_sample(WriterRun *run) {
    WriterSample sample = {0};
    if (run->context.resources) {
        sample.valid = xr_compile_resources_stats(run->context.resources, &sample.ledger) == XR_COMPILE_RESOURCE_OK;
        if (!sample.valid || sample.ledger.allocated_bytes > run->caps.allocated_bytes ||
            sample.ledger.peak_bytes > run->caps.live_bytes || sample.ledger.work > run->caps.work)
            run->accounting_ok = false;
    }
    sample.sites = source_program_compile_attempts;
    sample.blocks = source_program_compile_live;
    sample.bytes = source_program_compile_bytes;
    sample.peak_blocks = source_program_compile_peak_live;
    sample.peak_bytes = source_program_compile_peak_bytes;
    return sample;
}

static void writer_print_sample(const char *phase, const WriterSample *before, const WriterSample *after) {
    printf("writer-budget-phase phase=%s site_begin=%zu site_end=%zu sites=%zu valid=%u count=%llu allocated=%llu live=%llu peak=%llu work=%llu blocks=%zu bytes=%zu physical_peak_blocks=%zu physical_peak_bytes=%zu\n",
        phase, before->sites, after->sites, after->sites - before->sites, (unsigned)after->valid,
        (unsigned long long)after->ledger.allocation_count, (unsigned long long)after->ledger.allocated_bytes,
        (unsigned long long)after->ledger.live_bytes, (unsigned long long)after->ledger.peak_bytes,
        (unsigned long long)after->ledger.work, after->blocks, after->bytes,
        after->peak_blocks, after->peak_bytes);
}

static XrXirStatus writer_checked(const XrXirCompileContext *context, XrXirArtifact **output,
                                XrXirDiagnostic *diagnostic) {
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
    XrXirStatus status = xr_xir_compile_check(context, &built, output, diagnostic);
    if (status != XR_XIR_OK) return status;
    if (!*output) return XR_XIR_BAD_STRUCTURE;
    memset(name, 0xcc, sizeof(name));
    const XrXirModule *module = xr_xir_compile_artifact_module(*output);
    const XrXirCompileContext *owned = xr_xir_compile_artifact_context(*output);
    if (!module || module->stage != XR_XIR_CHECKED || module->function_count != 1 || !module->functions ||
        !module->functions[0].name || module->functions[0].name == name ||
        module->functions[0].name_length != 4 || memcmp(module->functions[0].name, "main", 4) ||
        !owned || owned->resources != context->resources) return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}

static bool writer_packet_matches(const XrXirCheckedPacket *packet) {
    return packet->bytes && packet->length == sizeof(writer_budget_golden) &&
        !memcmp(packet->bytes, writer_budget_golden, sizeof(writer_budget_golden));
}

static void writer_packets_free(WriterRun *run) {
    if (run->prefix.bytes && run->prefix.bytes == run->output.bytes)
        run->prefix = (XrXirCheckedPacket) {0};
    if (run->output.bytes) ++run->packet_frees;
    xr_xir_compile_checked_packet_free(&run->output);
    if (run->prefix.bytes) ++run->packet_frees;
    xr_xir_compile_checked_packet_free(&run->prefix);
}

static void writer_cleanup(WriterRun *run) {
    WriterSample before = writer_sample(run);
    xr_xir_compile_artifact_free(run->checked);
    run->checked = NULL;
    WriterSample after = writer_sample(run);
    writer_print_sample("checked_drop", &before, &after);
    run->bytes_after = writer_packet_matches(&run->output);
    if (run->prefix_ready) run->bytes_after = run->bytes_after && writer_packet_matches(&run->prefix);
    before = after;
    writer_packets_free(run);
    after = writer_sample(run);
    writer_print_sample("packets_drop", &before, &after);
    bool baseline = !run->context.resources ||
        (after.valid && run->owner_baseline && after.ledger.live_bytes == run->owner_live);
    if (run->context.resources) {
        xr_compile_resources_release(run->context.resources);
        run->context.resources = NULL;
    }
    after = writer_sample(run);
    printf("writer-budget-release ledger=destroyed site_end=%zu blocks=%zu bytes=%zu packet_frees=%u\n",
        after.sites, after.blocks, after.bytes, run->packet_frees);
    bool physical = !after.blocks && !after.bytes;
    xr_free(source_program_compile_allocations);
    source_program_compile_allocations = NULL;
    source_program_compile_capacity = 0;
    run->cleanup_ok = baseline && physical && !source_program_compile_allocations &&
        !run->output.bytes && !run->output.length && !run->prefix.bytes && !run->prefix.length;
}

static bool writer_create_checked(WriterRun *run) {
    WriterSample before = writer_sample(run);
    run->phase = WRITER_OWNER;
    run->owner_status = xr_compile_resources_new(&run->caps, &run->context.resources);
    run->constructor = writer_sample(run);
    run->operation = run->constructor;
    writer_print_sample("owner_new", &before, &run->constructor);
    if (run->owner_status != XR_COMPILE_RESOURCE_OK || !run->context.resources || !run->constructor.valid)
        return false;
    run->owner_live = run->constructor.ledger.live_bytes;
    run->owner_baseline = true;
    run->context.limits = xr_xir_compile_default_limits();
    before = run->constructor;
    run->phase = WRITER_CHECK;
    run->check_status = writer_checked(&run->context, &run->checked, &run->diagnostic);
    WriterSample after = writer_sample(run);
    run->operation = after;
    writer_print_sample("check", &before, &after);
    return run->check_status == XR_XIR_OK && run->checked;
}

static void writer_write(WriterRun *run) {
    WriterSample before = writer_sample(run);
    run->phase = WRITER_WRITE1;
    run->write1_status = xr_xir_compile_checked_write(run->checked, &run->output, &run->diagnostic);
    run->operation = writer_sample(run);
    writer_print_sample("write1", &before, &run->operation);
    if (run->write1_status != XR_XIR_OK) {
        run->output_preserved = !memcmp(run->original, &run->output, sizeof(run->original));
        return;
    }
    run->bytes_before = writer_packet_matches(&run->output);
    if (!run->bytes_before || run->mode != WRITER_OCCUPIED_NORMAL) return;
    memcpy(&run->prefix, &run->output, sizeof(run->prefix));
    memcpy(run->occupied, &run->output, sizeof(run->occupied));
    run->prefix_ready = true;
    run->before_second = run->operation;
    run->phase = WRITER_WRITE2;
    run->write2_status = xr_xir_compile_checked_write(run->checked, &run->output, &run->diagnostic);
    run->operation = writer_sample(run);
    writer_print_sample("write2", &run->before_second, &run->operation);
    if (run->write2_status != XR_XIR_OK) {
        run->output_preserved = !memcmp(run->occupied, &run->output, sizeof(run->occupied)) &&
            writer_packet_matches(&run->prefix);
        return;
    }
    run->distinct = run->output.bytes != run->prefix.bytes;
    run->bytes_before = run->distinct && writer_packet_matches(&run->output) && writer_packet_matches(&run->prefix);
}

static bool writer_expected(const WriterRun *run) {
    if (!run->cleanup_ok || !run->accounting_ok || source_program_compile_injected) return false;
    if (run->mode == WRITER_AXIS_MINUS1) {
        bool budget = run->phase == WRITER_OWNER ? run->owner_status == XR_COMPILE_RESOURCE_BUDGET :
            run->phase == WRITER_CHECK ? run->check_status == XR_XIR_BUDGET : run->write1_status == XR_XIR_BUDGET;
        return budget && run->output_preserved;
    }
    if (run->owner_status != XR_COMPILE_RESOURCE_OK || run->check_status != XR_XIR_OK ||
        run->write1_status != XR_XIR_OK || !run->bytes_before || !run->bytes_after || !run->operation.valid)
        return false;
    if (run->mode == WRITER_OCCUPIED_NORMAL)
        return run->phase == WRITER_WRITE2 && run->write2_status == XR_XIR_OK &&
            run->prefix_ready && run->distinct && run->packet_frees == 2 &&
            run->before_second.sites > 0 && run->operation.sites > run->before_second.sites;
    return run->operation.sites == 10 && run->operation.ledger.allocated_bytes == 979 &&
        run->operation.ledger.peak_bytes == 748 && run->operation.ledger.work == 1271 && run->packet_frees == 1;
}

static int writer_run(WriterMode mode, const XrCompileResourceLimits *caps) {
    WriterRun run = {0};
    run.mode = mode;
    run.caps = *caps;
    run.accounting_ok = true;
    run.output_preserved = false;
    run.owner_status = XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    run.check_status = run.write1_status = run.write2_status = XR_XIR_BAD_STRUCTURE;
    memset(&run.output, 0xa5, sizeof(run.output));
    run.output.bytes = NULL;
    run.output.length = 0;
    memcpy(run.original, &run.output, sizeof(run.original));
    if (source_program_compile_attempts || source_program_compile_live || source_program_compile_bytes ||
        source_program_compile_allocations || source_program_owner_count || source_program_compile_injected) {
        run.accounting_ok = false;
    } else if (writer_create_checked(&run)) {
        writer_write(&run);
    }
    if (run.phase != WRITER_WRITE1 && run.phase != WRITER_WRITE2)
        run.output_preserved = !memcmp(run.original, &run.output, sizeof(run.original));
    printf("writer-budget-outcome mode=%u phase=%s owner_status=%d check_status=%d write1_status=%d write2_status=%d diag=%d function=%u block=%u instruction=%u reason=%d unchanged=%u\n",
        (unsigned)mode, writer_phase_name(run.phase), (int)run.owner_status, (int)run.check_status,
        (int)run.write1_status, (int)run.write2_status, (int)run.diagnostic.status,
        run.diagnostic.function, run.diagnostic.block, run.diagnostic.instruction, (int)run.diagnostic.reason,
        (unsigned)run.output_preserved);
    printf("writer-budget-summary scope=%s valid=%u sites=%zu allocated=%llu peak=%llu work=%llu write2_begin=%zu write2_end=%zu\n",
        mode == WRITER_OCCUPIED_NORMAL ? "cold_owner_check_prefix_write2" : "cold_owner_check_write",
        (unsigned)run.operation.valid, run.operation.sites, (unsigned long long)run.operation.ledger.allocated_bytes,
        (unsigned long long)run.operation.ledger.peak_bytes, (unsigned long long)run.operation.ledger.work,
        run.before_second.sites, mode == WRITER_OCCUPIED_NORMAL ? run.operation.sites : 0);
    writer_cleanup(&run);
    bool passed = writer_expected(&run);
    printf("writer-budget-final result=%s bytes_before=%u bytes_after=%u distinct=%u cleanup=%u physical=%zu/%zu\n",
        passed ? "PASS" : "FAIL", (unsigned)run.bytes_before, (unsigned)run.bytes_after,
        (unsigned)run.distinct, (unsigned)run.cleanup_ok, source_program_compile_live, source_program_compile_bytes);
    puts("Public writer graph only; original1932/12axes/FI6 and occupied fault/resource replay remain OPEN");
    return passed ? 0 : 1;
}

static bool writer_axis_caps(const char *axis, const char *cut, WriterMode *mode, XrCompileResourceLimits *caps) {
    uint64_t below;
    if (!strcmp(cut, "exact")) { *mode = WRITER_AXIS_EXACT; below = 0; }
    else if (!strcmp(cut, "minus1")) { *mode = WRITER_AXIS_MINUS1; below = 1; }
    else return false;
    if (!strcmp(axis, "allocated")) caps->allocated_bytes = UINT64_C(979) - below;
    else if (!strcmp(axis, "live")) caps->live_bytes = UINT64_C(748) - below;
    else if (!strcmp(axis, "work")) caps->work = UINT64_C(1271) - below;
    else return false;
    return true;
}

int main(int argc, char **argv) {
    XrCompileResourceLimits caps = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    WriterMode mode = WRITER_EMPTY_NORMAL;
    if (argc == 1) return writer_run(WRITER_OCCUPIED_NORMAL, &caps);
    if (argc == 2 && !strcmp(argv[1], "--normal-empty")) return writer_run(mode, &caps);
    if (argc == 4 && !strcmp(argv[1], "--axis") && writer_axis_caps(argv[2], argv[3], &mode, &caps))
        return writer_run(mode, &caps);
    fputs("Usage: writer-budget-occupied [--normal-empty | --axis allocated|live|work exact|minus1]\n", stderr);
    return 2;
}
