/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_place.c - Preflighted publication without unrooted failure windows
 *
 * KEY CONCEPT:
 *   Stable owner fields are both the source of truth and rollback protection.
 *   Temporary edge slots protect all old sources before any destination changes.
 */
#include "xnative_place.h"
#include "base/xchecks.h"

typedef struct PlaceLocation {
    XrtManagedSlotLocation slot;
    const XrtExternalOwner *external;
    XrtPlaceNull nulls;
} PlaceLocation;
static XrtManagedStatus place_location(XrtPlace place, PlaceLocation *output) {
    if ((place.nulls != XRT_PLACE_NONNULL && place.nulls != XRT_PLACE_NULLABLE) ||
        (!!place.frame == !!place.external)) return XRT_MANAGED_ARGUMENT;
    PlaceLocation location = {.external=place.external, .nulls=place.nulls};
    if (place.frame) {
        XrtManagedStatus status = xrt_managed_slot_location(place.frame, place.slot, &location.slot);
        if (status != XRT_MANAGED_OK) return status;
    } else {
        if (!place.external->owner || !place.external->preflight) return XRT_MANAGED_ARGUMENT;
        location.slot.owner = place.external->owner;
        location.slot.busy = place.external->busy;
        location.slot.cell_domain = place.external->cell_domain;
    }
    if (!xrt_managed_domain_valid(location.slot)) return XRT_MANAGED_ARGUMENT;
    *output = location;
    return XRT_MANAGED_OK;
}
static XrtManagedStatus place_check(PlaceLocation location, XrtPlaceAction action) {
    if (location.slot.pending && (location.slot.pending->value || location.slot.pending->allocation_bytes))
        return XRT_MANAGED_ARGUMENT;
    if (location.external) {
        XrtManagedStatus status = location.external->preflight(location.external->context, action);
        if (status != XRT_MANAGED_OK) return status;
    }
    if (action != XRT_PLACE_REPLACE && !*location.slot.owner && location.nulls != XRT_PLACE_NULLABLE)
        return XRT_MANAGED_ARGUMENT;
    return XRT_MANAGED_OK;
}
static XrtManagedStatus place_lock_pair(PlaceLocation destination, PlaceLocation source) {
    bool same = xrt_managed_domain_same(destination.slot, source.slot);
    if (destination.slot.owner == source.slot.owner && !same)
        return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = xrt_managed_domain_enter(destination.slot);
    if (status != XRT_MANAGED_OK) return status;
    if (!same && (status = xrt_managed_domain_enter(source.slot)) != XRT_MANAGED_OK) {
        xrt_managed_domain_leave(destination.slot); return status;
    }
    return XRT_MANAGED_OK;
}
static void place_unlock_pair(PlaceLocation destination, PlaceLocation source) {
    if (!xrt_managed_domain_same(destination.slot, source.slot)) xrt_managed_domain_leave(source.slot);
    xrt_managed_domain_leave(destination.slot);
}
XR_FUNCDEF XrtManagedStatus xrt_place_replacement_begin(XrtPlace binding, XrtManagedFrame *frame,
    size_t index, XrtPlaceReplacement *output) {
    if (!output || output->root) return XRT_MANAGED_ARGUMENT;
    PlaceLocation source={0},candidate={0};
    XrtManagedStatus status=place_location(binding,&source);
    if (status==XRT_MANAGED_OK) status=place_location((XrtPlace){.frame=frame,.slot=index,
        .nulls=XRT_PLACE_NONNULL},&candidate);
    if (status!=XRT_MANAGED_OK) return status;
    if (source.slot.owner==candidate.slot.owner) return XRT_MANAGED_ARGUMENT;
    status=place_lock_pair(candidate,source);
    if (status!=XRT_MANAGED_OK) return status;
    status=place_check(source,XRT_PLACE_REPLACE);
    if (status==XRT_MANAGED_OK) status=place_check(candidate,XRT_PLACE_REPLACE);
    if (status==XRT_MANAGED_OK && (!*source.slot.owner || *candidate.slot.owner)) status=XRT_MANAGED_ARGUMENT;
    if (status==XRT_MANAGED_OK) status=xrt_managed_slot_reserved_begin(frame,index,source.slot,output);
    if (status!=XRT_MANAGED_OK) place_unlock_pair(candidate,source);
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_place_replacement_publish(XrtPlaceReplacement *replacement) {
    return xrt_managed_slot_reserved_publish(replacement);
}
XR_FUNCDEF XrtManagedStatus xrt_place_replacement_end(XrtPlaceReplacement *replacement) {
    return xrt_managed_slot_reserved_end(replacement);
}
XRAY_API XrtManagedStatus xrt_place_borrow(XrtPlace place, const XrtManagedHeader **output) {
    if (!output) return XRT_MANAGED_ARGUMENT;
    PlaceLocation location;
    XrtManagedStatus status = place_location(place, &location);
    if (status != XRT_MANAGED_OK) return status;
    status = xrt_managed_domain_enter(location.slot);
    if (status != XRT_MANAGED_OK) return status;
    status = place_check(location, XRT_PLACE_BORROW);
    if (status == XRT_MANAGED_OK) *output = *location.slot.owner;
    xrt_managed_domain_leave(location.slot);
    return status;
}
static XrtManagedStatus place_replace(XrtPlace destination, XrtPlace source, bool copy,
    const XrtPlacePublication *publication) {
    if (publication && !publication->publish) return XRT_MANAGED_ARGUMENT;
    PlaceLocation to = {0}, from = {0};
    XrtManagedStatus status = place_location(destination, &to);
    if (status == XRT_MANAGED_OK) status = place_location(source, &from);
    if (status != XRT_MANAGED_OK) return status;
    status = place_lock_pair(to, from);
    if (status != XRT_MANAGED_OK) return status;
    status = place_check(from, copy ? XRT_PLACE_COPY_FROM : XRT_PLACE_MOVE_FROM);
    if (status == XRT_MANAGED_OK) status = place_check(to, XRT_PLACE_REPLACE);
    if (status == XRT_MANAGED_OK && !*from.slot.owner && to.nulls != XRT_PLACE_NULLABLE)
        status = XRT_MANAGED_ARGUMENT;
    XrtManagedHeader *old = NULL;
    if (status == XRT_MANAGED_OK && to.slot.owner != from.slot.owner) {
        XrtManagedHeader *value = *from.slot.owner;
        if (copy && value && !xrt_managed_retain(value)) status = XRT_MANAGED_REFCOUNT;
        else {
            old = *to.slot.owner;
            *to.slot.owner = value;
            if (!copy) *from.slot.owner = NULL;
        }
    }
    if (status == XRT_MANAGED_OK) {
        if (publication) publication->publish(publication->context);
        xrt_managed_drop(old);
    }
    place_unlock_pair(to, from);
    return status;
}
XRAY_API XrtManagedStatus xrt_place_copy_replace(XrtPlace destination, XrtPlace source,
    const XrtPlacePublication *publication) {
    return place_replace(destination, source, true, publication);
}
XRAY_API XrtManagedStatus xrt_place_move_replace(XrtPlace destination, XrtPlace source,
    const XrtPlacePublication *publication) {
    return place_replace(destination, source, false, publication);
}
XRAY_API XrtManagedStatus xrt_place_frame_move_replace(XrtManagedFrame *frame, size_t destination,
    size_t source) {
    return place_replace((XrtPlace){.frame=frame, .slot=destination, .nulls=XRT_PLACE_NULLABLE},
        (XrtPlace){.frame=frame, .slot=source, .nulls=XRT_PLACE_NONNULL}, false, NULL);
}

static XrtPlace edge_place(const XrtPlaceEdge *edges, size_t index) {
    size_t row = index / 3;
    return index % 3 == 0 ? edges[row].source : index % 3 == 1 ? edges[row].destination : edges[row].temporary;
}
static PlaceLocation edge_location(const XrtPlaceEdge *edges, const PlaceLocation *single, size_t index) {
    if (single) return single[index];
    PlaceLocation location;
    XR_CHECK(place_location(edge_place(edges, index), &location) == XRT_MANAGED_OK,
        "an exclusively borrowed edge descriptor retains its resolved location");
    return location;
}
static bool edge_first_domain(const XrtPlaceEdge *edges, const PlaceLocation *single,
    size_t index, XrtManagedSlotLocation domain) {
    for (size_t previous = 0; previous < index; ++previous)
        if (xrt_managed_domain_same(edge_location(edges, single, previous).slot, domain)) return false;
    return true;
}
static void edge_unlock(const XrtPlaceEdge *edges, const PlaceLocation *single, size_t nodes) {
    while (nodes) {
        size_t index = --nodes;
        PlaceLocation location = edge_location(edges, single, index);
        if (edge_first_domain(edges, single, index, location.slot)) xrt_managed_domain_leave(location.slot);
    }
}
static XrtManagedStatus edge_preflight(const XrtPlaceEdge *edges, const PlaceLocation *single, size_t count) {
    for (size_t n = 0; n < count; ++n) {
        if (edges[n].transfer != XRT_PLACE_TRANSFER_COPY && edges[n].transfer != XRT_PLACE_TRANSFER_MOVE)
            return XRT_MANAGED_ARGUMENT;
        PlaceLocation source = edge_location(edges, single, n * 3), destination = edge_location(edges, single, n * 3 + 1);
        PlaceLocation temporary = edge_location(edges, single, n * 3 + 2);
        if (!edges[n].temporary.frame || *temporary.slot.owner) return XRT_MANAGED_ARGUMENT;
        XrtManagedStatus status = place_check(source,
            edges[n].transfer == XRT_PLACE_TRANSFER_COPY ? XRT_PLACE_COPY_FROM : XRT_PLACE_MOVE_FROM);
        if (status == XRT_MANAGED_OK) status = place_check(destination, XRT_PLACE_REPLACE);
        if (status == XRT_MANAGED_OK) status = place_check(temporary, XRT_PLACE_REPLACE);
        if (status != XRT_MANAGED_OK) return status;
        if (!*source.slot.owner && (destination.nulls != XRT_PLACE_NULLABLE || temporary.nulls != XRT_PLACE_NULLABLE))
            return XRT_MANAGED_ARGUMENT;
        for (size_t p = 0; p < count * 3; ++p) {
            PlaceLocation other = edge_location(edges, single, p);
            if (p != n * 3 + 2 && temporary.slot.owner == other.slot.owner) return XRT_MANAGED_ARGUMENT;
            if (p < n * 3 && ((p % 3 == 0 && source.slot.owner == other.slot.owner &&
                    edges[n].transfer == XRT_PLACE_TRANSFER_MOVE && edges[p / 3].transfer == XRT_PLACE_TRANSFER_MOVE) ||
                (p % 3 == 1 && destination.slot.owner == other.slot.owner))) return XRT_MANAGED_ARGUMENT;
        }
    }
    return XRT_MANAGED_OK;
}
static void edge_copy_rollback(const XrtPlaceEdge *edges, const PlaceLocation *single, size_t count) {
    while (count) {
        size_t n = --count;
        if (edges[n].transfer != XRT_PLACE_TRANSFER_COPY) continue;
        PlaceLocation temporary = edge_location(edges, single, n * 3 + 2);
        XrtManagedHeader *value = *temporary.slot.owner;
        *temporary.slot.owner = NULL;
        xrt_managed_drop(value);
    }
}
static XrtManagedStatus edge_stage_copies(const XrtPlaceEdge *edges, const PlaceLocation *single, size_t count) {
    /* A COPY may read a MOVE source, so every fallible retain precedes any
     * source consumption and keeps rollback backed by the original owners. */
    for (size_t n = 0; n < count; ++n) {
        if (edges[n].transfer != XRT_PLACE_TRANSFER_COPY) continue;
        PlaceLocation source = edge_location(edges, single, n * 3), temporary = edge_location(edges, single, n * 3 + 2);
        XrtManagedHeader *value = *source.slot.owner;
        if (value && !xrt_managed_retain(value)) {
            edge_copy_rollback(edges, single, n);
            return XRT_MANAGED_REFCOUNT;
        }
        *temporary.slot.owner = value;
    }
    return XRT_MANAGED_OK;
}
XRAY_API XrtManagedStatus xrt_place_edge_transfer(const XrtPlaceEdge *edges, size_t count,
    const XrtPlacePublication *publication) {
    if ((count && !edges) || (publication && !publication->publish)) return XRT_MANAGED_ARGUMENT;
    if (count > SIZE_MAX / 3) return XRT_MANAGED_OVERFLOW;
    size_t nodes = count * 3;
    /* The synchronous borrow keeps these addresses and descriptors stable.
     * Pending tokens, owner values and guard state are still read under lock. */
    PlaceLocation locations[3];
    PlaceLocation *single = count == 1 ? locations : NULL;
    for (size_t i = 0; i < nodes; ++i) {
        PlaceLocation location;
        XrtManagedStatus status = place_location(edge_place(edges, i), &location);
        if (status != XRT_MANAGED_OK) { edge_unlock(edges, single, i); return status; }
        if (single) single[i] = location;
        for (size_t p = 0; p < i; ++p) {
            PlaceLocation previous = edge_location(edges, single, p);
            if (previous.slot.owner == location.slot.owner && !xrt_managed_domain_same(previous.slot, location.slot)) {
                edge_unlock(edges, single, i); return XRT_MANAGED_ARGUMENT;
            }
        }
        if (edge_first_domain(edges, single, i, location.slot)) {
            status = xrt_managed_domain_enter(location.slot);
            if (status != XRT_MANAGED_OK) { edge_unlock(edges, single, i); return status; }
        }
    }
    XrtManagedStatus status = edge_preflight(edges, single, count);
    if (status == XRT_MANAGED_OK) status = edge_stage_copies(edges, single, count);
    if (status == XRT_MANAGED_OK) {
        for (size_t n = 0; n < count; ++n) {
            if (edges[n].transfer != XRT_PLACE_TRANSFER_MOVE) continue;
            PlaceLocation source = edge_location(edges, single, n * 3), temporary = edge_location(edges, single, n * 3 + 2);
            *temporary.slot.owner = *source.slot.owner;
            *source.slot.owner = NULL;
        }
        for (size_t n = 0; n < count; ++n) {
            PlaceLocation destination = edge_location(edges, single, n * 3 + 1), temporary = edge_location(edges, single, n * 3 + 2);
            XrtManagedHeader *old = *destination.slot.owner;
            *destination.slot.owner = *temporary.slot.owner;
            *temporary.slot.owner = old;
        }
        if (publication) publication->publish(publication->context);
        for (size_t n = 0; n < count; ++n) {
            PlaceLocation temporary = edge_location(edges, single, n * 3 + 2);
            XrtManagedHeader *old = *temporary.slot.owner;
            *temporary.slot.owner = NULL;
            xrt_managed_drop(old);
        }
    }
    edge_unlock(edges, single, nodes);
    return status;
}
