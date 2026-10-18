/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_pending_requests.c - Event-thread ownership of pending request IDs
 */
#include "xlsp_pending_requests.h"
#include <string.h>

static bool valid_bounds(const XlspPendingRequests *pending) {
    return pending && pending->head >= 0 && pending->head < MAX_PENDING_REQUESTS &&
        pending->count >= 0 && pending->count <= MAX_PENDING_REQUESTS;
}

XR_FUNC bool xlsp_pending_request_add(XlspPendingRequests *pending, XlspRequestId id,
                                      const char *method, uint64_t start_time) {
    if (!valid_bounds(pending) || !xlsp_request_id_valid(&id) || !method || !method[0] ||
        pending->count == MAX_PENDING_REQUESTS) return false;
    int occupied = 0;
    for (int i = 0; i < MAX_PENDING_REQUESTS; ++i) {
        if (pending->requests[i].id.kind == XLSP_ID_NONE) continue;
        if (!xlsp_request_id_valid(&pending->requests[i].id) ||
            xlsp_request_id_equals(&pending->requests[i].id, &id)) return false;
        ++occupied;
    }
    if (occupied != pending->count) return false;
    for (int step = 0; step < MAX_PENDING_REQUESTS; ++step) {
        int slot = (pending->head + step) % MAX_PENDING_REQUESTS;
        if (pending->requests[slot].id.kind != XLSP_ID_NONE) continue;
        pending->requests[slot] = (XlspPendingRequest){.id = id, .method = method, .start_time = start_time};
        pending->head = (slot + 1) % MAX_PENDING_REQUESTS; ++pending->count; return true;
    }
    return false;
}

XR_FUNC XlspPendingRequest *xlsp_pending_request_find(XlspPendingRequests *pending, const XlspRequestId *id) {
    if (!valid_bounds(pending) || !pending->count || !xlsp_request_id_valid(id)) return NULL;
    for (int i = 0; i < MAX_PENDING_REQUESTS; ++i)
        if (xlsp_request_id_equals(&pending->requests[i].id, id)) return &pending->requests[i];
    return NULL;
}

XR_FUNC bool xlsp_pending_request_remove(XlspPendingRequests *pending, const XlspRequestId *id) {
    XlspPendingRequest *entry = xlsp_pending_request_find(pending, id);
    if (!entry) return false;
    xlsp_request_id_free(&entry->id); memset(entry, 0, sizeof(*entry)); --pending->count; return true;
}
