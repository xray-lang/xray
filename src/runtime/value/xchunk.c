/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xchunk.c - Bytecode chunk implementation
 *
 * KEY CONCEPT:
 *   Manages function prototypes (XrProto) and constant pools.
 *   Provides bytecode emission and constant deduplication.
 */

#include "xchunk.h"
#include "../object/xstring.h"
#include "../../base/xmalloc.h"
#include "xtype_feedback.h"
#include "../gc/xbc_stackmap.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "xopcode_info.h"
#include "../../base/xchecks.h"

void xr_ic_method_table_free(struct XrICMethodTable *table);
void xr_ic_field_table_free(struct XrICFieldTable *table);
void xr_blueprint_free(struct XrBlueprint *bp);

// xr_opcode_name is implemented in runtime/value/xopcode_info.c.

// ========== Constant Table Operations ==========

// Initialize constant array
void xr_valuearray_init(ValueArray *array) {
    XR_DCHECK(array != NULL, "valuearray_init: NULL array");
    DYNARRAY_INIT(array, XrValue);
}

// Free constant array
void xr_valuearray_free(ValueArray *array) {
    XR_DCHECK(array != NULL, "valuearray_free: NULL array");
    DYNARRAY_FREE(array);
}

// Add constant to array
// Returns constant index
// Auto-dedup: if constant exists, return existing index
int xr_valuearray_add(ValueArray *array, XrValue value) {
    XR_DCHECK(array != NULL, "valuearray_add: NULL array");
    // Check if same constant already exists (dedup)
    // Require both value equality AND type identity (int vs float must be separate)
    int count = DYNARRAY_COUNT(array);
    for (int i = 0; i < count; i++) {
        XrValue existing = DYNARRAY_GET(array, i, XrValue);
        if (XR_IS_INT(existing) != XR_IS_INT(value)) continue;
        if (XR_IS_FLOAT(existing) != XR_IS_FLOAT(value)) continue;
        if (xr_value_deep_eq(existing, value)) {
            return i;  // Found duplicate with same type, return existing index
        }
    }

    // Add new constant
    return DYNARRAY_ADD(array, value, XrValue);
}

// ========== XrProto Operations ==========

// Create new function prototype
XrProto *xr_vm_proto_new(void) {
    XrProto *proto = (XrProto *)xr_calloc(1, sizeof(XrProto));
    if (proto == NULL) {
        return NULL;
    }

    // NOTE: All scalar fields are zero-initialized by xr_calloc.
    // Only containers that require explicit init are called below.
    DYNARRAY_INIT(&proto->code, XrInstruction);
    xr_valuearray_init(&proto->constants);
    DYNARRAY_INIT(&proto->protos, XrProto*);
    DYNARRAY_INIT(&proto->upvalues, UpvalInfo);
    DYNARRAY_INIT(&proto->lineinfo, int);
    DYNARRAY_INIT(&proto->locvars, XrLocVar);

    return proto;
}

// Free function prototype
void xr_vm_proto_free(XrProto *proto) {
    if (proto == NULL) {
        return;
    }

    // Free nested functions (recursive)
    int proto_count = DYNARRAY_COUNT(&proto->protos);
    for (int i = 0; i < proto_count; i++) {
        XrProto *child = DYNARRAY_GET(&proto->protos, i, XrProto*);
        xr_vm_proto_free(child);
    }

    // Free all dynamic arrays
    DYNARRAY_FREE(&proto->code);
    xr_valuearray_free(&proto->constants);
    DYNARRAY_FREE(&proto->protos);
    DYNARRAY_FREE(&proto->upvalues);
    DYNARRAY_FREE(&proto->lineinfo);
    DYNARRAY_FREE(&proto->locvars);

    // Free return type string
    if (proto->return_type != NULL) {
        xr_free(proto->return_type);
        proto->return_type = NULL;
    }

    // Free per-function symbol table
    if (proto->symbols != NULL) {
        xr_free(proto->symbols);
        proto->symbols = NULL;
    }

    // Free raw constant pool
    if (proto->raw_constants != NULL) {
        xr_free(proto->raw_constants);
        proto->raw_constants = NULL;
    }

    // Free JIT/AOT metadata
    if (proto->bb_leaders != NULL) {
        xr_free(proto->bb_leaders);
        proto->bb_leaders = NULL;
    }
    if (proto->loop_headers != NULL) {
        xr_free(proto->loop_headers);
        proto->loop_headers = NULL;
    }

    // Free type pipeline
    if (proto->param_types != NULL) {
        xr_free(proto->param_types);
        proto->param_types = NULL;
    }
    if (proto->inst_types != NULL) {
        xr_free(proto->inst_types);
        proto->inst_types = NULL;
    }
    // return_type_info points into analyzer_pool arena, do not free

    // Free Blueprint (compiler-generated JIT metadata)
    if (proto->blueprint != NULL) {
        xr_blueprint_free((struct XrBlueprint *)proto->blueprint);
        proto->blueprint = NULL;
    }

    // Free type feedback
    if (proto->type_feedback != NULL) {
        xfb_destroy(proto->type_feedback);
        proto->type_feedback = NULL;
    }

    // Free inline caches
    if (proto->ic_methods != NULL) {
        xr_ic_method_table_free(proto->ic_methods);
        proto->ic_methods = NULL;
    }
    if (proto->ic_fields != NULL) {
        xr_ic_field_table_free(proto->ic_fields);
        proto->ic_fields = NULL;
    }

    // Free struct layout cache (pointer array only; layouts are owned by XrClass)
    if (proto->struct_layouts != NULL) {
        xr_free(proto->struct_layouts);
        proto->struct_layouts = NULL;
    }

    // Free JIT runtime allocations (allocated via xr_malloc in jit pipeline)
    if (proto->osr_entries != NULL) {
        xr_free(proto->osr_entries);
        proto->osr_entries = NULL;
    }
    if (proto->deopt_table != NULL) {
        xr_free(proto->deopt_table);
        proto->deopt_table = NULL;
    }

    // Free bytecode stack map (precise GC for interpreter)
    if (proto->bc_stackmap != NULL) {
        xr_bc_stackmap_destroy((XrBcStackMap *)proto->bc_stackmap);
        proto->bc_stackmap = NULL;
    }

    // Free XrProto itself
    xr_free(proto);
}

// Add a global symbol to the per-function symbol table.
// Returns local index (0-based). Deduplicates: if the same global symbol
// was already added, returns the existing local index.
int xr_proto_add_symbol(XrProto *proto, int32_t global_symbol) {
    XR_DCHECK(proto != NULL, "proto_add_symbol: NULL proto");
    XR_DCHECK(global_symbol >= 0, "proto_add_symbol: negative symbol id");
    XR_DCHECK(proto->symbol_count >= 0, "proto_add_symbol: negative count");
    XR_DCHECK(proto->symbol_capacity >= 0, "proto_add_symbol: negative capacity");
    XR_DCHECK(proto->symbol_count <= proto->symbol_capacity,
              "proto_add_symbol: count > capacity");
    // Dedup: check if already registered
    for (int i = 0; i < proto->symbol_count; i++) {
        if (proto->symbols[i] == global_symbol) {
            return i;
        }
    }

    // Grow if needed
    if (proto->symbol_count >= proto->symbol_capacity) {
        int new_cap = proto->symbol_capacity < 8 ? 8 : proto->symbol_capacity * 2;
        int32_t *new_buf = (int32_t *)xr_realloc(proto->symbols, new_cap * sizeof(int32_t));
        if (!new_buf) {
            fprintf(stderr, "xr_proto_add_symbol: out of memory\n");
            return 0;
        }
        proto->symbols = new_buf;
        proto->symbol_capacity = new_cap;
    }

    int local_idx = proto->symbol_count;
    proto->symbols[local_idx] = global_symbol;
    proto->symbol_count++;

    XR_CHECK(local_idx < 255,
             "proto: too many unique symbols (>254), function too complex");

    return local_idx;
}

// Write one instruction
void xr_vm_proto_write(XrProto *proto, XrInstruction inst, int line) {
    XR_DCHECK(proto != NULL, "proto_write: NULL proto");
    XR_DCHECK(line >= 0, "proto_write: negative line number");
    // Add instruction
    DYNARRAY_ADD(&proto->code, inst, XrInstruction);

    // Record line number
    DYNARRAY_ADD(&proto->lineinfo, line, int);
}

// Add constant to constant pool
// Returns constant index
int xr_vm_proto_add_constant(XrProto *proto, XrValue value) {
    XR_DCHECK(proto != NULL, "proto_add_constant: NULL proto");
    return xr_valuearray_add(&proto->constants, value);
}

// Add raw 64-bit constant to raw constant pool
// Returns constant index
int xr_proto_add_raw_constant(XrProto *proto, uint64_t value) {
    XR_DCHECK(proto != NULL, "proto_add_raw_constant: NULL proto");
    XR_DCHECK(proto->raw_constant_count >= 0,
              "proto_add_raw_constant: negative count");
    XR_DCHECK(proto->raw_constant_capacity >= 0,
              "proto_add_raw_constant: negative capacity");
    XR_DCHECK(proto->raw_constant_count <= proto->raw_constant_capacity,
              "proto_add_raw_constant: count > capacity");
    // Dedup check
    for (int i = 0; i < proto->raw_constant_count; i++) {
        if (proto->raw_constants[i] == value) return i;
    }

    // Grow if needed
    if (proto->raw_constant_count >= proto->raw_constant_capacity) {
        int new_cap = proto->raw_constant_capacity < 8 ? 8 : proto->raw_constant_capacity * 2;
        uint64_t *new_buf = (uint64_t *)xr_realloc(proto->raw_constants, new_cap * sizeof(uint64_t));
        if (!new_buf) return 0;
        proto->raw_constants = new_buf;
        proto->raw_constant_capacity = new_cap;
    }

    int idx = proto->raw_constant_count;
    proto->raw_constants[idx] = value;
    proto->raw_constant_count++;
    return idx;
}

// Add nested function prototype
// Returns prototype index
int xr_vm_proto_add_proto(XrProto *proto, XrProto *child) {
    XR_DCHECK(proto != NULL, "proto_add_proto: NULL proto");
    XR_DCHECK(child != NULL, "proto_add_proto: NULL child");
    child->enclosing = proto;
    return DYNARRAY_ADD(&proto->protos, child, XrProto*);
}

// Add upvalue info
// Returns upvalue index
int xr_vm_proto_add_upvalue(XrProto *proto, uint8_t index, uint8_t storage_mode, uint8_t is_const, uint8_t slot_type, uint8_t source, struct XrType *type_info) {
    XR_DCHECK(proto != NULL, "proto_add_upvalue: NULL proto");
    // No dedup here: dedup is done at compiler level in scope_add_upvalue.
    // proto->upvalues must stay in 1-to-1 correspondence with XrCompiler->upvalues[].

    // Add new upvalue
    UpvalInfo new_uv = {
        .index = index,
        .storage_mode = storage_mode,
        .is_const = is_const,
        .slot_type = slot_type,
        .source = source,
        .type_info = type_info
    };
    return DYNARRAY_ADD(&proto->upvalues, new_uv, UpvalInfo);
}

