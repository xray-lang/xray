/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbc_stackmap.h - Bytecode stack map for precise GC root scanning
 *
 * KEY CONCEPT:
 *   At compile-time safepoints (allocation instructions), the compiler
 *   records which stack slots hold live GC-managed values. At GC time,
 *   mark_coro_roots uses this to scan only live slots instead of the
 *   entire frame.
 *
 *   Safepoints are only recorded for explicit alloc instructions
 *   (OP_NEWARRAY, OP_NEWMAP, etc.). For other PCs (CALL, INVOKE),
 *   the GC falls back to conservative full-frame scanning.
 *
 * LAYOUT:
 *   XrBcStackMap {
 *     count, maxslots,
 *     entries[count] {pc, bitmap_offset, live_count},
 *     bitmap_pool[]  (packed uint64_t words)
 *   }
 *   entries sorted by pc for binary search lookup.
 */

#ifndef XBC_STACKMAP_H
#define XBC_STACKMAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>            // NULL
#include "../../base/xdefs.h"  // XR_FUNC

/* One safepoint: which slots are live at this bytecode PC. */
typedef struct {
    uint32_t pc;             // Bytecode PC (instruction index)
    uint32_t bitmap_offset;  // Word offset into bitmap_pool
    uint16_t live_count;     // Number of live slots (for stats)
    uint16_t num_words;      // Number of uint64_t words in bitmap
} XrBcStackMapEntry;

/*
 * Per-function bytecode stack map, attached to XrProto.
 * Entries sorted by pc for O(log n) binary search.
 */
typedef struct {
    uint32_t count;     // Number of safepoint entries
    uint16_t maxslots;  // Max slot index across all entries
    uint16_t _pad;
    XrBcStackMapEntry *entries;  // [count] sorted by pc
    uint64_t *bitmap_pool;       // Shared bitmap storage
    uint32_t bitmap_pool_words;  // Total words in bitmap_pool
} XrBcStackMap;

/*
 * Lookup safepoint entry for a given PC.
 * Returns NULL if no safepoint recorded (caller should use conservative scan).
 */
static inline const XrBcStackMapEntry *xr_bc_stackmap_lookup(const XrBcStackMap *map, uint32_t pc) {
    if (!map || map->count == 0)
        return NULL;

    // Binary search on sorted entries
    uint32_t lo = 0, hi = map->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (map->entries[mid].pc < pc) {
            lo = mid + 1;
        } else if (map->entries[mid].pc > pc) {
            hi = mid;
        } else {
            return &map->entries[mid];
        }
    }
    return NULL;
}

/*
 * Check if slot `slot` is live according to the bitmap entry.
 */
static inline bool xr_bc_stackmap_slot_live(const XrBcStackMap *map, const XrBcStackMapEntry *entry,
                                            uint32_t slot) {
    uint32_t word_idx = slot / 64;
    uint32_t bit_idx = slot % 64;
    if (word_idx >= entry->num_words)
        return false;
    return (map->bitmap_pool[entry->bitmap_offset + word_idx] >> bit_idx) & 1;
}

/* ========== Builder API (used by codegen) ========== */

struct XrBcStackMapBuilder;
typedef struct XrBcStackMapBuilder XrBcStackMapBuilder;

// Create a builder for a function with maxslots stack slots.
XR_FUNC XrBcStackMapBuilder *xr_bc_stackmap_builder_create(uint16_t maxslots);

// Record a safepoint at `pc` with the given live slot bitmap.
// `live_bitmap` has `(maxslots+63)/64` words. Builder copies the data.
XR_FUNC void xr_bc_stackmap_builder_add(XrBcStackMapBuilder *b, uint32_t pc,
                                        const uint64_t *live_bitmap);

// Finalize: sort entries by pc and produce a compact XrBcStackMap.
// Each entry currently owns its own slice in bitmap_pool; bitmaps are
// not deduplicated (identical live-prefix maps occupy separate slots).
// Caller takes ownership of the returned map. Builder is freed.
XR_FUNC XrBcStackMap *xr_bc_stackmap_builder_finish(XrBcStackMapBuilder *b);

// Free a completed stackmap.
XR_FUNC void xr_bc_stackmap_destroy(XrBcStackMap *map);

#endif  // XBC_STACKMAP_H
