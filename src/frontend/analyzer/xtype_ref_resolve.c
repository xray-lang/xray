/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_ref_resolve.c - Resolve XrTypeRef (syntax) to XrType* (runtime)
 *
 * Walks the XrTypeRef tree and maps each node to its runtime XrType*
 * counterpart.  Named types are resolved via class/prelude lookup;
 * primitives map 1-to-1 to their runtime singletons.
 */

#include "xtype_ref_resolve.h"
#include "xconsteval.h"
#include "xanalyzer.h"
#include "xanalyzer_builtin_interfaces.h"
#include "xanalyzer_mono.h"
#include "xanalyzer_visitor.h"
#include "xanalyzer_visitor_internal.h"
#include "xanalyzer_symbol.h"
#include "../parser/xast_nodes.h"
#include "../parser/xtype_ref.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/value/xtype_names.h"
#include "../../runtime/value/xstruct_layout.h"
#include "../../runtime/xisolate_api.h"
#include "../../shared/xr_numeric_conversion_core.h"
#include "../../base/xchecks.h"
#include "../../base/xarena.h"
#include "../../base/xhash.h"
#include "../../../stdlib/prelude/prelude.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Resolve child type refs recursively. */
static XrType *resolve_impl(XrVMRuntime *X, const XrTypeRef *t);

static void canonicalize_object_fields(const char **names, XrType **types, bool *readonly,
                                       int count) {
    for (int i = 1; i < count; i++) {
        const char *current_name = names[i];
        XrType *current_type = types[i];
        bool current_readonly = readonly[i];
        uint64_t current_key = xr_hash_bytes64(current_name, strlen(current_name));
        uint32_t current_symbol = xr_hash_bytes(current_name, strlen(current_name));
        int j = i;
        while (j > 0) {
            const char *prior_name = names[j - 1];
            uint64_t prior_key = xr_hash_bytes64(prior_name, strlen(prior_name));
            uint32_t prior_symbol = xr_hash_bytes(prior_name, strlen(prior_name));
            int name_cmp = strcmp(prior_name, current_name);
            if (prior_key < current_key ||
                (prior_key == current_key && prior_symbol < current_symbol) ||
                (prior_key == current_key && prior_symbol == current_symbol && name_cmp <= 0))
                break;
            names[j] = names[j - 1];
            types[j] = types[j - 1];
            readonly[j] = readonly[j - 1];
            j--;
        }
        names[j] = current_name;
        types[j] = current_type;
        readonly[j] = current_readonly;
    }
}

#define XA_CONSTEVAL_MAX_DEPTH 64

static bool ct_stack_contains(const uint32_t *stack, int depth, uint32_t id) {
    if (!stack || id == 0)
        return false;
    for (int i = 0; i < depth; i++) {
        if (stack[i] == id)
            return true;
    }
    return false;
}

static bool ct_fail(const char **err, const char *message) {
    if (err)
        *err = message;
    return false;
}

static bool ct_expect_kind(const XrCtValue *v, XrCtValueKind kind, const char **err,
                           const char *message) {
    if (v && v->kind == kind)
        return true;
    return ct_fail(err, message);
}

static bool ct_is_numeric(const XrCtValue *v) {
    return v && (v->kind == XR_CT_INT || v->kind == XR_CT_FLOAT);
}

static double ct_as_double(const XrCtValue *v) {
    return v->kind == XR_CT_FLOAT ? v->as.float_val : (double) v->as.int_val;
}

static bool ct_eval_impl(XaAnalyzer *analyzer, const AstNode *expr, XrCtValue *out,
                         const char **err, uint32_t *stack, int depth);

static bool ct_eval_numeric_as(XaAnalyzer *analyzer, const AstNode *expr, XrCtValue *out,
                               const char **err, uint32_t *stack, int depth) {
    if (!analyzer || !expr || expr->type != AST_AS_EXPR || !out || expr->as.as_expr.is_safe)
        return ct_fail(err, "dynamic casts are not consteval-safe");

    XrConversionWitness conversion = {0};
    if (!xa_analyzer_get_node_conversion(analyzer, expr, &conversion) ||
        !xr_conversion_kind_is_numeric(conversion.kind))
        return ct_fail(err, "cast lacks numeric conversion evidence");

    XrCtValue source = {0};
    if (!ct_eval_impl(analyzer, expr->as.as_expr.expr, &source, err, stack, depth + 1))
        return false;

    bool source_is_float = conversion.source_scalar_rep == XR_NATIVE_F32 ||
                           conversion.source_scalar_rep == XR_NATIVE_F64;
    bool target_is_float = conversion.target_scalar_rep == XR_NATIVE_F32 ||
                           conversion.target_scalar_rep == XR_NATIVE_F64;
    uint8_t pointer_bits = (uint8_t) (sizeof(void *) * 8u);
    if (target_is_float) {
        out->kind = XR_CT_FLOAT;
        if (source_is_float) {
            if (source.kind != XR_CT_FLOAT)
                return ct_fail(err, "numeric conversion source is not a float constant");
            out->as.float_val =
                xr_numeric_float_convert(source.as.float_val, conversion.target_scalar_rep);
        } else {
            if (source.kind != XR_CT_INT)
                return ct_fail(err, "numeric conversion source is not an integer constant");
            out->as.float_val =
                xr_numeric_int_to_float(source.as.int_val, conversion.source_scalar_rep,
                                        conversion.target_scalar_rep, pointer_bits);
        }
        return true;
    }

    out->kind = XR_CT_INT;
    if (source_is_float) {
        if (source.kind != XR_CT_FLOAT)
            return ct_fail(err, "numeric conversion source is not a float constant");
        if (!xr_numeric_float_to_int(source.as.float_val, conversion.target_scalar_rep,
                                     pointer_bits, &out->as.int_val))
            return ct_fail(err, "numeric conversion is out of range");
    } else {
        if (source.kind != XR_CT_INT)
            return ct_fail(err, "numeric conversion source is not an integer constant");
        out->as.int_val =
            xr_numeric_int_convert_i64(source.as.int_val, conversion.source_scalar_rep,
                                       conversion.target_scalar_rep, pointer_bits);
    }
    return true;
}

static bool ct_object_is_module(XaAnalyzer *analyzer, const AstNode *object,
                                const char *module_name) {
    if (!analyzer || !object || object->type != AST_VARIABLE || !module_name ||
        !object->as.variable.name)
        return false;
    XaSymbol *sym = xa_scope_lookup(analyzer->current_scope, object->as.variable.name);
    if (!sym || sym->kind != XA_SYM_MODULE)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    return links && links->module_name && strcmp(links->module_name, module_name) == 0;
}

static bool ct_eval_mem_layout_call(XaAnalyzer *analyzer, const AstNode *expr, XrCtValue *out,
                                    const char **err) {
    if (!analyzer || !expr || expr->type != AST_CALL_EXPR || !out)
        return false;
    const CallExprNode *call = &expr->as.call_expr;
    if (!call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    const MemberAccessNode *member = &call->callee->as.member_access;
    if (!member->name || !ct_object_is_module(analyzer, member->object, "mem"))
        return false;
    bool is_size = strcmp(member->name, "sizeOf") == 0;
    bool is_align = strcmp(member->name, "alignOf") == 0;
    bool is_offset = strcmp(member->name, "offsetOf") == 0;
    if (!is_size && !is_align && !is_offset)
        return false;
    if (call->type_arg_count != 1 || !call->type_args || !call->type_args[0])
        return ct_fail(err, "mem layout consteval requires exactly one type argument");
    if (call->arg_count != (is_offset ? 1 : 0))
        return ct_fail(err, "mem layout consteval received the wrong number of arguments");

    XrType *target = xr_tref_resolve_in_analyzer(analyzer, call->type_args[0]);
    uint32_t size = 0;
    uint32_t align = 0;
    if (!xr_type_has_static_layout(xa_analyzer_target_data_layout(analyzer), target, &size, &align))
        return ct_fail(err, "mem layout consteval requires a statically laid out type");

    uint32_t value = is_size ? size : align;
    if (is_offset) {
        const AstNode *field_arg = call->arguments ? call->arguments[0] : NULL;
        if (!field_arg || field_arg->type != AST_LITERAL_STRING ||
            !field_arg->as.literal.raw_value.string_val)
            return ct_fail(err, "mem.offsetOf consteval requires a string literal field name");
        if (!xr_type_has_static_field_offset(xa_analyzer_target_data_layout(analyzer), target,
                                             field_arg->as.literal.raw_value.string_val, &value))
            return ct_fail(err, "mem.offsetOf consteval could not resolve the field");
    }

    out->kind = XR_CT_INT;
    out->as.int_val = (int64_t) value;
    return true;
}

static XrArena *ct_value_arena(XaAnalyzer *analyzer) {
    return analyzer ? analyzer->consteval_arena : NULL;
}

static XrCtValue *ct_alloc_values(XaAnalyzer *analyzer, int count, const char **err) {
    if (count <= 0)
        return NULL;
    XrArena *arena = ct_value_arena(analyzer);
    if (!arena) {
        if (err)
            *err = "consteval storage is unavailable";
        return NULL;
    }
    XrCtValue *values =
        (XrCtValue *) xr_arena_alloc_array(arena, sizeof(XrCtValue), (size_t) count);
    if (!values) {
        if (err)
            *err = "out of memory while evaluating compile-time array";
        return NULL;
    }
    return values;
}

static uint8_t *ct_alloc_bytes(XaAnalyzer *analyzer, size_t count, const char **err) {
    XrArena *arena = ct_value_arena(analyzer);
    if (!arena) {
        if (err)
            *err = "consteval storage is unavailable";
        return NULL;
    }
    uint8_t *bytes =
        (uint8_t *) xr_arena_alloc_array(arena, sizeof(uint8_t), count > 0 ? count : 1);
    if (!bytes && err)
        *err = "out of memory while evaluating compile-time bytes";
    return bytes;
}

static const char **ct_alloc_field_names(XaAnalyzer *analyzer, int count, const char **err) {
    if (count <= 0)
        return NULL;
    XrArena *arena = ct_value_arena(analyzer);
    if (!arena) {
        if (err)
            *err = "consteval storage is unavailable";
        return NULL;
    }
    const char **names =
        (const char **) xr_arena_alloc_array(arena, sizeof(const char *), (size_t) count);
    if (!names) {
        if (err)
            *err = "out of memory while evaluating compile-time struct";
        return NULL;
    }
    return names;
}

static bool ct_element_lists_equal(const XrCtElementListValue *a, const XrCtElementListValue *b);
static bool ct_fixed_arrays_equal(const XrCtFixedArrayValue *a, const XrCtFixedArrayValue *b);
static bool ct_struct_values_equal(const XrCtStructValue *a, const XrCtStructValue *b);

static bool ct_values_equal(const XrCtValue *a, const XrCtValue *b, bool *out) {
    if (!a || !b || !out)
        return false;
    if (ct_is_numeric(a) && ct_is_numeric(b)) {
        if (a->kind == XR_CT_INT && b->kind == XR_CT_INT)
            *out = a->as.int_val == b->as.int_val;
        else
            *out = ct_as_double(a) == ct_as_double(b);
        return true;
    }
    if (a->kind != b->kind)
        return false;
    switch (a->kind) {
        case XR_CT_INT:
            *out = a->as.int_val == b->as.int_val;
            return true;
        case XR_CT_FLOAT:
            *out = a->as.float_val == b->as.float_val;
            return true;
        case XR_CT_BOOL:
            *out = a->as.bool_val == b->as.bool_val;
            return true;
        case XR_CT_STRING:
            *out = strcmp(a->as.string_val ? a->as.string_val : "",
                          b->as.string_val ? b->as.string_val : "") == 0;
            return true;
        case XR_CT_CHAR:
            *out = a->as.rune_val == b->as.rune_val;
            return true;
        case XR_CT_NULL:
            *out = true;
            return true;
        case XR_CT_FIXED_ARRAY:
            *out = ct_fixed_arrays_equal(&a->as.fixed_array_val, &b->as.fixed_array_val);
            return true;
        case XR_CT_TUPLE:
            *out = ct_element_lists_equal(&a->as.tuple_val, &b->as.tuple_val);
            return true;
        case XR_CT_STRUCT_VALUE:
            *out = ct_struct_values_equal(&a->as.struct_val, &b->as.struct_val);
            return true;
        default:
            return false;
    }
}

static bool ct_element_lists_equal(const XrCtElementListValue *a, const XrCtElementListValue *b) {
    if (!a || !b || a->count != b->count)
        return false;
    for (int i = 0; i < a->count; i++) {
        bool elem_eq = false;
        if (!ct_values_equal(&a->elements[i], &b->elements[i], &elem_eq) || !elem_eq)
            return false;
    }
    return true;
}

static bool ct_fixed_arrays_equal(const XrCtFixedArrayValue *a, const XrCtFixedArrayValue *b) {
    if (!a || !b || a->count != b->count)
        return false;
    if (a->is_byte_blob && b->is_byte_blob)
        return a->count == 0 || memcmp(a->byte_blob, b->byte_blob, (size_t) a->count) == 0;
    for (int i = 0; i < a->count; i++) {
        XrCtValue av = {0};
        XrCtValue bv = {0};
        if (a->is_byte_blob) {
            av.kind = XR_CT_INT;
            av.as.int_val = a->byte_blob[i];
        } else {
            av = a->elements[i];
        }
        if (b->is_byte_blob) {
            bv.kind = XR_CT_INT;
            bv.as.int_val = b->byte_blob[i];
        } else {
            bv = b->elements[i];
        }
        bool equal = false;
        if (!ct_values_equal(&av, &bv, &equal) || !equal)
            return false;
    }
    return true;
}

static bool ct_struct_values_equal(const XrCtStructValue *a, const XrCtStructValue *b) {
    if (!a || !b || a->field_count != b->field_count)
        return false;
    const char *an = a->struct_name ? a->struct_name : "";
    const char *bn = b->struct_name ? b->struct_name : "";
    if (strcmp(an, bn) != 0)
        return false;
    for (int i = 0; i < a->field_count; i++) {
        const char *af = a->field_names && a->field_names[i] ? a->field_names[i] : "";
        const char *bf = b->field_names && b->field_names[i] ? b->field_names[i] : "";
        if (strcmp(af, bf) != 0)
            return false;
        bool field_eq = false;
        if (!ct_values_equal(&a->field_values[i], &b->field_values[i], &field_eq) || !field_eq)
            return false;
    }
    return true;
}

static bool ct_eval_array_literal(XaAnalyzer *analyzer, const AstNode *expr, XrCtValue *out,
                                  const char **err, uint32_t *stack, int depth) {
    const ArrayLiteralNode *arr = &expr->as.array_literal;
    if (arr->is_repeat) {
        XrCtValue count_value = {0};
        if (!ct_eval_impl(analyzer, arr->repeat_count, &count_value, err, stack, depth + 1))
            return false;
        if (count_value.kind != XR_CT_INT)
            return ct_fail(err, "fixed array repeat count must be an integer constant");
        if (count_value.as.int_val <= 0)
            return ct_fail(err, "fixed array repeat count must be greater than zero");
        if (count_value.as.int_val > 65535)
            return ct_fail(err, "fixed array repeat count exceeds maximum of 65535 elements");

        XrCtValue elem = {0};
        if (!ct_eval_impl(analyzer, arr->repeat_value, &elem, err, stack, depth + 1))
            return false;

        int count = (int) count_value.as.int_val;
        XrCtValue *values = ct_alloc_values(analyzer, count, err);
        if (!values)
            return false;
        for (int i = 0; i < count; i++)
            values[i] = elem;

        out->kind = XR_CT_FIXED_ARRAY;
        out->as.fixed_array_val.elements = values;
        out->as.fixed_array_val.count = count;
        out->as.fixed_array_val.byte_blob = NULL;
        out->as.fixed_array_val.is_byte_blob = false;
        return true;
    }

    if (arr->count <= 0)
        return ct_fail(err, "fixed array consteval literal must have at least one element");

    XrCtValue *values = ct_alloc_values(analyzer, arr->count, err);
    if (!values)
        return false;

    for (int i = 0; i < arr->count; i++) {
        AstNode *elem = arr->elements ? arr->elements[i] : NULL;
        if (!elem)
            return ct_fail(err, "missing fixed array consteval element");
        if (elem->type == AST_SPREAD_EXPR)
            return ct_fail(err, "fixed array consteval literal cannot use spread");
        if (!ct_eval_impl(analyzer, elem, &values[i], err, stack, depth + 1))
            return false;
    }

    out->kind = XR_CT_FIXED_ARRAY;
    out->as.fixed_array_val.elements = values;
    out->as.fixed_array_val.count = arr->count;
    out->as.fixed_array_val.byte_blob = NULL;
    out->as.fixed_array_val.is_byte_blob = false;
    return true;
}

static bool ct_eval_fixed_bytes_literal(XaAnalyzer *analyzer, const AstNode *expr, XrCtValue *out,
                                        const char **err) {
    const FixedBytesLiteralNode *literal = &expr->as.fixed_bytes_literal;
    size_t total = literal->payload_length + (literal->append_nul ? 1u : 0u);
    if (total > INT_MAX)
        return ct_fail(err, "fixed byte literal exceeds consteval length limit");
    uint8_t *bytes = ct_alloc_bytes(analyzer, total, err);
    if (!bytes)
        return false;
    if (literal->payload_length > 0)
        memcpy(bytes, literal->payload, literal->payload_length);
    if (literal->append_nul)
        bytes[literal->payload_length] = 0;
    else if (total == 0)
        bytes[0] = 0;
    out->kind = XR_CT_FIXED_ARRAY;
    out->as.fixed_array_val.elements = NULL;
    out->as.fixed_array_val.count = (int) total;
    out->as.fixed_array_val.byte_blob = bytes;
    out->as.fixed_array_val.is_byte_blob = true;
    return true;
}

static bool ct_eval_tuple_literal(XaAnalyzer *analyzer, const AstNode *expr, XrCtValue *out,
                                  const char **err, uint32_t *stack, int depth) {
    const TupleLiteralNode *tup = &expr->as.tuple_literal;
    if (tup->count <= 0)
        return ct_fail(err, "unit tuple consteval is not supported in this phase");

    XrCtValue *raw_values = ct_alloc_values(analyzer, tup->count, err);
    if (!raw_values)
        return false;

    int total_count = 0;
    for (int i = 0; i < tup->count; i++) {
        AstNode *elem = tup->elements ? tup->elements[i] : NULL;
        if (!elem)
            return ct_fail(err, "missing tuple consteval element");
        if (elem->type == AST_SPREAD_EXPR) {
            if (!ct_eval_impl(analyzer, elem->as.spread_expr.expr, &raw_values[i], err, stack,
                              depth + 1))
                return false;
            if (raw_values[i].kind != XR_CT_TUPLE)
                return ct_fail(err, "tuple consteval spread source must be a tuple constant");
            total_count += raw_values[i].as.tuple_val.count;
        } else {
            if (!ct_eval_impl(analyzer, elem, &raw_values[i], err, stack, depth + 1))
                return false;
            total_count++;
        }
    }

    if (total_count <= 0)
        return ct_fail(err, "tuple consteval literal must have at least one element");

    XrCtValue *values = ct_alloc_values(analyzer, total_count, err);
    if (!values)
        return false;

    int slot = 0;
    for (int i = 0; i < tup->count; i++) {
        AstNode *elem = tup->elements ? tup->elements[i] : NULL;
        if (elem && elem->type == AST_SPREAD_EXPR) {
            XrCtTupleValue *spread = &raw_values[i].as.tuple_val;
            for (int j = 0; j < spread->count; j++)
                values[slot++] = spread->elements[j];
        } else {
            values[slot++] = raw_values[i];
        }
    }

    out->kind = XR_CT_TUPLE;
    out->as.tuple_val.elements = values;
    out->as.tuple_val.count = total_count;
    return true;
}

static bool ct_eval_struct_literal(XaAnalyzer *analyzer, const AstNode *expr, XrCtValue *out,
                                   const char **err, uint32_t *stack, int depth) {
    const StructLiteralNode *sl = &expr->as.struct_literal;
    if (!sl->struct_name)
        return ct_fail(err, "struct consteval literal is missing a type name");
    if (sl->field_count <= 0)
        return ct_fail(err, "struct consteval literal must have at least one field");

    const char **field_names = ct_alloc_field_names(analyzer, sl->field_count, err);
    if (!field_names)
        return false;
    XrCtValue *field_values = ct_alloc_values(analyzer, sl->field_count, err);
    if (!field_values)
        return false;

    for (int i = 0; i < sl->field_count; i++) {
        const char *name = sl->field_names ? sl->field_names[i] : NULL;
        AstNode *value = sl->field_values ? sl->field_values[i] : NULL;
        if (!name || !value)
            return ct_fail(err, "missing struct consteval field");
        field_names[i] = name;
        if (!ct_eval_impl(analyzer, value, &field_values[i], err, stack, depth + 1))
            return false;
    }

    out->kind = XR_CT_STRUCT_VALUE;
    out->as.struct_val.struct_name = sl->struct_name;
    out->as.struct_val.field_names = field_names;
    out->as.struct_val.field_values = field_values;
    out->as.struct_val.field_count = sl->field_count;
    return true;
}

static bool ct_eval_member_access(XaAnalyzer *analyzer, const AstNode *expr, XrCtValue *out,
                                  const char **err, uint32_t *stack, int depth) {
    const MemberAccessNode *ma = &expr->as.member_access;
    if (ma->object && ma->object->type == AST_VARIABLE && ma->object->as.variable.name &&
        strcmp(ma->object->as.variable.name, "Type") == 0) {
        int tid = xr_type_from_name(ma->name);
        if (tid < 0)
            return ct_fail(err, "unknown TypeId constant");
        out->kind = XR_CT_INT;
        out->as.int_val = tid;
        return true;
    }

    XrCtValue object = {0};
    if (!ct_eval_impl(analyzer, ma->object, &object, err, stack, depth + 1))
        return false;
    const char *name = ma->name ? ma->name : "";

    if (object.kind == XR_CT_TUPLE) {
        if (!*name)
            return ct_fail(err, "tuple consteval field requires a numeric member name");
        for (const char *p = name; *p; p++) {
            if (*p < '0' || *p > '9')
                return ct_fail(err, "tuple consteval field must use a numeric .N member");
        }
        char *end = NULL;
        long index = strtol(name, &end, 10);
        if (!end || *end != '\0' || index < 0 || index >= object.as.tuple_val.count)
            return ct_fail(err, "tuple consteval field index is out of range");
        *out = object.as.tuple_val.elements[index];
        return true;
    }

    if (object.kind == XR_CT_STRUCT_VALUE) {
        const XrCtStructValue *st = &object.as.struct_val;
        for (int i = 0; i < st->field_count; i++) {
            const char *field = st->field_names ? st->field_names[i] : NULL;
            if (field && strcmp(field, name) == 0) {
                *out = st->field_values[i];
                return true;
            }
        }
        return ct_fail(err, "struct consteval field was not found");
    }

    return ct_fail(err, "consteval member access requires a tuple or struct constant");
}

static bool ct_eval_index_get(XaAnalyzer *analyzer, const AstNode *expr, XrCtValue *out,
                              const char **err, uint32_t *stack, int depth) {
    const IndexGetNode *ig = &expr->as.index_get;
    XrCtValue array = {0};
    if (!ct_eval_impl(analyzer, ig->array, &array, err, stack, depth + 1))
        return false;
    if (array.kind != XR_CT_FIXED_ARRAY)
        return ct_fail(err, "consteval index access requires a fixed array constant");

    XrCtValue index = {0};
    if (!ct_eval_impl(analyzer, ig->index, &index, err, stack, depth + 1))
        return false;
    if (index.kind != XR_CT_INT)
        return ct_fail(err, "fixed array consteval index must be an integer constant");
    if (index.as.int_val < 0 || index.as.int_val >= array.as.fixed_array_val.count)
        return ct_fail(err, "fixed array consteval index is out of range");

    int slot = (int) index.as.int_val;
    if (array.as.fixed_array_val.is_byte_blob) {
        out->kind = XR_CT_INT;
        out->as.int_val = array.as.fixed_array_val.byte_blob[slot];
    } else {
        *out = array.as.fixed_array_val.elements[slot];
    }
    return true;
}

static XaSymbol *ct_lookup_const_symbol(XaAnalyzer *analyzer, const AstNode *expr) {
    if (!analyzer || !expr || expr->type != AST_VARIABLE || !expr->as.variable.name)
        return NULL;
    XaSymbol *sym = NULL;
    if (expr->as.variable.symbol_id != 0)
        sym = xa_scope_lookup_by_id(analyzer->global_scope, expr->as.variable.symbol_id);
    if (!sym && analyzer->current_scope)
        sym = xa_scope_lookup(analyzer->current_scope, expr->as.variable.name);
    if (!sym && analyzer->global_scope)
        sym = xa_scope_lookup(analyzer->global_scope, expr->as.variable.name);
    return sym;
}

static bool ct_eval_unary(XaAnalyzer *analyzer, const AstNode *expr, XrCtValue *out,
                          const char **err, uint32_t *stack, int depth) {
    XrCtValue v = {0};
    if (!ct_eval_impl(analyzer, expr->as.unary.operand, &v, err, stack, depth + 1))
        return false;

    switch (expr->type) {
        case AST_UNARY_NEG:
            if (!ct_is_numeric(&v))
                return ct_fail(err, "unary '-' requires a numeric constant");
            if (v.kind == XR_CT_FLOAT) {
                out->kind = XR_CT_FLOAT;
                out->as.float_val = -v.as.float_val;
                return true;
            }
            if (!ct_expect_kind(&v, XR_CT_INT, err, "unary '-' requires a numeric constant"))
                return false;
            if (v.as.int_val == INT64_MIN)
                return ct_fail(err, "integer constant overflow");
            out->kind = XR_CT_INT;
            out->as.int_val = -v.as.int_val;
            return true;
        case AST_UNARY_BNOT:
            if (!ct_expect_kind(&v, XR_CT_INT, err, "unary '~' requires an integer constant"))
                return false;
            out->kind = XR_CT_INT;
            out->as.int_val = ~v.as.int_val;
            return true;
        case AST_UNARY_NOT:
            if (!ct_expect_kind(&v, XR_CT_BOOL, err, "unary '!' requires a bool constant"))
                return false;
            out->kind = XR_CT_BOOL;
            out->as.bool_val = !v.as.bool_val;
            return true;
        default:
            break;
    }
    return ct_fail(err, "unsupported unary consteval expression");
}

static bool ct_eval_binary(XaAnalyzer *analyzer, const AstNode *expr, XrCtValue *out,
                           const char **err, uint32_t *stack, int depth) {
    XrCtValue left = {0};
    XrCtValue right = {0};

    if (!ct_eval_impl(analyzer, expr->as.binary.left, &left, err, stack, depth + 1))
        return false;

    if (expr->type == AST_BINARY_AND || expr->type == AST_BINARY_OR) {
        if (!ct_expect_kind(&left, XR_CT_BOOL, err, "logical operator requires bool constants"))
            return false;
        if (expr->type == AST_BINARY_AND && !left.as.bool_val) {
            out->kind = XR_CT_BOOL;
            out->as.bool_val = false;
            return true;
        }
        if (expr->type == AST_BINARY_OR && left.as.bool_val) {
            out->kind = XR_CT_BOOL;
            out->as.bool_val = true;
            return true;
        }
    }

    if (!ct_eval_impl(analyzer, expr->as.binary.right, &right, err, stack, depth + 1))
        return false;

    switch (expr->type) {
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV: {
            if (!ct_is_numeric(&left) || !ct_is_numeric(&right))
                return ct_fail(err, "numeric operator requires numeric constants");
            if (left.kind == XR_CT_INT && right.kind == XR_CT_INT) {
                int64_t l = left.as.int_val;
                int64_t r = right.as.int_val;
                out->kind = XR_CT_INT;
                switch (expr->type) {
                    case AST_BINARY_ADD:
                        out->as.int_val = l + r;
                        return true;
                    case AST_BINARY_SUB:
                        out->as.int_val = l - r;
                        return true;
                    case AST_BINARY_MUL:
                        out->as.int_val = l * r;
                        return true;
                    case AST_BINARY_DIV:
                        if (r == 0)
                            return ct_fail(err, "division by zero in constant expression");
                        out->as.int_val = l / r;
                        return true;
                    default:
                        break;
                }
            }
            double l = ct_as_double(&left);
            double r = ct_as_double(&right);
            out->kind = XR_CT_FLOAT;
            switch (expr->type) {
                case AST_BINARY_ADD:
                    out->as.float_val = l + r;
                    return true;
                case AST_BINARY_SUB:
                    out->as.float_val = l - r;
                    return true;
                case AST_BINARY_MUL:
                    out->as.float_val = l * r;
                    return true;
                case AST_BINARY_DIV:
                    out->as.float_val = l / r;
                    return true;
                default:
                    break;
            }
            break;
        }
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT: {
            if (left.kind != XR_CT_INT || right.kind != XR_CT_INT)
                return ct_fail(err, "integer operator requires integer constants");
            int64_t l = left.as.int_val;
            int64_t r = right.as.int_val;
            out->kind = XR_CT_INT;
            switch (expr->type) {
                case AST_BINARY_MOD:
                    if (r == 0)
                        return ct_fail(err, "modulo by zero in constant expression");
                    out->as.int_val = l % r;
                    return true;
                case AST_BINARY_BAND:
                    out->as.int_val = l & r;
                    return true;
                case AST_BINARY_BOR:
                    out->as.int_val = l | r;
                    return true;
                case AST_BINARY_BXOR:
                    out->as.int_val = l ^ r;
                    return true;
                case AST_BINARY_LSHIFT:
                    if (l < 0 || r < 0 || r >= 63)
                        return ct_fail(err, "bit shift is out of range");
                    out->as.int_val = (int64_t) ((uint64_t) l << r);
                    return true;
                case AST_BINARY_RSHIFT:
                    if (r < 0 || r >= 63)
                        return ct_fail(err, "bit shift is out of range");
                    out->as.int_val = l >> r;
                    return true;
                default:
                    break;
            }
            break;
        }
        case AST_BINARY_EQ:
        case AST_BINARY_NE: {
            bool eq = false;
            if (!ct_values_equal(&left, &right, &eq))
                return ct_fail(err, "equality requires constants of the same consteval kind");
            out->kind = XR_CT_BOOL;
            out->as.bool_val = expr->type == AST_BINARY_EQ ? eq : !eq;
            return true;
        }
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE: {
            out->kind = XR_CT_BOOL;
            if (ct_is_numeric(&left) && ct_is_numeric(&right)) {
                if (left.kind == XR_CT_INT && right.kind == XR_CT_INT) {
                    int64_t l = left.as.int_val;
                    int64_t r = right.as.int_val;
                    switch (expr->type) {
                        case AST_BINARY_LT:
                            out->as.bool_val = l < r;
                            return true;
                        case AST_BINARY_LE:
                            out->as.bool_val = l <= r;
                            return true;
                        case AST_BINARY_GT:
                            out->as.bool_val = l > r;
                            return true;
                        case AST_BINARY_GE:
                            out->as.bool_val = l >= r;
                            return true;
                        default:
                            break;
                    }
                }
                double l = ct_as_double(&left);
                double r = ct_as_double(&right);
                switch (expr->type) {
                    case AST_BINARY_LT:
                        out->as.bool_val = l < r;
                        return true;
                    case AST_BINARY_LE:
                        out->as.bool_val = l <= r;
                        return true;
                    case AST_BINARY_GT:
                        out->as.bool_val = l > r;
                        return true;
                    case AST_BINARY_GE:
                        out->as.bool_val = l >= r;
                        return true;
                    default:
                        break;
                }
            }
            if (left.kind == XR_CT_STRING && right.kind == XR_CT_STRING) {
                int cmp = strcmp(left.as.string_val ? left.as.string_val : "",
                                 right.as.string_val ? right.as.string_val : "");
                switch (expr->type) {
                    case AST_BINARY_LT:
                        out->as.bool_val = cmp < 0;
                        return true;
                    case AST_BINARY_LE:
                        out->as.bool_val = cmp <= 0;
                        return true;
                    case AST_BINARY_GT:
                        out->as.bool_val = cmp > 0;
                        return true;
                    case AST_BINARY_GE:
                        out->as.bool_val = cmp >= 0;
                        return true;
                    default:
                        break;
                }
            }
            return ct_fail(err, "comparison requires numeric or string constants");
        }
        case AST_BINARY_AND:
        case AST_BINARY_OR:
            if (right.kind != XR_CT_BOOL)
                return ct_fail(err, "logical operator requires bool constants");
            out->kind = XR_CT_BOOL;
            out->as.bool_val = expr->type == AST_BINARY_AND
                                   ? (left.as.bool_val && right.as.bool_val)
                                   : (left.as.bool_val || right.as.bool_val);
            return true;
        default:
            break;
    }

    return ct_fail(err, "unsupported binary consteval expression");
}

static bool ct_eval_impl(XaAnalyzer *analyzer, const AstNode *expr, XrCtValue *out,
                         const char **err, uint32_t *stack, int depth) {
    if (!expr || !out)
        return ct_fail(err, "missing expression");
    if (depth > XA_CONSTEVAL_MAX_DEPTH)
        return ct_fail(err, "constant expression is too deeply nested");

    if (xa_analyzer_get_node_ct_value(analyzer, expr, out))
        return true;

    bool ok = false;

    switch (expr->type) {
        case AST_COMPTIME_EXPR: {
            AstNode *inner = expr->as.comptime_expr.expr;
            if (inner && inner->type == AST_BLOCK) {
                XaInferContext *ctx = xa_infer_context_new(analyzer);
                if (!ctx)
                    return ct_fail(err, "comptime block engine is unavailable");
                ctx->file_path = analyzer ? analyzer->current_file : NULL;
                xa_visit_comptime_block_expr(ctx, (AstNode *) expr);
                xa_infer_context_free(ctx);
                ok = xa_analyzer_get_node_ct_value(analyzer, expr, out);
                if (!ok)
                    return ct_fail(err, "comptime block expression did not produce a value");
                break;
            }
            ok = ct_eval_impl(analyzer, inner, out, err, stack, depth + 1);
            break;
        }
        case AST_GROUPING:
            ok = ct_eval_impl(analyzer, expr->as.grouping, out, err, stack, depth + 1);
            break;
        case AST_AS_EXPR:
            ok = ct_eval_numeric_as(analyzer, expr, out, err, stack, depth);
            break;
        case AST_LITERAL_INT:
            out->kind = XR_CT_INT;
            out->as.int_val = expr->as.literal.raw_value.int_val;
            ok = true;
            break;
        case AST_LITERAL_FLOAT:
            out->kind = XR_CT_FLOAT;
            out->as.float_val = expr->as.literal.raw_value.float_val;
            ok = true;
            break;
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
            out->kind = XR_CT_BOOL;
            out->as.bool_val = expr->type == AST_LITERAL_TRUE;
            ok = true;
            break;
        case AST_LITERAL_STRING:
            out->kind = XR_CT_STRING;
            out->as.string_val = expr->as.literal.raw_value.string_val;
            ok = true;
            break;
        case AST_FIXED_BYTES_LITERAL:
            ok = ct_eval_fixed_bytes_literal(analyzer, expr, out, err);
            break;
        case AST_LITERAL_RUNE:
            out->kind = XR_CT_CHAR;
            out->as.rune_val = expr->as.literal.raw_value.rune_val;
            ok = true;
            break;
        case AST_LITERAL_NULL:
            out->kind = XR_CT_NULL;
            ok = true;
            break;
        case AST_ARRAY_LITERAL:
            ok = ct_eval_array_literal(analyzer, expr, out, err, stack, depth);
            break;
        case AST_TUPLE_LITERAL:
            ok = ct_eval_tuple_literal(analyzer, expr, out, err, stack, depth);
            break;
        case AST_STRUCT_LITERAL:
            ok = ct_eval_struct_literal(analyzer, expr, out, err, stack, depth);
            break;
        case AST_MEMBER_ACCESS:
            ok = ct_eval_member_access(analyzer, expr, out, err, stack, depth);
            break;
        case AST_INDEX_GET:
            ok = ct_eval_index_get(analyzer, expr, out, err, stack, depth);
            break;
        case AST_UNARY_NEG:
        case AST_UNARY_BNOT:
        case AST_UNARY_NOT:
            ok = ct_eval_unary(analyzer, expr, out, err, stack, depth);
            break;
        case AST_VARIABLE: {
            XaSymbol *sym = ct_lookup_const_symbol(analyzer, expr);
            XaSymbolLinks *links = sym ? xa_analyzer_get_links(analyzer, sym) : NULL;
            bool is_ct_binding = links && (sym->is_const || links->is_comptime_local);
            if (!sym || !is_ct_binding || !links)
                return ct_fail(err, "identifier is not a compile-time const value");
            if (links->has_ct_value) {
                *out = links->ct_value;
                ok = true;
                break;
            }
            if (!sym->is_const || !links->const_initializer)
                return ct_fail(err, "identifier is not a compile-time const value");
            if (ct_stack_contains(stack, depth, sym->id))
                return ct_fail(err, "cyclic const expression");
            if (depth < XA_CONSTEVAL_MAX_DEPTH)
                stack[depth] = sym->id;
            ok = ct_eval_impl(analyzer, links->const_initializer, out, err, stack, depth + 1);
            break;
        }
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
            ok = ct_eval_binary(analyzer, expr, out, err, stack, depth);
            break;
        case AST_NULLISH_COALESCE: {
            XrCtValue left = {0};
            if (!ct_eval_impl(analyzer, expr->as.binary.left, &left, err, stack, depth + 1))
                return false;
            if (left.kind != XR_CT_NULL) {
                *out = left;
                ok = true;
                break;
            }
            ok = ct_eval_impl(analyzer, expr->as.binary.right, out, err, stack, depth + 1);
            break;
        }
        case AST_TERNARY: {
            XrCtValue cond = {0};
            if (!ct_eval_impl(analyzer, expr->as.ternary.condition, &cond, err, stack, depth + 1))
                return false;
            if (!ct_expect_kind(&cond, XR_CT_BOOL, err,
                                "ternary condition requires a bool constant"))
                return false;
            AstNode *selected =
                cond.as.bool_val ? expr->as.ternary.true_expr : expr->as.ternary.false_expr;
            ok = ct_eval_impl(analyzer, selected, out, err, stack, depth + 1);
            break;
        }
        case AST_CALL_EXPR:
            if (ct_eval_mem_layout_call(analyzer, expr, out, err)) {
                ok = true;
                break;
            }
            return ct_fail(err, "function calls are not consteval-safe in this phase");
        case AST_BLOCK:
            return ct_fail(err, "comptime block engine is not implemented in this phase");
        default:
            break;
    }

    if (!ok) {
        if (err && *err)
            return false;
        return ct_fail(err, "expression is not consteval-safe");
    }
    xa_analyzer_set_node_ct_value(analyzer, expr, out);
    return true;
}

bool xa_consteval_expr(XaAnalyzer *analyzer, const AstNode *expr, XrCtValue *out_value,
                       const char **out_error) {
    uint32_t stack[XA_CONSTEVAL_MAX_DEPTH + 1];
    memset(stack, 0, sizeof(stack));
    if (out_error)
        *out_error = NULL;
    XrCtValue tmp = {0};
    bool ok = ct_eval_impl(analyzer, expr, out_value ? out_value : &tmp, out_error, stack, 0);
    if (!ok && out_value)
        out_value->kind = XR_CT_NONE;
    return ok;
}

bool xa_consteval_int_expr(XaAnalyzer *analyzer, const AstNode *expr, int64_t *out_value,
                           const char **out_error) {
    XrCtValue value = {0};
    if (!xa_consteval_expr(analyzer, expr, &value, out_error))
        return false;
    if (value.kind != XR_CT_INT)
        return ct_fail(out_error, "expression is not an integer constant");
    if (out_value)
        *out_value = value.as.int_val;
    return true;
}

XR_FUNC bool xa_eval_const_int_expr(XaAnalyzer *analyzer, const AstNode *expr, int64_t *out_value,
                                    const char **out_error) {
    return xa_consteval_int_expr(analyzer, expr, out_value, out_error);
}

static void xa_report_fixed_array_length_diag(XaAnalyzer *analyzer, const AstNode *expr,
                                              const char *message) {
    if (!analyzer || !message)
        return;
    XrLocation loc = {.file = analyzer->current_file,
                      .line = expr ? expr->line : 0,
                      .column = expr ? expr->column : 0};
    xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, message,
                               &loc);
}

/* Map a known named type to its runtime XrType*. Unknown user type names are
 * deliberately excluded here; analyzer-aware callers must prove those through
 * symbols or active generic parameters before accepting them. */
static int builtin_interface_type_arity(const char *name) {
    if (!name)
        return -1;
#define XR_BUILTIN_IFACE(iface_name, iface_arity)                                                  \
    if (strcmp(name, iface_name) == 0)                                                             \
        return (iface_arity);
#include "../../../stdlib/prelude/builtin_symbols.def"
    return -1;
}

/* Arity of every built-in type head, taken from builtin_symbols.def. The table
 * stays isolate-independent — the analyzer runs on isolates that never loaded
 * the prelude module — and the prelude rows carry their arity there too, so no
 * second lookup through the prelude symbol table is needed. */
static int known_type_head_arity(const char *name) {
    if (!name)
        return -1;
    if (strcmp(name, "JSON.Value") == 0 || strcmp(name, "JSON.Object") == 0 ||
        strcmp(name, "JSON.PathSegment") == 0 || strcmp(name, "JSON.Path") == 0 ||
        strcmp(name, "JSON.UnknownFields") == 0 || strcmp(name, "JSON.Encodable") == 0 ||
        strcmp(name, "JSON.Decodable") == 0)
        return 0;
    if (strcmp(name, "JSON.WithRest") == 0)
        return 1;
#define XR_BUILTIN_PRELUDE_TYPE(type_name, type_arity, native_type, prelude_kind)                  \
    if (strcmp(name, type_name) == 0)                                                              \
        return (type_arity);
#define XR_BUILTIN_TYPE(type_name, type_arity)                                                     \
    if (strcmp(name, type_name) == 0)                                                              \
        return (type_arity);
#include "../../../stdlib/prelude/builtin_symbols.def"
    return builtin_interface_type_arity(name);
}

static XrType *resolve_known_named(XrVMRuntime *X, const char *name) {
    XR_DCHECK(name != NULL, "resolve_named: NULL name");

    int arity = known_type_head_arity(name);
    if (arity > 0)
        return xr_type_new_error(NULL);

    if (strcmp(name, "JSON.Value") == 0)
        return xr_type_new_json(X);
    if (strcmp(name, "JSON.Object") == 0)
        return xr_type_new_map(X, xr_type_new_string(NULL), xr_type_new_json(X));
    if (strcmp(name, "JSON.PathSegment") == 0) {
        XrType *members[2] = {xr_type_new_string(NULL), xr_type_new_int(NULL)};
        return xr_type_new_union(X, members, 2);
    }
    if (strcmp(name, "JSON.Path") == 0) {
        XrType *members[2] = {xr_type_new_string(NULL), xr_type_new_int(NULL)};
        return xr_type_new_array(X, xr_type_new_union(X, members, 2));
    }
    if (strcmp(name, "JSON.UnknownFields") == 0)
        return xr_type_new_enum(X, "JSON.UnknownFields");
    if (strcmp(name, "JSON.Encodable") == 0 || strcmp(name, "JSON.Decodable") == 0)
        return xr_type_new_interface(X, name);

    /* Built-in interfaces (Comparable, Hashable, Stringable, Equatable, ...).
     * These must be resolved as XR_KIND_INTERFACE so generic-constraint checks
     * can match by interface name rather than treating them as plain classes.
     * Create a fresh XrType via the current isolate instead of reading a
     * global cache — multiple isolates would race on shared mutable state. */
    if (xa_is_builtin_interface_name(name))
        return xr_type_new_interface(X, name);

    /* Prelude lookup (Array, Map, Set, Channel, Slice, Json, ...) */
    const XrPreludeSymbols *symbols = xr_prelude_get_symbols(X);
    if (symbols) {
        const XrPreludeTypeEntry *entry = xr_prelude_lookup_type(symbols, name, strlen(name));
        if (entry) {
            switch ((XrPreludeKind) entry->kind) {
                case XR_PRELUDE_KIND_SIMPLE:
                    return xr_type_new_named_instance(X, entry->name);
                case XR_PRELUDE_KIND_SINGLETON:
                    return xr_type_new_named_instance(X, entry->name);
                case XR_PRELUDE_KIND_GENERIC_1:
                case XR_PRELUDE_KIND_GENERIC_2:
                    return xr_type_new_error(NULL);
            }
        }
    }

    /* Well-known names not in prelude */
    if (strcmp(name, "Task") == 0)
        return xr_type_new_error(NULL);
    if (strcmp(name, "PanicInfo") == 0)
        return xr_type_new_named_instance(X, "PanicInfo");
    if (strcmp(name, TYPE_NAME_BUFFER) == 0)
        return xr_type_new_named_instance(X, TYPE_NAME_BUFFER);
    return NULL;
}

/* Map a named type to its runtime XrType*.
 * Order: built-in interfaces → prelude → well-known singletons → class fallback. */
static XrType *resolve_named(XrVMRuntime *X, const char *name) {
    XrType *known = resolve_known_named(X, name);
    if (known)
        return known;
    /* Default: treat as class name */
    return xr_type_new_class(X, name);
}

/* Map a generic type (Name<T1, ...>) to its runtime XrType*. */
static XrType *resolve_generic(XrVMRuntime *X, const XrTypeRef *t) {
    XR_DCHECK(t != NULL && t->name != NULL, "resolve_generic: NULL");
    const char *name = t->name;
    int nargs = t->nchildren;

    /* Resolve all type arguments first */
    XrType *stack_args[16];
    XrType **args =
        (nargs <= 16) ? stack_args : (XrType **) xr_malloc((size_t) nargs * sizeof(XrType *));
    if (nargs > 0 && !args)
        return xr_type_new_error(NULL);
    for (int i = 0; i < nargs; i++)
        args[i] = resolve_impl(X, t->children[i]);

    /* Dispatch to known container constructors */
    XrType *result = NULL;
    int expected_arity = known_type_head_arity(name);
    if (expected_arity >= 0 && nargs != expected_arity) {
        result = xr_type_new_error(NULL);
    } else if (nargs == 0 && expected_arity == 0) {
        result = resolve_known_named(X, name);
    } else if (strcmp(name, "Array") == 0 && nargs == 1) {
        result = xr_type_new_array(X, args[0]);
    } else if (strcmp(name, TYPE_NAME_SLICE) == 0 && nargs == 1) {
        result = xr_type_new_slice(X, args[0]);
    } else if (strcmp(name, "Set") == 0 && nargs == 1) {
        result = xr_type_new_set(X, args[0]);
    } else if (strcmp(name, "Channel") == 0 && nargs == 1) {
        result = xr_type_new_channel(X, args[0]);
    } else if (strcmp(name, "Map") == 0 && nargs == 2) {
        result = xr_type_new_map(X, args[0], args[1]);
    } else if (strcmp(name, "JSON.WithRest") == 0 && nargs == 1) {
        const char *field_names[2] = {"rest", "value"};
        XrType *field_types[2] = {xr_type_new_map(X, xr_type_new_string(NULL), xr_type_new_json(X)),
                                  args[0]};
        result = xr_type_new_struct_object_with_fields(X, field_names, field_types, 2);
        if (result)
            result->object.type_name = "JSON.WithRest";
    } else if (strcmp(name, "Task") == 0 && nargs == 1) {
        result = xr_type_new_task(X, args[0]);
    } else if (strcmp(name, "Ptr") == 0 && nargs == 1) {
        result = xr_type_new_pointer(X, args[0], false);  // const raw pointer
    } else if (strcmp(name, "MutPtr") == 0 && nargs == 1) {
        result = xr_type_new_pointer(X, args[0], true);  // mutable raw pointer
    } else if (strcmp(name, "CFn") == 0 && nargs == 1) {
        if (args[0] && args[0]->kind == XR_KIND_FUNCTION) {
            result = xr_type_copy(X, args[0]);
            if (result)
                result->function.is_c_abi = true;
        } else {
            result = xr_type_new_error(NULL);
        }
    } else if (xr_type_is_enum_metadata_type_name(name) && nargs == 1 && args[0] &&
               args[0]->kind == XR_KIND_ENUM && args[0]->enum_type.layout) {
        result = xr_type_new_enum_metadata(X, name, args[0]);
    } else if (xa_is_builtin_interface_name(name)) {
        /* Built-in interface with type args: e.g. Iterable<int>. Create a fresh
         * generic interface type via the current isolate. */
        result = xr_type_new_generic_interface(X, name, args, nargs);
    } else {
        /* Generic class instance — fallback for user types */
        result = xr_type_new_generic_instance(X, name, NULL, args, nargs);
    }

    if (args != stack_args)
        xr_free(args);
    return result;
}

static XrType *resolve_impl(XrVMRuntime *X, const XrTypeRef *t) {
    if (!t)
        return xr_type_new_error(NULL);

    switch ((XrTypeRefKind) t->kind) {
        case XR_TREF_INT:
            return xr_type_new_int(NULL);
        case XR_TREF_FLOAT:
            return xr_type_new_float(NULL);
        case XR_TREF_STRING:
            return xr_type_new_string(NULL);
        case XR_TREF_BOOL:
            return xr_type_new_bool(NULL);
        case XR_TREF_RUNE:
            return xr_type_new_rune(NULL);
        case XR_TREF_UNIT:
            return xr_type_new_unit(NULL);
        case XR_TREF_NULL:
            return xr_type_new_null(NULL);
        case XR_TREF_ERROR:
            return xr_type_new_error(NULL);

        case XR_TREF_INT_WIDTH:
            return xr_type_new_int_width(X, t->scalar_rep);
        case XR_TREF_FLOAT_WIDTH:
            return xr_type_new_float_width(X, t->scalar_rep);

        case XR_TREF_NAMED:
            return resolve_named(X, t->name);
        case XR_TREF_GENERIC:
            return resolve_generic(X, t);

        case XR_TREF_CONST: {
            XrType *inner =
                t->nchildren > 0 ? resolve_impl(X, t->children[0]) : xr_type_new_error(NULL);
            return xr_type_make_const(X, inner);
        }

        case XR_TREF_OPTIONAL: {
            XrType *inner = resolve_impl(X, t->children[0]);
            return xr_type_new_optional(X, inner);
        }

        case XR_TREF_UNION: {
            XrType *members[XR_UNION_MAX_MEMBERS];
            int count = t->nchildren < XR_UNION_MAX_MEMBERS ? t->nchildren : XR_UNION_MAX_MEMBERS;
            for (int i = 0; i < count; i++)
                members[i] = resolve_impl(X, t->children[i]);
            return xr_type_new_union(X, members, count);
        }

        case XR_TREF_FUNCTION: {
            int nparam = t->nchildren > 0 ? t->nchildren - 1 : 0;
            XrType *stack_params[16];
            XrType **params = (nparam <= 16)
                                  ? stack_params
                                  : (XrType **) xr_malloc((size_t) nparam * sizeof(XrType *));
            if (nparam > 0 && !params)
                return xr_type_new_error(NULL);
            for (int i = 0; i < nparam; i++)
                params[i] = resolve_impl(X, t->children[i]);
            XrType *ret = t->nchildren > 0 ? resolve_impl(X, t->children[t->nchildren - 1])
                                           : xr_type_new_unit(NULL);
            XrType *result =
                xr_type_new_function(X, nparam > 0 ? params : NULL, nparam, ret, false);
            if (result && t->requires_nothrow)
                xr_type_function_set_throw_effect(result, XR_FN_EFFECT_NO_THROW);
            if (result && t->function_param_modes) {
                for (int i = 0; i < nparam; i++)
                    xr_type_function_set_param_mode(result, i, t->function_param_modes[i]);
            }
            if (params != stack_params)
                xr_free(params);
            return result;
        }

        case XR_TREF_TUPLE: {
            int count = t->nchildren;
            XrType *stack_elems[16];
            XrType **elems = (count <= 16)
                                 ? stack_elems
                                 : (XrType **) xr_malloc((size_t) count * sizeof(XrType *));
            if (count > 0 && !elems)
                return xr_type_new_error(NULL);
            for (int i = 0; i < count; i++)
                elems[i] = resolve_impl(X, t->children[i]);
            XrType *result = xr_type_new_tuple(X, elems, count);
            if (elems != stack_elems)
                xr_free(elems);
            return result;
        }

        case XR_TREF_OBJECT: {
            int count = t->nchildren;
            const char *stack_names[16];
            XrType *stack_types[16];
            bool stack_readonly[16];
            const char **names =
                count <= 16 ? stack_names
                            : (const char **) xr_malloc((size_t) count * sizeof(const char *));
            XrType **types = count <= 16 ? stack_types
                                         : (XrType **) xr_malloc((size_t) count * sizeof(XrType *));
            bool *readonly =
                count <= 16 ? stack_readonly : (bool *) xr_malloc((size_t) count * sizeof(bool));
            if (count > 0 && (!names || !types || !readonly)) {
                if (names != stack_names)
                    xr_free((void *) names);
                if (types != stack_types)
                    xr_free(types);
                if (readonly != stack_readonly)
                    xr_free(readonly);
                return xr_type_new_error(NULL);
            }
            for (int i = 0; i < count; i++) {
                names[i] = t->field_names[i];
                types[i] = resolve_impl(X, t->children[i]);
                readonly[i] = t->field_readonly ? t->field_readonly[i] : false;
            }
            canonicalize_object_fields(names, types, readonly, count);
            XrType *result = xr_type_new_struct_object_with_fields(X, names, types, count);
            if (result && t->field_readonly)
                xr_type_set_object_field_readonly(X, result, readonly, count);
            if (result && t->name)
                xr_type_set_object_type_name(X, result, t->name);
            if (names != stack_names)
                xr_free((void *) names);
            if (types != stack_types)
                xr_free(types);
            if (readonly != stack_readonly)
                xr_free(readonly);
            return result;
        }

        case XR_TREF_FIXED_ARRAY: {
            XrType *elem =
                t->nchildren > 0 ? resolve_impl(X, t->children[0]) : xr_type_new_error(NULL);
            if (t->fixed_length <= 0 || t->fixed_length > (int) UINT16_MAX)
                return xr_type_new_error(X);
            return xr_type_new_fixed_array(X, elem, (int) t->fixed_length);
        }

        case XR_TREF_TYPE_PARAM:
            return xr_type_new_type_param(X, t->name, 0);
    }

    return xr_type_new_error(NULL);
}

XR_FUNC XrType *xr_tref_resolve(XrVMRuntime *X, const XrTypeRef *tref) {
    return resolve_impl(X, tref);
}

static bool is_sync_runtime_class_name(const char *name) {
    return name && (strcmp(name, "Semaphore") == 0 || strcmp(name, "CountdownLatch") == 0 ||
                    strcmp(name, "EventCount") == 0 || strcmp(name, "WorkQueue") == 0 ||
                    strcmp(name, "ResultGroup") == 0);
}

static const char *sync_runtime_import_class_name(const XaSymbolLinks *links) {
    if (!links || !links->module_name || strcmp(links->module_name, "sync") != 0 ||
        !is_sync_runtime_class_name(links->import_member_name))
        return NULL;
    return links->import_member_name;
}

/* Look up `name` as a declaration-backed type in analyzer scopes; on hit,
 * return the canonical XrType (carrying the inheritance chain for classes,
 * enum identity for enum values, or the interface singleton for interfaces).
 * Returns NULL when no matching symbol is registered. */
static XaSymbol *resolve_type_symbol(XaAnalyzer *analyzer, const char *name) {
    if (!analyzer || !name)
        return NULL;

    XaScope *start = analyzer->current_scope ? analyzer->current_scope : analyzer->global_scope;
    XaSymbol *sym = xa_scope_lookup(start, name);
    if (!sym)
        return NULL;
    if (sym->kind == XA_SYM_CLASS || sym->kind == XA_SYM_ENUM || sym->kind == XA_SYM_TYPE_ALIAS)
        return sym;
    if (sym->kind == XA_SYM_IMPORT) {
        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        XrType *type = links ? links->type : NULL;
        if (sync_runtime_import_class_name(links))
            return sym;
        if (links && links->module_name && links->import_member_name) {
            XrHashMap *exports = resolve_graph_export_symbols(analyzer, links->module_name);
            XaSymbol *export_sym =
                exports ? (XaSymbol *) xr_hashmap_get(exports, links->import_member_name) : NULL;
            if (export_sym) {
                xa_symbol_links_copy_export_metadata(analyzer, links, &export_sym->links);
                type = links->type;
                if (export_sym->kind == XA_SYM_CLASS || export_sym->kind == XA_SYM_ENUM ||
                    export_sym->kind == XA_SYM_TYPE_ALIAS)
                    return sym;
            }
        }
        if (type && (type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE ||
                     type->kind == XR_KIND_INTERFACE || type->kind == XR_KIND_ENUM)) {
            return sym;
        }
    }
    return NULL;
}

static XrType *resolve_type_ref_symbol_type(XaAnalyzer *analyzer, const char *name) {
    XaSymbol *sym = resolve_type_symbol(analyzer, name);
    if (!sym)
        return NULL;

    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    if (!links)
        return NULL;

    const char *sync_class = sync_runtime_import_class_name(links);
    if (sync_class)
        return xr_type_new_named_instance(analyzer->isolate, sync_class);

    if (sym->kind == XA_SYM_ENUM)
        return links->type;
    if (links->type && links->type->kind == XR_KIND_INTERFACE)
        return links->type;
    if (links->class_info)
        return xr_type_new_instance(analyzer->isolate, links->class_info);
    if (!links->type)
        return NULL;
    if (links->type->kind == XR_KIND_INSTANCE)
        return links->type;
    if (links->type->kind == XR_KIND_CLASS) {
        if (links->class_info)
            return xr_type_new_instance(analyzer->isolate, links->class_info);
        if (links->type->instance.class_name)
            return xr_type_new_named_instance(analyzer->isolate, links->type->instance.class_name);
        return xr_type_new_instance(analyzer->isolate, NULL);
    }
    return NULL;
}

static bool is_removed_enum_runtime_wrapper_name(const char *name) {
    return name && (strcmp(name, "EnumValue") == 0 || strcmp(name, "EnumType") == 0);
}

static void report_removed_enum_runtime_wrapper_type(XaAnalyzer *analyzer, const char *name) {
    if (!analyzer || !name)
        return;
    XrLocation loc = {.file = analyzer->current_file, .line = 0, .column = 0};
    char msg[192];
    snprintf(msg, sizeof(msg), "runtime enum wrapper type '%s' has been removed", name);
    xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
}

/* Names users keep reaching for that Xray deliberately does not define. Only
 * consulted once nothing else resolved, so a user declaration of the same name
 * still wins. See builtin_symbols.def for the list and the rationale. */
static const char *undefined_type_hint(const char *name) {
    if (!name)
        return NULL;
#define XR_BUILTIN_UNDEFINED_HINT(hint_name, hint_text)                                            \
    if (strcmp(name, hint_name) == 0)                                                              \
        return (hint_text);
#include "../../../stdlib/prelude/builtin_symbols.def"
    return NULL;
}

/* Every declared union funnels through the resolver, so aliases that expand to a
 * colliding member are caught here alongside the directly spelled form. */
static bool report_indiscriminable_union(XaAnalyzer *analyzer, const XrTypeRef *tref,
                                         XrType *resolved) {
    XrType *first = NULL;
    XrType *second = NULL;
    if (!analyzer || !xr_type_union_indiscriminable_pair(resolved, &first, &second))
        return false;
    XrLocation loc = {.file = analyzer->current_file,
                      .line = tref ? tref->line : 0,
                      .column = tref ? tref->column : 0};
    char msg[256];
    snprintf(msg, sizeof(msg),
             "union members '%s' and '%s' are the same type after erasure, so `is` and `match` "
             "cannot tell them apart; a union may carry at most one integer member and at most "
             "one float member",
             xr_type_to_string(first), xr_type_to_string(second));
    xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_UNION_INDISCRIMINABLE,
                               msg, &loc);
    return true;
}

static void report_undefined_public_type(XaAnalyzer *analyzer, const char *name) {
    if (!analyzer || !name)
        return;
    XrLocation loc = {.file = analyzer->current_file, .line = 0, .column = 0};
    char msg[256];
    const char *hint = undefined_type_hint(name);
    if (hint)
        snprintf(msg, sizeof(msg), "undefined type '%s': %s", name, hint);
    else
        snprintf(msg, sizeof(msg), "undefined type '%s'", name);
    xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
}

XR_FUNC bool xa_reject_error_type_success_type(XaAnalyzer *analyzer, const XrType *type,
                                               const char *role, const char *owner, int line,
                                               int column) {
    if (!analyzer || !xr_type_contains_error(type))
        return false;
    XrLocation loc = {.file = analyzer->current_file, .line = line, .column = column};
    char msg[256];
    if (owner && *owner) {
        snprintf(msg, sizeof(msg), "%s '%s' cannot contain compiler recovery ErrorType",
                 role ? role : "type", owner);
    } else {
        snprintf(msg, sizeof(msg), "%s cannot contain compiler recovery ErrorType",
                 role ? role : "type");
    }
    xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_CONSTRAINT, msg,
                               &loc);
    return true;
}

static int active_type_param_index(XaAnalyzer *analyzer, const char *name) {
    if (!analyzer || !name)
        return -1;

    for (XaScope *scope = analyzer->current_scope; scope; scope = scope->parent) {
        XaSymbol *owners[2] = {scope->function_symbol, scope->class_symbol};
        for (int owner_index = 0; owner_index < 2; owner_index++) {
            XaSymbol *owner = owners[owner_index];
            XaSymbolLinks *links = owner ? xa_analyzer_get_links(analyzer, owner) : NULL;
            int count = links ? xa_symbol_links_get_type_param_count(links) : 0;
            for (int i = 0; i < count; i++) {
                const char *tp_name = xa_symbol_links_get_type_param_name(links, i);
                if (tp_name && strcmp(tp_name, name) == 0)
                    return i;
            }
        }
    }
    return -1;
}

static XrType *report_type_alias_error(XaAnalyzer *analyzer, const char *name,
                                       const char *message) {
    if (analyzer && name && message) {
        XrLocation loc = {.file = analyzer->current_file, .line = 0, .column = 0};
        char msg[192];
        snprintf(msg, sizeof(msg), "type alias '%s': %s", name, message);
        xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_MISSING_TYPE, msg,
                                   &loc);
    }
    return xr_type_new_error(NULL);
}

static XrType *report_generic_arity_error(XaAnalyzer *analyzer, const char *name, int expected,
                                          int got) {
    if (analyzer && name && expected >= 0) {
        XrLocation loc = {.file = analyzer->current_file, .line = 0, .column = 0};
        char msg[192];
        snprintf(msg, sizeof(msg), "generic type '%s' expects %d type argument%s, got %d", name,
                 expected, expected == 1 ? "" : "s", got);
        xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_MISSING_TYPE, msg,
                                   &loc);
    }
    return xr_type_new_error(NULL);
}

static XrType *report_generic_type_argument_error(XaAnalyzer *analyzer, const char *name,
                                                  const char *message) {
    if (analyzer && name && message) {
        XrLocation loc = {.file = analyzer->current_file, .line = 0, .column = 0};
        char msg[192];
        snprintf(msg, sizeof(msg), "generic type '%s': %s", name, message);
        xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_MISSING_TYPE, msg,
                                   &loc);
    }
    return xr_type_new_error(NULL);
}

static XrType *resolve_type_alias_symbol_in_analyzer(XaAnalyzer *analyzer, XaSymbol *sym,
                                                     XrTypeRef **type_args, int type_arg_count) {
    if (!analyzer || !sym || sym->kind != XA_SYM_TYPE_ALIAS)
        return NULL;

    AstNode *node = sym->type_alias_node;
    TypeAliasNode *alias = (node && node->type == AST_TYPE_ALIAS) ? &node->as.type_alias : NULL;
    if (!alias || !alias->resolved_type) {
        if (sym->alias_type)
            return (XrType *) sym->alias_type;
        return report_type_alias_error(analyzer, sym->name, "definition is unavailable");
    }

    int expected = alias->type_param_count;
    if (expected != type_arg_count) {
        char detail[96];
        snprintf(detail, sizeof(detail), "expects %d type argument%s, got %d", expected,
                 expected == 1 ? "" : "s", type_arg_count);
        return report_type_alias_error(analyzer, sym->name, detail);
    }

    if (sym->alias_resolving)
        return report_type_alias_error(analyzer, sym->name, "circular definition");

    XrMonoTypeMap stack_map[8];
    XrMonoTypeMap *map = NULL;
    if (expected > 0) {
        map = expected <= 8 ? stack_map
                            : (XrMonoTypeMap *) xr_calloc((size_t) expected, sizeof(*map));
        if (!map)
            return xr_type_new_error(NULL);
        for (int i = 0; i < expected; i++) {
            map[i].param_name = alias->type_params[i] ? alias->type_params[i]->name : NULL;
            map[i].concrete_type = type_args ? type_args[i] : NULL;
        }
    }

    sym->alias_resolving = true;
    XrTypeRef *expanded =
        map ? xr_mono_type_substitute(alias->resolved_type, map, expected) : alias->resolved_type;
    XrType *resolved = xr_tref_resolve_in_analyzer(analyzer, expanded);
    sym->alias_resolving = false;

    if (map && map != stack_map)
        xr_free(map);

    if (!resolved)
        resolved = xr_type_new_error(NULL);
    if (expected == 0) {
        sym->alias_type = resolved;
        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (links) {
            links->type = resolved;
            links->declared_type = resolved;
        }
    }
    return resolved;
}

static bool is_known_generic_head(const char *name) {
    return name && known_type_head_arity(name) >= 0;
}

static bool generic_head_is_container_like(const char *name) {
    return name && (strcmp(name, "Array") == 0 || strcmp(name, TYPE_NAME_SLICE) == 0 ||
                    strcmp(name, "Set") == 0 || strcmp(name, "Channel") == 0);
}

static bool reject_error_type_args(XaAnalyzer *analyzer, XrType **args, int count,
                                   const char *head) {
    bool rejected = false;
    for (int i = 0; i < count; i++) {
        bool is_map_head = head && (strcmp(head, "Map") == 0);
        const char *role = generic_head_is_container_like(head) ? "container element type"
                           : (is_map_head && i == 0)            ? "container key type"
                           : (is_map_head && i == 1)            ? "container value type"
                                                                : "generic type argument";
        if (xa_reject_error_type_success_type(analyzer, args ? args[i] : NULL, role, head, 0, 0))
            rejected = true;
    }
    return rejected;
}

static XrType *resolve_known_generic_in_analyzer(XaAnalyzer *analyzer, const XrTypeRef *tref) {
    if (!analyzer || !tref || !tref->name || !is_known_generic_head(tref->name))
        return NULL;

    int nargs = tref->nchildren;
    XrType *stack_args[16];
    XrType **args =
        (nargs <= 16) ? stack_args : (XrType **) xr_malloc((size_t) nargs * sizeof(XrType *));
    if (nargs > 0 && !args)
        return xr_type_new_error(NULL);
    for (int i = 0; i < nargs; i++)
        args[i] = xr_tref_resolve_in_analyzer(analyzer, tref->children[i]);

    XrVMRuntime *X = analyzer->isolate;
    const char *name = tref->name;
    int expected_arity = known_type_head_arity(name);
    if (expected_arity >= 0 && nargs != expected_arity) {
        if (args != stack_args)
            xr_free(args);
        return report_generic_arity_error(analyzer, name, expected_arity, nargs);
    }
    if (reject_error_type_args(analyzer, args, nargs, name)) {
        if (args != stack_args)
            xr_free(args);
        return xr_type_new_error(NULL);
    }

    XrType *result = NULL;
    if (nargs == 0 && expected_arity == 0) {
        result = resolve_known_named(X, name);
    } else if (strcmp(name, "Array") == 0 && nargs == 1) {
        result = xr_type_new_array(X, args[0]);
    } else if (strcmp(name, TYPE_NAME_SLICE) == 0 && nargs == 1) {
        result = xr_type_new_slice(X, args[0]);
    } else if (strcmp(name, "Set") == 0 && nargs == 1) {
        result = xr_type_new_set(X, args[0]);
    } else if (strcmp(name, "Channel") == 0 && nargs == 1) {
        result = xr_type_new_channel(X, args[0]);
    } else if (strcmp(name, "Map") == 0 && nargs == 2) {
        result = xr_type_new_map(X, args[0], args[1]);
    } else if (strcmp(name, "JSON.WithRest") == 0 && nargs == 1) {
        const char *field_names[2] = {"rest", "value"};
        XrType *field_types[2] = {xr_type_new_map(X, xr_type_new_string(NULL), xr_type_new_json(X)),
                                  args[0]};
        result = xr_type_new_struct_object_with_fields(X, field_names, field_types, 2);
        if (result)
            result->object.type_name = "JSON.WithRest";
    } else if (strcmp(name, "Task") == 0 && nargs == 1) {
        result = xr_type_new_task(X, args[0]);
    } else if (strcmp(name, "Ptr") == 0 && nargs == 1) {
        result = xr_type_new_pointer(X, args[0], false);
    } else if (strcmp(name, "MutPtr") == 0 && nargs == 1) {
        result = xr_type_new_pointer(X, args[0], true);
    } else if (strcmp(name, "CFn") == 0 && nargs == 1) {
        if (args[0] && args[0]->kind == XR_KIND_FUNCTION) {
            result = xr_type_copy(X, args[0]);
            if (result)
                result->function.is_c_abi = true;
        } else {
            result = report_generic_type_argument_error(analyzer, name,
                                                        "requires a function type argument");
        }
    } else if (xr_type_is_enum_metadata_type_name(name) && nargs == 1 && args[0] &&
               args[0]->kind == XR_KIND_ENUM && args[0]->enum_type.layout) {
        result = xr_type_new_enum_metadata(X, name, args[0]);
    } else if (xa_is_builtin_interface_name(name)) {
        result = xr_type_new_generic_interface(X, name, args, nargs);
    } else {
        result = xr_type_new_generic_instance(X, name, NULL, args, nargs);
    }

    if (args != stack_args)
        xr_free(args);
    return result;
}

/* Generic value-struct annotations (e.g. `Box<int>`) must resolve to the same
 * monomorphized concrete instance that the corresponding struct literal
 * `Box<int>{...}` produces after the mono pass (`Box$i64`). Resolving the
 * annotation to the mangled instance lets `xr_type_equals` match the two by
 * class_ref identity instead of rejecting `Box$i64` vs `Box<int>`; it also lets
 * struct field-layout validation see a concrete nested aggregate. The mangled
 * name is computed with the exact same helper the mono pass uses for the
 * literal, so annotation and initializer land on one canonical type. Returns
 * NULL when the concrete instantiation has not been registered (annotation-only
 * usage that the mono pass never materialized), so the caller falls back to the
 * generic-instance form. */
static XrType *resolve_generic_value_struct_mono_instance(XaAnalyzer *analyzer,
                                                          const XrTypeRef *tref) {
    if (!analyzer || !tref || !tref->name || tref->nchildren <= 0)
        return NULL;
    char *mangled = xr_mono_mangle(tref->name, (XrTypeRef **) tref->children, tref->nchildren);
    if (!mangled)
        return NULL;
    XrType *result = NULL;
    XaSymbol *mono_sym = resolve_type_symbol(analyzer, mangled);
    if (mono_sym && mono_sym->kind == XA_SYM_CLASS) {
        XaSymbolLinks *mono_links = xa_analyzer_get_links(analyzer, mono_sym);
        if (mono_links && mono_links->class_info) {
            result = xr_type_new_instance(analyzer->isolate, mono_links->class_info);
            if (result)
                result->is_value_type = true;
        }
    }
    xr_free(mangled);
    return result;
}

XR_FUNC XrType *xr_tref_resolve_in_analyzer(XaAnalyzer *analyzer, const XrTypeRef *tref) {
    if (!tref)
        return xr_type_new_error(NULL);
    if (!analyzer)
        return resolve_impl(NULL, tref);

    if (tref->kind == XR_TREF_CONST) {
        XrType *inner = tref->nchildren > 0
                            ? xr_tref_resolve_in_analyzer(analyzer, tref->children[0])
                            : xr_type_new_error(NULL);
        return xr_type_make_const(analyzer->isolate, inner);
    }

    /* Named class lookup preserves the inheritance chain that
     * xr_type_new_class() would otherwise drop. */
    if (tref->kind == XR_TREF_NAMED && tref->name) {
        if (is_removed_enum_runtime_wrapper_name(tref->name)) {
            report_removed_enum_runtime_wrapper_type(analyzer, tref->name);
            return xr_type_new_error(NULL);
        }
        int type_param_index = active_type_param_index(analyzer, tref->name);
        if (type_param_index >= 0)
            return xr_type_new_type_param(analyzer->isolate, tref->name, type_param_index);
        int expected_arity = known_type_head_arity(tref->name);
        if (expected_arity > 0)
            return report_generic_arity_error(analyzer, tref->name, expected_arity, 0);
        XaSymbol *sym = resolve_type_symbol(analyzer, tref->name);
        if (sym && sym->kind == XA_SYM_TYPE_ALIAS)
            return resolve_type_alias_symbol_in_analyzer(analyzer, sym, NULL, 0);
        XrType *cls = resolve_type_ref_symbol_type(analyzer, tref->name);
        if (cls)
            return cls;
        XrType *known = resolve_known_named(analyzer->isolate, tref->name);
        if (known)
            return known;
        report_undefined_public_type(analyzer, tref->name);
        return xr_type_new_error(NULL);
    }

    /* Generic form: preserve declaration-backed class/interface identity so
     * member lookup and conformance checks do not fall back to a bare name. */
    if (tref->kind == XR_TREF_GENERIC && tref->name) {
        if (is_removed_enum_runtime_wrapper_name(tref->name)) {
            report_removed_enum_runtime_wrapper_type(analyzer, tref->name);
            return xr_type_new_error(NULL);
        }
        if (active_type_param_index(analyzer, tref->name) >= 0) {
            report_undefined_public_type(analyzer, tref->name);
            return xr_type_new_error(NULL);
        }
        int expected_arity = known_type_head_arity(tref->name);
        if (expected_arity >= 0 && tref->nchildren != expected_arity)
            return report_generic_arity_error(analyzer, tref->name, expected_arity,
                                              tref->nchildren);
        if (expected_arity >= 0)
            return resolve_known_generic_in_analyzer(analyzer, tref);
        XaSymbol *sym = resolve_type_symbol(analyzer, tref->name);
        if (sym && sym->kind == XA_SYM_TYPE_ALIAS)
            return resolve_type_alias_symbol_in_analyzer(analyzer, sym, tref->children,
                                                         tref->nchildren);
        XaSymbolLinks *links = sym ? xa_analyzer_get_links(analyzer, sym) : NULL;
        XrType *head = links ? links->type : NULL;
        if (head && (head->kind == XR_KIND_INTERFACE || head->kind == XR_KIND_CLASS ||
                     head->kind == XR_KIND_INSTANCE || head->kind == XR_KIND_ENUM)) {
            int nargs = tref->nchildren;
            XrType *stack_args[8];
            XrType **args =
                (nargs <= 8) ? stack_args : (XrType **) xr_malloc(sizeof(XrType *) * nargs);
            if (args) {
                for (int i = 0; i < nargs; i++)
                    args[i] = xr_tref_resolve_in_analyzer(analyzer, tref->children[i]);
                if (reject_error_type_args(analyzer, args, nargs, tref->name)) {
                    if (args != stack_args)
                        xr_free(args);
                    return xr_type_new_error(NULL);
                }
                /* A generic value struct annotation resolves to its
                 * monomorphized concrete instance so it is the same canonical
                 * type as the corresponding struct literal. Nominal classes
                 * keep their generic-instance identity (handled below). */
                if (head->is_value_type &&
                    (head->kind == XR_KIND_CLASS || head->kind == XR_KIND_INSTANCE)) {
                    XrType *mono = resolve_generic_value_struct_mono_instance(analyzer, tref);
                    if (mono) {
                        if (args != stack_args)
                            xr_free(args);
                        return mono;
                    }
                }
                XrType *result =
                    head->kind == XR_KIND_INTERFACE
                        ? xr_type_new_generic_interface(analyzer->isolate, tref->name, args, nargs)
                        : xr_type_new_generic_instance(analyzer->isolate, tref->name,
                                                       links->class_info, args, nargs);
                if (args != stack_args)
                    xr_free(args);
                return result;
            }
        }
        XrType *known = resolve_known_generic_in_analyzer(analyzer, tref);
        if (known)
            return known;
        report_undefined_public_type(analyzer, tref->name);
        return xr_type_new_error(NULL);
    }

    if (tref->kind == XR_TREF_OPTIONAL) {
        XrType *inner = tref->nchildren > 0
                            ? xr_tref_resolve_in_analyzer(analyzer, tref->children[0])
                            : xr_type_new_error(NULL);
        return xr_type_new_optional(analyzer->isolate, inner);
    }

    if (tref->kind == XR_TREF_UNION) {
        XrType *members[XR_UNION_MAX_MEMBERS];
        int count = tref->nchildren < XR_UNION_MAX_MEMBERS ? tref->nchildren : XR_UNION_MAX_MEMBERS;
        for (int i = 0; i < count; i++)
            members[i] = xr_tref_resolve_in_analyzer(analyzer, tref->children[i]);
        XrType *resolved = xr_type_new_union(analyzer->isolate, members, count);
        if (report_indiscriminable_union(analyzer, tref, resolved))
            return xr_type_new_error(NULL);
        return resolved;
    }

    if (tref->kind == XR_TREF_FUNCTION) {
        int nparam = tref->nchildren > 0 ? tref->nchildren - 1 : 0;
        XrType *stack_params[16];
        XrType **params = (nparam <= 16)
                              ? stack_params
                              : (XrType **) xr_malloc((size_t) nparam * sizeof(XrType *));
        if (nparam > 0 && !params)
            return xr_type_new_error(NULL);
        for (int i = 0; i < nparam; i++)
            params[i] = xr_tref_resolve_in_analyzer(analyzer, tref->children[i]);
        XrType *ret =
            tref->nchildren > 0
                ? xr_tref_resolve_in_analyzer(analyzer, tref->children[tref->nchildren - 1])
                : xr_type_new_unit(NULL);
        XrType *result =
            xr_type_new_function(analyzer->isolate, nparam > 0 ? params : NULL, nparam, ret, false);
        if (result && tref->requires_nothrow)
            xr_type_function_set_throw_effect(result, XR_FN_EFFECT_NO_THROW);
        if (result && tref->function_param_modes) {
            for (int i = 0; i < nparam; i++)
                xr_type_function_set_param_mode(result, i, tref->function_param_modes[i]);
        }
        if (params != stack_params)
            xr_free(params);
        return result;
    }

    if (tref->kind == XR_TREF_TUPLE) {
        int count = tref->nchildren;
        XrType *stack_elems[16];
        XrType **elems =
            (count <= 16) ? stack_elems : (XrType **) xr_malloc((size_t) count * sizeof(XrType *));
        if (count > 0 && !elems)
            return xr_type_new_error(NULL);
        for (int i = 0; i < count; i++)
            elems[i] = xr_tref_resolve_in_analyzer(analyzer, tref->children[i]);
        XrType *result = xr_type_new_tuple(analyzer->isolate, elems, count);
        if (elems != stack_elems)
            xr_free(elems);
        return result;
    }

    if (tref->kind == XR_TREF_OBJECT) {
        int count = tref->nchildren;
        const char *stack_names[16];
        XrType *stack_types[16];
        bool stack_readonly[16];
        const char **names = count <= 16
                                 ? stack_names
                                 : (const char **) xr_malloc((size_t) count * sizeof(const char *));
        XrType **types =
            count <= 16 ? stack_types : (XrType **) xr_malloc((size_t) count * sizeof(XrType *));
        bool *readonly =
            count <= 16 ? stack_readonly : (bool *) xr_malloc((size_t) count * sizeof(bool));
        if (count > 0 && (!names || !types || !readonly)) {
            if (names != stack_names)
                xr_free((void *) names);
            if (types != stack_types)
                xr_free(types);
            if (readonly != stack_readonly)
                xr_free(readonly);
            return xr_type_new_error(NULL);
        }
        for (int i = 0; i < count; i++) {
            names[i] = tref->field_names[i];
            types[i] = xr_tref_resolve_in_analyzer(analyzer, tref->children[i]);
            readonly[i] = tref->field_readonly ? tref->field_readonly[i] : false;
        }
        canonicalize_object_fields(names, types, readonly, count);
        XrType *result =
            xr_type_new_struct_object_with_fields(analyzer->isolate, names, types, count);
        if (result && tref->field_readonly)
            xr_type_set_object_field_readonly(analyzer->isolate, result, readonly, count);
        if (result && tref->name)
            xr_type_set_object_type_name(analyzer->isolate, result, tref->name);
        if (names != stack_names)
            xr_free((void *) names);
        if (types != stack_types)
            xr_free(types);
        if (readonly != stack_readonly)
            xr_free(readonly);
        return result;
    }

    if (tref->kind == XR_TREF_FIXED_ARRAY) {
        XrType *elem = tref->nchildren > 0
                           ? xr_tref_resolve_in_analyzer(analyzer, tref->children[0])
                           : xr_type_new_error(NULL);
        if (xa_reject_error_type_success_type(analyzer, elem, "container element type",
                                              "fixed array", 0, 0))
            return xr_type_new_error(NULL);
        int64_t length = tref->fixed_length;
        const AstNode *length_expr = tref->fixed_length_expr;
        if (length_expr) {
            const char *err = NULL;
            if (!xa_eval_const_int_expr(analyzer, length_expr, &length, &err)) {
                char msg[192];
                snprintf(msg, sizeof(msg),
                         "fixed array length must be a compile-time integer expression%s%s",
                         err ? ": " : "", err ? err : "");
                xa_report_fixed_array_length_diag(analyzer, length_expr, msg);
                return xr_type_new_error(NULL);
            }
        }
        if (length <= 0) {
            xa_report_fixed_array_length_diag(analyzer, length_expr,
                                              "fixed array length must be greater than zero");
            return xr_type_new_error(NULL);
        }
        if (length > UINT16_MAX) {
            xa_report_fixed_array_length_diag(
                analyzer, length_expr, "fixed array length exceeds maximum of 65535 elements");
            return xr_type_new_error(NULL);
        }
        return xr_type_new_fixed_array(analyzer->isolate, elem, (int) length);
    }

    return resolve_impl(analyzer->isolate, tref);
}

XrType *xr_tref_resolve_parameter_in_analyzer(XaAnalyzer *analyzer, const XrTypeRef *tref) {
    XrType *type = xr_tref_resolve_in_analyzer(analyzer, tref);
    if (type && tref && tref->kind == XR_TREF_FUNCTION && type->kind == XR_KIND_FUNCTION) {
        xr_type_function_set_throw_effect(type, tref->requires_nothrow ? XR_FN_EFFECT_NO_THROW
                                                                       : XR_FN_EFFECT_POLY);
    }
    return type;
}
