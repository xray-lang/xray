/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_value.h - AOT value representation (self-contained for standalone AOT)
 *
 * Unified with runtime/value/xvalue.h:
 *   - Same struct layout (16B, tag@0, payload@8)
 *   - Same tag namespace (XR_TAG_*)
 *   - Same boxing API (XR_FROM_* / XR_TO_*)
 *   - Same truthiness semantics
 *
 * AOT-specific extensions (tags >= 8) encode object types without object headers.
 */

#ifndef XRT_VALUE_H
#define XRT_VALUE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>
#include <stdarg.h>
#include <math.h>
#include "../shared/xr_atomic_compat.h"
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "../shared/xr_hash_core.h"
#include "../shared/xr_float_fmt.h"
#include "../shared/xr_native_type_core.h"
#include "../shared/xr_numeric_core.h"
#include "../shared/xr_numeric_conversion_core.h"
#include "../shared/xr_obj_header.h" /* XrObjType ids shared with the VM */
#include "../shared/xr_truthy_core.h"
#include "../shared/xr_compare_core.h" /* canonical equality and order relations */
#define xrt_compare_route(kind, left_class, right_class)                                           \
    XR_COMPARE_OWNER_ROUTE(XR_SEM_OWNER_ID_SHARED_COMPARE_HI, XR_SEM_OWNER_ID_SHARED_COMPARE_LO,   \
                           XR_SEM_CONSUMER_AOT_HOSTED, (kind), (left_class), (right_class))
#define xrt_compare_i64(kind, a, b)                                                                \
    XR_COMPARE_OWNER_APPLY_I64(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                                  \
                               XR_SEM_OWNER_ID_SHARED_COMPARE_LO, XR_SEM_CONSUMER_AOT_HOSTED,      \
                               (kind), (a), (b))
#define xrt_compare_u64(kind, a, b)                                                                \
    XR_COMPARE_OWNER_APPLY_U64(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                                  \
                               XR_SEM_OWNER_ID_SHARED_COMPARE_LO, XR_SEM_CONSUMER_AOT_HOSTED,      \
                               (kind), (a), (b))
#define xrt_compare_f64(kind, a, b)                                                                \
    XR_COMPARE_OWNER_APPLY_F64(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                                  \
                               XR_SEM_OWNER_ID_SHARED_COMPARE_LO, XR_SEM_CONSUMER_AOT_HOSTED,      \
                               (kind), (a), (b))
#define xrt_compare_ptr(kind, a, b)                                                                \
    XR_COMPARE_OWNER_APPLY_PTR(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                                  \
                               XR_SEM_OWNER_ID_SHARED_COMPARE_LO, XR_SEM_CONSUMER_AOT_HOSTED,      \
                               (kind), (a), (b))
#define xrt_compare_ordering(kind, ordering)                                                       \
    XR_COMPARE_OWNER_APPLY_ORDERING(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                             \
                                    XR_SEM_OWNER_ID_SHARED_COMPARE_LO, XR_SEM_CONSUMER_AOT_HOSTED, \
                                    (kind), (ordering))
#define xrt_compare_equal(kind, equal)                                                             \
    XR_COMPARE_OWNER_APPLY_EQUAL(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                                \
                                 XR_SEM_OWNER_ID_SHARED_COMPARE_LO, XR_SEM_CONSUMER_AOT_HOSTED,    \
                                 (kind), (equal))
/* Spelled into generated C for a proven scalar comparison: the relation comes
 * from the owner while the operand type stays the one the plan chose. */
#define xrt_compare_native(relation, a, b)                                                         \
    XR_COMPARE_OWNER_APPLY_NATIVE(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                               \
                                  XR_SEM_OWNER_ID_SHARED_COMPARE_LO, XR_SEM_CONSUMER_AOT_HOSTED,   \
                                  relation, (a), (b))
#include "../runtime/abi/xr_runtime_string_object.h"

#if defined(__GNUC__) || defined(__clang__)
typedef uint8_t xr_v16u8 __attribute__((vector_size(16)));
typedef uint32_t xr_v4u32 __attribute__((vector_size(16)));
typedef uint64_t xr_v2u64 __attribute__((vector_size(16)));
#endif

/* =========================================================================
 * Branch expectation and code-layout hints.
 * Error / overflow / grow paths are annotated so the C compiler keeps the
 * hot path straight-line and moves cold blocks out of the way.
 * ========================================================================= */

/* FFI: assembler symbol name for an extern C function. The Mach-O toolchain
 * prefixes C symbols with an underscore; ELF/PE64 do not. Used as a GCC/clang
 * asm label so the generated alias binds to the real symbol without colliding
 * with libc prototypes or fortify macros (memcpy, memset, ...). Resolved by the
 * TARGET compiler so cross-compiled output stays correct. */
#if defined(__APPLE__)
#define XR_FFI_ASMNAME(s) "_" s
#else
#define XR_FFI_ASMNAME(s) s
#endif

#if defined(__GNUC__) || defined(__clang__)
#define XR_LIKELY(x) __builtin_expect(!!(x), 1)
#define XR_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define XRT_INTERNAL __attribute__((visibility("hidden")))
#define XRT_COLD __attribute__((cold))
#define XRT_NORETURN __attribute__((noreturn))
/* Alignment promise for the optimizer. Only assert alignments that the
 * allocation contract actually guarantees: array element buffers are
 * XRT_DATA_ALIGN (32B) aligned by xrt_coll.h's inline/stack rounding and by
 * XRT_ALLOC_ALIGNED after growth, and xrt_arc_alloc keeps object user data
 * 16-byte aligned (see xrt_arc.h). */
#define XR_ASSUME_ALIGNED(p, n) __builtin_assume_aligned((p), (n))
#define XR_ASSUME(x)                                                                               \
    do {                                                                                           \
        if (!(x))                                                                                  \
            __builtin_unreachable();                                                               \
    } while (0)
/* Function purity attributes, emitted only when the AOT prepare pass proved
 * the function effect-free (XaotFuncAttrPlan): CONST touches no memory,
 * PURE reads memory but never writes / throws / suspends. */
#define XRT_FN_CONST __attribute__((const))
#define XRT_FN_PURE __attribute__((pure))
#define XRT_ATTR_SECTION(name) __attribute__((section(name)))
#define XRT_ATTR_WEAK __attribute__((weak))
#define XRT_ATTR_USED __attribute__((used))
#define XRT_ATTR_NAKED __attribute__((naked))
#if defined(__x86_64__) || defined(__i386__)
#if defined(__clang__)
#define XRT_TARGET_AVX2 __attribute__((target("avx2"), __min_vector_width__(256), flatten))
#else
#define XRT_TARGET_AVX2 __attribute__((target("avx2"), flatten))
#endif
/* Clang before 19 rejects the newer evex512 feature name and ignores the
 * entire target attribute, so retain the AVX-512F island with the portable
 * feature spelling on those providers. */
#if defined(__clang__) && __clang_major__ >= 19
#define XRT_TARGET_AVX512                                                                          \
    __attribute__((target("avx512f,evex512"), __min_vector_width__(512), flatten))
#elif defined(__clang__)
#define XRT_TARGET_AVX512 __attribute__((target("avx512f"), __min_vector_width__(512), flatten))
#else
#define XRT_TARGET_AVX512 __attribute__((target("avx512f"), flatten))
#endif
#else
#define XRT_TARGET_AVX2
#define XRT_TARGET_AVX512
#endif
#if defined(__arm__) || defined(__thumb__)
#define XRT_ATTR_INTERRUPT(abi) __attribute__((interrupt(abi)))
#else
#define XRT_ATTR_INTERRUPT(abi) __attribute__((interrupt))
#endif
/* Emitted only when the AOT prepare pass proved the pointer unique over its
 * storage (XaotAliasPlan) — the Rust-noalias analogue for generated C. */
#define XRT_RESTRICT __restrict__
#else
#define XR_LIKELY(x) (x)
#define XR_UNLIKELY(x) (x)
#define XRT_INTERNAL
#define XRT_COLD
#if defined(_MSC_VER)
#define XRT_NORETURN __declspec(noreturn)
#else
#define XRT_NORETURN
#endif
#define XR_ASSUME_ALIGNED(p, n) (p)
#if defined(_MSC_VER)
#define XR_ASSUME(x) __assume(x)
#else
#define XR_ASSUME(x) ((void) 0)
#endif
#define XRT_FN_CONST
#define XRT_FN_PURE
#define XRT_ATTR_SECTION(name)
#define XRT_ATTR_WEAK
#define XRT_ATTR_USED
#define XRT_ATTR_NAKED
#define XRT_TARGET_AVX2
#define XRT_TARGET_AVX512
#define XRT_ATTR_INTERRUPT(abi)
#if defined(_MSC_VER)
#define XRT_RESTRICT __restrict
#else
#define XRT_RESTRICT
#endif
#endif

/* Runtime width query used only by the x86 --simd dispatch plan. Keep the
 * probe self-contained: compiler CPU builtins can introduce hidden libgcc
 * symbols (__cpu_model) which are unavailable in freestanding/Zig-musl links. */
static inline int xrt_target_runtime_simd_bytes(void) {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    static _Atomic(int) cached_bytes = 0;
    int cached = atomic_load_explicit(&cached_bytes, memory_order_relaxed);
    if (cached != 0)
        return cached;
    unsigned eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0u), "c"(0u));
    if (eax < 7u)
        goto baseline;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1u), "c"(0u));
    if ((ecx & (1u << 27)) == 0 || (ecx & (1u << 28)) == 0)
        goto baseline;
    unsigned xcr0_lo, xcr0_hi;
    __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0u));
    (void) xcr0_hi;
    if ((xcr0_lo & 6u) != 6u)
        goto baseline;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7u), "c"(0u));
    cached = (ebx & (1u << 5)) != 0 && (ebx & (1u << 16)) != 0 && (xcr0_lo & 0xe6u) == 0xe6u ? 64
             : (ebx & (1u << 5)) != 0                                                        ? 32
                                                                                             : 16;
    atomic_store_explicit(&cached_bytes, cached, memory_order_relaxed);
    return cached;
baseline:
    atomic_store_explicit(&cached_bytes, 16, memory_order_relaxed);
    return 16;
#elif (defined(_M_X64) || defined(_M_IX86)) && defined(_MSC_VER)
    int regs[4];
    __cpuid(regs, 1);
    unsigned __int64 xcr0 = _xgetbv(0);
    if ((regs[2] & (1 << 27)) == 0 || (regs[2] & (1 << 28)) == 0 || (xcr0 & 6) != 6)
        return 16;
    __cpuidex(regs, 7, 0);
    return (regs[1] & (1 << 5)) != 0 && (regs[1] & (1 << 16)) != 0 && (xcr0 & 0xe6) == 0xe6 ? 64
           : (regs[1] & (1 << 5)) != 0                                                      ? 32
                                                                                            : 16;
#else
    return 16;
#endif
}

/* =========================================================================
 * XrValue — 16 bytes, struct-of-unions, binary-compatible with VM XrValue.
 *
 * MEMORY LAYOUT:
 *   [0]    tag       uint8_t   XR_TAG_*
 *   [1]    flags     uint8_t   reserved = 0
 *   [2-3]  heap_type uint16_t  object subtype (PTR only)
 *   [4-7]  ext       uint32_t  reserved = 0
 *   [8-15] payload   union     int64 / double / pointer
 * ========================================================================= */

#include "xray_value_abi.h"

#define XRT_VALUE_FLAG_ARRAY_REF_OWNED 0x01u

/* =========================================================================
 * Tag constants — base tags (0-7) identical to VM's XrValueTag.
 * Extended tags (>= 8) are AOT-specific: encode object type without object header.
 * ========================================================================= */

#define XR_TAG_NULL 0     /* null singleton */
#define XR_TAG_BOOL 1     /* bool: payload 0=false, 1=true */
#define XR_TAG_RUNE 2     /* char: Unicode scalar value in .i */
#define XR_TAG_I64 3      /* integer (stored in .i as int64) */
#define XR_TAG_F64 4      /* float (stored in .f as double) */
#define XR_TAG_PTR 5      /* generic heap object pointer */
#define XR_TAG_AGG_REF 6  /* AOT native struct reference */
#define XR_TAG_NOTFOUND 7 /* sentinel: map lookup miss */

/* AOT extensions — object type encoded in tag (no object header available) */
#define XR_TAG_STR XR_RUNTIME_STRING_LITERAL_DYNAMIC_TAG
/* static / literal string view */
#define XR_TAG_ARRAY 15        /* AOT array */
#define XR_TAG_MAP 16          /* AOT map */
#define XR_TAG_STRBUF 17       /* AOT string builder */
#define XR_TAG_CLOSURE 18      /* AOT closure */
#define XR_TAG_STR_ARC 19      /* execution-arena ARC string */
#define XR_TAG_CELL 20         /* AOT mutable closure cell */
#define XR_TAG_TUPLE 21        /* AOT tuple */
#define XR_TAG_SET 22          /* AOT set */
#define XR_TAG_RANGE 23        /* AOT range */
#define XR_TAG_ENUM 24         /* AOT bridged enum key */
#define XR_TAG_ITERATOR 25     /* AOT map/set iterator (for-in over the iterator protocol) */
#define XR_TAG_REGEX 26        /* AOT compiled regex handle */
#define XR_TAG_SYS_MUTEX 28    /* AOT sys.Mutex OS-domain handle */
#define XR_TAG_SYS_RWLOCK 29   /* AOT sys.RwLock OS-domain handle */
#define XR_TAG_SYS_CONDVAR 30  /* AOT sys.Condvar OS-domain handle */
#define XR_TAG_SYS_BARRIER 31  /* AOT sys.Barrier OS-domain handle */
#define XR_TAG_SYS_ONCE 32     /* AOT sys.Once OS-domain handle */
#define XR_TAG_THREAD 33       /* AOT Thread<T> OS-thread handle */
#define XR_TAG_BUFFER 34       /* AOT mem.Buffer managed byte allocation */
#define XR_TAG_BIGINT 35       /* AOT static BigInt literal view */
#define XR_TAG_NET_CONN_STORAGE 36     /* AOT net.__NetConnStorage resource */
#define XR_TAG_NET_LISTENER_STORAGE 37 /* AOT net.__NetListenerStorage resource */
#define XR_TAG_TLS_CONTEXT_STORAGE 38  /* AOT net.__TlsContextStorage resource */

typedef struct XrAotEnumBox {
    XrObjHeader hdr;
    void *klass;
    const char *enum_name;
    const char *member_name;
    uint32_t member_index;
    uint32_t payload_count;
    uint32_t layout_id;
    XrValue payloads[];
} XrAotEnumBox;

/* Boxes a compiler-emitted static no-payload member: immortal, sticky, and
 * allocation-free, so a member constant never costs a heap object no matter
 * how often it is evaluated. This is the boxed-form counterpart of
 * xrt_enum_scalar_box for enums that keep XrAotEnumBox because a sibling
 * variant carries a payload. */
static inline XrValue xrt_enum_box_from_static(const XrAotEnumBox *box) {
    XrValue out = {0};
    out.tag = XR_TAG_ENUM;
    out.ext = box->member_index;
    out.ptr = (void *) box;
    return out;
}

/* One immutable sidecar per reachable unit enum is sufficient to box any
 * compact ordinal at an erased/tagged boundary.  XrValue.ext carries the
 * declaration ordinal; ptr identifies this layout and supplies cold names.
 * Unlike XrAotEnumBox, this does not allocate one object per case or per loop
 * iteration. */
typedef struct XrAotEnumScalarLayout {
    XrObjHeader hdr;
    const char *enum_name;
    const char *const *member_names;
    uint32_t member_count;
    uint32_t layout_id;
} XrAotEnumScalarLayout;

static inline XrValue xrt_enum_scalar_box(const XrAotEnumScalarLayout *layout, int64_t ordinal) {
    if (!layout || ordinal < 0 || (uint64_t) ordinal >= layout->member_count)
        return (XrValue) {0};
    XrValue out = {0};
    out.tag = XR_TAG_ENUM;
    out.ext = (uint32_t) ordinal;
    out.ptr = (void *) layout;
    return out;
}

typedef struct XrAotErasedEnumDescriptor {
    uint32_t layout_id;
    uint8_t metadata_kind;
    uint8_t _reserved[3];
    int64_t scalar;
} XrAotErasedEnumDescriptor;

typedef struct XrAotRuntimeEnumCtorView {
    XrObjHeader hdr;
    const char *enum_name;
    const char *member_name;
    uint32_t member_index;
    uint32_t layout_id;
} XrAotRuntimeEnumCtorView;

#define XR_AOT_ENUM_AGG_PAYLOAD_CAP 16u

typedef struct XrAotEnumAggregate {
    const char *enum_name;
    const char *member_name;
    int64_t tag;
    uint32_t payload_count;
    uint32_t layout_id;
    XrValue payloads[XR_AOT_ENUM_AGG_PAYLOAD_CAP];
} XrAotEnumAggregate;

static inline int xrt_enum_key_parts(XrValue v, const char **enum_name, const char **member_name,
                                     uint32_t *member_index, uint32_t *layout_id) {
    if (v.tag != XR_TAG_ENUM || !v.ptr)
        return 0;

    const XrObjHeader *hdr = (const XrObjHeader *) v.ptr;
    if (hdr->type == XR_TENUM_SCALAR_LAYOUT) {
        const XrAotEnumScalarLayout *layout = (const XrAotEnumScalarLayout *) v.ptr;
        uint32_t index = v.ext;
        if (index >= layout->member_count)
            return 0;
        if (enum_name)
            *enum_name = layout->enum_name;
        if (member_name)
            *member_name = layout->member_names ? layout->member_names[index] : NULL;
        if (member_index)
            *member_index = index;
        if (layout_id)
            *layout_id = layout->layout_id;
        return 1;
    }
    if (hdr->type == XR_TENUM_CTOR) {
        const XrAotRuntimeEnumCtorView *ctor = (const XrAotRuntimeEnumCtorView *) v.ptr;
        if (enum_name)
            *enum_name = ctor->enum_name;
        if (member_name)
            *member_name = ctor->member_name;
        if (member_index)
            *member_index = ctor->member_index;
        if (layout_id)
            *layout_id = ctor->layout_id;
        return 1;
    }

    const XrAotEnumBox *ev = (const XrAotEnumBox *) v.ptr;
    if (enum_name)
        *enum_name = ev->enum_name;
    if (member_name)
        *member_name = ev->member_name;
    if (member_index)
        *member_index = ev->member_index;
    if (layout_id)
        *layout_id = ev->layout_id;
    return 1;
}

static inline uint32_t xrt_enum_value_layout_id(XrValue v) {
    uint32_t layout_id = 0;
    (void) xrt_enum_key_parts(v, NULL, NULL, NULL, &layout_id);
    return layout_id;
}

static inline const char *xrt_enum_to_cstr(XrValue v, char *buf, size_t bufsz) {
    const char *enum_name = NULL;
    const char *member_name = NULL;
    if (xrt_enum_key_parts(v, &enum_name, &member_name, NULL, NULL) && enum_name && member_name) {
        snprintf(buf, bufsz, "%s.%s", enum_name, member_name);
        return buf;
    }
    snprintf(buf, bufsz, "<enum@%p>", v.ptr);
    return buf;
}

static inline int xrt_cstr_eq(const char *a, const char *b) {
    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    return strcmp(a, b) == 0;
}

static inline int xrt_enum_key_eq(XrValue a, XrValue b) {
    if (a.ptr == b.ptr && a.ext == b.ext)
        return 1;
    if (a.ext != b.ext || !a.ptr || !b.ptr)
        return 0;
    const char *enum_a = NULL;
    const char *member_a = NULL;
    const char *enum_b = NULL;
    const char *member_b = NULL;
    uint32_t index_a = 0;
    uint32_t index_b = 0;
    uint32_t layout_a = 0;
    uint32_t layout_b = 0;
    if (!xrt_enum_key_parts(a, &enum_a, &member_a, &index_a, &layout_a) ||
        !xrt_enum_key_parts(b, &enum_b, &member_b, &index_b, &layout_b))
        return 0;
    if (index_a != index_b)
        return 0;
    if (layout_a != 0 && layout_b != 0)
        return layout_a == layout_b;
    return xrt_cstr_eq(enum_a, enum_b) && xrt_cstr_eq(member_a, member_b);
}

/* Payload view of an enum value. Scalar-layout ordinals and ctor views never
 * carry payloads, so they yield NULL; everything else is an XrAotEnumBox. */
static inline const XrAotEnumBox *xrt_enum_payload_box(XrValue v) {
    if (v.tag != XR_TAG_ENUM || !v.ptr)
        return NULL;
    const XrObjHeader *hdr = (const XrObjHeader *) v.ptr;
    if (hdr->type == XR_TENUM_SCALAR_LAYOUT || hdr->type == XR_TENUM_CTOR)
        return NULL;
    return (const XrAotEnumBox *) v.ptr;
}

/* Native field tags mirror XrNativeType for standalone generated C. */
#define XR_NATIVE_I64 0
#define XR_NATIVE_F64 1
#define XR_NATIVE_BOOL 2
#define XR_NATIVE_I8 3
#define XR_NATIVE_I16 4
#define XR_NATIVE_I32 5
#define XR_NATIVE_U8 6
#define XR_NATIVE_U16 7
#define XR_NATIVE_U32 8
#define XR_NATIVE_U64 9
#define XR_NATIVE_F32 10
#define XR_NATIVE_NESTED_AGGREGATE 11
#define XR_NATIVE_ARRAY 12
#define XR_NATIVE_STRING 13
#define XR_NATIVE_ARRAY_REF 14
#define XR_NATIVE_MAP_REF 15
#define XR_NATIVE_SET_REF 16
#define XR_NATIVE_VALUE 17
#define XR_NATIVE_ISIZE 18
#define XR_NATIVE_USIZE 19
#define XR_NATIVE_POINTER 20

/* String type check (literal or execution-arena ARC allocation) */
#define XR_IS_STR(v) ((v).tag == XR_TAG_STR || (v).tag == XR_TAG_STR_ARC)

/* Header-bearing container type checks. These containers box as a tagged
 * pointer carrying the XrObjType id in heap_type, identical to the VM, so the
 * same predicate works on both backends. */
#define XR_IS_ARRAY(v) ((v).tag == XR_TAG_PTR && (v).heap_type == XR_TARRAY)
#define XR_IS_MAP(v) ((v).tag == XR_TAG_PTR && (v).heap_type == XR_TMAP)
#define XR_IS_SET(v) ((v).tag == XR_TAG_PTR && (v).heap_type == XR_TSET)
#define XR_IS_BIGINT(v) ((v).tag == XR_TAG_BIGINT)

/* =========================================================================
 * String object — every AOT string value points at an xrt_str_t header.
 *
 * XR_TAG_STR marks compiler-interned literals: the header is static const
 * data with the content hash precomputed at C generation time.
 * XR_TAG_STR_ARC marks materialized runtime objects. Those values use the
 * canonical XrString header-first inline-tail contract shared with the VM.
 *
 * Length is O(1); `hash` caches the content hash for map keys and
 * equality short-circuits (0 = not computed yet; real hashes are never 0).
 * UTF-8 payloads stay NUL-terminated so C interop (`xr_str_data`) remains free.
 * ========================================================================= */

#define XRT_STR_LITERAL XR_RUNTIME_STRING_LITERAL_FLAG

typedef XrRuntimeStringLiteralView xrt_str_t;

static inline xrt_str_t *xr_str_literal_header(XrValue v) {
    return v.tag == XR_TAG_STR ? (xrt_str_t *) v.ptr : NULL;
}

static inline const char *xr_str_data(XrValue v) {
    return v.tag == XR_TAG_STR_ARC ? ((const XrString *) v.ptr)->data
                                   : ((const xrt_str_t *) v.ptr)->data;
}

/* Writable bytes of a freshly allocated (not yet shared) string. */
static inline char *xr_str_buf(XrValue v) {
    return v.tag == XR_TAG_STR_ARC ? ((XrString *) v.ptr)->data : ((xrt_str_t *) v.ptr)->data;
}

static inline int64_t xr_str_len(XrValue v) {
    return v.tag == XR_TAG_STR_ARC ? (int64_t) ((const XrString *) v.ptr)->length
                                   : ((const xrt_str_t *) v.ptr)->len;
}

static inline void xr_str_set_len(XrValue v, uint32_t length) {
    if (v.tag == XR_TAG_STR_ARC && v.ptr)
        ((XrString *) v.ptr)->length = length;
}

static inline void xr_str_set_rune_len(XrValue v, uint32_t rune_length) {
    if (v.tag == XR_TAG_STR_ARC && v.ptr)
        ((XrString *) v.ptr)->rune_length = rune_length;
}

static inline int64_t xr_str_rune_len(XrValue v) {
    if (v.tag == XR_TAG_STR_ARC) {
        XrString *string = (XrString *) v.ptr;
        if (!string || string->length == 0)
            return 0;
        if (string->rune_length != UINT32_MAX)
            return (int64_t) string->rune_length;
        const unsigned char *p = (const unsigned char *) string->data;
        const unsigned char *end = p + string->length;
        uint32_t count = 0;
        while (p < end) {
            unsigned char b = *p;
            ptrdiff_t width = (b < 0x80u)              ? 1
                              : ((b & 0xE0u) == 0xC0u) ? 2
                              : ((b & 0xF0u) == 0xE0u) ? 3
                              : ((b & 0xF8u) == 0xF0u) ? 4
                                                       : 1;
            p += width <= end - p ? width : 1;
            count++;
        }
        string->rune_length = count;
        return (int64_t) count;
    }
    xrt_str_t *h = (xrt_str_t *) v.ptr;
    if (!h || !h->data || h->len <= 0)
        return 0;
    if (h->rune_len >= 0)
        return h->rune_len;
    const unsigned char *p = (const unsigned char *) h->data;
    const unsigned char *end = p + h->len;
    int64_t count = 0;
    while (p < end) {
        unsigned char b = *p;
        int64_t width = (b < 0x80u)              ? 1
                        : ((b & 0xE0u) == 0xC0u) ? 2
                        : ((b & 0xF0u) == 0xE0u) ? 3
                        : ((b & 0xF8u) == 0xF0u) ? 4
                                                 : 1;
        p += width <= end - p ? width : 1;
        count++;
    }
    if (!(h->flags & XRT_STR_LITERAL))
        h->rune_len = count;
    return count;
}

/* Wrap a static literal header into a value. */
static inline XrValue xr_str_lit(const xrt_str_t *hdr) {
    XrValue r = {0};
    r.tag = XR_TAG_STR;
    r.ptr = (void *) hdr;
    return r;
}

static inline XrValue xr_str_value_from_ptr(void *ptr) {
    if (!ptr)
        return (XrValue) {.tag = XR_TAG_NULL};
    XrValue r = {0};
    const XrRuntimeObjectHeader *header = (const XrRuntimeObjectHeader *) ptr;
    /* Valid literal views are capped at uint32 length, so bytes 4-5 of their
     * int64 length are zero. A materialized string stores the nonzero canonical
     * object-kind encoding there. Raw frame reconstruction is therefore
     * unambiguous without a legacy tag or an allocation-address heuristic. */
    r.tag = header->object_kind == XR_RUNTIME_OBJECT_KIND_STRING &&
                    header->flags == XR_RUNTIME_OBJECT_FLAG_NONE &&
                    header->layout_id == XR_RUNTIME_STRING_LAYOUT_INDEX &&
                    header->domain_id < XR_RUNTIME_STRING_DOMAIN_COUNT
                ? XR_TAG_STR_ARC
                : XR_TAG_STR;
    r.ptr = ptr;
    return r;
}

/* Define a static literal header for a C string literal.  hash stays 0
 * (hand-written headers cannot precompute it); xrt_str_hash recomputes on
 * demand without caching into const storage. */
#define XRT_STR_LIT_DEF(name, s)                                                                   \
    static const xrt_str_t name = {(int64_t) sizeof(s) - 1, (int64_t) sizeof(s) - 1, 0,            \
                                   XRT_STR_LITERAL, (char *) (s)}

/* Content hash of a string value, cached in the header when writable.
 * The relaxed atomic store keeps concurrent lazy hashing well-defined:
 * every writer stores the same value. */
static inline uint32_t xrt_str_hash(XrValue v) {
    if (v.tag == XR_TAG_STR_ARC) {
        XrString *string = (XrString *) v.ptr;
        uint32_t cached = string->hash;
        if (cached)
            return cached;
        uint32_t computed = xr_hash_core_str_hash_bytes(string->data, string->length);
#if defined(_MSC_VER)
        _InterlockedExchange((volatile long *) &string->hash, (long) computed);
#else
        __atomic_store_n(&string->hash, computed, __ATOMIC_RELAXED);
#endif
        return computed;
    }
    xrt_str_t *h = (xrt_str_t *) v.ptr;
    uint32_t cached = h->hash;
    if (cached)
        return cached;
    uint32_t computed = xr_hash_core_str_hash_bytes(h->data, (size_t) h->len);
    if (!(h->flags & XRT_STR_LITERAL)) {
#if defined(_MSC_VER)
        _InterlockedExchange((volatile long *) &h->hash, (long) computed);
#else
        __atomic_store_n(&h->hash, computed, __ATOMIC_RELAXED);
#endif
    }
    return computed;
}

/* =========================================================================
 * Internal helpers — construct XrValue with explicit tag
 * ========================================================================= */

/* Box a heap pointer as a tagged value with an explicit object subtype.
 * tag is XR_TAG_PTR; heap_type carries the XrObjType id, exactly like the VM.
 * This is the canonical form for the header-bearing container types so both
 * backends discriminate them identically (XR_IS_ARRAY/MAP/SET). */
static inline XrValue xr_mkheap(void *p, uint16_t heap_type) {
    XrValue r = {0};
    r.tag = XR_TAG_PTR;
    r.heap_type = heap_type;
    r.ptr = p;
    return r;
}

/* Box a heap pointer. The three header-bearing container selectors
 * (XR_TAG_ARRAY/MAP/SET) are normalized to the shared tagged-pointer form
 * (tag=PTR, heap_type=XR_T*) the VM also uses, so a boxed container is
 * discriminated identically on both backends; every other tag is stored
 * verbatim. The selector argument is a compile-time constant at all call
 * sites (runtime and generated C), so the switch folds away. */
static inline XrValue xr_mkptr(void *p, uint8_t tag) {
    switch (tag) {
        case XR_TAG_ARRAY:
            return xr_mkheap(p, XR_TARRAY);
        case XR_TAG_MAP:
            return xr_mkheap(p, XR_TMAP);
        case XR_TAG_SET:
            return xr_mkheap(p, XR_TSET);
        default:
            break;
    }
    XrValue r = {0};
    r.tag = tag;
    r.ptr = p;
    return r;
}

static inline XrValue xr_aggregate_ref(void *p, uint16_t storage_size) {
    XrValue r = {0};
    r.tag = XR_TAG_AGG_REF;
    r.heap_type = storage_size;
    r.ptr = p;
    return r;
}

/* Logical object-kind tag used by kind-dispatch switches. The header-bearing
 * containers box as XR_TAG_PTR + heap_type, so map them back to their
 * XR_TAG_ARRAY/MAP/SET kind; every other value's physical tag is already its
 * kind. This keeps kind-dispatch switches expressed with their case labels
 * while the physical representation matches the VM's tagged-pointer form. */
static inline uint8_t xrt_value_kind(XrValue v) {
    if (v.tag == XR_TAG_PTR) {
        switch (v.heap_type) {
            case XR_TARRAY:
                return XR_TAG_ARRAY;
            case XR_TMAP:
                return XR_TAG_MAP;
            case XR_TSET:
                return XR_TAG_SET;
            default:
                return XR_TAG_PTR;
        }
    }
    return v.tag;
}

#define XR_ARRAY_REF_MAX_COUNT UINT32_C(0x00FFFFFF)

static inline XrValue xr_array_ref(void *ptr, uint8_t elem_native_type, uint32_t elem_count) {
    XrValue r = {0};
    r.tag = XR_TAG_AGG_REF;
    r.ext = ((uint32_t) elem_count << 8) | elem_native_type;
    r.ptr = ptr;
    return r;
}

static inline XrValue xr_array_ref_owned(void *ptr, uint8_t elem_native_type, uint32_t elem_count) {
    XrValue r = xr_array_ref(ptr, elem_native_type, elem_count);
    r.flags = XRT_VALUE_FLAG_ARRAY_REF_OWNED;
    return r;
}

#define XR_IS_ARRAY_REF(v) ((v).tag == XR_TAG_AGG_REF && (v).ext != 0)
#define XR_ARRAY_REF_ELEM_TYPE(v) ((uint8_t) ((v).ext & 0xFF))
#define XR_ARRAY_REF_ELEM_COUNT(v) ((uint32_t) ((v).ext >> 8))

static inline XrValue xr_mkf64(double v, uint8_t tag) {
    XrValue r = {0};
    r.tag = tag;
    r.f = v;
    return r;
}

/* =========================================================================
 * Boxing / unboxing — XR_FROM_* / XR_TO_* (same API as VM's xvalue.h)
 * ========================================================================= */

#define XR_FROM_INT(x) ((XrValue) {.tag = XR_TAG_I64, .i = (int64_t) (x)})
#define XR_FROM_FLOAT(x) ((XrValue) {.tag = XR_TAG_F64, .f = (double) (x)})
#define XR_FROM_BOOL(x) ((XrValue) {.tag = XR_TAG_BOOL, .i = (x) ? 1 : 0})
#define XR_FROM_RUNE(cp) ((XrValue) {.tag = XR_TAG_RUNE, .i = (int64_t) (uint32_t) (cp)})
#define XR_NULL_VAL ((XrValue) {.tag = XR_TAG_NULL})
#define XR_TRUE_VAL ((XrValue) {.tag = XR_TAG_BOOL, .i = 1})
#define XR_FALSE_VAL ((XrValue) {.tag = XR_TAG_BOOL, .i = 0})

#define XR_TO_INT(v) ((v).i)
#define XR_TO_FLOAT(v) ((v).f)
#define XR_TO_BOOL(v) ((int) (v).i)
#define XR_TO_RUNE(v) ((uint32_t) (v).i)

typedef struct xrt_bigint_view_s {
    XrObjHeader hdr;
    void *klass;
    int8_t sign;
    uint8_t _pad1[3];
    uint32_t len;
    uint32_t cap;
    uint32_t _pad2;
    uint32_t limbs[];
} xrt_bigint_view_t;

static inline const xrt_bigint_view_t *xrt_bigint_view(XrValue v) {
    if (v.tag != XR_TAG_BIGINT && !(v.tag == XR_TAG_PTR && v.heap_type == XR_TINSTANCE))
        return NULL;
    return (const xrt_bigint_view_t *) v.ptr;
}

static inline int xrt_bigint_is_zero_value(XrValue v) {
    const xrt_bigint_view_t *b = xrt_bigint_view(v);
    return !b || b->len == 0 || (b->len == 1 && b->limbs[0] == 0);
}

static inline int64_t xrt_bigint_sign_value(XrValue v) {
    const xrt_bigint_view_t *b = xrt_bigint_view(v);
    if (!b || xrt_bigint_is_zero_value(v))
        return 0;
    return b->sign < 0 ? -1 : 1;
}

static inline int xrt_bigint_is_negative_value(XrValue v) {
    return xrt_bigint_sign_value(v) < 0;
}

static inline int xrt_bigint_is_positive_value(XrValue v) {
    return xrt_bigint_sign_value(v) > 0;
}

static inline XrValue xrt_bigint_to_int_value(XrValue v) {
    const xrt_bigint_view_t *b = xrt_bigint_view(v);
    if (!b || b->len == 0)
        return XR_FROM_INT(0);
    if (b->len > 2)
        return XR_NULL_VAL;
    uint64_t value = (uint64_t) b->limbs[0];
    if (b->len == 2)
        value |= ((uint64_t) b->limbs[1]) << 32;
    if (b->sign < 0) {
        if (value > (uint64_t) INT64_MAX + 1u)
            return XR_NULL_VAL;
        if (value == (uint64_t) INT64_MAX + 1u)
            return XR_FROM_INT(INT64_MIN);
        return XR_FROM_INT(-(int64_t) value);
    }
    if (value > (uint64_t) INT64_MAX)
        return XR_NULL_VAL;
    return XR_FROM_INT((int64_t) value);
}

static inline double xrt_bigint_to_float_value(XrValue v) {
    const xrt_bigint_view_t *b = xrt_bigint_view(v);
    if (!b || b->len == 0)
        return 0.0;
    double result = 0.0;
    double base = 1.0;
    for (uint32_t i = 0; i < b->len; i++) {
        result += (double) b->limbs[i] * base;
        base *= 4294967296.0;
    }
    return b->sign < 0 ? -result : result;
}

static inline int64_t xrt_bigint_eq_value(XrValue a, XrValue b) {
    const xrt_bigint_view_t *ba = xrt_bigint_view(a);
    const xrt_bigint_view_t *bb = xrt_bigint_view(b);
    if (ba == bb)
        return 1;
    if (!ba || !bb)
        return 0;
    int za = ba->len == 0 || (ba->len == 1 && ba->limbs[0] == 0);
    int zb = bb->len == 0 || (bb->len == 1 && bb->limbs[0] == 0);
    if (za || zb)
        return za == zb;
    if (ba->sign != bb->sign || ba->len != bb->len)
        return 0;
    return memcmp(ba->limbs, bb->limbs, (size_t) ba->len * sizeof(uint32_t)) == 0;
}

/* Signed compare of two BigInt values: -1, 0, or 1, mirroring the VM's
 * xr_bigint_cmp so ordered comparisons agree across backends. A zero operand is
 * treated as sign 0 (independent of its stored sign byte), then equal signs
 * compare magnitudes high limb to low. Inputs are the normalized limb views
 * produced for literals and computed results. */
static inline int xrt_bigint_cmp_value(XrValue av, XrValue bv) {
    const xrt_bigint_view_t *a = xrt_bigint_view(av);
    const xrt_bigint_view_t *b = xrt_bigint_view(bv);
    if (!a || !b)
        return 0;
    int za = a->len == 0 || (a->len == 1 && a->limbs[0] == 0);
    int zb = b->len == 0 || (b->len == 1 && b->limbs[0] == 0);
    int sa = za ? 0 : (a->sign < 0 ? -1 : 1);
    int sb = zb ? 0 : (b->sign < 0 ? -1 : 1);
    if (sa != sb)
        return sa < sb ? -1 : 1;
    if (sa == 0)
        return 0;
    int mag = 0;
    if (a->len != b->len) {
        mag = a->len > b->len ? 1 : -1;
    } else {
        for (int i = (int) a->len - 1; i >= 0; i--) {
            if (a->limbs[i] != b->limbs[i]) {
                mag = a->limbs[i] > b->limbs[i] ? 1 : -1;
                break;
            }
        }
    }
    return sa < 0 ? -mag : mag;
}

static inline uint64_t xrt_bigint_hash_value(XrValue v) {
    const xrt_bigint_view_t *b = xrt_bigint_view(v);
    if (!b)
        return xr_hash_core_mix_u64((uint64_t) (uintptr_t) v.ptr);
    if (b->len == 0 || (b->len == 1 && b->limbs[0] == 0))
        return xr_hash_core_mix_u64(0x424947494e540000ull);
    uint64_t h = xr_hash_core_bytes((const char *) b->limbs, (size_t) b->len * sizeof(uint32_t));
    h ^= xr_hash_core_mix_u64((uint64_t) (b->sign < 0 ? 0x9e3779b9u : 0x7f4a7c15u));
    return xr_hash_core_mix_u64(h);
}

static inline XrAotEnumAggregate xrt_enum_aggregate_zero(void) {
    XrAotEnumAggregate out = {0};
    for (uint32_t i = 0; i < XR_AOT_ENUM_AGG_PAYLOAD_CAP; i++)
        out.payloads[i] = XR_NULL_VAL;
    return out;
}

static inline XrAotEnumAggregate
xrt_enum_aggregate_make(uint32_t layout_id, int64_t tag, uint32_t payload_count,
                        const char *enum_name, const char *member_name, const XrValue *payloads) {
    XrAotEnumAggregate out = xrt_enum_aggregate_zero();
    out.enum_name = enum_name;
    out.member_name = member_name;
    out.tag = tag;
    out.payload_count = payload_count;
    out.layout_id = layout_id;
    uint32_t limit =
        payload_count < XR_AOT_ENUM_AGG_PAYLOAD_CAP ? payload_count : XR_AOT_ENUM_AGG_PAYLOAD_CAP;
    for (uint32_t i = 0; i < limit; i++)
        out.payloads[i] = payloads ? payloads[i] : XR_NULL_VAL;
    return out;
}

/* C compound-literal arrays and C++ temporary arrays have different address
 * rules.  Keep the payload alive for the complete make call in either mode. */
#if defined(__cplusplus)
#define XRT_ENUM_AGGREGATE_MAKE(layout_id, tag, payload_count, enum_name, member_name, ...)        \
    ([&]() {                                                                                       \
        const XrValue _xrt_enum_payloads[(payload_count)] = {__VA_ARGS__};                         \
        return xrt_enum_aggregate_make((layout_id), (tag), (payload_count), (enum_name),           \
                                       (member_name), _xrt_enum_payloads);                         \
    }())
#else
#define XRT_ENUM_AGGREGATE_MAKE(layout_id, tag, payload_count, enum_name, member_name, ...)        \
    xrt_enum_aggregate_make((layout_id), (tag), (payload_count), (enum_name), (member_name),       \
                            (const XrValue[(payload_count)]) {__VA_ARGS__})
#endif

/* Unpack a boxed enum XrValue (as produced by dynamic reads: getprop, index
 * load, map/json get) into an XrAotEnumAggregate so a typed enum aggregate can
 * be reconstructed via <Enum>_from_base(...). Inverse of xrt_enum_aggregate_box.
 * A non-enum/null value yields a zeroed aggregate (tag 0, no payloads). */
static inline XrAotEnumAggregate xrt_value_to_enum_aggregate(XrValue v) {
    XrAotEnumAggregate out = xrt_enum_aggregate_zero();
    if (v.tag != XR_TAG_ENUM || !v.ptr)
        return out;
    const XrObjHeader *hdr = (const XrObjHeader *) v.ptr;
    if (hdr->type == XR_TENUM_CTOR) {
        const XrAotRuntimeEnumCtorView *ctor = (const XrAotRuntimeEnumCtorView *) v.ptr;
        out.enum_name = ctor->enum_name;
        out.member_name = ctor->member_name;
        out.tag = (int64_t) ctor->member_index;
        out.payload_count = 0;
        out.layout_id = ctor->layout_id;
        return out;
    }
    const XrAotEnumBox *ev = (const XrAotEnumBox *) v.ptr;
    out.enum_name = ev->enum_name;
    out.member_name = ev->member_name;
    out.tag = (int64_t) ev->member_index;
    out.payload_count = ev->payload_count;
    out.layout_id = ev->layout_id;
    uint32_t limit = ev->payload_count < XR_AOT_ENUM_AGG_PAYLOAD_CAP ? ev->payload_count
                                                                     : XR_AOT_ENUM_AGG_PAYLOAD_CAP;
    for (uint32_t i = 0; i < limit; i++)
        out.payloads[i] = ev->payloads[i];
    return out;
}

static inline XRT_COLD _Noreturn void xrt_enum_aggregate_shape_fail(const char *what,
                                                                    const char *enum_name) {
    fprintf(stderr, "AOT enum aggregate shape mismatch: %s", what ? what : "unknown");
    if (enum_name && enum_name[0])
        fprintf(stderr, " for %s", enum_name);
    fprintf(stderr, "\n");
    abort();
}

static inline void xrt_enum_aggregate_check_layout(uint32_t actual_layout_id,
                                                   uint32_t expected_layout_id,
                                                   const char *enum_name) {
    if (actual_layout_id != 0 && expected_layout_id != 0 && actual_layout_id != expected_layout_id)
        xrt_enum_aggregate_shape_fail("layout id", enum_name);
}

static inline void xrt_enum_aggregate_check_payload_count(uint32_t layout_id, uint32_t actual,
                                                          uint32_t expected,
                                                          const char *enum_name) {
    if (layout_id != 0 && actual != expected)
        xrt_enum_aggregate_shape_fail("payload count", enum_name);
}

static inline void xrt_enum_aggregate_check_payload_type(uint32_t layout_id, int ok,
                                                         const char *enum_name) {
    if (layout_id != 0 && !ok)
        xrt_enum_aggregate_shape_fail("payload type", enum_name);
}

static inline void xrt_enum_aggregate_check_known_tag(uint32_t layout_id, const char *enum_name) {
    if (layout_id != 0)
        xrt_enum_aggregate_shape_fail("tag", enum_name);
}

static inline const char *xr_unbox_str(XrValue v) {
    return xr_str_data(v);
}

static inline size_t xrt_value_native_type_size(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
        case XR_NATIVE_BOOL:
            return 1;
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
            return 2;
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
        case XR_NATIVE_F32:
            return 4;
        case XR_NATIVE_ISIZE:
            return sizeof(ptrdiff_t);
        case XR_NATIVE_USIZE:
            return sizeof(size_t);
        case XR_NATIVE_POINTER:
            return sizeof(void *);
        case XR_NATIVE_VALUE:
            return sizeof(XrValue);
        default:
            return 8;
    }
}

/* =========================================================================
 * Value equality — single authoritative implementation for the AOT runtime.
 * Mirrors the VM's xr_value_eq semantics: strings compare by content
 * (XR_TAG_STR and XR_TAG_STR_ARC are interchangeable), numbers by value,
 * other heap objects by identity. This is the `==` operator relation, so it
 * is IEEE on floats and not reflexive on NaN.
 *
 * Container keying and membership use xrt_key_eq instead: identical except
 * that it is reflexive on NaN, which is what keeps a stored key reachable.
 * Both must track their VM counterparts (xr_value_eq / xr_value_key_eq).
 * ========================================================================= */

/* The hosted carrier rule: a tagged pair compares only within one tag class,
 * with STR_ARC normalized to STR so a literal and an allocated string meet. The
 * relation itself - what each class's lane answers - belongs to the shared
 * owner. */
static inline int64_t xrt_compare_tagged_equal(XrCompareKind kind, XrValue a, XrValue b) {
    uint32_t ta = (a.tag == XR_TAG_STR_ARC) ? XR_TAG_STR : a.tag;
    uint32_t tb = (b.tag == XR_TAG_STR_ARC) ? XR_TAG_STR : b.tag;
    if (ta != tb)
        return xrt_compare_equal(kind, false);
    if (ta == XR_TAG_ENUM)
        return xrt_compare_equal(kind, xrt_enum_key_eq(a, b) != 0);
    if (ta == XR_TAG_BIGINT)
        return xrt_compare_equal(kind, xrt_bigint_eq_value(a, b) != 0);
    if (ta == XR_TAG_I64 || ta == XR_TAG_BOOL || ta == XR_TAG_RUNE)
        return xrt_compare_i64(kind, a.i, b.i);
    if (ta == XR_TAG_F64)
        return xrt_compare_f64(kind, a.f, b.f);
    if (ta == XR_TAG_STR) {
        if (a.ptr == b.ptr)
            return xrt_compare_equal(kind, true);
        int64_t length = xr_str_len(a);
        if (length != xr_str_len(b))
            return xrt_compare_equal(kind, false);
        uint32_t hash_a = xrt_str_hash(a);
        uint32_t hash_b = xrt_str_hash(b);
        if (hash_a != hash_b)
            return xrt_compare_equal(kind, false);
        return xrt_compare_equal(kind,
                                 memcmp(xr_str_data(a), xr_str_data(b), (size_t) length) == 0);
    }
    if (ta == XR_TAG_AGG_REF) {
        if (a.ptr == b.ptr)
            return xrt_compare_equal(kind, true);
        if (!a.ptr || !b.ptr || a.ext != b.ext)
            return xrt_compare_equal(kind, false);
        if (XR_IS_ARRAY_REF(a)) {
            size_t size = xrt_value_native_type_size(XR_ARRAY_REF_ELEM_TYPE(a)) *
                          (size_t) XR_ARRAY_REF_ELEM_COUNT(a);
            return xrt_compare_equal(kind, memcmp(a.ptr, b.ptr, size) == 0);
        }
        if (a.heap_type != 0 || b.heap_type != 0) {
            if (a.heap_type == 0 || b.heap_type == 0 || a.heap_type != b.heap_type)
                return xrt_compare_equal(kind, false);
            return xrt_compare_equal(kind, memcmp(a.ptr, b.ptr, (size_t) a.heap_type) == 0);
        }
        uint32_t sa = *(uint32_t *) a.ptr;
        uint32_t sb = *(uint32_t *) b.ptr;
        if (sa == 0 || sb == 0 || sa != sb || sa > (16u * 1024u * 1024u))
            return xrt_compare_equal(kind, false);
        return xrt_compare_equal(kind, memcmp(a.ptr, b.ptr, (size_t) sa) == 0);
    }
    return xrt_compare_ptr(kind, a.ptr, b.ptr);
}

static inline int64_t xrt_eq(XrValue a, XrValue b) {
    return xrt_compare_tagged_equal(XR_COMPARE_EQ, a, b);
}

static inline int64_t xrt_key_eq(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_F64 && b.tag == XR_TAG_F64)
        return xr_hash_core_key_eq_f64(a.f, b.f);
    return xrt_eq(a, b);
}

/* =========================================================================
 * Type checks
 * ========================================================================= */

#define XR_IS_NULL(v) ((v).tag == XR_TAG_NULL)
#define XR_IS_BOOL(v) ((v).tag == XR_TAG_BOOL)
#define XR_IS_RUNE(v) ((v).tag == XR_TAG_RUNE)
#define XR_IS_INT(v) ((v).tag == XR_TAG_I64)
#define XR_IS_FLOAT(v) ((v).tag == XR_TAG_F64)
#define XR_IS_FALSE(v) ((v).tag == XR_TAG_BOOL && (v).i == 0)
#define XR_IS_NUM(v) (XR_IS_INT(v) || XR_IS_FLOAT(v))

/* Coerce any numeric/bool value to int64 (for typed array storage).
 * Non-numeric values return 0 in AOT context. */
static inline int64_t xr_value_to_int64_coerce(XrValue v) {
    if (XR_IS_INT(v))
        return v.i;
    if (XR_IS_FLOAT(v))
        return (int64_t) v.f;
    if (XR_IS_BOOL(v))
        return v.i;
    return 0;
}

/* Coerce any numeric/bool value to double (for typed array storage). */
static inline double xr_value_to_f64_coerce(XrValue v) {
    if (XR_IS_FLOAT(v))
        return v.f;
    if (XR_IS_INT(v))
        return (double) v.i;
    if (XR_IS_BOOL(v))
        return (double) v.i;
    return 0.0;
}

static inline double xrt_math_number(XrValue v) {
    if (XR_IS_INT(v))
        return (double) v.i;
    if (XR_IS_FLOAT(v))
        return v.f;
    return NAN;
}

static inline XrValue xrt_math_abs(XrValue v) {
    if (XR_IS_INT(v)) {
        int64_t i = v.i;
        if (i == INT64_MIN)
            return XR_FROM_FLOAT((double) INT64_MAX + 1.0);
        return XR_FROM_INT(i < 0 ? -i : i);
    }
    return XR_FROM_FLOAT(fabs(xrt_math_number(v)));
}

/* =========================================================================
 * String helpers
 * ========================================================================= */

static inline int xrt_format_float(char *buf, size_t bufsz, double value) {
    return xr_format_float(buf, bufsz, value);
}

static inline int xrt_rune_utf8_encode(uint32_t cp, char *buf) {
    if (!buf)
        return 0;
    if (cp <= 0x7Fu) {
        buf[0] = (char) cp;
        return 1;
    }
    if (cp <= 0x7FFu) {
        buf[0] = (char) (0xC0u | (cp >> 6));
        buf[1] = (char) (0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp <= 0xFFFFu) {
        if (cp >= 0xD800u && cp <= 0xDFFFu)
            return 0;
        buf[0] = (char) (0xE0u | (cp >> 12));
        buf[1] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
        buf[2] = (char) (0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        buf[0] = (char) (0xF0u | (cp >> 18));
        buf[1] = (char) (0x80u | ((cp >> 12) & 0x3Fu));
        buf[2] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
        buf[3] = (char) (0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;
}

/* Defined in xrt_range.h (L1, included after this header in xrt.h). Forward
 * declared so the low-level stringifier can render ranges as "start..end"
 * instead of a generic pointer placeholder, matching the VM. */
struct xrt_range_s;
static inline int xrt_range_format_buf(const struct xrt_range_s *r, char *buf, size_t cap);

static inline const char *xr_to_cstr(XrValue v, char *buf, size_t bufsz) {
    switch (v.tag) {
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
            return xr_str_data(v);
        case XR_TAG_I64:
            snprintf(buf, bufsz, "%lld", (long long) v.i);
            return buf;
        case XR_TAG_F64:
            xrt_format_float(buf, bufsz, v.f);
            return buf;
        case XR_TAG_BOOL:
            return v.i ? "true" : "false";
        case XR_TAG_RUNE: {
            int n = (bufsz > 0) ? xrt_rune_utf8_encode(XR_TO_RUNE(v), buf) : 0;
            if (bufsz > 0)
                buf[(n > 0 && (size_t) n < bufsz) ? n : 0] = '\0';
            return buf;
        }
        case XR_TAG_NULL:
            return "null";
        case XR_TAG_BIGINT: {
            XrValue i = xrt_bigint_to_int_value(v);
            if (i.tag == XR_TAG_I64) {
                snprintf(buf, bufsz, "%lld", (long long) i.i);
            } else {
                /* Full decimal for a value beyond int64 needs a growable buffer
                 * (xrt_bigint_format on a strbuf); print/toString take that
                 * path. This fixed-buffer helper only backs secondary cstr
                 * sites, so emit a marker rather than a raw pointer address. */
                snprintf(buf, bufsz, "<BigInt>");
            }
            return buf;
        }
        case XR_TAG_ENUM:
            return xrt_enum_to_cstr(v, buf, bufsz);
        case XR_TAG_RANGE:
            if (v.ptr)
                xrt_range_format_buf((const struct xrt_range_s *) v.ptr, buf, bufsz);
            else
                snprintf(buf, bufsz, "<Range>");
            return buf;
        default:
            snprintf(buf, bufsz, "<object@%p>", v.ptr);
            return buf;
    }
}

/* =========================================================================
 * Truthiness (assert/debug helpers: null, false, 0, 0.0 are falsy)
 * ========================================================================= */

static inline int xr_truthy(XrValue v) {
    XrTruthyCoreKind kind = XR_TRUTHY_CORE_OBJECT;
    int64_t integer = 0;
    double floating = 0.0;
    if (XR_IS_NULL(v)) {
        kind = XR_TRUTHY_CORE_NULL;
    } else if (XR_IS_BOOL(v)) {
        kind = XR_TRUTHY_CORE_BOOL;
        integer = XR_TO_BOOL(v);
    } else if (XR_IS_INT(v)) {
        kind = XR_TRUTHY_CORE_INT;
        integer = XR_TO_INT(v);
    } else if (XR_IS_FLOAT(v)) {
        kind = XR_TRUTHY_CORE_FLOAT;
        floating = XR_TO_FLOAT(v);
    }
    return xr_truthy_core_eval(XR_SEM_OWNER_ID_SHARED_TRUTHINESS_HI,
                               XR_SEM_OWNER_ID_SHARED_TRUTHINESS_LO, XR_SEM_CONSUMER_AOT_HOSTED,
                               kind, integer, floating, 0);
}

/* =========================================================================
 * Runtime context — opaque handle passed to all AOT functions.
 * Points to XrCoroutine* internally; AOT code never dereferences it.
 * ========================================================================= */

typedef void *XrtContext;

#endif  // XRT_VALUE_H
