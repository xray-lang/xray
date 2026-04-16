/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_ops.c - Instruction operation helper functions
 *
 * KEY CONCEPT:
 *   Extracted helper functions for type conversion, string operations,
 *   arithmetic operations, and deep comparison with cycle detection.
 */

#include "xvm_internal.h"
#include "../base/xchecks.h"
#include "../runtime/value/xstruct_layout.h"
#include <inttypes.h>
#include "../coro/xchannel.h"
#include "../module/xmodule.h"
#include "../runtime/object/xstringbuilder.h"
#include "../runtime/object/xrange.h"
#include <stdlib.h>
#include <time.h>

#include "../runtime/xstdlib_bridge.h"

/* ========== Cycle Detection Data Structures ========== */

#define MAX_COMPARE_DEPTH 100
#define MAX_VISITING_PAIRS 64

typedef struct {
    void *a;
    void *b;
} ObjectPair;

typedef struct {
    XrayIsolate *isolate;
    ObjectPair visiting[MAX_VISITING_PAIRS];
    int visiting_count;
    int depth;
} CompareContext;

static bool deep_compare(CompareContext *ctx, XrValue a, XrValue b);

/* ========== Type Conversion Helpers ========== */

#define VM_TOSTRING_MAX_DEPTH 3
#define VM_TOSTRING_MAX_ELEMENTS 32

static void vm_value_to_strbuf(XrayIsolate *isolate, XrStrBuf *sb, XrValue val, int depth);

static void vm_array_to_strbuf(XrayIsolate *isolate, XrStrBuf *sb, XrArray *arr, int depth) {
    xr_strbuf_append_cstr(sb, "[", 1);
    int len = arr->length;
    int limit = (len > VM_TOSTRING_MAX_ELEMENTS) ? VM_TOSTRING_MAX_ELEMENTS : len;
    for (int i = 0; i < limit; i++) {
        if (i > 0) xr_strbuf_append_cstr(sb, ", ", 2);
        vm_value_to_strbuf(isolate, sb, xr_array_get_element(arr, i), depth + 1);
    }
    if (len > limit) {
        char more[32];
        int n = snprintf(more, sizeof(more), ", ...(%d more)", len - limit);
        xr_strbuf_append_cstr(sb, more, (size_t)n);
    }
    xr_strbuf_append_cstr(sb, "]", 1);
}

static void vm_map_to_strbuf(XrayIsolate *isolate, XrStrBuf *sb, XrMap *map, int depth) {
    xr_strbuf_append_cstr(sb, "#{", 2);
    if (!xr_map_isdummy(map)) {
        uint32_t size = xr_map_sizenode(map);
        int count = 0;
        for (uint32_t i = 0; i < size && count < VM_TOSTRING_MAX_ELEMENTS; i++) {
            XrMapNode *node = xr_map_node(map, i);
            if (!XR_MAP_NODE_EMPTY(node)) {
                if (count > 0) xr_strbuf_append_cstr(sb, ", ", 2);
                vm_value_to_strbuf(isolate, sb, node->key, depth + 1);
                xr_strbuf_append_cstr(sb, " => ", 4);
                vm_value_to_strbuf(isolate, sb, node->value, depth + 1);
                count++;
            }
        }
        if ((uint32_t)count < map->count) {
            char more[32];
            int n = snprintf(more, sizeof(more), ", ...(%u more)", map->count - (uint32_t)count);
            xr_strbuf_append_cstr(sb, more, (size_t)n);
        }
    }
    xr_strbuf_append_cstr(sb, "}", 1);
}

static void vm_set_to_strbuf(XrayIsolate *isolate, XrStrBuf *sb, XrSet *set, int depth) {
    xr_strbuf_append_cstr(sb, "#[", 2);
    int count = 0;
    for (uint32_t i = 0; i < set->capacity && count < VM_TOSTRING_MAX_ELEMENTS; i++) {
        XrSetEntry *entry = &set->entries[i];
        if (entry->state & XR_SET_VALID) {
            if (count > 0) xr_strbuf_append_cstr(sb, ", ", 2);
            vm_value_to_strbuf(isolate, sb, entry->value, depth + 1);
            count++;
        }
    }
    if ((uint32_t)count < set->count) {
        char more[32];
        int n = snprintf(more, sizeof(more), ", ...(%u more)", set->count - (uint32_t)count);
        xr_strbuf_append_cstr(sb, more, (size_t)n);
    }
    xr_strbuf_append_cstr(sb, "]", 1);
}

static void vm_json_to_strbuf(XrayIsolate *isolate, XrStrBuf *sb, XrJson *json, int depth) {
    xr_strbuf_append_cstr(sb, "{", 1);
    XrSymbolTable *st = (XrSymbolTable*)isolate->symbol_table;
    XrShape *shape = xr_json_shape(json);
    for (int i = 0; i < shape->field_count && i < VM_TOSTRING_MAX_ELEMENTS; i++) {
        if (i > 0) xr_strbuf_append_cstr(sb, ", ", 2);
        const char *fname = xr_symbol_get_name_in_table(st, shape->field_symbols[i]);
        if (fname) xr_strbuf_append_cstr(sb, fname, strlen(fname));
        else xr_strbuf_append_cstr(sb, "?", 1);
        xr_strbuf_append_cstr(sb, ": ", 2);
        vm_value_to_strbuf(isolate, sb, xr_json_get_field_any(json, i), depth + 1);
    }
    xr_strbuf_append_cstr(sb, "}", 1);
}

static void vm_value_to_strbuf(XrayIsolate *isolate, XrStrBuf *sb, XrValue val, int depth) {
    if (XR_IS_STRING(val)) {
        if (depth > 0) xr_strbuf_append_cstr(sb, "\"", 1);
        xr_strbuf_append_cstr(sb, xr_value_str_data(&val), xr_value_str_len(&val));
        if (depth > 0) xr_strbuf_append_cstr(sb, "\"", 1);
        return;
    }
    if (XR_IS_INT(val)) {
        xr_strbuf_append_int(sb, XR_TO_INT(val));
        return;
    }
    if (XR_IS_FLOAT(val)) {
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "%g", XR_TO_FLOAT(val));
        xr_strbuf_append_cstr(sb, buf, (size_t)n);
        return;
    }
    if (XR_IS_BOOL(val)) {
        if (XR_TO_BOOL(val)) xr_strbuf_append_cstr(sb, "true", 4);
        else xr_strbuf_append_cstr(sb, "false", 5);
        return;
    }
    if (XR_IS_NULL(val)) {
        xr_strbuf_append_cstr(sb, "null", 4);
        return;
    }
    
    if (!XR_IS_PTR(val)) {
        xr_strbuf_append_cstr(sb, "<unknown>", 9);
        return;
    }
    
    // Depth guard for recursive types
    if (depth > VM_TOSTRING_MAX_DEPTH) {
        xr_strbuf_append_cstr(sb, "...", 3);
        return;
    }
    
    XrGCHeader *gc = (XrGCHeader*)XR_TO_PTR(val);
    XrObjType type = XR_GC_GET_TYPE(gc);
    
    switch (type) {
    case XR_TARRAY:
    case XR_TARRAY_SLICE:
        vm_array_to_strbuf(isolate, sb, (XrArray*)gc, depth);
        break;
    case XR_TMAP:
        vm_map_to_strbuf(isolate, sb, (XrMap*)gc, depth);
        break;
    case XR_TSET:
        vm_set_to_strbuf(isolate, sb, (XrSet*)gc, depth);
        break;
    case XR_TJSON:
        vm_json_to_strbuf(isolate, sb, (XrJson*)gc, depth);
        break;
    case XR_TBIGINT: {
        char *s = xr_bigint_to_string((XrBigInt*)gc);
        if (s) {
            xr_strbuf_append_cstr(sb, s, strlen(s));
            free(s);
        } else {
            xr_strbuf_append_cstr(sb, "<BigInt>", 8);
        }
        break;
    }
    case XR_TDATETIME: {
        char buf[64];
        int n = xr_datetime_format((void*)gc, XR_DATETIME_DEFAULT_FORMAT, buf, sizeof(buf));
        if (n > 0) {
            xr_strbuf_append_cstr(sb, buf, (size_t)n);
        } else {
            xr_strbuf_append_cstr(sb, "<DateTime>", 10);
        }
        break;
    }
    case XR_TENUM_VALUE: {
        XrEnumValue *ev = (XrEnumValue*)gc;
        if (ev->enum_name && ev->member_name) {
            xr_strbuf_append_cstr(sb, ev->enum_name, strlen(ev->enum_name));
            xr_strbuf_append_cstr(sb, ".", 1);
            xr_strbuf_append_cstr(sb, ev->member_name, strlen(ev->member_name));
        } else {
            xr_strbuf_append_cstr(sb, "<enum>", 6);
        }
        break;
    }
    case XR_TENUM_TYPE: {
        XrEnumType *et = (XrEnumType*)gc;
        xr_strbuf_append_cstr(sb, "enum ", 5);
        if (et->name) xr_strbuf_append_cstr(sb, et->name, strlen(et->name));
        break;
    }
    case XR_TINSTANCE: {
        XrInstance *inst = xr_value_to_instance(val);
        XrClass *cls = xr_instance_get_class(inst);
        if (cls && cls->name) {
            xr_strbuf_append_cstr(sb, cls->name, strlen(cls->name));
            xr_strbuf_append_cstr(sb, "{...}", 5);
        } else {
            xr_strbuf_append_cstr(sb, "<instance>", 10);
        }
        break;
    }
    case XR_TCLASS: {
        XrClass *cls = xr_value_to_class(val);
        xr_strbuf_append_cstr(sb, "class ", 6);
        if (cls && cls->name) xr_strbuf_append_cstr(sb, cls->name, strlen(cls->name));
        break;
    }
    case XR_TCOROUTINE: {
        XrCoroutine *coro = (XrCoroutine*)gc;
        xr_strbuf_append_cstr(sb, "Coroutine(", 10);
        if (coro->name) xr_strbuf_append_cstr(sb, coro->name, strlen(coro->name));
        else xr_strbuf_append_cstr(sb, "anonymous", 9);
        xr_strbuf_append_cstr(sb, ")", 1);
        break;
    }
    case XR_TCHANNEL: {
        XrChannel *ch = (XrChannel*)gc;
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "Channel(cap=%u, count=%u)",
                        ch->buf_size, ch->buf_count);
        xr_strbuf_append_cstr(sb, buf, (size_t)n);
        break;
    }
    case XR_TFUNCTION: {
        XrClosure *closure = (XrClosure*)gc;
        xr_strbuf_append_cstr(sb, "fn ", 3);
        if (closure->proto && closure->proto->name)
            xr_strbuf_append_str(sb, closure->proto->name);
        else
            xr_strbuf_append_cstr(sb, "<anonymous>", 11);
        break;
    }
    case XR_TCFUNCTION:
        xr_strbuf_append_cstr(sb, "<native fn>", 11);
        break;
    case XR_TMODULE: {
        XrModule *mod = (XrModule*)gc;
        xr_strbuf_append_cstr(sb, "module ", 7);
        if (mod->name) xr_strbuf_append_cstr(sb, mod->name, strlen(mod->name));
        break;
    }
    case XR_TEXCEPTION: {
        XrException *exc = (XrException*)gc;
        if (exc->message) {
            xr_strbuf_append_cstr(sb, "Error: ", 7);
            xr_strbuf_append_str(sb, exc->message);
        } else {
            xr_strbuf_append_cstr(sb, "<exception>", 11);
        }
        break;
    }
    case XR_TERROR: {
        xr_strbuf_append_cstr(sb, "<error>", 7);
        break;
    }
    case XR_TITERATOR:
        xr_strbuf_append_cstr(sb, "<iterator>", 10);
        break;
    case XR_TSTRINGBUILDER: {
        XrStringBuilder *sbuilder = (XrStringBuilder*)gc;
        XrString *content = xr_stringbuilder_to_string(sbuilder);
        if (content && content->length > 0) {
            xr_strbuf_append_cstr(sb, "StringBuilder(\"", 15);
            if (content->length <= 64) {
                xr_strbuf_append_str(sb, content);
            } else {
                xr_strbuf_append_cstr(sb, content->data, 64);
                xr_strbuf_append_cstr(sb, "...", 3);
            }
            xr_strbuf_append_cstr(sb, "\")", 2);
        } else {
            xr_strbuf_append_cstr(sb, "StringBuilder()", 14);
        }
        break;
    }
    case XR_TCOROPOOL:
        xr_strbuf_append_cstr(sb, "<CoroPool>", 10);
        break;
    case XR_TREGEX: {
        struct XrRegex *re = xr_value_to_regex(val);
        const char *pat = re ? xr_regex_pattern(re) : NULL;
        if (pat) {
            xr_strbuf_append_cstr(sb, "/", 1);
            xr_strbuf_append_cstr(sb, pat, strlen(pat));
            xr_strbuf_append_cstr(sb, "/", 1);
        } else {
            xr_strbuf_append_cstr(sb, "<Regex>", 7);
        }
        break;
    }
    case XR_TRANGE: {
        XrRange *rng = (XrRange*)gc;
        char buf[80];
        int n = snprintf(buf, sizeof(buf), "%" PRId64 "..%" PRId64, (int64_t)rng->start, (int64_t)rng->end);
        xr_strbuf_append_cstr(sb, buf, (size_t)n);
        break;
    }
    case XR_TBOUND_METHOD: {
        XrBoundMethod *bm = (XrBoundMethod*)gc;
        xr_strbuf_append_cstr(sb, "<bound_method", 13);
        if (xr_value_is_instance(bm->receiver)) {
            XrInstance *inst = xr_value_to_instance(bm->receiver);
            XrClass *cls = xr_instance_get_class(inst);
            if (cls && cls->name) {
                xr_strbuf_append_cstr(sb, " ", 1);
                xr_strbuf_append_cstr(sb, cls->name, strlen(cls->name));
            }
        }
        xr_strbuf_append_cstr(sb, ">", 1);
        break;
    }
    default: {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "<%s>", xr_typeid_name(xr_value_typeid(val)));
        xr_strbuf_append_cstr(sb, buf, (size_t)n);
        break;
    }
    }
}

// Convert any value to string (for string concatenation and toString)
XrString* vm_value_to_string(XrayIsolate *isolate, XrValue val) {
    XR_DCHECK(isolate != NULL, "vm_value_to_string: NULL isolate");
    // Fast paths for common types (no XrStrBuf needed)
    if (XR_IS_STRING(val)) {
        return (XrString*)val.ptr;
    }
    if (XR_IS_INT(val)) {
        return xr_string_from_int(isolate, XR_TO_INT(val));
    }
    if (XR_IS_FLOAT(val)) {
        return xr_string_from_float(isolate, XR_TO_FLOAT(val));
    }
    if (XR_IS_BOOL(val)) {
        return XR_TO_BOOL(val) ? xr_string_intern(isolate, "true", 4, 0)
                               : xr_string_intern(isolate, "false", 5, 0);
    }
    if (XR_IS_NULL(val)) {
        return xr_string_intern(isolate, "null", 4, 0);
    }
    
    // General path: use dedicated XrStrBuf (not tmp, to avoid nesting issues
    // when called from OP_ADD which already holds xr_strbuf_tmp)
    XrStrBuf *sb = xr_strbuf_new(isolate, 128);
    vm_value_to_strbuf(isolate, sb, val, 0);
    XrString *result = xr_strbuf_to_string(sb);
    xr_strbuf_free(sb);
    return result;
}

// String concatenation
static inline XrValue vm_string_concat_values(XrayIsolate *isolate, XrValue left, XrValue right) {
    XR_DCHECK(isolate != NULL, "string_concat_values: NULL isolate");
    XrString *str_left = vm_value_to_string(isolate, left);
    XrString *str_right = vm_value_to_string(isolate, right);
    XrString *result = xr_string_concat(isolate, str_left, str_right);
    return xr_string_value(result);
}

// Numeric addition (handles int/float mix)
static inline XrValue vm_numeric_add(XrValue left, XrValue right) {
    if (XR_IS_INT(left) && XR_IS_INT(right)) {
        return xr_int(XR_TO_INT(left) + XR_TO_INT(right));
    } else if ((XR_IS_INT(left) || XR_IS_FLOAT(left)) && (XR_IS_INT(right) || XR_IS_FLOAT(right))) {
        double nl = XR_IS_INT(left) ? (double)XR_TO_INT(left) : XR_TO_FLOAT(left);
        double nr = XR_IS_INT(right) ? (double)XR_TO_INT(right) : XR_TO_FLOAT(right);
        return xr_float(nl + nr);
    }
    return xr_null();
}

/* ========== Arithmetic Operation Helpers ========== */

// Generic add operation (numeric or string+string, no implicit conversion)
XrValue vm_add_operation(XrayIsolate *isolate, XrValue left, XrValue right) {
    XR_DCHECK(isolate != NULL, "vm_add_operation: NULL isolate");
    if ((XR_IS_INT(left) || XR_IS_FLOAT(left)) && (XR_IS_INT(right) || XR_IS_FLOAT(right))) {
        return vm_numeric_add(left, right);
    }
    if (XR_IS_STRING(left) && XR_IS_STRING(right)) {
        return vm_string_concat_values(isolate, left, right);
    }
    return xr_null();
}

// Subtraction
XrValue vm_numeric_sub(XrValue left, XrValue right) {
    if (XR_IS_INT(left) && XR_IS_INT(right)) {
        return xr_int(XR_TO_INT(left) - XR_TO_INT(right));
    } else if ((XR_IS_INT(left) || XR_IS_FLOAT(left)) && (XR_IS_INT(right) || XR_IS_FLOAT(right))) {
        double nl = XR_IS_INT(left) ? (double)XR_TO_INT(left) : XR_TO_FLOAT(left);
        double nr = XR_IS_INT(right) ? (double)XR_TO_INT(right) : XR_TO_FLOAT(right);
        return xr_float(nl - nr);
    }
    return xr_null();
}

// Multiplication
XrValue vm_numeric_mul(XrValue left, XrValue right) {
    if (XR_IS_INT(left) && XR_IS_INT(right)) {
        return xr_int(XR_TO_INT(left) * XR_TO_INT(right));
    } else if ((XR_IS_INT(left) || XR_IS_FLOAT(left)) && (XR_IS_INT(right) || XR_IS_FLOAT(right))) {
        double nl = XR_IS_INT(left) ? (double)XR_TO_INT(left) : XR_TO_FLOAT(left);
        double nr = XR_IS_INT(right) ? (double)XR_TO_INT(right) : XR_TO_FLOAT(right);
        return xr_float(nl * nr);
    }
    return xr_null();
}

// Division
XrValue vm_numeric_div(XrayIsolate *isolate, XrValue left, XrValue right) {
    XR_DCHECK(isolate != NULL, "vm_numeric_div: NULL isolate");
    if ((XR_IS_INT(left) || XR_IS_FLOAT(left)) && (XR_IS_INT(right) || XR_IS_FLOAT(right))) {
        double nl = XR_IS_INT(left) ? (double)XR_TO_INT(left) : XR_TO_FLOAT(left);
        double nr = XR_IS_INT(right) ? (double)XR_TO_INT(right) : XR_TO_FLOAT(right);
        
        if (nr == 0.0) {
            xr_runtime_error(isolate, "Division by zero");
            return xr_null();
        }
        
        return xr_float(nl / nr);
    }
    return xr_null();
}

// Modulo
XrValue vm_numeric_mod(XrayIsolate *isolate, XrValue left, XrValue right) {
    XR_DCHECK(isolate != NULL, "vm_numeric_mod: NULL isolate");
    if (XR_IS_INT(left) && XR_IS_INT(right)) {
        xr_Integer r = XR_TO_INT(right);
        if (r == 0) {
            xr_runtime_error(isolate, "Modulo by zero");
            return xr_null();
        }
        return xr_int(XR_TO_INT(left) % r);
    }
    return xr_null();
}

/* ========== Deep Comparison Helpers ========== */

// Check if object pair is being visited (cycle detection)
static bool is_visiting(CompareContext *ctx, void *a, void *b) {
    XR_DCHECK(ctx != NULL, "is_visiting: NULL ctx");
    for (int i = 0; i < ctx->visiting_count; i++) {
        if ((ctx->visiting[i].a == a && ctx->visiting[i].b == b) ||
            (ctx->visiting[i].a == b && ctx->visiting[i].b == a)) {
            return true;
        }
    }
    return false;
}

// Add object pair to visiting list
static bool push_visiting(CompareContext *ctx, void *a, void *b) {
    XR_DCHECK(ctx != NULL, "push_visiting: NULL ctx");
    if (ctx->visiting_count >= MAX_VISITING_PAIRS) {
        return false;
    }
    ctx->visiting[ctx->visiting_count].a = a;
    ctx->visiting[ctx->visiting_count].b = b;
    ctx->visiting_count++;
    return true;
}

// Remove object pair from visiting list
static void pop_visiting(CompareContext *ctx) {
    if (ctx->visiting_count > 0) {
        ctx->visiting_count--;
    }
}

// Array deep comparison
static bool array_deep_equal(CompareContext *ctx, XrArray *a, XrArray *b) {
    if (a == b) return true;
    if (a->length != b->length) return false;
    if (ctx->depth > MAX_COMPARE_DEPTH) return false;
    
    if (is_visiting(ctx, a, b)) return false;
    if (!push_visiting(ctx, a, b)) return false;
    
    ctx->depth++;
    
    bool result = true;
    for (int i = 0; i < a->length; i++) {
        if (!deep_compare(ctx, ((XrValue*)a->data)[i], ((XrValue*)b->data)[i])) {
            result = false;
            break;
        }
    }
    
    ctx->depth--;
    pop_visiting(ctx);
    
    return result;
}

// Map deep comparison
static bool map_deep_equal(CompareContext *ctx, XrMap *a, XrMap *b) {
    if (a == b) return true;
    if (a->count != b->count) return false;
    if (ctx->depth > MAX_COMPARE_DEPTH) return false;
    
    if (is_visiting(ctx, a, b)) return false;
    if (!push_visiting(ctx, a, b)) return false;
    
    ctx->depth++;
    
    bool result = true;
    if (!xr_map_isdummy(a)) {
        uint32_t size = xr_map_sizenode(a);
        for (size_t i = 0; i < size; i++) {
            XrMapNode *node = xr_map_node(a, i);
            if (!XR_MAP_NODE_EMPTY(node)) {
                bool found = false;
                XrValue b_value = xr_map_get(b, node->key, &found);
                if (!found) {
                    result = false;
                    break;
                }
                if (!deep_compare(ctx, node->value, b_value)) {
                    result = false;
                    break;
                }
            }
        }
    }
    
    ctx->depth--;
    pop_visiting(ctx);
    
    return result;
}

// Set deep comparison
static bool set_deep_equal(CompareContext *ctx, XrSet *a, XrSet *b) {
    if (a == b) return true;
    if (a->count != b->count) return false;
    if (ctx->depth > MAX_COMPARE_DEPTH) return false;
    
    if (is_visiting(ctx, a, b)) return false;
    if (!push_visiting(ctx, a, b)) return false;
    
    ctx->depth++;
    
    bool result = true;
    for (size_t i = 0; i < a->capacity; i++) {
        XrSetEntry *entry = &a->entries[i];
        if (entry->state & XR_SET_VALID) {
            if (!xr_set_has(b, entry->value)) {
                result = false;
                break;
            }
        }
    }
    
    ctx->depth--;
    pop_visiting(ctx);
    
    return result;
}

// Deep comparison core function (recursive)
// Custom objects use operator== at OP_CMP_EQ level, not here
static bool deep_compare(CompareContext *ctx, XrValue a, XrValue b) {
    if (xr_value_same(a, b)) return true;
    
    if (XR_IS_INT(a) && XR_IS_INT(b)) return XR_TO_INT(a) == XR_TO_INT(b);
    if (XR_IS_FLOAT(a) && XR_IS_FLOAT(b)) return XR_TO_FLOAT(a) == XR_TO_FLOAT(b);
    if (XR_IS_BOOL(a) && XR_IS_BOOL(b)) return XR_TO_BOOL(a) == XR_TO_BOOL(b);
    if (XR_IS_NULL(a) && XR_IS_NULL(b)) return true;
    if (XR_IS_STRING(a) && XR_IS_STRING(b)) return xr_string_equal(XR_TO_STRING(a), XR_TO_STRING(b));
    if (XR_IS_ARRAY(a) && XR_IS_ARRAY(b)) return array_deep_equal(ctx, xr_value_to_array(a), xr_value_to_array(b));
    if (XR_IS_MAP(a) && XR_IS_MAP(b)) return map_deep_equal(ctx, xr_value_to_map(a), xr_value_to_map(b));
    if (XR_IS_SET(a) && XR_IS_SET(b)) return set_deep_equal(ctx, xr_value_to_set(a), xr_value_to_set(b));
    // Instance field-by-field comparison for value types (structs)
    if (xr_value_is_instance(a) && xr_value_is_instance(b)) {
        XrInstance *ia = xr_value_to_instance(a);
        XrInstance *ib = xr_value_to_instance(b);
        if (ia->klass != ib->klass) return false;
        int fc = ia->klass->field_count;
        for (int i = 0; i < fc; i++) {
            if (!deep_compare(ctx, ia->fields[i], ib->fields[i]))
                return false;
        }
        return true;
    }
    if (XR_IS_PTR(a) && XR_IS_PTR(b)) return XR_TO_PTR(a) == XR_TO_PTR(b);
    
    return false;
}

/* ========== Comparison Operation Helpers ========== */

// Deep equality comparison (with isolate, supports Array/Map/Set)
bool vm_values_equal_deep(XrayIsolate *isolate, XrValue a, XrValue b) {
    if (xr_value_same(a, b)) return true;
    
    if (XR_IS_INT(a) && XR_IS_INT(b)) return XR_TO_INT(a) == XR_TO_INT(b);
    if (XR_IS_FLOAT(a) && XR_IS_FLOAT(b)) return XR_TO_FLOAT(a) == XR_TO_FLOAT(b);
    if (XR_IS_BOOL(a) && XR_IS_BOOL(b)) return XR_TO_BOOL(a) == XR_TO_BOOL(b);
    if (XR_IS_NULL(a) && XR_IS_NULL(b)) return true;
    if (XR_IS_STRING(a) && XR_IS_STRING(b)) {
        uint32_t la = xr_value_str_len(&a), lb = xr_value_str_len(&b);
        if (la != lb) return false;
        return memcmp(xr_value_str_data(&a), xr_value_str_data(&b), la) == 0;
    }
    if (XR_IS_JSON(a) && XR_IS_JSON(b)) return xr_value_deep_eq(a, b);
    
    // BigInt equality
    if (XR_IS_BIGINT(a) && XR_IS_BIGINT(b)) {
        return xr_bigint_cmp((XrBigInt*)XR_TO_PTR(a), (XrBigInt*)XR_TO_PTR(b)) == 0;
    }
    // BigInt == int (mixed comparison)
    if (XR_IS_BIGINT(a) && XR_IS_INT(b)) {
        return xr_bigint_cmp_int((XrBigInt*)XR_TO_PTR(a), XR_TO_INT(b)) == 0;
    }
    if (XR_IS_INT(a) && XR_IS_BIGINT(b)) {
        return xr_bigint_cmp_int((XrBigInt*)XR_TO_PTR(b), XR_TO_INT(a)) == 0;
    }
    
    // Struct ref: field-by-field comparison via native layout
    if (XR_IS_STRUCT_REF(a) && XR_IS_STRUCT_REF(b)) {
        uint8_t *sa = (uint8_t*)xr_to_struct_ptr(a);
        uint8_t *sb = (uint8_t*)xr_to_struct_ptr(b);
        XrClass *ca = *(XrClass**)sa;
        XrClass *cb = *(XrClass**)sb;
        if (ca != cb) return false;
        XrStructLayout *layout = ca->struct_layout;
        if (!layout) return sa == sb;
        // Compare the entire native field data area
        return memcmp(sa + 8, sb + 8, layout->total_size) == 0;
    }
    
    if (XR_IS_ARRAY(a) || XR_IS_MAP(a) || XR_IS_SET(a) || xr_value_is_instance(a)) {
        if (isolate) {
            CompareContext ctx = { .isolate = isolate, .visiting_count = 0, .depth = 0 };
            return deep_compare(&ctx, a, b);
        }
    }
    
    if (XR_IS_PTR(a) && XR_IS_PTR(b)) return XR_TO_PTR(a) == XR_TO_PTR(b);
    return false;
}

// General equality comparison (for == operator)
// Uses pointer comparison for reference types (use vm_values_equal_deep for deep comparison)
bool vm_values_equal(XrValue a, XrValue b) {
    // NaN is never equal to itself (IEEE 754)
    if (XR_IS_FLOAT(a) && XR_IS_FLOAT(b)) return XR_TO_FLOAT(a) == XR_TO_FLOAT(b);
    if (xr_value_same(a, b)) return true;
    
    if (XR_IS_INT(a) && XR_IS_INT(b)) return XR_TO_INT(a) == XR_TO_INT(b);
    // Numeric cross-type: int == float, float == int
    if (XR_IS_INT(a) && XR_IS_FLOAT(b)) return (double)XR_TO_INT(a) == XR_TO_FLOAT(b);
    if (XR_IS_FLOAT(a) && XR_IS_INT(b)) return XR_TO_FLOAT(a) == (double)XR_TO_INT(b);
    if (XR_IS_BOOL(a) && XR_IS_BOOL(b)) return XR_TO_BOOL(a) == XR_TO_BOOL(b);
    if (XR_IS_NULL(a) && XR_IS_NULL(b)) return true;
    if (XR_IS_STRING(a) && XR_IS_STRING(b)) {
        // Direct content comparison — works for SSO, heap, or mixed
        uint32_t la = xr_value_str_len(&a);
        uint32_t lb = xr_value_str_len(&b);
        if (la != lb) return false;
        return memcmp(xr_value_str_data(&a), xr_value_str_data(&b), la) == 0;
    }
    
    // BigInt equality
    if (XR_IS_BIGINT(a) && XR_IS_BIGINT(b)) {
        return xr_bigint_cmp((XrBigInt*)XR_TO_PTR(a), (XrBigInt*)XR_TO_PTR(b)) == 0;
    }
    // BigInt == int (mixed comparison)
    if (XR_IS_BIGINT(a) && XR_IS_INT(b)) {
        return xr_bigint_cmp_int((XrBigInt*)XR_TO_PTR(a), XR_TO_INT(b)) == 0;
    }
    if (XR_IS_INT(a) && XR_IS_BIGINT(b)) {
        return xr_bigint_cmp_int((XrBigInt*)XR_TO_PTR(b), XR_TO_INT(a)) == 0;
    }
    
    if (XR_IS_PTR(a) && XR_IS_PTR(b)) return XR_TO_PTR(a) == XR_TO_PTR(b);
    
    return false;
}

// Numeric less than
bool vm_numeric_less(XrValue left, XrValue right) {
    // BigInt comparisons
    if (XR_IS_BIGINT(left) && XR_IS_BIGINT(right)) {
        return xr_bigint_cmp((XrBigInt*)XR_TO_PTR(left), (XrBigInt*)XR_TO_PTR(right)) < 0;
    }
    if (XR_IS_BIGINT(left) && XR_IS_INT(right)) {
        return xr_bigint_cmp_int((XrBigInt*)XR_TO_PTR(left), XR_TO_INT(right)) < 0;
    }
    if (XR_IS_INT(left) && XR_IS_BIGINT(right)) {
        return xr_bigint_cmp_int((XrBigInt*)XR_TO_PTR(right), XR_TO_INT(left)) > 0;
    }
    
    if ((XR_IS_INT(left) || XR_IS_FLOAT(left)) && (XR_IS_INT(right) || XR_IS_FLOAT(right))) {
        double nl = XR_IS_INT(left) ? (double)XR_TO_INT(left) : XR_TO_FLOAT(left);
        double nr = XR_IS_INT(right) ? (double)XR_TO_INT(right) : XR_TO_FLOAT(right);
        return nl < nr;
    }
    if (XR_IS_STRING(left) && XR_IS_STRING(right)) {
        return xr_string_compare(XR_TO_STRING(left), XR_TO_STRING(right)) < 0;
    }
    return false;
}

// Numeric less than or equal
bool vm_numeric_less_equal(XrValue left, XrValue right) {
    // BigInt comparisons
    if (XR_IS_BIGINT(left) && XR_IS_BIGINT(right)) {
        return xr_bigint_cmp((XrBigInt*)XR_TO_PTR(left), (XrBigInt*)XR_TO_PTR(right)) <= 0;
    }
    if (XR_IS_BIGINT(left) && XR_IS_INT(right)) {
        return xr_bigint_cmp_int((XrBigInt*)XR_TO_PTR(left), XR_TO_INT(right)) <= 0;
    }
    if (XR_IS_INT(left) && XR_IS_BIGINT(right)) {
        return xr_bigint_cmp_int((XrBigInt*)XR_TO_PTR(right), XR_TO_INT(left)) >= 0;
    }
    
    if ((XR_IS_INT(left) || XR_IS_FLOAT(left)) && (XR_IS_INT(right) || XR_IS_FLOAT(right))) {
        double nl = XR_IS_INT(left) ? (double)XR_TO_INT(left) : XR_TO_FLOAT(left);
        double nr = XR_IS_INT(right) ? (double)XR_TO_INT(right) : XR_TO_FLOAT(right);
        return nl <= nr;
    }
    if (XR_IS_STRING(left) && XR_IS_STRING(right)) {
        return xr_string_compare(XR_TO_STRING(left), XR_TO_STRING(right)) <= 0;
    }
    return false;
}

// Numeric greater than
bool vm_numeric_greater(XrValue left, XrValue right) {
    // BigInt comparisons
    if (XR_IS_BIGINT(left) && XR_IS_BIGINT(right)) {
        return xr_bigint_cmp((XrBigInt*)XR_TO_PTR(left), (XrBigInt*)XR_TO_PTR(right)) > 0;
    }
    if (XR_IS_BIGINT(left) && XR_IS_INT(right)) {
        return xr_bigint_cmp_int((XrBigInt*)XR_TO_PTR(left), XR_TO_INT(right)) > 0;
    }
    if (XR_IS_INT(left) && XR_IS_BIGINT(right)) {
        return xr_bigint_cmp_int((XrBigInt*)XR_TO_PTR(right), XR_TO_INT(left)) < 0;
    }
    
    if ((XR_IS_INT(left) || XR_IS_FLOAT(left)) && (XR_IS_INT(right) || XR_IS_FLOAT(right))) {
        double nl = XR_IS_INT(left) ? (double)XR_TO_INT(left) : XR_TO_FLOAT(left);
        double nr = XR_IS_INT(right) ? (double)XR_TO_INT(right) : XR_TO_FLOAT(right);
        return nl > nr;
    }
    if (XR_IS_STRING(left) && XR_IS_STRING(right)) {
        return xr_string_compare(XR_TO_STRING(left), XR_TO_STRING(right)) > 0;
    }
    return false;
}

// Numeric greater than or equal
bool vm_numeric_greater_equal(XrValue left, XrValue right) {
    // BigInt comparisons
    if (XR_IS_BIGINT(left) && XR_IS_BIGINT(right)) {
        return xr_bigint_cmp((XrBigInt*)XR_TO_PTR(left), (XrBigInt*)XR_TO_PTR(right)) >= 0;
    }
    if (XR_IS_BIGINT(left) && XR_IS_INT(right)) {
        return xr_bigint_cmp_int((XrBigInt*)XR_TO_PTR(left), XR_TO_INT(right)) >= 0;
    }
    if (XR_IS_INT(left) && XR_IS_BIGINT(right)) {
        return xr_bigint_cmp_int((XrBigInt*)XR_TO_PTR(right), XR_TO_INT(left)) <= 0;
    }
    
    if ((XR_IS_INT(left) || XR_IS_FLOAT(left)) && (XR_IS_INT(right) || XR_IS_FLOAT(right))) {
        double nl = XR_IS_INT(left) ? (double)XR_TO_INT(left) : XR_TO_FLOAT(left);
        double nr = XR_IS_INT(right) ? (double)XR_TO_INT(right) : XR_TO_FLOAT(right);
        return nl >= nr;
    }
    if (XR_IS_STRING(left) && XR_IS_STRING(right)) {
        return xr_string_compare(XR_TO_STRING(left), XR_TO_STRING(right)) >= 0;
    }
    return false;
}
