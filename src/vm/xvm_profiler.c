/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_profiler.c - VM performance profiler implementation
 */

#include "xvm_profiler.h"
#include "../base/xchecks.h"
#include "../runtime/value/xopcode_info.h"

#if XR_ENABLE_VM_PROFILER

#include <stdio.h>
#include <stdlib.h>
#include "xdebug.h"

VMProfiler g_vm_profiler;

// Structure for sorting opcode statistics
typedef struct {
    int op;
    VMOpStats stats;
} SortedOpStats;

// Comparison function for sorting by count
static int compare_by_count(const void *a, const void *b) {
    const SortedOpStats *sa = (const SortedOpStats *)a;
    const SortedOpStats *sb = (const SortedOpStats *)b;

    if (sb->stats.count > sa->stats.count) return 1;
    if (sb->stats.count < sa->stats.count) return -1;
    return 0;
}

#if XR_PROFILE_TIMING
// Comparison function for sorting by time
static int compare_by_time(const void *a, const void *b) {
    const SortedOpStats *sa = (const SortedOpStats *)a;
    const SortedOpStats *sb = (const SortedOpStats *)b;

    if (sb->stats.total_ns > sa->stats.total_ns) return 1;
    if (sb->stats.total_ns < sa->stats.total_ns) return -1;
    return 0;
}
#endif

// Get opcode name (delegates to the single-source-of-truth table in
// xopcode_info.c so the profiler never falls out of sync with the
// OpCode enum or the disassembler).
static const char* get_opcode_name(int op) {
    if (op < 0 || op >= NUM_OPCODES) return "UNKNOWN";
    return xr_opcode_name((OpCode)op);
}

// Print performance report
void vm_profiler_report(void) {
    uint64_t end_time_ms = vm_profiler_get_ms();

    printf("\n");
    printf("======== VM Performance Report ========\n");
    printf("\n");

#if XR_PROFILE_TIMING
    double total_sec = (end_time_ms - g_vm_profiler.start_time_ms) / 1000.0;
#else
    double total_sec = 0.0;
#endif
    printf("Summary\n");
    printf("  Total instructions: %llu\n", (unsigned long long)g_vm_profiler.total_instructions);
#if XR_PROFILE_TIMING
    printf("  Runtime: %.3f sec\n", total_sec);
    if (total_sec > 0) {
        printf("  Speed: %.2f MIPS\n", g_vm_profiler.total_instructions / total_sec / 1000000.0);
    }
#else
    printf("  (Timing disabled, set XR_PROFILE_TIMING=1 to enable)\n");
#endif
    printf("\n");

    SortedOpStats sorted_stats[256];
    int count = 0;

    for (int i = 0; i < 256; i++) {
        if (g_vm_profiler.op_stats[i].count > 0) {
            sorted_stats[count].op = i;
            sorted_stats[count].stats = g_vm_profiler.op_stats[i];
            count++;
        }
    }

    if (count == 0) {
        printf("No instruction stats collected\n");
        return;
    }

    qsort(sorted_stats, count, sizeof(SortedOpStats), compare_by_count);

    printf("Hot Instructions (by count, Top 20)\n");
    printf("%-20s %12s %8s\n", "Instruction", "Count", "Percent");
    printf("%-20s %12s %8s\n", "--------------------", "------------", "--------");

    for (int i = 0; i < count && i < 20; i++) {
        int op = sorted_stats[i].op;
        VMOpStats *stats = &sorted_stats[i].stats;
        const char *name = get_opcode_name(op);
        double percent = (double)stats->count * 100.0 / g_vm_profiler.total_instructions;

        printf("%-20s %12llu %7.2f%%\n",
               name, (unsigned long long)stats->count, percent);
    }
    printf("\n");

#if XR_PROFILE_TIMING
    qsort(sorted_stats, count, sizeof(SortedOpStats), compare_by_time);

    printf("Slow Instructions (by time, Top 20)\n");
    printf("%-20s %12s %12s %10s\n", "Instruction", "Count", "Total(ms)", "Avg(ns)");
    printf("%-20s %12s %12s %10s\n", "--------------------", "------------", "------------", "----------");

    for (int i = 0; i < count && i < 20; i++) {
        int op = sorted_stats[i].op;
        VMOpStats *stats = &sorted_stats[i].stats;
        const char *name = get_opcode_name(op);
        double ms = stats->total_ns / 1000000.0;
        double avg_ns = (double)stats->total_ns / stats->count;

        printf("%-20s %12llu %12.2f %10.1f\n",
               name, (unsigned long long)stats->count, ms, avg_ns);
    }
    printf("\n");
#endif

    printf("All Executed Instructions (%d types)\n", count);
    printf("%-20s %12s\n", "Instruction", "Count");
    printf("%-20s %12s\n", "--------------------", "------------");

    for (int i = 0; i < count; i++) {
        int op = sorted_stats[i].op;
        VMOpStats *stats = &sorted_stats[i].stats;
        const char *name = get_opcode_name(op);

        printf("%-20s %12llu\n",
               name, (unsigned long long)stats->count);
    }
    printf("\n");
}

#endif
