/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_sort_core.h - Runtime-neutral Array sort core.
 */

#ifndef XR_SORT_CORE_H
#define XR_SORT_CORE_H

#include "xr_elem_type.h"
#include <stdint.h>
#include <string.h>

typedef int (*XrSortCoreCmpFn)(void *ctx, XrValue a, XrValue b);

static inline int xr_sort_core_compare_result(XrValue result) {
    if (XR_IS_INT(result))
        return (XR_TO_INT(result) > 0) - (XR_TO_INT(result) < 0);
    if (XR_IS_FLOAT(result))
        return (XR_TO_FLOAT(result) > 0.0) - (XR_TO_FLOAT(result) < 0.0);
    return 0;
}

static inline int xr_sort_core_compare_default(XrValue a, XrValue b, const char *a_str,
                                               int64_t a_len, const char *b_str, int64_t b_len) {
    if (XR_IS_INT(a) && XR_IS_INT(b))
        return (XR_TO_INT(a) > XR_TO_INT(b)) - (XR_TO_INT(a) < XR_TO_INT(b));
    if (XR_IS_FLOAT(a) && XR_IS_FLOAT(b))
        return (XR_TO_FLOAT(a) > XR_TO_FLOAT(b)) - (XR_TO_FLOAT(a) < XR_TO_FLOAT(b));
    if (XR_IS_INT(a) && XR_IS_FLOAT(b)) {
        double fa = (double) XR_TO_INT(a);
        return (fa > XR_TO_FLOAT(b)) - (fa < XR_TO_FLOAT(b));
    }
    if (XR_IS_FLOAT(a) && XR_IS_INT(b)) {
        double fb = (double) XR_TO_INT(b);
        return (XR_TO_FLOAT(a) > fb) - (XR_TO_FLOAT(a) < fb);
    }
    if (a_str && b_str) {
        int64_t minlen = a_len < b_len ? a_len : b_len;
        int cmp = memcmp(a_str, b_str, (size_t) minlen);
        if (cmp != 0)
            return cmp;
        return (a_len > b_len) - (a_len < b_len);
    }
    return 0;
}

#define XR_SORT_CORE_SMALL 24

#define XR_SORT_CORE_DEF(SFX, T, LESS)                                                             \
    static inline void xr_sort_core_heap_sift_##SFX(T *a, int64_t root, int64_t end) {             \
        while (root * 2 + 1 <= end) {                                                              \
            int64_t child = root * 2 + 1;                                                          \
            if (child + 1 <= end && LESS(a[child], a[child + 1]))                                  \
                child++;                                                                           \
            if (!LESS(a[root], a[child]))                                                          \
                return;                                                                            \
            T t = a[root];                                                                         \
            a[root] = a[child];                                                                    \
            a[child] = t;                                                                          \
            root = child;                                                                          \
        }                                                                                          \
    }                                                                                              \
    static inline void xr_sort_core_heapsort_##SFX(T *a, int64_t n) {                              \
        for (int64_t s = (n - 2) / 2; s >= 0; s--)                                                 \
            xr_sort_core_heap_sift_##SFX(a, s, n - 1);                                             \
        for (int64_t e = n - 1; e > 0; e--) {                                                      \
            T t = a[e];                                                                            \
            a[e] = a[0];                                                                           \
            a[0] = t;                                                                              \
            xr_sort_core_heap_sift_##SFX(a, 0, e - 1);                                             \
        }                                                                                          \
    }                                                                                              \
    static inline void xr_sort_core_isort_##SFX(T *a, int64_t n) {                                 \
        for (int64_t i = 1; i < n; i++) {                                                          \
            T key = a[i];                                                                          \
            int64_t j = i - 1;                                                                     \
            while (j >= 0 && LESS(key, a[j])) {                                                    \
                a[j + 1] = a[j];                                                                   \
                j--;                                                                               \
            }                                                                                      \
            a[j + 1] = key;                                                                        \
        }                                                                                          \
    }                                                                                              \
    static inline int64_t xr_sort_core_med3_##SFX(T *a, int64_t x, int64_t y, int64_t z) {         \
        if (LESS(a[y], a[x])) {                                                                    \
            if (LESS(a[z], a[y]))                                                                  \
                return y;                                                                          \
            return LESS(a[z], a[x]) ? z : x;                                                       \
        }                                                                                          \
        if (LESS(a[z], a[y]))                                                                      \
            return LESS(a[z], a[x]) ? x : z;                                                       \
        return y;                                                                                  \
    }                                                                                              \
    static inline void xr_sort_core_introsort_##SFX(T *a, int64_t n, int depth) {                  \
        while (n > XR_SORT_CORE_SMALL) {                                                           \
            if (depth-- == 0) {                                                                    \
                xr_sort_core_heapsort_##SFX(a, n);                                                 \
                return;                                                                            \
            }                                                                                      \
            int64_t mid = n / 2;                                                                   \
            int64_t pi;                                                                            \
            if (n > 128) {                                                                         \
                int64_t step = n / 8;                                                              \
                int64_t m1 = xr_sort_core_med3_##SFX(a, 0, step, step * 2);                        \
                int64_t m2 = xr_sort_core_med3_##SFX(a, mid - step, mid, mid + step);              \
                int64_t m3 = xr_sort_core_med3_##SFX(a, n - 1 - step * 2, n - 1 - step, n - 1);    \
                pi = xr_sort_core_med3_##SFX(a, m1, m2, m3);                                       \
            } else {                                                                               \
                pi = xr_sort_core_med3_##SFX(a, 0, mid, n - 1);                                    \
            }                                                                                      \
            T pv = a[pi];                                                                          \
            int64_t lt = 0;                                                                        \
            int64_t i = 0;                                                                         \
            int64_t gt = n;                                                                        \
            while (i < gt) {                                                                       \
                if (LESS(a[i], pv)) {                                                              \
                    T t = a[lt];                                                                   \
                    a[lt] = a[i];                                                                  \
                    a[i] = t;                                                                      \
                    lt++;                                                                          \
                    i++;                                                                           \
                } else if (LESS(pv, a[i])) {                                                       \
                    gt--;                                                                          \
                    T t = a[i];                                                                    \
                    a[i] = a[gt];                                                                  \
                    a[gt] = t;                                                                     \
                } else {                                                                           \
                    i++;                                                                           \
                }                                                                                  \
            }                                                                                      \
            if (lt < n - gt) {                                                                     \
                xr_sort_core_introsort_##SFX(a, lt, depth);                                        \
                a += gt;                                                                           \
                n -= gt;                                                                           \
            } else {                                                                               \
                xr_sort_core_introsort_##SFX(a + gt, n - gt, depth);                               \
                n = lt;                                                                            \
            }                                                                                      \
        }                                                                                          \
        xr_sort_core_isort_##SFX(a, n);                                                            \
    }                                                                                              \
    static inline void xr_sort_core_##SFX(T *a, int64_t n) {                                       \
        if (!a || n < 2)                                                                           \
            return;                                                                                \
        int depth = 0;                                                                             \
        for (int64_t m = n; m > 0; m >>= 1)                                                        \
            depth += 2;                                                                            \
        xr_sort_core_introsort_##SFX(a, n, depth);                                                 \
    }

#define XR_SORT_CORE_LT(x, y) ((x) < (y))
XR_SORT_CORE_DEF(i8, int8_t, XR_SORT_CORE_LT)
XR_SORT_CORE_DEF(u8, uint8_t, XR_SORT_CORE_LT)
XR_SORT_CORE_DEF(i16, int16_t, XR_SORT_CORE_LT)
XR_SORT_CORE_DEF(u16, uint16_t, XR_SORT_CORE_LT)
XR_SORT_CORE_DEF(i32, int32_t, XR_SORT_CORE_LT)
XR_SORT_CORE_DEF(u32, uint32_t, XR_SORT_CORE_LT)
XR_SORT_CORE_DEF(i64, int64_t, XR_SORT_CORE_LT)
XR_SORT_CORE_DEF(u64, uint64_t, XR_SORT_CORE_LT)
XR_SORT_CORE_DEF(f32, float, XR_SORT_CORE_LT)
XR_SORT_CORE_DEF(f64, double, XR_SORT_CORE_LT)

static inline int xr_sort_core_typed(void *data, int64_t length, uint8_t elem_type) {
    if (!data || length < 2)
        return 1;
    switch (elem_type) {
        case XR_ELEM_I8:
            xr_sort_core_i8((int8_t *) data, length);
            return 1;
        case XR_ELEM_U8:
        case XR_ELEM_BOOL:
            xr_sort_core_u8((uint8_t *) data, length);
            return 1;
        case XR_ELEM_I16:
            xr_sort_core_i16((int16_t *) data, length);
            return 1;
        case XR_ELEM_U16:
            xr_sort_core_u16((uint16_t *) data, length);
            return 1;
        case XR_ELEM_I32:
            xr_sort_core_i32((int32_t *) data, length);
            return 1;
        case XR_ELEM_U32:
            xr_sort_core_u32((uint32_t *) data, length);
            return 1;
        case XR_ELEM_I64:
            xr_sort_core_i64((int64_t *) data, length);
            return 1;
        case XR_ELEM_U64:
            xr_sort_core_u64((uint64_t *) data, length);
            return 1;
        case XR_ELEM_F32:
            xr_sort_core_f32((float *) data, length);
            return 1;
        case XR_ELEM_F64:
            xr_sort_core_f64((double *) data, length);
            return 1;
        default:
            return 0;
    }
}

static inline int xr_sort_core_vless(XrValue a, XrValue b, XrSortCoreCmpFn cmp, void *ctx) {
    return cmp && cmp(ctx, a, b) < 0;
}

static inline void xr_sort_core_vheap_sift(XrValue *a, int64_t root, int64_t end,
                                           XrSortCoreCmpFn cmp, void *ctx) {
    while (root * 2 + 1 <= end) {
        int64_t child = root * 2 + 1;
        if (child + 1 <= end && xr_sort_core_vless(a[child], a[child + 1], cmp, ctx))
            child++;
        if (!xr_sort_core_vless(a[root], a[child], cmp, ctx))
            return;
        XrValue t = a[root];
        a[root] = a[child];
        a[child] = t;
        root = child;
    }
}

static inline void xr_sort_core_vheapsort(XrValue *a, int64_t n, XrSortCoreCmpFn cmp, void *ctx) {
    for (int64_t s = (n - 2) / 2; s >= 0; s--)
        xr_sort_core_vheap_sift(a, s, n - 1, cmp, ctx);
    for (int64_t e = n - 1; e > 0; e--) {
        XrValue t = a[e];
        a[e] = a[0];
        a[0] = t;
        xr_sort_core_vheap_sift(a, 0, e - 1, cmp, ctx);
    }
}

static inline void xr_sort_core_visort(XrValue *a, int64_t n, XrSortCoreCmpFn cmp, void *ctx) {
    for (int64_t i = 1; i < n; i++) {
        XrValue key = a[i];
        int64_t j = i - 1;
        while (j >= 0 && xr_sort_core_vless(key, a[j], cmp, ctx)) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }
}

static inline int64_t xr_sort_core_vmed3(XrValue *a, int64_t x, int64_t y, int64_t z,
                                         XrSortCoreCmpFn cmp, void *ctx) {
    if (xr_sort_core_vless(a[y], a[x], cmp, ctx)) {
        if (xr_sort_core_vless(a[z], a[y], cmp, ctx))
            return y;
        return xr_sort_core_vless(a[z], a[x], cmp, ctx) ? z : x;
    }
    if (xr_sort_core_vless(a[z], a[y], cmp, ctx))
        return xr_sort_core_vless(a[z], a[x], cmp, ctx) ? x : z;
    return y;
}

static inline void xr_sort_core_vintrosort(XrValue *a, int64_t n, int depth, XrSortCoreCmpFn cmp,
                                           void *ctx) {
    while (n > XR_SORT_CORE_SMALL) {
        if (depth-- == 0) {
            xr_sort_core_vheapsort(a, n, cmp, ctx);
            return;
        }
        int64_t mid = n / 2;
        int64_t pi;
        if (n > 128) {
            int64_t step = n / 8;
            int64_t m1 = xr_sort_core_vmed3(a, 0, step, step * 2, cmp, ctx);
            int64_t m2 = xr_sort_core_vmed3(a, mid - step, mid, mid + step, cmp, ctx);
            int64_t m3 = xr_sort_core_vmed3(a, n - 1 - step * 2, n - 1 - step, n - 1, cmp, ctx);
            pi = xr_sort_core_vmed3(a, m1, m2, m3, cmp, ctx);
        } else {
            pi = xr_sort_core_vmed3(a, 0, mid, n - 1, cmp, ctx);
        }
        XrValue pv = a[pi];
        int64_t lt = 0;
        int64_t i = 0;
        int64_t gt = n;
        while (i < gt) {
            int c = cmp ? cmp(ctx, a[i], pv) : 0;
            if (c < 0) {
                XrValue t = a[lt];
                a[lt] = a[i];
                a[i] = t;
                lt++;
                i++;
            } else if (c > 0) {
                gt--;
                XrValue t = a[i];
                a[i] = a[gt];
                a[gt] = t;
            } else {
                i++;
            }
        }
        if (lt < n - gt) {
            xr_sort_core_vintrosort(a, lt, depth, cmp, ctx);
            a += gt;
            n -= gt;
        } else {
            xr_sort_core_vintrosort(a + gt, n - gt, depth, cmp, ctx);
            n = lt;
        }
    }
    xr_sort_core_visort(a, n, cmp, ctx);
}

static inline void xr_sort_core_values(XrValue *a, int64_t n, XrSortCoreCmpFn cmp, void *ctx) {
    if (!a || n < 2)
        return;
    int depth = 0;
    for (int64_t m = n; m > 0; m >>= 1)
        depth += 2;
    xr_sort_core_vintrosort(a, n, depth, cmp, ctx);
}

#undef XR_SORT_CORE_LT
#undef XR_SORT_CORE_DEF
#undef XR_SORT_CORE_SMALL

#endif  // XR_SORT_CORE_H
