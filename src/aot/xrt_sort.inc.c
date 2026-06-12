/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_sort.inc.c - Array sort runtime (included by xrt_method.h).
 *
 * Unstable introsort with three-way (Dutch flag) partitioning:
 *   - insertion sort below 24 elements
 *   - median-of-3 pivot (ninther above 128 elements)
 *   - three-way partition makes equal-key inputs O(n)
 *   - depth budget 2*log2(n), then heapsort (O(n log n) worst case)
 * Primitive element types get monomorphic loops (direct value compares,
 * no per-element boxing) — the same property Rust gets from generics.
 * Matches VM ordering semantics: numbers by value, strings by content,
 * other tags compare equal; comparator return value is used by sign.
 */

/* Default ordering, mirrors the VM's xr_value_compare_default. */
static inline int xrt_sort_cmp_default(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return (a.i > b.i) - (a.i < b.i);
    if (a.tag == XR_TAG_F64 && b.tag == XR_TAG_F64)
        return (a.f > b.f) - (a.f < b.f);
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_F64) {
        double fa = (double) a.i;
        return (fa > b.f) - (fa < b.f);
    }
    if (a.tag == XR_TAG_F64 && b.tag == XR_TAG_I64) {
        double fb = (double) b.i;
        return (a.f > fb) - (a.f < fb);
    }
    if (XR_IS_STR(a) && XR_IS_STR(b)) {
        int64_t la = xr_str_len(a), lb = xr_str_len(b);
        int64_t minlen = la < lb ? la : lb;
        int cmp = memcmp(xr_str_data(a), xr_str_data(b), (size_t) minlen);
        if (cmp != 0)
            return cmp;
        return (la > lb) - (la < lb);
    }
    return 0;
}

/* Comparator closure result interpreted by sign (int or float). */
static inline int xrt_sort_cmp(XrValue a, XrValue b, xrt_closure_t *cl) {
    if (cl) {
        typedef XrValue (*xrt_fn2_t)(xrt_closure_t *, XrValue, XrValue);
        XrValue r = ((xrt_fn2_t) cl->fn)(cl, a, b);
        if (r.tag == XR_TAG_I64)
            return (r.i > 0) - (r.i < 0);
        if (r.tag == XR_TAG_F64)
            return (r.f > 0.0) - (r.f < 0.0);
        return 0;
    }
    return xrt_sort_cmp_default(a, b);
}

#define XRT_SORT_SMALL 24

/* Monomorphic introsort over a primitive element type.
 * LESS(x, y) must be a strict weak ordering. */
#define XRT_SORT_DEF(SFX, T, LESS)                                                                 \
    static void xrt_heap_sift_##SFX(T *a, int64_t root, int64_t end) {                             \
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
    static void xrt_heapsort_##SFX(T *a, int64_t n) {                                              \
        for (int64_t s = (n - 2) / 2; s >= 0; s--)                                                 \
            xrt_heap_sift_##SFX(a, s, n - 1);                                                      \
        for (int64_t e = n - 1; e > 0; e--) {                                                      \
            T t = a[e];                                                                            \
            a[e] = a[0];                                                                           \
            a[0] = t;                                                                              \
            xrt_heap_sift_##SFX(a, 0, e - 1);                                                      \
        }                                                                                          \
    }                                                                                              \
    static void xrt_isort_##SFX(T *a, int64_t n) {                                                 \
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
    static inline int64_t xrt_med3_##SFX(T *a, int64_t x, int64_t y, int64_t z) {                  \
        if (LESS(a[y], a[x])) {                                                                    \
            if (LESS(a[z], a[y]))                                                                  \
                return y;                                                                          \
            return LESS(a[z], a[x]) ? z : x;                                                       \
        }                                                                                          \
        if (LESS(a[z], a[y]))                                                                      \
            return LESS(a[z], a[x]) ? x : z;                                                       \
        return y;                                                                                  \
    }                                                                                              \
    static void xrt_introsort_##SFX(T *a, int64_t n, int depth) {                                  \
        while (n > XRT_SORT_SMALL) {                                                               \
            if (depth-- == 0) {                                                                    \
                xrt_heapsort_##SFX(a, n);                                                          \
                return;                                                                            \
            }                                                                                      \
            int64_t mid = n / 2;                                                                   \
            int64_t pi;                                                                            \
            if (n > 128) {                                                                         \
                int64_t step = n / 8;                                                              \
                int64_t m1 = xrt_med3_##SFX(a, 0, step, step * 2);                                 \
                int64_t m2 = xrt_med3_##SFX(a, mid - step, mid, mid + step);                       \
                int64_t m3 = xrt_med3_##SFX(a, n - 1 - step * 2, n - 1 - step, n - 1);             \
                pi = xrt_med3_##SFX(a, m1, m2, m3);                                                \
            } else {                                                                               \
                pi = xrt_med3_##SFX(a, 0, mid, n - 1);                                             \
            }                                                                                      \
            T pv = a[pi];                                                                          \
            /* Dutch flag: [0,lt) < pv, [lt,i) == pv, [gt,n) > pv */                               \
            int64_t lt = 0, i = 0, gt = n;                                                         \
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
            /* Recurse into the smaller side, loop on the larger. */                               \
            if (lt < n - gt) {                                                                     \
                xrt_introsort_##SFX(a, lt, depth);                                                 \
                a += gt;                                                                           \
                n -= gt;                                                                           \
            } else {                                                                               \
                xrt_introsort_##SFX(a + gt, n - gt, depth);                                        \
                n = lt;                                                                            \
            }                                                                                      \
        }                                                                                          \
        xrt_isort_##SFX(a, n);                                                                     \
    }                                                                                              \
    static void xrt_sort_##SFX(T *a, int64_t n) {                                                  \
        if (n < 2)                                                                                 \
            return;                                                                                \
        int depth = 0;                                                                             \
        for (int64_t m = n; m > 0; m >>= 1)                                                        \
            depth += 2;                                                                            \
        xrt_introsort_##SFX(a, n, depth);                                                          \
    }

#define XRT_SORT_LT(x, y) ((x) < (y))
XRT_SORT_DEF(i8, int8_t, XRT_SORT_LT)
XRT_SORT_DEF(u8, uint8_t, XRT_SORT_LT)
XRT_SORT_DEF(i16, int16_t, XRT_SORT_LT)
XRT_SORT_DEF(u16, uint16_t, XRT_SORT_LT)
XRT_SORT_DEF(i32, int32_t, XRT_SORT_LT)
XRT_SORT_DEF(u32, uint32_t, XRT_SORT_LT)
XRT_SORT_DEF(i64, int64_t, XRT_SORT_LT)
XRT_SORT_DEF(u64, uint64_t, XRT_SORT_LT)
XRT_SORT_DEF(f32, float, XRT_SORT_LT)
XRT_SORT_DEF(f64, double, XRT_SORT_LT)

/* Boxed elements: same introsort, comparisons through xrt_sort_cmp.
 * Used for XR_ELEM_ANY and for any custom comparator. */
#define XRT_SORT_VLESS(x, y) (xrt_sort_cmp((x), (y), cl) < 0)
static void xrt_vheap_sift(XrValue *a, int64_t root, int64_t end, xrt_closure_t *cl) {
    while (root * 2 + 1 <= end) {
        int64_t child = root * 2 + 1;
        if (child + 1 <= end && XRT_SORT_VLESS(a[child], a[child + 1]))
            child++;
        if (!XRT_SORT_VLESS(a[root], a[child]))
            return;
        XrValue t = a[root];
        a[root] = a[child];
        a[child] = t;
        root = child;
    }
}
static void xrt_vheapsort(XrValue *a, int64_t n, xrt_closure_t *cl) {
    for (int64_t s = (n - 2) / 2; s >= 0; s--)
        xrt_vheap_sift(a, s, n - 1, cl);
    for (int64_t e = n - 1; e > 0; e--) {
        XrValue t = a[e];
        a[e] = a[0];
        a[0] = t;
        xrt_vheap_sift(a, 0, e - 1, cl);
    }
}
static void xrt_visort(XrValue *a, int64_t n, xrt_closure_t *cl) {
    for (int64_t i = 1; i < n; i++) {
        XrValue key = a[i];
        int64_t j = i - 1;
        while (j >= 0 && XRT_SORT_VLESS(key, a[j])) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }
}
static int64_t xrt_vmed3(XrValue *a, int64_t x, int64_t y, int64_t z, xrt_closure_t *cl) {
    if (XRT_SORT_VLESS(a[y], a[x])) {
        if (XRT_SORT_VLESS(a[z], a[y]))
            return y;
        return XRT_SORT_VLESS(a[z], a[x]) ? z : x;
    }
    if (XRT_SORT_VLESS(a[z], a[y]))
        return XRT_SORT_VLESS(a[z], a[x]) ? x : z;
    return y;
}
static void xrt_vintrosort(XrValue *a, int64_t n, int depth, xrt_closure_t *cl) {
    while (n > XRT_SORT_SMALL) {
        if (depth-- == 0) {
            xrt_vheapsort(a, n, cl);
            return;
        }
        int64_t mid = n / 2;
        int64_t pi;
        if (n > 128) {
            int64_t step = n / 8;
            int64_t m1 = xrt_vmed3(a, 0, step, step * 2, cl);
            int64_t m2 = xrt_vmed3(a, mid - step, mid, mid + step, cl);
            int64_t m3 = xrt_vmed3(a, n - 1 - step * 2, n - 1 - step, n - 1, cl);
            pi = xrt_vmed3(a, m1, m2, m3, cl);
        } else {
            pi = xrt_vmed3(a, 0, mid, n - 1, cl);
        }
        XrValue pv = a[pi];
        int64_t lt = 0, i = 0, gt = n;
        while (i < gt) {
            int c = xrt_sort_cmp(a[i], pv, cl);
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
            xrt_vintrosort(a, lt, depth, cl);
            a += gt;
            n -= gt;
        } else {
            xrt_vintrosort(a + gt, n - gt, depth, cl);
            n = lt;
        }
    }
    xrt_visort(a, n, cl);
}
static void xrt_vsort(XrValue *a, int64_t n, xrt_closure_t *cl) {
    if (n < 2)
        return;
    int depth = 0;
    for (int64_t m = n; m > 0; m >>= 1)
        depth += 2;
    xrt_vintrosort(a, n, depth, cl);
}

/* Entry point: in-place sort, optional comparator closure.
 * Typed arrays sort their native buffers directly (no boxing) when no
 * comparator is given; with a comparator, elements round-trip through a
 * boxed scratch buffer so the callback sees XrValue.
 * Returns recv (matches `sort` returning the array). */
static XrValue xrt_array_sort(XrValue recv, xrt_closure_t *cl) {
    if (recv.tag != XR_TAG_ARRAY || !recv.ptr)
        return recv;
    xrt_array_t *arr = (xrt_array_t *) recv.ptr;
    int64_t n = arr->len;
    if (n < 2)
        return recv;
    if (!cl) {
        switch (arr->elem_type) {
            case XR_ELEM_I8:
                xrt_sort_i8((int8_t *) arr->data, n);
                return recv;
            case XR_ELEM_U8:
            case XR_ELEM_BOOL:
                xrt_sort_u8((uint8_t *) arr->data, n);
                return recv;
            case XR_ELEM_I16:
                xrt_sort_i16((int16_t *) arr->data, n);
                return recv;
            case XR_ELEM_U16:
                xrt_sort_u16((uint16_t *) arr->data, n);
                return recv;
            case XR_ELEM_I32:
                xrt_sort_i32((int32_t *) arr->data, n);
                return recv;
            case XR_ELEM_U32:
                xrt_sort_u32((uint32_t *) arr->data, n);
                return recv;
            case XR_ELEM_I64:
                xrt_sort_i64((int64_t *) arr->data, n);
                return recv;
            case XR_ELEM_U64:
                xrt_sort_u64((uint64_t *) arr->data, n);
                return recv;
            case XR_ELEM_F32:
                xrt_sort_f32((float *) arr->data, n);
                return recv;
            case XR_ELEM_F64:
                xrt_sort_f64((double *) arr->data, n);
                return recv;
            default:
                xrt_vsort((XrValue *) arr->data, n, NULL);
                return recv;
        }
    }
    if (arr->elem_type == XR_ELEM_ANY) {
        xrt_vsort((XrValue *) arr->data, n, cl);
        return recv;
    }
    /* Typed buffer + custom comparator: box into scratch, sort, write back. */
    XrValue *tmp = (XrValue *) XRT_MALLOC((size_t) n * sizeof(XrValue));
    if (XR_UNLIKELY(!tmp)) {
        fprintf(stderr, "xrt_array_sort: out of memory\n");
        abort();
    }
    for (int64_t i = 0; i < n; i++)
        tmp[i] = xr_typed_get(arr->data, (int32_t) i, arr->elem_type);
    xrt_vsort(tmp, n, cl);
    for (int64_t i = 0; i < n; i++)
        xr_typed_set(arr->data, (int32_t) i, tmp[i], arr->elem_type);
    XRT_FREE(tmp);
    return recv;
}
