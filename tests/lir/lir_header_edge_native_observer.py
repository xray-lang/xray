"""Exercise an actual generated Header MOVE helper against canonical live state."""

from pathlib import Path
import re


def header_edge_observer_source(generated):
    generated = Path(generated)
    emitted = generated.read_text(encoding="utf-8")
    signature = re.search(
        r"static\s+XrtManagedStatus\s+(xrl_edge_(\d+)_\d+_\d+)\s*\(\s*"
        r"XrtManagedFrame\s*\*\s*frame\s*,\s*void\s*\*\s*payload\s*,\s*"
        r"const\s+XrtPlacePublication\s*\*\s*scalars\s*,\s*"
        r"const\s+XrtManagedFrameLocations\s*\*\s*locations\s*\)\s*\{(.*?)\n\}",
        emitted, re.DOTALL,
    )
    if not signature:
        raise ValueError("generated C has no closed Header edge with canonical locations")
    helper, function, body = signature.groups()
    positions = re.findall(r"locations->slots\[(\d+)\]\.owner_offset\s*==\s*(\d+)", body)
    count = re.search(r"locations->slot_count\s*==\s*(\d+)", body)
    spec = re.search(
        rf"static\s+const\s+XrtManagedFrameSpec\s+xrl_frame_{function}\s*=\s*"
        r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*xrl_owners_\d+\s*\}", emitted,
    )
    if len(positions) != 3 or not count or not spec or count.group(1) != spec.group(1):
        raise ValueError("Header helper lost its exact three owner locations or real Frame spec")
    slots, offsets = zip(*[(int(slot), int(offset)) for slot, offset in positions])
    if len(set(slots)) != 3 or len(set(offsets)) != 3 or max(slots) >= int(count.group(1)):
        raise ValueError("Header helper locations are not three distinct in-range owners")

    text = r'''#include "runtime/native/xnative_array_backing.h"
#include "runtime/native/xnative_managed_root.h"
#include "xray_native_place.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr,"Header edge observer line %d: %s\n",__LINE__,#c); abort(); } } while (0)
static size_t observed_fallbacks;
static XrtManagedStatus observed_place_edge(const XrtPlaceEdge *edges, size_t count,
    const XrtPlacePublication *publication) {
    ++observed_fallbacks;
    return xrt_place_edge_transfer(edges,count,publication);
}
#define xrt_place_edge_transfer observed_place_edge
'''
    text += f'#include "{generated.name}"\n#undef xrt_place_edge_transfer\n'
    text += f'''static const size_t observed_slots[] = {{{slots[0]}, {slots[1]}, {slots[2]}}};
static const size_t observed_offsets[] = {{{offsets[0]}, {offsets[1]}, {offsets[2]}}};
static const XrtManagedFrameSpec *observed_frame_spec(void) {{ return &xrl_frame_{function}; }}
static XrtManagedStatus observed_helper(XrtManagedFrame *frame, void *payload,
    const XrtPlacePublication *scalars, const XrtManagedFrameLocations *locations) {{
    return {helper}(frame,payload,scalars,locations);
}}
'''
    text += r'''
enum HeaderMode {
    FAST_MOVE, PENDING_SOURCE, PENDING_DESTINATION, PENDING_TEMPORARY, BUSY_ROOT,
    REPLACE_TARGET, OCCUPIED_TEMPORARY, OTHER_FRAME_VIEW, SCALAR_PUBLICATION, HEADER_MODE_COUNT
};
static XrtManagedHeader *observed_owner(XrtManagedFrame *frame, size_t slot) {
    XrtManagedHeader *owner=NULL;
    REQUIRE(xrt_managed_slot_borrow(frame,slot,&owner)==XRT_MANAGED_OK);
    return owner;
}
static void same_storage(XrtBillingStats before, XrtBillingStats after) {
    REQUIRE(before.bytes==after.bytes && before.blocks==after.blocks);
}
static void unchanged_cost(XrtBillingStats before, XrtBillingStats after) {
    same_storage(before,after); REQUIRE(before.allocations==after.allocations);
}
static XrtManagedHeader *create_array(const XrtManagedFrameLocations *view,
    const XrtBackingTypes *types, size_t slot, int64_t key) {
    REQUIRE(xrt_array_backing_i64_create(view->frame,slot,types,&key,1,1)==XRT_MANAGED_OK);
    return observed_owner(view->frame,slot);
}
static void actual_locations(const XrtManagedFrameLocations *view) {
    REQUIRE(view->frame && view->payload && view->slots && view->busy && !*view->busy &&
        view->slot_count==observed_frame_spec()->slot_count);
    for (size_t n=0;n<3;++n) {
        REQUIRE(observed_slots[n]<view->slot_count && view->slots[observed_slots[n]].owner_offset==observed_offsets[n]);
        XrtManagedSlotLocation location={0};
        REQUIRE(xrt_managed_slot_location(view->frame,observed_slots[n],&location)==XRT_MANAGED_OK);
        REQUIRE(location.pending==&view->slots[observed_slots[n]].pending && location.busy==view->busy &&
            (void *)location.owner==(unsigned char *)view->payload+observed_offsets[n]);
    }
}
typedef struct HeaderPublication {
    const XrtManagedFrameLocations *view;
    XrtManagedHeader *incoming;
    size_t count;
} HeaderPublication;
static void published(void *context) {
    HeaderPublication *publication=context;
    REQUIRE(publication->view->busy && *publication->view->busy);
    REQUIRE(!observed_owner(publication->view->frame,observed_slots[0]) &&
        observed_owner(publication->view->frame,observed_slots[1])==publication->incoming);
    ++publication->count;
}
static void run_header_mode(enum HeaderMode mode) {
    XrtBillingOwner *billing=NULL;
    REQUIRE(xrt_billing_owner_new(NULL,SIZE_MAX,&billing)==XRT_MANAGED_OK);
    XrtBillingStats empty=xrt_billing_owner_stats(billing);
    XrtBackingTypes types={0}; REQUIRE(xrt_backing_types_init(billing,&types)==XRT_MANAGED_OK);
    XrtManagedRoot *root=NULL; REQUIRE(xrt_managed_root_new(billing,&root)==XRT_MANAGED_OK);
    XrtManagedFrameLocations view={0},other={0};
    REQUIRE(xrt_managed_frame_enter_locations(root,observed_frame_spec(),&view)==XRT_MANAGED_OK);
    actual_locations(&view);
    XrtManagedHeader *incoming=create_array(&view,&types,observed_slots[0],11);
    REQUIRE(incoming);
    int32_t references=atomic_load(&incoming->references);
    XrtBillingStats without_replaced=xrt_billing_owner_stats(billing);
    if (mode==REPLACE_TARGET) REQUIRE(create_array(&view,&types,observed_slots[1],22));
    if (mode==OCCUPIED_TEMPORARY) REQUIRE(create_array(&view,&types,observed_slots[2],33));
    XrtManagedHeader *other_owners[3]={0}; int32_t other_references[3]={0};
    if (mode==OTHER_FRAME_VIEW) {
        REQUIRE(xrt_managed_frame_enter_locations(root,observed_frame_spec(),&other)==XRT_MANAGED_OK);
        actual_locations(&other);
        REQUIRE(other.frame!=view.frame && other.payload!=view.payload && other.busy==view.busy);
        for (size_t n=0;n<3;++n) {
            other_owners[n]=create_array(&other,&types,observed_slots[n],44+(int64_t)n);
            REQUIRE(other_owners[n]); other_references[n]=atomic_load(&other_owners[n]->references);
        }
    }
    size_t pending_slot=SIZE_MAX; XrtBillingStats without_pending=xrt_billing_owner_stats(billing);
    if (mode>=PENDING_SOURCE && mode<=PENDING_TEMPORARY) {
        pending_slot=observed_slots[(unsigned)mode-(unsigned)PENDING_SOURCE];
        XrtManagedHeader *pending=NULL;
        REQUIRE(xrt_managed_slot_prepare(view.frame,pending_slot,types.arrays[XRT_BACKING_ARRAY_I64],
            sizeof(XrtArrayBacking)+sizeof(int64_t),&pending)==XRT_MANAGED_OK);
        XrtArrayBacking *array=(XrtArrayBacking *)pending;
        REQUIRE(array && array->capacity==1); array->length=1;
        int64_t key=77; memcpy(array->elements,&key,sizeof(key));
        REQUIRE(!*view.busy && view.slots[pending_slot].pending.value==pending &&
            view.slots[pending_slot].pending.allocation_bytes);
    }
    XrtManagedHeader *before_owners[3]={0};
    for (size_t n=0;n<3;++n) before_owners[n]=observed_owner(view.frame,observed_slots[n]);
    XrtManagedSlotLocation domain={0};
    REQUIRE(xrt_managed_slot_location(view.frame,observed_slots[0],&domain)==XRT_MANAGED_OK);
    HeaderPublication publication={&view,incoming,0};
    XrtPlacePublication scalars={published,&publication};
    XrtBillingStats before=xrt_billing_owner_stats(billing);
    observed_fallbacks=0; xrt_billing_owner_fail_after(billing,0);
    if (mode==BUSY_ROOT) REQUIRE(xrt_managed_domain_enter(domain)==XRT_MANAGED_OK && *view.busy);
    XrtManagedStatus status=observed_helper(view.frame,view.payload,
        mode==SCALAR_PUBLICATION ? &scalars : NULL,mode==OTHER_FRAME_VIEW ? &other : &view);
    if (mode==BUSY_ROOT) { REQUIRE(*view.busy); xrt_managed_domain_leave(domain); }
    XrtBillingStats after=xrt_billing_owner_stats(billing);
    xrt_billing_owner_fail_after(billing,SIZE_MAX);
    bool failed=(mode>=PENDING_SOURCE && mode<=BUSY_ROOT) || mode==OCCUPIED_TEMPORARY;
    REQUIRE(status==(failed ? XRT_MANAGED_ARGUMENT : XRT_MANAGED_OK) &&
        observed_fallbacks==(mode==FAST_MOVE ? 0u : 1u) && !*view.busy);
    REQUIRE(atomic_load(&incoming->references)==references && after.allocations==before.allocations);
    if (failed) {
        unchanged_cost(before,after);
        for (size_t n=0;n<3;++n) REQUIRE(observed_owner(view.frame,observed_slots[n])==before_owners[n]);
    } else {
        REQUIRE(!observed_owner(view.frame,observed_slots[0]) &&
            observed_owner(view.frame,observed_slots[1])==incoming && !observed_owner(view.frame,observed_slots[2]));
        if (mode==REPLACE_TARGET) same_storage(without_replaced,after);
        else unchanged_cost(before,after);
    }
    REQUIRE(publication.count==(mode==SCALAR_PUBLICATION ? 1u : 0u));
    if (pending_slot!=SIZE_MAX) {
        REQUIRE(view.slots[pending_slot].pending.value && view.slots[pending_slot].pending.allocation_bytes);
        REQUIRE(xrt_managed_slot_abort(view.frame,pending_slot)==XRT_MANAGED_OK);
        same_storage(without_pending,xrt_billing_owner_stats(billing));
    }
    if (mode==OTHER_FRAME_VIEW) for (size_t n=0;n<3;++n) {
        REQUIRE(observed_owner(other.frame,observed_slots[n])==other_owners[n] &&
            atomic_load(&other_owners[n]->references)==other_references[n]);
    }
    REQUIRE(xrt_managed_root_destroy(&root)==XRT_MANAGED_OK && !root);
    xrt_backing_types_drop(&types);
    XrtBillingStats final=xrt_billing_owner_stats(billing); same_storage(empty,final);
    printf("{\"mode\":%u,\"status\":%u,\"fallback\":%zu,\"publication\":%zu,\"allocationDelta\":%zu,"
        "\"bytes\":%zu,\"blocks\":%zu,\"controlBytes\":%zu,\"controlBlocks\":%zu}\n",
        (unsigned)mode,(unsigned)status,observed_fallbacks,publication.count,after.allocations-before.allocations,
        final.bytes,final.blocks,empty.bytes,empty.blocks);
    REQUIRE(fflush(stdout)==0); xrt_billing_owner_drop(billing);
}
int main(void) {
    for (unsigned mode=0;mode<HEADER_MODE_COUNT;++mode) run_header_mode((enum HeaderMode)mode);
    return 0;
}
'''
    return text
