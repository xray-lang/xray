/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xshape.h - Object shape descriptor (hidden class)
 *
 * KEY CONCEPT:
 *   Describes object property layout.
 *   Objects with same property structure share one Shape.
 *   Zero-copy transitions when adding properties.
 */

#ifndef XSHAPE_H
#define XSHAPE_H

#include "../gc/xgc_header.h"
#include "../gc/xgc.h"
#include "../symbol/xsymbol_table.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct XrShapeTransition {
    SymbolId symbol;
    struct XrShape *target;
} XrShapeTransition;

typedef struct XrTransitionTable {
    XrShapeTransition *entries;
    uint16_t count;
    uint16_t capacity;
} XrTransitionTable;

typedef struct XrShape XrShape;

#define SHAPE_DEFAULT_CAPACITY 8
#define SHAPE_MAX_FAST_FIELDS 32

/* ========== Compact Field Types ========== */

typedef enum {
    XR_COMPACT_INT32 = 0,    // 4 bytes (int32_t)
    XR_COMPACT_INT64 = 1,    // 8 bytes (int64_t) — default for 'int'
    XR_COMPACT_BOOL = 2,     // 1 byte  (uint8_t)
    XR_COMPACT_FLOAT64 = 3,  // 8 bytes (double) — default for 'float'
    XR_COMPACT_UINT8 = 4,    // 1 byte  (uint8_t)
    XR_COMPACT_UINT16 = 5,   // 2 bytes (uint16_t)
    XR_COMPACT_UINT32 = 6,   // 4 bytes (uint32_t)
    XR_COMPACT_FLOAT32 = 7,  // 4 bytes (float)
    XR_COMPACT_PTR = 8,      // 8 bytes (XrValue — GC pointer, string/Json/Array)
} XrCompactType;

static inline uint8_t xr_compact_type_size(XrCompactType t) {
    static const uint8_t sizes[] = {4, 8, 1, 8, 1, 2, 4, 4, 8};
    return (t <= XR_COMPACT_PTR) ? sizes[t] : 0;
}

// For xr_shape_new_compact() builder
typedef struct {
    const char *name;
    XrCompactType type;
} XrCompactFieldDef;

struct XrShape {
    XrGCHeader gc;

    uint16_t id;  // Global registry index (14 bits, max 16383)
    uint16_t field_count;
    uint16_t in_object_capacity;  // Fixed at creation

    SymbolId *field_symbols;  // In insertion order

    int16_t *symbol_to_index;  // O(1) lookup (int16 to support field_count up to 256)
    SymbolId min_symbol;
    SymbolId max_symbol;

    struct XrShape *parent;
    XrTransitionTable *transitions;  // symbol -> child shape

    bool is_sealed;  // true = reject field additions via transition

    // Compact mode (fields stored as native C types, not XrValue)
    bool is_compact;             // true = compact layout
    bool has_gc_fields;          // true = at least one PTR field (Level 1)
    bool is_value_layout;        // true = compact && no GC fields (memcpy safe)
    uint16_t compact_data_size;  // Total byte size of compact data area
    XrCompactType *field_types;  // field_types[field_count]
    uint16_t *field_offsets;     // field_offsets[field_count] (byte offset into data[])
};

/* ========== Per-Isolate Shape Registry ========== */

#define XR_SHAPE_MAX_ID 16383  // 14 bits

XR_FUNC void xr_shape_registry_init(XrayIsolate *X);
XR_FUNC void xr_shape_registry_destroy(XrayIsolate *X);
XR_FUNC XrShape *xr_shape_get_by_id(XrayIsolate *X, uint16_t id);

/* ========== Creation ========== */

XR_FUNC XrShape *xr_shape_new(XrayIsolate *X, uint16_t capacity);
// Build a compact shape from field definitions (name + type pairs).
// Compact shapes have fixed byte offsets, native type storage, and precise GC info.
XR_FUNC XrShape *xr_shape_new_compact(XrayIsolate *X, const XrCompactFieldDef *fields,
                                      uint16_t count);

/* ========== Transition ========== */

// Returns NULL if field_count >= in_object_capacity (needs downgrade)
XR_FUNC XrShape *xr_shape_transition(XrayIsolate *X, XrShape *from, SymbolId symbol);

// Build a fully-populated Shape from a fixed list of symbol IDs.
// Used for pre-built shared shapes (e.g., connection handles).
XR_FUNC XrShape *xr_shape_build_fixed(XrayIsolate *X, SymbolId *symbols, uint16_t count);

/* ========== Query ========== */

static inline int xr_shape_field_index(XrShape *shape, SymbolId symbol) {
    if (!shape)
        return -1;
    if (symbol < shape->min_symbol || symbol > shape->max_symbol)
        return -1;
    if (!shape->symbol_to_index)
        return -1;
    return shape->symbol_to_index[symbol - shape->min_symbol];
}

static inline bool xr_shape_has_field(XrShape *shape, SymbolId symbol) {
    return xr_shape_field_index(shape, symbol) >= 0;
}

static inline uint16_t xr_shape_field_count(XrShape *shape) {
    return shape ? shape->field_count : 0;
}

/* ========== Type Tracking ========== */

XR_FUNC bool xr_shape_is_descendant_of(XrShape *child, XrShape *ancestor);

/* ========== GC ========== */

XR_FUNC void xr_gc_destroy_shape(void *obj);

/* ========== Shape ID for GC Header ========== */

// GC header extra field layout for Json objects:
//   bit 0:    shared storage flag (existing)
//   bit 1:    reserved
//   bits 2-15: shape_id (14 bits)
#define XR_SHAPE_ID_SHIFT 2
#define XR_SHAPE_ID_MASK 0xFFFC  // bits 2-15

static inline uint16_t xr_gc_get_shape_id(XrGCHeader *gc) {
    return (gc->extra & XR_SHAPE_ID_MASK) >> XR_SHAPE_ID_SHIFT;
}

static inline void xr_gc_set_shape_id(XrGCHeader *gc, uint16_t shape_id) {
    gc->extra = (gc->extra & ~XR_SHAPE_ID_MASK) | (shape_id << XR_SHAPE_ID_SHIFT);
}

#endif  // XSHAPE_H
