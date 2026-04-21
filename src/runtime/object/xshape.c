/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xshape.c - Shape (hidden class) implementation
 *
 * KEY CONCEPT:
 *   Shape is immutable. Adding field creates new Shape.
 *   Transitions table caches child shapes to avoid duplicates.
 */

#include "xshape.h"
#include "../../base/xchecks.h"
#include "../gc/xgc.h"
#include "../../base/xmalloc.h"
#include "../xisolate_api.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========== Global Shape Registry ========== */
/*
 * WARNING: Thread safety
 *   These are process-global mutable variables, NOT per-isolate.
 *   In a multi-isolate (multi-threaded) environment, concurrent access
 *   to shape_registry is a data race. To fix properly, migrate the
 *   registry into XrayIsolate and pass isolate to xr_shape_get_by_id().
 *   This requires signature changes across VM, JIT, and object layers.
 */

#define SHAPE_REGISTRY_INIT_CAP 256

static XrShape **shape_registry = NULL;
static uint16_t shape_registry_count = 0;
static uint16_t shape_registry_cap = 0;

void xr_shape_registry_init(void) {
    if (shape_registry) return;
    shape_registry = (XrShape**)xr_calloc(SHAPE_REGISTRY_INIT_CAP, sizeof(XrShape*));
    shape_registry_cap = SHAPE_REGISTRY_INIT_CAP;
    shape_registry_count = 0;
}

void xr_shape_registry_destroy(void) {
    if (shape_registry) {
        xr_free(shape_registry);
        shape_registry = NULL;
        shape_registry_count = 0;
        shape_registry_cap = 0;
    }
}

XrShape *xr_shape_get_by_id(uint16_t id) {
    if (id >= shape_registry_count) return NULL;
    return shape_registry[id];
}

static uint16_t shape_registry_add(XrShape *shape) {
    if (!shape_registry) xr_shape_registry_init();
    if (shape_registry_count >= XR_SHAPE_MAX_ID) {
        // Overflow: return 0 (empty shape) as fallback
        return 0;
    }
    if (shape_registry_count >= shape_registry_cap) {
        uint16_t new_cap = shape_registry_cap * 2;
        if (new_cap > XR_SHAPE_MAX_ID + 1) new_cap = XR_SHAPE_MAX_ID + 1;
        XrShape **new_reg = (XrShape**)xr_realloc(shape_registry, new_cap * sizeof(XrShape*));
        if (!new_reg) return 0;
        memset(new_reg + shape_registry_cap, 0, (new_cap - shape_registry_cap) * sizeof(XrShape*));
        shape_registry = new_reg;
        shape_registry_cap = new_cap;
    }
    uint16_t id = shape_registry_count++;
    shape_registry[id] = shape;
    return id;
}

/* ========== Internal Helpers ========== */

static void shape_build_index_table(XrShape *shape) {
    if (shape->field_count == 0) {
        shape->symbol_to_index = NULL;
        shape->min_symbol = 0;
        shape->max_symbol = 0;
        return;
    }

    SymbolId min_sym = shape->field_symbols[0];
    SymbolId max_sym = shape->field_symbols[0];

    for (uint16_t i = 1; i < shape->field_count; i++) {
        SymbolId sym = shape->field_symbols[i];
        if (sym < min_sym) min_sym = sym;
        if (sym > max_sym) max_sym = sym;
    }

    shape->min_symbol = min_sym;
    shape->max_symbol = max_sym;

    size_t table_size = (size_t)(max_sym - min_sym + 1);
    shape->symbol_to_index = (int16_t*)xr_malloc(table_size * sizeof(int16_t));
    memset(shape->symbol_to_index, -1, table_size * sizeof(int16_t));

    for (uint16_t i = 0; i < shape->field_count; i++) {
        SymbolId sym = shape->field_symbols[i];
        XR_DCHECK(sym >= min_sym && sym <= max_sym, "shape_build_index: symbol out of range");
        shape->symbol_to_index[sym - min_sym] = (int16_t)i;
    }
}

/* ========== Creation ========== */

// Shape uses xr_malloc(not GC), lifetime equals Isolate
XrShape *xr_shape_new(XrayIsolate *X, uint16_t capacity) {
    XR_DCHECK(X != NULL, "shape_new: NULL isolate");
    XR_DCHECK(capacity > 0, "shape_new: zero capacity");
    (void)X;

    XrShape *shape = (XrShape*)xr_calloc(1, sizeof(XrShape));
    if (!shape) return NULL;

    shape->id = shape_registry_add(shape);
    shape->field_count = 0;
    shape->in_object_capacity = capacity;
    shape->field_symbols = NULL;
    shape->symbol_to_index = NULL;
    shape->min_symbol = 0;
    shape->max_symbol = 0;
    shape->parent = NULL;
    shape->transitions = NULL;

    return shape;
}

/* ========== Compact Shape ========== */

// Alignment helper: round up offset to natural alignment of type
static uint16_t compact_align_offset(uint16_t offset, uint8_t size) {
    if (size == 0) return offset;
    uint16_t align = (size >= 8) ? 8 : (size >= 4) ? 4 : (size >= 2) ? 2 : 1;
    return (offset + align - 1) & ~(align - 1);
}

XrShape *xr_shape_new_compact(XrayIsolate *X, const XrCompactFieldDef *fields, uint16_t count) {
    XR_DCHECK(X != NULL, "shape_new_compact: NULL isolate");
    XR_DCHECK(fields != NULL, "shape_new_compact: NULL fields");
    XR_DCHECK(count > 0, "shape_new_compact: zero count");
    XrShape *shape = (XrShape*)xr_calloc(1, sizeof(XrShape));
    if (!shape) return NULL;

    shape->id = shape_registry_add(shape);
    shape->field_count = count;
    shape->in_object_capacity = count;
    shape->is_compact = true;
    shape->has_gc_fields = false;

    // Allocate arrays
    shape->field_symbols = (SymbolId*)xr_malloc(count * sizeof(SymbolId));
    shape->field_types = (XrCompactType*)xr_malloc(count * sizeof(XrCompactType));
    shape->field_offsets = (uint16_t*)xr_malloc(count * sizeof(uint16_t));

    // Register symbols + compute offsets with natural alignment
    XrSymbolTable *symtab = X ? (XrSymbolTable*)xr_isolate_get_symbol_table(X) : NULL;
    uint16_t offset = 0;

    for (uint16_t i = 0; i < count; i++) {
        // Register symbol
        SymbolId sym = SYMBOL_INVALID;
        if (symtab && fields[i].name) {
            sym = xr_symbol_register_in_table(symtab, fields[i].name);
        }
        shape->field_symbols[i] = sym;
        shape->field_types[i] = fields[i].type;

        // Compute aligned offset
        uint8_t size = xr_compact_type_size(fields[i].type);
        offset = compact_align_offset(offset, size);
        shape->field_offsets[i] = offset;
        offset += size;

        if (fields[i].type == XR_COMPACT_PTR) {
            shape->has_gc_fields = true;
        }
    }

    // Round total size up to 8-byte alignment for GC header compat
    shape->compact_data_size = (offset + 7) & ~7;
    shape->is_value_layout = shape->is_compact && !shape->has_gc_fields;

    // Build symbol→index lookup table
    shape_build_index_table(shape);

    return shape;
}

/* ========== Transition Table ========== */

static XrShape* transition_table_find(XrTransitionTable *table, SymbolId symbol) {
    if (!table || !table->entries) return NULL;

    for (uint16_t i = 0; i < table->count; i++) {
        if (table->entries[i].symbol == symbol) {
            return table->entries[i].target;
        }
    }
    return NULL;
}

static void transition_table_add(XrTransitionTable *table, SymbolId symbol, XrShape *target) {
    XR_DCHECK(target != NULL, "transition_table_add: NULL target");
    if (!table) return;

    if (table->count >= table->capacity) {
        uint16_t new_cap = table->capacity == 0 ? 4 : table->capacity * 2;
        size_t old_size = table->capacity * sizeof(XrShapeTransition);
        size_t new_size = new_cap * sizeof(XrShapeTransition);
        XrShapeTransition *new_entries = (XrShapeTransition*)xr_realloc(
            table->entries, new_size);
        if (!new_entries) return;
        if (new_size > old_size) {
            memset((char*)new_entries + old_size, 0, new_size - old_size);
        }
        table->entries = new_entries;
        table->capacity = new_cap;
    }

    table->entries[table->count].symbol = symbol;
    table->entries[table->count].target = target;
    table->count++;
    XR_DCHECK(table->count <= table->capacity, "transition_table_add: count > capacity");
}

/* ========== Shape Transition API ========== */

// Returns NULL if max field limit reached (256).
// Overflow fields beyond in_object_capacity are stored in XrJsonOverflow.
#define SHAPE_MAX_TOTAL_FIELDS 256
XrShape *xr_shape_transition(XrayIsolate *X, XrShape *from, SymbolId symbol) {
    XR_DCHECK(X != NULL, "shape_transition: NULL isolate");
    XR_DCHECK(symbol != SYMBOL_INVALID, "shape_transition: invalid symbol");
    if (!from) return NULL;

    if (from->field_count >= SHAPE_MAX_TOTAL_FIELDS) {
        return NULL;
    }

    // Check cache
    if (from->transitions) {
        XrShape *cached = transition_table_find(from->transitions, symbol);
        if (cached) {
            return cached;
        }
    }

    // Create new Shape
    XrShape *to = xr_shape_new(X, from->in_object_capacity);
    if (!to) return NULL;

    to->field_count = from->field_count + 1;
    to->parent = from;

    to->field_symbols = (SymbolId*)xr_malloc(to->field_count * sizeof(SymbolId));
    if (from->field_symbols && from->field_count > 0) {
        memcpy(to->field_symbols, from->field_symbols,
               from->field_count * sizeof(SymbolId));
    }
    to->field_symbols[from->field_count] = symbol;

    shape_build_index_table(to);

    // Cache transition
    if (!from->transitions) {
        from->transitions = (XrTransitionTable*)xr_calloc(1, sizeof(XrTransitionTable));
    }
    transition_table_add(from->transitions, symbol, to);

    return to;
}

/* ========== Pre-built Shape ========== */

XrShape *xr_shape_build_fixed(XrayIsolate *X, SymbolId *symbols, uint16_t count) {
    XR_DCHECK(X != NULL, "shape_build_fixed: NULL isolate");
    XrShape *shape = xr_shape_new(X, count);
    if (!shape) return NULL;

    for (uint16_t i = 0; i < count; i++) {
        XrShape *next = xr_shape_transition(X, shape, symbols[i]);
        if (!next) return NULL;
        shape = next;
    }
    return shape;
}

/* ========== Type Tracking ========== */

bool xr_shape_is_descendant_of(XrShape *child, XrShape *ancestor) {
    while (child) {
        if (child == ancestor) return true;
        child = child->parent;
    }
    return false;
}

/* ========== GC Integration ========== */

void xr_gc_destroy_shape(void *obj) {
    XrShape *shape = (XrShape*)obj;
    if (!shape) return;

    if (shape->field_symbols) {
        xr_free(shape->field_symbols);
        shape->field_symbols = NULL;
    }

    if (shape->symbol_to_index) {
        xr_free(shape->symbol_to_index);
        shape->symbol_to_index = NULL;
    }

    if (shape->transitions) {
        if (shape->transitions->entries) {
            xr_free(shape->transitions->entries);
        }
        xr_free(shape->transitions);
        shape->transitions = NULL;
    }

    if (shape->field_types) {
        xr_free(shape->field_types);
        shape->field_types = NULL;
    }

    if (shape->field_offsets) {
        xr_free(shape->field_offsets);
        shape->field_offsets = NULL;
    }
}
