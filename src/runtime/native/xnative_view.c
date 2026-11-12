/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_view.c - Activation-owned canonical integer Array loans
 *
 * KEY CONCEPT:
 *   Typed identities select actual canonical owners. The resource registry
 *   authenticates lifetime; the canonical value kernel owns every live loan.
 */
#include "xnative_view.h"
#include "xnative_array.h"
#include "xir/xxir_array.h"
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena_internal.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_value_take_internal.h"
#include "xir/xxir_view_value_internal.h"
#include "base/xhash.h"
#include <stdatomic.h>
#include <string.h>

enum { VIEW_ARRAY_TYPE = 256, VIEW_CELL_TYPE = 260, VIEW_SLICE_TYPE = 261 };
typedef enum XrtViewRecordKind { XRT_VIEW_FRAME, XRT_VIEW_CELL, XRT_VIEW_SLICE } XrtViewRecordKind;
typedef struct XrtViewFrameBody XrtViewFrameBody;
typedef struct XrtViewScope XrtViewScope;
typedef struct XrtViewCellSlot { XrXirValue value; uint64_t identity; } XrtViewCellSlot;
typedef struct XrtViewSlot {
    XrXirValue value;
    XrtViewScope *scope;
    uint64_t identity;
} XrtViewSlot;
typedef struct XrtViewRecord {
    uint64_t identity;
    XrtViewFrameBody *frame;
    uint32_t slot;
    XrtViewRecordKind kind;
} XrtViewRecord;
typedef struct XrtViewTypeEntry {
    XirTypeIdentity identity;
    XrXirTypeArena *arena;
} XrtViewTypeEntry;
struct XrtViewRegistry {
    XrtValueRoot *root;
    XrXirTypeArena *types;
    XrtViewFrameBody *current;
    XrtViewRecord *records;
    uint32_t count, capacity;
    XrtViewTypeEntry *type_entries;
    XrXirTypeArena *pending_types;
    uint32_t type_count, type_capacity;
};
struct XrtViewScope {
    XrXirViewOriginPin pin;
    XrXirCellAuthority authority;
    XrtViewFrameBody *owner, *root_owner;
    XrtViewScope *previous, *next, *parent;
    uint32_t references;
    bool active, module_root;
};
struct XrtViewFrameBody {
    XrtViewRegistry *registry;
    XrtViewFrameBody *parent;
    XrtViewScope *scopes, *pending_scope;
    XrtViewCellSlot *cells;
    XrtViewSlot *views;
    XrtViewSlot result;
    uint32_t cell_count, view_count;
    uint64_t identity;
    XrXirValue pending;
    XirCellNewTake pending_cell;
    bool active;
};

#include "xnative_view_registry.inc.c"

static void view_require_status(XrXirValueStatus status, uint32_t source) {
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
}
static XrXirValueAdmission view_value_admission(const XrXirValue *value) {
    const XirObject *object = (const XirObject *)(intptr_t)value->payload;
    return (XrXirValueAdmission){object->arena, object->domain,
        NULL, NULL, UINT64_MAX, UINT64_MAX};
}

#include "xnative_view_types.inc.c"
static void view_scope_unlink(XrtViewScope *scope) {
    if (scope->previous) scope->previous->next = scope->next;
    else scope->owner->scopes = scope->next;
    if (scope->next) scope->next->previous = scope->previous;
    scope->previous = scope->next = NULL;
}
static void view_scope_release(XrtViewScope *scope) {
    while (scope) {
        XR_CHECK(scope->references, "each logical binding or child owns its physical scope metadata");
        if (--scope->references) return;
        XrtViewScope *parent = scope->parent;
        XrXirDomain *domain = scope->owner->registry->root->domain;
        scope->active = false;
        xr_xir_view_scope_close(&scope->pin);
        view_scope_unlink(scope);
        xr_xir_domain_deallocate(domain, scope, sizeof(*scope));
        scope = parent;
    }
}
static void view_clear_slot(XrtViewFrameBody *frame, uint32_t ordinal) {
    XrtViewSlot *slot = ordinal == UINT32_MAX ? &frame->result : &frame->views[ordinal];
    XrtViewScope *scope = slot->scope;
    view_record_remove(frame->registry, slot->identity);
    slot->identity = 0;
    slot->scope = NULL;
    xr_xir_value_drop(&slot->value);
    view_scope_release(scope);
}
static void view_clear_cell(XrtViewFrameBody *frame, uint32_t ordinal) {
    XrtViewCellSlot *slot = &frame->cells[ordinal];
    view_record_remove(frame->registry, slot->identity);
    slot->identity = 0;
    xr_xir_value_drop(&slot->value);
}
static void view_frame_destroy(XrtViewFrameBody *frame) {
    XrtViewRegistry *registry = frame->registry;
    XrXirDomain *domain = registry->root->domain;
    frame->active = false;
    if (frame->views)
        for (uint32_t n = frame->view_count; n; --n) view_clear_slot(frame, n - 1);
    view_clear_slot(frame, UINT32_MAX);
    XR_CHECK(!frame->scopes, "owned view scopes retire before activation storage");
    if (frame->pending_scope) xr_xir_domain_deallocate(domain, frame->pending_scope, sizeof(*frame->pending_scope));
    xir_cell_new_take_abort(&frame->pending_cell);
    xr_xir_value_drop(&frame->pending);
    if (frame->cells)
        for (uint32_t n = frame->cell_count; n; --n) view_clear_cell(frame, n - 1);
    view_record_remove(registry, frame->identity);
    registry->current = frame->parent;
    if (frame->views) xr_xir_domain_deallocate(domain, frame->views, (size_t)frame->view_count * sizeof(*frame->views));
    if (frame->cells) xr_xir_domain_deallocate(domain, frame->cells, (size_t)frame->cell_count * sizeof(*frame->cells));
    xr_xir_domain_deallocate(domain, frame, sizeof(*frame));
}
XR_FUNC void xrt_view_root_drain(XrtValueRoot *root) {
    XrtViewRegistry *registry = root->views;
    if (!registry) return;
    while (registry->current) view_frame_destroy(registry->current);
    XR_CHECK(!registry->count, "all activation identities retire with their actual owners");
    if (registry->records) xr_xir_domain_deallocate(root->domain, registry->records,
        (size_t)registry->capacity * sizeof(*registry->records));
    for (uint32_t n = 0; n < registry->type_capacity; ++n)
        xr_xir_compile_type_arena_drop(registry->type_entries[n].arena);
    if (registry->type_entries) xr_xir_domain_deallocate(root->domain, registry->type_entries,
        (size_t)registry->type_capacity * sizeof(*registry->type_entries));
    xr_xir_compile_type_arena_drop(registry->pending_types);
    xr_xir_compile_type_arena_drop(registry->types);
    root->views = NULL;
    xr_xir_domain_deallocate(root->domain, registry, sizeof(*registry));
}
XRAY_API XR_NOINLINE XrtViewFrame xrt_view_frame_enter(uint32_t cells, uint32_t views, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    if (!root->views) {
        XrXirValueStatus status = XR_XIR_VALUE_OK;
        XrtViewRegistry *registry = xr_xir_domain_allocate(xrt_value_root_domain(root, source),
            sizeof(*registry), &status);
        if (!registry) xrt_value_failure(status, source);
        memset(registry, 0, sizeof(*registry));
        registry->root = root;
        root->views = registry;
        uint64_t bytes = 0;
        view_require_status(xrt_view_types_new(root->byte_limit ? root->byte_limit : UINT64_MAX,
            &registry->types, &bytes), source);
    }
    XrtViewRegistry *registry = root->views;
    if (views == UINT32_MAX || cells > UINT32_MAX - views - 1 ||
        sizeof(XrtViewCellSlot) > SIZE_MAX / (cells ? cells : 1) ||
        sizeof(XrtViewSlot) > SIZE_MAX / (views ? views : 1)) xrt_value_failure(XR_XIR_VALUE_OVERFLOW, source);
    view_reserve(registry, 1 + cells + views, source);
    XrtViewFrameBody *frame = view_allocate(registry, sizeof(*frame), source);
    frame->registry = registry;
    frame->parent = registry->current;
    frame->active = true;
    registry->current = frame;
    frame->identity = view_identity(source);
    frame->cell_count = cells;
    frame->view_count = views;
    view_record_insert(registry, (XrtViewRecord){frame->identity, frame, 0, XRT_VIEW_FRAME});
    if (cells) frame->cells = view_allocate(registry, (size_t)cells * sizeof(*frame->cells), source);
    if (views) frame->views = view_allocate(registry, (size_t)views * sizeof(*frame->views), source);
    return (XrtViewFrame){frame->identity};
}
XRAY_API XR_NOINLINE void xrt_view_frame_leave(XrtViewFrame token, uint32_t source) {
    view_frame_destroy(view_current(token, source));
}
static XrtViewCellSlot *view_cell(XrtViewFrameBody *frame, XrtViewCell token, uint32_t source) {
    XrtViewRecord record = view_require(frame, token.identity, XRT_VIEW_CELL, source);
    XrtViewCellSlot *slot = &record.frame->cells[record.slot];
    if (slot->identity != token.identity) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    return slot;
}
XRAY_API XR_NOINLINE XrtViewCell xrt_view_cell_new(XrtViewFrame token, uint32_t ordinal,
    const XrtArray *array, uint32_t source) {
    XrtViewFrameBody *frame = view_current(token, source);
    if (ordinal >= frame->cell_count) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    if (frame->cells[ordinal].value.type && !xr_xir_cell_unborrowed(&frame->cells[ordinal].value))
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    uint64_t identity = view_identity(source);
    XrtArray *copy = NULL;
    view_require_status(xrt_array_owned_copy(array, frame->registry->types, XRT_ARRAY_I64, &copy), source);
    frame->pending = (XrXirValue){((const XirObject *)copy)->type, 0, (int64_t)(intptr_t)copy};
    XrXirTypeArena *types = view_types_get(frame->registry, &frame->pending, source);
    XrXirValueAdmission admission = {types, frame->registry->root->domain,
        NULL, NULL, UINT64_MAX, UINT64_MAX};
    const XirObject *array_owner = (const XirObject *)(intptr_t)frame->pending.payload;
    /* A retained module value may belong to another live allocation domain.
     * Taking requires the current domain; ordinary canonical copying retains
     * that input without transferring the original owner's reference. */
    if (array_owner->domain == admission.domain) {
        view_require_status(xir_cell_new_take_prepare(VIEW_CELL_TYPE, &frame->pending,
            &admission, &frame->pending_cell), source);
        view_clear_cell(frame, ordinal);
        xir_cell_new_take_commit(&frame->pending_cell, &frame->pending.payload, &frame->cells[ordinal].value);
        frame->pending = (XrXirValue){0};
    } else {
        XrXirValue created = {0};
        view_require_status(xr_xir_cell_new(admission.domain, types,
            VIEW_CELL_TYPE, &frame->pending, &admission, &created), source);
        view_clear_cell(frame, ordinal);
        frame->cells[ordinal].value = created;
        xr_xir_value_drop(&frame->pending);
    }
    frame->cells[ordinal].identity = identity;
    view_record_insert(frame->registry, (XrtViewRecord){identity, frame, ordinal, XRT_VIEW_CELL});
    return (XrtViewCell){identity};
}
XRAY_API XR_NOINLINE XrtArray *xrt_view_cell_read(XrtViewFrame token, XrtViewCell cell, uint32_t source) {
    XrtViewFrameBody *frame = view_current(token, source);
    XrtViewCellSlot *slot = view_cell(frame, cell, source);
    view_require_status(xr_xir_cell_authorized_read(&slot->value, NULL, &frame->registry->root->pending), source);
    return xrt_array_publish_pending(source);
}
XRAY_API XR_NOINLINE void xrt_view_cell_write(XrtViewFrame token, XrtViewCell cell,
    const XrtArray *array, uint32_t source) {
    XrtViewFrameBody *frame = view_current(token, source);
    XrtViewCellSlot *slot = view_cell(frame, cell, source);
    XrtArray *copy = NULL;
    view_require_status(xrt_array_owned_copy(array, frame->registry->types, XRT_ARRAY_I64, &copy), source);
    frame->pending = (XrXirValue){((const XirObject *)copy)->type, 0, (int64_t)(intptr_t)copy};
    XrXirValueAdmission admission = view_value_admission(&slot->value);
    view_require_status(xr_xir_cell_authorized_write(&slot->value, NULL, &frame->pending, &admission), source);
    xr_xir_value_drop(&frame->pending);
}
XRAY_API XR_NOINLINE void xrt_view_cell_drop(XrtViewFrame token, XrtViewCell cell, uint32_t source) {
    XrtViewFrameBody *frame = view_current(token, source);
    XrtViewRecord record = view_require(frame, cell.identity, XRT_VIEW_CELL, source);
    if (record.frame != frame || !xr_xir_cell_unborrowed(&record.frame->cells[record.slot].value))
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    view_clear_cell(record.frame, record.slot);
}
XRAY_API XR_NOINLINE int64_t xrt_view_cell_i64_get(XrtViewFrame token, XrtViewCell cell,
    int64_t index, uint32_t source) {
    XrtViewFrameBody *frame = view_current(token, source);
    XrtViewCellSlot *slot = view_cell(frame, cell, source);
    int64_t output = 0;
    view_require_status(xir_cell_array_i64_read_owned(&slot->value, index, NULL, &output), source);
    return output;
}
XRAY_API XR_NOINLINE void xrt_view_cell_i64_set(XrtViewFrame token, XrtViewCell cell,
    int64_t index, int64_t number, uint32_t source) {
    XrtViewFrameBody *frame = view_current(token, source);
    XrtViewCellSlot *slot = view_cell(frame, cell, source);
    XrXirValueAdmission admission = view_value_admission(&slot->value);
    XrXirFaultDetail fault = {0};
    view_require_status(xir_cell_array_i64_write_owned(&slot->value, index, number,
        NULL, &admission, &fault), source);
}

#include "xnative_view_slice.inc.c"
