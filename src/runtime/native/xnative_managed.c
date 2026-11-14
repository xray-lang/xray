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

XR_FUNCDEF XrtManagedStatus xrt_billing_owner_new(const XrtManagedAllocator *input,
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
XR_FUNCDEF bool xrt_billing_owner_retain(XrtBillingOwner *owner) {
    return owner && xr_signed_rc_retain(&owner->references);
}
XR_FUNCDEF void xrt_billing_owner_drop(XrtBillingOwner *owner) {
    if (!owner || !metadata_last(&owner->references)) return;
    XR_CHECK(!owner->weak_ops && atomic_load(&owner->bytes) == sizeof(*owner) &&
        atomic_load(&owner->blocks) == 1, "the last billing lease follows every physical free");
    XrtManagedAllocator allocator = owner->allocator;
    allocator.deallocate(allocator.context, owner, sizeof(*owner));
    lease_drop(allocator.lease);
}
XR_FUNCDEF void xrt_billing_owner_close(XrtBillingOwner *owner) {
    billing_lock(owner);
    atomic_store_explicit(&owner->closed, true, memory_order_release);
    billing_unlock(owner);
}
XR_FUNCDEF void xrt_billing_owner_fail_after(XrtBillingOwner *owner, size_t count) {
    billing_lock(owner);
    atomic_store_explicit(&owner->fail_after, count, memory_order_relaxed);
    billing_unlock(owner);
}
XR_FUNCDEF XrtBillingStats xrt_billing_owner_stats(const XrtBillingOwner *owner) {
    return (XrtBillingStats){atomic_load(&owner->bytes), atomic_load(&owner->blocks),
        atomic_load(&owner->allocations), atomic_load(&owner->fail_after), atomic_load(&owner->closed)};
}

/* Only the reservation holds the index lock. Provider calls can reenter binding
 * or release independently owned metadata; in-flight reservations stay charged. */
static XrtManagedStatus billing_allocate(XrtBillingOwner *owner, size_t bytes, void **out) {
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
    if (status != XRT_MANAGED_OK) return status;
    void *memory = owner->allocator.allocate(owner->allocator.context, bytes);
    if (!memory) {
        atomic_fetch_sub(&owner->bytes, bytes);
        atomic_fetch_sub(&owner->blocks, 1);
        atomic_fetch_sub(&owner->allocations, 1);
        return XRT_MANAGED_OOM;
    }
    *out = memory;
    return XRT_MANAGED_OK;
}
static void billing_free(XrtBillingOwner *owner, void *memory, size_t bytes) {
    owner->allocator.deallocate(owner->allocator.context, memory, bytes);
    size_t blocks = atomic_fetch_sub(&owner->blocks, 1);
    size_t held = atomic_fetch_sub(&owner->bytes, bytes);
    XR_CHECK(blocks > 1 && held >= bytes + sizeof(*owner), "every allocation refunds its original billing owner");
}

static bool type_spec_valid(const XrtRuntimeTypeSpec *spec) {
    if (!spec || spec->fixed_size < sizeof(XrtManagedHeader) || spec->alignment < alignof(XrtManagedHeader) ||
        spec->alignment > alignof(max_align_t) || (spec->alignment & (spec->alignment - 1)) ||
        !lease_valid(spec->code) || (spec->child_count && !spec->child_offsets) ||
        spec->child_count > (SIZE_MAX - sizeof(XrtRuntimeTypePlan)) / sizeof(size_t)) return false;
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
    return true;
}
XR_FUNCDEF XrtManagedStatus xrt_runtime_type_plan_new(XrtBillingOwner *owner,
    const XrtRuntimeTypeSpec *spec, XrtRuntimeTypePlan **output) {
    if (!owner || !output || *output || !type_spec_valid(spec)) return XRT_MANAGED_ARGUMENT;
    if (!xrt_billing_owner_retain(owner)) return XRT_MANAGED_REFCOUNT;
    if (!lease_take(spec->code)) { xrt_billing_owner_drop(owner); return XRT_MANAGED_REFCOUNT; }
    size_t bytes = sizeof(XrtRuntimeTypePlan) + spec->child_count * sizeof(size_t);
    void *memory = NULL;
    XrtManagedStatus status = billing_allocate(owner, bytes, &memory);
    if (status != XRT_MANAGED_OK) { lease_drop(spec->code); xrt_billing_owner_drop(owner); return status; }
    XrtRuntimeTypePlan *plan = memory;
    atomic_init(&plan->references, -1);
    plan->billing = owner; plan->spec = *spec;
    size_t *fields = (size_t *)(plan + 1);
    if (spec->child_count) memcpy(fields, spec->child_offsets, spec->child_count * sizeof(size_t));
    plan->spec.child_offsets = fields;
    *output = plan;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF bool xrt_runtime_type_plan_retain(XrtRuntimeTypePlan *plan) {
    return plan && xr_signed_rc_retain(&plan->references);
}
XR_FUNCDEF void xrt_runtime_type_plan_drop(XrtRuntimeTypePlan *plan) {
    if (!plan || !metadata_last(&plan->references)) return;
    XrtBillingOwner *owner = plan->billing;
    XrtManagedLease code = plan->spec.code;
    size_t bytes = sizeof(*plan) + plan->spec.child_count * sizeof(size_t);
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
XR_FUNCDEF XrtManagedStatus xrt_managed_ops_bind(XrtBillingOwner *owner,
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
XR_FUNCDEF bool xrt_managed_ops_retain(XrtManagedOps *ops) {
    return ops && xr_signed_rc_retain(&ops->references);
}
XR_FUNCDEF void xrt_managed_ops_drop(XrtManagedOps *ops) {
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
static void payload_drop(XrtManagedHeader *value, const XrtRuntimeTypePlan *plan) {
    for (size_t i = 0; i < plan->spec.child_count; ++i) {
        void *field = (unsigned char *)value + plan->spec.child_offsets[i];
        XrtManagedHeader *child;
        memcpy(&child, field, sizeof(child));
        XrtManagedHeader *empty = NULL;
        memcpy(field, &empty, sizeof(empty));
        xrt_managed_drop(child);
    }
    if (plan->spec.drop_payload) plan->spec.drop_payload(value, plan->spec.code.context);
}
static void allocation_dispose(XrtManagedHeader *value, size_t bytes) {
    XrtManagedOps *ops = (XrtManagedOps *)value->ops;
    XrtBillingOwner *owner = ops->billing;
    payload_drop(value, ops->plan);
    billing_free(owner, value, bytes);
    xrt_managed_ops_drop(ops);
    xrt_billing_owner_drop(owner);
}
XR_FUNCDEF XrtManagedStatus xrt_managed_prepare(XrtManagedOps *ops, size_t bytes, XrtManagedPending *pending) {
    if (!ops || !pending || pending->value || pending->allocation_bytes) return XRT_MANAGED_ARGUMENT;
    const XrtRuntimeTypeSpec *spec = &ops->plan->spec;
    if (bytes < spec->fixed_size || (spec->stride ? (bytes - spec->fixed_size) % spec->stride :
        bytes != spec->fixed_size)) return XRT_MANAGED_ARGUMENT;
    if (!xrt_managed_ops_retain(ops)) return XRT_MANAGED_REFCOUNT;
    if (!xrt_billing_owner_retain(ops->billing)) { xrt_managed_ops_drop(ops); return XRT_MANAGED_REFCOUNT; }
    void *memory = NULL;
    XrtManagedStatus status = billing_allocate(ops->billing, bytes, &memory);
    if (status != XRT_MANAGED_OK) {
        xrt_billing_owner_drop(ops->billing); xrt_managed_ops_drop(ops); return status;
    }
    memset(memory, 0, bytes);
    XrtManagedHeader *value = memory;
    for (size_t i = 0; i < spec->child_count; ++i) {
        XrtManagedHeader *empty = NULL;
        memcpy((unsigned char *)memory + spec->child_offsets[i], &empty, sizeof(empty));
    }
    xr_signed_rc_initialize(&value->references);
    value->ops = ops;
    if (spec->stride) {
        size_t capacity = (bytes - spec->fixed_size) / spec->stride;
        memcpy((unsigned char *)memory + spec->capacity_offset, &capacity, sizeof(capacity));
    }
    *pending = (XrtManagedPending){value, bytes};
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_commit(XrtManagedPending *pending, XrtManagedHeader **output) {
    if (!pending || !pending->value || !output || output == &pending->value || *output)
        return XRT_MANAGED_ARGUMENT;
    size_t bytes;
    if (!allocation_extent(pending->value->ops->plan, pending->value, &bytes) ||
        bytes != pending->allocation_bytes) return XRT_MANAGED_ARGUMENT;
    *output = pending->value;
    *pending = (XrtManagedPending){0};
    return XRT_MANAGED_OK;
}
XR_FUNCDEF void xrt_managed_abort(XrtManagedPending *pending) {
    if (!pending || !pending->value) return;
    XrtManagedPending owned = *pending;
    *pending = (XrtManagedPending){0};
    allocation_dispose(owned.value, owned.allocation_bytes);
}
XR_FUNCDEF bool xrt_managed_retain(XrtManagedHeader *value) {
    return value && xr_signed_rc_retain(&value->references);
}
XR_FUNCDEF void xrt_managed_drop(XrtManagedHeader *value) {
    if (!value) return;
    XrSignedRcRelease released = xr_signed_rc_release(&value->references);
    XR_CHECK(released != XR_SIGNED_RC_INVALID, "managed value reference count underflow");
    if (released != XR_SIGNED_RC_LAST) return;
    size_t bytes;
    XR_CHECK(allocation_extent(value->ops->plan, value, &bytes), "published physical extent remains representable");
    allocation_dispose(value, bytes);
}
