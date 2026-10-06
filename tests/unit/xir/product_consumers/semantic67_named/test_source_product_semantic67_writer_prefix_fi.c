/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Public writer allocation prefixes retain one ledger from construction through release.
 */
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"

_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u,
    "Public allocation prefixes require the frozen Checked packet contract");
static const uint8_t prefix_golden[] = {
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
_Static_assert(sizeof(prefix_golden) == 224, "Complete independently framed Checked packet");

typedef enum PrefixPhase { PREFIX_OWNER, PREFIX_CHECK, PREFIX_WRITE1, PREFIX_WRITE2, PREFIX_PHASE_COUNT } PrefixPhase;
typedef struct PrefixSample {
    XrCompileResourceStats ledger;
    size_t sites, blocks, bytes, peak_blocks, peak_bytes;
    bool valid;
} PrefixSample;
typedef struct PrefixRun {
    XrXirCompileContext context;
    XrXirArtifact *checked;
    XrXirCheckedPacket output, prefix;
    XrXirDiagnostic diagnostic;
    XrCompileResourceLimits caps;
    XrCompileResourceStatus owner_status;
    XrXirStatus check_status, write1_status, write2_status;
    PrefixSample before[PREFIX_PHASE_COUNT], after[PREFIX_PHASE_COUNT];
    unsigned char empty[sizeof(XrXirCheckedPacket)], occupied[sizeof(XrXirCheckedPacket)];
    unsigned char checked_original[sizeof(XrXirArtifact *)], owner_original[sizeof(XrCompileResources *)];
    size_t normal_sites, fault_site;
    unsigned packet_frees, axis_mode;
    PrefixPhase phase;
    bool called[PREFIX_PHASE_COUNT];
    bool occupied_graph, fault, armed, hit, accounting_ok, identity_ok, prefix_ready;
    bool bytes_before, bytes_after, distinct, output_preserved, failure_kept, cleanup_ok;
} PrefixRun;

static const char *prefix_graph(const PrefixRun *run) {
    return run->occupied_graph ? "occupied" : "empty";
}
static const char *prefix_phase_name(PrefixPhase phase) {
    static const char *const names[] = {"owner_new", "check", "write1", "write2"};
    return names[(unsigned)phase];
}
static PrefixSample prefix_sample(PrefixRun *run) {
    PrefixSample sample = {0};
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
static void prefix_print_sample(const PrefixRun *run, const char *phase,
                                const PrefixSample *before, const PrefixSample *after) {
    printf("writer-prefix-phase graph=%s phase=%s site_begin=%zu site_end=%zu valid=%u count=%llu allocated=%llu live=%llu peak=%llu work=%llu blocks=%zu bytes=%zu physical_peak_blocks=%zu physical_peak_bytes=%zu\n",
        prefix_graph(run), phase, before->sites, after->sites, (unsigned)after->valid,
        (unsigned long long)after->ledger.allocation_count, (unsigned long long)after->ledger.allocated_bytes,
        (unsigned long long)after->ledger.live_bytes, (unsigned long long)after->ledger.peak_bytes,
        (unsigned long long)after->ledger.work, after->blocks, after->bytes, after->peak_blocks, after->peak_bytes);
}
static void prefix_begin(PrefixRun *run, PrefixPhase phase) {
    run->phase = phase;
    run->before[phase] = prefix_sample(run);
    run->called[phase] = true;
}
static void prefix_end(PrefixRun *run) {
    run->after[run->phase] = prefix_sample(run);
    prefix_print_sample(run, prefix_phase_name(run->phase), &run->before[run->phase], &run->after[run->phase]);
}
static bool prefix_packet_matches(const XrXirCheckedPacket *packet) {
    return packet->bytes && packet->length == sizeof(prefix_golden) &&
        !memcmp(packet->bytes, prefix_golden, sizeof(prefix_golden));
}
static bool prefix_owned_checked(const PrefixRun *run, const char *borrowed_name) {
    if (!run->checked) return false;
    const XrXirModule *module = xr_xir_compile_artifact_module(run->checked);
    const XrXirCompileContext *owned = xr_xir_compile_artifact_context(run->checked);
    return module && module->stage == XR_XIR_CHECKED && module->function_count == 1 && module->functions &&
        module->functions[0].name && module->functions[0].name != borrowed_name &&
        module->functions[0].name_length == 4 && !memcmp(module->functions[0].name, "main", 4) &&
        owned && owned->resources == run->context.resources;
}
static XrXirStatus prefix_check(PrefixRun *run) {
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
    XrXirStatus status = xr_xir_compile_check(&run->context, &built, &run->checked, &run->diagnostic);
    if (status == XR_XIR_OK) {
        memset(name, 0xcc, sizeof(name));
        run->identity_ok = prefix_owned_checked(run, name);
    }
    return status;
}
static void prefix_write(PrefixRun *run, PrefixPhase phase) {
    prefix_begin(run, phase);
    XrXirStatus status = xr_xir_compile_checked_write(run->checked, &run->output, &run->diagnostic);
    if (phase == PREFIX_WRITE1) run->write1_status = status;
    else run->write2_status = status;
    prefix_end(run);
}
static void prefix_operations(PrefixRun *run) {
    prefix_begin(run, PREFIX_OWNER);
    run->owner_status = xr_compile_resources_new(&run->caps, &run->context.resources);
    prefix_end(run);
    if (run->owner_status != XR_COMPILE_RESOURCE_OK || !run->context.resources) return;
    run->context.limits = xr_xir_compile_default_limits();
    prefix_begin(run, PREFIX_CHECK);
    run->check_status = prefix_check(run);
    prefix_end(run);
    if (run->check_status != XR_XIR_OK || !run->identity_ok) return;
    prefix_write(run, PREFIX_WRITE1);
    if (run->write1_status != XR_XIR_OK) return;
    run->bytes_before = prefix_packet_matches(&run->output);
    if (!run->bytes_before || !run->occupied_graph) return;
    memcpy(&run->prefix, &run->output, sizeof(run->prefix));
    memcpy(run->occupied, &run->output, sizeof(run->occupied));
    run->prefix_ready = true;
    prefix_write(run, PREFIX_WRITE2);
    if (run->write2_status != XR_XIR_OK) return;
    run->distinct = run->output.bytes != run->prefix.bytes;
    run->bytes_before = run->distinct && prefix_packet_matches(&run->output) && prefix_packet_matches(&run->prefix);
}
static bool prefix_physical_same(const PrefixSample *before, const PrefixSample *after) {
    return before->blocks == after->blocks && before->bytes == after->bytes &&
        before->valid == after->valid && (!before->valid || before->ledger.live_bytes == after->ledger.live_bytes);
}
static void prefix_failure_snapshot(PrefixRun *run) {
    if (run->phase == PREFIX_WRITE2) {
        run->output_preserved = run->prefix_ready &&
            !memcmp(run->occupied, &run->output, sizeof(run->occupied)) && prefix_packet_matches(&run->output) &&
            prefix_packet_matches(&run->prefix);
    } else {
        run->output_preserved = !memcmp(run->empty, &run->output, sizeof(run->empty));
        if (run->phase == PREFIX_OWNER)
            run->output_preserved = run->output_preserved &&
                !memcmp(run->owner_original, &run->context.resources, sizeof(run->owner_original));
        if (run->phase == PREFIX_OWNER || run->phase == PREFIX_CHECK)
            run->output_preserved = run->output_preserved &&
                !memcmp(run->checked_original, &run->checked, sizeof(run->checked_original));
    }
    run->failure_kept = run->output_preserved &&
        prefix_physical_same(&run->before[run->phase], &run->after[run->phase]);
}
static void prefix_packets_free(PrefixRun *run) {
    if (run->prefix.bytes && run->prefix.bytes == run->output.bytes)
        run->prefix = (XrXirCheckedPacket) {0};
    if (run->output.bytes) ++run->packet_frees;
    xr_xir_compile_checked_packet_free(&run->output);
    if (run->prefix.bytes) ++run->packet_frees;
    xr_xir_compile_checked_packet_free(&run->prefix);
}
static bool prefix_drop_uncharged(const PrefixSample *before, const PrefixSample *after) {
    return before->sites == after->sites && before->valid == after->valid &&
        before->ledger.allocation_count == after->ledger.allocation_count &&
        before->ledger.allocated_bytes == after->ledger.allocated_bytes &&
        before->ledger.work == after->ledger.work && before->ledger.peak_bytes == after->ledger.peak_bytes &&
        after->ledger.live_bytes <= before->ledger.live_bytes && after->blocks <= before->blocks &&
        after->bytes <= before->bytes && before->peak_blocks == after->peak_blocks && before->peak_bytes == after->peak_bytes;
}
static void prefix_cleanup(PrefixRun *run) {
    PrefixSample before = prefix_sample(run);
    xr_xir_compile_artifact_free(run->checked);
    run->checked = NULL;
    PrefixSample after = prefix_sample(run);
    prefix_print_sample(run, "checked_drop", &before, &after);
    bool drops_uncharged = prefix_drop_uncharged(&before, &after);
    run->bytes_after = run->output.bytes ? prefix_packet_matches(&run->output) :
        !memcmp(run->empty, &run->output, sizeof(run->empty));
    if (run->prefix_ready) run->bytes_after = run->bytes_after && prefix_packet_matches(&run->prefix);
    if (run->fault || (run->axis_mode && !(run->axis_mode & 1u))) {
        if (run->phase == PREFIX_WRITE2)
            run->bytes_after = run->bytes_after && !memcmp(run->occupied, &run->output, sizeof(run->occupied)) &&
                !memcmp(run->occupied, &run->prefix, sizeof(run->occupied));
        else run->bytes_after = run->bytes_after && !memcmp(run->empty, &run->output, sizeof(run->empty));
    }
    before = after;
    prefix_packets_free(run);
    after = prefix_sample(run);
    prefix_print_sample(run, "packets_drop", &before, &after);
    drops_uncharged = prefix_drop_uncharged(&before, &after) && drops_uncharged;
    bool baseline = run->context.resources ? after.valid && run->after[PREFIX_OWNER].valid &&
        after.ledger.live_bytes == run->after[PREFIX_OWNER].ledger.live_bytes : !after.blocks && !after.bytes;
    PrefixSample before_owner = after;
    xr_compile_resources_release(run->context.resources);
    run->context.resources = NULL;
    PrefixSample after_owner = prefix_sample(run);
    bool owner_uncharged = before_owner.sites == after_owner.sites &&
        before_owner.peak_blocks == after_owner.peak_blocks && before_owner.peak_bytes == after_owner.peak_bytes;
    xr_free(source_program_compile_allocations);
    source_program_compile_allocations = NULL;
    source_program_compile_capacity = 0;
    run->cleanup_ok = baseline && drops_uncharged && owner_uncharged &&
        after_owner.sites == source_program_compile_attempts && !source_program_compile_live && !source_program_compile_bytes &&
        !run->context.resources && !run->checked && !run->output.bytes && !run->output.length &&
        !run->prefix.bytes && !run->prefix.length && !source_program_compile_allocations && !source_program_owner_count;
    printf("writer-prefix-release graph=%s sites=%zu packet_frees=%u cleanup=%u physical=%zu/%zu owner_site_before=%zu owner_site_after=%zu drops_uncharged=%u owner_uncharged=%u\n",
        prefix_graph(run), source_program_compile_attempts, run->packet_frees, (unsigned)run->cleanup_ok,
        source_program_compile_live, source_program_compile_bytes, before_owner.sites, after_owner.sites,
        (unsigned)drops_uncharged, (unsigned)owner_uncharged);
}
static bool prefix_earlier_stages_ok(const PrefixRun *run) {
    for (unsigned phase = 0; phase < (unsigned)run->phase; ++phase) if (!run->called[phase]) return false;
    for (unsigned phase = (unsigned)run->phase + 1; phase < (unsigned)PREFIX_PHASE_COUNT; ++phase)
        if (run->called[phase]) return false;
    if (run->phase > PREFIX_OWNER && run->owner_status != XR_COMPILE_RESOURCE_OK) return false;
    if (run->phase > PREFIX_CHECK && (run->check_status != XR_XIR_OK || !run->identity_ok)) return false;
    if (run->phase > PREFIX_WRITE1 && (run->write1_status != XR_XIR_OK || !run->bytes_before || !run->prefix_ready)) return false;
    return run->called[run->phase];
}
static bool prefix_fault_expected(const PrefixRun *run) {
    const PrefixSample *before = &run->before[run->phase], *after = &run->after[run->phase];
    if (!run->armed || !run->hit || !source_program_compile_injected || !run->normal_sites ||
        run->fault_site >= run->normal_sites || after->sites != run->fault_site + 1 ||
        run->fault_site < before->sites || run->fault_site >= after->sites ||
        !run->failure_kept || !run->bytes_after || !prefix_earlier_stages_ok(run)) return false;
    if (run->phase == PREFIX_OWNER)
        return run->owner_status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && run->packet_frees == 0;
    if (run->diagnostic.status != XR_XIR_OUT_OF_MEMORY) return false;
    if (run->phase == PREFIX_CHECK)
        return run->check_status == XR_XIR_OUT_OF_MEMORY && run->packet_frees == 0;
    if (run->phase == PREFIX_WRITE1)
        return run->write1_status == XR_XIR_OUT_OF_MEMORY && run->packet_frees == 0;
    return run->occupied_graph && run->write2_status == XR_XIR_OUT_OF_MEMORY && run->packet_frees == 1;
}
static bool prefix_normal_expected(const PrefixRun *run) {
    if (!run->called[PREFIX_OWNER] || !run->called[PREFIX_CHECK] || !run->called[PREFIX_WRITE1] ||
        run->owner_status != XR_COMPILE_RESOURCE_OK || run->check_status != XR_XIR_OK || !run->identity_ok ||
        run->write1_status != XR_XIR_OK || !run->bytes_before || !run->bytes_after ||
        source_program_compile_injected || !run->after[run->phase].valid) return false;
    for (unsigned phase = PREFIX_OWNER; phase <= (unsigned)run->phase; ++phase)
        if (!run->called[phase] || run->before[phase].sites >= run->after[phase].sites ||
            (phase && run->before[phase].sites != run->after[phase - 1].sites)) return false;
    if (!run->occupied_graph)
        return run->phase == PREFIX_WRITE1 && !run->called[PREFIX_WRITE2] && !run->prefix_ready && run->packet_frees == 1;
    return run->phase == PREFIX_WRITE2 && run->called[PREFIX_WRITE2] && run->write2_status == XR_XIR_OK &&
        run->prefix_ready && run->distinct && run->packet_frees == 2;
}
static uint64_t prefix_axis_value(const PrefixRun *run, bool limit) {
    unsigned axis = (run->axis_mode - 1) / 2;
    const XrCompileResourceStats *stats = &run->after[run->phase].ledger;
    if (!axis) return limit ? run->caps.allocated_bytes : stats->allocated_bytes;
    if (axis == 1) return limit ? run->caps.live_bytes : stats->peak_bytes;
    return limit ? run->caps.work : stats->work;
}
static bool prefix_axis_expected(const PrefixRun *run) {
    if (run->axis_mode & 1u)
        return prefix_normal_expected(run) && prefix_axis_value(run, false) == prefix_axis_value(run, true);
    if (run->hit || source_program_compile_injected || !run->failure_kept || !run->bytes_after ||
        !prefix_earlier_stages_ok(run)) return false;
    if (run->phase == PREFIX_OWNER)
        return run->owner_status == XR_COMPILE_RESOURCE_BUDGET && run->packet_frees == 0;
    if (run->diagnostic.status != XR_XIR_BUDGET) return false;
    if (run->phase == PREFIX_CHECK)
        return run->check_status == XR_XIR_BUDGET && run->packet_frees == 0;
    if (run->phase == PREFIX_WRITE1)
        return run->write1_status == XR_XIR_BUDGET && run->packet_frees == 0;
    return run->occupied_graph && run->write2_status == XR_XIR_BUDGET && run->packet_frees == 1;
}
static const char *prefix_mode(const PrefixRun *run) {
    if (run->fault) return "fault";
    if (!run->axis_mode) return "normal";
    return run->axis_mode & 1u ? "budget_exact" : "budget_minus1";
}
static void prefix_report(const PrefixRun *run) {
    const int statuses[] = {(int)run->owner_status, (int)run->check_status, (int)run->write1_status, (int)run->write2_status};
    for (unsigned phase = 0; phase < (unsigned)PREFIX_PHASE_COUNT; ++phase) {
        if (run->called[phase]) printf("writer-prefix-stage phase=%s called=1 status=%d\n",
            prefix_phase_name((PrefixPhase)phase), statuses[phase]);
        else printf("writer-prefix-stage phase=%s called=0 status=NOT_ENTERED\n", prefix_phase_name((PrefixPhase)phase));
    }
    if (run->called[PREFIX_CHECK]) printf("writer-prefix-diagnostic phase=%s status=%d function=%u block=%u instruction=%u reason=%d\n",
        prefix_phase_name(run->phase), (int)run->diagnostic.status, run->diagnostic.function,
        run->diagnostic.block, run->diagnostic.instruction, (int)run->diagnostic.reason);
    else puts("writer-prefix-diagnostic status=NOT_ENTERED");
    const PrefixSample *after = &run->after[run->phase];
    printf("writer-prefix-summary graph=%s mode=%s sites=%zu allocated=%llu live=%llu peak=%llu work=%llu frozen_N=%zu fault_site=%zu hit=%u output_kept=%u failure_kept=%u\n",
        prefix_graph(run), prefix_mode(run), after->sites,
        (unsigned long long)after->ledger.allocated_bytes, (unsigned long long)after->ledger.live_bytes,
        (unsigned long long)after->ledger.peak_bytes, (unsigned long long)after->ledger.work,
        run->normal_sites, run->fault_site, (unsigned)run->hit, (unsigned)run->output_preserved, (unsigned)run->failure_kept);
}
static bool prefix_cold(void) {
    return !source_program_compile_attempts && !source_program_compile_live && !source_program_compile_bytes &&
        !source_program_compile_allocations && !source_program_compile_capacity && !source_program_owner_count &&
        !source_program_compile_injected && source_program_compile_fail_at == SIZE_MAX;
}
static int prefix_run(bool occupied, bool fault, size_t normal_sites, size_t site,
                      const XrCompileResourceLimits *caps, unsigned axis_mode) {
    PrefixRun run = {0};
    run.caps = caps ? *caps : (XrCompileResourceLimits) {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    run.axis_mode = axis_mode;
    run.occupied_graph = occupied;
    run.fault = fault;
    run.normal_sites = normal_sites;
    run.fault_site = site;
    run.accounting_ok = prefix_cold();
    run.owner_status = XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    run.check_status = run.write1_status = run.write2_status = XR_XIR_BAD_STRUCTURE;
    run.diagnostic = (XrXirDiagnostic) {XR_XIR_BAD_STRUCTURE, 17, 18, 19, XR_XIR_DIAGNOSTIC_NONE};
    memset(&run.output, 0xa5, sizeof(run.output));
    run.output.bytes = NULL; run.output.length = 0;
    memcpy(run.empty, &run.output, sizeof(run.empty));
    memcpy(run.checked_original, &run.checked, sizeof(run.checked_original));
    memcpy(run.owner_original, &run.context.resources, sizeof(run.owner_original));
    if (run.accounting_ok) {
        if (fault) { source_program_compile_fail_at = site; run.armed = true; }
        prefix_operations(&run);
    }
    run.hit = source_program_compile_injected;
    source_program_compile_fail_at = SIZE_MAX;
    if (fault || (axis_mode && !(axis_mode & 1u))) prefix_failure_snapshot(&run);
    prefix_report(&run);
    if (axis_mode) printf("writer-prefix-axis graph=%s axis=%u cut=%s limit=%llu actual=%llu\n",
        prefix_graph(&run), (axis_mode - 1) / 2, axis_mode & 1u ? "exact" : "minus1",
        (unsigned long long)prefix_axis_value(&run, true), (unsigned long long)prefix_axis_value(&run, false));
    prefix_cleanup(&run);
    bool passed = run.cleanup_ok && run.accounting_ok &&
        (fault ? prefix_fault_expected(&run) : axis_mode ? prefix_axis_expected(&run) : prefix_normal_expected(&run));
    printf("writer-prefix-final graph=%s mode=%s result=%s full_after=%u cleanup=%u physical=%zu/%zu\n",
        prefix_graph(&run), prefix_mode(&run), passed ? "PASS" : "FAIL", (unsigned)run.bytes_after,
        (unsigned)run.cleanup_ok, source_program_compile_live, source_program_compile_bytes);
    puts("One public I64 fixture only; original1932/12axes/FI6 and illegal-name resource/fault responsibilities remain OPEN");
    return passed ? 0 : 1;
}
static bool prefix_number(const char *text, size_t *output) {
    size_t value = 0;
    if (!text || !*text) return false;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        size_t digit = (size_t)(*p - '0');
        if (value > (SIZE_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (value == SIZE_MAX) return false;
    *output = value;
    return true;
}
static bool prefix_graph_argument(const char *text, bool *occupied) {
    if (!strcmp(text, "empty")) { *occupied = false; return true; }
    if (!strcmp(text, "occupied")) { *occupied = true; return true; }
    return false;
}
static bool prefix_axis_caps(const char *axis, const char *cut, size_t normal_total,
                             XrCompileResourceLimits *caps, unsigned *mode) {
    unsigned kind;
    if (!normal_total) return false;
    bool below = !strcmp(cut, "minus1");
    if (!below && strcmp(cut, "exact")) return false;
    if (!strcmp(axis, "allocated")) kind = 0;
    else if (!strcmp(axis, "live")) kind = 1;
    else if (!strcmp(axis, "work")) kind = 2;
    else return false;
    *caps = (XrCompileResourceLimits) {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    uint64_t value = (uint64_t)normal_total - (below ? 1u : 0u);
    if (!kind) { if ((uint64_t)normal_total > caps->allocated_bytes) return false; caps->allocated_bytes = value; }
    else if (kind == 1) { if ((uint64_t)normal_total > caps->live_bytes) return false; caps->live_bytes = value; }
    else { if ((uint64_t)normal_total > caps->work) return false; caps->work = value; }
    *mode = kind * 2 + (below ? 2u : 1u);
    return true;
}
int main(int argc, char **argv) {
    bool occupied = false;
    size_t normal_sites = 0, site = 0, normal_total = 0;
    unsigned axis_mode = 0;
    XrCompileResourceLimits caps = {0};
    if (argc == 3 && !strcmp(argv[1], "--normal") && prefix_graph_argument(argv[2], &occupied))
        return prefix_run(occupied, false, 0, 0, NULL, 0);
    if (argc == 5 && !strcmp(argv[1], "--fault") && prefix_graph_argument(argv[2], &occupied) &&
        prefix_number(argv[3], &normal_sites) && normal_sites && prefix_number(argv[4], &site) && site < normal_sites)
        return prefix_run(occupied, true, normal_sites, site, NULL, 0);
    if (argc == 6 && !strcmp(argv[1], "--axis") && prefix_graph_argument(argv[2], &occupied) &&
        prefix_number(argv[5], &normal_total) && prefix_axis_caps(argv[3], argv[4], normal_total, &caps, &axis_mode))
        return prefix_run(occupied, false, 0, 0, &caps, axis_mode);
    fputs("Usage: writer-prefix-fi --normal empty|occupied | --fault empty|occupied frozen-normal-N site | --axis empty|occupied allocated|live|work exact|minus1 frozen-normal-total\n", stderr);
    return 2;
}
