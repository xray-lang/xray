"""Observe real generated Slice transactions without changing their ABI."""


def slice_boundary_observer_source(generated):
    text = r'''#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_values.h"
#include "runtime/native/xnative_managed_root.h"
#include "xray_native_array_slice.h"
#include "xray_native_values.h"
#include "xray_native_place.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr,"Slice observer line %d: %s\n",__LINE__,#c); abort(); } } while (0)
enum Fault {
    NO_FAULT, ROOT_ALLOCATE, FIRST_FRAME_ALLOCATE, LATE_FRAME_ALLOCATE, ARRAY_ALLOCATE,
    RETURN_RETAIN, RETURN_BUSY, EDGE_RETAIN, EDGE_ADOPT_BUSY, EDGE_BUSY, FAULT_COUNT
};
typedef struct Probe {
    enum Fault fault;
    XrtBillingOwner *billing;
    XrtManagedFrame *frames[16], *edge_frame, *stages[2];
    size_t stage_slots[2], depth, enters, leaves, layout_calls, retain_calls, stage_count, stage_drops;
    size_t returns, edges, injected, checked, payload_bytes;
    XrtManagedHeader *edge_header;
    int32_t edge_references;
    unsigned char payload_before[8192];
    bool edge_failed, completed;
} Probe;
static Probe probe;
static void same_storage(XrtBillingStats before) {
    XrtBillingStats after=xrt_billing_owner_stats(probe.billing);
    REQUIRE(after.bytes==before.bytes && after.blocks==before.blocks);
}
static void no_allocation(XrtBillingStats before) {
    same_storage(before);
    REQUIRE(xrt_billing_owner_stats(probe.billing).allocations==before.allocations);
}
static XrtManagedRoot *observed_root(uint32_t source) {
    if (probe.fault==ROOT_ALLOCATE && !probe.injected) {
        ++probe.injected; xrt_billing_owner_fail_after(probe.billing,0);
    }
    return xrt_native_managed_root(source);
}
static XrtManagedStatus observed_payload(XrtManagedRoot *root, const XrtManagedFrameSpec *spec,
    XrtManagedFrame **output, void **payload) {
    ++probe.layout_calls;
    bool fail=(probe.fault==FIRST_FRAME_ALLOCATE && probe.layout_calls==1) ||
        (probe.fault==LATE_FRAME_ALLOCATE && probe.layout_calls==3);
    XrtBillingStats before=xrt_billing_owner_stats(probe.billing);
    if (fail) { ++probe.injected; xrt_billing_owner_fail_after(probe.billing,0); }
    XrtManagedStatus status=xrt_managed_frame_enter_payload(root,spec,output,payload);
    if (fail) {
        xrt_billing_owner_fail_after(probe.billing,SIZE_MAX);
        REQUIRE(status==XRT_MANAGED_OOM && !*output && !*payload); same_storage(before); ++probe.checked;
    }
    if (status==XRT_MANAGED_OK) {
        void *actual=NULL; size_t bytes=0;
        REQUIRE(*output && xrt_managed_frame_payload(*output,&actual,&bytes)==XRT_MANAGED_OK && actual==*payload);
        REQUIRE(probe.depth<sizeof(probe.frames)/sizeof(*probe.frames));
        probe.frames[probe.depth++]=*output; ++probe.enters;
    }
    return status;
}
static XrtManagedStatus observed_leave(XrtManagedRoot *root, XrtManagedFrame *frame) {
    REQUIRE(probe.depth && probe.frames[probe.depth-1]==frame);
    XrtManagedStatus status=xrt_managed_frame_leave(root,frame);
    if (status==XRT_MANAGED_OK) { --probe.depth; ++probe.leaves; }
    return status;
}
static void observed_array(XrtManagedFrame *frame, size_t slot, const XrtArrayInput *input, uint32_t source) {
    if (probe.fault==ARRAY_ALLOCATE && !probe.injected) {
        /* Warm only real immutable type setup; the next actual Array backing
         * allocation fails, rather than an unrelated type-plan reservation. */
        const XrtBackingTypes *types=NULL;
        REQUIRE(xrt_native_values_backings(xrt_value_root_active(),&types)==XRT_MANAGED_OK && types);
        ++probe.injected; xrt_billing_owner_fail_after(probe.billing,0);
    }
    xrt_native_array_create(frame,slot,input,source);
}
static XrtManagedStatus observed_replace(const XrtInlineValueHome *home,
    const XrtInlineValueSpec *spec, const void *source) {
    bool returning=probe.depth && home->frame!=probe.frames[probe.depth-1] &&
        spec->size==sizeof(XrtArraySliceBundle);
    if (returning) ++probe.returns;
    if (returning && !probe.injected && (probe.fault==RETURN_RETAIN || probe.fault==RETURN_BUSY)) {
        /* Seed this actual caller landing through the existing transaction.
         * A distinct valid old window makes failed replacement observable. */
        const XrtArraySliceBundle *incoming=source;
        REQUIRE(incoming && incoming->hold && incoming->value.count>1);
        XrtArraySliceBundle old=*incoming; old.value.count=1;
        REQUIRE(xrt_inline_value_replace(home,spec,&old)==XRT_MANAGED_OK);
        XrtArraySliceBundle saved; memcpy(&saved,home->value,sizeof(saved));
        XrtBillingStats before=xrt_billing_owner_stats(probe.billing);
        XrtManagedStatus status;
        if (probe.fault==RETURN_RETAIN) {
            int32_t references=atomic_load(&incoming->hold->references);
            atomic_store(&incoming->hold->references,-INT32_MAX);
            status=xrt_inline_value_replace(home,spec,source);
            atomic_store(&incoming->hold->references,references);
            REQUIRE(status==XRT_MANAGED_REFCOUNT);
        } else {
            XrtManagedSlotLocation location={0};
            REQUIRE(xrt_managed_slot_location(home->frame,home->first_owner,&location)==XRT_MANAGED_OK);
            REQUIRE(xrt_managed_domain_enter(location)==XRT_MANAGED_OK);
            status=xrt_inline_value_replace(home,spec,source);
            xrt_managed_domain_leave(location); REQUIRE(status==XRT_MANAGED_ARGUMENT);
        }
        XrtArraySliceBundle empty={0};
        REQUIRE(!memcmp(&saved,home->value,sizeof(saved)) && !memcmp(&empty,home->temporary,sizeof(empty)));
        no_allocation(before); ++probe.injected; ++probe.checked; return status;
    }
    /* Landing/BIND transport is allocation-free. A real zero-allocation
     * budget must therefore preserve success, not manufacture an OOM. */
    XrtBillingStats before=xrt_billing_owner_stats(probe.billing);
    xrt_billing_owner_fail_after(probe.billing,0);
    XrtManagedStatus status=xrt_inline_value_replace(home,spec,source);
    xrt_billing_owner_fail_after(probe.billing,SIZE_MAX);
    no_allocation(before); return status;
}
static bool edge_fault(void) {
    return probe.fault==EDGE_RETAIN || probe.fault==EDGE_ADOPT_BUSY || probe.fault==EDGE_BUSY;
}
static void edge_snapshot(XrtManagedHeader *header) {
    REQUIRE(probe.depth);
    probe.edge_frame=probe.frames[probe.depth-1];
    void *payload=NULL; size_t bytes=0;
    REQUIRE(xrt_managed_frame_payload(probe.edge_frame,&payload,&bytes)==XRT_MANAGED_OK);
    REQUIRE(bytes<=sizeof(probe.payload_before));
    memcpy(probe.payload_before,payload,bytes); probe.payload_bytes=bytes;
    probe.edge_header=header; probe.edge_references=atomic_load(&header->references);
}
static bool observed_retain(XrtManagedHeader *header) {
    ++probe.retain_calls;
    if (edge_fault() && probe.retain_calls==1) edge_snapshot(header);
    if (probe.fault==EDGE_RETAIN && probe.retain_calls==2 && !probe.injected) {
        int32_t references=atomic_load(&header->references);
        atomic_store(&header->references,-INT32_MAX);
        bool retained=xrt_managed_retain(header);
        atomic_store(&header->references,references);
        REQUIRE(!retained); ++probe.injected; probe.edge_failed=true; return false;
    }
    return xrt_managed_retain(header);
}
static XrtManagedStatus observed_adopt(XrtManagedFrame *frame, size_t slot, XrtManagedHeader **incoming) {
    XrtManagedStatus status;
    if (probe.fault==EDGE_ADOPT_BUSY && probe.retain_calls==2 && !probe.injected) {
        XrtManagedSlotLocation location={0}; XrtManagedHeader *saved=*incoming;
        REQUIRE(xrt_managed_slot_location(frame,slot,&location)==XRT_MANAGED_OK);
        REQUIRE(!*location.owner && xrt_managed_domain_enter(location)==XRT_MANAGED_OK);
        status=xrt_managed_slot_adopt(frame,slot,incoming);
        xrt_managed_domain_leave(location);
        REQUIRE(status==XRT_MANAGED_ARGUMENT && *incoming==saved && !*location.owner);
        ++probe.injected; probe.edge_failed=true; return status;
    }
    status=xrt_managed_slot_adopt(frame,slot,incoming);
    if (edge_fault() && status==XRT_MANAGED_OK) {
        REQUIRE(probe.stage_count<sizeof(probe.stages)/sizeof(*probe.stages));
        probe.stages[probe.stage_count]=frame; probe.stage_slots[probe.stage_count++]=slot;
    }
    return status;
}
static XrtManagedStatus observed_drop(XrtManagedFrame *frame, size_t slot) {
    XrtManagedStatus status=xrt_managed_slot_drop(frame,slot);
    if (edge_fault() && probe.edge_failed) {
        REQUIRE(probe.stage_drops<probe.stage_count);
        size_t ordinal=probe.stage_count-1-probe.stage_drops;
        REQUIRE(frame==probe.stages[ordinal] && slot==probe.stage_slots[ordinal] && status==XRT_MANAGED_OK);
        XrtManagedHeader *remaining=NULL;
        REQUIRE(xrt_managed_slot_borrow(frame,slot,&remaining)==XRT_MANAGED_OK && !remaining);
        if (++probe.stage_drops==probe.stage_count) {
            void *payload=NULL; size_t bytes=0;
            REQUIRE(xrt_managed_frame_payload(probe.edge_frame,&payload,&bytes)==XRT_MANAGED_OK && bytes==probe.payload_bytes);
            REQUIRE(!memcmp(probe.payload_before,payload,bytes) &&
                atomic_load(&probe.edge_header->references)==probe.edge_references);
            ++probe.checked;
        }
    }
    return status;
}
static XrtManagedStatus observed_edge(const XrtPlaceEdge *edges, size_t count,
    const XrtPlacePublication *publication) {
    ++probe.edges;
    XrtBillingStats before=xrt_billing_owner_stats(probe.billing);
    xrt_billing_owner_fail_after(probe.billing,0);
    XrtManagedStatus status;
    if (probe.fault==EDGE_BUSY && !probe.injected) {
        REQUIRE(count==2 && edges[0].destination.frame);
        XrtManagedSlotLocation location={0};
        REQUIRE(xrt_managed_slot_location(edges[0].destination.frame,edges[0].destination.slot,&location)==XRT_MANAGED_OK);
        REQUIRE(xrt_managed_domain_enter(location)==XRT_MANAGED_OK);
        status=xrt_place_edge_transfer(edges,count,publication);
        xrt_managed_domain_leave(location);
        REQUIRE(status==XRT_MANAGED_ARGUMENT); ++probe.injected; probe.edge_failed=true;
    } else status=xrt_place_edge_transfer(edges,count,publication);
    xrt_billing_owner_fail_after(probe.billing,SIZE_MAX);
    no_allocation(before); return status;
}
#define xrt_native_managed_root observed_root
#define xrt_managed_frame_enter_payload(root,spec,out,payload) observed_payload((root),(spec),(out),(payload))
#define xrt_managed_frame_leave observed_leave
#define xrt_native_array_create observed_array
#define xrt_inline_value_replace observed_replace
#define xrt_managed_retain observed_retain
#define xrt_managed_slot_adopt observed_adopt
#define xrt_managed_slot_drop observed_drop
#define xrt_place_edge_transfer observed_edge
'''
    text += f'#include "{generated.name}"\n'
    text += r'''#undef xrt_native_managed_root
#undef xrt_managed_frame_enter_payload
#undef xrt_managed_frame_leave
#undef xrt_native_array_create
#undef xrt_inline_value_replace
#undef xrt_managed_retain
#undef xrt_managed_slot_adopt
#undef xrt_managed_slot_drop
#undef xrt_place_edge_transfer
typedef struct Observation { XrtValueRoot values; } Observation;
static void observe(void *data) {
    (void)data;
    int64_t answer=0; xrl_invoke(&answer);
    REQUIRE(answer==68 && !probe.depth && probe.enters==probe.leaves && probe.returns && probe.edges);
    probe.completed=true;
}
int main(void) {
    XrtLocationOwner *locations=NULL;
    REQUIRE(xrt_locations_create(xrt_standalone_source_map.locations,xrt_standalone_source_map.count,&locations)==XRT_LOCATION_OK);
    XrtBillingOwner *control=NULL;
    REQUIRE(xrt_billing_owner_new(NULL,SIZE_MAX,&control)==XRT_MANAGED_OK);
    XrtBillingStats empty=xrt_billing_owner_stats(control);
    REQUIRE(empty.bytes && empty.blocks==1);
    for (enum Fault fault=NO_FAULT; fault<FAULT_COUNT; fault=(enum Fault)(fault+1)) {
        memset(&probe,0,sizeof(probe)); probe.fault=fault;
        Observation observation={0}; XrtExecutionFailure failure={0};
        REQUIRE(xrt_native_values_prepare(&observation.values,&probe.billing)==XRT_MANAGED_OK);
        REQUIRE(xrt_billing_owner_retain(probe.billing));
        XrtExecutionStatus status=xrt_run_embedded_owned(locations,&failure,observe,&observation,&observation.values);
        REQUIRE(xrt_value_root_empty(&observation.values));
        xrt_billing_owner_fail_after(probe.billing,SIZE_MAX);
        XrtBillingStats final=xrt_billing_owner_stats(probe.billing);
        /* The observer still owns the billing handle. Its actual control
         * allocation remains charged until that final handle is dropped. */
        if (final.bytes!=empty.bytes || final.blocks!=empty.blocks)
            fprintf(stderr,"Slice refund mode %u: final %zu/%zu, control %zu/%zu\n",
                (unsigned)fault,final.bytes,final.blocks,empty.bytes,empty.blocks);
        REQUIRE(final.bytes==empty.bytes && final.blocks==empty.blocks);
        if (fault==NO_FAULT) REQUIRE(status==XRT_EXECUTION_COMPLETED && probe.completed && !probe.injected);
        else {
            XrXirValueStatus expected=(fault<=ARRAY_ALLOCATE ? XR_XIR_VALUE_OOM :
                fault==RETURN_RETAIN || fault==EDGE_RETAIN ? XR_XIR_VALUE_REFCOUNT_LIMIT : XR_XIR_VALUE_BAD_ARGUMENT);
            REQUIRE(status==XRT_EXECUTION_FAILURE && !probe.completed && probe.injected==1 &&
                failure.kind==XRT_FAILURE_VALUE && failure.detail.value.status==expected);
            if (fault==FIRST_FRAME_ALLOCATE || fault==LATE_FRAME_ALLOCATE || fault>=RETURN_RETAIN) REQUIRE(probe.checked==1);
        }
        printf("{\"mode\":%u,\"execution\":%u,\"injected\":%zu,\"checked\":%zu,\"stageDrops\":%zu,\"returns\":%zu,\"edges\":%zu,\"bytes\":%zu,\"blocks\":%zu,\"controlBytes\":%zu,\"controlBlocks\":%zu}\n",
            (unsigned)fault,(unsigned)status,probe.injected,probe.checked,probe.stage_drops,probe.returns,probe.edges,
            final.bytes,final.blocks,empty.bytes,empty.blocks);
        REQUIRE(fflush(stdout)==0);
        xrt_billing_owner_drop(probe.billing);
        probe.billing=NULL;
    }
    xrt_billing_owner_drop(control);
    xrt_locations_release(locations);
    return 0;
}
'''
    return text
