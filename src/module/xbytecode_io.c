/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbytecode_io.c - Bytecode serialization/deserialization implementation
 *
 * KEY CONCEPT:
 *   Serializes XrProto to portable bytecode format (.xrc) and loads it back.
 *   Handles symbol table remapping for cross-compilation compatibility.
 */

#include "xbytecode_io.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"
#include "xray_isolate.h"
#include "../base/xmalloc.h"
#include "../runtime/xisolate_api.h"
#include "xexec_state.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xtype.h"
#include "../runtime/object/xstring.h"
#include "../runtime/value/xvalue.h"
#include "../base/xdynarray.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xparse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/symbol/xsymbol_table.h"

/* ========== Writer Helper ========== */

typedef struct {
    uint8_t *buf;
    size_t size;
    size_t capacity;
    XrayIsolate *X;
    int flags;
} BcWriter;

static void bc_writer_init(BcWriter *w, XrayIsolate *X, int flags) {
    XR_DCHECK(w != NULL, "bc_writer_init: NULL writer");
    w->buf = NULL;
    w->size = 0;
    w->capacity = 0;
    w->X = X;
    w->flags = flags;
}

static bool bc_writer_ensure(BcWriter *w, size_t need) {
    if (w->size + need <= w->capacity) return true;

    size_t new_cap = w->capacity ? w->capacity * 2 : 4096;
    while (new_cap < w->size + need) new_cap *= 2;

    uint8_t *new_buf = xr_realloc(w->buf, new_cap);
    if (!new_buf) return false;

    w->buf = new_buf;
    w->capacity = new_cap;
    XR_DCHECK(w->size + need <= w->capacity, "bc_writer_ensure: capacity still insufficient");
    return true;
}

static bool bc_put_u8(BcWriter *w, uint8_t v) {
    if (!bc_writer_ensure(w, 1)) return false;
    w->buf[w->size++] = v;
    return true;
}

static bool bc_put_u16(BcWriter *w, uint16_t v) {
    if (!bc_writer_ensure(w, 2)) return false;
    w->buf[w->size++] = v & 0xFF;
    w->buf[w->size++] = (v >> 8) & 0xFF;
    return true;
}

static bool bc_put_u32(BcWriter *w, uint32_t v) {
    if (!bc_writer_ensure(w, 4)) return false;
    w->buf[w->size++] = v & 0xFF;
    w->buf[w->size++] = (v >> 8) & 0xFF;
    w->buf[w->size++] = (v >> 16) & 0xFF;
    w->buf[w->size++] = (v >> 24) & 0xFF;
    return true;
}

static bool bc_put_i64(BcWriter *w, int64_t v) {
    if (!bc_writer_ensure(w, 8)) return false;
    for (int i = 0; i < 8; i++) {
        w->buf[w->size++] = (v >> (i * 8)) & 0xFF;
    }
    return true;
}

static bool bc_put_f64(BcWriter *w, double v) {
    union { double d; uint64_t u; } u;
    u.d = v;
    return bc_put_i64(w, (int64_t)u.u);
}

static bool bc_put_bytes(BcWriter *w, const void *data, size_t len) {
    if (!bc_writer_ensure(w, len)) return false;
    memcpy(w->buf + w->size, data, len);
    w->size += len;
    return true;
}

static bool bc_put_string(BcWriter *w, const char *str) {
    uint32_t len = str ? (uint32_t)strlen(str) : 0;
    if (!bc_put_u32(w, len)) return false;
    if (len > 0 && !bc_put_bytes(w, str, len)) return false;
    return true;
}

/* ========== Reader Helper ========== */

typedef struct {
    const uint8_t *buf;
    size_t size;
    size_t pos;
    XrayIsolate *X;
    XrBcError error;
} BcReader;

static void bc_reader_init(BcReader *r, XrayIsolate *X, const uint8_t *buf, size_t size) {
    XR_DCHECK(r != NULL, "bc_reader_init: NULL reader");
    XR_DCHECK(buf != NULL, "bc_reader_init: NULL buf");
    XR_DCHECK(size > 0, "bc_reader_init: zero size");
    r->buf = buf;
    r->size = size;
    r->pos = 0;
    r->X = X;
    r->error = XR_BC_OK;
}

static bool bc_has_bytes(BcReader *r, size_t n) {
    return r->pos + n <= r->size;
}

static uint8_t bc_get_u8(BcReader *r) {
    if (!bc_has_bytes(r, 1)) { r->error = XR_BC_ERR_TRUNCATED; return 0; }
    return r->buf[r->pos++];
}

static uint16_t bc_get_u16(BcReader *r) {
    if (!bc_has_bytes(r, 2)) { r->error = XR_BC_ERR_TRUNCATED; return 0; }
    uint16_t v = r->buf[r->pos] | (r->buf[r->pos + 1] << 8);
    r->pos += 2;
    return v;
}

static uint32_t bc_get_u32(BcReader *r) {
    if (!bc_has_bytes(r, 4)) { r->error = XR_BC_ERR_TRUNCATED; return 0; }
    uint32_t v = r->buf[r->pos] | (r->buf[r->pos + 1] << 8) |
                 (r->buf[r->pos + 2] << 16) | (r->buf[r->pos + 3] << 24);
    r->pos += 4;
    return v;
}

static int64_t bc_get_i64(BcReader *r) {
    if (!bc_has_bytes(r, 8)) { r->error = XR_BC_ERR_TRUNCATED; return 0; }
    int64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((int64_t)r->buf[r->pos++]) << (i * 8);
    }
    return v;
}

static double bc_get_f64(BcReader *r) {
    union { double d; int64_t i; } u;
    u.i = bc_get_i64(r);
    return u.d;
}

static char* bc_get_string(BcReader *r) {
    uint32_t len = bc_get_u32(r);
    if (r->error != XR_BC_OK) return NULL;
    if (len == 0) return NULL;

    if (!bc_has_bytes(r, len)) { r->error = XR_BC_ERR_TRUNCATED; return NULL; }

    char *str = xr_malloc(len + 1);
    if (!str) { r->error = XR_BC_ERR_ALLOC; return NULL; }

    memcpy(str, r->buf + r->pos, len);
    str[len] = '\0';
    r->pos += len;
    return str;
}

/* ========== Value Serialization ========== */

// Value type tags
#define BC_VAL_NULL     0
#define BC_VAL_BOOL     1
#define BC_VAL_INT      2
#define BC_VAL_FLOAT    3
#define BC_VAL_STRING   4

static bool bc_write_value(BcWriter *w, XrValue val) {
    if (XR_IS_NULL(val)) {
        return bc_put_u8(w, BC_VAL_NULL);
    } else if (XR_IS_BOOL(val)) {
        if (!bc_put_u8(w, BC_VAL_BOOL)) return false;
        return bc_put_u8(w, XR_TO_BOOL(val) ? 1 : 0);
    } else if (XR_IS_INT(val)) {
        if (!bc_put_u8(w, BC_VAL_INT)) return false;
        return bc_put_i64(w, XR_TO_INT(val));
    } else if (XR_IS_FLOAT(val)) {
        if (!bc_put_u8(w, BC_VAL_FLOAT)) return false;
        return bc_put_f64(w, XR_TO_FLOAT(val));
    } else if (XR_IS_STRING(val)) {
        if (!bc_put_u8(w, BC_VAL_STRING)) return false;
        XrString *s = XR_TO_STRING(val);
        return bc_put_string(w, s->data);
    }
    // Other types not supported yet
    return bc_put_u8(w, BC_VAL_NULL);
}

static XrValue bc_read_value(BcReader *r) {
    uint8_t type = bc_get_u8(r);
    if (r->error != XR_BC_OK) return xr_null();

    switch (type) {
        case BC_VAL_NULL:
            return xr_null();
        case BC_VAL_BOOL:
            return xr_bool(bc_get_u8(r) != 0);
        case BC_VAL_INT:
            return xr_int(bc_get_i64(r));
        case BC_VAL_FLOAT:
            return xr_float(bc_get_f64(r));
        case BC_VAL_STRING: {
            char *str = bc_get_string(r);
            if (!str) return xr_null();
            XrString *s = xr_string_intern(r->X, str, strlen(str), 0);
            xr_free(str);
            return s ? xr_string_value(s) : xr_null();
        }
        default:
            r->error = XR_BC_ERR_CORRUPT;
            return xr_null();
    }
}

/* ========== Symbol Table Serialization ========== */

// Collect global symbol IDs from per-function symbol table, returns max_symbol_id + 1
static int collect_symbols_from_proto(XrProto *proto, int max_symbol) {
    if (!proto) return max_symbol;

    // Scan per-function symbol table (no need to scan instructions)
    for (int i = 0; i < proto->symbol_count; i++) {
        int32_t sym = proto->symbols[i];
        if (sym >= max_symbol) {
            max_symbol = sym + 1;
        }
    }

    // Recursively process nested Protos
    uint32_t sub_count = (uint32_t)PROTO_PROTO_COUNT(proto);
    for (uint32_t i = 0; i < sub_count; i++) {
        XrProto *sub = PROTO_PROTO(proto, i);
        max_symbol = collect_symbols_from_proto(sub, max_symbol);
    }

    return max_symbol;
}

// Remap global symbol IDs in per-function symbol table
static void remap_symbols_in_proto(XrProto *proto, int *id_map, int map_size) {
    if (!proto) return;

    // Remap per-function symbol table entries (instructions are untouched)
    for (int i = 0; i < proto->symbol_count; i++) {
        int32_t old_id = proto->symbols[i];
        if (old_id >= 0 && old_id < map_size && id_map[old_id] >= 0) {
            proto->symbols[i] = id_map[old_id];
        }
    }

    // Recursively process nested Protos
    uint32_t sub_count = (uint32_t)PROTO_PROTO_COUNT(proto);
    for (uint32_t i = 0; i < sub_count; i++) {
        XrProto *sub = PROTO_PROTO(proto, i);
        remap_symbols_in_proto(sub, id_map, map_size);
    }
}

/* ========== Shared Variable Index Remapping ========== */

// Collect max shared index used in Proto, returns max_shared_index + 1
static int collect_shared_from_proto(XrProto *proto, int max_shared) {
    if (!proto) return max_shared;

    uint32_t code_count = (uint32_t)PROTO_CODE_COUNT(proto);
    for (uint32_t i = 0; i < code_count; i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        OpCode op = GET_OPCODE(inst);

        // Check opcodes that use shared index
        if (op == OP_GETSHARED || op == OP_SETSHARED) {
            int shared_idx = GETARG_Bx(inst);
            if (shared_idx >= max_shared) {
                max_shared = shared_idx + 1;
            }
        }
    }

    // Recursively process nested Protos
    uint32_t sub_count = (uint32_t)PROTO_PROTO_COUNT(proto);
    for (uint32_t i = 0; i < sub_count; i++) {
        XrProto *sub = PROTO_PROTO(proto, i);
        max_shared = collect_shared_from_proto(sub, max_shared);
    }

    return max_shared;
}

// Set shared_offset on proto and all nested protos (no instruction rewriting needed)
static void set_shared_offset_recursive(XrProto *proto, int offset) {
    if (!proto) return;

    proto->shared_offset = offset;

    uint32_t sub_count = (uint32_t)PROTO_PROTO_COUNT(proto);
    for (uint32_t i = 0; i < sub_count; i++) {
        XrProto *sub = PROTO_PROTO(proto, i);
        set_shared_offset_recursive(sub, offset);
    }
}

/* ========== Proto Serialization ========== */

static bool bc_write_proto(BcWriter *w, XrProto *proto);
static XrProto* bc_read_proto(BcReader *r);

static bool bc_write_proto(BcWriter *w, XrProto *proto) {
    if (!proto) return false;

    // 1. Function name
    const char *name = proto->name ? proto->name->data : "";
    if (!bc_put_string(w, name)) return false;

    // 2. Source file (optional)
    if (w->flags & XR_BC_STRIP_SOURCE) {
        if (!bc_put_string(w, "")) return false;
    } else {
        if (!bc_put_string(w, proto->source_file)) return false;
    }

    // 3. Function attributes
    if (!bc_put_u32(w, proto->numparams)) return false;
    if (!bc_put_u32(w, proto->maxstacksize)) return false;
    if (!bc_put_u32(w, proto->num_globals)) return false;
    if (!bc_put_u32(w, proto->num_spill_slots)) return false;
    if (!bc_put_u8(w, proto->is_vararg ? 1 : 0)) return false;
    if (!bc_put_u8(w, proto->is_coro_safe ? 1 : 0)) return false;

    // 4. Bytecode
    uint32_t code_count = (uint32_t)PROTO_CODE_COUNT(proto);
    if (!bc_put_u32(w, code_count)) return false;
    for (uint32_t i = 0; i < code_count; i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        if (!bc_put_u32(w, inst)) return false;
    }

    // 5. Constants
    uint32_t const_count = (uint32_t)PROTO_CONST_COUNT(proto);
    if (!bc_put_u32(w, const_count)) return false;
    for (uint32_t i = 0; i < const_count; i++) {
        XrValue val = PROTO_CONSTANT(proto, i);
        if (!bc_write_value(w, val)) return false;
    }

    // 6. Line info (optional)
    if (w->flags & XR_BC_STRIP_DEBUG) {
        if (!bc_put_u32(w, 0)) return false;
    } else {
        uint32_t line_count = (uint32_t)PROTO_LINE_COUNT(proto);
        if (!bc_put_u32(w, line_count)) return false;
        for (uint32_t i = 0; i < line_count; i++) {
            if (!bc_put_u32(w, PROTO_LINE(proto, i))) return false;
        }
    }

    // 7. Upvalue info
    uint32_t upval_count = (uint32_t)PROTO_UPVAL_COUNT(proto);
    if (!bc_put_u32(w, upval_count)) return false;
    for (uint32_t i = 0; i < upval_count; i++) {
        UpvalInfo info = PROTO_UPVALUE(proto, i);
        if (!bc_put_u8(w, info.index)) return false;
        if (!bc_put_u8(w, info.source)) return false;
        if (!bc_put_u8(w, info.storage_mode)) return false;
        if (!bc_put_u8(w, info.is_const)) return false;
        if (!bc_put_u8(w, info.slot_type)) return false;
    }

    // 8. Nested Protos
    uint32_t sub_count = (uint32_t)PROTO_PROTO_COUNT(proto);
    if (!bc_put_u32(w, sub_count)) return false;
    for (uint32_t i = 0; i < sub_count; i++) {
        XrProto *sub = PROTO_PROTO(proto, i);
        if (!bc_write_proto(w, sub)) return false;
    }

    // 9. Per-function symbol table
    if (!bc_put_u32(w, (uint32_t)proto->symbol_count)) return false;
    for (int i = 0; i < proto->symbol_count; i++) {
        if (!bc_put_u32(w, (uint32_t)proto->symbols[i])) return false;
    }

    return true;
}

static XrProto* bc_read_proto(BcReader *r) {
    // Allocate Proto
    XrProto *proto = xr_malloc(sizeof(XrProto));
    if (!proto) { r->error = XR_BC_ERR_ALLOC; return NULL; }
    memset(proto, 0, sizeof(XrProto));

    // 1. Function name
    char *name = bc_get_string(r);
    if (r->error != XR_BC_OK) goto fail;
    if (name && name[0]) {
        proto->name = xr_string_intern(r->X, name, strlen(name), 0);
        xr_free(name);
    }

    // 2. Source file
    char *source = bc_get_string(r);
    if (r->error != XR_BC_OK) goto fail;
    if (source && source[0]) {
        proto->source_file = source;
    } else {
        xr_free(source);
    }

    // 3. Function attributes
    proto->numparams = bc_get_u32(r);
    proto->maxstacksize = bc_get_u32(r);
    proto->num_globals = bc_get_u32(r);
    proto->num_spill_slots = bc_get_u32(r);
    proto->is_vararg = bc_get_u8(r) != 0;
    proto->is_coro_safe = bc_get_u8(r) != 0;
    if (r->error != XR_BC_OK) goto fail;

    // 4. Bytecode
    uint32_t code_count = bc_get_u32(r);
    if (r->error != XR_BC_OK) goto fail;
    xr_dynarray_init(&proto->code, sizeof(XrInstruction));
    for (uint32_t i = 0; i < code_count; i++) {
        XrInstruction inst = bc_get_u32(r);
        if (r->error != XR_BC_OK) goto fail;
        DYNARRAY_ADD(&proto->code, inst, XrInstruction);
    }

    // 5. Constants
    uint32_t const_count = bc_get_u32(r);
    if (r->error != XR_BC_OK) goto fail;
    xr_dynarray_init(&proto->constants, sizeof(XrValue));
    for (uint32_t i = 0; i < const_count; i++) {
        XrValue val = bc_read_value(r);
        if (r->error != XR_BC_OK) goto fail;
        DYNARRAY_ADD(&proto->constants, val, XrValue);
    }

    // 6. Line info
    uint32_t line_count = bc_get_u32(r);
    if (r->error != XR_BC_OK) goto fail;
    xr_dynarray_init(&proto->lineinfo, sizeof(int));
    for (uint32_t i = 0; i < line_count; i++) {
        int line = (int)bc_get_u32(r);
        if (r->error != XR_BC_OK) goto fail;
        DYNARRAY_ADD(&proto->lineinfo, line, int);
    }

    // 7. Upvalue info
    uint32_t upval_count = bc_get_u32(r);
    if (r->error != XR_BC_OK) goto fail;
    xr_dynarray_init(&proto->upvalues, sizeof(UpvalInfo));
    for (uint32_t i = 0; i < upval_count; i++) {
        UpvalInfo info = {0};
        info.index        = bc_get_u8(r);
        info.source       = bc_get_u8(r);
        info.storage_mode = bc_get_u8(r);
        info.is_const     = bc_get_u8(r);
        info.slot_type    = bc_get_u8(r);
        if (r->error != XR_BC_OK) goto fail;
        DYNARRAY_ADD(&proto->upvalues, info, UpvalInfo);
    }

    // 8. Nested Protos
    uint32_t sub_count = bc_get_u32(r);
    if (r->error != XR_BC_OK) goto fail;
    xr_dynarray_init(&proto->protos, sizeof(XrProto*));
    for (uint32_t i = 0; i < sub_count; i++) {
        XrProto *sub = bc_read_proto(r);
        if (!sub) goto fail;
        DYNARRAY_ADD(&proto->protos, sub, XrProto*);
    }

    // 9. Per-function symbol table
    uint32_t sym_count = bc_get_u32(r);
    if (r->error != XR_BC_OK) goto fail;
    if (sym_count > 0) {
        proto->symbols = xr_malloc(sym_count * sizeof(int32_t));
        if (!proto->symbols) { r->error = XR_BC_ERR_ALLOC; goto fail; }
        proto->symbol_count = (int)sym_count;
        proto->symbol_capacity = (int)sym_count;
        for (uint32_t i = 0; i < sym_count; i++) {
            proto->symbols[i] = (int32_t)bc_get_u32(r);
            if (r->error != XR_BC_OK) goto fail;
        }
    }

    return proto;

fail:
    // Free all resources allocated during partial read
    if (proto->source_file) xr_free((void*)proto->source_file);
    xr_dynarray_free(&proto->code);
    xr_dynarray_free(&proto->constants);
    xr_dynarray_free(&proto->lineinfo);
    xr_dynarray_free(&proto->upvalues);
    // Recursively free nested protos already read
    for (int i = 0; i < proto->protos.count; i++) {
        XrProto *sub = DYNARRAY_GET(&proto->protos, i, XrProto*);
        if (sub) xr_free(sub);  // sub's own cleanup handled by its bc_read_proto fail path
    }
    xr_dynarray_free(&proto->protos);
    xr_free(proto->symbols);
    xr_free(proto);
    return NULL;
}

/* ========== Public API ========== */

uint8_t* xr_bytecode_write(XrayIsolate *X, XrProto *proto, int flags, size_t *out_size) {
    if (!X || !proto || !out_size) return NULL;

    BcWriter w;
    bc_writer_init(&w, X, flags);

    // Collect symbols (symbol ID starts from 1, returns max ID + 1)
    int max_symbol_id = collect_symbols_from_proto(proto, 0);

    // Collect shared variable count
    int shared_count = collect_shared_from_proto(proto, 0);

    // Write header
    if (!bc_put_u32(&w, XR_BC_MAGIC)) goto fail;
    if (!bc_put_u16(&w, XR_BC_VERSION)) goto fail;
    if (!bc_put_u16(&w, (uint16_t)flags)) goto fail;
    if (!bc_put_u32(&w, 1)) goto fail; // proto count
    if (!bc_put_u32(&w, (uint32_t)max_symbol_id)) goto fail; // max symbol id
    if (!bc_put_u32(&w, (uint32_t)shared_count)) goto fail; // shared count

    // Write symbol table (symbol ID starts from 1)
    for (int i = 1; i <= max_symbol_id; i++) {
        const char *name = xr_symbol_get_name_by_id(X, i);
        if (!bc_put_string(&w, name ? name : "")) goto fail;
    }

    // Write Proto
    if (!bc_write_proto(&w, proto)) goto fail;

    *out_size = w.size;
    return w.buf;

fail:
    xr_free(w.buf);
    *out_size = 0;
    return NULL;
}

XrProto* xr_bytecode_read(XrayIsolate *X, const uint8_t *data, size_t size, XrBcError *error) {
    if (!X || !data || size == 0) {
        if (error) *error = XR_BC_ERR_TRUNCATED;
        return NULL;
    }

    BcReader r;
    bc_reader_init(&r, X, data, size);

    // Read header
    uint32_t magic = bc_get_u32(&r);
    if (magic != XR_BC_MAGIC) {
        if (error) *error = XR_BC_ERR_MAGIC;
        return NULL;
    }

    uint16_t version = bc_get_u16(&r);
    if (version != XR_BC_VERSION) {
        if (error) *error = XR_BC_ERR_VERSION;
        return NULL;
    }

    bc_get_u16(&r); // flags
    bc_get_u32(&r); // proto count
    uint32_t max_symbol_id = bc_get_u32(&r); // max symbol id
    uint32_t shared_count = bc_get_u32(&r); // shared count

    if (r.error != XR_BC_OK) {
        if (error) *error = r.error;
        return NULL;
    }

    // Calculate shared index offset (based on current shared count)
    XrVMState *vm = xr_isolate_get_vm_state(X);
    int shared_offset = vm->shared.count;

    (void)shared_count;

    // Read symbol table and build mapping (symbol ID starts from 1)
    int *id_map = NULL;
    int map_size = (int)max_symbol_id + 1;
    if (max_symbol_id > 0) {
        id_map = xr_malloc(map_size * sizeof(int));
        if (!id_map) {
            if (error) *error = XR_BC_ERR_ALLOC;
            return NULL;
        }
        memset(id_map, -1, map_size * sizeof(int));

        for (uint32_t i = 1; i <= max_symbol_id; i++) {
            char *name = bc_get_string(&r);
            if (r.error != XR_BC_OK) {
                xr_free(id_map);
                if (error) *error = r.error;
                return NULL;
            }
            if (name && name[0]) {
                id_map[i] = xr_symbol_register_in_table(xr_isolate_get_symbol_table(X), name);
                xr_free(name);
            } else {
                id_map[i] = -1;
                xr_free(name);
            }
        }
    }

    // Read Proto
    XrProto *proto = bc_read_proto(&r);

    // Remap symbol IDs
    if (proto && id_map) {
        remap_symbols_in_proto(proto, id_map, map_size);
    }

    // Set per-module shared_offset (VM applies offset at runtime, no instruction rewriting)
    if (proto && shared_offset > 0) {
        set_shared_offset_recursive(proto, shared_offset);
    }

    // Update global shared.count
    if (shared_count > 0) {
        vm->shared.count += (int)shared_count;
        xr_shared_array_ensure(&vm->shared, vm->shared.count - 1);
    }

    xr_free(id_map);
    if (error) *error = r.error;
    return proto;
}

int xr_eval_bytecode(XrayIsolate *X, const uint8_t *data, size_t size) {
    XR_DCHECK(X != NULL, "eval_bytecode: NULL isolate");
    XR_DCHECK(data != NULL, "eval_bytecode: NULL data");
    XrBcError error;
    XrProto *proto = xr_bytecode_read(X, data, size, &error);
    if (!proto) {
        xr_log_warning("bytecode", "failed to load: error=%d", error);
        return -1;
    }

    // Use xr_execute which properly initializes coroutine and runtime
    int result = xr_execute(X, proto);
    xr_vm_proto_free(proto);
    return result;
}

/* ========== AOT Bytecode Load (decomposed API) ========== */

XrProto* xr_bytecode_load(XrayIsolate *X, const uint8_t *data, size_t size) {
    XR_DCHECK(X != NULL, "bytecode_load: NULL isolate");
    XR_DCHECK(data != NULL, "bytecode_load: NULL data");
    XrBcError error;
    XrProto *proto = xr_bytecode_read(X, data, size, &error);
    if (!proto) {
        xr_log_warning("bytecode", "failed to load: error=%d", error);
        return NULL;
    }
    return proto;
}

/* ========== AOT Registration Helpers ========== */

const char* xr_proto_name(XrProto *p) {
    if (!p || !p->name) return NULL;
    return XR_STRING_CHARS(p->name);
}

XrProto** xr_proto_children(XrProto *p, int *count) {
    if (!p) { *count = 0; return NULL; }
    *count = PROTO_PROTO_COUNT(p);
    if (*count == 0) return NULL;
    return (XrProto**)p->protos.data;
}

void xr_proto_set_jit_entry(XrProto *p, void *entry) {
    if (p) p->jit_entry = entry;
}

void xr_proto_set_param_types(XrProto *p, const uint8_t *ptypes,
                               int nparams, uint8_t return_type) {
    if (!p) return;
    p->return_type_info = xr_slot_type_to_type(NULL, return_type);
    if (nparams > 0 && ptypes && !p->param_types) {
        p->param_types = (struct XrType **)xr_calloc(
            nparams, sizeof(struct XrType *));
        if (p->param_types) {
            p->param_types_count = nparams;
            for (int i = 0; i < nparams; i++) {
                if (ptypes[i] == XR_SLOT_I64)
                    p->param_types[i] = xr_type_new_int(NULL);
                else if (ptypes[i] == XR_SLOT_F64)
                    p->param_types[i] = xr_type_new_float(NULL);
                else if (ptypes[i] == XR_SLOT_BOOL)
                    p->param_types[i] = xr_type_new_bool(NULL);
            }
        }
    }
}

/* ========== File API ========== */

bool xr_compile_to_file(XrayIsolate *X, const char *source_file,
                        const char *output_file, int flags) {
    // Read source file
    FILE *f = fopen(source_file, "r");
    if (!f) {
        xr_log_warning("compile", "cannot open: %s", source_file);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *source = xr_malloc(size + 1);
    if (!source) { fclose(f); return false; }

    size_t read = fread(source, 1, size, f);
    source[read] = '\0';
    fclose(f);

    // Parse
    AstNode *ast = xr_parse_with_source(X, source, source_file);
    xr_free(source);

    if (!ast) {
        xr_log_warning("compile", "parse failed: %s", source_file);
        return false;
    }

    // Compile
    XrProto *proto = xr_compile_ast_with_source(X, ast, source_file);
    xr_program_destroy(ast);

    if (!proto) {
        xr_log_warning("compile", "compilation failed: %s", source_file);
        return false;
    }

    // Serialize
    size_t bc_size;
    uint8_t *bc = xr_bytecode_write(X, proto, flags, &bc_size);
    if (!bc) {
        xr_vm_proto_free(proto);
        xr_log_warning("compile", "serialization failed");
        return false;
    }

    xr_vm_proto_free(proto);

    // Write to file
    f = fopen(output_file, "wb");
    if (!f) {
        xr_free(bc);
        xr_log_warning("compile", "cannot create: %s", output_file);
        return false;
    }

    fwrite(bc, 1, bc_size, f);
    fclose(f);
    xr_free(bc);

    return true;
}

int xr_run_bytecode_file(XrayIsolate *X, const char *bytecode_file) {
    XR_DCHECK(X != NULL, "run_bytecode_file: NULL isolate");
    XR_DCHECK(bytecode_file != NULL, "run_bytecode_file: NULL bytecode_file");
    FILE *f = fopen(bytecode_file, "rb");
    if (!f) {
        xr_log_warning("bytecode", "cannot open: %s", bytecode_file);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = xr_malloc(size);
    if (!data) { fclose(f); return -1; }

    fread(data, 1, size, f);
    fclose(f);

    int result = xr_eval_bytecode(X, data, size);
    xr_free(data);
    return result;
}

/* ========== Output Format ========== */

XrOutputFormat xr_detect_output_format(const char *filename, XrOutputFormat explicit_fmt) {
    if (explicit_fmt != XR_OUTPUT_AUTO) return explicit_fmt;
    if (!filename) return XR_OUTPUT_BYTECODE;

    const char *ext = strrchr(filename, '.');
    if (!ext) return XR_OUTPUT_BYTECODE;

    if (strcmp(ext, ".c") == 0) return XR_OUTPUT_C_SOURCE;
    if (strcmp(ext, ".h") == 0) return XR_OUTPUT_C_HEADER;
    if (strcmp(ext, ".xrc") == 0) return XR_OUTPUT_BYTECODE;

    return XR_OUTPUT_BYTECODE;
}

bool xr_output_c_source(XrayIsolate *X, XrProto *proto,
                        const char *output_file, const char *var_name, int flags) {
    // Serialize
    size_t bc_size;
    uint8_t *bc = xr_bytecode_write(X, proto, flags, &bc_size);
    if (!bc) return false;

    // Write C file
    FILE *f = fopen(output_file, "w");
    if (!f) { xr_free(bc); return false; }

    fprintf(f, "/* Auto-generated by xray compile */\n\n");
    fprintf(f, "#include <stdint.h>\n\n");
    fprintf(f, "const uint32_t %s_size = %zu;\n\n", var_name, bc_size);
    fprintf(f, "const uint8_t %s[%zu] = {\n", var_name, bc_size);

    for (size_t i = 0; i < bc_size; i++) {
        if (i % 12 == 0) fprintf(f, "    ");
        fprintf(f, "0x%02x", bc[i]);
        if (i < bc_size - 1) fprintf(f, ",");
        if ((i + 1) % 12 == 0 || i == bc_size - 1) fprintf(f, "\n");
        else fprintf(f, " ");
    }

    fprintf(f, "};\n");

    fclose(f);
    xr_free(bc);
    return true;
}
