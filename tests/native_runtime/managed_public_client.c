/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * managed_public_client.c - Public C11 storage, stable slots and payload publication
 */
#include "xray_native_managed.h"
#include "xray_native_values.h"
#include "xray_native_managed_root.h"
#include "xray_native_place.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define OK(c) CHECK((c) == XRT_MANAGED_OK)
typedef struct Node { XrtManagedHeader header; XrtManagedHeader *child; int generation; } Node;
typedef struct Slab { XrtManagedHeader *left, *right, *temporary; int64_t number; uint8_t tag; } Slab;
typedef struct Audit { Slab *slab; size_t retired; } Audit;
static Audit public_audit;
static void retired(XrtManagedHeader *header, void *context) {
    (void)context;
    Audit *audit = &public_audit;
    Node *node = (Node *)header;
    CHECK(!node->child);
    if (node->generation == 1)
        CHECK(audit->slab->left == audit->slab->right && audit->slab->tag == 1 && audit->slab->number == 42);
    ++audit->retired;
}
static void publish(void *context) {
    Slab *slab = context;
    slab->number = 42; slab->tag = 1;
}
int main(void) {
    XrtBillingOwner *billing = NULL; XrtRuntimeTypePlan *plan = NULL; XrtManagedOps *ops = NULL;
    XrtManagedRoot *root = NULL; XrtManagedFrame *frame = NULL;
    OK(xrt_billing_owner_new(NULL, SIZE_MAX, &billing));
    XrtBillingStats initial = xrt_billing_owner_stats(billing);
    size_t child = offsetof(Node, child);
    Audit *audit = &public_audit;
    XrtRuntimeTypeSpec type = {.fixed_size=sizeof(Node), .alignment=_Alignof(Node), .capacity_offset=SIZE_MAX,
        .child_offsets=&child, .child_count=1, .drop_payload=retired};
    OK(xrt_runtime_type_plan_new(billing, &type, &plan));
    OK(xrt_managed_ops_bind(billing, plan, &ops));
    OK(xrt_managed_root_new(billing, &root));
    size_t offsets[] = {offsetof(Slab, left), offsetof(Slab, right), offsetof(Slab, temporary)};
    XrtManagedFrameSpec spec = {3, sizeof(Slab), _Alignof(Slab), offsets};
    OK(xrt_managed_frame_enter_layout(root, &spec, &frame));
    void *payload = NULL; size_t bytes = 0;
    OK(xrt_managed_frame_payload(frame, &payload, &bytes));
    CHECK(bytes == sizeof(Slab)); audit->slab = payload;
    CHECK(!audit->slab->left && !audit->slab->right && !audit->slab->temporary);
    XrtManagedHeader *prepared = NULL;
    OK(xrt_managed_slot_prepare(frame, 0, ops, sizeof(Node), &prepared));
    CHECK(!((Node *)prepared)->child); ((Node *)prepared)->generation = 2;
    OK(xrt_managed_slot_commit(frame, 0));
    prepared = NULL;
    OK(xrt_managed_slot_prepare(frame, 1, ops, sizeof(Node), &prepared));
    ((Node *)prepared)->generation = 1;
    OK(xrt_managed_slot_commit(frame, 1));
    prepared = NULL;
    XrtPlace source = {frame, 0, NULL, XRT_PLACE_NONNULL};
    XrtPlace destination = {frame, 1, NULL, XRT_PLACE_NULLABLE};
    XrtPlacePublication publication = {publish, audit->slab};
    OK(xrt_place_copy_replace(destination, source, &publication));
    CHECK(audit->slab->left == audit->slab->right && audit->slab->number == 42 && audit->slab->tag == 1);
    xrt_billing_owner_fail_after(billing, 0);
    CHECK(xrt_managed_slot_prepare(frame, 1, ops, sizeof(Node), &prepared) == XRT_MANAGED_OOM);
    CHECK(audit->slab->left == audit->slab->right);
    xrt_billing_owner_fail_after(billing, SIZE_MAX);
    OK(xrt_managed_frame_leave(root, frame)); OK(xrt_managed_root_destroy(&root));
    xrt_managed_ops_drop(ops); xrt_runtime_type_plan_drop(plan);
    CHECK(audit->retired == 2);
    XrtBillingStats after = xrt_billing_owner_stats(billing);
    CHECK(after.bytes == initial.bytes && after.blocks == initial.blocks);
    xrt_billing_owner_drop(billing);
    puts("public C11 managed ABI, stable payload and real ownership publication passed");
    return 0;
}
