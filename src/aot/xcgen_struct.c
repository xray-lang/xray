/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcgen_struct.c - Json → C struct promotion for AOT
 *
 * KEY CONCEPT:
 *   Scans bytecode for OP_NEWJSON instructions, extracts Shape metadata,
 *   infers field types from JSON_INIT sequences, and registers promotable
 *   structs for C code generation.
 *
 * RELATED MODULES:
 *   - xcgen_call.c: intercepts CALL_C(xr_json_new_with_shape) for promotion
 *   - xcgen_expr.c: LOAD_FIELD/STORE_FIELD use struct field access
 */

#include "xcgen_struct.h"
#include "../base/xchecks.h"
#include "xcgen.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xtype.h"
#include "../runtime/object/xshape.h"
#include "../jit/xir_offsets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/symbol/xsymbol_table.h"

/* ========== Registry Init ========== */

void xcgen_struct_registry_init(XcgenStructRegistry *reg) {
    XR_DCHECK(reg != NULL, "xcgen_struct_registry_init: NULL reg");
    memset(reg, 0, sizeof(*reg));
}

/* ========== Symbol-ID Lookup Table ========== */

static int cmp_field_entry(const void *a, const void *b) {
    const XcgenFieldEntry *ea = (const XcgenFieldEntry *) a;
    const XcgenFieldEntry *eb = (const XcgenFieldEntry *) b;
    if (ea->symbol_id < eb->symbol_id)
        return -1;
    if (ea->symbol_id > eb->symbol_id)
        return 1;
    return 0;
}

// Rebuild sorted field_entries after new structs are added
static void rebuild_field_entries(XcgenStructRegistry *reg) {
    reg->nfield_entries = 0;
    for (int si = 0; si < reg->nstructs; si++) {
        XcgenStruct *st = &reg->structs[si];
        for (int fi = 0; fi < st->field_count; fi++) {
            if (reg->nfield_entries >= XCGEN_MAX_FIELD_ENTRIES)
                break;
            XcgenFieldEntry *e = &reg->field_entries[reg->nfield_entries++];
            e->symbol_id = st->fields[fi].symbol_id;
            e->struct_idx = (uint8_t) si;
            e->field_idx = (uint8_t) fi;
        }
    }
    qsort(reg->field_entries, reg->nfield_entries, sizeof(XcgenFieldEntry), cmp_field_entry);
}

void xcgen_rebuild_field_index(XcgenStructRegistry *reg) {
    if (reg)
        rebuild_field_entries(reg);
}

const XcgenFieldEntry *xcgen_find_field_by_symbol(const XcgenStructRegistry *reg,
                                                  uint32_t symbol_id) {
    if (!reg || reg->nfield_entries == 0)
        return NULL;
    int lo = 0, hi = reg->nfield_entries - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t mid_sym = reg->field_entries[mid].symbol_id;
        if (mid_sym == symbol_id)
            return &reg->field_entries[mid];
        if (mid_sym < symbol_id)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* ========== Promotability Check ========== */

bool xcgen_shape_promotable(XrShape *shape) {
    if (!shape)
        return false;
    // Must have at least one field
    if (shape->field_count == 0)
        return false;
    // Too many fields
    if (shape->field_count > XCGEN_MAX_STRUCT_FIELDS)
        return false;
    return true;
}

/* ========== Registry Lookup ========== */

int xcgen_find_struct(XcgenStructRegistry *reg, void *shape_ptr) {
    if (!reg || !shape_ptr)
        return -1;
    for (int i = 0; i < reg->nstructs; i++) {
        if (reg->structs[i].shape_ptr == shape_ptr)
            return i;
    }
    return -1;
}

int xcgen_field_by_offset(XcgenStruct *st, int64_t byte_offset) {
    if (!st)
        return -1;
    for (int i = 0; i < st->field_count; i++) {
        if (st->fields[i].original_offset == (uint16_t) byte_offset)
            return i;
    }
    return -1;
}

/* ========== Field Type Inference ========== */

// Backward scan: look at the instruction that produced value in register rc
// to infer its type (e.g. LOADF → F64, LOADI → I64)
static uint8_t infer_from_backward_scan(XrProto *proto, uint32_t from_pc, int rc) {
    // Fast path: check param_types for declared parameter type
    if (proto->param_types && rc >= 0 && rc < proto->param_types_count && proto->param_types[rc]) {
        uint8_t gc = xr_type_to_slot_type(proto->param_types[rc]);
        if (gc == XR_SLOT_F64)
            return XR_REP_F64;
        if (gc == XR_SLOT_I64)
            return XR_REP_I64;
    }

    for (int32_t pc = (int32_t) from_pc - 1; pc >= 0; pc--) {
        XrInstruction inst = PROTO_CODE(proto, (uint32_t) pc);
        OpCode op = GET_OPCODE(inst);
        int a = GETARG_A(inst);
        if (a != rc)
            continue;

        switch (op) {
            case OP_LOADF:
            case OP_MULK:
            case OP_DIVK:
            case OP_ADDK:
            case OP_SUBK:
                return XR_REP_F64;
            case OP_LOADI:
            case OP_ADDI:
            case OP_SUBI:
            case OP_MULI:
                return XR_REP_I64;
            case OP_LOADK: {
                int bx = GETARG_Bx(inst);
                if (bx >= 0 && bx < (int) VALUEARRAY_COUNT(&proto->constants)) {
                    XrValue cv = VALUEARRAY_GET(&proto->constants, (uint32_t) bx);
                    if (XR_IS_FLOAT(cv))
                        return XR_REP_F64;
                    if (XR_IS_INT(cv))
                        return XR_REP_I64;
                }
                return XR_REP_TAGGED;
            }
            case OP_ADD:
            case OP_SUB:
            case OP_MUL:
            case OP_DIV:
            case OP_MOD: {
                // Use inst_types[pc] for arithmetic result type
                if (proto->inst_types && (uint32_t) pc < proto->inst_types_count &&
                    proto->inst_types[pc]) {
                    uint8_t gc = xr_type_to_slot_type(proto->inst_types[pc]);
                    if (gc == XR_SLOT_F64)
                        return XR_REP_F64;
                    if (gc == XR_SLOT_I64)
                        return XR_REP_I64;
                }
                return XR_REP_TAGGED;
            }
            case OP_MOVE: {
                // Use inst_types for MOVE source type, or recurse
                int src = GETARG_B(inst);
                return infer_from_backward_scan(proto, (uint32_t) pc, src);
            }
            default:
                break;
        }
    }
    return XR_REP_TAGGED;
}

// Infer field type from JSON_INIT instruction sequence after NEWJSON.
// Scans forward from newjson_pc until control flow break or end of function.
static uint8_t infer_field_type(XrProto *proto, uint32_t newjson_pc, int field_idx) {
    uint32_t code_count = (uint32_t) proto->code.count;
    int a_reg = GETARG_A(PROTO_CODE(proto, newjson_pc));

    for (uint32_t pc = newjson_pc + 1; pc < code_count; pc++) {
        XrInstruction inst = PROTO_CODE(proto, pc);
        OpCode op = GET_OPCODE(inst);

        if (op == OP_JSON_INIT_I && GETARG_A(inst) == a_reg && GETARG_B(inst) == field_idx)
            return XR_REP_I64;  // immediate integer init

        if (op == OP_JSON_INIT_N && GETARG_A(inst) == a_reg && GETARG_B(inst) == field_idx)
            return XR_REP_TAGGED;  // null → keep XrtValue

        if (op == OP_JSON_INIT && GETARG_A(inst) == a_reg && GETARG_B(inst) == field_idx) {
            int rc = GETARG_C(inst);
            // Backward scan from JSON_INIT position to find the NEAREST def of rc.
            // This will check param_types, inst_types, and opcode-based inference.
            return infer_from_backward_scan(proto, pc, rc);
        }

        // If we hit another NEWJSON or a control flow instruction, stop scanning
        if (op == OP_NEWJSON || op == OP_JMP || op == OP_RETURN || op == OP_RETURN0 ||
            op == OP_RETURN1)
            break;
    }
    return XR_REP_TAGGED;  // default: keep as XrtValue
}

// Map XIR type to C type index: 0=int64_t, 1=double, 2=XrtValue
static uint8_t xir_type_to_c_type(uint8_t xir_type) {
    switch (xir_type) {
        case XR_REP_I64:
            return 0;
        case XR_REP_F64:
            return 1;
        default:
            return 2;
    }
}

// Map C type index to byte size
static int c_type_size(uint8_t c_type) {
    switch (c_type) {
        case 0:
            return 8;  // int64_t
        case 1:
            return 8;  // double
        case 2:
            return 16;  // XrtValue
        default:
            return 8;
    }
}

/* ========== C-safe Name Generation ========== */

// Make a C-safe identifier from a symbol name
// Replaces non-alphanumeric chars with '_'
static void make_c_safe_name(char *dst, size_t dst_size, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < dst_size; i++) {
        char c = src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_') {
            dst[i] = c;
        } else {
            dst[i] = '_';
        }
    }
    dst[i] = '\0';
}

/* ========== Register a Struct ========== */

static bool xcgen_register_struct(XcgenStructRegistry *reg, XrShape *shape, void *isolate) {
    if (reg->nstructs >= XCGEN_MAX_STRUCTS)
        return false;

    XcgenStruct *st = &reg->structs[reg->nstructs];
    memset(st, 0, sizeof(*st));
    st->shape_ptr = (void *) shape;
    st->field_count = shape->field_count;
    st->all_native = true;

    // Resolve field names from SymbolId
    for (int i = 0; i < shape->field_count; i++) {
        SymbolId sym = shape->field_symbols[i];
        const char *name = xr_symbol_get_name_by_id(isolate, (int) sym);

        // Allocate a copy of the C-safe name
        char safe_name[64];
        if (name) {
            make_c_safe_name(safe_name, sizeof(safe_name), name);
        } else {
            snprintf(safe_name, sizeof(safe_name), "f%d", i);
        }
        st->fields[i].name = strdup(safe_name);
        st->fields[i].symbol_id = (uint32_t) sym;

        // Default type: will be refined by infer_field_type later
        st->fields[i].xir_type = XR_REP_TAGGED;
        st->fields[i].c_type = 2;                     // XrtValue (layout preserved)
        st->fields[i].val_hint_type = XR_REP_TAGGED;  // semantic hint, set later

        // Original byte offset in Json layout
        st->fields[i].original_offset = (uint16_t) (XIR_JSON_FIELDS_OFFSET + i * XIR_XRVALUE_SIZE);
    }

    // Generate C struct type name from shape id
    // Will be overridden if we find a type alias name
    char name_buf[64];
    snprintf(name_buf, sizeof(name_buf), "xrs_%u", shape->id);
    st->c_name = strdup(name_buf);

    reg->nstructs++;
    return true;
}

/* ========== Collect Shapes from Proto ========== */

void xcgen_collect_shapes(XrProto *proto, XcgenStructRegistry *reg, void *isolate) {
    if (!proto || !reg)
        return;

    uint32_t code_count = (uint32_t) proto->code.count;
    for (uint32_t pc = 0; pc < code_count; pc++) {
        XrInstruction inst = PROTO_CODE(proto, pc);
        if (GET_OPCODE(inst) != OP_NEWJSON)
            continue;

        int storage_mode = GETARG_C(inst);
        if (storage_mode != 0)
            continue;  // only normal mode

        int bx = GETARG_B(inst);
        XrValue shape_val = PROTO_CONST_FAST(proto, bx);
        XrShape *shape = (XrShape *) (intptr_t) XR_TO_INT(shape_val);

        // Already registered?
        if (xcgen_find_struct(reg, (void *) shape) >= 0)
            continue;

        // Check promotability
        if (!xcgen_shape_promotable(shape))
            continue;

        // Register the struct
        if (!xcgen_register_struct(reg, shape, isolate))
            continue;

        int si = reg->nstructs - 1;
        XcgenStruct *st = &reg->structs[si];

        // Infer field types: prefer shape->field_types (compact type info)
        // which is populated from 'type T = { field: float, ... }' declarations.
        //
        // Two outcomes are recorded per field:
        //   xir_type / c_type: actual C struct storage type (XrtValue layout preserved
        //     unless the shape explicitly declares a compact type). This keeps
        //     field offsets consistent with the fallback path (16B per slot).
        //   val_hint_type: semantic type inferred from bytecode (e.g. F64 from LOADK
        //     of a float constant). Used to avoid xrt_box/unbox in struct promotion
        //     path without changing the C struct layout.
        for (int fi = 0; fi < st->field_count; fi++) {
            uint8_t xir_type = XR_REP_TAGGED;
            if (shape->field_types && fi < shape->field_count) {
                XrCompactType ct = shape->field_types[fi];
                if (ct == XR_COMPACT_FLOAT64 || ct == XR_COMPACT_FLOAT32) {
                    xir_type = XR_REP_F64;
                } else if (ct == XR_COMPACT_INT32 || ct == XR_COMPACT_INT64 ||
                           ct == XR_COMPACT_BOOL || ct == XR_COMPACT_UINT8 ||
                           ct == XR_COMPACT_UINT16 || ct == XR_COMPACT_UINT32) {
                    xir_type = XR_REP_I64;
                } else {
                    xir_type = infer_field_type(proto, pc, fi);
                }
            } else {
                xir_type = infer_field_type(proto, pc, fi);
            }
            st->fields[fi].xir_type = xir_type;
            st->fields[fi].c_type = xir_type_to_c_type(xir_type);
            // val_hint_type: separate inference pass that also checks OP_LOADK.
            // Checks constant pool to learn float vs int from K[] constants.
            // This does NOT affect c_type or struct layout.
            uint8_t hint = xir_type;
            if (hint == XR_REP_TAGGED) {
                // Try OP_LOADK-aware scan for better semantic type hint
                uint32_t code_count = (uint32_t) proto->code.count;
                int a_reg = GETARG_A(PROTO_CODE(proto, pc));
                for (uint32_t hpc = pc + 1; hpc < code_count; hpc++) {
                    XrInstruction hinst = PROTO_CODE(proto, hpc);
                    OpCode hop = GET_OPCODE(hinst);
                    if (hop == OP_NEWJSON || hop == OP_JMP || hop == OP_RETURN ||
                        hop == OP_RETURN0 || hop == OP_RETURN1)
                        break;
                    if (hop == OP_JSON_INIT && GETARG_A(hinst) == a_reg && GETARG_B(hinst) == fi) {
                        int rc = GETARG_C(hinst);
                        // Try LOADK scan for this register
                        for (int32_t hpc2 = (int32_t) hpc - 1; hpc2 >= 0; hpc2--) {
                            XrInstruction h2 = PROTO_CODE(proto, (uint32_t) hpc2);
                            if (GETARG_A(h2) != rc)
                                continue;
                            if (GET_OPCODE(h2) == OP_LOADK) {
                                int bx = GETARG_Bx(h2);
                                if (bx >= 0 && bx < (int) VALUEARRAY_COUNT(&proto->constants)) {
                                    XrValue cv = VALUEARRAY_GET(&proto->constants, (uint32_t) bx);
                                    if (XR_IS_FLOAT(cv))
                                        hint = XR_REP_F64;
                                    else if (XR_IS_INT(cv))
                                        hint = XR_REP_I64;
                                }
                            }
                            break;
                        }
                        break;
                    }
                }
            }
            st->fields[fi].val_hint_type = hint;
            if (st->fields[fi].c_type == 2) {
                st->all_native = false;
            }
        }

        // Compute total struct size
        int total = 0;
        for (int fi = 0; fi < st->field_count; fi++) {
            total += c_type_size(st->fields[fi].c_type);
        }
        st->total_size = total;

        printf("  [struct] Shape %u → %s (%d fields, %d bytes, %s)\n", shape->id, st->c_name,
               st->field_count, st->total_size, st->all_native ? "all-native" : "mixed");
    }

    // Also scan child protos
    for (int i = 0; i < proto->protos.count; i++) {
        XrProto *child = *(XrProto **) xr_dynarray_get_raw(&proto->protos, i);
        xcgen_collect_shapes(child, reg, isolate);
    }
}

/* ========== Typedef Generation ========== */

void xcgen_emit_struct_typedef(XcgenBuf *b, XcgenStruct *st) {
    if (!b || !st)
        return;
    const char *tagged = "XrValue";
    xcgen_buf_printf(b, "typedef struct {\n");
    for (int i = 0; i < st->field_count; i++) {
        const char *ctype;
        switch (st->fields[i].c_type) {
            case 0:
                ctype = "int64_t";
                break;
            case 1:
                ctype = "double";
                break;
            case 2:
                ctype = tagged;
                break;
            default:
                ctype = "int64_t";
                break;
        }
        xcgen_buf_printf(b, "    %s %s;\n", ctype, st->fields[i].name);
    }
    xcgen_buf_printf(b, "} %s;\n\n", st->c_name);
}

void xcgen_emit_all_typedefs(XcgenBuf *b, XcgenStructRegistry *reg) {
    if (!b || !reg || reg->nstructs == 0)
        return;
    xcgen_buf_puts(b, "/* === Promoted Json structs === */\n\n");
    for (int i = 0; i < reg->nstructs; i++) {
        xcgen_emit_struct_typedef(b, &reg->structs[i]);
    }
}
