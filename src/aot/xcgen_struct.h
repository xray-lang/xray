/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcgen_struct.h - Json → C struct promotion for AOT
 *
 * KEY CONCEPT:
 *   Analyzes Shape metadata from OP_NEWJSON instructions to determine
 *   which Json objects can be promoted to stack-allocated C structs.
 *   Fields become native C types (int64_t/double) instead of XrValue.
 *
 * WHY THIS DESIGN:
 *   - Eliminates heap allocation and GC pressure for typed Json objects
 *   - Enables C compiler SROA to put fields in registers
 *   - Same XIR pipeline, only AOT codegen changes
 */

#ifndef XCGEN_STRUCT_H
#define XCGEN_STRUCT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../base/xdefs.h"

#define XCGEN_MAX_STRUCT_FIELDS 32
#define XCGEN_MAX_STRUCTS       16
#define XCGEN_MAX_FIELD_ENTRIES (XCGEN_MAX_STRUCTS * XCGEN_MAX_STRUCT_FIELDS)

// Single field descriptor
typedef struct {
    const char *name;             // C-safe field name (from SymbolId lookup)
    uint8_t     xir_type;        // XR_REP_I64 / XR_REP_F64 / XR_REP_PTR
    uint8_t     c_type;          // 0=int64_t, 1=double, 2=XrtValue
    uint8_t     val_hint_type;   // Semantic type hint (XR_REP_*) from infer_field_type.
                                 // When c_type==2 (XrtValue layout preserved), this records
                                 // the actual value type so struct promotion path can avoid
                                 // xrt_box_float/xrt_unbox_float calls.
    uint16_t    original_offset; // Byte offset in original Json layout
    uint32_t    symbol_id;       // SymbolId for field name resolution
} XcgenStructField;

// Promotable struct descriptor
typedef struct {
    const char *c_name;          // C struct type name (e.g. "xr_Point")
    void       *shape_ptr;      // Original XrShape* (for matching NEWJSON constants)
    XcgenStructField fields[XCGEN_MAX_STRUCT_FIELDS];
    int         field_count;
    int         total_size;      // C struct byte size
    bool        all_native;      // true = all fields are native types (no XrtValue)
} XcgenStruct;

// Flat entry for symbol_id → struct/field lookup (sorted by symbol_id)
typedef struct {
    uint32_t symbol_id;
    uint8_t  struct_idx;
    uint8_t  field_idx;
} XcgenFieldEntry;

// Module-level struct registry
typedef struct {
    XcgenStruct    structs[XCGEN_MAX_STRUCTS];
    int            nstructs;
    // Sorted lookup table: symbol_id → (struct_idx, field_idx)
    XcgenFieldEntry field_entries[XCGEN_MAX_FIELD_ENTRIES];
    int             nfield_entries;
} XcgenStructRegistry;

// Forward declarations for opaque types used in analysis
struct XrProto;
struct XrShape;

// Initialize registry
XR_FUNC void xcgen_struct_registry_init(XcgenStructRegistry *reg);

// Collect promotable shapes from a proto's bytecode
// isolate is needed for SymbolId → field name resolution
XR_FUNC void xcgen_collect_shapes(struct XrProto *proto, XcgenStructRegistry *reg, void *isolate);

// Check if a shape is promotable to C struct
XR_FUNC bool xcgen_shape_promotable(struct XrShape *shape);

// Find struct index by shape pointer, returns -1 if not found
XR_FUNC int xcgen_find_struct(XcgenStructRegistry *reg, void *shape_ptr);

// Find field index by original byte offset, returns -1 if not found
XR_FUNC int xcgen_field_by_offset(XcgenStruct *st, int64_t byte_offset);

// Rebuild sorted symbol_id lookup table (call after all xcgen_collect_shapes)
XR_FUNC void xcgen_rebuild_field_index(XcgenStructRegistry *reg);

// O(log n) lookup: find struct/field by symbol_id, returns NULL if not found
XR_FUNC const XcgenFieldEntry *xcgen_find_field_by_symbol(
        const XcgenStructRegistry *reg, uint32_t symbol_id);

// Emit C typedef for a struct into buffer
struct XcgenBuf;
XR_FUNC void xcgen_emit_struct_typedef(struct XcgenBuf *b, XcgenStruct *st);

// Emit all struct typedefs in registry
XR_FUNC void xcgen_emit_all_typedefs(struct XcgenBuf *b, XcgenStructRegistry *reg);

// Emit per-struct deinit functions and function pointer table.
// Generated after typedefs so struct types are visible.
// Only emitted when at least one struct has PTR/TAGGED (XrtValue) fields.
XR_FUNC void xcgen_emit_struct_deinits(struct XcgenBuf *b, XcgenStructRegistry *reg);

#endif // XCGEN_STRUCT_H
