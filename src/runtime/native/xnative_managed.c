/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_managed.c - Metered metadata and failure-atomic physical ownership
 *
 * KEY CONCEPT:
 *   Weak descriptor indexes never own a cycle; final free precedes final leases.
 */
#include "xnative_managed.h"
#include "base/xchecks.h"
#include "base/xmalloc.h"
#include "base/xmax_align.h"
#include <stdalign.h>
#include <string.h>

struct XrtBillingOwner {
    _Atomic(int32_t) references;
    _Atomic(bool) lock;
    _Atomic(size_t) bytes, blocks, allocations, fail_after;
    _Atomic(bool) closed;
    size_t limit;
    XrtManagedAllocator allocator;
    XrtManagedOps *weak_ops;
};
struct XrtRuntimeTypePlan {
    _Atomic(int32_t) references;
    XrtBillingOwner *billing;
    XrtRuntimeTypeSpec spec;
    size_t owned_tail_length_offset;
};
struct XrtManagedOps {
    _Atomic(int32_t) references;
    XrtBillingOwner *billing;
    XrtRuntimeTypePlan *plan;
    XrtManagedOps *next;
};

static void billing_lock(XrtBillingOwner *owner) {
    while (atomic_exchange_explicit(&owner->lock, true, memory_order_acquire)) {}
}
static void billing_unlock(XrtBillingOwner *owner) {
    atomic_store_explicit(&owner->lock, false, memory_order_release);
}
static bool lease_valid(XrtManagedLease lease) {
    return (lease.retain && lease.drop) || (!lease.context && !lease.retain && !lease.drop);
}
static bool lease_take(XrtManagedLease lease) { return !lease.retain || lease.retain(lease.context); }
static void lease_drop(XrtManagedLease lease) { if (lease.drop) lease.drop(lease.context); }
static bool metadata_last(_Atomic(int32_t) *references) {
    XrSignedRcRelease release = xr_signed_rc_release(references);
    XR_CHECK(release != XR_SIGNED_RC_INVALID, "metadata reference count underflow");
    return release == XR_SIGNED_RC_LAST;
}
static void *default_allocate(void *context, size_t bytes) { (void)context; return xr_malloc(bytes); }
static void default_deallocate(void *context, void *memory, size_t bytes) {
    (void)context; (void)bytes; xr_free(memory);
}

XRAY_API XrtManagedStatus xrt_billing_owner_new(const XrtManagedAllocator *input,
    size_t limit, XrtBillingOwner **output) {
    if (!output || *output || (input && (!input->allocate || !input->deallocate ||
        !input->lease.retain || !input->lease.drop))) return XRT_MANAGED_ARGUMENT;
    if (limit < sizeof(XrtBillingOwner)) return XRT_MANAGED_LIMIT;
    XrtManagedAllocator allocator = input ? *input :
        (XrtManagedAllocator){.allocate=default_allocate, .deallocate=default_deallocate};
    if (!lease_take(allocator.lease)) return XRT_MANAGED_REFCOUNT;
    XrtBillingOwner *owner = allocator.allocate(allocator.context, sizeof(*owner));
    if (!owner) { lease_drop(allocator.lease); return XRT_MANAGED_OOM; }
    memset(owner, 0, sizeof(*owner));
    atomic_init(&owner->references, -1);
    atomic_init(&owner->lock, false);
    atomic_init(&owner->bytes, sizeof(*owner));
    atomic_init(&owner->blocks, 1);
    atomic_init(&owner->allocations, 1);
    atomic_init(&owner->fail_after, SIZE_MAX);
    atomic_init(&owner->closed, false);
    owner->limit = limit; owner->allocator = allocator; owner->weak_ops = NULL;
    *output = owner;
    return XRT_MANAGED_OK;
}
XRAY_API bool xrt_billing_owner_retain(XrtBillingOwner *owner) {
    return owner && xr_signed_rc_retain(&owner->references);
}
XRAY_API void xrt_billing_owner_drop(XrtBillingOwner *owner) {
    if (!owner || !metadata_last(&owner->references)) return;
    XR_CHECK(!owner->weak_ops && atomic_load(&owner->bytes) == sizeof(*owner) &&
        atomic_load(&owner->blocks) == 1, "the last billing lease follows every physical free");
    XrtManagedAllocator allocator = owner->allocator;
    allocator.deallocate(allocator.context, owner, sizeof(*owner));
    lease_drop(allocator.lease);
}
XRAY_API void xrt_billing_owner_close(XrtBillingOwner *owner) {
    billing_lock(owner);
    atomic_store_explicit(&owner->closed, true, memory_order_release);
    billing_unlock(owner);
}
XRAY_API void xrt_billing_owner_fail_after(XrtBillingOwner *owner, size_t count) {
    billing_lock(owner);
    atomic_store_explicit(&owner->fail_after, count, memory_order_relaxed);
    billing_unlock(owner);
}
XRAY_API XrtBillingStats xrt_billing_owner_stats(const XrtBillingOwner *owner) {
    return (XrtBillingStats){atomic_load(&owner->bytes), atomic_load(&owner->blocks),
        atomic_load(&owner->allocations), atomic_load(&owner->fail_after), atomic_load(&owner->closed)};
}

/* Only the reservation holds the index lock. Provider calls can reenter binding
 * or release independently owned metadata; in-flight reservations stay charged. */
static XrtManagedStatus billing_reserve(XrtBillingOwner *owner, size_t bytes) {
    billing_lock(owner);
    size_t held = atomic_load(&owner->bytes), budget = atomic_load(&owner->fail_after);
    XrtManagedStatus status = atomic_load(&owner->closed) ? XRT_MANAGED_CLOSED :
        bytes > owner->limit - held || atomic_load(&owner->blocks) == SIZE_MAX ||
        atomic_load(&owner->allocations) == SIZE_MAX ? XRT_MANAGED_LIMIT :
        !budget ? XRT_MANAGED_OOM : XRT_MANAGED_OK;
    if (status == XRT_MANAGED_OK) {
        atomic_fetch_add(&owner->bytes, bytes);
        atomic_fetch_add(&owner->blocks, 1);
        atomic_fetch_add(&owner->allocations, 1);
        if (budget != SIZE_MAX) atomic_store(&owner->fail_after, budget - 1);
    }
    billing_unlock(owner);
    return status;
}
static void billing_refund(XrtBillingOwner *owner, size_t bytes) {
    size_t blocks = atomic_fetch_sub(&owner->blocks, 1);
    size_t held = atomic_fetch_sub(&owner->bytes, bytes);
    XR_CHECK(blocks > 1 && held >= bytes + sizeof(*owner), "every allocation refunds its original billing owner");
}
static void billing_failed_allocation(XrtBillingOwner *owner, size_t bytes) {
    atomic_fetch_sub(&owner->bytes, bytes);
    atomic_fetch_sub(&owner->blocks, 1);
    atomic_fetch_sub(&owner->allocations, 1);
}
static XrtManagedStatus billing_allocate(XrtBillingOwner *owner, size_t bytes, void **out) {
    XrtManagedStatus status = billing_reserve(owner, bytes);
    if (status != XRT_MANAGED_OK) return status;
    void *memory = owner->allocator.allocate(owner->allocator.context, bytes);
    if (!memory) {
        billing_failed_allocation(owner, bytes);
        return XRT_MANAGED_OOM;
    }
    *out = memory;
    return XRT_MANAGED_OK;
}
static void billing_free(XrtBillingOwner *owner, void *memory, size_t bytes) {
    owner->allocator.deallocate(owner->allocator.context, memory, bytes);
    billing_refund(owner, bytes);
}
/* Explicit alignment above the provider's fundamental alignment reserves one
 * slack span and stores the provider pointer in the word in front of the
 * returned address. Billing still counts the requested bytes, so the physical
 * slack never changes a limit decision; only the provider sees the larger
 * block, and it gets the same length back on release. */
static bool aligned_allocation_bytes(size_t bytes, size_t alignment, size_t *physical) {
    if (!alignment || (alignment & (alignment - 1)) || alignment > XR_ALIGNMENT_LIMIT ||
        bytes > SIZE_MAX - alignment - sizeof(void *)) return false;
    *physical = bytes + alignment - 1 + sizeof(void *);
    return true;
}
static XrtManagedStatus billing_allocate_aligned(XrtBillingOwner *owner, size_t bytes,
    size_t alignment, void **out) {
    if (alignment <= alignof(XrMaxAlignment)) return billing_allocate(owner, bytes, out);
    size_t physical;
    if (!aligned_allocation_bytes(bytes, alignment, &physical)) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = billing_reserve(owner, bytes);
    if (status != XRT_MANAGED_OK) return status;
    void *base = owner->allocator.allocate(owner->allocator.context, physical);
    if (!base) {
        billing_failed_allocation(owner, bytes);
        return XRT_MANAGED_OOM;
    }
    uintptr_t start = (uintptr_t)base + sizeof(void *);
    uintptr_t aligned = (start + alignment - 1) & ~(uintptr_t)(alignment - 1);
    ((void **)aligned)[-1] = base;
    *out = (void *)aligned;
    return XRT_MANAGED_OK;
}
static void billing_free_aligned(XrtBillingOwner *owner, void *memory, size_t bytes,
    size_t alignment) {
    if (alignment <= alignof(XrMaxAlignment)) { billing_free(owner, memory, bytes); return; }
    size_t physical;
    XR_CHECK(aligned_allocation_bytes(bytes, alignment, &physical),
        "an over-aligned release mirrors its admitted allocation");
    void *base = ((void **)memory)[-1];
    owner->allocator.deallocate(owner->allocator.context, base, physical);
    billing_refund(owner, bytes);
}

XRAY_API XrtManagedStatus xrt_managed_metadata_allocate(XrtBillingOwner *owner, size_t bytes, void **empty) {
    if (!owner || !bytes || !empty || *empty) return XRT_MANAGED_ARGUMENT;
    return billing_allocate(owner, bytes, empty);
}
XRAY_API void xrt_managed_metadata_free(XrtBillingOwner *owner, void *memory, size_t bytes) {
    XR_CHECK(owner && memory && bytes, "metadata free consumes its exact allocation under a live billing reference");
    billing_free(owner, memory, bytes);
}
XRAY_API XrtManagedStatus xrt_managed_metadata_allocate_aligned(XrtBillingOwner *owner, size_t bytes,
    size_t alignment, void **empty) {
    if (!owner || !bytes || !empty || *empty) return XRT_MANAGED_ARGUMENT;
    return billing_allocate_aligned(owner, bytes, alignment, empty);
}
XRAY_API void xrt_managed_metadata_free_aligned(XrtBillingOwner *owner, void *memory, size_t bytes,
    size_t alignment) {
    XR_CHECK(owner && memory && bytes, "metadata free consumes its exact allocation under a live billing reference");
    billing_free_aligned(owner, memory, bytes, alignment);
}

static bool type_spec_valid(const XrtRuntimeTypeSpec *spec) {
    if (!spec || spec->fixed_size < sizeof(XrtManagedHeader) || spec->alignment < alignof(XrtManagedHeader) ||
        spec->alignment > XR_ALIGNMENT_LIMIT || (spec->alignment & (spec->alignment - 1)) ||
        !lease_valid(spec->code) || (spec->child_count && !spec->child_offsets) ||
        (spec->tail_child_count && !spec->tail_child_offsets) ||
        spec->child_count > (SIZE_MAX - sizeof(XrtRuntimeTypePlan)) / sizeof(size_t) ||
        spec->tail_child_count > (SIZE_MAX - sizeof(XrtRuntimeTypePlan)) / sizeof(size_t) -
            spec->child_count) return false;
    if (spec->stride ? spec->capacity_offset < sizeof(XrtManagedHeader) ||
        spec->fixed_size < sizeof(size_t) || spec->capacity_offset > spec->fixed_size - sizeof(size_t) ||
        spec->capacity_offset % alignof(size_t) : spec->capacity_offset != SIZE_MAX) return false;
    for (size_t i = 0; i < spec->child_count; ++i) {
        size_t offset = spec->child_offsets[i];
        if (offset < sizeof(XrtManagedHeader) || offset % alignof(void *) ||
            offset > spec->fixed_size - sizeof(void *) ||
            (i && offset - spec->child_offsets[i - 1] < sizeof(void *)) ||
            (i && offset < spec->child_offsets[i - 1]) ||
            (spec->stride && offset < spec->capacity_offset + sizeof(size_t) &&
                spec->capacity_offset < offset + sizeof(void *))) return false;
    }
    if (spec->tail_child_count && (spec->stride < sizeof(XrtManagedHeader *) ||
        spec->fixed_size % alignof(XrtManagedHeader *) ||
        spec->stride % alignof(XrtManagedHeader *))) return false;
    for (size_t i = 0; i < spec->tail_child_count; ++i) {
        size_t offset = spec->tail_child_offsets[i];
        if (offset % alignof(XrtManagedHeader *) || offset > spec->stride - sizeof(XrtManagedHeader *) ||
            (i && offset < spec->tail_child_offsets[i - 1]) ||
            (i && offset - spec->tail_child_offsets[i - 1] < sizeof(XrtManagedHeader *))) return false;
    }
    return true;
}
static bool prefix_count_offset_valid(const XrtRuntimeTypeSpec *spec, size_t offset) {
    if (offset == SIZE_MAX) return true;
    if (!spec->tail_child_count || offset < sizeof(XrtManagedHeader) ||
        offset > spec->fixed_size - sizeof(size_t) || offset % alignof(size_t) || offset == spec->capacity_offset)
        return false;
    for (size_t i = 0; i < spec->child_count; ++i) {
        size_t child = spec->child_offsets[i];
        if (child < offset + sizeof(size_t) && offset < child + sizeof(XrtManagedHeader *)) return false;
    }
    return true;
}
static XrtManagedStatus runtime_type_plan_new(XrtBillingOwner *owner, const XrtRuntimeTypeSpec *spec,
    size_t owned_tail_length_offset, XrtRuntimeTypePlan **output) {
    if (!owner || !output || *output || !type_spec_valid(spec)) return XRT_MANAGED_ARGUMENT;
    if (!prefix_count_offset_valid(spec, owned_tail_length_offset)) return XRT_MANAGED_ARGUMENT;
    if (!xrt_billing_owner_retain(owner)) return XRT_MANAGED_REFCOUNT;
    if (!lease_take(spec->code)) { xrt_billing_owner_drop(owner); return XRT_MANAGED_REFCOUNT; }
    size_t bytes = sizeof(XrtRuntimeTypePlan) + (spec->child_count + spec->tail_child_count) * sizeof(size_t);
    void *memory = NULL;
    XrtManagedStatus status = billing_allocate(owner, bytes, &memory);
    if (status != XRT_MANAGED_OK) { lease_drop(spec->code); xrt_billing_owner_drop(owner); return status; }
    XrtRuntimeTypePlan *plan = memory;
    atomic_init(&plan->references, -1);
    plan->billing = owner; plan->spec = *spec;
    plan->owned_tail_length_offset = owned_tail_length_offset;
    size_t *fields = (size_t *)(plan + 1);
    if (spec->child_count) memcpy(fields, spec->child_offsets, spec->child_count * sizeof(size_t));
    plan->spec.child_offsets = fields;
    fields += spec->child_count;
    if (spec->tail_child_count) memcpy(fields, spec->tail_child_offsets, spec->tail_child_count * sizeof(size_t));
    plan->spec.tail_child_offsets = fields;
    *output = plan;
    return XRT_MANAGED_OK;
}
XRAY_API XrtManagedStatus xrt_runtime_type_plan_new(XrtBillingOwner *owner,
    const XrtRuntimeTypeSpec *spec, XrtRuntimeTypePlan **output) {
    return runtime_type_plan_new(owner, spec, SIZE_MAX, output);
}
XR_FUNCDEF XrtManagedStatus xrt_runtime_type_plan_new_owned_prefix(XrtBillingOwner *owner,
    const XrtRuntimeTypeSpec *spec, size_t length_offset, XrtRuntimeTypePlan **output) {
    if (length_offset == SIZE_MAX) return XRT_MANAGED_ARGUMENT;
    return runtime_type_plan_new(owner, spec, length_offset, output);
}
XRAY_API bool xrt_runtime_type_plan_retain(XrtRuntimeTypePlan *plan) {
    return plan && xr_signed_rc_retain(&plan->references);
}
XRAY_API void xrt_runtime_type_plan_drop(XrtRuntimeTypePlan *plan) {
    if (!plan || !metadata_last(&plan->references)) return;
    XrtBillingOwner *owner = plan->billing;
    XrtManagedLease code = plan->spec.code;
    size_t bytes = sizeof(*plan) + (plan->spec.child_count + plan->spec.tail_child_count) * sizeof(size_t);
    billing_free(owner, plan, bytes);
    lease_drop(code);
    xrt_billing_owner_drop(owner);
}

static XrtManagedOps *ops_find(XrtBillingOwner *owner, const XrtRuntimeTypePlan *plan) {
    for (XrtManagedOps *ops = owner->weak_ops; ops; ops = ops->next)
        if (ops->plan == plan) return ops;
    return NULL;
}
static void ops_dispose(XrtManagedOps *ops) {
    XrtBillingOwner *owner = ops->billing;
    XrtRuntimeTypePlan *plan = ops->plan;
    billing_free(owner, ops, sizeof(*ops));
    xrt_runtime_type_plan_drop(plan);
    xrt_billing_owner_drop(owner);
}
XRAY_API XrtManagedStatus xrt_managed_ops_bind(XrtBillingOwner *owner,
    XrtRuntimeTypePlan *plan, XrtManagedOps **output) {
    if (!owner || !plan || !output || *output) return XRT_MANAGED_ARGUMENT;
    billing_lock(owner);
    XrtManagedOps *existing = ops_find(owner, plan);
    XrtManagedStatus status = atomic_load(&owner->closed) ? XRT_MANAGED_CLOSED :
        existing && !xr_signed_rc_retain(&existing->references) ? XRT_MANAGED_REFCOUNT : XRT_MANAGED_OK;
    billing_unlock(owner);
    if (status != XRT_MANAGED_OK || existing) {
        if (status == XRT_MANAGED_OK) *output = existing;
        return status;
    }
    if (!xrt_billing_owner_retain(owner)) return XRT_MANAGED_REFCOUNT;
    if (!xrt_runtime_type_plan_retain(plan)) { xrt_billing_owner_drop(owner); return XRT_MANAGED_REFCOUNT; }
    void *memory = NULL;
    status = billing_allocate(owner, sizeof(XrtManagedOps), &memory);
    if (status != XRT_MANAGED_OK) {
        xrt_runtime_type_plan_drop(plan); xrt_billing_owner_drop(owner); return status;
    }
    XrtManagedOps *candidate = memory;
    atomic_init(&candidate->references, -1);
    candidate->billing = owner; candidate->plan = plan;
    billing_lock(owner);
    existing = ops_find(owner, plan);
    status = atomic_load(&owner->closed) ? XRT_MANAGED_CLOSED :
        existing && !xr_signed_rc_retain(&existing->references) ? XRT_MANAGED_REFCOUNT : XRT_MANAGED_OK;
    if (status == XRT_MANAGED_OK && !existing) {
        candidate->next = owner->weak_ops; owner->weak_ops = candidate;
    }
    billing_unlock(owner);
    if (status != XRT_MANAGED_OK || existing) ops_dispose(candidate);
    if (status == XRT_MANAGED_OK) *output = existing ? existing : candidate;
    return status;
}
XRAY_API bool xrt_managed_ops_retain(XrtManagedOps *ops) {
    return ops && xr_signed_rc_retain(&ops->references);
}
XRAY_API void xrt_managed_ops_drop(XrtManagedOps *ops) {
    if (!ops) return;
    XrtBillingOwner *owner = ops->billing;
    billing_lock(owner);
    bool last = metadata_last(&ops->references);
    if (last) {
        XrtManagedOps **slot = &owner->weak_ops;
        while (*slot && *slot != ops) slot = &(*slot)->next;
        XR_CHECK(*slot == ops, "last descriptor release unlinks the same weak index entry");
        *slot = ops->next;
    }
    billing_unlock(owner);
    if (last) ops_dispose(ops);
}
XR_FUNCDEF XrtManagedStatus xrt_managed_producer_borrow(const XrtManagedHeader *owner,
    bool (*retain)(void *), void (*drop)(void *), const XrtRuntimeTypeSpec **empty) {
    return xrt_managed_ops_producer_borrow(owner ? owner->ops : NULL, retain, drop, empty);
}
XR_FUNCDEF XrtManagedStatus xrt_managed_ops_producer_borrow(const XrtManagedOps *ops,
    bool (*retain)(void *), void (*drop)(void *), const XrtRuntimeTypeSpec **empty) {
    if (!ops || !retain || !drop || !empty || *empty) return XRT_MANAGED_ARGUMENT;
    const XrtRuntimeTypeSpec *spec = &ops->plan->spec;
    if (spec->code.retain != retain || spec->code.drop != drop) return XRT_MANAGED_ARGUMENT;
    *empty = spec;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF bool xrt_managed_layout_matches(const XrtManagedHeader *owner, const XrtRuntimeTypeSpec *expected) {
    if (!owner || !owner->ops || !expected || (expected->child_count && !expected->child_offsets) ||
        (expected->tail_child_count && !expected->tail_child_offsets) ||
        owner->ops->plan->owned_tail_length_offset != SIZE_MAX) return false;
    const XrtRuntimeTypeSpec *actual = &owner->ops->plan->spec;
    return actual->fixed_size == expected->fixed_size && actual->alignment == expected->alignment &&
        actual->capacity_offset == expected->capacity_offset && actual->stride == expected->stride &&
        actual->child_count == expected->child_count && actual->tail_child_count == expected->tail_child_count &&
        actual->drop_payload == expected->drop_payload && actual->code.context == expected->code.context &&
        actual->code.retain == expected->code.retain && actual->code.drop == expected->code.drop &&
        (!actual->child_count || !memcmp(actual->child_offsets, expected->child_offsets,
            actual->child_count * sizeof(size_t))) &&
        (!actual->tail_child_count || !memcmp(actual->tail_child_offsets, expected->tail_child_offsets,
            actual->tail_child_count * sizeof(size_t)));
}

static bool allocation_extent(const XrtRuntimeTypePlan *plan, const XrtManagedHeader *value,
    size_t *bytes) {
    *bytes = plan->spec.fixed_size;
    if (!plan->spec.stride) return true;
    size_t capacity;
    memcpy(&capacity, (const unsigned char *)value + plan->spec.capacity_offset, sizeof(capacity));
    if (capacity > (SIZE_MAX - *bytes) / plan->spec.stride) return false;
    *bytes += capacity * plan->spec.stride;
    return true;
}
static bool allocation_children(const XrtRuntimeTypeSpec *spec, size_t bytes, size_t *count) {
    if (bytes < spec->fixed_size || (spec->stride ? (bytes - spec->fixed_size) % spec->stride :
        bytes != spec->fixed_size)) return false;
    size_t capacity = spec->stride ? (bytes - spec->fixed_size) / spec->stride : 0;
    if (spec->tail_child_count && capacity > (SIZE_MAX - spec->child_count) / spec->tail_child_count)
        return false;
    *count = spec->child_count + capacity * spec->tail_child_count;
    return true;
}
static bool allocation_owned_children(const XrtManagedHeader *value, size_t bytes, size_t *count) {
    const XrtRuntimeTypePlan *plan = value->ops->plan;
    const XrtRuntimeTypeSpec *spec = &plan->spec;
    if (!allocation_children(spec, bytes, count)) return false;
    if (plan->owned_tail_length_offset == SIZE_MAX) return true;
    size_t length;
    memcpy(&length, (const unsigned char *)value + plan->owned_tail_length_offset, sizeof(length));
    if (length > (bytes - spec->fixed_size) / spec->stride) return false;
    *count = spec->child_count + length * spec->tail_child_count;
    return true;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_grow_unique_scalar_tail(XrtManagedHeader **binding,
    XrtManagedOps *expected, size_t bytes, XrtManagedHeader **grown) {
    if (!binding || !*binding || !expected || !grown || *grown || binding == grown)
        return XRT_MANAGED_ARGUMENT;
    XrtManagedHeader *value = *binding;
    XrtBillingOwner *billing = expected->billing;
    if (value->ops != expected || billing->allocator.allocate != default_allocate ||
        xr_signed_rc_read(&value->references) != 1 ||
        atomic_load_explicit(&value->flags, memory_order_relaxed)) return XRT_MANAGED_OK;
    const XrtRuntimeTypeSpec *spec = &expected->plan->spec;
    size_t previous, children;
    if (!spec->stride || spec->child_count || spec->tail_child_count || spec->drop_payload ||
        spec->alignment > alignof(XrMaxAlignment) ||
        !allocation_extent(expected->plan, value, &previous) || previous >= bytes ||
        !allocation_children(spec, bytes, &children)) return XRT_MANAGED_ARGUMENT;

    /* Preserve the replacement factory's reference and full quota checks.
     * The live binding still owns its original leases through the resize. */
    if (!xrt_managed_ops_retain(expected)) return XRT_MANAGED_REFCOUNT;
    if (!xrt_billing_owner_retain(billing)) {
        xrt_managed_ops_drop(expected);
        return XRT_MANAGED_REFCOUNT;
    }
    XrtManagedStatus status = billing_reserve(billing, bytes);
    if (status == XRT_MANAGED_OK) {
        XrtManagedHeader *replacement = xr_realloc(value, bytes);
        if (!replacement) {
            billing_failed_allocation(billing, bytes);
            status = XRT_MANAGED_OOM;
        } else {
            size_t capacity = (bytes - spec->fixed_size) / spec->stride;
            xr_signed_rc_initialize(&replacement->references);
            atomic_init(&replacement->flags, 0);
            memcpy((unsigned char *)replacement + spec->capacity_offset, &capacity, sizeof(capacity));
            *binding = replacement;
            *grown = replacement;
            billing_refund(billing, previous);
        }
    }
    xrt_billing_owner_drop(billing);
    xrt_managed_ops_drop(expected);
    return status;
}
static const uint32_t retirement_cursor = UINT32_C(0x80000000);
_Static_assert(sizeof(size_t) <= sizeof(XrtManagedHeader *), "a dead child slot holds its cursor bytes");

/* The logical inventory concatenates the fixed fields with its owned rows.
 * Its first two slots provide the same continuation regardless of region. */
static void *child_field(XrtManagedHeader *value, size_t index) {
    const XrtRuntimeTypeSpec *spec = &value->ops->plan->spec;
    size_t offset;
    if (index < spec->child_count) offset = spec->child_offsets[index];
    else {
        index -= spec->child_count;
        offset = spec->fixed_size + index / spec->tail_child_count * spec->stride +
            spec->tail_child_offsets[index % spec->tail_child_count];
    }
    return (unsigned char *)value + offset;
}
static void child_store(XrtManagedHeader *value, size_t index, XrtManagedHeader *child) {
    memcpy(child_field(value, index), &child, sizeof(child));
}
static XrtManagedHeader *child_take(XrtManagedHeader *value, size_t index) {
    XrtManagedHeader *child;
    memcpy(&child, child_field(value, index), sizeof(child));
    child_store(value, index, NULL);
    return child;
}
static void cursor_store(XrtManagedHeader *value, size_t index) {
    memcpy(child_field(value, 1), &index, sizeof(index));
}
static bool value_release_last(XrtManagedHeader *value) {
    if (!value) return false;
    XrSignedRcRelease released = xr_signed_rc_release(&value->references);
    XR_CHECK(released != XR_SIGNED_RC_INVALID, "managed value reference count underflow");
    return released == XR_SIGNED_RC_LAST;
}

/* No continuation is stored before LAST. Slot zero then holds the parent. Slot
 * one remains a real owner until taken; only the phase bit permits cursor reads. */
static bool retirement_next(XrtManagedHeader *value, XrtManagedHeader *parent,
    bool entering, size_t children, XrtManagedHeader **child) {
    if (entering) {
        XR_CHECK(xr_signed_rc_read(&value->references) == 0, "only a last release owns retirement storage");
        XR_CHECK(!(atomic_load_explicit(&value->flags, memory_order_relaxed) & retirement_cursor),
            "live flags exclude the dead cursor phase");
        if (!children) return false;
        *child = child_take(value, 0);
        child_store(value, 0, parent);
        return true;
    }
    if (children < 2) return false;
    if (!(atomic_load_explicit(&value->flags, memory_order_relaxed) & retirement_cursor)) {
        *child = child_take(value, 1);
        cursor_store(value, 2);
        atomic_fetch_or_explicit(&value->flags, retirement_cursor, memory_order_relaxed);
        return true;
    }
    size_t index;
    memcpy(&index, child_field(value, 1), sizeof(index));
    XR_CHECK(index >= 2 && index <= children, "a dead cursor stays within the owned schema");
    if (index == children) return false;
    *child = child_take(value, index);
    cursor_store(value, index + 1);
    return true;
}

static XrtManagedHeader *retirement_finish(XrtManagedHeader *value, XrtManagedHeader *parent,
    size_t children, size_t bytes) {
    XrtManagedOps *ops = (XrtManagedOps *)value->ops;
    XrtBillingOwner *owner = ops->billing;
    const XrtRuntimeTypePlan *plan = ops->plan;
    if (children) parent = child_take(value, 0);
    if (children > 1) child_store(value, 1, NULL);
    atomic_fetch_and_explicit(&value->flags, ~retirement_cursor, memory_order_relaxed);
    if (plan->spec.stride) {
        size_t capacity = (bytes - plan->spec.fixed_size) / plan->spec.stride;
        memcpy((unsigned char *)value + plan->spec.capacity_offset, &capacity, sizeof(capacity));
    }
    if (plan->spec.drop_payload) plan->spec.drop_payload(value, plan->spec.code.context);
    billing_free_aligned(owner, value, bytes, plan->spec.alignment);
    xrt_managed_ops_drop(ops);
    xrt_billing_owner_drop(owner);
    return parent;
}
static void allocation_dispose(XrtManagedHeader *root, size_t root_bytes) {
    XrtManagedHeader *value = root, *parent = NULL;
    bool entering = true;
    while (value) {
        size_t bytes = root_bytes, children;
        if (value != root)
            XR_CHECK(allocation_extent(value->ops->plan, value, &bytes), "published physical extent remains representable");
        XR_CHECK(allocation_owned_children(value, bytes, &children),
            "physical extent bounds every fixed and repeated owned slot");
        XrtManagedHeader *child = NULL;
        if (retirement_next(value, parent, entering, children, &child)) {
            if (value_release_last(child)) {
                parent = value; value = child; entering = true;
            } else entering = false;
        } else {
            value = retirement_finish(value, parent, children, bytes);
            parent = NULL; entering = false;
        }
    }
}
static XrtManagedStatus managed_prepare(XrtManagedOps *ops, size_t bytes, bool clear_tail,
    XrtManagedPending *pending) {
    if (!ops || !pending || pending->value || pending->allocation_bytes) return XRT_MANAGED_ARGUMENT;
    const XrtRuntimeTypeSpec *spec = &ops->plan->spec;
    if (!clear_tail && (!spec->stride || spec->child_count || spec->tail_child_count || spec->drop_payload))
        return XRT_MANAGED_ARGUMENT;
    size_t children;
    if (!allocation_children(spec, bytes, &children)) return XRT_MANAGED_ARGUMENT;
    if (ops->plan->owned_tail_length_offset != SIZE_MAX) {
        clear_tail = false;
        children = spec->child_count;
    }
    if (!xrt_managed_ops_retain(ops)) return XRT_MANAGED_REFCOUNT;
    if (!xrt_billing_owner_retain(ops->billing)) { xrt_managed_ops_drop(ops); return XRT_MANAGED_REFCOUNT; }
    void *memory = NULL;
    XrtManagedStatus status = billing_allocate_aligned(ops->billing, bytes, spec->alignment, &memory);
    if (status != XRT_MANAGED_OK) {
        xrt_billing_owner_drop(ops->billing); xrt_managed_ops_drop(ops); return status;
    }
    memset(memory, 0, clear_tail ? bytes : spec->fixed_size);
    XrtManagedHeader *value = memory;
    xr_signed_rc_initialize(&value->references);
    atomic_init(&value->flags, 0);
    value->ops = ops;
    for (size_t i = 0; i < children; ++i) child_store(value, i, NULL);
    if (spec->stride) {
        size_t capacity = (bytes - spec->fixed_size) / spec->stride;
        memcpy((unsigned char *)memory + spec->capacity_offset, &capacity, sizeof(capacity));
    }
    *pending = (XrtManagedPending){value, bytes};
    return XRT_MANAGED_OK;
}
XRAY_API XrtManagedStatus xrt_managed_prepare(XrtManagedOps *ops, size_t bytes, XrtManagedPending *pending) {
    return managed_prepare(ops, bytes, true, pending);
}
XR_FUNCDEF XrtManagedStatus xrt_managed_prepare_scalar_tail(XrtManagedOps *ops, size_t bytes,
    XrtManagedPending *pending) {
    return managed_prepare(ops, bytes, false, pending);
}
XRAY_API XrtManagedStatus xrt_managed_commit(XrtManagedPending *pending, XrtManagedHeader **output) {
    if (!pending || !pending->value || !output || output == &pending->value || *output)
        return XRT_MANAGED_ARGUMENT;
    size_t bytes;
    if (!allocation_extent(pending->value->ops->plan, pending->value, &bytes) ||
        bytes != pending->allocation_bytes) return XRT_MANAGED_ARGUMENT;
    size_t children;
    if (pending->value->ops->plan->owned_tail_length_offset != SIZE_MAX &&
        !allocation_owned_children(pending->value, bytes, &children)) return XRT_MANAGED_ARGUMENT;
    *output = pending->value;
    *pending = (XrtManagedPending){0};
    return XRT_MANAGED_OK;
}
XRAY_API void xrt_managed_abort(XrtManagedPending *pending) {
    if (!pending || !pending->value) return;
    XrtManagedPending owned = *pending;
    *pending = (XrtManagedPending){0};
    XR_CHECK(value_release_last(owned.value), "a pending token owns the last unpublished reference");
    allocation_dispose(owned.value, owned.allocation_bytes);
}
XRAY_API bool xrt_managed_retain(XrtManagedHeader *value) {
    return value && xr_signed_rc_retain(&value->references);
}
XRAY_API void xrt_managed_drop(XrtManagedHeader *value) {
    if (!value_release_last(value)) return;
    size_t bytes;
    XR_CHECK(allocation_extent(value->ops->plan, value, &bytes), "published physical extent remains representable");
    allocation_dispose(value, bytes);
}
