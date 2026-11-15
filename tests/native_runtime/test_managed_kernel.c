/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_managed_kernel.c - Dynamic allocator, schema and physical-owner retirement
 */
#include "runtime/native/xnative_managed.h"
#include "base/xmalloc.h"
#include "os/os_thread.h"
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define OK(c) CHECK((c) == XRT_MANAGED_OK)

typedef struct Audit {
    _Atomic(size_t) bytes, blocks, finalized, callbacks, fail_after, allocation_calls;
    _Atomic(bool) reenter;
    XrtBillingOwner *owner;
    XrtRuntimeTypePlan *plan;
    XrtManagedOps *saved;
} Audit;
typedef struct Provider { _Atomic(int32_t) references; Audit *audit; } Provider;
typedef struct Payload {
    XrtManagedHeader header;
    XrtManagedHeader *child;
    size_t capacity, length;
} Payload;

static Provider *provider_new(Audit *audit) {
    Provider *provider = xr_malloc(sizeof(*provider));
    CHECK(provider);
    atomic_init(&provider->references, -1);
    provider->audit = audit;
    atomic_fetch_add(&audit->bytes, sizeof(*provider));
    atomic_fetch_add(&audit->blocks, 1);
    atomic_store(&audit->fail_after, SIZE_MAX);
    return provider;
}
static bool provider_retain(void *context) {
    Provider *provider = context;
    return xr_signed_rc_retain(&provider->references);
}
static void provider_drop(void *context) {
    Provider *provider = context;
    XrSignedRcRelease release = xr_signed_rc_release(&provider->references);
    CHECK(release != XR_SIGNED_RC_INVALID);
    if (release != XR_SIGNED_RC_LAST) return;
    Audit *audit = provider->audit;
    CHECK(atomic_fetch_sub(&audit->bytes, sizeof(*provider)) == sizeof(*provider));
    CHECK(atomic_fetch_sub(&audit->blocks, 1) == 1);
    xr_free(provider);
    atomic_fetch_add(&audit->finalized, 1);
}
static void *provider_allocate(void *context, size_t bytes) {
    Audit *audit = ((Provider *)context)->audit;
    atomic_fetch_add(&audit->allocation_calls, 1);
    size_t budget = atomic_load(&audit->fail_after);
    while (budget != SIZE_MAX) {
        if (!budget) return NULL;
        if (atomic_compare_exchange_weak(&audit->fail_after, &budget, budget - 1)) break;
    }
    if (atomic_exchange(&audit->reenter, false))
        OK(xrt_managed_ops_bind(audit->owner, audit->plan, &audit->saved));
    void *memory = xr_malloc(bytes);
    if (memory) { atomic_fetch_add(&audit->bytes, bytes); atomic_fetch_add(&audit->blocks, 1); }
    return memory;
}
static void provider_deallocate(void *context, void *memory, size_t bytes) {
    Audit *audit = ((Provider *)context)->audit;
    CHECK(atomic_fetch_sub(&audit->bytes, bytes) >= bytes);
    CHECK(atomic_fetch_sub(&audit->blocks, 1) > 1);
    xr_free(memory);
}
static XrtManagedAllocator allocator_for(Provider *provider) {
    return (XrtManagedAllocator){provider, provider_allocate, provider_deallocate,
        {provider, provider_retain, provider_drop}};
}
static void payload_callback(XrtManagedHeader *value, void *context) {
    Provider *provider = context;
    CHECK(!((Payload *)value)->child && atomic_load(&provider->audit->bytes) > 0);
    atomic_fetch_add(&provider->audit->callbacks, 1);
}
static XrtRuntimeTypeSpec payload_spec(Provider *provider, const size_t *fields, size_t count) {
    return (XrtRuntimeTypeSpec){.fixed_size=sizeof(Payload), .alignment=alignof(Payload),
        .capacity_offset=offsetof(Payload, capacity), .stride=1, .child_offsets=fields, .child_count=count,
        .drop_payload=provider ? payload_callback : NULL,
        .code=provider ? (XrtManagedLease){provider, provider_retain, provider_drop} : (XrtManagedLease){0}};
}
static void balanced(Audit *audit) {
    CHECK(!atomic_load(&audit->bytes) && !atomic_load(&audit->blocks) && atomic_load(&audit->finalized) == 1);
}
static void *retire_worker(void *value) {
    xrt_managed_drop(value);
    return NULL;
}

static XrtBillingOwner *default_owner_borrow;
static XrtBillingStats default_baseline;
static unsigned default_callbacks;
static void default_payload_callback(XrtManagedHeader *value, void *context) {
    Payload *payload = (Payload *)value;
    CHECK(!context && !payload->child && payload->capacity > payload->length);
    ++default_callbacks;
    if (payload->length == 1) {
        XrtBillingStats stats = xrt_billing_owner_stats(default_owner_borrow);
        CHECK(default_callbacks == 2 && stats.closed);
        CHECK(stats.bytes == default_baseline.bytes + sizeof(Payload) + 129 &&
            stats.blocks == default_baseline.blocks + 1);
        default_owner_borrow = NULL;
    }
}
static void default_provider_retirement(void) {
    XrtBillingOwner *owner = NULL;
    OK(xrt_billing_owner_new(NULL, SIZE_MAX, &owner));
    size_t field = offsetof(Payload, child);
    XrtRuntimeTypeSpec spec = payload_spec(NULL, &field, 1);
    spec.drop_payload = default_payload_callback;
    CHECK(!spec.code.context && !spec.code.retain && !spec.code.drop);
    XrtRuntimeTypePlan *plan = NULL;
    XrtManagedOps *ops = NULL;
    OK(xrt_runtime_type_plan_new(owner, &spec, &plan));
    OK(xrt_managed_ops_bind(owner, plan, &ops));
    default_baseline = xrt_billing_owner_stats(owner);
    XrtManagedPending parent = {0}, child = {0};
    OK(xrt_managed_prepare(ops, sizeof(Payload) + 129, &parent));
    OK(xrt_managed_prepare(ops, sizeof(Payload) + 9, &child));
    CHECK(((Payload *)parent.value)->capacity == 129 && ((Payload *)child.value)->capacity == 9);
    ((Payload *)parent.value)->length = 1;
    ((Payload *)child.value)->length = 2;
    XrtManagedHeader *leaf = NULL, *root = NULL;
    OK(xrt_managed_commit(&child, &leaf));
    ((Payload *)parent.value)->child = leaf;
    OK(xrt_managed_commit(&parent, &root));
    xrt_managed_ops_drop(ops);
    xrt_runtime_type_plan_drop(plan);
    xrt_billing_owner_close(owner);
    default_owner_borrow = owner;
    xrt_billing_owner_drop(owner);
    CHECK(!default_callbacks && xrt_billing_owner_stats(default_owner_borrow).closed);
    xrt_managed_drop(root);
    CHECK(default_callbacks == 2 && !default_owner_borrow);
}

static void dynamic_retirement(void) {
    Audit audit = {0};
    Provider *provider = provider_new(&audit);
    XrtManagedAllocator allocator = allocator_for(provider);
    XrtBillingOwner *left = NULL, *right = NULL;
    OK(xrt_billing_owner_new(&allocator, SIZE_MAX, &left));
    OK(xrt_billing_owner_new(&allocator, SIZE_MAX, &right));
    size_t field = offsetof(Payload, child);
    XrtRuntimeTypeSpec spec = payload_spec(provider, &field, 1);
    XrtRuntimeTypePlan *plan = NULL;
    OK(xrt_runtime_type_plan_new(right, &spec, &plan));
    field = SIZE_MAX; /* The plan owns the original physical field inventory. */
    XrtManagedOps *a = NULL, *b = NULL;
    OK(xrt_managed_ops_bind(left, plan, &a));
    OK(xrt_managed_ops_bind(right, plan, &b));
    XrtManagedPending parent = {0}, child = {0};
    OK(xrt_managed_prepare(a, sizeof(Payload) + 129, &parent));
    OK(xrt_managed_prepare(b, sizeof(Payload) + 67, &child));
    XrtManagedHeader *leaf = NULL, *root = NULL;
    OK(xrt_managed_commit(&child, &leaf));
    ((Payload *)parent.value)->child = leaf;
    ((Payload *)parent.value)->length = 1;
    OK(xrt_managed_commit(&parent, &root));
    CHECK(((Payload *)root)->capacity == 129);
    xrt_managed_ops_drop(a); xrt_managed_ops_drop(b);
    xrt_runtime_type_plan_drop(plan);
    xrt_billing_owner_close(left); xrt_billing_owner_close(right);
    xrt_billing_owner_drop(left); xrt_billing_owner_drop(right);
    provider_drop(provider);
    CHECK(!atomic_load(&audit.finalized) && atomic_load(&audit.callbacks) == 0);
    CHECK(xrt_billing_owner_stats(left).closed && xrt_billing_owner_stats(right).closed);
    xr_thread_t retire;
    CHECK(xr_thread_create(&retire, retire_worker, root));
    CHECK(xr_thread_join(retire, NULL) == 0);
    CHECK(atomic_load(&audit.callbacks) == 2);
    balanced(&audit);
}

static void failure_atomicity(void) {
    Audit audit = {0};
    Provider *provider = provider_new(&audit);
    XrtManagedAllocator allocator = allocator_for(provider);
    XrtBillingOwner *owner = NULL;
    atomic_store(&audit.fail_after, 0);
    CHECK(xrt_billing_owner_new(&allocator, SIZE_MAX, &owner) == XRT_MANAGED_OOM && !owner);
    atomic_store(&audit.fail_after, SIZE_MAX);
    OK(xrt_billing_owner_new(&allocator, SIZE_MAX, &owner));
    XrtBillingStats baseline = xrt_billing_owner_stats(owner);
    XrtRuntimeTypeSpec spec = payload_spec(NULL, NULL, 0);
    XrtRuntimeTypePlan *plan = NULL;
    xrt_billing_owner_fail_after(owner, 0);
    CHECK(xrt_runtime_type_plan_new(owner, &spec, &plan) == XRT_MANAGED_OOM && !plan);
    CHECK(xrt_billing_owner_stats(owner).bytes == baseline.bytes);
    xrt_billing_owner_fail_after(owner, SIZE_MAX);
    OK(xrt_runtime_type_plan_new(owner, &spec, &plan));
    XrtManagedOps *ops = NULL;
    baseline = xrt_billing_owner_stats(owner);
    xrt_billing_owner_fail_after(owner, 0);
    CHECK(xrt_managed_ops_bind(owner, plan, &ops) == XRT_MANAGED_OOM && !ops);
    CHECK(xrt_billing_owner_stats(owner).bytes == baseline.bytes);
    xrt_billing_owner_fail_after(owner, SIZE_MAX);
    OK(xrt_managed_ops_bind(owner, plan, &ops));
    XrtManagedPending pending = {0};
    baseline = xrt_billing_owner_stats(owner);
    xrt_billing_owner_fail_after(owner, 0);
    CHECK(xrt_managed_prepare(ops, sizeof(Payload) + 100, &pending) == XRT_MANAGED_OOM && !pending.value);
    xrt_billing_owner_fail_after(owner, SIZE_MAX);
    atomic_store(&audit.fail_after, 0);
    CHECK(xrt_managed_prepare(ops, sizeof(Payload) + 100, &pending) == XRT_MANAGED_OOM && !pending.value);
    CHECK(xrt_billing_owner_stats(owner).bytes == baseline.bytes &&
        xrt_billing_owner_stats(owner).blocks == baseline.blocks &&
        xrt_billing_owner_stats(owner).allocations == baseline.allocations);
    atomic_store(&audit.fail_after, SIZE_MAX);
    OK(xrt_managed_prepare(ops, sizeof(Payload) + 100, &pending));
    XrtManagedHeader *result = NULL;
    CHECK(xrt_managed_commit(&pending, &pending.value) == XRT_MANAGED_ARGUMENT && pending.value);
    ((Payload *)pending.value)->capacity = SIZE_MAX;
    CHECK(xrt_managed_commit(&pending, &result) == XRT_MANAGED_ARGUMENT && !result && pending.value);
    xrt_managed_abort(&pending);
    CHECK(!pending.value && xrt_billing_owner_stats(owner).bytes == baseline.bytes);
    OK(xrt_managed_prepare(ops, sizeof(Payload) + 2, &pending));
    xrt_billing_owner_close(owner);
    OK(xrt_managed_commit(&pending, &result));
    CHECK(xrt_managed_prepare(ops, sizeof(Payload), &pending) == XRT_MANAGED_CLOSED && !pending.value);
    xrt_managed_drop(result);
    xrt_managed_ops_drop(ops); xrt_runtime_type_plan_drop(plan);
    xrt_billing_owner_drop(owner); provider_drop(provider);
    balanced(&audit);
}

static void reentrant_binding(void) {
    Audit audit = {0};
    Provider *provider = provider_new(&audit);
    XrtManagedAllocator allocator = allocator_for(provider);
    XrtBillingOwner *owner = NULL;
    OK(xrt_billing_owner_new(&allocator, SIZE_MAX, &owner));
    XrtRuntimeTypeSpec spec = payload_spec(NULL, NULL, 0);
    XrtRuntimeTypePlan *plan = NULL;
    OK(xrt_runtime_type_plan_new(owner, &spec, &plan));
    XrtBillingStats baseline = xrt_billing_owner_stats(owner);
    audit.owner = owner; audit.plan = plan;
    atomic_store(&audit.reenter, true);
    XrtManagedOps *ops = NULL;
    OK(xrt_managed_ops_bind(owner, plan, &ops));
    CHECK(ops == audit.saved && xrt_billing_owner_stats(owner).blocks == baseline.blocks + 1 &&
        xrt_billing_owner_stats(owner).allocations == baseline.allocations + 2);
    xrt_managed_ops_drop(ops); xrt_managed_ops_drop(audit.saved);
    CHECK(xrt_billing_owner_stats(owner).bytes == baseline.bytes);
    xrt_runtime_type_plan_drop(plan); xrt_billing_owner_drop(owner); provider_drop(provider);
    balanced(&audit);
}

typedef struct Worker { XrtBillingOwner *owner; XrtRuntimeTypePlan *plan; } Worker;
static void *bind_worker(void *opaque) {
    Worker *worker = opaque;
    for (unsigned i = 0; i < 250; ++i) {
        XrtManagedOps *ops = NULL;
        OK(xrt_managed_ops_bind(worker->owner, worker->plan, &ops));
        XrtManagedPending pending = {0};
        OK(xrt_managed_prepare(ops, sizeof(Payload) + i, &pending));
        XrtManagedHeader *value = NULL;
        OK(xrt_managed_commit(&pending, &value));
        xrt_managed_ops_drop(ops);
        xrt_managed_drop(value);
    }
    return NULL;
}
static void concurrent_final_free(void) {
    Audit audit = {0};
    Provider *provider = provider_new(&audit);
    XrtManagedAllocator allocator = allocator_for(provider);
    XrtBillingOwner *owner = NULL;
    OK(xrt_billing_owner_new(&allocator, SIZE_MAX, &owner));
    XrtRuntimeTypeSpec spec = payload_spec(NULL, NULL, 0);
    XrtRuntimeTypePlan *plan = NULL;
    OK(xrt_runtime_type_plan_new(owner, &spec, &plan));
    XrtBillingStats baseline = xrt_billing_owner_stats(owner);
    Worker worker = {owner, plan};
    xr_thread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) CHECK(xr_thread_create(&threads[i], bind_worker, &worker));
    for (unsigned i = 0; i < 4; ++i) CHECK(xr_thread_join(threads[i], NULL) == 0);
    CHECK(xrt_billing_owner_stats(owner).bytes == baseline.bytes &&
        xrt_billing_owner_stats(owner).blocks == baseline.blocks);
    xrt_runtime_type_plan_drop(plan); xrt_billing_owner_drop(owner); provider_drop(provider);
    balanced(&audit);
}

static void deep_chain_retirement(void) {
    typedef struct Node { XrtManagedHeader header; XrtManagedHeader *child; } Node;
    const size_t count = 100000;
    size_t field = offsetof(Node, child);
    XrtRuntimeTypeSpec spec = {.fixed_size=sizeof(Node), .alignment=alignof(Node),
        .capacity_offset=SIZE_MAX, .child_offsets=&field, .child_count=1};
    XrtBillingOwner *owner = NULL;
    XrtRuntimeTypePlan *plan = NULL;
    XrtManagedOps *ops = NULL;
    OK(xrt_billing_owner_new(NULL, SIZE_MAX, &owner));
    OK(xrt_runtime_type_plan_new(owner, &spec, &plan));
    OK(xrt_managed_ops_bind(owner, plan, &ops));
    XrtBillingStats baseline = xrt_billing_owner_stats(owner);
    XrtManagedHeader *head = NULL;
    for (size_t i = 0; i < count; ++i) {
        XrtManagedPending pending = {0};
        OK(xrt_managed_prepare(ops, sizeof(Node), &pending));
        CHECK(!((Node *)pending.value)->child);
        ((Node *)pending.value)->child = head;
        head = NULL;
        OK(xrt_managed_commit(&pending, &head));
    }
    size_t actual = 0;
    for (Node *node = (Node *)head; node; node = (Node *)node->child) {
        CHECK(++actual <= count && xr_signed_rc_read(&node->header.references) == 1);
    }
    CHECK(actual == count && xrt_billing_owner_stats(owner).blocks == baseline.blocks + count &&
        xrt_billing_owner_stats(owner).bytes == baseline.bytes + count * sizeof(Node));
    xrt_managed_ops_drop(ops);
    xrt_runtime_type_plan_drop(plan);
    xrt_billing_owner_close(owner);
    xrt_billing_owner_drop(owner);
    puts("built and verified 100000 single-owner nodes");
    CHECK(fflush(stdout) == 0);
    xrt_managed_drop(head);
    puts("deep managed chain passed");
}

typedef struct RetireNode {
    XrtManagedHeader header;
    size_t serial, charge;
    XrtManagedHeader *first;
    uint64_t gap[3];
    XrtManagedHeader *second;
    size_t capacity;
    uint64_t gap_after;
    XrtManagedHeader *third;
} RetireNode;
typedef struct RetirementAudit {
    Audit provider;
    XrtBillingOwner *owner;
    XrtBillingStats metadata;
    size_t bytes, blocks, callbacks, allocations, provider_allocations;
    XrtManagedHeader *shared;
} RetirementAudit;
static const uint32_t payload_flags = UINT32_C(0x12345678);

static void retirement_callback(XrtManagedHeader *value, void *context) {
    Provider *provider = context;
    RetirementAudit *audit = (RetirementAudit *)provider->audit;
    RetireNode *node = (RetireNode *)value;
    CHECK(xr_signed_rc_read(&value->references) == 0 && value->flags == payload_flags);
    CHECK(!node->first && !node->second && !node->third);
    CHECK(node->gap[0] == UINT64_C(0x123456789abcdef0) &&
        node->gap[1] == node->serial && node->gap[2] == ~node->serial &&
        node->gap_after == UINT64_C(0xfedcba9876543210));
    CHECK(node->serial == audit->callbacks);
    if (audit->shared && !node->serial) {
        CHECK(xr_signed_rc_read(&audit->shared->references) == 1 &&
            audit->shared->flags == payload_flags);
    }
    if (value == audit->shared) audit->shared = NULL;
    XrtBillingStats stats = xrt_billing_owner_stats(audit->owner);
    CHECK(stats.closed && !stats.fail_after && stats.allocations == audit->allocations);
    CHECK(atomic_load(&audit->provider.allocation_calls) == audit->provider_allocations);
    CHECK(stats.bytes == audit->metadata.bytes + audit->bytes &&
        stats.blocks == audit->metadata.blocks + audit->blocks);
    CHECK(atomic_load(&audit->provider.bytes) == stats.bytes + sizeof(*provider) &&
        atomic_load(&audit->provider.blocks) == stats.blocks + 1);
    CHECK(audit->bytes >= node->charge && audit->blocks > 0);
    audit->bytes -= node->charge;
    --audit->blocks;
    ++audit->callbacks;
}
static XrtRuntimeTypeSpec retirement_spec(Provider *provider, size_t count) {
    static const size_t fields[] = {offsetof(RetireNode, first),
        offsetof(RetireNode, second), offsetof(RetireNode, third)};
    CHECK(count <= 3);
    return (XrtRuntimeTypeSpec){.fixed_size=sizeof(RetireNode), .alignment=alignof(RetireNode),
        .capacity_offset=offsetof(RetireNode, capacity), .stride=3, .child_offsets=fields,
        .child_count=count, .drop_payload=retirement_callback, .code={provider, provider_retain, provider_drop}};
}
static void retirement_prepare(XrtManagedOps *ops, size_t serial, XrtManagedPending *pending) {
    size_t bytes = sizeof(RetireNode) + (7 + serial % 11) * 3;
    OK(xrt_managed_prepare(ops, bytes, pending));
    RetireNode *node = (RetireNode *)pending->value;
    CHECK(!node->first && !node->second && !node->third);
    node->header.flags = payload_flags;
    node->serial = serial; node->charge = bytes;
    node->gap[0] = UINT64_C(0x123456789abcdef0);
    node->gap[1] = serial; node->gap[2] = ~serial;
    node->gap_after = UINT64_C(0xfedcba9876543210);
}
static XrtManagedHeader *retirement_node(XrtManagedOps *ops, size_t serial,
    XrtManagedHeader *first, XrtManagedHeader *second, XrtManagedHeader *third) {
    XrtManagedPending pending = {0};
    retirement_prepare(ops, serial, &pending);
    RetireNode *node = (RetireNode *)pending.value;
    node->first = first; node->second = second; node->third = third;
    XrtManagedHeader *value = NULL;
    OK(xrt_managed_commit(&pending, &value));
    return value;
}
static void retirement_begin(RetirementAudit *audit, size_t count) {
    XrtBillingStats stats = xrt_billing_owner_stats(audit->owner);
    audit->bytes = stats.bytes - audit->metadata.bytes;
    audit->blocks = stats.blocks - audit->metadata.blocks;
    audit->allocations = stats.allocations;
    audit->provider_allocations = atomic_load(&audit->provider.allocation_calls);
    CHECK(audit->blocks == count && stats.allocations == audit->metadata.allocations + count);
    xrt_billing_owner_fail_after(audit->owner, 0);
    atomic_store(&audit->provider.fail_after, 0);
    xrt_billing_owner_close(audit->owner);
}

/* Two different parents consume the shared leaf's two real references. The
 * first parent's callback precedes that leaf's last release through the other. */
static void sparse_dag_retirement(void) {
    RetirementAudit audit = {0};
    Provider *provider = provider_new(&audit.provider);
    XrtManagedAllocator allocator = allocator_for(provider);
    OK(xrt_billing_owner_new(&allocator, SIZE_MAX, &audit.owner));
    const size_t counts[] = {0, 1, 3};
    XrtRuntimeTypePlan *plans[3] = {0};
    XrtManagedOps *ops[3] = {0};
    for (size_t i = 0; i < 3; ++i) {
        XrtRuntimeTypeSpec spec = retirement_spec(provider, counts[i]);
        OK(xrt_runtime_type_plan_new(audit.owner, &spec, &plans[i]));
        OK(xrt_managed_ops_bind(audit.owner, plans[i], &ops[i]));
    }
    audit.metadata = xrt_billing_owner_stats(audit.owner);
    XrtManagedHeader *shared = retirement_node(ops[0], 2, NULL, NULL, NULL);
    CHECK(xrt_managed_retain(shared) && xr_signed_rc_read(&shared->references) == 2);
    audit.shared = shared;
    XrtManagedHeader *left = retirement_node(ops[1], 0, shared, NULL, NULL);
    XrtManagedHeader *leaf = retirement_node(ops[0], 1, NULL, NULL, NULL);
    XrtManagedHeader *right = retirement_node(ops[2], 3, leaf, shared, NULL);
    leaf = NULL; shared = NULL;
    XrtManagedHeader *root = retirement_node(ops[2], 4, left, NULL, right);
    left = NULL; right = NULL;
    retirement_begin(&audit, 5);
    xrt_managed_drop(root);
    CHECK(audit.callbacks == 5 && !audit.bytes && !audit.blocks && !audit.shared);
    XrtBillingStats final = xrt_billing_owner_stats(audit.owner);
    CHECK(final.bytes == audit.metadata.bytes && final.blocks == audit.metadata.blocks &&
        final.allocations == audit.allocations);
    for (size_t i = 0; i < 3; ++i) {
        xrt_managed_ops_drop(ops[i]); xrt_runtime_type_plan_drop(plans[i]);
    }
    xrt_billing_owner_drop(audit.owner); provider_drop(provider);
    balanced(&audit.provider);
    CHECK(atomic_load(&audit.provider.allocation_calls) == audit.provider_allocations);
    puts("sparse owned schemas and shared DAG passed");
}

static void deep_branch_retirement(bool abort_pending) {
    const size_t count = 100000;
    RetirementAudit audit = {0};
    Provider *provider = provider_new(&audit.provider);
    XrtManagedAllocator allocator = allocator_for(provider);
    OK(xrt_billing_owner_new(&allocator, SIZE_MAX, &audit.owner));
    XrtRuntimeTypeSpec spec = retirement_spec(provider, 2);
    XrtRuntimeTypePlan *plan = NULL;
    XrtManagedOps *ops = NULL;
    OK(xrt_runtime_type_plan_new(audit.owner, &spec, &plan));
    OK(xrt_managed_ops_bind(audit.owner, plan, &ops));
    audit.metadata = xrt_billing_owner_stats(audit.owner);
    XrtManagedHeader *head = NULL;
    size_t bytes = 0;
    for (size_t i = 0; i < count; ++i) {
        XrtManagedPending pending = {0};
        retirement_prepare(ops, i, &pending);
        bytes += pending.allocation_bytes;
        ((RetireNode *)pending.value)->first = head;
        head = NULL;
        OK(xrt_managed_commit(&pending, &head));
    }
    size_t actual = 0;
    for (RetireNode *node = (RetireNode *)head; node; node = (RetireNode *)node->first) {
        CHECK(++actual <= count && node->serial == count - actual &&
            !node->second && !node->third && node->header.flags == payload_flags &&
            xr_signed_rc_read(&node->header.references) == 1);
    }
    CHECK(actual == count && xrt_billing_owner_stats(audit.owner).bytes == audit.metadata.bytes + bytes);
    XrtManagedPending pending = {0};
    if (abort_pending) {
        retirement_prepare(ops, count, &pending);
        ((RetireNode *)pending.value)->first = head;
        head = NULL;
        ((RetireNode *)pending.value)->capacity = SIZE_MAX;
        CHECK(xrt_managed_commit(&pending, &head) == XRT_MANAGED_ARGUMENT && !head && pending.value);
    }
    retirement_begin(&audit, count + (abort_pending ? 1 : 0));
    xrt_managed_ops_drop(ops); xrt_runtime_type_plan_drop(plan);
    xrt_billing_owner_drop(audit.owner); provider_drop(provider);
    CHECK(!atomic_load(&audit.provider.finalized));
    if (abort_pending) {
        xrt_managed_abort(&pending);
        CHECK(!pending.value && !pending.allocation_bytes);
    } else xrt_managed_drop(head);
    CHECK(audit.callbacks == count + (abort_pending ? 1 : 0) && !audit.bytes && !audit.blocks);
    balanced(&audit.provider);
    CHECK(atomic_load(&audit.provider.allocation_calls) == audit.provider_allocations);
    puts(abort_pending ? "pending abort with deep published child passed" :
        "100000 two-child nodes with NULL second edges passed");
}

typedef struct TailRow {
    uint64_t marker;
    XrtManagedHeader *first;
    uint64_t gap;
    XrtManagedHeader *second;
} TailRow;
typedef struct TailPayload {
    XrtManagedHeader header;
    XrtManagedHeader *fixed;
    size_t capacity, serial, rows, charge, owner, fixed_count, tail_count;
} TailPayload;
typedef struct TailInit { size_t serial, rows, owner, fixed_count, tail_count; } TailInit;
typedef struct TailAudit {
    Audit provider;
    XrtBillingOwner *owners[2];
    XrtBillingStats metadata[2];
    size_t bytes[2], blocks[2], allocations[2], callbacks, provider_allocations;
    XrtManagedHeader *shared;
} TailAudit;
static TailRow *tail_rows(TailPayload *value) { return (TailRow *)(value + 1); }
static void tail_callback(XrtManagedHeader *header, void *context) {
    Provider *provider = context;
    TailAudit *audit = (TailAudit *)provider->audit;
    TailPayload *value = (TailPayload *)header;
    CHECK(xr_signed_rc_read(&header->references) == 0 && header->flags == payload_flags);
    CHECK(value->serial == audit->callbacks && value->owner < 2 && value->capacity == value->rows);
    if (value->fixed_count) CHECK(!value->fixed);
    for (size_t i = 0; i < value->rows; ++i) {
        TailRow *row = &tail_rows(value)[i];
        CHECK(row->marker == UINT64_C(0x13579bdf2468ace0) && row->gap == i);
        if (value->tail_count) CHECK(!row->first);
        if (value->tail_count > 1) CHECK(!row->second);
    }
    if (audit->shared && !value->serial)
        CHECK(xr_signed_rc_read(&audit->shared->references) == 1 && audit->shared->flags == payload_flags);
    if (header == audit->shared) audit->shared = NULL;
    size_t bytes = sizeof(*provider), blocks = 1;
    for (size_t i = 0; i < 2; ++i) if (audit->owners[i]) {
        XrtBillingStats stats = xrt_billing_owner_stats(audit->owners[i]);
        CHECK(stats.closed && !stats.fail_after && stats.allocations == audit->allocations[i]);
        CHECK(stats.bytes == audit->metadata[i].bytes + audit->bytes[i] &&
            stats.blocks == audit->metadata[i].blocks + audit->blocks[i]);
        bytes += stats.bytes; blocks += stats.blocks;
    }
    CHECK(atomic_load(&audit->provider.bytes) == bytes && atomic_load(&audit->provider.blocks) == blocks &&
        atomic_load(&audit->provider.allocation_calls) == audit->provider_allocations);
    CHECK(audit->bytes[value->owner] >= value->charge && audit->blocks[value->owner] > 0);
    audit->bytes[value->owner] -= value->charge; --audit->blocks[value->owner];
    ++audit->callbacks;
}
static XrtRuntimeTypeSpec tail_spec(Provider *provider, size_t fixed_count, size_t tail_count) {
    static const size_t fixed = offsetof(TailPayload, fixed);
    static const size_t fields[] = {offsetof(TailRow, first), offsetof(TailRow, second)};
    CHECK(fixed_count <= 1 && tail_count <= 2);
    return (XrtRuntimeTypeSpec){.fixed_size=sizeof(TailPayload), .alignment=alignof(TailPayload),
        .capacity_offset=offsetof(TailPayload, capacity), .stride=sizeof(TailRow),
        .child_offsets=&fixed, .child_count=fixed_count, .drop_payload=tail_callback,
        .code={provider, provider_retain, provider_drop}, .tail_child_offsets=fields, .tail_child_count=tail_count};
}
static void tail_prepare(XrtManagedOps *ops, TailInit init, XrtManagedPending *pending) {
    size_t bytes = sizeof(TailPayload) + init.rows * sizeof(TailRow);
    OK(xrt_managed_prepare(ops, bytes, pending));
    TailPayload *value = (TailPayload *)pending->value;
    CHECK(value->capacity == init.rows);
    if (init.fixed_count) CHECK(!value->fixed);
    value->header.flags = payload_flags;
    value->serial = init.serial; value->rows = init.rows; value->charge = bytes;
    value->owner = init.owner; value->fixed_count = init.fixed_count; value->tail_count = init.tail_count;
    for (size_t i = 0; i < init.rows; ++i) {
        TailRow *row = &tail_rows(value)[i];
        if (init.tail_count) CHECK(!row->first);
        if (init.tail_count > 1) CHECK(!row->second);
        row->marker = UINT64_C(0x13579bdf2468ace0); row->gap = i;
    }
}
static XrtManagedHeader *tail_new(XrtManagedOps *ops, TailInit init) {
    XrtManagedPending pending = {0};
    tail_prepare(ops, init, &pending);
    XrtManagedHeader *value = NULL;
    OK(xrt_managed_commit(&pending, &value));
    return value;
}
static void tail_begin(TailAudit *audit, size_t count) {
    size_t blocks = 0;
    for (size_t i = 0; i < 2; ++i) if (audit->owners[i]) {
        XrtBillingStats stats = xrt_billing_owner_stats(audit->owners[i]);
        audit->bytes[i] = stats.bytes - audit->metadata[i].bytes;
        audit->blocks[i] = stats.blocks - audit->metadata[i].blocks;
        audit->allocations[i] = stats.allocations;
        CHECK(stats.allocations == audit->metadata[i].allocations + audit->blocks[i]);
        blocks += audit->blocks[i];
        xrt_billing_owner_fail_after(audit->owners[i], 0);
        xrt_billing_owner_close(audit->owners[i]);
    }
    CHECK(blocks == count);
    audit->provider_allocations = atomic_load(&audit->provider.allocation_calls);
    atomic_store(&audit->provider.fail_after, 0);
}
static void tail_finished(TailAudit *audit, size_t count) {
    CHECK(audit->callbacks == count && !audit->shared && !audit->bytes[0] && !audit->bytes[1] &&
        !audit->blocks[0] && !audit->blocks[1]);
    CHECK(atomic_load(&audit->provider.allocation_calls) == audit->provider_allocations);
}

static void repeated_tail_dag(void) {
    TailAudit audit = {0};
    Provider *provider = provider_new(&audit.provider);
    XrtManagedAllocator allocator = allocator_for(provider);
    for (size_t i = 0; i < 2; ++i) OK(xrt_billing_owner_new(&allocator, SIZE_MAX, &audit.owners[i]));
    XrtRuntimeTypePlan *plain = NULL, *mixed = NULL;
    XrtManagedOps *ops[2][2] = {{0}};
    XrtRuntimeTypeSpec spec = tail_spec(provider, 0, 1);
    OK(xrt_runtime_type_plan_new(audit.owners[0], &spec, &plain));
    size_t fixed = offsetof(TailPayload, fixed);
    size_t fields[] = {offsetof(TailRow, first), offsetof(TailRow, second)};
    spec = tail_spec(provider, 1, 2);
    spec.child_offsets = &fixed; spec.tail_child_offsets = fields;
    XrtBillingStats before = xrt_billing_owner_stats(audit.owners[1]);
    OK(xrt_runtime_type_plan_new(audit.owners[1], &spec, &mixed));
    XrtBillingStats with_fields = xrt_billing_owner_stats(audit.owners[1]);
    XrtRuntimeTypeSpec empty_spec = tail_spec(provider, 0, 0);
    XrtRuntimeTypePlan *empty = NULL;
    OK(xrt_runtime_type_plan_new(audit.owners[1], &empty_spec, &empty));
    XrtBillingStats with_empty = xrt_billing_owner_stats(audit.owners[1]);
    CHECK(with_fields.bytes - before.bytes == with_empty.bytes - with_fields.bytes + 3 * sizeof(size_t));
    xrt_runtime_type_plan_drop(empty);
    CHECK(xrt_billing_owner_stats(audit.owners[1]).bytes == with_fields.bytes);
    fixed = SIZE_MAX; fields[0] = SIZE_MAX; fields[1] = SIZE_MAX;
    for (size_t i = 0; i < 2; ++i) {
        OK(xrt_managed_ops_bind(audit.owners[i], plain, &ops[i][0]));
        OK(xrt_managed_ops_bind(audit.owners[i], mixed, &ops[i][1]));
        audit.metadata[i] = xrt_billing_owner_stats(audit.owners[i]);
    }
    XrtManagedHeader *shared = tail_new(ops[1][0], (TailInit){2, 2, 1, 0, 1});
    CHECK(xrt_managed_retain(shared) && xr_signed_rc_read(&shared->references) == 2);
    audit.shared = shared;
    XrtManagedHeader *left = tail_new(ops[0][0], (TailInit){0, 1, 0, 0, 1});
    tail_rows((TailPayload *)left)[0].first = shared;
    XrtManagedHeader *leaf = tail_new(ops[0][0], (TailInit){1, 0, 0, 0, 1});
    XrtManagedHeader *right = tail_new(ops[1][1], (TailInit){3, 3, 1, 1, 2});
    ((TailPayload *)right)->fixed = leaf;
    tail_rows((TailPayload *)right)[0].second = shared;
    leaf = NULL; shared = NULL;
    XrtManagedHeader *root = tail_new(ops[0][1], (TailInit){4, 2, 0, 1, 2});
    ((TailPayload *)root)->fixed = left;
    tail_rows((TailPayload *)root)[1].second = right;
    left = NULL; right = NULL;
    tail_begin(&audit, 5);
    xrt_managed_drop(root);
    tail_finished(&audit, 5);
    for (size_t i = 0; i < 2; ++i) {
        CHECK(xrt_billing_owner_stats(audit.owners[i]).bytes == audit.metadata[i].bytes);
        xrt_managed_ops_drop(ops[i][0]); xrt_managed_ops_drop(ops[i][1]);
    }
    xrt_runtime_type_plan_drop(plain); xrt_runtime_type_plan_drop(mixed);
    for (size_t i = 0; i < 2; ++i) xrt_billing_owner_drop(audit.owners[i]);
    provider_drop(provider); balanced(&audit.provider);
    puts("repeated tail rows, mixed slots and cross-owner DAG passed");
}

static void deep_tail_retirement(bool abort_pending) {
    const size_t count = 100000;
    TailAudit audit = {0};
    Provider *provider = provider_new(&audit.provider);
    XrtManagedAllocator allocator = allocator_for(provider);
    OK(xrt_billing_owner_new(&allocator, SIZE_MAX, &audit.owners[0]));
    XrtRuntimeTypePlan *plan = NULL;
    XrtManagedOps *ops = NULL;
    XrtRuntimeTypeSpec spec = tail_spec(provider, 0, 1);
    OK(xrt_runtime_type_plan_new(audit.owners[0], &spec, &plan));
    OK(xrt_managed_ops_bind(audit.owners[0], plan, &ops));
    audit.metadata[0] = xrt_billing_owner_stats(audit.owners[0]);
    XrtManagedHeader *head = NULL;
    size_t bytes = 0;
    for (size_t i = 0; i < count; ++i) {
        XrtManagedPending pending = {0};
        tail_prepare(ops, (TailInit){i, 1 + i % 2, 0, 0, 1}, &pending);
        bytes += pending.allocation_bytes;
        tail_rows((TailPayload *)pending.value)[0].first = head;
        head = NULL;
        OK(xrt_managed_commit(&pending, &head));
    }
    size_t actual = 0;
    for (TailPayload *node = (TailPayload *)head; node; node = (TailPayload *)tail_rows(node)[0].first) {
        CHECK(++actual <= count && node->serial == count - actual &&
            xr_signed_rc_read(&node->header.references) == 1);
        if (node->rows > 1) CHECK(!tail_rows(node)[1].first);
    }
    CHECK(actual == count && xrt_billing_owner_stats(audit.owners[0]).bytes == audit.metadata[0].bytes + bytes);
    XrtManagedPending pending = {0};
    if (abort_pending) {
        tail_prepare(ops, (TailInit){count, 4, 0, 0, 1}, &pending);
        tail_rows((TailPayload *)pending.value)[3].first = head;
        head = NULL;
        ((TailPayload *)pending.value)->capacity = 0;
        CHECK(xrt_managed_commit(&pending, &head) == XRT_MANAGED_ARGUMENT && !head && pending.value);
        ((TailPayload *)pending.value)->capacity = SIZE_MAX;
        CHECK(xrt_managed_commit(&pending, &head) == XRT_MANAGED_ARGUMENT && !head && pending.value);
    }
    tail_begin(&audit, count + (abort_pending ? 1 : 0));
    xrt_managed_ops_drop(ops); xrt_runtime_type_plan_drop(plan);
    xrt_billing_owner_drop(audit.owners[0]); provider_drop(provider);
    CHECK(!atomic_load(&audit.provider.finalized));
    if (abort_pending) {
        xrt_managed_abort(&pending);
        CHECK(!pending.value && !pending.allocation_bytes);
    } else xrt_managed_drop(head);
    tail_finished(&audit, count + (abort_pending ? 1 : 0));
    balanced(&audit.provider);
    puts(abort_pending ? "corrupt-capacity pending abort with deep owned tail passed" :
        "100000 repeated-tail nodes retired on the default stack");
}

static void tail_admission_and_oom(void) {
    TailAudit audit = {0};
    Provider *provider = provider_new(&audit.provider);
    XrtManagedAllocator allocator = allocator_for(provider);
    OK(xrt_billing_owner_new(&allocator, SIZE_MAX, &audit.owners[0]));
    XrtRuntimeTypeSpec good = tail_spec(provider, 1, 2), bad;
    XrtRuntimeTypePlan *plan = NULL;
    size_t invalid[][2] = {{1, offsetof(TailRow, second)}, {sizeof(TailRow), 0},
        {offsetof(TailRow, second), offsetof(TailRow, first)}, {offsetof(TailRow, first), offsetof(TailRow, first)}};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        bad = good; bad.tail_child_offsets = invalid[i];
        CHECK(xrt_runtime_type_plan_new(audit.owners[0], &bad, &plan) == XRT_MANAGED_ARGUMENT && !plan);
    }
    bad = good; bad.tail_child_offsets = NULL;
    CHECK(xrt_runtime_type_plan_new(audit.owners[0], &bad, &plan) == XRT_MANAGED_ARGUMENT && !plan);
    bad = good; bad.stride = 0;
    CHECK(xrt_runtime_type_plan_new(audit.owners[0], &bad, &plan) == XRT_MANAGED_ARGUMENT && !plan);
    bad = good; bad.stride -= 1;
    CHECK(xrt_runtime_type_plan_new(audit.owners[0], &bad, &plan) == XRT_MANAGED_ARGUMENT && !plan);
    bad = good; bad.fixed_size += 1;
    CHECK(xrt_runtime_type_plan_new(audit.owners[0], &bad, &plan) == XRT_MANAGED_ARGUMENT && !plan);
    bad = good; bad.tail_child_count = SIZE_MAX;
    CHECK(xrt_runtime_type_plan_new(audit.owners[0], &bad, &plan) == XRT_MANAGED_ARGUMENT && !plan);
    XrtBillingStats baseline = xrt_billing_owner_stats(audit.owners[0]);
    atomic_store(&audit.provider.fail_after, 0);
    CHECK(xrt_runtime_type_plan_new(audit.owners[0], &good, &plan) == XRT_MANAGED_OOM && !plan);
    CHECK(xrt_billing_owner_stats(audit.owners[0]).bytes == baseline.bytes &&
        xrt_billing_owner_stats(audit.owners[0]).allocations == baseline.allocations);
    atomic_store(&audit.provider.fail_after, SIZE_MAX);
    OK(xrt_runtime_type_plan_new(audit.owners[0], &good, &plan));
    XrtManagedOps *ops = NULL;
    OK(xrt_managed_ops_bind(audit.owners[0], plan, &ops));
    baseline = xrt_billing_owner_stats(audit.owners[0]);
    XrtManagedPending pending = {0};
    CHECK(xrt_managed_prepare(ops, sizeof(TailPayload) + 1, &pending) == XRT_MANAGED_ARGUMENT && !pending.value);
    atomic_store(&audit.provider.fail_after, 0);
    CHECK(xrt_managed_prepare(ops, sizeof(TailPayload) + 4 * sizeof(TailRow), &pending) == XRT_MANAGED_OOM && !pending.value);
    CHECK(xrt_billing_owner_stats(audit.owners[0]).bytes == baseline.bytes &&
        xrt_billing_owner_stats(audit.owners[0]).blocks == baseline.blocks &&
        xrt_billing_owner_stats(audit.owners[0]).allocations == baseline.allocations);
    atomic_store(&audit.provider.fail_after, SIZE_MAX);
    audit.metadata[0] = baseline;
    XrtManagedHeader *child = tail_new(ops, (TailInit){0, 0, 0, 1, 2});
    tail_prepare(ops, (TailInit){1, 4, 0, 1, 2}, &pending);
    tail_rows((TailPayload *)pending.value)[3].second = child;
    child = NULL;
    ((TailPayload *)pending.value)->capacity = 0;
    CHECK(xrt_managed_commit(&pending, &child) == XRT_MANAGED_ARGUMENT && !child && pending.value);
    tail_begin(&audit, 2);
    xrt_managed_abort(&pending);
    tail_finished(&audit, 2);
    CHECK(xrt_billing_owner_stats(audit.owners[0]).bytes == baseline.bytes);
    xrt_managed_ops_drop(ops); xrt_runtime_type_plan_drop(plan);
    xrt_billing_owner_drop(audit.owners[0]); provider_drop(provider);
    balanced(&audit.provider);
    puts("owned-tail admission, provider OOM and partial abort passed");
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "deep-chain")) {
        deep_chain_retirement();
        return 0;
    }
    CHECK(argc == 1);
    default_provider_retirement();
    dynamic_retirement();
    failure_atomicity();
    reentrant_binding();
    concurrent_final_free();
    deep_chain_retirement();
    sparse_dag_retirement();
    deep_branch_retirement(false);
    deep_branch_retirement(true);
    repeated_tail_dag();
    tail_admission_and_oom();
    deep_tail_retirement(false);
    deep_tail_retirement(true);
    puts("managed kernel passed");
    return 0;
}
