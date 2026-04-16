/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmem_profiler.h - Memory profiler for runtime analysis
 *
 * KEY CONCEPT:
 *   Compile-time controlled memory statistics per component.
 *   Zero overhead when disabled (macros expand to nothing).
 *
 * USAGE:
 *   1. Uncomment macro (e.g. XR_PROFILE_MAP_MEMORY)
 *   2. Run program, stats printed on exit
 *   3. Comment out macro when done
 */

#ifndef XMEM_PROFILER_H
#define XMEM_PROFILER_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* ========== Profiler Switches ========== */

// Uncomment to enable profiling for specific components
// #define XR_PROFILE_MAP_MEMORY
// #define XR_PROFILE_ARRAY_MEMORY
// #define XR_PROFILE_STRING_MEMORY
// #define XR_PROFILE_CLASS_MEMORY
// #define XR_PROFILE_GC_MEMORY

/* ========== Statistics Structure ========== */

typedef struct XrProfileStats {
    size_t alloc_count;
    size_t free_count;
    size_t alloc_bytes;
    size_t free_bytes;
    size_t peak_bytes;
    size_t current_bytes;
} XrProfileStats;

// Initialize stats
static inline void xr_profile_stats_init(XrProfileStats *stats) {
    stats->alloc_count = 0;
    stats->free_count = 0;
    stats->alloc_bytes = 0;
    stats->free_bytes = 0;
    stats->peak_bytes = 0;
    stats->current_bytes = 0;
}

// Record allocation
static inline void xr_profile_stats_alloc(XrProfileStats *stats, size_t bytes) {
    stats->alloc_count++;
    stats->alloc_bytes += bytes;
    stats->current_bytes += bytes;
    if (stats->current_bytes > stats->peak_bytes) {
        stats->peak_bytes = stats->current_bytes;
    }
}

// Record free
static inline void xr_profile_stats_free(XrProfileStats *stats, size_t bytes) {
    stats->free_count++;
    stats->free_bytes += bytes;
    if (stats->current_bytes >= bytes) {
        stats->current_bytes -= bytes;
    }
}

// Print stats
static inline void xr_profile_stats_print(const char *name, XrProfileStats *stats) {
    printf("\n===== %s Memory Stats =====\n", name);
    printf("Allocations:    %zu\n", stats->alloc_count);
    printf("Frees:          %zu\n", stats->free_count);
    printf("Live objects:   %zu\n", stats->alloc_count - stats->free_count);
    printf("Alloc bytes:    %zu (%.2f MB)\n", stats->alloc_bytes, stats->alloc_bytes / 1048576.0);
    printf("Peak bytes:     %zu (%.2f MB)\n", stats->peak_bytes, stats->peak_bytes / 1048576.0);
    printf("Current bytes:  %zu (%.2f MB)\n", stats->current_bytes, stats->current_bytes / 1048576.0);
    if (stats->alloc_count > 0) {
        printf("Avg per alloc:  %.1f bytes\n", (double)stats->alloc_bytes / stats->alloc_count);
    }
    printf("==============================\n");
}

/* ========== Map Profiling ========== */

#ifdef XR_PROFILE_MAP_MEMORY

extern XrProfileStats g_map_header_stats;
extern XrProfileStats g_map_node_stats;
extern size_t g_map_new_count;
extern size_t g_map_rehash_count;

#define XR_MAP_PROFILE_INIT() do { \
    xr_profile_stats_init(&g_map_header_stats); \
    xr_profile_stats_init(&g_map_node_stats); \
    g_map_new_count = 0; \
    g_map_rehash_count = 0; \
} while(0)

#define XR_MAP_PROFILE_ALLOC_HEADER(bytes) \
    xr_profile_stats_alloc(&g_map_header_stats, bytes)

#define XR_MAP_PROFILE_ALLOC_NODES(bytes) \
    xr_profile_stats_alloc(&g_map_node_stats, bytes)

#define XR_MAP_PROFILE_FREE_HEADER(bytes) \
    xr_profile_stats_free(&g_map_header_stats, bytes)

#define XR_MAP_PROFILE_FREE_NODES(bytes) \
    xr_profile_stats_free(&g_map_node_stats, bytes)

#define XR_MAP_PROFILE_COUNT_NEW() (g_map_new_count++)
#define XR_MAP_PROFILE_COUNT_REHASH() (g_map_rehash_count++)

#define XR_MAP_PROFILE_PRINT() do { \
    xr_profile_stats_print("Map Headers", &g_map_header_stats); \
    xr_profile_stats_print("Map Nodes", &g_map_node_stats); \
    printf("Map creations:   %zu\n", g_map_new_count); \
    printf("Rehashes:        %zu\n", g_map_rehash_count); \
    printf("Map Total: %.2f MB\n", \
           (g_map_header_stats.alloc_bytes + g_map_node_stats.alloc_bytes) / 1048576.0); \
} while(0)

#else

#define XR_MAP_PROFILE_INIT()
#define XR_MAP_PROFILE_ALLOC_HEADER(bytes)
#define XR_MAP_PROFILE_ALLOC_NODES(bytes)
#define XR_MAP_PROFILE_FREE_HEADER(bytes)
#define XR_MAP_PROFILE_FREE_NODES(bytes)
#define XR_MAP_PROFILE_COUNT_NEW()
#define XR_MAP_PROFILE_COUNT_REHASH()
#define XR_MAP_PROFILE_PRINT()

#endif // ========== Array Profiling ==========

#ifdef XR_PROFILE_ARRAY_MEMORY

extern XrProfileStats g_array_header_stats;
extern XrProfileStats g_array_elem_stats;

#define XR_ARRAY_PROFILE_INIT() do { \
    xr_profile_stats_init(&g_array_header_stats); \
    xr_profile_stats_init(&g_array_elem_stats); \
} while(0)

#define XR_ARRAY_PROFILE_ALLOC_HEADER(bytes) \
    xr_profile_stats_alloc(&g_array_header_stats, bytes)

#define XR_ARRAY_PROFILE_ALLOC_ELEMENTS(bytes) \
    xr_profile_stats_alloc(&g_array_elem_stats, bytes)

#define XR_ARRAY_PROFILE_PRINT() do { \
    xr_profile_stats_print("Array Headers", &g_array_header_stats); \
    xr_profile_stats_print("Array Elements", &g_array_elem_stats); \
} while(0)

#else

#define XR_ARRAY_PROFILE_INIT()
#define XR_ARRAY_PROFILE_ALLOC_HEADER(bytes)
#define XR_ARRAY_PROFILE_ALLOC_ELEMENTS(bytes)
#define XR_ARRAY_PROFILE_PRINT()

#endif // ========== String Profiling ==========

#ifdef XR_PROFILE_STRING_MEMORY

extern XrProfileStats g_string_stats;
extern size_t g_string_intern_hits;
extern size_t g_string_intern_misses;

#define XR_STRING_PROFILE_INIT() do { \
    xr_profile_stats_init(&g_string_stats); \
    g_string_intern_hits = 0; \
    g_string_intern_misses = 0; \
} while(0)

#define XR_STRING_PROFILE_ALLOC(bytes) \
    xr_profile_stats_alloc(&g_string_stats, bytes)

#define XR_STRING_PROFILE_INTERN_HIT() (g_string_intern_hits++)
#define XR_STRING_PROFILE_INTERN_MISS() (g_string_intern_misses++)

#define XR_STRING_PROFILE_PRINT() do { \
    xr_profile_stats_print("Strings", &g_string_stats); \
    printf("Intern hits:    %zu\n", g_string_intern_hits); \
    printf("Intern misses:  %zu\n", g_string_intern_misses); \
    if (g_string_intern_hits + g_string_intern_misses > 0) { \
        printf("Intern rate:    %.1f%%\n", \
               100.0 * g_string_intern_hits / (g_string_intern_hits + g_string_intern_misses)); \
    } \
} while(0)

#else

#define XR_STRING_PROFILE_INIT()
#define XR_STRING_PROFILE_ALLOC(bytes)
#define XR_STRING_PROFILE_INTERN_HIT()
#define XR_STRING_PROFILE_INTERN_MISS()
#define XR_STRING_PROFILE_PRINT()

#endif // ========== Class/Instance Profiling ==========

#ifdef XR_PROFILE_CLASS_MEMORY

extern XrProfileStats g_class_stats;
extern XrProfileStats g_instance_stats;

#define XR_CLASS_PROFILE_INIT() do { \
    xr_profile_stats_init(&g_class_stats); \
    xr_profile_stats_init(&g_instance_stats); \
} while(0)

#define XR_CLASS_PROFILE_ALLOC(bytes) \
    xr_profile_stats_alloc(&g_class_stats, bytes)

#define XR_INSTANCE_PROFILE_ALLOC(bytes) \
    xr_profile_stats_alloc(&g_instance_stats, bytes)

#define XR_CLASS_PROFILE_PRINT() do { \
    xr_profile_stats_print("Classes", &g_class_stats); \
    xr_profile_stats_print("Instances", &g_instance_stats); \
} while(0)

#else

#define XR_CLASS_PROFILE_INIT()
#define XR_CLASS_PROFILE_ALLOC(bytes)
#define XR_INSTANCE_PROFILE_ALLOC(bytes)
#define XR_CLASS_PROFILE_PRINT()

#endif // ========== Global Init and Print ==========

// Initialize all enabled profilers
static inline void xr_mem_profiler_init(void) {
    XR_MAP_PROFILE_INIT();
    XR_ARRAY_PROFILE_INIT();
    XR_STRING_PROFILE_INIT();
    XR_CLASS_PROFILE_INIT();
}

// Print all enabled profiler stats
static inline void xr_mem_profiler_report(void) {
    XR_MAP_PROFILE_PRINT();
    XR_ARRAY_PROFILE_PRINT();
    XR_STRING_PROFILE_PRINT();
    XR_CLASS_PROFILE_PRINT();
}

// Register atexit handler for auto-print
static int g_mem_profiler_registered = 0;
static inline void xr_mem_profiler_register_atexit(void) {
    if (!g_mem_profiler_registered) {
        atexit(xr_mem_profiler_report);
        g_mem_profiler_registered = 1;
    }
}

#endif // XMEM_PROFILER_H
