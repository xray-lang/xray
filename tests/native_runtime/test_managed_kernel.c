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

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define OK(c) CHECK((c) == XRT_MANAGED_OK)

typedef struct Audit {
    _Atomic(size_t) bytes, blocks, finalized, callbacks, fail_after;
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
    return (XrtRuntimeTypeSpec){sizeof(Payload), alignof(Payload), offsetof(Payload, capacity), 1,
        fields, count, provider ? payload_callback : NULL,
        provider ? (XrtManagedLease){provider, provider_retain, provider_drop} : (XrtManagedLease){0}};
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
    puts("managed kernel passed");
    return 0;
}
