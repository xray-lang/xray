/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_verify.c - Scalar signatures, graph structure and SSA dominance
 *
 * KEY CONCEPT:
 *   Validation derives dominance from the actual edges and checks every
 *   definition, including block parameters and values passed along backedges.
 */
#include "xlir_internal.h"
#include "../shared/xnative_host_leaf.h"
#include "xlir_verify.h"
#include "xlir_pod.h"
#include "xlir_ownership.h"
#include "xlir_cell.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const lir_type_names[] = {"invalid", "unit", "i1",  "i8",  "i16", "i32", "i64",
                                             "u8", "u16", "u32", "u64", "f32", "f64", "ptr", "i64_cell",
                                             "string", "string_cell", "state", "state_ref", "array", "array_cell", "array_ref", "view_frame", "view_cell", "slice_i64", "string_ref", "function_scope",
                                             "map_string_i64", "map_string_i64_cell", "map_string_i64_ref", "set_i64", "set_i64_cell", "set_i64_ref", "i64_ref"};
static const char *const lir_op_names[] = {
    "invalid",
#define XR_LIR_OP(name, text, arity) text,
#include "xlir_ops.def"
#undef XR_LIR_OP
};
static const int lir_arities[] = {
    -1,
#define XR_LIR_OP(name, text, arity) arity,
#include "xlir_ops.def"
#undef XR_LIR_OP
};

XR_FUNC const char *xr_lir_type_name(XrLirType type) {
    return type > XR_LIR_TYPE_INVALID && type < XR_LIR_TYPE_COUNT ? lir_type_names[type]
                                                                  : "invalid";
}
XR_FUNC const char *xr_lir_op_name(XrLirOp op) {
    return op > XR_LIR_OP_INVALID && op < XR_LIR_OP_COUNT ? lir_op_names[op] : "invalid";
}
XR_FUNC bool xr_lir_internal_type_valid(const XrLirModule *m, XrLirType type, bool unit) {
    if (type < XR_LIR_TYPE_COUNT) return type > XR_LIR_TYPE_INVALID && (unit || type != XR_LIR_UNIT) &&
        type != XR_LIR_VIEW_FRAME && type != XR_LIR_VIEW_CELL && type != XR_LIR_SLICE_I64;
    const XrLirTypeData *data = xr_lir_internal_constructed_data(m, type);
    return data && !data->class_payload_only;
}
XR_FUNC bool xr_lir_internal_terminal(XrLirOp op) {
    return op == XR_LIR_BR || op == XR_LIR_CONDBR || op == XR_LIR_RET ||
        op == XR_LIR_INVOKE || op == XR_LIR_RETURN_ERROR || op == XR_LIR_PANIC_ERROR ||
        op == XR_LIR_UNCAUGHT_ERROR;
}
XR_FUNC XrLirStatus xr_lir_internal_error(XrLirDiagnostic *d, const char *message,
                                          uint32_t function, uint32_t block, uint32_t instruction,
                                          const XrLirSource *source) {
    if (d) {
        *d = (XrLirDiagnostic) {.status = XR_LIR_VERIFY_ERROR,
                                .function = function,
                                .block = block,
                                .instruction = instruction};
        if (source)
            d->source = *source;
        snprintf(d->message, sizeof(d->message), "%s", message);
    }
    return XR_LIR_VERIFY_ERROR;
}

static XrLirStatus lir_status_error(XrLirDiagnostic *d, XrLirStatus status, const char *message) {
    xr_lir_internal_error(d, message, 0, 0, 0, NULL);
    if (d)
        d->status = status;
    return status;
}

static bool lir_integer(XrLirType type) {
    return type >= XR_LIR_I8 && type <= XR_LIR_U64;
}
static bool lir_float(XrLirType type) {
    return type == XR_LIR_F32 || type == XR_LIR_F64;
}
static const char *clock_external_name(XrLirExternKind kind) {
    switch (kind) {
    case XR_LIR_EXTERN_CLOCK_REALTIME: return "xrt_clock_realtime_nanos";
    case XR_LIR_EXTERN_CLOCK_CPU: return "xrt_clock_process_cpu_nanos";
    case XR_LIR_EXTERN_CLOCK_MONOTONIC: return "xrt_clock_monotonic_nanos";
    case XR_LIR_EXTERN_UTC_OFFSET_AT: return "xrt_clock_utc_offset_at";
    default: return NULL;
    }
}

static bool lir_constant_valid(XrLirType type, uint64_t bits) {
    if (type == XR_LIR_I1)
        return bits <= 1;
    if (type == XR_LIR_PTR)
        return bits == 0;
    if (lir_integer(type)) {
        unsigned ordinal =
            type <= XR_LIR_I64 ? (unsigned) (type - XR_LIR_I8) : (unsigned) (type - XR_LIR_U8);
        unsigned width = 8u << ordinal;
        return width == 64 || (bits >> width) == 0;
    }
    if (type == XR_LIR_F32) {
        if (bits >> 32)
            return false;
        bool nan =
            (bits & UINT64_C(0x7f800000)) == UINT64_C(0x7f800000) && (bits & UINT64_C(0x007fffff));
        return !nan || bits == UINT64_C(0x7fc00000);
    }
    if (type == XR_LIR_F64) {
        bool nan = (bits & UINT64_C(0x7ff0000000000000)) == UINT64_C(0x7ff0000000000000) &&
                   (bits & UINT64_C(0x000fffffffffffff));
        return !nan || bits == UINT64_C(0x7ff8000000000000);
    }
    return false;
}

static bool lir_signature_matches(const XrLirModule *m, const XrLirFunctionData *f, const XrLirInstructionView *v,
                                  const XrLirType *parameters, uint32_t count, XrLirType result) {
    if (v->operand_count != count || v->type != result)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        const XrLirValueData *actual = &f->values[v->operands[i] - 1];
        if (parameters[i] == XR_LIR_I64_REF && v->op == XR_LIR_CALL &&
            (actual->type == XR_LIR_I64_CELL || (actual->type == XR_LIR_STATE_REF &&
                actual->state_reference.payload == XR_LIR_I64 && actual->state_reference.mutable))) continue;
        if (actual->type != parameters[i] && !xr_lir_callable_accepts(m, parameters[i], actual->type))
            return false;
    }
    return true;
}

static const XrLirInstructionView *lir_terminator(const XrLirFunctionData *, uint32_t);
#include "xlir_state_verify.inc.c"
#include "xlir_array_verify.inc.c"

#include "xlir_callable_verify.inc.c"

static bool lir_slice_cleanup_target(const XrLirModule *m, const XrLirFunctionData *f, uint64_t id) {
    if (!id || id > f->value_count || !xr_lir_internal_slice_type(m, f->values[id - 1].type)) return false;
    XrLirValueData value = f->values[id - 1];
    if (value.parameter) return value.block && value.block <= f->block_count &&
        value.instruction < f->blocks[value.block - 1].parameter_count &&
        f->blocks[value.block - 1].parameters[value.instruction] == id;
    const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, (uint32_t)id);
    XrLirSliceIntrinsic role;
    if (!definition || definition->result != id || definition->type != value.type) return false;
    if (definition->op == XR_LIR_CALL) {
        XrLirSliceFunctionFact fact;
        return xr_lir_internal_slice_function_fact(m, definition->callee, &fact) == XR_LIR_OK &&
            fact.available && fact.result_parameter < definition->operand_count;
    }
    return definition->op == XR_LIR_CALL_INTRINSIC && xr_lir_slice_intrinsic_view(m, definition->intrinsic, &role) == XR_LIR_OK &&
        (role.operation == XR_LIR_SLICE_OP_NEW || role.operation == XR_LIR_SLICE_OP_BIND || role.operation == XR_LIR_SLICE_OP_REBORROW);
}
static bool lir_slice_formal_cleanup_target(const XrLirModule *m, const XrLirFunctionData *f, uint64_t id) {
    if (!id || id > f->value_count || !xr_lir_internal_slice_type(m, f->values[id - 1].type)) return false;
    const XrLirValueData *value = &f->values[id - 1];
    return value->parameter && !value->block && value->instruction < f->parameter_count &&
        f->parameters[value->instruction] == id;
}

static bool lir_scalar_instruction_types(const XrLirModule *m, const XrLirFunctionData *f,
                                         const XrLirInstructionView *v) {
    XrLirType a = v->operand_count ? f->values[v->operands[0] - 1].type : XR_LIR_TYPE_INVALID;
    XrLirType b = v->operand_count > 1 ? f->values[v->operands[1] - 1].type : XR_LIR_TYPE_INVALID;
    switch (v->op) {
        case XR_LIR_MAP_LITERAL_NEW: {
            if(v->type!=XR_LIR_MAP_STRING_I64 || !v->bits || v->bits>m->map_literal_count)return false;
            const XrLirMapLiteral *literal=&m->map_literals[v->bits-1];uint32_t operand=0;
            for(uint32_t n=0;n<literal->count;++n) {
                bool spread=literal->items[n].kind!=0;
                uint32_t width=spread?1u:2u;
                if(operand>v->operand_count || width>v->operand_count-operand ||
                    f->values[v->operands[operand]-1].type!=(XrLirType)(spread?XR_LIR_MAP_STRING_I64:XR_LIR_STRING) ||
                    (!spread && f->values[v->operands[operand+1]-1].type!=XR_LIR_I64))return false;
                operand+=width;
            }
            return operand==v->operand_count;
        }
        case XR_LIR_VALUE_RETAIN:
            return v->type == a && (xr_lir_boxed_type(m, a) || xr_lir_cell_value_type(m,a,NULL));
        case XR_LIR_CLASS_ADOPT: {
            XrLirTypeView target;
            return v->operand_count == 1 && xr_lir_type_view(m, v->type, &target) == XR_LIR_OK &&
                !xr_lir_class_storage(m, v->type) && !xr_lir_class_storage(m, a) &&
                target.kind == XR_LIR_TYPE_CLASS && xr_lir_type_view(m, a, &target) == XR_LIR_OK &&
                target.kind == XR_LIR_TYPE_CLASS;
        }
        case XR_LIR_CLASS_IS: {
            XrLirTypeView target;
            return v->operand_count == 1 && v->type == XR_LIR_I1 &&
                xr_lir_type_view(m, a, &target) == XR_LIR_OK && target.kind == XR_LIR_TYPE_CLASS &&
                v->bits && xr_lir_type_view(m, (XrLirType)v->bits, &target) == XR_LIR_OK &&
                target.kind == XR_LIR_TYPE_CLASS;
        }
        case XR_LIR_VALUE_DROP:
            return v->type == XR_LIR_UNIT && (xr_lir_boxed_type(m, a) || xr_lir_cell_value_type(m,a,NULL));
        case XR_LIR_RECORD_NEW: case XR_LIR_CLASS_NEW: {
            XrLirType target = v->type;
            if (v->op == XR_LIR_CLASS_NEW &&
                (xr_lir_class_storage(m, target) || !xr_lir_class_payload(m, target, &target))) return false;
            const XrLirRecordField *fields;
            uint32_t count;
            if (xr_lir_aggregate_fields(m, target, &fields, &count) != XR_LIR_OK || count != v->operand_count) return false;
            for (uint32_t n = 0; n < count; ++n) {
                const XrLirValueData *input = &f->values[v->operands[n] - 1];
                if (input->type != fields[n].type || input->array_element != fields[n].array_element) return false;
            }
            return true;
        }
        case XR_LIR_RECORD_FIELD: case XR_LIR_CLASS_FIELD: {
            if (v->op == XR_LIR_CLASS_FIELD && !xr_lir_class_payload(m, a, &a)) return false;
            const XrLirRecordField *fields;
            uint32_t count;
            return xr_lir_aggregate_fields(m, a, &fields, &count) == XR_LIR_OK &&
                v->bits < count && fields[v->bits].type == v->type;
        }
        case XR_LIR_CLASS_SCALAR_WRITE: case XR_LIR_CLASS_ARRAY_WRITE: case XR_LIR_CLASS_ARRAY_PUSH: {
            const XrLirRecordField *fields;
            uint32_t count;
            if (!xr_lir_class_payload(m,a,&a) || xr_lir_aggregate_fields(m,a,&fields,&count) != XR_LIR_OK ||
                v->bits >= count || v->type != XR_LIR_UNIT) return false;
            const XrLirRecordField *field = &fields[v->bits];
            if (!(field->flags & XR_LIR_RECORD_MUTABLE)) return false;
            if (v->op == XR_LIR_CLASS_SCALAR_WRITE) return field->type == b && xr_lir_class_scalar_field_supported(m,b);
            return field->type == XR_LIR_ARRAY && xr_lir_class_array_element_supported(field->array_element) &&
                (v->op == XR_LIR_CLASS_ARRAY_PUSH ? b == field->array_element :
                    b == XR_LIR_ARRAY && f->values[v->operands[1]-1].array_element == field->array_element);
        }
        case XR_LIR_FUNCTION_SCOPE_ENTER:
            return v->type == XR_LIR_FUNCTION_SCOPE && v->operand_count <= 1 &&
                (!v->operand_count || a == XR_LIR_FUNCTION_SCOPE);
        case XR_LIR_FUNCTION_SCOPE_LEAVE: {
            XrLirCallableType signature;
            return a == XR_LIR_FUNCTION_SCOPE && (v->operand_count == 1 ? v->type == XR_LIR_UNIT :
                v->operand_count == 2 && v->type == b && (xr_lir_callable_type_view(m, b, &signature) == XR_LIR_OK ||
                    xr_lir_cell_value_type(m,b,NULL)));
        }
        case XR_LIR_FUNCTION_BIND: {
            XrLirCallableType signature;
            return a == XR_LIR_FUNCTION_SCOPE && v->type == b && v->bits < UINT32_MAX &&
                (xr_lir_callable_type_view(m, b, &signature) == XR_LIR_OK ||
                    xr_lir_cell_value_type(m,b,NULL));
        }
        case XR_LIR_FIXED_ARRAY_NEW: {
            XrLirTypeView shape;
            if (xr_lir_type_view(m, v->type, &shape) != XR_LIR_OK ||
                shape.kind != XR_LIR_TYPE_FIXED_ARRAY ||
                v->operand_count != shape.count) return false;
            for (uint32_t p = 0; p < v->operand_count; ++p)
                if (f->values[v->operands[p] - 1].type != shape.element) return false;
            return true;
        }
        case XR_LIR_FIXED_ARRAY_GET: {
            XrLirTypeView shape;
            return v->type == XR_LIR_U8 && b == XR_LIR_I64 &&
                xr_lir_type_view(m, a, &shape) == XR_LIR_OK && shape.kind == XR_LIR_TYPE_FIXED_ARRAY;
        }
        case XR_LIR_SUM_NEW: {
            const XrLirRecordField *fields = NULL; uint32_t field_count = 0;
            const XrLirSumVariant *variants = NULL; uint32_t variant_count = 0;
            if (!xr_lir_sum_variants(m, v->type, &fields, &field_count, &variants, &variant_count) ||
                v->bits >= variant_count) return false;
            const XrLirSumVariant *variant = &variants[v->bits];
            if (v->operand_count != variant->field_count) return false;
            for (uint32_t n = 0; n < variant->field_count; ++n) {
                const XrLirValueData *input = &f->values[v->operands[n] - 1];
                XrLirRecordField field = fields[variant->field_begin + n];
                if (input->type != field.type || input->array_element != field.array_element) return false;
            }
            return true;
        }
        case XR_LIR_SUM_TAG: {
            const XrLirRecordField *fields = NULL; uint32_t field_count = 0;
            const XrLirSumVariant *variants = NULL; uint32_t variant_count = 0;
            return v->operand_count == 1 && v->type == XR_LIR_I64 &&
                xr_lir_sum_variants(m, a, &fields, &field_count, &variants, &variant_count);
        }
        case XR_LIR_SUM_GET: {
            const XrLirRecordField *fields = NULL; uint32_t field_count = 0;
            const XrLirSumVariant *variants = NULL; uint32_t variant_count = 0;
            return v->operand_count == 1 &&
                xr_lir_sum_variants(m, a, &fields, &field_count, &variants, &variant_count) &&
                v->bits < field_count && fields[v->bits].type == v->type;
        }
        case XR_LIR_CONST:
            return lir_constant_valid(v->type, v->bits);
        case XR_LIR_NULLABLE_NONE: {
            XrLirTypeView shape;
            return xr_lir_type_view(m, v->type, &shape) == XR_LIR_OK && shape.kind == XR_LIR_TYPE_NULLABLE;
        }
        case XR_LIR_NULLABLE_SOME: {
            XrLirTypeView shape;
            return xr_lir_type_view(m, v->type, &shape) == XR_LIR_OK && shape.kind == XR_LIR_TYPE_NULLABLE &&
                (shape.element == XR_LIR_UNIT ? v->operand_count == 0 : v->operand_count == 1 && a == shape.element &&
                    (a != XR_LIR_ARRAY || f->values[v->operands[0] - 1].array_element ==
                        xr_lir_nullable_array_element(m, v->type)));
        }
        case XR_LIR_NULLABLE_IS_SOME: case XR_LIR_NULLABLE_UNWRAP: {
            XrLirTypeView shape;
            return xr_lir_type_view(m, a, &shape) == XR_LIR_OK && shape.kind == XR_LIR_TYPE_NULLABLE &&
                v->type == (v->op == XR_LIR_NULLABLE_IS_SOME ? XR_LIR_I1 : shape.element);
        }
        case XR_LIR_CELL_NEW: {
            XrLirCellType cell;
            if (xr_lir_cell_type_view(m, v->type, &cell) == XR_LIR_OK)
                return (cell.payload == XR_LIR_I64 || cell.payload == XR_LIR_MAP_STRING_I64) && a == cell.payload;
            return (v->type == XR_LIR_I64_CELL && a == XR_LIR_I64) ||
                (v->type == XR_LIR_STRING_CELL && a == XR_LIR_STRING) ||
                (v->type == XR_LIR_MAP_STRING_I64_CELL && a == XR_LIR_MAP_STRING_I64) ||
                (v->type == XR_LIR_SET_I64_CELL && a == XR_LIR_SET_I64) ||
                (v->type == XR_LIR_ARRAY_CELL && a == XR_LIR_ARRAY);
        }
        case XR_LIR_CELL_READ: {
            XrLirCellType cell;
            if (xr_lir_cell_type_view(m, a, &cell) == XR_LIR_OK)
                return (cell.payload == XR_LIR_I64 || cell.payload == XR_LIR_MAP_STRING_I64) && v->type == cell.payload;
            return (v->type == XR_LIR_I64 && (a == XR_LIR_I64_CELL || a == XR_LIR_I64_REF)) ||
                (v->type == XR_LIR_STRING && a == XR_LIR_STRING_CELL) ||
                (v->type == XR_LIR_MAP_STRING_I64 && a == XR_LIR_MAP_STRING_I64_CELL) ||
                (v->type == XR_LIR_SET_I64 && a == XR_LIR_SET_I64_CELL) ||
                (v->type == XR_LIR_ARRAY && a == XR_LIR_ARRAY_CELL);
        }
        case XR_LIR_CELL_WRITE: {
            XrLirCellType cell;
            if (xr_lir_cell_type_view(m, a, &cell) == XR_LIR_OK)
                return (cell.payload == XR_LIR_I64 || cell.payload == XR_LIR_MAP_STRING_I64) &&
                    b == cell.payload && v->type == XR_LIR_UNIT;
            return v->type == XR_LIR_UNIT && (((a == XR_LIR_I64_CELL || a == XR_LIR_I64_REF) && b == XR_LIR_I64) ||
                (a == XR_LIR_STRING_CELL && b == XR_LIR_STRING) ||
                (a == XR_LIR_MAP_STRING_I64_CELL && b == XR_LIR_MAP_STRING_I64) ||
                (a == XR_LIR_SET_I64_CELL && b == XR_LIR_SET_I64) ||
                (a == XR_LIR_ARRAY_CELL && b == XR_LIR_ARRAY));
        }
        case XR_LIR_SET_NEW:return v->type==XR_LIR_SET_I64;
        case XR_LIR_SET_LEN:return v->type==XR_LIR_I64 && a==XR_LIR_SET_I64;
        case XR_LIR_SET_CONTAINS:return v->type==XR_LIR_I1 && a==XR_LIR_SET_I64 && b==XR_LIR_I64;
        case XR_LIR_SET_DISPLAY:return v->type==XR_LIR_STRING && a==XR_LIR_SET_I64;
        case XR_LIR_SET_ADD:case XR_LIR_SET_REMOVE:
            return v->type==(XrLirType)(v->op==XR_LIR_SET_ADD?XR_LIR_UNIT:XR_LIR_I1) &&
                (a==XR_LIR_SET_I64_CELL || a==XR_LIR_SET_I64_REF) && b==XR_LIR_I64;
        case XR_LIR_SET_CLEAR:return v->type==XR_LIR_UNIT && (a==XR_LIR_SET_I64_CELL || a==XR_LIR_SET_I64_REF);
        case XR_LIR_SET_RETAIN:return v->type==XR_LIR_SET_I64 && a==XR_LIR_SET_I64;
        case XR_LIR_SET_DROP:return v->type==XR_LIR_UNIT && a==XR_LIR_SET_I64;
        case XR_LIR_SET_CELL_DROP:return v->type==XR_LIR_UNIT && a==XR_LIR_SET_I64_CELL;
        case XR_LIR_SET_BORROW:return v->type==XR_LIR_SET_I64_REF && (a==XR_LIR_SET_I64_CELL ||
            (a==XR_LIR_STATE_REF && f->values[v->operands[0]-1].state_reference.payload==XR_LIR_SET_I64 && f->values[v->operands[0]-1].state_reference.mutable));
        case XR_LIR_SET_REF_READ:return v->type==XR_LIR_SET_I64 && a==XR_LIR_SET_I64_REF;
        case XR_LIR_SET_REF_WRITE:return v->type==XR_LIR_UNIT && a==XR_LIR_SET_I64_REF && b==XR_LIR_SET_I64;
        case XR_LIR_MAP_RETAIN: return v->type == XR_LIR_MAP_STRING_I64 && a == XR_LIR_MAP_STRING_I64;
        case XR_LIR_MAP_DROP: return v->type == XR_LIR_UNIT && a == XR_LIR_MAP_STRING_I64;
        case XR_LIR_MAP_CELL_DROP: return v->type == XR_LIR_UNIT && a == XR_LIR_MAP_STRING_I64_CELL;
        case XR_LIR_MAP_BORROW: {
            XrLirCellType cell;
            return v->type == XR_LIR_MAP_STRING_I64_REF && (a == XR_LIR_MAP_STRING_I64_CELL ||
                (xr_lir_cell_type_view(m, a, &cell) == XR_LIR_OK && cell.payload == XR_LIR_MAP_STRING_I64) ||
                (a == XR_LIR_STATE_REF && f->values[v->operands[0] - 1].state_reference.payload == XR_LIR_MAP_STRING_I64 &&
                 f->values[v->operands[0] - 1].state_reference.mutable));
        }
        case XR_LIR_MAP_REF_READ: return v->type == XR_LIR_MAP_STRING_I64 && a == XR_LIR_MAP_STRING_I64_REF;
        case XR_LIR_MAP_REF_WRITE:
            return v->type == XR_LIR_UNIT && a == XR_LIR_MAP_STRING_I64_REF && b == XR_LIR_MAP_STRING_I64;
        case XR_LIR_ARRAY_RETAIN: return v->type == XR_LIR_ARRAY && a == XR_LIR_ARRAY;
        case XR_LIR_ARRAY_DROP: return v->type == XR_LIR_UNIT && a == XR_LIR_ARRAY;
        case XR_LIR_ARRAY_CELL_DROP: return v->type == XR_LIR_UNIT && a == XR_LIR_ARRAY_CELL;
        case XR_LIR_ARRAY_BORROW:
            return v->type == XR_LIR_ARRAY_REF && (a == XR_LIR_ARRAY_CELL ||
                (a == XR_LIR_STATE_REF && f->values[v->operands[0] - 1].state_reference.payload == XR_LIR_ARRAY &&
                 f->values[v->operands[0] - 1].state_reference.mutable));
        case XR_LIR_ARRAY_REF_READ: return v->type == XR_LIR_ARRAY && a == XR_LIR_ARRAY_REF;
        case XR_LIR_ARRAY_REF_WRITE:
            return v->type == XR_LIR_UNIT && a == XR_LIR_ARRAY_REF && b == XR_LIR_ARRAY;
        case XR_LIR_STRING_I64:
            return v->type == XR_LIR_STRING && a == XR_LIR_I64;
        case XR_LIR_STRING_F64:
            return v->type == XR_LIR_STRING && a == XR_LIR_F64;
        case XR_LIR_STRING_CONCAT:
            return v->type == XR_LIR_STRING && a == XR_LIR_STRING && b == XR_LIR_STRING;
        case XR_LIR_STRING_REPLACE:
            return v->type == XR_LIR_STRING && v->operand_count == 3 && a == XR_LIR_STRING &&
                b == XR_LIR_STRING && f->values[v->operands[2] - 1].type == XR_LIR_STRING && v->bits <= 1;
        case XR_LIR_STRING_SLICE:
            return v->type == XR_LIR_STRING && a == XR_LIR_STRING && b == XR_LIR_I64 &&
                f->values[v->operands[2] - 1].type == XR_LIR_I64;
        case XR_LIR_STRING_INDEX_OF:
            return v->type == XR_LIR_I64 && a == XR_LIR_STRING && b == XR_LIR_STRING &&
                f->values[v->operands[2] - 1].type == XR_LIR_I64;
        case XR_LIR_STRING_ORDER:
            return v->type == XR_LIR_I1 && v->operand_count == 2 && a == XR_LIR_STRING &&
                b == XR_LIR_STRING && v->predicate >= XR_LIR_LT && v->predicate <= XR_LIR_GE;
        case XR_LIR_STRING_LAST_INDEX_OF:
            return v->type == XR_LIR_I64 && v->operand_count == 2 && a == XR_LIR_STRING && b == XR_LIR_STRING;
        case XR_LIR_STRING_TEST:
            return v->type == XR_LIR_I1 && v->operand_count == 2 && a == XR_LIR_STRING &&
                b == XR_LIR_STRING && v->bits <= 2;
        case XR_LIR_ASSERT_CONDITION:
            return v->type == XR_LIR_UNIT && a == XR_LIR_I1 && b == XR_LIR_STRING;
        case XR_LIR_STRING_RETAIN:
            return v->type == XR_LIR_STRING && a == XR_LIR_STRING;
        case XR_LIR_HOST_PATH_BOOL: {
            uint32_t paths = v->bits <= UINT32_MAX ? xr_native_blocking_path_count((uint32_t)v->bits) : 0;
            return paths && v->operand_count == paths && v->type == XR_LIR_I1 &&
                a == XR_LIR_STRING && (paths == 1 || b == XR_LIR_STRING);
        }
        case XR_LIR_HOST_FILE_OPEN_WRITE: case XR_LIR_HOST_PATH_CLASS: {
            XrLirTypeView shape;
            bool writing = v->op == XR_LIR_HOST_FILE_OPEN_WRITE;
            if (a != XR_LIR_STRING || v->bits > UINT32_MAX) return false;
            uint32_t id = (uint32_t)v->bits;
            uint32_t storage = writing ? XR_NATIVE_DECLARATION_FILE_STORAGE : xr_native_host_path_result_class(id);
            return storage && (!writing || (b == XR_LIR_I1 && xr_native_host_shape(id) == XR_NATIVE_HOST_FILE_OPEN_WRITE)) &&
                xr_lir_type_view(m, v->type, &shape) == XR_LIR_OK && shape.kind == XR_LIR_TYPE_NULLABLE &&
                xr_lir_class_storage(m, shape.element) == storage;
        }
        case XR_LIR_HOST_FILE_READ:
            return xr_lir_class_storage(m, a) == XR_NATIVE_DECLARATION_FILE_STORAGE &&
                b == XR_LIR_I64 && v->bits <= UINT32_MAX &&
                xr_native_host_shape((uint32_t)v->bits) == XR_NATIVE_HOST_FILE_READ &&
                xr_lir_nullable_array_element(m, v->type) == XR_LIR_U8;
        case XR_LIR_HOST_ENV_GET: {
            XrLirTypeView shape;
            return a == XR_LIR_STRING && v->bits <= UINT32_MAX &&
                xr_native_host_shape((uint32_t)v->bits) == XR_NATIVE_HOST_ENV_GET &&
                xr_lir_type_view(m, v->type, &shape) == XR_LIR_OK &&
                shape.kind == XR_LIR_TYPE_NULLABLE && shape.element == XR_LIR_STRING;
        }
        case XR_LIR_HOST_FILE_CLOSE:
            return v->type == XR_LIR_I1 && xr_lir_class_storage(m, a) == XR_NATIVE_DECLARATION_FILE_STORAGE &&
                v->bits <= UINT32_MAX && xr_native_host_shape((uint32_t)v->bits) == XR_NATIVE_HOST_FILE_CLOSE;
        case XR_LIR_HOST_FILE_WRITE: {
            if (v->type != XR_LIR_I64 || xr_lir_class_storage(m, a) != XR_NATIVE_DECLARATION_FILE_STORAGE ||
                v->operand_count != 3 || f->values[v->operands[2] - 1].type != XR_LIR_I64 || v->bits > UINT32_MAX)
                return false;
            XrNativeHostShape shape = xr_native_host_shape((uint32_t)v->bits);
            return (shape == XR_NATIVE_HOST_FILE_WRITE_STRING && b == XR_LIR_STRING) ||
                (shape == XR_NATIVE_HOST_FILE_WRITE_BYTES && b == XR_LIR_ARRAY &&
                    f->values[v->operands[1] - 1].array_element == XR_LIR_U8);
        }
        case XR_LIR_STRING_LEN: case XR_LIR_STRING_BYTE_LEN:
            return v->type == XR_LIR_I64 && a == XR_LIR_STRING;
        case XR_LIR_I64_TRY_PARSE: {
            XrLirTypeView shape;
            return a == XR_LIR_STRING &&
                xr_lir_type_view(m, v->type, &shape) == XR_LIR_OK &&
                shape.kind == XR_LIR_TYPE_NULLABLE && shape.element == XR_LIR_I64;
        }
        case XR_LIR_F64_TRY_PARSE: {
            XrLirTypeView shape;
            return a == XR_LIR_STRING &&
                xr_lir_type_view(m, v->type, &shape) == XR_LIR_OK &&
                shape.kind == XR_LIR_TYPE_NULLABLE && shape.element == XR_LIR_F64;
        }
        case XR_LIR_STRING_DROP: case XR_LIR_STRING_PRINT:
            return v->type == XR_LIR_UNIT && a == XR_LIR_STRING;
        case XR_LIR_STRING_CELL_DROP:
            return v->type == XR_LIR_UNIT && a == XR_LIR_STRING_CELL;
        case XR_LIR_ADD:
        case XR_LIR_SUB:
        case XR_LIR_MUL:
        case XR_LIR_DIV:
        case XR_LIR_REM:
        case XR_LIR_BIT_AND: case XR_LIR_BIT_OR: case XR_LIR_BIT_XOR:
            return lir_integer(v->type) && a == v->type && b == v->type;
        case XR_LIR_SHL: case XR_LIR_SHR:
            return lir_integer(v->type) && a == v->type && lir_integer(b);
        case XR_LIR_NEG:
            return lir_integer(v->type) && a == v->type;
        case XR_LIR_FADD:
        case XR_LIR_FSUB:
        case XR_LIR_FMUL:
        case XR_LIR_FDIV:
            return lir_float(v->type) && a == v->type && b == v->type;
        case XR_LIR_FNEG:
            return lir_float(v->type) && a == v->type;
        case XR_LIR_ICMP:
            return v->type == XR_LIR_I1 && a == b &&
                   (lir_integer(a) || ((a == XR_LIR_I1 || a == XR_LIR_PTR) &&
                                       (v->predicate == XR_LIR_EQ || v->predicate == XR_LIR_NE)));
        case XR_LIR_FCMP:
            return v->type == XR_LIR_I1 && lir_float(a) && a == b;
        case XR_LIR_STRING_EQUAL:
            /* One owned string backing on each side. A synchronous borrow is not
             * admissible here: the reference pass refuses it and neither backend
             * has a comparison that reads through a place. */
            return v->type == XR_LIR_I1 && a == XR_LIR_STRING && b == XR_LIR_STRING &&
                   (v->predicate == XR_LIR_EQ || v->predicate == XR_LIR_NE);
        case XR_LIR_INTCAST:
            return lir_integer(a) && lir_integer(v->type);
        case XR_LIR_ITOF:
            return lir_integer(a) && lir_float(v->type);
        case XR_LIR_FTOI:
            return a == XR_LIR_F64 && v->type == XR_LIR_I64;
        case XR_LIR_FCAST:
            return lir_float(a) && lir_float(v->type);
        case XR_LIR_BR:
            return v->type == XR_LIR_UNIT;
        case XR_LIR_CONDBR:
            return v->type == XR_LIR_UNIT && a == XR_LIR_I1;
        case XR_LIR_RET:
            return v->type == XR_LIR_UNIT &&
                   v->operand_count == (f->result == XR_LIR_UNIT ? 0u : 1u) &&
                   (f->result == XR_LIR_UNIT || a == f->result);
        default:
            return false;
    }
}

static bool lir_edges_valid(const XrLirModule *m, const XrLirFunctionData *f, const XrLirInstructionView *v) {
    const XrLirFunctionData *callee = v->op == XR_LIR_INVOKE && v->callee && v->callee <= m->function_count ?
        &m->functions[v->callee - 1] : NULL;
    if (v->op == XR_LIR_INVOKE && (!callee || !callee->error || v->edges[0].target == v->edges[1].target))
        return false;
    for (uint32_t e = 0; e < v->edge_count; ++e) {
        const XrLirEdgeView *edge = &v->edges[e];
        if (!edge->target || edge->target > f->block_count ||
            (edge->argument_count && !edge->arguments))
            return false;
        const XrLirBlockData *target = &f->blocks[edge->target - 1];
        if (callee) {
            XrLirType produced = e ? callee->error : callee->result;
            uint32_t parameter = target->parameter_count && target->parameters ? target->parameters[0] : 0;
            if (edge->argument_count || target->parameter_count != (produced != XR_LIR_UNIT ? 1u : 0u) ||
                (produced != XR_LIR_UNIT && (!parameter || parameter > f->value_count ||
                    f->values[parameter - 1].type != produced))) return false;
            if (produced == XR_LIR_ARRAY &&
                f->values[parameter - 1].array_element != callee->array_result) return false;
            continue;
        }
        if (edge->argument_count != target->parameter_count ||
            (target->parameter_count && !target->parameters))
            return false;
        for (uint32_t i = 0; i < edge->argument_count; ++i) {
            uint32_t id = edge->arguments[i], parameter = target->parameters[i];
            if (!id || id > f->value_count || !parameter || parameter > f->value_count ||
                f->values[id - 1].type != f->values[parameter - 1].type)
                return false;
            if (xr_lir_internal_array_type(f->values[id - 1].type) &&
                f->values[id - 1].array_element != f->values[parameter - 1].array_element) return false;
        }
    }
    return true;
}

static bool lir_literal_call(const XrLirModule *m, const XrLirFunctionData *f,
    const XrLirInstructionView *v) {
    if (v->operand_count != 2) return false;
    const XrLirInstructionView *address = xr_lir_internal_value_definition(f, v->operands[0]);
    const XrLirInstructionView *length = xr_lir_internal_value_definition(f, v->operands[1]);
    return address && address->op == XR_LIR_LITERAL_ADDR && address->bits && address->bits <= m->literal_count &&
        length && length->op == XR_LIR_CONST && length->type == XR_LIR_U64 &&
        length->bits == m->literals[address->bits - 1].length;
}

XR_FUNC XrLirStatus xr_lir_internal_instruction_check(const XrLirModule *m, uint32_t function,
                                                      const XrLirInstructionView *v,
                                                      XrLirDiagnostic *d) {
    if (d)
        memset(d, 0, sizeof(*d));
    if (!m || !function || function > m->function_count || !v)
        return lir_status_error(d, XR_LIR_INVALID_ARGUMENT,
                                "invalid LIR instruction check argument");
    const XrLirFunctionData *f = &m->functions[function - 1];
    const char *message = "instruction type or signature mismatch";
    if (v->op <= XR_LIR_OP_INVALID || v->op >= XR_LIR_OP_COUNT ||
        v->op == XR_LIR_VIEW_FRAME_ENTER || v->op == XR_LIR_VIEW_FRAME_LEAVE ||
        !xr_lir_internal_type_valid(m, v->type, true) || v->type == XR_LIR_I64_REF)
        goto invalid;
    if ((v->operand_count && !v->operands) ||
        (lir_arities[v->op] >= 0 && v->operand_count != (uint32_t) lir_arities[v->op]))
        goto invalid;
    for (uint32_t i = 0; i < v->operand_count; ++i)
        if (!v->operands[i] || v->operands[i] > f->value_count) {
            message = "instruction uses an undefined value";
            goto invalid;
        }
    bool comparison = v->op == XR_LIR_ICMP || v->op == XR_LIR_FCMP || v->op == XR_LIR_STRING_ORDER;
    bool equality = v->op == XR_LIR_STRING_EQUAL;
    if (comparison ? v->predicate < XR_LIR_EQ || v->predicate > XR_LIR_GE
                   : equality ? v->predicate != XR_LIR_EQ && v->predicate != XR_LIR_NE
                              : v->predicate != XR_LIR_PREDICATE_NONE)
        goto invalid;
    if (v->op != XR_LIR_CONST && v->op != XR_LIR_LITERAL_ADDR &&
        v->op != XR_LIR_STRING_LITERAL && v->op != XR_LIR_MODULE_BEGIN &&
        v->op != XR_LIR_MODULE_READY && v->op != XR_LIR_STATE_CONSTRUCT &&
        v->op != XR_LIR_STATE_INITIALIZE && v->op != XR_LIR_STATE_REFERENCE &&
        v->op != XR_LIR_FUNCTION_REF && v->op != XR_LIR_FUNCTION_BIND &&
        v->op != XR_LIR_RECORD_FIELD && v->op != XR_LIR_CLASS_FIELD &&
        v->op != XR_LIR_CLASS_IS &&
        v->op != XR_LIR_PANIC_ERROR && v->op != XR_LIR_UNCAUGHT_ERROR &&
        v->op != XR_LIR_SUM_NEW && v->op != XR_LIR_SUM_GET && v->op != XR_LIR_HOST_PATH_BOOL &&
        v->op != XR_LIR_HOST_FILE_OPEN_WRITE && v->op != XR_LIR_HOST_FILE_CLOSE && v->op != XR_LIR_HOST_FILE_WRITE &&
        v->op != XR_LIR_HOST_PATH_CLASS && v->op != XR_LIR_HOST_FILE_READ && v->op != XR_LIR_HOST_ENV_GET &&
        v->op != XR_LIR_CLASS_SCALAR_WRITE && v->op != XR_LIR_CLASS_ARRAY_WRITE && v->op != XR_LIR_CLASS_ARRAY_PUSH &&
        v->op != XR_LIR_STATE_GROUP_INITIALIZE && v->op != XR_LIR_SLICE_CLEANUP &&
        v->op != XR_LIR_SLICE_FORMAL_CLEANUP && v->op != XR_LIR_MAP_LITERAL_NEW &&
        v->op != XR_LIR_STRING_TEST && v->op != XR_LIR_STRING_REPLACE && v->bits)
        goto invalid;
    if ((v->op != XR_LIR_CALL && v->op != XR_LIR_INVOKE && v->op != XR_LIR_FUNCTION_REF && v->callee) ||
        (v->op != XR_LIR_CALL_INTRINSIC && v->intrinsic))
        goto invalid;
    if (v->op == XR_LIR_SLICE_CLEANUP || v->op == XR_LIR_SLICE_FORMAL_CLEANUP) {
        bool target = v->op == XR_LIR_SLICE_FORMAL_CLEANUP ?
            lir_slice_formal_cleanup_target(m, f, v->bits) : lir_slice_cleanup_target(m, f, v->bits);
        if (v->type != XR_LIR_UNIT || v->edge_count || v->result || !target) goto invalid;
        return XR_LIR_OK;
    }
    uint32_t edges = v->op == XR_LIR_BR ? 1 : (v->op == XR_LIR_CONDBR || v->op == XR_LIR_INVOKE ? 2 : 0);
    if (v->edge_count != edges || !lir_edges_valid(m, f, v)) {
        message = "control flow edge target or parameter signature mismatch";
        goto invalid;
    }
    for (uint32_t a = 0; a < v->operand_count; ++a) {
        const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, v->operands[a]);
        if (definition && definition->op == XR_LIR_LITERAL_ADDR &&
            !(v->op == XR_LIR_CALL && !a && v->callee && v->callee <= m->function_count &&
              m->functions[v->callee - 1].kind == XR_LIR_FUNCTION_EXTERNAL &&
              m->functions[v->callee - 1].extern_kind == XR_LIR_EXTERN_PRINT_STRING)) goto invalid;
    }
    for (uint32_t e = 0; e < v->edge_count; ++e)
        for (uint32_t a = 0; a < v->edges[e].argument_count; ++a) {
            const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, v->edges[e].arguments[a]);
            if (definition && definition->op == XR_LIR_LITERAL_ADDR) goto invalid;
        }
    if (v->op == XR_LIR_LITERAL_ADDR) {
        if (v->type != XR_LIR_PTR || !v->bits || v->bits > m->literal_count) goto invalid;
        return XR_LIR_OK;
    }
    if (v->op == XR_LIR_STRING_LITERAL) {
        if (v->type != XR_LIR_STRING || !v->bits || v->bits > m->literal_count) goto invalid;
        return XR_LIR_OK;
    }
    if (v->op == XR_LIR_FUNCTION_REF || v->op == XR_LIR_CALL_INDIRECT || v->op == XR_LIR_FUNCTION_WEAKEN) {
        if ((v->op==XR_LIR_FUNCTION_REF && m->startup_entry && v->callee==m->startup_entry) ||
            (v->op==XR_LIR_FUNCTION_REF && v->callee && v->callee<=m->function_count &&
             m->functions[v->callee-1].cleanup_owner) ||
            !xr_lir_internal_callable_instruction(m, f, v)) goto invalid;
        return XR_LIR_OK;
    }
    if (v->op==XR_LIR_RETURN_ERROR || v->op==XR_LIR_PANIC_ERROR || v->op==XR_LIR_UNCAUGHT_ERROR) {
        XrLirRecordIdentity identity={0};
        XrLirType error=f->values[v->operands[0]-1].type;
        if (v->type!=XR_LIR_UNIT || !xr_lir_error_sum_identity(m,error,&identity)) goto invalid;
        if (v->op==XR_LIR_RETURN_ERROR) {
            if (f->error!=error) goto invalid;
            return XR_LIR_OK;
        }
        if (v->bits>UINT32_MAX || !xr_lir_error_literal_matches(m,error,(uint32_t)v->bits)) goto invalid;
        if (v->op==XR_LIR_PANIC_ERROR) {
            if (!v->source.path) goto invalid;
        } else {
            XrLirSource where=v->source;
            if (function!=m->startup_entry || where.path || where.path_length ||
                where.line || where.column || where.end_line || where.end_column) goto invalid;
        }
        return XR_LIR_OK;
    }
    if (v->op == XR_LIR_CALL || v->op == XR_LIR_INVOKE) {
        if (!v->callee || v->callee > m->function_count)
            goto invalid;
        const XrLirFunctionData *callee = &m->functions[v->callee - 1];
        if ((callee->error != 0) != (v->op == XR_LIR_INVOKE) || v->callee==m->startup_entry ||
            (callee->cleanup_owner && callee->cleanup_owner != function) ||
            (v->op==XR_LIR_INVOKE && (!!callee->initializer_module!=(function==m->startup_entry)))) goto invalid;
        for (uint32_t p = 0; p < callee->callable_parameter_count; ++p) {
            XrLirFunctionCallableParameter parameter = callee->callable_parameters[p];
            if ((parameter.contract.escaping || parameter.parameter < callee->closure.capture_count) &&
                parameter.parameter < v->operand_count &&
                !xr_lir_internal_callable_persistent(m, f, v->operands[parameter.parameter])) goto invalid;
        }
        if (!lir_signature_matches(m, f, v, callee->parameter_types, callee->parameter_count,
                                   callee->result))
            goto invalid;
        if (v->op == XR_LIR_INVOKE)
            for (uint32_t a = 0; a < v->operand_count; ++a) {
                const XrLirValueData *actual = &f->values[v->operands[a] - 1];
                if (actual->type == XR_LIR_STRING && (!actual->parameter || actual->block)) goto invalid;
            }
        if (callee->kind == XR_LIR_FUNCTION_EXTERNAL && callee->extern_kind == XR_LIR_EXTERN_PRINT_STRING &&
            !lir_literal_call(m, f, v)) goto invalid;
        if (!xr_lir_internal_array_instruction(m, f, v)) goto invalid;
        return XR_LIR_OK;
    }
    if (v->op == XR_LIR_CALL_INTRINSIC) {
        for (uint32_t i = 0; i < m->intrinsic_count; ++i) {
            const XrLirIntrinsic *entry = &m->intrinsics[i];
            if (entry->id != v->intrinsic)
                continue;
            if (xr_lir_map_intrinsic_valid(m, entry)) {
                if (entry->operation == XR_LIR_MAP_OP_NEW) {
                    if (v->type != XR_LIR_MAP_STRING_I64 || v->operand_count % 2) goto invalid;
                    for (uint32_t p = 0; p < v->operand_count; ++p) {
                        XrLirType expected = p % 2 ? XR_LIR_I64 : XR_LIR_STRING;
                        if (f->values[v->operands[p] - 1].type != expected) goto invalid;
                    }
                } else if (!lir_signature_matches(m, f, v, entry->parameters, entry->parameter_count, entry->result) ||
                    !xr_lir_internal_array_instruction(m, f, v)) goto invalid;
                return XR_LIR_OK;
            }
            if (xr_lir_range_intrinsic_valid(entry)) {
                if (!lir_signature_matches(m, f, v, entry->parameters, entry->parameter_count, entry->result)) goto invalid;
                return XR_LIR_OK;
            }
            if (xr_lir_string_intrinsic_valid(entry)) {
                if (!lir_signature_matches(m, f, v, entry->parameters, entry->parameter_count, entry->result)) goto invalid;
                if ((entry->operation == XR_LIR_STRING_OP_APPEND_STATE || entry->operation == XR_LIR_STRING_OP_BORROW_STATE)) {
                    XrLirStateReference shape = f->values[v->operands[0] - 1].state_reference;
                    if (shape.payload != XR_LIR_STRING || !shape.mutable) goto invalid;
                }
                return XR_LIR_OK;
            }
            if (entry->operation == XR_LIR_INTRINSIC_DISPLAY) {
                if (!xr_lir_display_intrinsic_valid(entry) || v->operand_count != 1 ||
                    v->type != XR_LIR_STRING ||
                    !xr_lir_display_type(m, f->values[v->operands[0] - 1].type)) goto invalid;
                return XR_LIR_OK;
            }
            if (xr_lir_internal_slice_intrinsic_valid(m, entry)) {
                XrLirSliceIntrinsic slice;
                if (!lir_signature_matches(m, f, v, entry->parameters, entry->parameter_count, entry->result) ||
                    xr_lir_slice_intrinsic_view(m, entry->id, &slice) != XR_LIR_OK) goto invalid;
                if (slice.operation == XR_LIR_SLICE_OP_STRICT_UTF8 &&
                    (!f->strict_utf8 || f->parameter_count != 1 || v->operands[0] != f->parameters[0])) goto invalid;
                if (slice.operation == XR_LIR_SLICE_OP_NEW) {
                    const XrLirValueData *binding = &f->values[v->operands[0] - 1];
                    if (slice.binding_type == XR_LIR_STATE_REF) {
                        if (binding->state_reference.payload != XR_LIR_ARRAY ||
                            binding->state_reference.array_element != slice.element) goto invalid;
                    } else if (slice.binding_type == XR_LIR_STRING_CELL) {
                        const XrLirInstructionView *root = xr_lir_internal_value_definition(f, v->operands[0]);
                        if (slice.element != XR_LIR_U8 || binding->array_element || v->operand_count != 1 ||
                            !root || root->op != XR_LIR_CELL_NEW) goto invalid;
                    } else if (binding->array_element != slice.element) goto invalid;
                }
                return XR_LIR_OK;
            }
            if (entry->operation) {
                goto invalid;
            }
            const XrLirArrayIntrinsic *shape = xr_lir_internal_array_intrinsic(m, v->intrinsic);
            if (shape) {
                if (!lir_array_intrinsic_signature(m, f, v, entry, shape) ||
                    !xr_lir_internal_array_instruction(m, f, v)) goto invalid;
                return XR_LIR_OK;
            }
            bool cell = entry->result >= XR_LIR_I64_CELL;
            for (uint32_t p = 0; p < entry->parameter_count; ++p)
                cell |= entry->parameters[p] >= XR_LIR_I64_CELL;
            if (cell || !entry->scalar_sync ||
                !lir_signature_matches(m, f, v, entry->parameters, entry->parameter_count,
                                       entry->result))
                goto invalid;
            return XR_LIR_OK;
        }
        xr_lir_internal_error(d, "intrinsic is not registered", function, 0, 0, &v->source);
        if (d)
            d->status = XR_LIR_UNSUPPORTED;
        return XR_LIR_UNSUPPORTED;
    }
    if (((v->op >= XR_LIR_MODULE_BEGIN && v->op <= XR_LIR_STATE_WRITE) ||
         v->op == XR_LIR_STATE_GROUP_INITIALIZE) ? !lir_state_instruction_types(m, f, v) :
        !lir_scalar_instruction_types(m, f, v))
        goto invalid;
    if (!xr_lir_internal_array_instruction(m, f, v)) goto invalid;
    return XR_LIR_OK;
invalid:
    return xr_lir_internal_error(d, message, function, 0, 0, &v->source);
}

static bool lir_definition(const XrLirFunctionData *f, uint8_t *seen, uint32_t value,
                           XrLirType type, uint32_t block, uint32_t instruction, bool parameter) {
    if (!value || value > f->value_count || seen[value - 1])
        return false;
    const XrLirValueData *v = &f->values[value - 1];
    if (v->type != type || v->block != block || v->instruction != instruction ||
        v->parameter != parameter)
        return false;
    seen[value - 1] = 1;
    return true;
}

static XrLirStatus lir_definitions(const XrLirModule *m, uint32_t index, uint8_t *seen,
                                   bool *scopes, XrLirDiagnostic *d) {
    const XrLirFunctionData *f = &m->functions[index];
    uint32_t scope_count = 0;
    for (uint32_t p = 0; p < f->parameter_count; ++p) {
        *scopes |= xr_lir_cell_value_type(m,f->parameter_types[p],NULL);
        if (!lir_definition(f, seen, f->parameters[p], f->parameter_types[p], 0, p, true))
            return xr_lir_internal_error(d, "function parameter definition is inconsistent",
                                         index + 1, 0, 0, NULL);
    }
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirBlockData *block = &f->blocks[b];
        if ((block->parameter_count && !block->parameters) || !block->instructions ||
            !block->instruction_count || block->instruction_count > block->instruction_capacity ||
            !block->terminated)
            return xr_lir_internal_error(d, "block has no complete terminator", index + 1, b + 1, 0,
                                         NULL);
        for (uint32_t p = 0; p < block->parameter_count; ++p) {
            uint32_t id = block->parameters[p];
            if (!id || id > f->value_count ||
                f->values[id - 1].type == XR_LIR_FUNCTION_SCOPE || f->values[id - 1].type == XR_LIR_I64_REF ||
                !xr_lir_internal_type_valid(m, f->values[id - 1].type, false) ||
                !lir_definition(f, seen, id, f->values[id - 1].type, b + 1, p, true))
                return xr_lir_internal_error(d, "block parameter definition is inconsistent",
                                             index + 1, b + 1, 0, NULL);
            *scopes |= xr_lir_cell_value_type(m,f->values[id - 1].type,NULL);
        }
        for (uint32_t i = 0; i < block->instruction_count; ++i) {
            const XrLirInstructionView *v = &block->instructions[i].view;
            *scopes |= v->op == XR_LIR_FUNCTION_SCOPE_ENTER || v->op == XR_LIR_FUNCTION_SCOPE_LEAVE ||
                v->op == XR_LIR_FUNCTION_BIND || xr_lir_cell_value_type(m,v->type,NULL);
            scope_count += v->op == XR_LIR_FUNCTION_SCOPE_ENTER;
            if (v->op == XR_LIR_CELL_NEW && b + 1 != f->entry && v->type != XR_LIR_I64_CELL && v->type != XR_LIR_ARRAY_CELL &&
                v->type != XR_LIR_MAP_STRING_I64_CELL && v->type != XR_LIR_SET_I64_CELL &&
                !xr_lir_cell_value_type(m,v->type,NULL))
                return xr_lir_internal_error(d, "Cell storage requires the activation entry block",
                                             index + 1, b + 1, i + 1, &v->source);
            bool terminal = xr_lir_internal_terminal(v->op);
            if (terminal != (i == block->instruction_count - 1))
                return xr_lir_internal_error(d, "terminator must be the final instruction",
                                             index + 1, b + 1, i + 1, &v->source);
            XrLirStatus status = xr_lir_internal_instruction_check(m, index + 1, v, d);
            if (status != XR_LIR_OK) {
                if (d) {
                    d->status = status;
                    d->function = index + 1;
                    d->block = b + 1;
                    d->instruction = i + 1;
                }
                return status;
            }
            if (terminal || v->type == XR_LIR_UNIT) {
                if (!v->result)
                    continue;
            } else if (lir_definition(f, seen, v->result, v->type, b + 1, i, false))
                continue;
            return xr_lir_internal_error(d, "instruction result definition is inconsistent",
                                         index + 1, b + 1, i + 1, &v->source);
        }
    }
    if (scope_count != f->scope_count)
        return xr_lir_internal_error(d, "scope producer count is inconsistent", index + 1, 0, 0, NULL);
    for (uint32_t v = 0; v < f->value_count; ++v)
        if (!seen[v])
            return xr_lir_internal_error(d, "value has no definition", index + 1, 0, 0, NULL);
    return XR_LIR_OK;
}

typedef struct LirGraph {
    uint32_t count, entry;
    uint32_t *memory, *offsets, *predecessors, *order, *rank, *idom, *stack, *next, *cursor;
    uint8_t *multiplicity;
    uint32_t *cleanup_rank;
    bool owns_multiplicity;
} LirGraph;

static const XrLirInstructionView *lir_terminator(const XrLirFunctionData *f, uint32_t block) {
    const XrLirBlockData *b = &f->blocks[block];
    XR_DCHECK(b->instruction_count > 0, "validated block has a terminator");
    return &b->instructions[b->instruction_count - 1].view;
}

static XrLirStatus lir_graph_create(const XrLirFunctionData *f, LirGraph *g) {
    uint64_t edge_count = 0;
    for (uint32_t b = 0; b < f->block_count; ++b)
        edge_count += lir_terminator(f, b)->edge_count;
    uint64_t words = (uint64_t) f->block_count * 7 + edge_count + 1;
    if (edge_count > UINT32_MAX || words > SIZE_MAX / sizeof(uint32_t))
        return XR_LIR_OUT_OF_MEMORY;
    g->memory = xr_malloc((size_t) words * sizeof(*g->memory));
    if (!g->memory)
        return XR_LIR_OUT_OF_MEMORY;
    memset(g->memory, 0, (size_t) words * sizeof(*g->memory));
    g->count = f->block_count;
    g->entry = f->entry - 1;
    g->offsets = g->memory;
    g->predecessors = g->offsets + g->count + 1;
    g->order = g->predecessors + (size_t) edge_count;
    g->rank = g->order + g->count;
    g->idom = g->rank + g->count;
    g->stack = g->idom + g->count;
    g->next = g->stack + g->count;
    g->cursor = g->next + g->count;
    for (uint32_t b = 0; b < g->count; ++b) {
        g->rank[b] = UINT32_MAX;
        g->idom[b] = UINT32_MAX;
        const XrLirInstructionView *term = lir_terminator(f, b);
        for (uint32_t e = 0; e < term->edge_count; ++e) {
            uint32_t target = term->edges[e].target - 1;
            if (target == g->entry)
                return XR_LIR_VERIFY_ERROR;
            ++g->offsets[target + 1];
        }
    }
    for (uint32_t b = 0; b < g->count; ++b) {
        g->offsets[b + 1] += g->offsets[b];
        g->cursor[b] = g->offsets[b];
    }
    for (uint32_t b = 0; b < g->count; ++b) {
        const XrLirInstructionView *term = lir_terminator(f, b);
        for (uint32_t e = 0; e < term->edge_count; ++e) {
            uint32_t target = term->edges[e].target - 1;
            g->predecessors[g->cursor[target]++] = b;
        }
    }
    return XR_LIR_OK;
}

static bool lir_graph_order(const XrLirFunctionData *f, LirGraph *g) {
    uint32_t depth = 1, count = 0;
    g->stack[0] = g->entry;
    g->rank[g->entry] = 0;
    while (depth) {
        uint32_t b = g->stack[depth - 1];
        const XrLirInstructionView *term = lir_terminator(f, b);
        if (g->next[b] < term->edge_count) {
            uint32_t target = term->edges[g->next[b]++].target - 1;
            if (g->rank[target] == UINT32_MAX) {
                g->rank[target] = 0;
                g->stack[depth++] = target;
            }
        } else {
            g->order[count++] = b;
            --depth;
        }
    }
    if (count != g->count)
        return false;
    for (uint32_t i = 0; i < count / 2; ++i) {
        uint32_t saved = g->order[i];
        g->order[i] = g->order[count - i - 1];
        g->order[count - i - 1] = saved;
    }
    for (uint32_t i = 0; i < count; ++i)
        g->rank[g->order[i]] = i;
    return true;
}

static bool lir_startup_publication(const XrLirModule *m,const XrLirFunctionData *f,
    uint32_t block,const XrLirInstructionView *call) {
    if (!m->state_layout_set) return true;
    const XrLirBlockData *from=&f->blocks[block],*normal=&f->blocks[call->edges[0].target-1];
    uint32_t owner=m->functions[call->callee-1].initializer_module-1;
    if (from->instruction_count<2) return false;
    const XrLirInstructionView *begin=&from->instructions[from->instruction_count-2].view;
    const XrLirInstructionView *ready=&normal->instructions[0].view;
    uint32_t state=f->parameters[0];
    return begin->op==XR_LIR_MODULE_BEGIN && ready->op==XR_LIR_MODULE_READY &&
        begin->bits==owner && ready->bits==owner &&
        begin->operands[0]==state && ready->operands[0]==state &&
        (!call->operand_count || call->operands[0]==state);
}

static bool lir_invoke_landing_view_cleanup(const XrLirBlockData *failed) {
    for (uint32_t i = 0; i + 1 < failed->instruction_count; ++i) {
        XrLirOp op = failed->instructions[i].view.op;
        if (op != XR_LIR_SLICE_CLEANUP && op != XR_LIR_SLICE_FORMAL_CLEANUP) return false;
    }
    return true;
}

static bool lir_optional_error_branch(const XrLirFunctionData *f,const XrLirInstructionView *branch) {
    const XrLirBlockData *absent=&f->blocks[branch->edges[0].target-1];
    if (absent->instruction_count!=2) return false;
    const XrLirInstructionView *none=&absent->instructions[0].view;
    const XrLirInstructionView *join=&absent->instructions[1].view;
    return none->op==XR_LIR_NULLABLE_NONE && join->op==XR_LIR_BR &&
        join->edges[0].argument_count && join->edges[0].arguments[0]==none->result;
}

/* A dedicated landing block can still dominate later blocks. Check their
 * operands and edge arguments before accepting an intentionally unused E. */
static bool lir_discarded_errors_unused(const XrLirFunctionData *f,const uint8_t *discarded) {
    for (uint32_t b=0;b<f->block_count;++b) {
        const XrLirBlockData *block=&f->blocks[b];
        for (uint32_t i=0;i<block->instruction_count;++i) {
            const XrLirInstructionView *op=&block->instructions[i].view;
            for (uint32_t a=0;a<op->operand_count;++a)
                if (discarded[op->operands[a]-1]) return false;
            for (uint32_t e=0;e<op->edge_count;++e)
                for (uint32_t a=0;a<op->edges[e].argument_count;++a)
                    if (discarded[op->edges[e].arguments[a]-1]) return false;
        }
    }
    return true;
}

static bool lir_catch_receives_error(const XrLirBlockData *failed,
    const XrLirInstructionView *branch) {
    if (branch->op != XR_LIR_BR || failed->parameter_count != 1) return false;
    const XrLirEdgeView *edge = &branch->edges[0];
    return edge->argument_count && edge->arguments[0] == failed->parameters[0];
}

static bool lir_invoke_edges(const XrLirModule *m,uint32_t function,const XrLirFunctionData *f,
    const LirGraph *g,uint8_t *discarded) {
    bool startup=function==m->startup_entry,optional=false;
    if (f->strict_utf8) {
        const XrLirInstructionView *decode = &f->blocks[f->entry - 1].instructions[0].view;
        XrLirSliceIntrinsic role;
        if (decode->op != XR_LIR_CALL_INTRINSIC ||
            xr_lir_slice_intrinsic_view(m, decode->intrinsic, &role) != XR_LIR_OK ||
            role.operation != XR_LIR_SLICE_OP_STRICT_UTF8) return false;
    }
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirInstructionView *term = lir_terminator(f,b);
        if (startup && term->op!=XR_LIR_INVOKE && term->op!=XR_LIR_UNCAUGHT_ERROR && term->op!=XR_LIR_RET)
            return false;
        if (term->op == XR_LIR_INVOKE) {
            for (uint32_t e = 0; e < 2; ++e) {
                uint32_t target = term->edges[e].target - 1;
                if (g->offsets[target + 1] - g->offsets[target] != 1) return false;
            }
            uint32_t target=term->edges[1].target-1;
            const XrLirBlockData *failed=&f->blocks[target];
            const XrLirInstructionView *exit=lir_terminator(f,target);
            if (failed->parameter_count!=1) return false;
            if (startup) {
                if (exit->op!=XR_LIR_UNCAUGHT_ERROR || failed->instruction_count!=1 ||
                    !lir_startup_publication(m,f,b,term)) return false;
            } else if (exit->op==XR_LIR_BR) {
                /* A catch receives this exact edge result as its first parameter.
                 * The landing may first retire temporary Slice loans, or run a
                 * proved cleanup suffix that shares the call's frontier. */
                if (lir_catch_receives_error(failed, exit)) {
                    if (failed->instruction_count == 1) continue;
                    if (lir_invoke_landing_view_cleanup(failed)) continue;
                    const XrLirCleanupBlock *source = f->blocks[b].cleanup;
                    if (f->has_cleanups && source && source->frontier && failed->cleanup &&
                        failed->cleanup->frontier == source->frontier) continue;
                    return false;
                }
                if (!lir_invoke_landing_view_cleanup(failed)) return false;
                if (!lir_optional_error_branch(f,exit)) return false;
                if (!optional) memset(discarded,0,f->value_count);
                optional=true;
                discarded[failed->parameters[0]-1]=1;
                continue;
            } else if (exit->op!=XR_LIR_PANIC_ERROR && exit->op!=XR_LIR_RETURN_ERROR) {
                return false;
            }
            if (exit->operands[0]!=failed->parameters[0]) return false;
        }
        if (term->op!=XR_LIR_PANIC_ERROR && term->op!=XR_LIR_UNCAUGHT_ERROR) continue;
        const XrLirBlockData *block = &f->blocks[b];
        if (block->parameter_count != 1 || term->operands[0] != block->parameters[0] ||
            g->offsets[b + 1] - g->offsets[b] != 1) return false;
        const XrLirInstructionView *source = lir_terminator(f,g->predecessors[g->offsets[b]]);
        if (source->op != XR_LIR_INVOKE || source->edges[1].target != b + 1) return false;
    }
    return !optional || lir_discarded_errors_unused(f,discarded);
}

static uint32_t lir_intersect(const LirGraph *g, uint32_t a, uint32_t b) {
    while (a != b) {
        while (g->rank[a] > g->rank[b])
            a = g->idom[a];
        while (g->rank[b] > g->rank[a])
            b = g->idom[b];
    }
    return a;
}

static void lir_graph_dominators(LirGraph *g) {
    g->idom[g->entry] = g->entry;
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t i = 1; i < g->count; ++i) {
            uint32_t b = g->order[i], parent = UINT32_MAX;
            for (uint32_t p = g->offsets[b]; p < g->offsets[b + 1]; ++p) {
                uint32_t predecessor = g->predecessors[p];
                if (g->idom[predecessor] == UINT32_MAX)
                    continue;
                parent = parent == UINT32_MAX ? predecessor : lir_intersect(g, parent, predecessor);
            }
            if (parent != g->idom[b]) {
                g->idom[b] = parent;
                changed = true;
            }
        }
    }
}

static bool lir_dominates_use(const XrLirFunctionData *f, const LirGraph *g, uint32_t id,
                              uint32_t block, uint32_t instruction) {
    const XrLirValueData *value = &f->values[id - 1];
    if (!value->block)
        return value->parameter;
    uint32_t definition = value->block - 1;
    if (definition == block)
        return value->parameter || value->instruction < instruction;
    while (g->rank[block] > g->rank[definition])
        block = g->idom[block];
    return block == definition;
}

static XrLirStatus lir_uses(const XrLirFunctionData *f, uint32_t function, const LirGraph *g,
                            XrLirDiagnostic *d) {
    for (uint32_t b = 0; b < f->block_count; ++b)
        for (uint32_t i = 0; i < f->blocks[b].instruction_count; ++i) {
            const XrLirInstructionView *v = &f->blocks[b].instructions[i].view;
            for (uint32_t a = 0; a < v->operand_count; ++a)
                if (!lir_dominates_use(f, g, v->operands[a], b, i))
                    return xr_lir_internal_error(d, "value definition does not dominate its use",
                                                 function, b + 1, i + 1, &v->source);
            for (uint32_t e = 0; e < v->edge_count; ++e)
                for (uint32_t a = 0; a < v->edges[e].argument_count; ++a)
                    if (!lir_dominates_use(f, g, v->edges[e].arguments[a], b, i)) {
                        XrLirStatus status = xr_lir_internal_error(
                            d, "edge argument definition does not dominate its use", function,
                            b + 1, i + 1, &v->source);
                        if (d)
                            d->edge = e + 1;
                        return status;
                    }
        }
    return XR_LIR_OK;
}
static XrLirStatus lir_frame_cell_constructors(const XrLirFunctionData *f,uint32_t function,
    const LirGraph *graph,XrLirDiagnostic *diagnostic) {
    if (!graph->multiplicity) return XR_LIR_VERIFY_ERROR;
    for (uint32_t b=0;b<f->block_count;++b) {
        if (!graph->multiplicity[b]) continue;
        const XrLirBlockData *block=f->blocks+b;
        for (uint32_t i=0;i<block->instruction_count;++i) {
            const XrLirInstructionView *v=&block->instructions[i].view;
            if (v->op==XR_LIR_CELL_NEW && v->type==XR_LIR_I64_CELL)
                return xr_lir_internal_error(diagnostic,"Frame scalar Cell construction may repeat in this activation",
                    function,b+1,i+1,&v->source);
        }
    }
    return XR_LIR_OK;
}

#include "xlir_cleanup_verify.inc.c"

static bool lir_function_shape(const XrLirModule *m, const XrLirFunctionData *f) {
    if (f->result == XR_LIR_FUNCTION_SCOPE || f->result == XR_LIR_MAP_STRING_I64_CELL ||
        f->result == XR_LIR_MAP_STRING_I64_REF || f->result == XR_LIR_SET_I64_CELL || f->result == XR_LIR_SET_I64_REF) return false;
    if (!f->name || !f->name_length || !xr_lir_internal_type_valid(m, f->result, true) ||
        f->result == XR_LIR_I64_CELL || f->result == XR_LIR_I64_REF || f->result == XR_LIR_STRING_REF || f->result == XR_LIR_STRING_CELL ||
        f->result == XR_LIR_STATE || f->result == XR_LIR_STATE_REF ||
        f->value_count > f->value_capacity || (f->value_count && !f->values) ||
        (f->parameter_count && (!f->parameters || !f->parameter_types)) ||
        !xr_lir_internal_callable_parameters_valid(m, f->parameter_types, f->parameter_count,
            f->callable_parameters, f->callable_parameter_count, false) ||
        !xr_lir_internal_closure_target_valid(m, f))
        return false;
    uint32_t function = (uint32_t)(f - m->functions) + 1;
    bool startup=m->startup_entry==function;
    if (f->strict_utf8 && (startup || m->state_entry == function || f->initializer_module ||
        f->kind != XR_LIR_FUNCTION_DEFINITION || !f->error || f->result != XR_LIR_STRING ||
        f->parameter_count != 1 || f->cleanup_owner || f->closure.callable)) return false;
    if (startup && (f->kind!=XR_LIR_FUNCTION_DEFINITION || f->initializer_module ||
        f->error || f->closure.callable || f->parameter_count!=(uint32_t)m->state_layout_set ||
        (m->state_layout_set && (f->parameter_types[0]!=XR_LIR_STATE || m->state_entry!=m->startup_entry))))
        return false;
    if (f->initializer_module && (!m->startup_entry || f->initializer_module>m->startup_module_count ||
        !f->error || f->result!=XR_LIR_UNIT || f->kind!=XR_LIR_FUNCTION_DEFINITION ||
        f->parameter_count>1 || (f->parameter_count && f->parameter_types[0]!=XR_LIR_STATE))) return false;
    if (f->cleanup_owner) {
        if (f->cleanup_owner >= function || f->kind != XR_LIR_FUNCTION_DEFINITION ||
            f->result != XR_LIR_UNIT || f->error || f->initializer_module || f->closure.callable ||
            f->callable_parameter_count || startup || m->state_entry == function ||
            !lir_cleanup_registration(m, f->cleanup_owner, function)) return false;
        for (uint32_t p = 0; p < f->parameter_count; ++p) {
            XrLirType type = f->parameter_types[p];
            if (type != XR_LIR_I64 && type != XR_LIR_I1 && type != XR_LIR_I64_REF) return false;
        }
    } else if (f->cleanup_registration) return false;
    for (uint32_t p = 0; p < f->parameter_count; ++p)
        if (!xr_lir_internal_type_valid(m, f->parameter_types[p], false) ||
            f->parameter_types[p] == XR_LIR_FUNCTION_SCOPE ||
            (f->parameter_types[p] == XR_LIR_I64_REF && (f->kind != XR_LIR_FUNCTION_DEFINITION || f->closure.callable)))
            return false;
    if (f->kind == XR_LIR_FUNCTION_EXTERNAL) {
        if (f->defined || f->block_count || f->entry) return false;
        if (f->extern_kind == XR_LIR_EXTERN_ASSERT_CONDITION)
            return xr_lir_assertion_entry_signature(f->parameter_types, f->parameter_count, f->result);
        const char *clock = clock_external_name(f->extern_kind);
        if (clock) return f->result == XR_LIR_I64 &&
            f->parameter_count == (f->extern_kind == XR_LIR_EXTERN_UTC_OFFSET_AT ? 1u : 0u) &&
            (!f->parameter_count || f->parameter_types[0] == XR_LIR_I64) &&
            f->name_length == strlen(clock) && !memcmp(f->name, clock, f->name_length);
        if (f->result != XR_LIR_UNIT) return false;
        if (f->extern_kind == XR_LIR_EXTERN_PRINT_STRING)
            return f->parameter_count == 2 && f->parameter_types[0] == XR_LIR_PTR &&
                f->parameter_types[1] == XR_LIR_U64 && f->name_length == sizeof("xrt_print_string") - 1 &&
                !memcmp(f->name, "xrt_print_string", sizeof("xrt_print_string") - 1);
        return f->extern_kind == XR_LIR_EXTERN_PRINT_I64 && f->parameter_count == 1 &&
            f->parameter_types[0] == XR_LIR_I64 && f->name_length == sizeof("xrt_print_i64") - 1 &&
            !memcmp(f->name, "xrt_print_i64", sizeof("xrt_print_i64") - 1);
    }
    return f->kind == XR_LIR_FUNCTION_DEFINITION && f->extern_kind == XR_LIR_EXTERN_NONE &&
           f->defined && f->entry && f->entry <= f->block_count && f->blocks &&
           f->block_count <= f->block_capacity && !f->blocks[f->entry - 1].parameter_count;
}

static XrLirStatus lir_literal_consumers(const XrLirFunctionData *f, uint32_t function,
    uint8_t *uses, XrLirDiagnostic *d) {
    if (f->value_count) memset(uses, 0, f->value_count);
    for (uint32_t b = 0; b < f->block_count; ++b)
        for (uint32_t i = 0; i < f->blocks[b].instruction_count; ++i) {
            const XrLirInstructionView *v = &f->blocks[b].instructions[i].view;
            for (uint32_t a = 0; a < v->operand_count; ++a) {
                uint32_t id = v->operands[a];
                const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, id);
                if (definition && definition->op == XR_LIR_LITERAL_ADDR && uses[id - 1] < 2)
                    ++uses[id - 1];
            }
        }
    for (uint32_t id = 1; id <= f->value_count; ++id) {
        const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, id);
        if (definition && definition->op == XR_LIR_LITERAL_ADDR && uses[id - 1] != 1) {
            const XrLirValueData *value = &f->values[id - 1];
            return xr_lir_internal_error(d, "literal address requires one direct output consumer",
                function, value->block, value->instruction + 1, &definition->source);
        }
    }
    return XR_LIR_OK;
}

static bool lir_managed_value(const XrLirModule *m, XrLirType type) {
    /* Lexical Cell identities have COPY edges and scope retirement; they do
     * not use this ordinary block-local owner's consuming edge protocol. */
    return type == XR_LIR_STRING || type == XR_LIR_ARRAY || type == XR_LIR_MAP_STRING_I64 ||
        type == XR_LIR_SET_I64 || xr_lir_boxed_type(m, type);
}
static bool lir_managed_cell(XrLirType type) {
    return type == XR_LIR_STRING_CELL || type == XR_LIR_ARRAY_CELL || type == XR_LIR_MAP_STRING_I64_CELL || type==XR_LIR_SET_I64_CELL;
}
static bool lir_borrowed_set_formal(const XrLirFunctionData *f, uint32_t value) {
    const XrLirValueData *row = &f->values[value - 1];
    return row->type == XR_LIR_SET_I64 && row->parameter && !row->block;
}

static bool lir_frame_cell_owner(const XrLirFunctionData *f, uint32_t id) {
    const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, id);
    return definition && definition->op == XR_LIR_CELL_NEW && lir_managed_cell(definition->type);
}

/* String Cells retain their entry-frame lifetime. Array Cells use the actual
 * CFG availability analysis because their constructors may run inside loops. */
static XrLirStatus lir_frame_cells(const XrLirModule *m, const XrLirFunctionData *f, uint32_t function,
    uint8_t *released, XrLirDiagnostic *d) {
    for (uint32_t id = 1; id <= f->value_count; ++id) {
        const XrLirValueData *value = &f->values[id - 1];
        if (value->type != XR_LIR_STRING_CELL) continue;
        if (!lir_frame_cell_owner(f, id) && (!value->parameter || value->block))
            return xr_lir_internal_error(d, "Managed Cell requires an entry owner or incoming borrow",
                function, value->block, 0, NULL);
    }
    for (uint32_t b = 0; b < f->block_count; ++b) {
        memset(released, 0, f->value_count);
        const XrLirBlockData *block = &f->blocks[b];
        const XrLirInstructionView *term = lir_terminator(f, b);
        for (uint32_t i = 0; i < block->instruction_count; ++i) {
            const XrLirInstructionView *v = &block->instructions[i].view;
            for (uint32_t a = 0; a < v->operand_count; ++a) {
                uint32_t id = v->operands[a];
                if (f->values[id - 1].type != XR_LIR_STRING_CELL) continue;
                if (released[id - 1])
                    return xr_lir_internal_error(d, "Managed Cell use follows release", function, b+1, i+1, &v->source);
                XrLirSliceIntrinsic slice;
                bool bytes = a == 0 && v->op == XR_LIR_CALL_INTRINSIC &&
                    xr_lir_slice_intrinsic_view(m, v->intrinsic, &slice) == XR_LIR_OK &&
                    slice.operation == XR_LIR_SLICE_OP_NEW && slice.binding_type == XR_LIR_STRING_CELL;
                if (v->op != XR_LIR_CELL_READ && v->op != XR_LIR_CELL_WRITE &&
                    v->op != XR_LIR_CALL && v->op != XR_LIR_STRING_CELL_DROP &&
                    xr_lir_string_role(m, v) != XR_LIR_STRING_OP_APPEND_CELL &&
                    xr_lir_string_role(m, v) != XR_LIR_STRING_OP_BORROW_CELL && !bytes)
                    return xr_lir_internal_error(d, "Managed Cell handle cannot escape its frame",
                        function, b+1, i+1, &v->source);
            }
            if (v->op == XR_LIR_STRING_CELL_DROP) {
                uint32_t id = v->operands[0];
                if (!lir_frame_cell_owner(f, id))
                    return xr_lir_internal_error(d, "incoming Managed Cell borrow cannot be released",
                        function, b+1, i+1, &v->source);
                if (f->block_count != 1 && !xr_lir_function_exit(term->op))
                    return xr_lir_internal_error(d, "frame Managed Cell release requires a normal return",
                        function, b+1, i+1, &v->source);
                released[id - 1] = 1;
            }
        }
        for (uint32_t e = 0; e < term->edge_count; ++e)
            for (uint32_t a = 0; a < term->edges[e].argument_count; ++a)
                if (f->values[term->edges[e].arguments[a] - 1].type == XR_LIR_STRING_CELL)
                    return xr_lir_internal_error(d, "Managed Cell handle cannot transfer on an edge",
                        function, b+1, 0, &term->source);
        if (xr_lir_function_exit(term->op))
            for (uint32_t id = 1; id <= f->value_count; ++id)
                if (f->values[id - 1].type == XR_LIR_STRING_CELL &&
                    lir_frame_cell_owner(f, id) && !released[id - 1])
                    return xr_lir_internal_error(d, "frame Managed Cell requires release on every normal return",
                        function, b+1, 0, &term->source);
    }
    return XR_LIR_OK;
}
#include "xlir_array_cell_flow.inc.c"
#include "xlir_string_verify.inc.c"
#include "xlir_ownership.inc.c"

/* Each block owns its managed parameters and instruction results. Boxed values
 * and direct references use the same selected-edge transfer rule. Physical Cell
 * handles retain their activation's lifetime. */
static XrLirStatus lir_managed_owners(const XrLirModule *m, const XrLirFunctionData *f, uint32_t function,
    const LirGraph *g, uint8_t *owned, XrLirDiagnostic *d) {
    bool managed = false, cells = false, array_cells = false, string_references = false;
    for (uint32_t i = 0; i < f->value_count; ++i) {
        managed |= lir_managed_value(m, f->values[i].type) || lir_managed_cell(f->values[i].type);
        cells |= f->values[i].type == XR_LIR_STRING_CELL;
        array_cells |= f->values[i].type == XR_LIR_ARRAY_CELL || f->values[i].type == XR_LIR_ARRAY_REF ||
            f->values[i].type == XR_LIR_MAP_STRING_I64_CELL || f->values[i].type == XR_LIR_MAP_STRING_I64_REF ||
            f->values[i].type == XR_LIR_SET_I64_CELL || f->values[i].type == XR_LIR_SET_I64_REF;
        string_references |= f->values[i].type == XR_LIR_STRING_REF;
    }
    if (string_references) {
        XrLirStatus references = lir_string_references(m, f, function, owned, d);
        if (references != XR_LIR_OK) return references;
    }
    if (!managed && !array_cells) return XR_LIR_OK;
    if (cells) {
        XrLirStatus status = lir_frame_cells(m, f, function, owned, d);
        if (status != XR_LIR_OK) return status;
    }
    if (array_cells) {
        XrLirStatus status = lir_array_cells(f, function, g, d);
        if (status != XR_LIR_OK) return status;
    }
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirInstructionView *term = lir_terminator(f, b);
        for (uint32_t e = 0; e < term->edge_count; ++e)
            if (g->rank[term->edges[e].target - 1] <= g->rank[b]) {
                for (uint32_t a = 0; a < term->edges[e].argument_count; ++a) {
                    XrLirType type = f->values[term->edges[e].arguments[a] - 1].type;
                    if (lir_managed_cell(type))
                        return xr_lir_internal_error(d, "loop-carried managed transfer is not supported", function, b + 1, 0, &term->source);
                }
            }
    }
    for (uint32_t b = 0; b < f->block_count; ++b) {
        memset(owned, 0, f->value_count);
        const XrLirBlockData *block = &f->blocks[b];
        for (uint32_t p = 0; p < block->parameter_count; ++p)
            if (lir_managed_value(m, f->values[block->parameters[p] - 1].type)) {
                owned[block->parameters[p] - 1] = 1;
            }
        for (uint32_t i = 0; i < block->instruction_count; ++i) {
            const XrLirInstructionView *v = &block->instructions[i].view;
            bool transfer = lir_plan_transfer(m, f, v) != 0;
            for (uint32_t a = 0; a < v->operand_count; ++a) {
                uint32_t id = v->operands[a];
                const XrLirValueData *value = &f->values[id - 1];
                if (lir_managed_value(m, value->type) &&
                    (!value->parameter || value->block) && !owned[id - 1])
                    return xr_lir_internal_error(d, "Managed use follows release or precedes ownership", function, b + 1, i + 1, &v->source);
            }
            if (transfer) {
                uint32_t id = v->operands[0];
                if (!owned[id - 1])
                    return xr_lir_internal_error(d, "Managed transfer requires an owned reference", function, b + 1, i + 1, &v->source);
                owned[id - 1] = 0;
            }
            if (v->result && lir_managed_value(m, v->type)) {
                owned[v->result - 1] = 1;
            }
        }
        const XrLirInstructionView *term = lir_terminator(f, b);
        for (uint32_t e = 0; e < term->edge_count; ++e) {
            const XrLirEdgeView *edge = &term->edges[e];
            for (uint32_t a = 0; a < edge->argument_count; ++a) {
                uint32_t id = edge->arguments[a];
                if (!lir_managed_value(m, f->values[id - 1].type)) continue;
                bool borrowed = lir_borrowed_set_formal(f, id);
                if (!owned[id - 1] && !borrowed)
                    return xr_lir_internal_error(d, "Managed edge requires a distinct owned reference", function, b + 1, 0, &term->source);
                for (uint32_t p = 0; p < a; ++p)
                    if (!borrowed && edge->arguments[p] == id)
                        return xr_lir_internal_error(d, "Managed edge duplicates one owned reference", function, b + 1, 0, &term->source);
            }
            for (uint32_t id = 1; id <= f->value_count; ++id) {
                if (!owned[id - 1]) continue;
                bool transferred = false;
                for (uint32_t a = 0; a < edge->argument_count; ++a)
                    transferred |= edge->arguments[a] == id;
                if (!transferred)
                    return xr_lir_internal_error(d, "Managed owner is not released or transferred on every edge", function, b + 1, 0, &term->source);
            }
        }
        if (!term->edge_count)
            for (uint32_t id = 0; id < f->value_count; ++id)
                if (owned[id])
                    return xr_lir_internal_error(d, "Managed owner requires release or result transfer", function, b + 1, 0, NULL);
    }
    return XR_LIR_OK;
}

#include "xlir_callable_facts.inc.c"
#include "xlir_slice_facts.inc.c"
#include "xlir_cell_verify.inc.c"
#include "xlir_callable_scope_verify.inc.c"
#include "xlir_slice_verify.inc.c"

typedef struct LirStartupRole {
    uint32_t initializer, call_block, ready_block;
} LirStartupRole;

static XrLirStatus lir_startup_boundaries(const XrLirModule *m,const LirStartupRole *roles,XrLirDiagnostic *d) {
    if (!m->state_layout_set) return XR_LIR_OK;
    for (uint32_t f=0;f<m->function_count;++f) {
        const XrLirFunctionData *function=&m->functions[f];
        for (uint32_t b=0;b<function->block_count;++b) {
            const XrLirBlockData *block=&function->blocks[b];
            for (uint32_t i=0;i<block->instruction_count;++i) {
                const XrLirInstructionView *op=&block->instructions[i].view;
                if (op->op!=XR_LIR_MODULE_BEGIN && op->op!=XR_LIR_MODULE_READY) continue;
                const LirStartupRole *role=&roles[op->bits];
                if (!role->initializer) continue;
                bool position=op->op==XR_LIR_MODULE_BEGIN ?
                    role->call_block==b+1 && i==block->instruction_count-2 :
                    role->ready_block==b+1 && i==0;
                if (f+1!=m->startup_entry || !position)
                    return xr_lir_internal_error(d,"typed module publication is outside its startup invoke",
                        f+1,b+1,i+1,&op->source);
            }
        }
    }
    return XR_LIR_OK;
}

static XrLirStatus lir_startup_inventory(const XrLirModule *m,XrLirDiagnostic *d) {
    if (!m->startup_entry) return XR_LIR_OK;
    LirStartupRole *roles=xr_calloc(m->startup_module_count,sizeof(*roles));
    if (!roles) return lir_status_error(d,XR_LIR_OUT_OF_MEMORY,"startup role verification allocation failed");
    bool good=true;
    uint32_t remaining=0;
    for (uint32_t f=0;good && f<m->function_count;++f) {
        uint32_t owner=m->functions[f].initializer_module;
        if (!owner) continue;
        if (roles[owner-1].initializer) good=false;
        roles[owner-1].initializer=f+1;
        ++remaining;
    }
    const XrLirFunctionData *startup=&m->functions[m->startup_entry-1];
    good=good && remaining!=0;
    for (uint32_t b=0;good && b<startup->block_count;++b) {
        const XrLirInstructionView *term=lir_terminator(startup,b);
        if (term->op!=XR_LIR_INVOKE) continue;
        uint32_t owner=m->functions[term->callee-1].initializer_module;
        LirStartupRole *role=owner ? &roles[owner-1] : NULL;
        if (!role || role->initializer!=term->callee || role->call_block) good=false;
        else {
            role->call_block=b+1;
            role->ready_block=term->edges[0].target;
            --remaining;
        }
    }
    XrLirStatus status=good && !remaining ? lir_startup_boundaries(m,roles,d) :
        xr_lir_internal_error(d,"startup requires one invoke per typed initializer",m->startup_entry,0,0,NULL);
    xr_free(roles);
    return status;
}

static XrLirStatus lir_verify_structure(const XrLirModule *m, XrLirDiagnostic *d, XrLirCallableFacts **facts,
    XrLirBlockConstructionFacts *multiplicity) {
    if (d)
        memset(d, 0, sizeof(*d));
    if (!m || m->failure != XR_LIR_OK || m->function_count > m->function_capacity ||
        (m->function_count && !m->functions))
        return lir_status_error(d, XR_LIR_INVALID_ARGUMENT, "invalid LIR module");
    if (m->literal_count > m->literal_capacity || (m->literal_count && !m->literals))
        return lir_status_error(d, XR_LIR_INVALID_ARGUMENT, "invalid literal inventory");
    if (!xr_lir_internal_types_valid(m) || !xr_lir_internal_map_literals_valid(m))
        return lir_status_error(d, XR_LIR_VERIFY_ERROR, "constructed type pool is invalid");
    if (m->slice_facts && !lir_slice_effects_valid(m,m->slice_facts->view))
        return lir_status_error(d,XR_LIR_VERIFY_ERROR,"Slice receiving summary is invalid");
    uint32_t array_operations[XR_LIR_ARRAY_FAMILY_COUNT][XR_LIR_ARRAY_OP_COPY_STRING_BYTES] = {{0}};
    uint32_t array_ref_operations[XR_LIR_ARRAY_FAMILY_COUNT][2] = {{0}};
    if (m->intrinsic_count && !m->intrinsics)
        return lir_status_error(d, XR_LIR_VERIFY_ERROR, "intrinsic registry is missing");
    for (uint32_t i = 0; i < m->intrinsic_count; ++i) {
        const XrLirIntrinsic *entry = &m->intrinsics[i];
        if (!entry->id || !entry->scalar_sync || !xr_lir_internal_type_valid(m, entry->result, true) ||
            entry->result == XR_LIR_I64_REF ||
            (entry->parameter_count && !entry->parameters))
            return lir_status_error(d, XR_LIR_VERIFY_ERROR, "intrinsic signature is invalid");
        bool array = xr_lir_internal_array_type(entry->result);
        if (!entry->operation && (entry->result == XR_LIR_MAP_STRING_I64_REF || entry->result == XR_LIR_STRING_REF || entry->result == XR_LIR_VIEW_FRAME ||
            entry->result == XR_LIR_VIEW_CELL || entry->result == XR_LIR_SLICE_I64 || xr_lir_internal_slice_type(m, entry->result)))
            return lir_status_error(d, XR_LIR_VERIFY_ERROR, "generic intrinsic cannot produce a view capability");
        for (uint32_t p = 0; p < entry->parameter_count; ++p) {
            if (!xr_lir_internal_type_valid(m, entry->parameters[p], false) || entry->parameters[p] == XR_LIR_I64_REF)
                return lir_status_error(d, XR_LIR_VERIFY_ERROR, "intrinsic parameter type is invalid");
            array |= xr_lir_internal_array_type(entry->parameters[p]);
            if (!entry->operation && (entry->parameters[p] == XR_LIR_MAP_STRING_I64_REF || entry->parameters[p] == XR_LIR_STRING_REF || entry->parameters[p] == XR_LIR_VIEW_FRAME ||
                entry->parameters[p] == XR_LIR_VIEW_CELL || entry->parameters[p] == XR_LIR_SLICE_I64 ||
                xr_lir_internal_slice_type(m, entry->parameters[p])))
                return lir_status_error(d, XR_LIR_VERIFY_ERROR, "generic intrinsic cannot receive a view capability");
        }
        const XrLirArrayIntrinsic *shape = m->array_intrinsics ? &m->array_intrinsics[i] : NULL;
        bool empty = !shape || (!shape->id && !shape->parameter_elements && !shape->parameter_count &&
            !shape->result_element && !shape->variadic && !shape->operation);
        if (entry->operation) {
            if (!empty || (!xr_lir_internal_slice_intrinsic_valid(m, entry) &&
                !xr_lir_display_intrinsic_valid(entry) &&
                !xr_lir_string_intrinsic_valid(entry) && !xr_lir_range_intrinsic_valid(entry) &&
                !xr_lir_map_intrinsic_valid(m, entry)))
                return lir_status_error(d, XR_LIR_VERIFY_ERROR, "view intrinsic signature is invalid");
            continue;
        }
        if (!m->array_intrinsics) {
            if (array) return lir_status_error(d, XR_LIR_VERIFY_ERROR, "Array intrinsic shape is missing");
            continue;
        }
        if ((array && empty) || (!empty && (shape->id != entry->id ||
            !xr_lir_internal_array_intrinsic_valid(m, entry, shape))))
            return lir_status_error(d, XR_LIR_VERIFY_ERROR, "Array intrinsic shape is invalid");
        if (!empty) {
            XrLirType element = entry->result == XR_LIR_ARRAY ?
                shape->result_element : shape->parameter_elements[0];
            uint32_t family = xr_lir_internal_array_family(element);
            if (family == UINT32_MAX) {
                if (m->types[element - XR_LIR_TYPE_COUNT].inline_array_operations[shape->operation - 1] != i + 1)
                    return lir_status_error(d, XR_LIR_VERIFY_ERROR, "inline Array operation index is invalid");
                continue;
            }
            bool reference = entry->parameter_count && entry->parameters[0] == XR_LIR_ARRAY_REF;
            uint32_t *id = reference ? &array_ref_operations[family][(uint32_t)shape->operation - XR_LIR_ARRAY_OP_SET] :
                &array_operations[family][(uint32_t)shape->operation - 1];
            if (*id) return lir_status_error(d, XR_LIR_VERIFY_ERROR, "Array operation is duplicated");
            *id = entry->id;
        }
    }
    if (memcmp(array_operations, m->array_operations, sizeof(array_operations)))
        return lir_status_error(d, XR_LIR_VERIFY_ERROR, "Array operation index is invalid");
    if (memcmp(array_ref_operations, m->array_ref_operations, sizeof(array_ref_operations)))
        return lir_status_error(d, XR_LIR_VERIFY_ERROR, "Array reference operation index is invalid");
    if (m->inline_array_count) {
        uint32_t count = 0;
        for (uint32_t t = 0; t < m->type_count; ++t) {
            const XrLirTypeData *type = &m->types[t];
            if (!type->inline_array_ordinal) continue;
            if (type->inline_array_ordinal != ++count || !xr_lir_internal_array_element(m, XR_LIR_TYPE_COUNT + t))
                return lir_status_error(d, XR_LIR_VERIFY_ERROR, "inline Array element plan is invalid");
            for (uint32_t op = 0; op < XR_LIR_ARRAY_OP_GET; ++op) {
                uint32_t index = type->inline_array_operations[op];
                if (!index || index > m->intrinsic_count || !m->array_intrinsics)
                    return lir_status_error(d, XR_LIR_VERIFY_ERROR, "inline Array operation is missing");
                const XrLirArrayIntrinsic *shape = &m->array_intrinsics[index - 1];
                if ((uint32_t)shape->operation != op + 1 ||
                    (shape->result_element ? shape->result_element : shape->parameter_elements[0]) != XR_LIR_TYPE_COUNT + t)
                    return lir_status_error(d, XR_LIR_VERIFY_ERROR, "inline Array operation disagrees with its element");
            }
        }
        if (count != m->inline_array_count)
            return lir_status_error(d, XR_LIR_VERIFY_ERROR, "inline Array element count is invalid");
    }
    for (uint32_t i = 0; i < m->literal_count; ++i)
        if (!m->literals[i].bytes || m->literals[i].length == SIZE_MAX)
            return lir_status_error(d, XR_LIR_INVALID_ARGUMENT, "invalid literal record");
    if (!!m->startup_entry!=!!m->startup_module_count || m->startup_entry>m->function_count ||
        (m->startup_entry && m->state_layout_set && m->startup_module_count!=m->state_layout.module_count))
        return xr_lir_internal_error(d,"invalid startup entry",0,0,0,NULL);
    XrLirStatus state_status = lir_state_layout_verify(m, d);
    if (state_status != XR_LIR_OK) {
        if (d && d->status == XR_LIR_OK)
            lir_status_error(d, state_status, "state layout verification failed");
        return state_status;
    }
    for (uint32_t index = 0; index < m->function_count; ++index) {
        const XrLirFunctionData *f = &m->functions[index];
        if (!lir_function_shape(m, f) || !lir_state_signature(m, f) || !lir_array_function_shape(m, f))
            return xr_lir_internal_error(d, "function signature or entry is invalid", index + 1, 0,
                                         0, NULL);
        if (index + 1 == m->state_entry && (f->kind != XR_LIR_FUNCTION_DEFINITION ||
            !f->parameter_count || f->parameter_types[f->parameter_count - 1] != XR_LIR_STATE))
            return xr_lir_internal_error(d, "injected entry lacks its trailing state", index + 1, 0, 0, NULL);
        /* Constructed metadata queries follow this structural type boundary. */
        for (uint32_t value = 0; value < f->value_count; ++value)
            if (!xr_lir_internal_type_valid(m, f->values[value].type, true))
                return xr_lir_internal_error(d, "SSA value type is invalid", index + 1, 0, 0, NULL);
        if (f->kind == XR_LIR_FUNCTION_EXTERNAL) {
            XrLirStatus status = lir_callable_facts_capture(m, f, index + 1, (LirCallableGeometry){0}, facts);
            if (status != XR_LIR_OK) return lir_status_error(d, status, "callable description allocation failed");
            continue;
        }
        uint8_t *seen = f->value_count ? xr_malloc(f->value_count) : NULL;
        if (f->value_count && !seen)
            return lir_status_error(d, XR_LIR_OUT_OF_MEMORY, "LIR verifier allocation failed");
        if (seen)
            memset(seen, 0, f->value_count);
        bool scopes = false;
        XrLirStatus status = lir_definitions(m, index, seen, &scopes, d);
        if (status == XR_LIR_OK)
            status = lir_literal_consumers(f, index + 1, seen, d);
        if (status != XR_LIR_OK) { xr_free(seen); return status; }
        LirGraph graph = {0};
        status = lir_graph_create(f, &graph);
        if (status == XR_LIR_OK && !lir_graph_order(f, &graph))
            status = XR_LIR_VERIFY_ERROR;
        if (status == XR_LIR_VERIFY_ERROR)
            xr_lir_internal_error(d, "entry has an incoming edge or a block is unreachable",
                                  index + 1, 0, 0, NULL);
        if (status == XR_LIR_OK && !lir_invoke_edges(m,index+1,f,&graph,seen))
            status = xr_lir_internal_error(d, "invoke requires dedicated normal and error successors",
                                           index + 1, 0, 0, NULL);
        if (status==XR_LIR_OK) {
            XrLirBlockConstructionFacts blocks={0};
            status=lir_block_multiplicity_create(f,multiplicity && lir_cleanup_rank_needed(f),&blocks);
            graph.multiplicity=blocks.may_repeat;graph.cleanup_rank=blocks.sink_first;
            graph.owns_multiplicity=status==XR_LIR_OK;
        }
        if (status==XR_LIR_OK) status=lir_frame_cell_constructors(f,index+1,&graph,d);
        if (status == XR_LIR_OK)
            status = lir_managed_owners(m, f, index + 1, &graph, seen, d);
        if (status == XR_LIR_OK && scopes)
            status = lir_callable_scopes(m, f, index + 1, &graph, d, facts);
        else if (status == XR_LIR_OK)
            status = lir_callable_facts_capture(m, f, index + 1,
                (LirCallableGeometry){.multiplicity=graph.multiplicity}, facts);
        xr_free(seen);
        if (status == XR_LIR_OK) {
            lir_graph_dominators(&graph);
            status = lir_uses(f, index + 1, &graph, d);
        }
        if (status == XR_LIR_OK && !lir_cleanup_paths(m, f, index + 1, &graph))
            status = xr_lir_internal_error(d, "cleanup frontier, captures or exit order is invalid",
                index + 1, 0, 0, NULL);
        if (status == XR_LIR_OK) status = lir_state_function(m, index, d);
        if (status==XR_LIR_OK && multiplicity) {
            multiplicity[index]=(XrLirBlockConstructionFacts){graph.multiplicity,graph.cleanup_rank,f->block_count};
            graph.owns_multiplicity=false;
        }
        if (graph.owns_multiplicity) xr_free(graph.multiplicity);
        xr_free(graph.memory);
        if (status != XR_LIR_OK) {
            if (d && d->status == XR_LIR_OK) {
                lir_status_error(d, status, "LIR graph allocation failed");
                d->function = index + 1;
            }
            return status;
        }
    }
    XrLirStatus status = lir_startup_inventory(m,d);
    return status == XR_LIR_OK ? lir_cleanup_call_targets(m,d) : status;
}
static XrLirStatus lir_verify(const XrLirModule *m, XrLirDiagnostic *d,XrLirCallableFacts **facts,
    XrLirBlockConstructionFacts *multiplicity) {
    XrLirCallableFacts *temporary=NULL;
    XrLirCallableFacts **receiving=facts ? facts : m && m->slice_facts ? &temporary : NULL;
    XrLirStatus status=lir_verify_structure(m,d,receiving,multiplicity);
    /* Every target's canonical captures are complete before the one loan pass.
     * Independent verification owns its receiving copy until that pass ends. */
    for (uint32_t f=0;status == XR_LIR_OK && f<m->function_count;++f) {
        status=lir_slice_function(m,&m->functions[f],f+1,receiving ? *receiving : NULL,d);
        if (status != XR_LIR_OK && d && d->status == XR_LIR_OK) {
            lir_status_error(d,status,"Slice loan receiving failed");d->function=f+1;
        }
    }
    xr_lir_callable_facts_destroy(temporary);
    return status;
}

XR_FUNC XrLirStatus xr_lir_verify(const XrLirModule *m, XrLirDiagnostic *d) {
    return lir_verify(m, d, NULL, NULL);
}
/* The producer retires its own metadata allocation, including when a builder
 * is compiled with an independent allocation provider for failure injection. */
XR_FUNC void xr_lir_internal_block_multiplicity_free(uint8_t *multiplicity) {
    xr_free(multiplicity);
}

XR_FUNC XrLirStatus xr_lir_internal_finish_verify(XrLirModule *m, XrLirDiagnostic *d) {
    if (!m || m->finished || m->callable_facts)
        return lir_status_error(d, XR_LIR_INVALID_STATE, "callable construction is already published");
    XrLirCallableFacts *pending = NULL;
    XrLirBlockConstructionFacts *multiplicity=m->function_count ? xr_calloc(m->function_count,sizeof(*multiplicity)) : NULL;
    if (m->function_count && !multiplicity)
        return lir_status_error(d,XR_LIR_OUT_OF_MEMORY,"cannot own block construction multiplicity");
    XrLirStatus status = lir_verify(m, d, &pending, multiplicity);
    if (status == XR_LIR_OK) {
        m->callable_facts = pending;
        for (uint32_t f=0;f<m->function_count;++f) {
            XR_CHECK(!m->functions[f].block_multiplicity,"finished block facts publish once into their actual function");
            m->functions[f].block_multiplicity=multiplicity[f].may_repeat;
            m->functions[f].block_cleanup_rank=multiplicity[f].sink_first;
            m->functions[f].block_multiplicity_count=m->functions[f].block_count;
            multiplicity[f]=(XrLirBlockConstructionFacts){0};
        }
    }
    else xr_lir_callable_facts_destroy(pending);
    for (uint32_t f=0;f<m->function_count;++f) xr_free(multiplicity[f].may_repeat);
    xr_free(multiplicity);
    return status;
}
