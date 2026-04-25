/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstmt_helpers.c - Shared helpers for statement-level type bridging
 *
 * Lifted unchanged from xstmt_simple.c during Phase 3.4 (C-02). The
 * four exported functions and the two file-static helpers that
 * support them used to be inline at the top of xstmt_simple.c; the
 * split into xstmt_simple.c / xstmt_typed.c forced the move. No
 * behaviour changes -- byte-for-byte identical to the pre-split
 * implementation.
 */

#include "xstmt_helpers.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xcompiler_class_registry.h"
#include "xemit.h"
#include "xexpr_desc.h"
#include "xexpr.h"
#include "../parser/xast.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/value/xtype_names.h"
#include <string.h>

void xstmt_emit_value_copy(XrCompilerContext *ctx, XrCompiler *compiler,
                           int dest_reg, int src_reg, XrType *compile_type) {
    if (compile_type && ctx->class_registry) {
        const char *cn = NULL;
        if (compile_type->kind == XR_KIND_CLASS || compile_type->kind == XR_KIND_INSTANCE)
            cn = compile_type->instance.class_name;
        if (cn) {
            ClassInfo *ci = xr_class_registry_lookup(ctx->class_registry, cn);
            if (ci && ci->struct_layout) {
                int alloc_size = 8 + ci->struct_layout->total_size;
                int aligned_size = (alloc_size + 15) & ~15;
                int slot_offset = compiler->struct_area_offset / 16;
                compiler->struct_area_offset += aligned_size;
                emit_abc(compiler->emitter, OP_STRUCT_COPY, dest_reg, src_reg, slot_offset);
                return;
            }
        }
    }
    emit_abc(compiler->emitter, OP_COPY, dest_reg, src_reg, 0);
}

const char *xstmt_type_flag_name(XrType *type) {
    if (!type) return "unknown";
    if (XR_TYPE_IS_INT(type)) return "int";
    if (XR_TYPE_IS_FLOAT(type)) return "float";
    if (XR_TYPE_IS_STRING(type) || (type->kind == XR_KIND_STRING && type->is_literal)) return "string";
    if (XR_TYPE_IS_BOOL(type) || (type->kind == XR_KIND_BOOL && type->is_literal)) return "bool";
    if (type->kind == XR_KIND_NULL) return "null";
    if (type->kind == XR_KIND_ARRAY) return "Array";
    if (type->kind == XR_KIND_MAP) return "Map";
    if (type->kind == XR_KIND_SET) return "Set";
    if (type->kind == XR_KIND_JSON) return "Json";
    if (type->kind == XR_KIND_INSTANCE) {
        return type->instance.class_name ? type->instance.class_name : "instance";
    }
    if (type->kind == XR_KIND_CLASS) return "class";
    if (type->kind == XR_KIND_FUNCTION) return "function";
    if (type->kind == XR_KIND_CHANNEL) return "Channel";
    if (xr_type_is_named_class(type, "BigInt")) return "BigInt";
    if (type->kind == XR_KIND_UNION && type->union_type.member_count > 0) {
        static char buf[256];
        int pos = 0;
        for (int i = 0; i < type->union_type.member_count && pos < 240; i++) {
            if (i > 0) { buf[pos++] = ' '; buf[pos++] = '|'; buf[pos++] = ' '; }
            const char *m = xstmt_type_flag_name(type->union_type.members[i]);
            int len = (int)strlen(m);
            if (pos + len >= 250) { memcpy(buf + pos, "...", 3); pos += 3; break; }
            memcpy(buf + pos, m, len);
            pos += len;
        }
        if (type->is_nullable && pos < 245) {
            memcpy(buf + pos, " | null", 7);
            pos += 7;
        }
        buf[pos] = '\0';
        return buf;
    }
    return "unknown";
}

void xstmt_emit_box_if_raw(XrEmitter *emitter, int reg, const XrExprDesc *expr) {
    if (xexpr_is_raw_i64(expr)) {
        emit_abc(emitter, OP_BOX_I64, reg, reg, 0);
    } else if (xexpr_is_raw_f64(expr)) {
        emit_abc(emitter, OP_BOX_F64, reg, reg, 0);
    }
}

/*
 * Convert XrTypeKind to XrTypeId for CHECKTYPE bitmask.
 * Returns -1 if the kind has no direct TID mapping.
 */
static int kind_to_tid(XrTypeKind kind) {
    switch (kind) {
    case XR_KIND_INT:    return XR_TID_INT;
    case XR_KIND_FLOAT:  return XR_TID_FLOAT;
    case XR_KIND_STRING: return XR_TID_STRING;
    case XR_KIND_BOOL:   return XR_TID_BOOL;
    case XR_KIND_JSON:   return XR_TID_JSON;
    case XR_KIND_ARRAY:  return XR_TID_ARRAY;
    case XR_KIND_NULL:   return XR_TID_NULL;
    default:             return -1;
    }
}

/*
 * Build bitmask from target type for OP_CHECKTYPE.
 * Single primitive: one bit set.  Union: OR of all member bits.
 * Returns 0 if no valid bitmask can be built.
 */
static int64_t build_checktype_mask(XrType *target) {
    if (!target) return 0;
    int tid = kind_to_tid(target->kind);
    if (tid >= 0) return (1LL << tid);
    if (target->kind == XR_KIND_UNION) {
        int64_t mask = 0;
        for (int i = 0; i < target->union_type.member_count; i++) {
            int mt = kind_to_tid(target->union_type.members[i]->kind);
            if (mt < 0) return 0;
            mask |= (1LL << mt);
        }
        if (target->is_nullable) mask |= (1LL << XR_TID_NULL);
        return mask;
    }
    return 0;
}

void xstmt_emit_json_checktype(XrCompilerContext *ctx, XrCompiler *compiler,
                               int reg, XrType *declared_type, AstNode *init_expr) {
    if (!declared_type || !init_expr) return;

    XrType *init_type = get_expr_type(ctx, compiler, init_expr);
    if (!init_type || !xr_is_json_coercion(declared_type, init_type)) return;

    int64_t mask = build_checktype_mask(declared_type);
    if (mask != 0) {
        int type_const = xr_vm_proto_add_constant(compiler->proto, xr_int(mask));
        emit_abc(compiler->emitter, OP_CHECKTYPE, reg, type_const, 0);
    }
}
