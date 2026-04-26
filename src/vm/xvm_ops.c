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
#include "../runtime/value/xvalue_format.h"
#include <inttypes.h>
#include "../coro/xchannel.h"
#include "../module/xmodule.h"
#include "../runtime/object/xstringbuilder.h"
#include "../runtime/object/xrange.h"
#include <stdlib.h>
#include <time.h>

#include "../runtime/xstdlib_bridge.h"

/* ========== Cycle Detection Data Structures ========== */

/*
 * Open-addressing hash set of (a, b) pointer pairs currently being
 * compared. Sized to grow without bound so cyclic structures with
 * arbitrarily many shared sub-objects can be compared without false
 * negatives. Pairs are canonicalised so that (a, b) and (b, a) hash
 * to the same slot -- equality is symmetric.
 *
 * Memory: empty / tombstone / occupied tracked in a parallel meta byte
 * array; resized at 75% load factor. Worst-case lookup is O(1) average
 * with linear probing under a good hash. Total time/space for a
 * compare operation is O(n) where n is the number of distinct
 * sub-object pairs visited.
 */

typedef struct {
    void *a;
    void *b;
} ObjectPair;

#define VS_EMPTY 0
#define VS_TOMBSTONE 1
#define VS_OCCUPIED 2

typedef struct {
    ObjectPair *entries;
    uint8_t *meta;
    int capacity;  // power of two, 0 until first insert
    int count;     // distinct occupied slots
    int load;      // occupied + tombstones (resize trigger)
} VisitedSet;

typedef struct {
    XrayIsolate *isolate;
    VisitedSet visiting;
} CompareContext;

static bool deep_compare(CompareContext *ctx, XrValue a, XrValue b);

static inline void visited_init(VisitedSet *vs) {
    vs->entries = NULL;
    vs->meta = NULL;
    vs->capacity = 0;
    vs->count = 0;
    vs->load = 0;
}

static inline void visited_free(VisitedSet *vs) {
    if (vs->entries)
        xr_free(vs->entries);
    if (vs->meta)
        xr_free(vs->meta);
    vs->entries = NULL;
    vs->meta = NULL;
    vs->capacity = 0;
    vs->count = 0;
    vs->load = 0;
}

// Canonicalise so (a,b) and (b,a) end up in the same slot. Sorting by
// pointer value avoids storing both orderings.
static inline void pair_canon(void **a, void **b) {
    if ((uintptr_t) *a > (uintptr_t) *b) {
        void *t = *a;
        *a = *b;
        *b = t;
    }
}

static inline uint32_t pair_hash(void *a, void *b) {
    // Mix two pointers via a 64-bit FNV-style fold, then truncate to
    // 32 bits. The "+ 1" guard ensures non-zero output so the empty
    // slot marker (zeroed entries) never collides with a live key.
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    h ^= (uint64_t) (uintptr_t) a;
    h *= XR_FNV64_PRIME;
    h ^= (uint64_t) (uintptr_t) b;
    h *= XR_FNV64_PRIME;
    return (uint32_t) ((h >> 32) ^ h) + 1u;
}

// Resize and rehash. new_cap must be a power of two. Returns false on OOM.
static bool visited_resize(VisitedSet *vs, int new_cap) {
    XR_DCHECK((new_cap & (new_cap - 1)) == 0, "visited_resize: cap not pow2");
    ObjectPair *new_entries = (ObjectPair *) xr_calloc((size_t) new_cap, sizeof(ObjectPair));
    if (!new_entries)
        return false;
    uint8_t *new_meta = (uint8_t *) xr_calloc((size_t) new_cap, sizeof(uint8_t));
    if (!new_meta) {
        xr_free(new_entries);
        return false;
    }
    uint32_t mask = (uint32_t) new_cap - 1u;
    for (int i = 0; i < vs->capacity; i++) {
        if (vs->meta[i] != VS_OCCUPIED)
            continue;
        ObjectPair p = vs->entries[i];
        uint32_t idx = pair_hash(p.a, p.b) & mask;
        while (new_meta[idx] == VS_OCCUPIED) {
            idx = (idx + 1u) & mask;
        }
        new_entries[idx] = p;
        new_meta[idx] = VS_OCCUPIED;
    }
    if (vs->entries)
        xr_free(vs->entries);
    if (vs->meta)
        xr_free(vs->meta);
    vs->entries = new_entries;
    vs->meta = new_meta;
    vs->capacity = new_cap;
    vs->load = vs->count;
    return true;
}

// Returns true if (a,b) is currently in the visited set.
static bool is_visiting(CompareContext *ctx, void *a, void *b) {
    VisitedSet *vs = &ctx->visiting;
    if (vs->capacity == 0)
        return false;
    pair_canon(&a, &b);
    uint32_t mask = (uint32_t) vs->capacity - 1u;
    uint32_t idx = pair_hash(a, b) & mask;
    while (vs->meta[idx] != VS_EMPTY) {
        if (vs->meta[idx] == VS_OCCUPIED && vs->entries[idx].a == a && vs->entries[idx].b == b) {
            return true;
        }
        idx = (idx + 1u) & mask;
    }
    return false;
}

// Insert (a,b). Returns false on OOM (caller must treat as cycle/abort).
static bool push_visiting(CompareContext *ctx, void *a, void *b) {
    VisitedSet *vs = &ctx->visiting;
    pair_canon(&a, &b);

    // Initial allocation or grow at 75% (load == capacity * 3/4).
    if (vs->capacity == 0) {
        if (!visited_resize(vs, 16))
            return false;
    } else if (vs->load * 4 >= vs->capacity * 3) {
        if (!visited_resize(vs, vs->capacity * 2))
            return false;
    }

    uint32_t mask = (uint32_t) vs->capacity - 1u;
    uint32_t idx = pair_hash(a, b) & mask;
    int first_tomb = -1;
    while (vs->meta[idx] != VS_EMPTY) {
        if (vs->meta[idx] == VS_TOMBSTONE) {
            if (first_tomb < 0)
                first_tomb = (int) idx;
        } else if (vs->entries[idx].a == a && vs->entries[idx].b == b) {
            // Already present (cycle re-entered) -- caller checks
            // is_visiting() first, so this path implies a logic bug.
            return true;
        }
        idx = (idx + 1u) & mask;
    }
    if (first_tomb >= 0)
        idx = (uint32_t) first_tomb;
    else
        vs->load++;
    vs->entries[idx].a = a;
    vs->entries[idx].b = b;
    vs->meta[idx] = VS_OCCUPIED;
    vs->count++;
    return true;
}

// Remove (a,b) from the set. Stack-discipline: only the most recently
// pushed pair is popped, so locating it is one find + tombstone.
static void pop_visiting(CompareContext *ctx, void *a, void *b) {
    VisitedSet *vs = &ctx->visiting;
    if (vs->capacity == 0)
        return;
    pair_canon(&a, &b);
    uint32_t mask = (uint32_t) vs->capacity - 1u;
    uint32_t idx = pair_hash(a, b) & mask;
    while (vs->meta[idx] != VS_EMPTY) {
        if (vs->meta[idx] == VS_OCCUPIED && vs->entries[idx].a == a && vs->entries[idx].b == b) {
            vs->meta[idx] = VS_TOMBSTONE;
            vs->count--;
            return;
        }
        idx = (idx + 1u) & mask;
    }
    // Not found -- this means push/pop mismatched, indicating a bug.
    XR_DCHECK(false, "pop_visiting: pair not in set");
}

/* ========== Type Conversion Helpers ========== */

/* Value-to-string formatting has moved to runtime/value/xvalue_format.c.
 * VM callers use xr_value_to_string / xr_value_to_strbuf directly. */

// String concatenation
static inline XrValue vm_string_concat_values(XrayIsolate *isolate, XrValue left, XrValue right) {
    XR_DCHECK(isolate != NULL, "string_concat_values: NULL isolate");
    XrString *str_left = xr_value_to_string(isolate, left);
    XrString *str_right = xr_value_to_string(isolate, right);
    XrString *result = xr_string_concat(isolate, str_left, str_right);
    return xr_string_value(result);
}

// Numeric addition (handles int/float mix)
static inline XrValue vm_numeric_add(XrValue left, XrValue right) {
    if (XR_IS_INT(left) && XR_IS_INT(right)) {
        return xr_int(XR_TO_INT(left) + XR_TO_INT(right));
    } else if ((XR_IS_INT(left) || XR_IS_FLOAT(left)) && (XR_IS_INT(right) || XR_IS_FLOAT(right))) {
        double nl = XR_IS_INT(left) ? (double) XR_TO_INT(left) : XR_TO_FLOAT(left);
        double nr = XR_IS_INT(right) ? (double) XR_TO_INT(right) : XR_TO_FLOAT(right);
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
        double nl = XR_IS_INT(left) ? (double) XR_TO_INT(left) : XR_TO_FLOAT(left);
        double nr = XR_IS_INT(right) ? (double) XR_TO_INT(right) : XR_TO_FLOAT(right);
        return xr_float(nl - nr);
    }
    return xr_null();
}

// Multiplication
XrValue vm_numeric_mul(XrValue left, XrValue right) {
    if (XR_IS_INT(left) && XR_IS_INT(right)) {
        return xr_int(XR_TO_INT(left) * XR_TO_INT(right));
    } else if ((XR_IS_INT(left) || XR_IS_FLOAT(left)) && (XR_IS_INT(right) || XR_IS_FLOAT(right))) {
        double nl = XR_IS_INT(left) ? (double) XR_TO_INT(left) : XR_TO_FLOAT(left);
        double nr = XR_IS_INT(right) ? (double) XR_TO_INT(right) : XR_TO_FLOAT(right);
        return xr_float(nl * nr);
    }
    return xr_null();
}

// Division
XrValue vm_numeric_div(XrayIsolate *isolate, XrValue left, XrValue right) {
    XR_DCHECK(isolate != NULL, "vm_numeric_div: NULL isolate");
    if ((XR_IS_INT(left) || XR_IS_FLOAT(left)) && (XR_IS_INT(right) || XR_IS_FLOAT(right))) {
        double nl = XR_IS_INT(left) ? (double) XR_TO_INT(left) : XR_TO_FLOAT(left);
        double nr = XR_IS_INT(right) ? (double) XR_TO_INT(right) : XR_TO_FLOAT(right);

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

// Array deep comparison.
// Cycles are detected by the visited set: re-entering the same (a,b)
// pair returns true (the structures are recursively equal at this
// point unless a difference appears elsewhere in the traversal).
static bool array_deep_equal(CompareContext *ctx, XrArray *a, XrArray *b) {
    if (a == b)
        return true;
    if (a->length != b->length)
        return false;

    if (is_visiting(ctx, a, b))
        return true;
    if (!push_visiting(ctx, a, b))
        return false;

    bool result = true;
    for (int i = 0; i < a->length; i++) {
        if (!deep_compare(ctx, ((XrValue *) a->data)[i], ((XrValue *) b->data)[i])) {
            result = false;
            break;
        }
    }

    pop_visiting(ctx, a, b);
    return result;
}

// Map deep comparison
static bool map_deep_equal(CompareContext *ctx, XrMap *a, XrMap *b) {
    if (a == b)
        return true;
    if (a->count != b->count)
        return false;

    if (is_visiting(ctx, a, b))
        return true;
    if (!push_visiting(ctx, a, b))
        return false;

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

    pop_visiting(ctx, a, b);
    return result;
}

// Set deep comparison
static bool set_deep_equal(CompareContext *ctx, XrSet *a, XrSet *b) {
    if (a == b)
        return true;
    if (a->count != b->count)
        return false;

    if (is_visiting(ctx, a, b))
        return true;
    if (!push_visiting(ctx, a, b))
        return false;

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

    pop_visiting(ctx, a, b);
    return result;
}

// Deep comparison core function (recursive)
// Custom objects use operator== at OP_CMP_EQ level, not here
static bool deep_compare(CompareContext *ctx, XrValue a, XrValue b) {
    if (xr_value_same(a, b))
        return true;

    if (XR_IS_INT(a) && XR_IS_INT(b))
        return XR_TO_INT(a) == XR_TO_INT(b);
    if (XR_IS_FLOAT(a) && XR_IS_FLOAT(b))
        return XR_TO_FLOAT(a) == XR_TO_FLOAT(b);
    if (XR_IS_BOOL(a) && XR_IS_BOOL(b))
        return XR_TO_BOOL(a) == XR_TO_BOOL(b);
    if (XR_IS_NULL(a) && XR_IS_NULL(b))
        return true;
    if (XR_IS_STRING(a) && XR_IS_STRING(b))
        return xr_string_equal(XR_TO_STRING(a), XR_TO_STRING(b));
    if (XR_IS_ARRAY(a) && XR_IS_ARRAY(b))
        return array_deep_equal(ctx, xr_value_to_array(a), xr_value_to_array(b));
    if (XR_IS_MAP(a) && XR_IS_MAP(b))
        return map_deep_equal(ctx, xr_value_to_map(a), xr_value_to_map(b));
    if (XR_IS_SET(a) && XR_IS_SET(b))
        return set_deep_equal(ctx, xr_value_to_set(a), xr_value_to_set(b));
    // Instance field-by-field comparison for value types (structs)
    if (xr_value_is_instance(a) && xr_value_is_instance(b)) {
        XrInstance *ia = xr_value_to_instance(a);
        XrInstance *ib = xr_value_to_instance(b);
        if (ia->klass != ib->klass)
            return false;
        int fc = ia->klass->field_count;
        for (int i = 0; i < fc; i++) {
            if (!deep_compare(ctx, ia->fields[i], ib->fields[i]))
                return false;
        }
        return true;
    }
    if (XR_IS_PTR(a) && XR_IS_PTR(b))
        return XR_TO_PTR(a) == XR_TO_PTR(b);

    return false;
}

/* ========== Comparison Operation Helpers ========== */

// Deep equality comparison (with isolate, supports Array/Map/Set)
bool vm_values_equal_deep(XrayIsolate *isolate, XrValue a, XrValue b) {
    if (xr_value_same(a, b))
        return true;

    if (XR_IS_INT(a) && XR_IS_INT(b))
        return XR_TO_INT(a) == XR_TO_INT(b);
    if (XR_IS_FLOAT(a) && XR_IS_FLOAT(b))
        return XR_TO_FLOAT(a) == XR_TO_FLOAT(b);
    if (XR_IS_BOOL(a) && XR_IS_BOOL(b))
        return XR_TO_BOOL(a) == XR_TO_BOOL(b);
    if (XR_IS_NULL(a) && XR_IS_NULL(b))
        return true;
    if (XR_IS_STRING(a) && XR_IS_STRING(b)) {
        uint32_t la = xr_value_str_len(&a), lb = xr_value_str_len(&b);
        if (la != lb)
            return false;
        return memcmp(xr_value_str_data(&a), xr_value_str_data(&b), la) == 0;
    }
    if (XR_IS_JSON(a) && XR_IS_JSON(b))
        return xr_value_deep_eq(a, b);

    // BigInt equality
    if (XR_IS_BIGINT(a) && XR_IS_BIGINT(b)) {
        return xr_bigint_cmp((XrBigInt *) XR_TO_PTR(a), (XrBigInt *) XR_TO_PTR(b)) == 0;
    }
    // BigInt == int (mixed comparison)
    if (XR_IS_BIGINT(a) && XR_IS_INT(b)) {
        return xr_bigint_cmp_int((XrBigInt *) XR_TO_PTR(a), XR_TO_INT(b)) == 0;
    }
    if (XR_IS_INT(a) && XR_IS_BIGINT(b)) {
        return xr_bigint_cmp_int((XrBigInt *) XR_TO_PTR(b), XR_TO_INT(a)) == 0;
    }

    // Struct ref: field-by-field comparison via native layout
    if (XR_IS_STRUCT_REF(a) && XR_IS_STRUCT_REF(b)) {
        uint8_t *sa = (uint8_t *) xr_to_struct_ptr(a);
        uint8_t *sb = (uint8_t *) xr_to_struct_ptr(b);
        XrClass *ca = *(XrClass **) sa;
        XrClass *cb = *(XrClass **) sb;
        if (ca != cb)
            return false;
        XrStructLayout *layout = ca->struct_layout;
        if (!layout)
            return sa == sb;
        // Compare the entire native field data area
        return memcmp(sa + 8, sb + 8, layout->total_size) == 0;
    }

    if (XR_IS_ARRAY(a) || XR_IS_MAP(a) || XR_IS_SET(a) || xr_value_is_instance(a)) {
        if (isolate) {
            CompareContext ctx;
            ctx.isolate = isolate;
            visited_init(&ctx.visiting);
            bool eq = deep_compare(&ctx, a, b);
            visited_free(&ctx.visiting);
            return eq;
        }
    }

    if (XR_IS_PTR(a) && XR_IS_PTR(b))
        return XR_TO_PTR(a) == XR_TO_PTR(b);
    return false;
}

// General equality comparison (for == operator)
// Uses pointer comparison for reference types (use vm_values_equal_deep for deep comparison)
bool vm_values_equal(XrValue a, XrValue b) {
    // NaN is never equal to itself (IEEE 754)
    if (XR_IS_FLOAT(a) && XR_IS_FLOAT(b))
        return XR_TO_FLOAT(a) == XR_TO_FLOAT(b);
    if (xr_value_same(a, b))
        return true;

    if (XR_IS_INT(a) && XR_IS_INT(b))
        return XR_TO_INT(a) == XR_TO_INT(b);
    // Numeric cross-type: int == float, float == int
    if (XR_IS_INT(a) && XR_IS_FLOAT(b))
        return (double) XR_TO_INT(a) == XR_TO_FLOAT(b);
    if (XR_IS_FLOAT(a) && XR_IS_INT(b))
        return XR_TO_FLOAT(a) == (double) XR_TO_INT(b);
    if (XR_IS_BOOL(a) && XR_IS_BOOL(b))
        return XR_TO_BOOL(a) == XR_TO_BOOL(b);
    if (XR_IS_NULL(a) && XR_IS_NULL(b))
        return true;
    if (XR_IS_STRING(a) && XR_IS_STRING(b)) {
        // Direct content comparison — works for SSO, heap, or mixed
        uint32_t la = xr_value_str_len(&a);
        uint32_t lb = xr_value_str_len(&b);
        if (la != lb)
            return false;
        return memcmp(xr_value_str_data(&a), xr_value_str_data(&b), la) == 0;
    }

    // BigInt equality
    if (XR_IS_BIGINT(a) && XR_IS_BIGINT(b)) {
        return xr_bigint_cmp((XrBigInt *) XR_TO_PTR(a), (XrBigInt *) XR_TO_PTR(b)) == 0;
    }
    // BigInt == int (mixed comparison)
    if (XR_IS_BIGINT(a) && XR_IS_INT(b)) {
        return xr_bigint_cmp_int((XrBigInt *) XR_TO_PTR(a), XR_TO_INT(b)) == 0;
    }
    if (XR_IS_INT(a) && XR_IS_BIGINT(b)) {
        return xr_bigint_cmp_int((XrBigInt *) XR_TO_PTR(b), XR_TO_INT(a)) == 0;
    }

    if (XR_IS_PTR(a) && XR_IS_PTR(b))
        return XR_TO_PTR(a) == XR_TO_PTR(b);

    return false;
}

// Numeric less than
bool vm_numeric_less(XrValue left, XrValue right) {
    // BigInt comparisons
    if (XR_IS_BIGINT(left) && XR_IS_BIGINT(right)) {
        return xr_bigint_cmp((XrBigInt *) XR_TO_PTR(left), (XrBigInt *) XR_TO_PTR(right)) < 0;
    }
    if (XR_IS_BIGINT(left) && XR_IS_INT(right)) {
        return xr_bigint_cmp_int((XrBigInt *) XR_TO_PTR(left), XR_TO_INT(right)) < 0;
    }
    if (XR_IS_INT(left) && XR_IS_BIGINT(right)) {
        return xr_bigint_cmp_int((XrBigInt *) XR_TO_PTR(right), XR_TO_INT(left)) > 0;
    }

    if ((XR_IS_INT(left) || XR_IS_FLOAT(left)) && (XR_IS_INT(right) || XR_IS_FLOAT(right))) {
        double nl = XR_IS_INT(left) ? (double) XR_TO_INT(left) : XR_TO_FLOAT(left);
        double nr = XR_IS_INT(right) ? (double) XR_TO_INT(right) : XR_TO_FLOAT(right);
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
        return xr_bigint_cmp((XrBigInt *) XR_TO_PTR(left), (XrBigInt *) XR_TO_PTR(right)) <= 0;
    }
    if (XR_IS_BIGINT(left) && XR_IS_INT(right)) {
        return xr_bigint_cmp_int((XrBigInt *) XR_TO_PTR(left), XR_TO_INT(right)) <= 0;
    }
    if (XR_IS_INT(left) && XR_IS_BIGINT(right)) {
        return xr_bigint_cmp_int((XrBigInt *) XR_TO_PTR(right), XR_TO_INT(left)) >= 0;
    }

    if ((XR_IS_INT(left) || XR_IS_FLOAT(left)) && (XR_IS_INT(right) || XR_IS_FLOAT(right))) {
        double nl = XR_IS_INT(left) ? (double) XR_TO_INT(left) : XR_TO_FLOAT(left);
        double nr = XR_IS_INT(right) ? (double) XR_TO_INT(right) : XR_TO_FLOAT(right);
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
        return xr_bigint_cmp((XrBigInt *) XR_TO_PTR(left), (XrBigInt *) XR_TO_PTR(right)) > 0;
    }
    if (XR_IS_BIGINT(left) && XR_IS_INT(right)) {
        return xr_bigint_cmp_int((XrBigInt *) XR_TO_PTR(left), XR_TO_INT(right)) > 0;
    }
    if (XR_IS_INT(left) && XR_IS_BIGINT(right)) {
        return xr_bigint_cmp_int((XrBigInt *) XR_TO_PTR(right), XR_TO_INT(left)) < 0;
    }

    if ((XR_IS_INT(left) || XR_IS_FLOAT(left)) && (XR_IS_INT(right) || XR_IS_FLOAT(right))) {
        double nl = XR_IS_INT(left) ? (double) XR_TO_INT(left) : XR_TO_FLOAT(left);
        double nr = XR_IS_INT(right) ? (double) XR_TO_INT(right) : XR_TO_FLOAT(right);
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
        return xr_bigint_cmp((XrBigInt *) XR_TO_PTR(left), (XrBigInt *) XR_TO_PTR(right)) >= 0;
    }
    if (XR_IS_BIGINT(left) && XR_IS_INT(right)) {
        return xr_bigint_cmp_int((XrBigInt *) XR_TO_PTR(left), XR_TO_INT(right)) >= 0;
    }
    if (XR_IS_INT(left) && XR_IS_BIGINT(right)) {
        return xr_bigint_cmp_int((XrBigInt *) XR_TO_PTR(right), XR_TO_INT(left)) <= 0;
    }

    if ((XR_IS_INT(left) || XR_IS_FLOAT(left)) && (XR_IS_INT(right) || XR_IS_FLOAT(right))) {
        double nl = XR_IS_INT(left) ? (double) XR_TO_INT(left) : XR_TO_FLOAT(left);
        double nr = XR_IS_INT(right) ? (double) XR_TO_INT(right) : XR_TO_FLOAT(right);
        return nl >= nr;
    }
    if (XR_IS_STRING(left) && XR_IS_STRING(right)) {
        return xr_string_compare(XR_TO_STRING(left), XR_TO_STRING(right)) >= 0;
    }
    return false;
}
