"""Execute the exact emitted NEW/READ handoffs with real runtime fault controls."""
from pathlib import Path
import re


def _block_end(text, opening):
    depth = 0
    for position in range(opening, len(text)):
        depth += text[position] == "{"
        depth -= text[position] == "}"
        if not depth:
            return position + 1
    raise ValueError("unterminated generated forwarding conditional")


def array_forward_observer_source(generated):
    generated = Path(generated)
    emitted = generated.read_text(encoding="ascii")
    selected = {}
    pattern = re.compile(
        r"    xrl_binding_forwarded = xrt_managed_frame_try_forward\(xrl_frame, xrl_payload, "
        r"&xrl_frame_locations, \(XrtManagedFrameForward\)\{(\d+), (\d+), (\d+), (\d+)\}\);\n")
    for match in pattern.finditer(emitted):
        position = match.end()
        for _ in range(2):
            conditional = re.match(r"\s*if \(!xrl_binding_forwarded\) \{", emitted[position:])
            if not conditional:
                raise ValueError("COPY and adjacent DROP must be the two consecutive guarded blocks")
            position = _block_end(emitted, position + conditional.end() - 1)
        fragment = emitted[match.start():position]
        if re.search(r"\b(?:v|p)\d+\b|xrl_root|enter_(?:payload|locations)", fragment):
            raise ValueError("forwarding fragment depends on other values or inserts Frame setup")
        if fragment.count("xrt_place_copy_replace(") != 1 or fragment.count("xrt_managed_slot_drop(") != 1:
            raise ValueError("original COPY and original DROP must both remain")
        kinds = re.findall(r"\.nulls=(XRT_PLACE_\w+)", fragment)
        if len(kinds) != 2 or kinds[1] != "XRT_PLACE_NONNULL":
            raise ValueError("forwarding fragment changed original source nullability")
        kind = "READ" if kinds[0] == "XRT_PLACE_NULLABLE" else "NEW"
        source_ids = re.findall(r"xrt_native_managed_failure\(status, UINT32_C\((\d+)\)\)", fragment)
        factories = re.findall(r"enter_locations\(xrl_root, &xrl_frame_(\d+), &xrl_frame_locations\)", emitted[:match.start()])
        if len(source_ids) != 2 or not factories:
            raise ValueError("fragment lost original diagnostics or actual factory locations")
        function = factories[-1]
        owner_vector = re.search(rf"xrl_owners_{function}\[\] = \{{(.*?)\}};", emitted, re.DOTALL)
        if not owner_vector:
            raise ValueError("missing actual Frame owner vector")
        offsets = []
        for token in (x.strip() for x in owner_vector[1].split(",") if x.strip()):
            offset = re.fullmatch(r"UINT64_C\((\d+)\)", token)
            if token != "SIZE_MAX" and not offset:
                raise ValueError("unexpected physical offset representation")
            offsets.append(int(offset[1]) if offset else None)
        slots = tuple(map(int, match.groups()))
        if slots[0] == slots[2] or slots[1] == slots[3] or offsets[slots[0]] != slots[1] or offsets[slots[2]] != slots[3]:
            raise ValueError("forwarding arguments do not match the real independent owner slots")
        selected.setdefault(kind, (fragment, function, slots, int(source_ids[0])))
    if set(selected) != {"NEW", "READ"}:
        raise ValueError("real generated input must contain both NEW and READ forwarding pairs")
    text = r'''#include "runtime/native/xnative_array_backing.h"
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_managed_root.h"
#include "runtime/native/xnative_values.h"
#include "xray_native_values.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr,"Array forward observer line %d: %s\n",__LINE__,#c); abort(); } } while (0)
'''
    text += f'#include "{generated.name}"\n'
    text += r'''
enum ForwardMode { LOCAL_ONE, SNAPSHOT_TWO, BUSY, SOURCE_PENDING, TARGET_PENDING,
    OLD_TARGET, EMPTY_SOURCE, COUNT_MAX, COUNT_ZERO, SHARED_ONE, SHARED_LIMIT, SAME_TARGET, MODE_COUNT };
typedef struct ForwardObservation {
    XrtValueRoot values;
    XrtBillingOwner *billing;
    XrtManagedFrameLocations view;
    XrtManagedSlotLocation domain;
    XrtManagedHeader *incoming, *snapshot, *before_source, *before_target;
    XrtBillingStats before, without_old;
    size_t pair, attempts;
    enum ForwardMode mode;
    int32_t count_before, count_after;
    bool fast, completed, failed, count_fault, domain_held;
    uint32_t failure_source;
    XrtManagedStatus failure_status;
} ForwardObservation;
static ForwardObservation observation;
static void unchanged_storage(XrtBillingStats before, XrtBillingStats after) {
    REQUIRE(before.bytes==after.bytes && before.blocks==after.blocks);
}
static XrtManagedHeader **owner_field(size_t offset) {
    return (XrtManagedHeader **)((unsigned char *)observation.view.payload+offset);
}
'''
    geometries, specs, source_ids = [], [], []
    for kind in ("NEW", "READ"):
        _, function, slots, source_id = selected[kind]
        geometries.append("{" + ",".join(map(str, slots)) + "}")
        specs.append(f"&xrl_frame_{function}")
        source_ids.append(str(source_id))
    text += "static const size_t geometry[2][4] = {" + ",".join(geometries) + "};\n"
    text += "static const XrtManagedFrameSpec *const frame_specs[2] = {" + ",".join(specs) + "};\n"
    text += "static const uint32_t copy_sources[2] = {" + ",".join(source_ids) + "};\n"
    text += r'''
static bool observed_try(XrtManagedFrame *frame, void *payload, const XrtManagedFrameLocations *locations,
    XrtManagedFrameForward transfer) {
    size_t source=transfer.source_slot, source_offset=transfer.source_offset;
    size_t target=transfer.destination_slot, target_offset=transfer.destination_offset;
    REQUIRE(frame==observation.view.frame && payload==observation.view.payload && locations->frame==frame &&
        locations->payload==payload && source==geometry[observation.pair][0] && source_offset==geometry[observation.pair][1] &&
        target==geometry[observation.pair][2] && target_offset==geometry[observation.pair][3]);
    XrtManagedHeader *from=*owner_field(source_offset), *to=*owner_field(target_offset);
    int32_t count=from?atomic_load_explicit(&from->references,memory_order_relaxed):0;
    XrtBillingStats before=xrt_billing_owner_stats(observation.billing);
    bool success=xrt_managed_frame_try_forward(frame,payload,locations,transfer);
    ++observation.attempts; observation.fast=success;
    REQUIRE(*owner_field(source_offset)==(success?NULL:from) && *owner_field(target_offset)==(success?from:to));
    REQUIRE(!from || atomic_load_explicit(&from->references,memory_order_relaxed)==count);
    unchanged_storage(before,xrt_billing_owner_stats(observation.billing));
    REQUIRE(before.allocations==xrt_billing_owner_stats(observation.billing).allocations);
    return success;
}
static _Noreturn void observed_failure(XrtManagedStatus status, uint32_t source) {
    const size_t *g=geometry[observation.pair];
    REQUIRE(!observation.fast && observation.attempts==1 && source==copy_sources[observation.pair] &&
        *owner_field(g[1])==observation.before_source && *owner_field(g[3])==observation.before_target);
    REQUIRE(!observation.incoming || atomic_load_explicit(&observation.incoming->references,memory_order_relaxed)==observation.count_before);
    unchanged_storage(observation.before,xrt_billing_owner_stats(observation.billing));
    REQUIRE(observation.before.allocations==xrt_billing_owner_stats(observation.billing).allocations);
    observation.failed=true; observation.failure_status=status; observation.failure_source=source;
    /* Restore only the deliberately injected fault before actual runtime drain. */
    if (observation.count_fault) atomic_store_explicit(&observation.incoming->references,1,memory_order_relaxed);
    if (observation.domain_held) { xrt_managed_domain_leave(observation.domain); observation.domain_held=false; }
    xrt_native_managed_failure(status,source);
}
'''
    for index, kind in enumerate(("NEW", "READ")):
        fragment = selected[kind][0].replace("xrt_managed_frame_try_forward(", "observed_try(")
        fragment = fragment.replace("xrt_native_managed_failure(", "observed_failure(")
        text += f'''static void emitted_pair_{index}(XrtManagedFrame *xrl_frame, void *xrl_payload,
    const XrtManagedFrameLocations *factory) {{
    XrtManagedFrameLocations xrl_frame_locations=*factory;
    bool xrl_binding_forwarded=false;
{fragment}
}}
'''
    text += r'''
static void execute_pair(void *unused) {
    (void)unused;
    const XrtBackingTypes *types=NULL;
    REQUIRE(xrt_native_values_backings(&observation.values,&types)==XRT_MANAGED_OK);
    XrtManagedRoot *root=xrt_native_managed_root(copy_sources[observation.pair]);
    REQUIRE(xrt_managed_frame_enter_locations(root,frame_specs[observation.pair],&observation.view)==XRT_MANAGED_OK);
    const size_t *g=geometry[observation.pair];
    if (observation.mode!=EMPTY_SOURCE) {
        int64_t key=17;
        REQUIRE(xrt_array_backing_i64_create(observation.view.frame,g[0],types,&key,1,1)==XRT_MANAGED_OK);
        observation.incoming=*owner_field(g[1]); REQUIRE(observation.incoming);
    }
    if (observation.mode==SNAPSHOT_TWO) {
        observation.snapshot=observation.incoming; REQUIRE(xrt_managed_retain(observation.snapshot));
    }
    observation.without_old=xrt_billing_owner_stats(observation.billing);
    if (observation.mode==OLD_TARGET) {
        int64_t old=23;
        REQUIRE(xrt_array_backing_i64_create(observation.view.frame,g[2],types,&old,1,1)==XRT_MANAGED_OK);
    }
    if (observation.mode==SAME_TARGET) {
        XrtManagedHeader *owned=observation.incoming; REQUIRE(xrt_managed_retain(owned));
        REQUIRE(xrt_managed_slot_adopt(observation.view.frame,g[2],&owned)==XRT_MANAGED_OK && !owned);
    }
    if (observation.mode==SOURCE_PENDING || observation.mode==TARGET_PENDING) {
        size_t slot=observation.mode==SOURCE_PENDING?g[0]:g[2]; XrtManagedHeader *pending=NULL;
        REQUIRE(xrt_managed_slot_prepare(observation.view.frame,slot,types->arrays[XRT_BACKING_ARRAY_I64],
            sizeof(XrtArrayBacking)+sizeof(int64_t),&pending)==XRT_MANAGED_OK);
        XrtArrayBacking *array=(XrtArrayBacking *)pending; REQUIRE(array && array->capacity==1);
        array->length=1; int64_t key=29; memcpy(array->elements,&key,sizeof(key));
    }
    if (observation.mode==BUSY) {
        REQUIRE(xrt_managed_slot_location(observation.view.frame,g[0],&observation.domain)==XRT_MANAGED_OK);
        REQUIRE(xrt_managed_domain_enter(observation.domain)==XRT_MANAGED_OK); observation.domain_held=true;
    }
    /* Counter-only faults retain real allocated storage; restore before drain. */
    if (observation.mode==COUNT_MAX || observation.mode==COUNT_ZERO ||
        observation.mode==SHARED_ONE || observation.mode==SHARED_LIMIT) {
        int32_t count=observation.mode==COUNT_MAX?INT32_MAX:observation.mode==COUNT_ZERO?0:
            observation.mode==SHARED_ONE?-1:-INT32_MAX;
        atomic_store_explicit(&observation.incoming->references,count,memory_order_relaxed); observation.count_fault=true;
    }
    observation.before_source=*owner_field(g[1]); observation.before_target=*owner_field(g[3]);
    observation.count_before=observation.incoming?atomic_load_explicit(&observation.incoming->references,memory_order_relaxed):0;
    observation.before=xrt_billing_owner_stats(observation.billing);
    if (!observation.pair) emitted_pair_0(observation.view.frame,observation.view.payload,&observation.view);
    else emitted_pair_1(observation.view.frame,observation.view.payload,&observation.view);
    REQUIRE(observation.attempts==1 && !*owner_field(g[1]) && *owner_field(g[3])==observation.incoming);
    observation.count_after=atomic_load_explicit(&observation.incoming->references,memory_order_relaxed);
    REQUIRE(observation.count_after==(observation.mode==SNAPSHOT_TWO?2:observation.mode==SHARED_ONE?-1:1));
    REQUIRE(observation.fast==(observation.mode==LOCAL_ONE || observation.mode==SNAPSHOT_TWO));
    unchanged_storage(observation.mode==OLD_TARGET?observation.without_old:observation.before,
        xrt_billing_owner_stats(observation.billing));
    REQUIRE(observation.before.allocations==xrt_billing_owner_stats(observation.billing).allocations);
    if (observation.count_fault) atomic_store_explicit(&observation.incoming->references,1,memory_order_relaxed);
    observation.completed=true;
}
int main(void) {
    XrtLocationOwner *locations=NULL;
    REQUIRE(xrt_locations_create(xrt_standalone_source_map.locations,xrt_standalone_source_map.count,&locations)==XRT_LOCATION_OK);
    XrtBillingOwner *control=NULL; REQUIRE(xrt_billing_owner_new(NULL,SIZE_MAX,&control)==XRT_MANAGED_OK);
    XrtBillingStats empty=xrt_billing_owner_stats(control);
    for (size_t pair=0;pair<2;++pair) for (unsigned mode=0;mode<MODE_COUNT;++mode) {
        memset(&observation,0,sizeof(observation)); observation.pair=pair; observation.mode=(enum ForwardMode)mode;
        REQUIRE(xrt_native_values_prepare(&observation.values,&observation.billing)==XRT_MANAGED_OK);
        REQUIRE(xrt_billing_owner_retain(observation.billing));
        XrtExecutionFailure failure={0};
        XrtExecutionStatus status=xrt_run_embedded_owned(locations,&failure,execute_pair,NULL,&observation.values);
        REQUIRE(xrt_value_root_empty(&observation.values));
        if (observation.snapshot) { REQUIRE(atomic_load_explicit(&observation.snapshot->references,memory_order_relaxed)==1);
            xrt_managed_drop(observation.snapshot); observation.snapshot=NULL; }
        bool fails=mode==BUSY || mode==SOURCE_PENDING || mode==TARGET_PENDING || mode==EMPTY_SOURCE ||
            mode==COUNT_MAX || mode==COUNT_ZERO || mode==SHARED_LIMIT;
        if (fails) {
            XrtManagedStatus expected=(mode==COUNT_MAX || mode==COUNT_ZERO || mode==SHARED_LIMIT)?XRT_MANAGED_REFCOUNT:XRT_MANAGED_ARGUMENT;
            XrXirValueStatus value=expected==XRT_MANAGED_REFCOUNT?XR_XIR_VALUE_REFCOUNT_LIMIT:XR_XIR_VALUE_BAD_ARGUMENT;
            REQUIRE(status==XRT_EXECUTION_FAILURE && observation.failed && !observation.completed &&
                observation.failure_status==expected && failure.kind==XRT_FAILURE_VALUE && failure.detail.value.status==value &&
                failure.detail.value.source_id==copy_sources[pair]);
        } else REQUIRE(status==XRT_EXECUTION_COMPLETED && observation.completed && !observation.failed);
        XrtBillingStats final=xrt_billing_owner_stats(observation.billing); unchanged_storage(empty,final);
        printf("{\"pair\":%zu,\"mode\":%u,\"fast\":%u,\"completed\":%u,\"attempts\":%zu,\"countBefore\":%d,\"countAfter\":%d,\"bytes\":%zu,\"blocks\":%zu,\"controlBytes\":%zu,\"controlBlocks\":%zu}\n",
            pair,mode,(unsigned)observation.fast,(unsigned)observation.completed,observation.attempts,
            (int)observation.count_before,(int)observation.count_after,final.bytes,final.blocks,empty.bytes,empty.blocks);
        REQUIRE(fflush(stdout)==0); xrt_billing_owner_drop(observation.billing);
    }
    xrt_billing_owner_drop(control); xrt_locations_release(locations); return 0;
}
'''
    return text
