/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_format.c - Canonical XrValue -> XrString formatter
 *
 * KEY CONCEPT:
 *   Migrated from vm/xvm_ops.c. Serves as the single runtime formatter for
 *   print, toString and string concatenation. Recurses with bounded depth
 *   and element count to avoid cycle / blow-up issues on user structures.
 */

#include "xvalue_format.h"

#include "../../base/xchecks.h"
#include "../../base/xconstants.h"
#include "../../coro/xchannel.h"
#include "../../coro/xcoroutine.h"
#include "../../coro/xtask.h"
#include "../../module/xmodule.h"
#include "../class/xclass.h"
#include "../class/xenum.h"
#include "../class/xinstance.h"
#include "../closure/xbound_method.h"
#include "../closure/xclosure.h"
#include "../object/xarray.h"
#include "../object/xbigint.h"
#include "../object/xpanic_info.h"
#include "../object/xjson.h"
#include "../object/xmap.h"
#include "../object/xrange.h"
#include "../object/xset.h"
#include "../object/xtuple.h"
#include "../object/xstring.h"
#include "../object/xstringbuilder.h"
#include "../symbol/xsymbol_table.h"
#include "../xisolate_internal.h"
#include "../xstdlib_bridge.h"
#include "../../base/xutf8.h"
#include "../../shared/xr_float_fmt.h"
#include "xtype_names.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== Tunables ========== */

#define XR_FORMAT_MAX_DEPTH 3
#define XR_FORMAT_MAX_ELEMENTS 32

static bool class_is_named_datetime(XrClass *cls) {
    const char *name = cls ? xr_class_display_name(cls) : NULL;
    return name && strcmp(name, "DateTime") == 0;
}

/* The second way rendering re-enters the interpreter: this one calls back
 * synchronously rather than through a VM frame, so it never passes the print
 * dispatch's own toString continuation. It stays inside the print group's
 * atomicity anyway, because it only ever appends to the caller's buffer — a
 * failure here loses the buffer instead of publishing part of a line. Writing
 * the rendered text out from here would be the one way to break that. */
static bool format_datetime_method(XrVMRuntime *isolate, XrStrBuf *sb, XrObjectInstance *inst,
                                   const char *method) {
    if (!isolate || !sb || !inst || !method)
        return false;
    XrValue result = xr_instance_call_method(isolate, inst, method, NULL, 0);
    if (!XR_IS_STRING(result))
        return false;
    XrString *str = XR_TO_STRING(result);
    xr_strbuf_append_str(sb, str);
    return true;
}

/* ========== Container helpers (static) ========== */

static void format_array(XrVMRuntime *isolate, XrStrBuf *sb, XrArray *arr, int depth) {
    xr_strbuf_append_cstr(sb, "[", 1);
    int len = arr->length;
    int limit = (len > XR_FORMAT_MAX_ELEMENTS) ? XR_FORMAT_MAX_ELEMENTS : len;
    for (int i = 0; i < limit; i++) {
        if (i > 0)
            xr_strbuf_append_cstr(sb, ", ", 2);
        xr_value_to_strbuf(isolate, sb, xr_array_get_element(arr, i), depth + 1);
    }
    if (len > limit) {
        char more[32];
        int n = snprintf(more, sizeof(more), ", ...(%d more)", len - limit);
        xr_strbuf_append_cstr(sb, more, (size_t) n);
    }
    xr_strbuf_append_cstr(sb, "]", 1);
}

static void format_span(XrVMRuntime *isolate, XrStrBuf *sb, XrValue value, int depth) {
    XrSliceView *span = XR_TO_SLICE_REF(value);
    xr_strbuf_append_cstr(sb, "[", 1);
    int64_t len = span ? span->length : 0;
    int64_t limit = (len > XR_FORMAT_MAX_ELEMENTS) ? XR_FORMAT_MAX_ELEMENTS : len;
    for (int64_t i = 0; i < limit; i++) {
        if (i > 0)
            xr_strbuf_append_cstr(sb, ", ", 2);
        xr_value_to_strbuf(isolate, sb,
                           xr_typed_get(span->data, (int32_t) i, XR_SLICE_REF_ELEM_TYPE(value)),
                           depth + 1);
    }
    if (len > limit) {
        char more[32];
        int n = snprintf(more, sizeof(more), ", ...(%" PRId64 " more)", len - limit);
        xr_strbuf_append_cstr(sb, more, (size_t) n);
    }
    xr_strbuf_append_cstr(sb, "]", 1);
}

static void format_tuple(XrVMRuntime *isolate, XrStrBuf *sb, XrTuple *tup, int depth) {
    xr_strbuf_append_cstr(sb, "(", 1);
    uint16_t n = xr_tuple_arity(tup);
    uint16_t limit = (n > XR_FORMAT_MAX_ELEMENTS) ? XR_FORMAT_MAX_ELEMENTS : n;
    for (uint16_t i = 0; i < limit; i++) {
        if (i > 0)
            xr_strbuf_append_cstr(sb, ", ", 2);
        xr_value_to_strbuf(isolate, sb, tup->elements[i], depth + 1);
    }
    if (n > limit) {
        char more[32];
        int m = snprintf(more, sizeof(more), ", ...(%u more)", (unsigned) (n - limit));
        xr_strbuf_append_cstr(sb, more, (size_t) m);
    }
    if (n == 1)
        xr_strbuf_append_cstr(sb, ",", 1);
    xr_strbuf_append_cstr(sb, ")", 1);
}

static void format_map(XrVMRuntime *isolate, XrStrBuf *sb, XrMap *map, int depth) {
    xr_strbuf_append_cstr(sb, "#{", 2);
    if (!xr_map_isdummy(map)) {
        uint32_t size = map->nentries;
        int count = 0;
        for (uint32_t i = 0; i < size && count < XR_FORMAT_MAX_ELEMENTS; i++) {
            XrMapEntry *node = xr_map_entry(map, i);
            if (!XR_MAP_ENTRY_EMPTY(node)) {
                if (count > 0)
                    xr_strbuf_append_cstr(sb, ", ", 2);
                xr_value_to_strbuf(isolate, sb, node->key, depth + 1);
                xr_strbuf_append_cstr(sb, ": ", 2);
                xr_value_to_strbuf(isolate, sb, node->value, depth + 1);
                count++;
            }
        }
        if ((uint32_t) count < map->count) {
            char more[32];
            int n = snprintf(more, sizeof(more), ", ...(%u more)", map->count - (uint32_t) count);
            xr_strbuf_append_cstr(sb, more, (size_t) n);
        }
    }
    xr_strbuf_append_cstr(sb, "}", 1);
}

static void format_set(XrVMRuntime *isolate, XrStrBuf *sb, XrSet *set, int depth) {
    xr_strbuf_append_cstr(sb, "#[", 2);
    int count = 0;
    for (uint32_t i = 0; i < set->nentries && count < XR_FORMAT_MAX_ELEMENTS; i++) {
        XrSetEntry *entry = &set->entries[i];
        if (!XR_SET_ENTRY_EMPTY(entry)) {
            if (count > 0)
                xr_strbuf_append_cstr(sb, ", ", 2);
            xr_value_to_strbuf(isolate, sb, entry->value, depth + 1);
            count++;
        }
    }
    if ((uint32_t) count < set->count) {
        char more[32];
        int n = snprintf(more, sizeof(more), ", ...(%u more)", set->count - (uint32_t) count);
        xr_strbuf_append_cstr(sb, more, (size_t) n);
    }
    xr_strbuf_append_cstr(sb, "]", 1);
}

static void format_json(XrVMRuntime *isolate, XrStrBuf *sb, XrObjectInstance *json, int depth) {
    xr_strbuf_append_cstr(sb, "{", 1);
    XrClass *cls = json->klass;
    if (!cls) {
        xr_strbuf_append_cstr(sb, "}", 1);
        return;
    }
    for (int i = 0; i < cls->field_count && i < XR_FORMAT_MAX_ELEMENTS; i++) {
        if (i > 0)
            xr_strbuf_append_cstr(sb, ", ", 2);
        const char *fname = cls->fields[i].name;
        if (fname)
            xr_strbuf_append_cstr(sb, fname, strlen(fname));
        else
            xr_strbuf_append_cstr(sb, "?", 1);
        xr_strbuf_append_cstr(sb, ": ", 2);
        xr_value_to_strbuf(isolate, sb, xr_instance_get_dynamic_field(json, (uint16_t) i),
                           depth + 1);
    }
    xr_strbuf_append_cstr(sb, "}", 1);
}

static void format_inspect_instance(XrVMRuntime *isolate, XrStrBuf *sb, XrObjectInstance *inst,
                                    XrClass *cls, int depth) {
    const char *class_name = xr_class_display_name(cls);
    xr_strbuf_append_cstr(sb, class_name, strlen(class_name));
    xr_strbuf_append_cstr(sb, "{", 1);

    int emitted = 0;
    for (uint16_t i = 0; cls && i < cls->field_count && emitted < XR_FORMAT_MAX_ELEMENTS; i++) {
        if (cls->fields[i].flags & XR_FIELD_STATIC)
            continue;
        if (emitted > 0)
            xr_strbuf_append_cstr(sb, ", ", 2);
        const char *name = cls->fields[i].name;
        if (name)
            xr_strbuf_append_cstr(sb, name, strlen(name));
        else {
            char buf[16];
            int n = snprintf(buf, sizeof(buf), "field%u", (unsigned) i);
            xr_strbuf_append_cstr(sb, buf, (size_t) n);
        }
        xr_strbuf_append_cstr(sb, ": ", 2);
        xr_value_to_strbuf(isolate, sb, xr_instance_load_field(isolate, inst, i), depth + 1);
        emitted++;
    }
    if (cls && cls->field_count > XR_FORMAT_MAX_ELEMENTS)
        xr_strbuf_append_cstr(sb, ", ...", 5);
    xr_strbuf_append_cstr(sb, "}", 1);
}

/* ========== Public API ========== */

void xr_value_to_strbuf(XrVMRuntime *isolate, XrStrBuf *sb, XrValue val, int depth) {
    if (XR_IS_STRING(val)) {
        if (depth > 0)
            xr_strbuf_append_cstr(sb, "\"", 1);
        xr_strbuf_append_cstr(sb, xr_value_str_data(&val), xr_value_str_len(&val));
        if (depth > 0)
            xr_strbuf_append_cstr(sb, "\"", 1);
        return;
    }
    if (XR_IS_INT(val)) {
        xr_strbuf_append_int(sb, XR_TO_INT(val));
        return;
    }
    if (XR_IS_FLOAT(val)) {
        char buf[64];
        int n = xr_format_float(buf, sizeof(buf), XR_TO_FLOAT(val));
        xr_strbuf_append_cstr(sb, buf, (size_t) n);
        return;
    }
    if (XR_IS_BOOL(val)) {
        if (XR_TO_BOOL(val))
            xr_strbuf_append_cstr(sb, "true", 4);
        else
            xr_strbuf_append_cstr(sb, "false", 5);
        return;
    }
    if (XR_IS_RUNE(val)) {
        /* In nested/dump context wrap in single quotes so a char is visibly
         * distinct from a one-character string ("a" vs 'a'). */
        char buf[XR_UTF8_MAX_BYTES];
        int n = xr_utf8_encode(XR_TO_RUNE(val), buf);
        if (depth > 0)
            xr_strbuf_append_cstr(sb, "'", 1);
        xr_strbuf_append_cstr(sb, buf, (size_t) (n > 0 ? n : 0));
        if (depth > 0)
            xr_strbuf_append_cstr(sb, "'", 1);
        return;
    }
    if (XR_IS_NULL(val)) {
        xr_strbuf_append_cstr(sb, "null", 4);
        return;
    }
    if (XR_IS_SLICE_REF(val)) {
        format_span(isolate, sb, val, depth);
        return;
    }

    if (!XR_IS_PTR(val)) {
        xr_strbuf_append_cstr(sb, "<unknown>", 9);
        return;
    }

    // Depth guard for recursive types
    if (depth > XR_FORMAT_MAX_DEPTH) {
        xr_strbuf_append_cstr(sb, "...", 3);
        return;
    }

    XrObjHeader *gc = (XrObjHeader *) XR_TO_PTR(val);
    XrObjType type = XR_OBJ_GET_TYPE(gc);

    switch (type) {
        case XR_TARRAY:
            format_array(isolate, sb, (XrArray *) gc, depth);
            break;
        case XR_TMAP:
            format_map(isolate, sb, (XrMap *) gc, depth);
            break;
        case XR_TSET:
            format_set(isolate, sb, (XrSet *) gc, depth);
            break;
        case XR_TENUM_CTOR: {
            XrEnumCtor *ev = (XrEnumCtor *) gc;
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
            XrEnumType *et = (XrEnumType *) gc;
            xr_strbuf_append_cstr(sb, "enum ", 5);
            if (et->name)
                xr_strbuf_append_cstr(sb, et->name, strlen(et->name));
            break;
        }
        case XR_TINSTANCE: {
            XrObjectInstance *inst = xr_value_to_instance(val);
            XrClass *cls = xr_instance_get_class(inst);
            /* BigInt: decimal string representation */
            if (cls && cls->builtin_kind == XR_BK_BIGINT) {
                char *s = xr_bigint_to_string((XrBigInt *) gc);
                if (s) {
                    xr_strbuf_append_cstr(sb, s, strlen(s));
                    xr_free(s);
                } else {
                    xr_strbuf_append_cstr(sb, "<BigInt>", 8);
                }
                break;
            }
            /* Structural object: recursive key-value format. */
            if (cls && cls->builtin_kind == XR_BK_STRUCT_OBJECT) {
                format_json(isolate, sb, (XrObjectInstance *) gc, depth);
                break;
            }
            /* Tuple: parenthesised form, matches the literal syntax. */
            if (cls && cls->builtin_kind == XR_BK_TUPLE) {
                format_tuple(isolate, sb, (XrTuple *) inst, depth);
                break;
            }
            /* Exception: keep the legacy "Error: <message>" form so
             * `string(e)` and string concatenation behave identically
             * to the pre-unified-class implementation. */
            if (xr_value_is_panic_info(isolate, val)) {
                const char *msg = xr_panic_info_get_message(isolate, val);
                if (msg) {
                    xr_strbuf_append_cstr(sb, "Error: ", 7);
                    xr_strbuf_append_cstr(sb, msg, strlen(msg));
                } else {
                    xr_strbuf_append_cstr(sb, "<exception>", 11);
                }
                break;
            }
            /* Range prints in the same compact notation used by source. */
            if (xr_value_is_range(isolate, val)) {
                XrRange *rng = (XrRange *) xr_instance_native_body(inst);
                char buf[80];
                int n = xr_range_core_format_buf(xr_range_core_view(rng), buf, sizeof(buf));
                xr_strbuf_append_cstr(sb, buf, (size_t) n);
            } else if (class_is_named_datetime(cls)) {
                if (!format_datetime_method(isolate, sb, inst, "toString"))
                    xr_strbuf_append_cstr(sb, "DateTime{...}", 13);
            } else if (cls && cls->builtin_kind == XR_BK_REGEX) {
                /* Regex keeps its pattern in field slot 0. The engine moved to
                 * stdlib/regex/regex.xr, so the handle no longer has a native
                 * body holding an XrRegex*. */
                XrValue pat_val = xr_instance_get_field_fast(inst, 0);
                if (XR_IS_STRING(pat_val)) {
                    XrString *pat = XR_TO_STRING(pat_val);
                    xr_strbuf_append_cstr(sb, "/", 1);
                    xr_strbuf_append_cstr(sb, pat->data, pat->length);
                    xr_strbuf_append_cstr(sb, "/", 1);
                } else {
                    xr_strbuf_append_cstr(sb, "<Regex>", 7);
                }
            } else if (cls && cls->builtin_kind == XR_BK_ITERATOR) {
                xr_strbuf_append_cstr(sb, "<iterator>", 10);
            } else if (cls && cls->builtin_kind == XR_BK_STRINGBUILDER) {
                XrStringBuilder *sbuilder = (XrStringBuilder *) gc;
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
            } else if (cls && cls->builtin_kind == XR_BK_ADT_ENUM) {
                /* ADT enum aggregate: format as "EnumName.Variant(p1, p2, ...)". */
                XrEnumAggregateValue *agg = xr_value_to_enum_aggregate(val);
                XrEnumType *etype = xr_enum_aggregate_type(agg);
                if (agg && etype) {
                    uint32_t tag_index = agg->member_index;
                    const char *member_name = xr_enum_type_member_name(etype, tag_index);
                    if (!member_name) {
                        xr_strbuf_append_cstr(sb, cls->name ? cls->name : "<adt>",
                                              cls->name ? strlen(cls->name) : 5);
                        xr_strbuf_append_cstr(sb, "{...}", 5);
                    } else {
                        const char *enum_name = etype->name;
                        if (enum_name)
                            xr_strbuf_append_cstr(sb, enum_name, strlen(enum_name));
                        xr_strbuf_append_cstr(sb, ".", 1);
                        if (member_name)
                            xr_strbuf_append_cstr(sb, member_name, strlen(member_name));
                        uint32_t pc = agg->payload_count;
                        if (pc > 0) {
                            xr_strbuf_append_cstr(sb, "(", 1);
                            for (uint32_t fi = 0; fi < pc; fi++) {
                                if (fi > 0)
                                    xr_strbuf_append_cstr(sb, ", ", 2);
                                xr_value_to_strbuf(isolate, sb, agg->payloads[fi], depth + 1);
                            }
                            xr_strbuf_append_cstr(sb, ")", 1);
                        }
                    }
                } else {
                    xr_strbuf_append_cstr(sb, cls->name ? cls->name : "<adt>",
                                          cls->name ? strlen(cls->name) : 5);
                    xr_strbuf_append_cstr(sb, "{...}", 5);
                }
            } else if (cls && (cls->flags & XR_CLASS_DERIVE_INSPECT)) {
                format_inspect_instance(isolate, sb, inst, cls, depth);
            } else if (cls && cls->name) {
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
            if (cls && cls->name)
                xr_strbuf_append_cstr(sb, cls->name, strlen(cls->name));
            break;
        }
        case XR_TCOROUTINE: {
            XrCoroutine *coro = (XrCoroutine *) gc;
            xr_strbuf_append_cstr(sb, "Coroutine(", 10);
            const char *name = xr_coro_name(coro);
            if (name)
                xr_strbuf_append_cstr(sb, name, strlen(name));
            else
                xr_strbuf_append_cstr(sb, "anonymous", 9);
            xr_strbuf_append_cstr(sb, ")", 1);
            break;
        }
        case XR_TTASK: {
            XrTask *task = (XrTask *) gc;
            static const char *const task_state_names[] = {
                "active", "completing", "cancelling", "completed", "failed", "cancelled",
            };
            uint8_t st = atomic_load_explicit(&task->state, memory_order_relaxed);
            const char *sn = (st < 6) ? task_state_names[st] : "unknown";
            xr_strbuf_append_cstr(sb, "Task(", 5);
            xr_strbuf_append_cstr(sb, sn, strlen(sn));
            xr_strbuf_append_cstr(sb, ")", 1);
            break;
        }
        case XR_TCHANNEL: {
            XrChannel *ch = (XrChannel *) gc;
            char buf[64];
            int n = snprintf(buf, sizeof(buf), "Channel(cap=%u, count=%u)", ch->buf_size,
                             ch->buf_count);
            xr_strbuf_append_cstr(sb, buf, (size_t) n);
            break;
        }
        /* A function value renders without its source name. The name is debug
         * metadata: AOT's callable descriptor does not carry one, and gating it
         * on the type-name profile would make a program's output depend on how
         * it was built. One name-free spelling keeps VM and AOT in step on
         * every profile. */
        case XR_TFUNCTION:
            xr_strbuf_append_cstr(sb, "<fn>", 4);
            break;
        case XR_TCFUNCTION:
            xr_strbuf_append_cstr(sb, "<native fn>", 11);
            break;
        case XR_TMODULE: {
            XrModule *mod = (XrModule *) gc;
            xr_strbuf_append_cstr(sb, "module ", 7);
            if (mod->name)
                xr_strbuf_append_cstr(sb, mod->name, strlen(mod->name));
            break;
        }
        case XR_TERROR:
            xr_strbuf_append_cstr(sb, "<error>", 7);
            break;
        case XR_TCOROPOOL:
            xr_strbuf_append_cstr(sb, "<CoroPool>", 10);
            break;
        case XR_TBOUND_METHOD: {
            XrBoundMethod *bm = (XrBoundMethod *) gc;
            xr_strbuf_append_cstr(sb, "<bound_method", 13);
            if (xr_value_is_instance(bm->receiver)) {
                XrObjectInstance *inst = xr_value_to_instance(bm->receiver);
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
            xr_strbuf_append_cstr(sb, buf, (size_t) n);
            break;
        }
    }
}

XrString *xr_value_to_string(XrVMRuntime *isolate, XrValue val) {
    XR_DCHECK(isolate != NULL, "xr_value_to_string: NULL isolate");

    // Fast paths for common types (no XrStrBuf needed)
    if (XR_IS_STRING(val)) {
        return (XrString *) val.ptr;
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
    if (XR_IS_RUNE(val)) {
        char buf[XR_UTF8_MAX_BYTES];
        int n = xr_utf8_encode(XR_TO_RUNE(val), buf);
        return xr_string_intern(isolate, buf, (size_t) (n > 0 ? n : 0), 0);
    }
    if (XR_IS_NULL(val)) {
        return xr_string_intern(isolate, "null", 4, 0);
    }

    // General path: dedicated XrStrBuf (avoids nesting issues when callers
    // already hold xr_strbuf_tmp, e.g. OP_ADD).
    XrStrBuf *sb = xr_strbuf_new(isolate, 128);
    xr_value_to_strbuf(isolate, sb, val, 0);
    XrString *result = xr_strbuf_to_string(sb);
    xr_strbuf_free(sb);
    return result;
}
