/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstring_methods.c - String instance method bodies + dispatch table.
 *
 * The bodies here are thin adapters over xr_string_*() in xstring.c.
 * Each adapter:
 *   - validates the receiver via XR_DCHECK,
 *   - implements only the canonical string surface,
 *   - returns XrValue with explicit null/bool/int/string boxing.
 *
 */

#include "xstring_methods.h"
#include "xstring.h"
#include "xarray.h"
#include "xarray_vm.h"
#include "xiterator.h"
#include "xpanic_info.h"
#include "../value/xvalue.h"
#include "../value/xvalue_format.h"
#include "../symbol/xsymbol_table.h"
#include "../../coro/xcoroutine.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include "../../base/xutf8.h"
#include "../../vm/xvm.h"
#include "../xerror_codes.h"
#include <string.h>

static inline XrString *str_self(XrValue self) {
    XR_DCHECK(XR_IS_STRING(self), "string method: receiver is not a string");
    return XR_TO_STRING(self);
}

static XrValue m_slice(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    XrString *str = str_self(self);
    if (argc < 1)
        return xr_string_value(str);
    xr_Integer start = XR_TO_INT(args[0]);
    xr_Integer count = (xr_Integer) xr_string_rune_length(str);
    xr_Integer end = (argc >= 2) ? XR_TO_INT(args[1]) : count;
    if (start < 0 || end < start || end > count) {
        XrValue exc = xr_panic_info_newf(iso, XR_ERR_INDEX_OUT_OF_BOUNDS,
                                         "string.slice rune range out of bounds");
        xr_vm_throw_exception(iso, exc);
        return xr_null();
    }
    XrString *result = xr_string_slice(iso, str, start, end);
    return result ? xr_string_value(result) : xr_null();
}

static XrValue m_slice_bytes(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    XrString *str = str_self(self);
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1])) {
        XrValue exc = xr_panic_info_newf(iso, XR_ERR_TYPE_MISMATCH,
                                         "string.sliceBytes expects start and end byte offsets");
        xr_vm_throw_exception(iso, exc);
        return xr_null();
    }
    xr_Integer start = XR_TO_INT(args[0]);
    xr_Integer end = XR_TO_INT(args[1]);
    XrString *result = xr_string_slice_bytes(iso, str, start, end);
    if (!result) {
        XrValue exc = xr_panic_info_newf(
            iso, XR_ERR_INDEX_OUT_OF_BOUNDS,
            "string.sliceBytes byte range out of bounds or not on UTF-8 scalar boundaries");
        xr_vm_throw_exception(iso, exc);
        return xr_null();
    }
    return xr_string_value(result);
}

/* === Search === */

static XrValue m_index_of(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_int(-1);
    XrString *str = str_self(self);
    XrString *substr = xr_value_to_string(iso, args[0]);
    xr_Integer start = (argc >= 2 && XR_IS_INT(args[1])) ? XR_TO_INT(args[1]) : 0;
    return xr_int(xr_string_index_of_from(iso, str, substr, start));
}

static XrValue m_last_index_of(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    XrString *str = str_self(self);
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_int(-1);
    XrString *substr = xr_value_to_string(iso, args[0]);
    return xr_int(xr_string_last_index_of(iso, str, substr));
}

static XrValue m_includes(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_bool(0);
    XrString *str = str_self(self);
    XrString *substr = xr_value_to_string(iso, args[0]);
    return xr_bool(xr_string_has(iso, str, substr));
}

static XrValue m_starts_with(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_bool(0);
    XrString *str = str_self(self);
    XrString *prefix = xr_value_to_string(iso, args[0]);
    return xr_bool(xr_string_starts_with(iso, str, prefix));
}

static XrValue m_ends_with(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_bool(0);
    XrString *str = str_self(self);
    XrString *suffix = xr_value_to_string(iso, args[0]);
    return xr_bool(xr_string_ends_with(iso, str, suffix));
}

/* === Replacement / construction === */

static XrValue m_split(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    XrString *str = str_self(self);
    if (argc < 1) {
        XrArray *arr = xr_array_new(NULL);
        xr_array_push(arr, xr_string_value(str));
        return xr_value_from_array(arr);
    }
    if (!XR_IS_STRING(args[0]))
        return xr_null();
    XrString *delim = xr_value_to_string(iso, args[0]);
    XrArray *result = xr_string_split(iso, str, delim);
    return result ? xr_value_from_array(result) : xr_null();
}

static XrValue m_replace(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    XrString *str = str_self(self);
    if (argc < 2 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1])) {
        return xr_string_value(str);
    }
    XrString *old_str = xr_value_to_string(iso, args[0]);
    XrString *new_str = xr_value_to_string(iso, args[1]);
    XrString *result = xr_string_replace(iso, str, old_str, new_str);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue m_replace_all(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    XrString *str = str_self(self);
    if (argc < 2 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1])) {
        return xr_string_value(str);
    }
    XrString *old_str = xr_value_to_string(iso, args[0]);
    XrString *new_str = xr_value_to_string(iso, args[1]);
    XrString *result = xr_string_replace_all(iso, str, old_str, new_str);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue m_repeat(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    XrString *str = str_self(self);
    if (argc < 1)
        return xr_string_value(str);
    xr_Integer count = XR_TO_INT(args[0]);
    if (count <= 0)
        return xr_string_value(xr_string_intern(iso, "", 0, 0));
    XrString *result = xr_string_repeat(iso, str, count);
    return result ? xr_string_value(result) : xr_null();
}

/* str.copyBytes() -> Array<byte>. The receiver remains immutable; the result
 * owns its own backing storage. Decode explicitly with string.fromUtf8(). */
static XrValue m_to_bytes(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    XrString *str = str_self(self);
    int32_t byte_len = (int32_t) (str ? str->length : 0);
    XrCoroutine *coro = NULL;
    XrArray *bytes = xr_array_bytes_new(coro, byte_len);
    if (!bytes)
        return xr_null();
    if (byte_len > 0)
        xr_array_append_data(bytes, (const uint8_t *) str->data, byte_len);
    return xr_value_from_array(bytes);
}

/* === toString === */

static XrValue m_to_string(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_string_value(str_self(self));
}

static bool string_materialize_bytes(const void *storage, int64_t length, uint8_t elem_type,
                                     const uint8_t **data, size_t *len, uint8_t **owned) {
    if (length < 0)
        return false;
    *len = (size_t) length;
    if (elem_type == XR_ELEM_U8) {
        *data = (const uint8_t *) storage;
        return true;
    }
    if (elem_type != XR_ELEM_ANY)
        return false;
    uint8_t *copy = length > 0 ? (uint8_t *) xr_malloc((size_t) length) : NULL;
    if (length > 0 && !copy)
        return false;
    for (int64_t i = 0; i < length; i++) {
        XrValue value = xr_typed_get((void *) storage, (int32_t) i, elem_type);
        if (!XR_IS_INT(value) || XR_TO_INT(value) < 0 || XR_TO_INT(value) > 255) {
            xr_free(copy);
            return false;
        }
        copy[i] = (uint8_t) XR_TO_INT(value);
    }
    *data = copy;
    *owned = copy;
    return true;
}

static bool string_bytes_arg(XrValue *args, int argc, const uint8_t **data, size_t *len,
                             uint8_t **owned) {
    if (argc != 1)
        return false;
    *owned = NULL;
    if (XR_IS_SPAN_REF(args[0])) {
        XrSpanView *span = XR_TO_SPAN_REF(args[0]);
        if (!span)
            return false;
        return string_materialize_bytes(span->data, span->length, span->elem_type, data, len,
                                        owned);
    }
    if (!XR_IS_ARRAY(args[0]))
        return false;
    XrArray *bytes = XR_TO_ARRAY(args[0]);
    if (!bytes)
        return false;
    return string_materialize_bytes(bytes->data, bytes->length, bytes->elem_type, data, len, owned);
}

static XrValue m_from_utf8(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    (void) self;
    const uint8_t *data = NULL;
    size_t len = 0;
    uint8_t *owned = NULL;
    if (!string_bytes_arg(args, argc, &data, &len, &owned))
        return xr_null();
    if (!xr_utf8_validate((const char *) data, len)) {
        xr_free(owned);
        return xr_null();
    }
    XrString *result = xr_string_new(iso, (const char *) data, len);
    xr_free(owned);
    return result ? xr_string_value(result) : xr_null();
}

static XrValue m_from_utf8_lossy(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    (void) self;
    const uint8_t *data = NULL;
    size_t len = 0;
    uint8_t *owned = NULL;
    if (!string_bytes_arg(args, argc, &data, &len, &owned))
        return xr_string_value(xr_string_new(iso, "", 0));
    if (xr_utf8_validate((const char *) data, len)) {
        XrString *result = xr_string_new(iso, (const char *) data, len);
        xr_free(owned);
        return result ? xr_string_value(result) : xr_null();
    }

    if (len > (SIZE_MAX - 1) / 3) {
        xr_free(owned);
        return xr_null();
    }
    char *buf = (char *) xr_malloc(len * 3 + 1);
    if (!buf) {
        xr_free(owned);
        return xr_null();
    }
    size_t src = 0, dst = 0;
    while (src < len) {
        uint32_t cp = 0;
        int consumed = xr_utf8_decode((const char *) data + src, len - src, &cp);
        if (consumed <= 0 || !xr_unicode_is_scalar(cp)) {
            cp = XR_UNICODE_INVALID;
            consumed = 1;
        }
        char encoded[XR_UTF8_MAX_BYTES];
        int written = xr_utf8_encode(cp, encoded);
        memcpy(buf + dst, encoded, (size_t) written);
        dst += (size_t) written;
        src += (size_t) consumed;
    }
    XrString *result = xr_string_new(iso, buf, dst);
    xr_free(buf);
    xr_free(owned);
    return result ? xr_string_value(result) : xr_null();
}

static XrValue m_from_rune(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    (void) self;
    if (argc != 1 || !XR_IS_RUNE(args[0]))
        return xr_null();
    XrString *result = xr_string_from_codepoint(iso, XR_TO_RUNE(args[0]));
    return result ? xr_string_value(result) : xr_null();
}

static XrValue m_join_static(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    (void) self;
    if (argc < 1 || !XR_IS_ARRAY(args[0]))
        return xr_null();
    XrString *separator = NULL;
    if (argc >= 2) {
        if (!XR_IS_STRING(args[1]))
            return xr_null();
        separator = XR_TO_STRING(args[1]);
    } else {
        separator = xr_string_intern(iso, "", 0, 0);
    }
    XrString *result = xr_array_join(iso, XR_TO_ARRAY(args[0]), separator);
    return result ? xr_string_value(result) : xr_null();
}

/* === Iteration === */

/* Rune iterator: yields each Unicode scalar as a rune value. */
static XrValue m_iterator(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrString *s = str_self(self);
    XrIterator *iter = xr_iterator_new_from_string(NULL, s, iso);
    if (iter)
        iter->mode = XR_ITER_MODE_VALUES;
    return iter ? xr_value_from_iterator(iter) : xr_null();
}

/* Lazy entries iterator used by `for (i, c in s)` lowering.
 * Yields (index, char) tuples by UTF-8 character index. */
static XrValue m_entries_iterator(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrString *s = str_self(self);
    XrIterator *iter = xr_iterator_new_from_string(NULL, s, iso);
    return iter ? xr_value_from_iterator(iter) : xr_null();
}

/* ========== XrClass Registration ========== */

#include "xnative_type.h"

void xr_string_register_native_type(XrVMRuntime *isolate) {
    static const XrNativeMethod string_methods[] = {
        /* Indexing / extraction */
        {"slice", m_slice, 0},
        {"sliceBytes", m_slice_bytes, 2},
        /* Search */
        {"indexOf", m_index_of, 0},
        {"lastIndexOf", m_last_index_of, 1},
        {"contains", m_includes, 1},
        {"startsWith", m_starts_with, 1},
        {"endsWith", m_ends_with, 1},
        /* Case / whitespace */
        /* Replacement / construction */
        {"split", m_split, 0},
        {"replace", m_replace, 2},
        {"replaceAll", m_replace_all, 2},
        {"repeat", m_repeat, 1},
        /* Reverse / translate */
        /* Byte-array interop */
        {"copyBytes", m_to_bytes, 0},
        {"toString", m_to_string, 0},
        /* Iteration */
        {"iterator", m_iterator, 0},
        {"runes", m_iterator, 0},
        {"entriesIterator", m_entries_iterator, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeMethod string_statics[] = {
        {"fromUtf8", m_from_utf8, 1},
        {"fromUtf8Lossy", m_from_utf8_lossy, 1},
        {"fromRune", m_from_rune, 1},
        {"join", m_join_static, 1},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo string_info = {
        .name = "String",
        .gc_type = XR_TSTRING,
        .methods = string_methods,
        .getters = NULL,
        .static_methods = (XrNativeMethod *) string_statics,
    };
    xr_register_native_type(isolate, &string_info);
}
