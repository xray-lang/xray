/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_method.h - AOT-side method dispatch, property access, toString.
 *
 * This header is included verbatim into generated C and stays independent
 * from the VM/runtime value headers. XRT_SYM_* values must match the VM
 * symbol table; xrt_symbol_check.c validates the shared numeric namespace.
 */

#ifndef XRT_METHOD_H
#define XRT_METHOD_H

#include "xrt_value.h"
#include "xrt_arc.h"   // xrt_str_concat, xrt_str_alloc
#include "xrt_coll.h"  // xrt_array_t, xrt_map_t, xrt_strbuf_finish, xrt_array_push
#include "xrt_mem.h"
#include "xrt_array_hof.h"
#include "xrt_range.h"
#include "xrt_arith.h"  // xrt_value_to_string for container/tuple toString
#include "../shared/xr_int_arith_core.h"
#include "../shared/xr_arith_core.h"  // int.addOverflows/... (task 153)
#include "../shared/xr_range_core.h"
#include "../shared/xr_string_core.h"

/* Builtin method symbol IDs. */
#include "xrt_method_symbols.h"

#include "xrt_range_methods.inc.c"
#include "xrt_sort.inc.c"

extern XR_THREAD_LOCAL XrValue xrt_pending_error;

/* Defined by the exception layer (xrt_exception.h hosted, xrt_core_freestanding.h
 * freestanding); xrt.h orders that header after this one. */
static inline int xrt_has_pending_error(void);

static inline void xrt_set_builtin_enum_error(const char *enum_name, const char *member_name,
                                              uint32_t member_index) {
    XrAotEnumAggregate err =
        xrt_enum_aggregate_make(0, (int64_t) member_index, 0, enum_name, member_name, NULL);
    xrt_pending_error = xrt_enum_aggregate_box(err);
}

/* toString helper. */

static inline int xrt_rune_encode(uint32_t cp, char *tmp) {
    if (cp <= 0x7Fu) {
        tmp[0] = (char) cp;
        return 1;
    }
    if (cp <= 0x7FFu) {
        tmp[0] = (char) (0xC0u | (cp >> 6));
        tmp[1] = (char) (0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp <= 0xFFFFu) {
        if (cp >= 0xD800u && cp <= 0xDFFFu)
            return 0;
        tmp[0] = (char) (0xE0u | (cp >> 12));
        tmp[1] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
        tmp[2] = (char) (0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        tmp[0] = (char) (0xF0u | (cp >> 18));
        tmp[1] = (char) (0x80u | ((cp >> 12) & 0x3Fu));
        tmp[2] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
        tmp[3] = (char) (0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;
}

static inline XrValue xrt_rune_to_string(uint32_t cp) {
    char tmp[4];
    int n = xrt_rune_encode(cp, tmp);
    XrValue out = xrt_str_alloc((size_t) (n > 0 ? n : 0));
    if (n > 0)
        memcpy(xr_str_buf(out), tmp, (size_t) n);
    xr_str_buf(out)[n > 0 ? n : 0] = 0;
    return out;
}

static inline int xrt_rune_is_letter(uint32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

static inline int xrt_rune_is_number(uint32_t cp) {
    return cp >= '0' && cp <= '9';
}

static inline int xrt_rune_is_alnum(uint32_t cp) {
    return xrt_rune_is_letter(cp) || xrt_rune_is_number(cp);
}

static inline int xrt_rune_is_whitespace(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v';
}

static XrValue xrt_tostring(XrValue val, int slot_hint) {
    if (slot_hint == 3)
        return xrt_uint64_to_string((uint64_t) val.i);
    if (slot_hint == 1 || val.tag == XR_TAG_I64) {
        char tmp[32];
        int n = 0;
        int64_t v = val.i;
        uint64_t t = xr_i64_abs_magnitude(v);
        if (v < 0) {
            tmp[n++] = '-';
        }
        if (t == 0) {
            tmp[n++] = '0';
        } else {
            char rev[20];
            int r = 0;
            while (t > 0) {
                rev[r++] = '0' + (char) (t % 10u);
                t /= 10;
            }
            while (r > 0)
                tmp[n++] = rev[--r];
        }
        tmp[n] = 0;
        return xrt_str_from_cstr(tmp);
    }
    if (slot_hint == 2 || val.tag == XR_TAG_F64) {
        char tmp[64];
        xrt_format_float(tmp, sizeof(tmp), val.f);
        return xrt_str_from_cstr(tmp);
    }
    if (val.tag == XR_TAG_STR || val.tag == XR_TAG_STR_ARC)
        return val;
    if (val.tag == XR_TAG_RANGE)
        return xrt_range_to_string(val);
    if (val.tag == XR_TAG_NULL)
        return xr_box_str("null");
    if (val.tag == XR_TAG_BOOL)
        return xr_box_str(val.i ? "true" : "false");
    if (val.tag == XR_TAG_RUNE) {
        return xrt_rune_to_string(XR_TO_RUNE(val));
    }
    /* Enum values render through the shared formatter: a payload variant's
     * "(p1, ...)" text is unbounded (payloads may nest strings or further
     * enums), so a fixed-size buffer cannot reproduce the VM output. */
    if (val.tag == XR_TAG_ENUM)
        return xrt_value_to_string(val);
    /* Aggregates render through the shared formatter, which reproduces the VM's
     * xr_value_to_string output. Restricted to the shapes xrt_format_value
     * renders structurally; every other heap tag would fall through to its
     * "<object@%p>" placeholder, which is worse than a stable "[object]". */
    if (xrt_value_kind_is_formattable_aggregate(val))
        return xrt_value_to_string(val);
    return xr_box_str("[object]");
}

/* char(x): construct a Unicode scalar char (tagged XR_TAG_RUNE).
 * Validates range and excludes UTF-16 surrogates; invalid yields null. */
static XrValue xrt_to_rune(XrValue val) {
    if (XR_IS_RUNE(val))
        return val;
    if (XR_IS_INT(val)) {
        int64_t cp = XR_TO_INT(val);
        if (cp >= 0 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF))
            return XR_FROM_RUNE((uint32_t) cp);
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_chr(XrValue val) {
    XrValue ch = xrt_to_rune(val);
    return XR_IS_NULL(ch) ? XR_NULL_VAL : xrt_rune_to_string(XR_TO_RUNE(ch));
}

static XrValue xrt_to_int(XrValue val) {
    if (XR_IS_INT(val))
        return val;
    if (XR_IS_RUNE(val))
        return XR_FROM_INT((int64_t) XR_TO_RUNE(val));
    if (XR_IS_FLOAT(val))
        return XR_FROM_INT((int64_t) XR_TO_FLOAT(val));
    if (XR_IS_STR(val)) {
        /* Spec 13.2: a string that is not a whole decimal integer throws. The
         * declared return type is non-null int, so there is no null to hand
         * back here. */
        XrStringParseIntResult parsed =
            xr_string_parse_int64(xr_str_data(val), (size_t) xr_str_len(val));
        if (!parsed.ok)
            xrt_throw_error(XR_ERR_INVALID_ARG_TYPE, XR_ERROR_CORE_INT_PARSE_MSG);
        return XR_FROM_INT(parsed.value);
    }
    if (XR_IS_BOOL(val))
        return XR_FROM_INT(val.i != 0 ? 1 : 0);
    return XR_NULL_VAL;
}

static XrValue xrt_to_float(XrValue val) {
    if (XR_IS_FLOAT(val))
        return val;
    if (XR_IS_INT(val))
        return XR_FROM_FLOAT((double) XR_TO_INT(val));
    if (XR_IS_STR(val)) {
        /* Spec 13.2: see xrt_to_int -- a non-numeric string throws rather than
         * producing a null the declared float return type forbids. */
        XrStringParseFloatResult parsed =
            xr_string_parse_float64(xr_str_data(val), (size_t) xr_str_len(val));
        if (!parsed.ok)
            xrt_throw_error(XR_ERR_INVALID_ARG_TYPE, XR_ERROR_CORE_FLOAT_PARSE_MSG);
        return XR_FROM_FLOAT(parsed.value);
    }
    if (XR_IS_BOOL(val))
        return XR_FROM_FLOAT(val.i != 0 ? 1.0 : 0.0);
    return XR_NULL_VAL;
}

static XrValue xrt_to_string(XrValue val) {
    return XR_IS_STR(val) ? val : xrt_tostring(val, 0);
}

static XrValue xrt_to_bool(XrValue val) {
    XrTruthyCoreKind kind = XR_TRUTHY_CORE_OBJECT;
    int64_t integer = 0;
    double floating = 0.0;
    int64_t size = 0;
    if (XR_IS_BOOL(val)) {
        kind = XR_TRUTHY_CORE_BOOL;
        integer = XR_TO_BOOL(val);
    } else if (XR_IS_NULL(val)) {
        kind = XR_TRUTHY_CORE_NULL;
    } else if (XR_IS_INT(val)) {
        kind = XR_TRUTHY_CORE_INT;
        integer = XR_TO_INT(val);
    } else if (XR_IS_FLOAT(val)) {
        kind = XR_TRUTHY_CORE_FLOAT;
        floating = XR_TO_FLOAT(val);
    } else if (XR_IS_STR(val)) {
        kind = XR_TRUTHY_CORE_SIZED;
        size = xr_str_len(val);
    } else if (XR_IS_ARRAY(val)) {
        kind = XR_TRUTHY_CORE_SIZED;
        size = ((xrt_array_t *) val.ptr)->length;
    } else if (XR_IS_MAP(val)) {
        kind = XR_TRUTHY_CORE_SIZED;
        size = xrt_map_len((xrt_map_t *) val.ptr);
    } else if (XR_IS_SET(val)) {
        kind = XR_TRUTHY_CORE_SIZED;
        size = xrt_set_len((xrt_set_t *) val.ptr);
    }
    return XR_FROM_BOOL(xr_truthy_core_eval(XR_SEM_OWNER_ID_SHARED_TRUTHINESS_HI,
                                             XR_SEM_OWNER_ID_SHARED_TRUTHINESS_LO,
                                             XR_SEM_CONSUMER_AOT_HOSTED, kind, integer,
                                             floating, size));
}

/* Fixed-arity method dispatch is intentionally inlineable by the C compiler. */

static inline int64_t xrt_utf8_scalar_count(const char *s, int64_t slen) {
    if (!s || slen <= 0)
        return 0;
    const unsigned char *p = (const unsigned char *) s;
    const unsigned char *end = p + slen;
    int64_t count = 0;
    while (p < end) {
        unsigned char b = *p;
        int size = 1;
        if ((b & 0x80u) == 0)
            size = 1;
        else if ((b & 0xE0u) == 0xC0u)
            size = 2;
        else if ((b & 0xF0u) == 0xE0u)
            size = 3;
        else if ((b & 0xF8u) == 0xF0u)
            size = 4;
        if (p + size > end)
            size = 1;
        p += size;
        count++;
    }
    return count;
}

static inline XrValue xrt_string_entries(XrValue recv) {
    int64_t n = xrt_utf8_scalar_count(xr_str_data(recv), xr_str_len(recv));
    XrValue arr = xrt_array_with_capacity(n);
    xrt_iterator_t iter = {
        .coll = recv,
        .cursor = 0,
        .index = 0,
        .kind = XRT_ITER_PAIRS,
    };
    while (xrt_iterator_has_next(&iter))
        xrt_array_push(arr, xrt_iterator_next(&iter));
    return arr;
}

/* Return the receiver as an OWNED (+1) result.
 *
 * An arm that answers with its own receiver must still hand back a reference
 * the caller owns.  Returning it borrowed leaves one refcount with two owners:
 * `for (v in gen)` inside a generator puts both the generator and the iterator
 * its `iterator()` arm returns into frame slots that release_frame releases, so
 * nested_generator.xr freed the iterator when the loop ended and read it again
 * when the frame was torn down.
 *
 * The reason self-returning arms could not simply retain used to be the
 * statement-position use — `a.reverse()` for its side effect — where generated
 * C dropped the result without releasing it, so a retain there leaked.  The
 * code generator now releases what it discards (xrt_method_discard_N below), so
 * every self-returning arm in this file retains uniformly.
 *
 * A no-op for values ARC does not track, since xrt_retain early-returns on them.
 */
static inline XrValue xrt_method_return_self(XrValue recv) {
    xrt_retain(recv);
    return recv;
}

/* Drop an OWNED (+1) result the program does not consume.
 *
 * The single expression of "a discarded result is still owned, so releasing it
 * is the caller's job".  Generated C emits this directly for the lowerings that
 * bypass the symbol dispatchers (StringBuilder.toString), and xrt_method_discard_N
 * below routes through it for the ones that do not.
 *
 * Answers XR_NULL_VAL rather than void so it composes wherever the value form
 * was: an unused result still flows through the caller's representation
 * conversion before being cast away. */
static inline XrValue xrt_discard_owned(XrValue owned) {
    xrt_release(owned);
    return XR_NULL_VAL;
}

/* Does xrt_method_N hand back an OWNED (+1) result for this method symbol?
 *
 * This is the compiler-visible half of the dispatcher ownership convention
 * documented on xrt_method_0.  Generated C consults it (through
 * xrt_method_discard_N) when a call's result is discarded, so listing a symbol
 * here asserts that EVERY arm answering that symbol, for EVERY receiver kind,
 * returns either a +1 reference or a value ARC does not track.  Getting that
 * wrong releases a borrow, so the default is "no": an unlisted symbol is simply
 * never released, which at worst leaks the way the whole dispatcher used to.
 *
 * The table is keyed on the symbol alone, so a symbol whose arms disagree —
 * one receiver kind answering owned and another borrowed — cannot be listed at
 * all.  Each exclusion below says which arm disqualified it.
 *
 * "Owned" has to hold for the whole object graph, not just its outermost
 * allocation: releasing an array walks its elements, so a freshly allocated
 * array of borrowed elements is NOT an owned result.  That is the trap the
 * collection producers fall into; see XRT_SYM_KEYS below. */
static inline int xrt_method_result_is_owned(int sym) {
    switch (sym) {
        /* Freshly built strings, plus String.toString() answering with a
         * retained receiver. */
        case XRT_SYM_TOSTRING:
        case XRT_SYM_REPEAT:
        case XRT_SYM_REPLACE:
        case XRT_SYM_REPLACEALL:
        case XRT_SYM_SLICE:
        case XRT_SYM_SLICE_BYTES:
        case XRT_SYM_TOHEX:
        case XRT_SYM_TOFIXED:
        case XRT_SYM_FROM_UTF8:
        case XRT_SYM_FROM_UTF8_LOSSY:
        case XRT_SYM_FROM_RUNE:
        /* Freshly built iterator shells, plus Iterator.iterator() answering
         * with a retained receiver. */
        case XRT_SYM_ITERATOR:
        case XRT_SYM_ENTRIES_ITERATOR:
        case XRT_SYM_RUNES:
        /* Freshly built arrays whose ELEMENTS are also the caller's: split()
         * pushes strings it just allocated, and Range.toArray() pushes
         * integers.  See the note below for why most array producers cannot
         * be listed. */
        case XRT_SYM_SPLIT:
        case XRT_SYM_TO_ARRAY:
        /* Moved out of the receiver: pop/shift shorten it without releasing
         * what they hand back, so the reference is the caller's. */
        case XRT_SYM_POP:
        case XRT_SYM_SHIFT:
        /* Map.get() answers through xrt_map_get_owned, which promotes the
         * stored value to an owned reference. */
        case XRT_SYM_GET:
        /* Fresh collection shells whose element owners are promoted or
         * transferred into the result. */
        case XRT_SYM_KEYS:
        case XRT_SYM_VALUES:
        case XRT_SYM_ENTRIES:
        case XRT_SYM_MAP:
        case XRT_SYM_FILTER:
        /* Iterator pulls promote collection slots while preserving fresh pair
         * tuples and transferred generator yields. */
        case XRT_SYM_NEXT:
        case XRT_SYM_NTH:
        /* Retained receiver.  reverse() and StringBuilder.clear() answer with
         * it directly; the in-place array mutators answer through a helper
         * that returns its own argument, wrapped at the arm.  The Map, Set and
         * Array clear() arms answer with an untracked null, and Buffer.resize()
         * with an untracked bool. */
        case XRT_SYM_REVERSE:
        case XRT_SYM_CLEAR:
        case XRT_SYM_SORT:
        case XRT_SYM_RESERVE:
        case XRT_SYM_FILL:
        case XRT_SYM_RESIZE:
        case XRT_SYM_APPEND_FROM:
        case XRT_SYM_REPEATFROM:
            return 1;
        /* Deliberately absent, even where one reading looks owned:
         *
         *   XRT_SYM_JOIN               — Array.join and String.join build a
         *                                fresh string, but Thread.join answers
         *                                with thread->retval, which the thread
         *                                object still owns.  This table is
         *                                keyed on the symbol alone, so one
         *                                borrowed arm disqualifies it.
         *   XRT_SYM_REDUCE             — the accumulator comes back out of a
         *                                user closure, whose convention this
         *                                layer cannot see.
         */
        default:
            return 0;
    }
}

/* String 0-arg method dispatch. */
static inline XrValue xrt_str_method_0(const char *s, int64_t slen, XrValue recv, int sym) {
    if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
        return XR_FROM_INT(xr_str_rune_len(recv));
    if (sym == XRT_SYM_IS_EMPTY)
        return XR_FROM_BOOL(slen == 0);
    if (sym == XRT_SYM_TOSTRING)
        return xrt_method_return_self(recv);
    if (sym == XRT_SYM_ITERATOR || sym == XRT_SYM_RUNES)
        return xrt_iterator_new(recv, XRT_ITER_VALUES);
    if (sym == XRT_SYM_ENTRIES_ITERATOR)
        return xrt_iterator_new(recv, XRT_ITER_PAIRS);
    return XR_NULL_VAL;
}

/* string.copyBytes() -> Array<byte>: the UTF-8 bytes of the string. */
static inline XrValue xrt_str_to_bytes(XrValue s) {
    int64_t len = (int64_t) xr_str_len(s);
    xrt_array_t *b = xrt_array_new_typed_ptr(len, XR_ELEM_U8);
    if (len > 0)
        memcpy(b->data, xr_str_data(s), (size_t) len);
    return xr_mkptr(b, XR_TAG_ARRAY);
}

/* Builtin method dispatch by symbol id (xi_method_sym.def), 0..4 arguments.
 *
 * OWNERSHIP.  The receiver and the arguments are BORROWED: an arm may read them
 * and may mutate the receiver in place, but it never consumes a reference the
 * caller still holds.  A result is either +1 — a freshly allocated object, an
 * element moved out of the receiver (pop/shift shorten the receiver without
 * releasing what they hand back), or a receiver passed through
 * xrt_method_return_self — or a value ARC does not track at all (integers,
 * booleans, null, literal strings).  An arm answering with an untouched
 * `recv`, or with an element the receiver still owns, is a borrow and breaks
 * that convention: it leaves one refcount with two owners, which is how
 * nested_generator.xr came to read a freed iterator.
 *
 * Thread.join() still answers borrowed because the thread object owns
 * thread->retval. Iterator.next() and Iterator.nth() instead normalize every
 * source to an owned language result: collection slots are retained, pair
 * tuples are fresh, and generator yields transfer ownership.
 *
 * The in-place array mutators — sort(), reserve(), fill(), resize(),
 * appendFrom(), repeatFrom() — answer through a helper that returns its own
 * argument, so the arm wraps that call in xrt_method_return_self rather than
 * changing the helper, whose other callers keep their own conventions.
 *
 * A new arm that returns +1 belongs in xrt_method_result_is_owned(); a new arm
 * that returns a borrow needs a line in this comment saying why.
 *
 * SCOPE.  This convention covers the dispatchers, not the language operation.
 * The code generator has specialized lowerings that reach the same helpers
 * directly and answer BORROWED — `Array<byte>.fill()` becomes a bare
 * xrt_array_fill_value(), and reserve/resize/appendFrom/repeatFrom have their
 * own emitters.  Each of those is self-consistent (nothing releases what they
 * return), so the split is safe today, but it is the reason ownership cannot
 * yet be lifted into Xi: marking these symbols +1 in xi_arc would make the
 * borrowed lowerings double-free.  Making every lowering agree is the
 * prerequisite for an ARC pass that drops an owned method result at its death
 * point, which is what would close the leak in `var b = a.reverse()`. */
static inline XrValue xrt_method_0(XrValue recv, int sym) {
    /* Container/tuple toString renders via the shared value formatter so AOT
     * matches the VM ("[1, 2, 3]", "#{...}", "#[...]"). Simple enums are
     * XR_TAG_ENUM and handled by their own toString case below. */
    if (sym == XRT_SYM_TOSTRING) {
        int rk = xrt_value_kind(recv);
        if (rk == XR_TAG_ARRAY) {
            return xrt_value_to_string(recv);
        }
        if (rk == XR_TAG_MAP || rk == XR_TAG_SET || rk == XR_TAG_TUPLE)
            return xrt_value_to_string(recv);
        if (rk == XR_TAG_BIGINT)
            return xrt_value_to_string(recv);
    }
    if (XR_IS_STR(recv)) {
        return xrt_str_method_0(xr_str_data(recv), xr_str_len(recv), recv, sym);
    }
    if (XR_IS_ARRAY(recv)) {
        xrt_array_t *a = (xrt_array_t *) recv.ptr;
        if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
            return XR_FROM_INT(a->length);
        if (sym == XRT_SYM_CAPACITY)
            return XR_FROM_INT(a->capacity);
        if (sym == XRT_SYM_IS_EMPTY)
            return XR_FROM_BOOL(a->length == 0);
        if (sym == XRT_SYM_CLEAR)
            return xrt_array_clear_value(recv);
        if (sym == XRT_SYM_POP && a->length > 0) {
            a->length--;
            return xr_typed_get(a->data, (int32_t) a->length, a->elem_type);
        }
        if (sym == XRT_SYM_SHIFT && a->length > 0) {
            XrValue first = xr_typed_get(a->data, 0, a->elem_type);
            for (int64_t i = 0; i < a->length - 1; i++) {
                XrValue next = xr_typed_get(a->data, (int32_t) (i + 1), a->elem_type);
                xr_typed_set(a->data, (int32_t) i, next, a->elem_type);
            }
            a->length--;
            return first;
        }
        if (sym == XRT_SYM_REVERSE) {
            for (int64_t i = 0, j = a->length - 1; i < j; i++, j--) {
                XrValue vi = xr_typed_get(a->data, (int32_t) i, a->elem_type);
                XrValue vj = xr_typed_get(a->data, (int32_t) j, a->elem_type);
                xr_typed_set(a->data, (int32_t) i, vj, a->elem_type);
                xr_typed_set(a->data, (int32_t) j, vi, a->elem_type);
            }
            return xrt_method_return_self(recv);
        }
        if (sym == XRT_SYM_SORT)
            return xrt_method_return_self(xrt_array_sort(recv, NULL));
        /* Same iteration protocol Map / Set / string / Json already answer.
         * Statically typed `for (x in arr)` never lands here (it lowers to
         * len()/index), but a nested generic body — which keeps its type
         * parameter, since only file-scope generics monomorphize — and an
         * explicit arr.iterator() both do. */
        if (sym == XRT_SYM_ITERATOR)
            return xrt_iterator_new(recv, XRT_ITER_VALUES);
        if (sym == XRT_SYM_ENTRIES_ITERATOR)
            return xrt_iterator_new(recv, XRT_ITER_PAIRS);
    }
    if (XR_IS_MAP(recv)) {
        xrt_map_t *m = (xrt_map_t *) recv.ptr;
        if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
            return XR_FROM_INT(xrt_map_len(m));
        if (sym == XRT_SYM_IS_EMPTY)
            return XR_FROM_BOOL(xrt_map_len(m) == 0);
        if (sym == XRT_SYM_CLEAR) {
            xrt_map_clear(m);
            return XR_NULL_VAL;
        }
        if (sym == XRT_SYM_KEYS)
            return xrt_map_keys(m);
        if (sym == XRT_SYM_VALUES)
            return xrt_map_values(m);
        if (sym == XRT_SYM_ENTRIES)
            return xrt_map_entries(m);
        if (sym == XRT_SYM_ITERATOR)
            return xrt_iterator_new(recv, XRT_ITER_KEYS);
        if (sym == XRT_SYM_ENTRIES_ITERATOR)
            return xrt_iterator_new(recv, XRT_ITER_PAIRS);
    }
    if (XR_IS_SET(recv)) {
        xrt_set_t *s = (xrt_set_t *) recv.ptr;
        if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
            return XR_FROM_INT(xrt_set_len(s));
        if (sym == XRT_SYM_IS_EMPTY)
            return XR_FROM_BOOL(xrt_set_len(s) == 0);
        if (sym == XRT_SYM_CLEAR) {
            xrt_set_clear(s);
            return XR_NULL_VAL;
        }
        if (sym == XRT_SYM_VALUES)
            return xrt_set_values(s);
        if (sym == XRT_SYM_ITERATOR)
            return xrt_iterator_new(recv, XRT_ITER_VALUES);
    }
    if (recv.tag == XR_TAG_ITERATOR) {
        xrt_iterator_t *it = (xrt_iterator_t *) recv.ptr;
        if (sym == XRT_SYM_ITERATOR)
            return xrt_method_return_self(recv);
        if (sym == XRT_SYM_HAS_NEXT)
            return XR_FROM_BOOL(xrt_iterator_has_next(it));
        if (sym == XRT_SYM_NEXT)
            return xrt_iterator_next(it);
    }
    if (recv.tag == XR_TAG_STRBUF) {
        xrt_strbuf_t *sb = (xrt_strbuf_t *) recv.ptr;
        if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
            return XR_FROM_INT(sb ? sb->len : 0);
        if (sym == XRT_SYM_CLEAR) {
            if (sb) {
                sb->len = 0;
                if (sb->buf)
                    sb->buf[0] = 0;
            }
            return xrt_method_return_self(recv);
        }
        if (sym == XRT_SYM_TOSTRING)
            return xrt_strbuf_finish(recv);
    }
    if (recv.tag == XR_TAG_RANGE)
        return xrt_range_method_0(recv, sym);
    if (recv.tag == XR_TAG_BUFFER)
        return xrt_buffer_method_0(recv, sym);
    if (recv.tag == XR_TAG_SYS_MUTEX)
        return xrt_sys_mutex_method_0(recv, sym);
    if (recv.tag == XR_TAG_SYS_RWLOCK)
        return xrt_sys_rwlock_method_0(recv, sym);
    if (recv.tag == XR_TAG_SYS_CONDVAR)
        return xrt_sys_condvar_method_0(recv, sym);
    if (recv.tag == XR_TAG_SYS_BARRIER)
        return xrt_sys_barrier_method_0(recv, sym);
#ifdef XRT_ENABLE_SYS_THREAD
    if (recv.tag == XR_TAG_THREAD)
        return xrt_thread_method_0(recv, sym);
#endif
    if (recv.tag == XR_TAG_I64) {
        if (sym == XRT_SYM_ABS)
            return XR_FROM_INT(xr_i64_abs_wrap(recv.i));
        if (sym == XRT_SYM_TOSTRING)
            return xrt_tostring(recv, 1);
        if (sym == XRT_SYM_TOHEX) {
            char buf[32];
            if (recv.i < 0)
                snprintf(buf, sizeof(buf), "-0x%" PRIX64, xr_i64_abs_magnitude(recv.i));
            else
                snprintf(buf, sizeof(buf), "0x%" PRIX64, (uint64_t) recv.i);
            return xrt_str_from_cstr(buf);
        }
        if (sym == XRT_SYM_SQRT) {
            double value = (double) recv.i;
            return XR_FROM_FLOAT(value < 0 ? NAN : sqrt(value));
        }
    }
    if (recv.tag == XR_TAG_F64) {
        double v = recv.f;
        if (sym == XRT_SYM_TOSTRING)
            return xrt_tostring(recv, 2);
        /* floor/ceil/round return int (matching the VM float methods), not float. */
        if (sym == XRT_SYM_FLOOR)
            return XR_FROM_INT((int64_t) floor(v));
        if (sym == XRT_SYM_CEIL)
            return XR_FROM_INT((int64_t) ceil(v));
        if (sym == XRT_SYM_ROUND)
            return XR_FROM_INT((int64_t) round(v));
        if (sym == XRT_SYM_ABS)
            return XR_FROM_FLOAT(fabs(v));
        if (sym == XRT_SYM_SQRT)
            return XR_FROM_FLOAT(sqrt(v));
        if (sym == XRT_SYM_ISNAN)
            return XR_FROM_BOOL(isnan(v));
    }
    if (recv.tag == XR_TAG_BOOL && sym == XRT_SYM_TOSTRING)
        return xrt_tostring(recv, 0);
    if (recv.tag == XR_TAG_RUNE) {
        uint32_t cp = XR_TO_RUNE(recv);
        if (sym == XRT_SYM_TOSTRING)
            return xrt_rune_to_string(cp);
        if (sym == XRT_SYM_TO_UINT32)
            return XR_FROM_INT((int64_t) cp);
        if (sym == XRT_SYM_IS_LETTER)
            return XR_FROM_BOOL(xrt_rune_is_letter(cp));
        if (sym == XRT_SYM_IS_NUMBER)
            return XR_FROM_BOOL(xrt_rune_is_number(cp));
        if (sym == XRT_SYM_IS_ALNUM)
            return XR_FROM_BOOL(xrt_rune_is_alnum(cp));
        if (sym == XRT_SYM_IS_WHITESPACE)
            return XR_FROM_BOOL(xrt_rune_is_whitespace(cp));
    }
    if (recv.tag == XR_TAG_ENUM && sym == XRT_SYM_TOSTRING)
        return xrt_tostring(recv, 0);
    return XR_NULL_VAL;
}

static inline XrValue xrt_str_from_core_slice(XrStringCoreSlice slice) {
    XrValue sv = xrt_str_alloc(slice.len);
    if (slice.len != 0)
        memcpy(xr_str_buf(sv), slice.data, slice.len);
    xr_str_buf(sv)[slice.len] = 0;
    return sv;
}

typedef struct XrtStringSplitCtx {
    XrValue array;
} XrtStringSplitCtx;

static inline bool xrt_str_split_emit(XrStringCoreSlice slice, void *user) {
    XrtStringSplitCtx *ctx = (XrtStringSplitCtx *) user;
    xrt_array_push(ctx->array, xrt_str_from_core_slice(slice));
    return true;
}

static inline XrValue xrt_str_split(const char *s, int64_t slen, const char *sep, size_t sep_len) {
    if (slen < 0)
        return XR_NULL_VAL;
    size_t len = (size_t) slen;
    XrStringCoreSplitPlan plan = xr_string_core_split_plan(s, len, sep, sep_len);
    if (plan.kind == XR_STRING_CORE_SPLIT_INVALID || plan.count > (size_t) INT64_MAX)
        return XR_NULL_VAL;

    XrValue arr = xrt_array_with_capacity((int64_t) plan.count);
    XrtStringSplitCtx ctx = {arr};
    size_t emitted = xr_string_core_split_each(s, len, sep, sep_len, xrt_str_split_emit, &ctx);
    return emitted == plan.count ? arr : XR_NULL_VAL;
}

/* String 1-arg method dispatch. */
static inline XrValue xrt_str_method_1(const char *s, int64_t slen, XrValue recv, int sym,
                                       XrValue arg0) {
    if (sym == XRT_SYM_FROM_UTF8 || sym == XRT_SYM_FROM_UTF8_LOSSY) {
        const uint8_t *data = NULL;
        size_t len = 0;
        bool has_bytes = false;
        if (XR_IS_ARRAY(arg0)) {
            xrt_array_t *bytes = (xrt_array_t *) arg0.ptr;
            if (bytes && bytes->elem_type == XR_ELEM_U8 && bytes->length >= 0) {
                data = (const uint8_t *) bytes->data;
                len = (size_t) bytes->length;
                has_bytes = len == 0 || data != NULL;
            }
        } else if (arg0.tag == XR_TAG_AGG_REF && arg0.ext == 0 && arg0.heap_type == UINT16_MAX &&
                   arg0.ptr) {
            xr_span_t bytes = xrt_span_from_value_ref(arg0);
            if (bytes.length >= 0 && (uint64_t) bytes.length <= (uint64_t) SIZE_MAX &&
                (bytes.length == 0 || bytes.data != NULL)) {
                data = (const uint8_t *) bytes.data;
                len = (size_t) bytes.length;
                has_bytes = true;
            }
        }
        if (!has_bytes)
            return sym == XRT_SYM_FROM_UTF8 ? XR_NULL_VAL : xrt_str_alloc(0);
        XrUtf8ScanResult scan = xr_utf8_core_scan_strict(data, len);
        if (scan.error == XR_UTF8_OK) {
            XrValue out = xrt_str_alloc(len);
            if (len > 0)
                memcpy(xr_str_buf(out), data, len);
            xr_str_buf(out)[len] = 0;
            xr_str_set_rune_len(out, (uint32_t) scan.rune_count);
            return out;
        }
        if (sym == XRT_SYM_FROM_UTF8) {
            xrt_set_builtin_enum_error("Utf8Error", "InvalidUtf8", 0);
            return XR_NULL_VAL;
        }

        XrUtf8LossyPlan plan = xr_utf8_core_lossy_plan(data, len);
        if (plan.overflow || plan.rune_count > (size_t) INT64_MAX ||
            plan.output_length > (size_t) INT64_MAX)
            return XR_NULL_VAL;
        XrValue out = xrt_str_alloc(plan.output_length);
        size_t dst = xr_utf8_core_lossy_write(xr_str_buf(out), data, len);
        xr_str_buf(out)[dst] = 0;
        xr_str_set_len(out, (uint32_t) dst);
        xr_str_set_rune_len(out, (uint32_t) plan.rune_count);
        return out;
    }
    if (sym == XRT_SYM_CONTAINS && XR_IS_STR(arg0)) {
        return XR_FROM_BOOL(xr_string_core_contains(s, (size_t) slen, xr_str_data(arg0),
                                                    (size_t) xr_str_len(arg0)));
    }
    if (sym == XRT_SYM_INDEXOF && XR_IS_STR(arg0)) {
        return XR_FROM_INT((int64_t) xr_string_core_index_of(s, (size_t) slen, xr_str_data(arg0),
                                                             (size_t) xr_str_len(arg0)));
    }
    if (sym == XRT_SYM_SLICE && arg0.tag == XR_TAG_I64) {
        int64_t count = xr_str_rune_len(recv);
        if (arg0.i < 0 || arg0.i > count)
            xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "string.slice rune range out of bounds");
        XrStringCoreSlice slice = xr_string_core_range_slice(s, (size_t) slen, arg0.i, count);
        return xrt_str_from_core_slice(slice);
    }
    if (sym == XRT_SYM_STARTSWITH && XR_IS_STR(arg0)) {
        const char *p = xr_str_data(arg0);
        size_t plen = (size_t) xr_str_len(arg0);
        return XR_FROM_BOOL(xr_string_core_starts_with(s, (size_t) slen, p, plen));
    }
    if (sym == XRT_SYM_ENDSWITH && XR_IS_STR(arg0)) {
        const char *p = xr_str_data(arg0);
        size_t plen = (size_t) xr_str_len(arg0);
        return XR_FROM_BOOL(xr_string_core_ends_with(s, (size_t) slen, p, plen));
    }
    if (sym == XRT_SYM_LASTINDEXOF && XR_IS_STR(arg0)) {
        const char *needle = xr_str_data(arg0);
        size_t nlen = (size_t) xr_str_len(arg0);
        if (nlen == 0)
            return XR_FROM_INT(slen);
        for (int64_t i = slen - (int64_t) nlen; i >= 0; i--) {
            if (memcmp(s + i, needle, nlen) == 0)
                return XR_FROM_INT(i);
        }
        return XR_FROM_INT(-1);
    }
    if (sym == XRT_SYM_SPLIT && XR_IS_STR(arg0)) {
        const char *sep = xr_str_data(arg0);
        size_t seplen = (size_t) xr_str_len(arg0);
        return xrt_str_split(s, slen, sep, seplen);
    }
    if (sym == XRT_SYM_REPEAT && arg0.tag == XR_TAG_I64) {
        XrStringCoreRepeatPlan plan = xr_string_core_repeat_plan(s, (size_t) slen, arg0.i);
        if (plan.kind == XR_STRING_CORE_REPEAT_INVALID)
            return XR_NULL_VAL;
        if (plan.kind == XR_STRING_CORE_REPEAT_EMPTY)
            return xrt_str_alloc(0);
        if (plan.kind == XR_STRING_CORE_REPEAT_ORIGINAL)
            return xrt_method_return_self(recv);
        XrValue sv = xrt_str_alloc(plan.len);
        xr_string_core_repeat_write(xr_str_buf(sv), s, (size_t) slen, arg0.i);
        return sv;
    }
    if (sym == XRT_SYM_REPLACE && XR_IS_STR(arg0)) {
        /* replace(old) with empty string — 1-arg form */
        const char *old_s = xr_str_data(arg0);
        const char *found = strstr(s, old_s);
        if (!found)
            return xrt_method_return_self(recv);
        size_t olen = (size_t) xr_str_len(arg0);
        size_t rlen = (size_t) slen - olen;
        XrValue sv = xrt_str_alloc(rlen);
        char *r = xr_str_buf(sv);
        size_t pre = (size_t) (found - s);
        memcpy(r, s, pre);
        memcpy(r + pre, found + olen, (size_t) slen - pre - olen);
        r[rlen] = 0;
        return sv;
    }
    return XR_NULL_VAL;
}

/* Dedicated target-plan spelling for the exact built-in String.runes member.
 * Generated C never re-selects the numeric method symbol. */
static inline XrValue xrt_string_runes(XrValue receiver) {
    return xrt_method_0(receiver, XRT_SYM_RUNES);
}

/* Dedicated target-plan spelling for Iterator<rune>.hasNext. The generic
 * method remains responsible for validation and the pending-error channel;
 * generated C neither selects a method symbol nor infers the scalar result. */
static inline uint8_t xrt_iterator_rune_has_next(XrValue receiver) {
    return (uint8_t) XR_TO_BOOL(xrt_method_0(receiver, XRT_SYM_HAS_NEXT));
}

static inline uint32_t xrt_iterator_rune_next(XrValue receiver) {
    return XR_TO_RUNE(xrt_method_0(receiver, XRT_SYM_NEXT));
}

static inline uint32_t xrt_rune_to_uint32(uint32_t receiver) {
    return receiver;
}

static inline XrValue xrt_len_value(XrValue recv) {
    XrValue result = xrt_method_0(recv, XRT_SYM_LENGTH);
    if (!XR_IS_NULL(result))
        return result;
    xrt_throw_error(XR_ERR_TYPE_MISMATCH, "value does not implement Lengthable");
    return XR_NULL_VAL;
}

/* ISO C expression form used by MSVC-hosted fragments.  Keeping the temporary
 * inside an inline function gives the same single-evaluation guarantee as the
 * old GNU statement expression without leaking a compiler extension into
 * generated C. */
static inline int64_t xrt_len_i64(XrValue recv) {
    if (recv.tag == XR_TAG_RANGE)
        return xrt_range_length_ptr((const xrt_range_t *) recv.ptr);
    return XR_TO_INT(xrt_len_value(recv));
}

static inline XrValue xrt_method_1(XrValue recv, int sym, XrValue arg0) {
    /* Static string constructors do not depend on the runtime class value. */
    if (sym == XRT_SYM_FROM_UTF8 || sym == XRT_SYM_FROM_UTF8_LOSSY)
        return xrt_str_method_1("", 0, XR_NULL_VAL, sym, arg0);
    if (sym == XRT_SYM_FROM_RUNE && XR_IS_RUNE(arg0))
        return xrt_rune_to_string(XR_TO_RUNE(arg0));
    if (sym == XRT_SYM_JOIN && XR_IS_ARRAY(arg0))
        return xrt_method_1(arg0, XRT_SYM_JOIN, xrt_str_alloc(0));
    if (recv.tag == XR_TAG_ITERATOR && sym == XRT_SYM_NTH && arg0.tag == XR_TAG_I64) {
        xrt_iterator_t *it = (xrt_iterator_t *) recv.ptr;
        if (arg0.i < 0)
            xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "Iterator.nth index must be non-negative");
        for (int64_t i = 0; i <= arg0.i; i++) {
            /* Same contract as next(): nth(index) needs index+1 elements left,
             * and running dry is a protocol violation, not a bad index. A
             * generator that ran dry by failing already reported its own error
             * through hasNext(); do not bury that under the violation. */
            if (!xrt_iterator_has_next(it) &&
                !(it->kind == XRT_ITER_GENERATOR && xrt_has_pending_error()))
                xrt_throw_error(XR_ERR_ITERATOR_EXHAUSTED,
                                XR_ERROR_CORE_ITERATOR_EXHAUSTED_NTH_MSG);
            XrValue value = xrt_iterator_next(it);
            if (i == arg0.i)
                return value;
        }
    }
    if (XR_IS_STR(recv)) {
        return xrt_str_method_1(xr_str_data(recv), xr_str_len(recv), recv, sym, arg0);
    }
    if (XR_IS_ARRAY(recv)) {
        xrt_array_t *a = (xrt_array_t *) recv.ptr;
        if (sym == XRT_SYM_PUSH) {
            xrt_array_push(recv, arg0);
            return XR_NULL_VAL;
        }
        if (sym == XRT_SYM_RESERVE)
            return xrt_method_return_self(xrt_array_reserve_value(recv, arg0));
        if (sym == XRT_SYM_APPEND_FROM)
            return xrt_method_return_self(xrt_byte_array_append_from_value(recv, arg0));
        if (sym == XRT_SYM_RESIZE)
            xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_ARRAY_RESIZE_REQUIRES_FILL_MSG);
        if (sym == XRT_SYM_UNSHIFT) {
            xrt_array_check_store_or_abort(a, arg0, "Array.unshift");
            if (XR_UNLIKELY(a->data_storage == XR_ARRAY_DATA_BORROWED)) {
                fprintf(stderr, "xrt_array_unshift: cannot unshift array slice\n");
                abort();
            }
            if (XR_UNLIKELY(a->length >= a->capacity))
                xrt_array_data_grow(a, a->capacity == 0 ? 4 : a->capacity * 2);
            memmove((uint8_t *) a->data + a->elem_size, a->data,
                    (size_t) a->length * (size_t) a->elem_size);
            a->length++;
            xr_typed_set(a->data, 0, arg0, a->elem_type);
            XR_ARRAY_MARK_MUTATED(a);
            return XR_NULL_VAL;
        }
        if (sym == XRT_SYM_FILL) {
            return xrt_method_return_self(
                xrt_array_fill_value(recv, arg0, XR_FROM_INT(0), XR_FROM_INT(a->length)));
        }
        if (sym == XRT_SYM_INDEXOF) {
            int handled;
            int64_t idx = xrt_array_indexof_typed_fast(a, arg0, &handled);
            if (handled)
                return XR_FROM_INT(idx);
            for (int64_t i = 0; i < a->length; i++) {
                XrValue elem = xr_typed_get(a->data, (int32_t) i, a->elem_type);
                if (xrt_key_eq(elem, arg0))
                    return XR_FROM_INT(i);
            }
            return XR_FROM_INT(-1);
        }
        if (sym == XRT_SYM_CONTAINS) {
            int handled;
            int64_t idx = xrt_array_indexof_typed_fast(a, arg0, &handled);
            if (handled)
                return XR_FROM_BOOL(idx >= 0);
            for (int64_t i = 0; i < a->length; i++) {
                XrValue elem = xr_typed_get(a->data, (int32_t) i, a->elem_type);
                if (xrt_key_eq(elem, arg0))
                    return XR_FROM_BOOL(1);
            }
            return XR_FROM_BOOL(0);
        }
        if (sym == XRT_SYM_JOIN && XR_IS_STR(arg0)) {
            const char *sep = xr_str_data(arg0);
            size_t seplen = (size_t) xr_str_len(arg0);
            size_t total = 0;
            for (int64_t i = 0; i < a->length; i++) {
                XrValue sv = xrt_tostring(xr_typed_get(a->data, (int32_t) i, a->elem_type), 0);
                total += (size_t) xr_str_len(sv);
                if (i < a->length - 1)
                    total += seplen;
            }
            XrValue result = xrt_str_alloc(total);
            char *r = xr_str_buf(result);
            size_t pos = 0;
            for (int64_t i = 0; i < a->length; i++) {
                XrValue sv = xrt_tostring(xr_typed_get(a->data, (int32_t) i, a->elem_type), 0);
                const char *p = xr_str_data(sv);
                size_t plen = (size_t) xr_str_len(sv);
                memcpy(r + pos, p, plen);
                pos += plen;
                if (i < a->length - 1) {
                    memcpy(r + pos, sep, seplen);
                    pos += seplen;
                }
            }
            r[total] = 0;
            return result;
        }
        /* Higher-order callbacks are AOT closures. */
        if (arg0.tag == XR_TAG_CLOSURE) {
            xrt_closure_t *cl = (xrt_closure_t *) arg0.ptr;
            typedef XrValue (*xrt_fn1_t)(xrt_closure_t *, XrValue);
            xrt_fn1_t fn = (xrt_fn1_t) cl->callable->sync_entry;
            if (sym == XRT_SYM_SORT)
                return xrt_method_return_self(xrt_array_sort(recv, cl));
            if (sym == XRT_SYM_MAP) {
                return xrt_array_map_typed(recv, arg0, XR_ELEM_ANY);
            }
            if (sym == XRT_SYM_FILTER) {
                return xrt_array_filter_typed(recv, arg0);
            }
            if (sym == XRT_SYM_FOREACH) {
                for (int64_t i = 0; i < a->length; i++)
                    fn(cl, xr_typed_get(a->data, (int32_t) i, a->elem_type));
                return XR_NULL_VAL;
            }
        }
    }
    if (XR_IS_MAP(recv)) {
        xrt_map_t *m = (xrt_map_t *) recv.ptr;
        if (sym == XRT_SYM_GET)
            return xrt_map_get_owned(m, arg0);
        if (sym == XRT_SYM_CONTAINS_KEY)
            return XR_FROM_BOOL(xrt_map_has(m, arg0));
        if (sym == XRT_SYM_CONTAINS_VALUE)
            return XR_FROM_BOOL(xrt_map_has_value(m, arg0));
        if (sym == XRT_SYM_DELETE)
            return XR_FROM_BOOL(xrt_map_delete(m, arg0));
    }
    if (XR_IS_SET(recv)) {
        xrt_set_t *s = (xrt_set_t *) recv.ptr;
        if (sym == XRT_SYM_ADD) {
            (void) xrt_set_add(s, arg0);
            return XR_NULL_VAL;
        }
        if (sym == XRT_SYM_CONTAINS)
            return XR_FROM_BOOL(xrt_set_has(s, arg0));
        if (sym == XRT_SYM_DELETE)
            return XR_FROM_BOOL(xrt_set_delete(s, arg0));
    }
    if (recv.tag == XR_TAG_RANGE)
        return xrt_range_method_1(recv, sym, arg0);
    if (recv.tag == XR_TAG_BUFFER)
        return xrt_buffer_method_1(recv, sym, arg0);
    if (recv.tag == XR_TAG_SYS_CONDVAR)
        return xrt_sys_condvar_method_1(recv, sym, arg0);
    if (recv.tag == XR_TAG_SYS_ONCE)
        return xrt_sys_once_method_1(recv, sym, arg0);
    if (recv.tag == XR_TAG_F64 && sym == XRT_SYM_POW) {
        double exp = (arg0.tag == XR_TAG_F64) ? arg0.f : (double) arg0.i;
        return XR_FROM_FLOAT(pow(recv.f, exp));
    }
    /* toFixed(digits): clamp decimals to [0, XR_TOFIXED_MAX_DECIMALS] via the
     * shared numeric core, matching the VM (negative -> 0, large -> capped). */
    if (recv.tag == XR_TAG_F64 && sym == XRT_SYM_TOFIXED && arg0.tag == XR_TAG_I64) {
        char buf[64];
        xr_numeric_core_format_fixed(buf, sizeof(buf), recv.f, arg0.i);
        return xrt_str_from_cstr(buf);
    }
    if (recv.tag == XR_TAG_I64 && arg0.tag == XR_TAG_I64) {
        int64_t out;
        if (sym == XRT_SYM_CHECKED_ADD)
            return xr_i64_checked_add(recv.i, arg0.i, &out) ? XR_FROM_INT(out) : XR_NULL_VAL;
        if (sym == XRT_SYM_CHECKED_SUB)
            return xr_i64_checked_sub(recv.i, arg0.i, &out) ? XR_FROM_INT(out) : XR_NULL_VAL;
        if (sym == XRT_SYM_CHECKED_MUL)
            return xr_i64_checked_mul(recv.i, arg0.i, &out) ? XR_FROM_INT(out) : XR_NULL_VAL;
        if (sym == XRT_SYM_SATURATING_ADD)
            return XR_FROM_INT(xr_i64_saturating_add(recv.i, arg0.i));
        if (sym == XRT_SYM_SATURATING_SUB)
            return XR_FROM_INT(xr_i64_saturating_sub(recv.i, arg0.i));
        if (sym == XRT_SYM_SATURATING_MUL)
            return XR_FROM_INT(xr_i64_saturating_mul(recv.i, arg0.i));
        if (sym == XRT_SYM_WRAPPING_ADD)
            return XR_FROM_INT(xr_i64_add_wrap(recv.i, arg0.i));
        if (sym == XRT_SYM_WRAPPING_SUB)
            return XR_FROM_INT(xr_i64_sub_wrap(recv.i, arg0.i));
        if (sym == XRT_SYM_WRAPPING_MUL)
            return XR_FROM_INT(xr_i64_mul_wrap(recv.i, arg0.i));
        /* Overflow predicates use the same core as the VM binding. */
        if (sym == XRT_SYM_ADD_OVERFLOWS)
            return XR_FROM_BOOL(xr_arith_core_add_overflows(recv.i, arg0.i) != 0);
        if (sym == XRT_SYM_SUB_OVERFLOWS)
            return XR_FROM_BOOL(xr_arith_core_sub_overflows(recv.i, arg0.i) != 0);
        if (sym == XRT_SYM_MUL_OVERFLOWS)
            return XR_FROM_BOOL(xr_arith_core_mul_overflows(recv.i, arg0.i) != 0);
    }
    /* max/min accept int or float operands. */
    if (sym == XRT_SYM_MAX) {
        if (recv.tag == XR_TAG_I64 && arg0.tag == XR_TAG_I64)
            return XR_FROM_INT(recv.i > arg0.i ? recv.i : arg0.i);
        double a = (recv.tag == XR_TAG_F64) ? recv.f : (double) recv.i;
        double b = (arg0.tag == XR_TAG_F64) ? arg0.f : (double) arg0.i;
        return XR_FROM_FLOAT(a > b ? a : b);
    }
    if (sym == XRT_SYM_MIN) {
        if (recv.tag == XR_TAG_I64 && arg0.tag == XR_TAG_I64)
            return XR_FROM_INT(recv.i < arg0.i ? recv.i : arg0.i);
        double a = (recv.tag == XR_TAG_F64) ? recv.f : (double) recv.i;
        double b = (arg0.tag == XR_TAG_F64) ? arg0.f : (double) arg0.i;
        return XR_FROM_FLOAT(a < b ? a : b);
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_method_2(XrValue recv, int sym, XrValue arg0, XrValue arg1) {
    if (sym == XRT_SYM_JOIN && XR_IS_ARRAY(arg0) && XR_IS_STR(arg1))
        return xrt_method_1(arg0, XRT_SYM_JOIN, arg1);
    if (XR_IS_STR(recv) && sym == XRT_SYM_INDEXOF && XR_IS_STR(arg0) && arg1.tag == XR_TAG_I64) {
        return XR_FROM_INT((int64_t) xr_string_core_index_of_from(
            xr_str_data(recv), (size_t) xr_str_len(recv), xr_str_data(arg0),
            (size_t) xr_str_len(arg0), arg1.i));
    }
    if (XR_IS_STR(recv) && sym == XRT_SYM_SLICE) {
        const char *s = xr_str_data(recv);
        size_t slen = (size_t) xr_str_len(recv);
        int64_t start = (arg0.tag == XR_TAG_I64) ? arg0.i : 0;
        int64_t count = xr_str_rune_len(recv);
        int64_t end = (arg1.tag == XR_TAG_I64) ? arg1.i : count;
        if (start < 0 || end < start || end > count)
            xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "string.slice rune range out of bounds");
        XrStringCoreSlice slice = xr_string_core_range_slice(s, slen, start, end);
        return xrt_str_from_core_slice(slice);
    }
    if (XR_IS_STR(recv) && sym == XRT_SYM_SLICE_BYTES) {
        const char *s = xr_str_data(recv);
        int64_t slen = xr_str_len(recv);
        int64_t start = (arg0.tag == XR_TAG_I64) ? arg0.i : 0;
        int64_t end = (arg1.tag == XR_TAG_I64) ? arg1.i : slen;
        XrStringCoreByteRange range =
            xr_string_core_utf8_byte_range(s, slen > 0 ? (size_t) slen : 0, start, end);
        if (range.error != XR_STRING_CORE_BYTE_RANGE_OK) {
            xrt_set_builtin_enum_error("StringSliceError", "InvalidByteRange", 0);
            return XR_NULL_VAL;
        }
        XrValue out = xrt_str_alloc(range.slice.len);
        if (range.slice.len > 0)
            memcpy(xr_str_buf(out), range.slice.data, range.slice.len);
        xr_str_buf(out)[range.slice.len] = 0;
        return out;
    }
    if (XR_IS_STR(recv) && sym == XRT_SYM_REPLACEALL && XR_IS_STR(arg0) && XR_IS_STR(arg1)) {
        const char *s = xr_str_data(recv);
        const char *old_s = xr_str_data(arg0);
        const char *new_s = xr_str_data(arg1);
        size_t slen = (size_t) xr_str_len(recv);
        size_t olen = (size_t) xr_str_len(arg0), nlen = (size_t) xr_str_len(arg1);
        XrStringCoreReplacePlan plan =
            xr_string_core_replace_plan(s, slen, old_s, olen, new_s, nlen, true);
        if (plan.kind == XR_STRING_CORE_REPLACE_INVALID)
            return XR_NULL_VAL;
        if (plan.kind == XR_STRING_CORE_REPLACE_ORIGINAL)
            return xrt_method_return_self(recv);
        XrValue sv = xrt_str_alloc(plan.len);
        xr_string_core_replace_write(xr_str_buf(sv), s, slen, old_s, olen, new_s, nlen, plan, true);
        return sv;
    }
    if (XR_IS_STR(recv) && sym == XRT_SYM_REPLACE && XR_IS_STR(arg0) && XR_IS_STR(arg1)) {
        const char *s = xr_str_data(recv);
        const char *old_s = xr_str_data(arg0);
        const char *new_s = xr_str_data(arg1);
        size_t slen = (size_t) xr_str_len(recv);
        size_t olen = (size_t) xr_str_len(arg0), nlen = (size_t) xr_str_len(arg1);
        XrStringCoreReplacePlan plan =
            xr_string_core_replace_plan(s, slen, old_s, olen, new_s, nlen, false);
        if (plan.kind == XR_STRING_CORE_REPLACE_INVALID)
            return XR_NULL_VAL;
        if (plan.kind == XR_STRING_CORE_REPLACE_ORIGINAL)
            return xrt_method_return_self(recv);
        XrValue sv = xrt_str_alloc(plan.len);
        xr_string_core_replace_write(xr_str_buf(sv), s, slen, old_s, olen, new_s, nlen, plan,
                                     false);
        return sv;
    }
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_REDUCE && arg0.tag == XR_TAG_CLOSURE)
        return xrt_array_reduce_typed(recv, arg0, arg1);
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_RESIZE)
        return xrt_method_return_self(xrt_array_resize_value(recv, arg0, arg1));
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_REPEATFROM)
        return xrt_method_return_self(xrt_byte_array_repeat_from_tail_value(recv, arg0, arg1));
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_FILL) {
        xrt_array_t *a = (xrt_array_t *) recv.ptr;
        return xrt_method_return_self(
            xrt_array_fill_value(recv, arg0, arg1, XR_FROM_INT(a->length)));
    }
    if (recv.tag == XR_TAG_SYS_CONDVAR)
        return xrt_sys_condvar_method_2(recv, sym, arg0, arg1);
    if (XR_IS_MAP(recv) && sym == XRT_SYM_SET) {
        xrt_map_t *m = (xrt_map_t *) recv.ptr;
        xrt_map_set(m, arg0, arg1);
        return XR_NULL_VAL;
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_method_3(XrValue recv, int sym, XrValue arg0, XrValue arg1,
                                   XrValue arg2) {
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_FILL)
        return xrt_method_return_self(xrt_array_fill_value(recv, arg0, arg1, arg2));
    return XR_NULL_VAL;
}

static inline XrValue xrt_method_4(XrValue recv, int sym, XrValue arg0, XrValue arg1, XrValue arg2,
                                   XrValue arg3) {
    return XR_NULL_VAL;
}

/* Statement-position dispatch: run the method for its effects and drop the
 * result.
 *
 * Generated C emits these when a call's Xi result has no consumer, in place of
 * evaluating xrt_method_N into a value it never reads.  Xi cannot drop the
 * result itself: xi_arc runs before backend lowering and is shared with the VM,
 * whose builtin methods answer borrowed where these answer owned, so it treats
 * every call result as alias-uncertain and never inserts an unconsumed drop.
 * Releasing here is what lets the arms above retain uniformly instead of
 * choosing +0 or +1 per arm to suit the caller.
 *
 * A discarded call must still run — an arm can mutate the receiver, throw, or
 * set the pending error — so the dispatch is unconditional and only the release
 * is gated. */
static inline XrValue xrt_method_discard_0(XrValue recv, int sym) {
    XrValue result = xrt_method_0(recv, sym);
    return xrt_method_result_is_owned(sym) ? xrt_discard_owned(result) : XR_NULL_VAL;
}

static inline XrValue xrt_method_discard_1(XrValue recv, int sym, XrValue arg0) {
    XrValue result = xrt_method_1(recv, sym, arg0);
    return xrt_method_result_is_owned(sym) ? xrt_discard_owned(result) : XR_NULL_VAL;
}

static inline XrValue xrt_method_discard_2(XrValue recv, int sym, XrValue arg0, XrValue arg1) {
    XrValue result = xrt_method_2(recv, sym, arg0, arg1);
    return xrt_method_result_is_owned(sym) ? xrt_discard_owned(result) : XR_NULL_VAL;
}

static inline XrValue xrt_method_discard_3(XrValue recv, int sym, XrValue arg0, XrValue arg1,
                                           XrValue arg2) {
    XrValue result = xrt_method_3(recv, sym, arg0, arg1, arg2);
    return xrt_method_result_is_owned(sym) ? xrt_discard_owned(result) : XR_NULL_VAL;
}

static inline XrValue xrt_method_discard_4(XrValue recv, int sym, XrValue arg0, XrValue arg1,
                                           XrValue arg2, XrValue arg3) {
    XrValue result = xrt_method_4(recv, sym, arg0, arg1, arg2, arg3);
    return xrt_method_result_is_owned(sym) ? xrt_discard_owned(result) : XR_NULL_VAL;
}

#include "xrt_getprop.inc.c"

#endif  // XRT_METHOD_H
