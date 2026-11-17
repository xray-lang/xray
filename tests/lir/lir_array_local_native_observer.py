"""Run actual emitted SET/PUSH fragments across real cold setup and fallback."""
from pathlib import Path
import re
from lir_array_forward_native_observer import _block_end


def array_local_observer_source(generated):
    generated = Path(generated)
    text = generated.read_text(encoding="ascii")
    if '#include "xray_native_array_local.h"' not in text:
        raise ValueError("I64 local operations did not select the separate native header")
    selected = {}
    prefix = re.compile(r"    \{\n    const int64_t xrl_array_index = ([^;\n]+);\n"
                        r"    const int64_t xrl_array_item = ([^;\n]+);\n"
                        r"    const XrtArrayUpdate xrl_array_update = \{(XRT_ARRAY_UPDATE_(?:SET|PUSH)),[^\n]+\n")
    for match in prefix.finditer(text):
        fragment = text[match.start():_block_end(text, match.start()+4)]
        kernel = re.search(r"xrt_array_i64_try_update_local\(xrl_frame, xrl_payload, &xrl_frame_locations, &xrl_frame_(\d+),\s*"
                           r"\(XrtArrayI64LocalUpdate\)\{(\d+), (\d+), (\d+), (\d+), (XRT_ARRAY_UPDATE_(?:SET|PUSH)), "
                           r"xrl_array_update.index, xrl_array_update.item.value.i64\}\)\)\) \{", fragment)
        full = re.search(r"xrt_native_array_update_place\(\(XrtPlace\)\{.frame=xrl_frame, .slot=(\d+), .nulls=XRT_PLACE_NONNULL\}, "
                         r"XRT_BACKING_ARRAY_I64, xrl_frame, (\d+), &xrl_array_update, UINT32_C\((\d+)\)\);", fragment)
        if not kernel or not full or kernel[6] != match[3] or not re.search(r"if \(!\(xrl_array_ready && xrt_array_i64_try_update_local", fragment):
            raise ValueError("local fragment lost its ready short-circuit or original full native call")
        function, binding, offset, workspace, workspace_offset = map(int, kernel.groups()[:5])
        if (binding, workspace) != tuple(map(int, full.groups()[:2])) or binding == workspace or offset == workspace_offset:
            raise ValueError("local fragment changed receiver/workspace identities")
        for expression in (match[1], match[2]):
            if expression != "0" and (not re.fullmatch(r"[vp]\d+", expression) or len(re.findall(r"\b"+expression+r"\b", fragment)) != 1):
                raise ValueError("index/item is not materialized exactly once from its scalar SSA")
        if match[1] == match[2] or not re.fullmatch(r"[vp]\d+", match[2]):
            raise ValueError("observer needs separate actual scalar operands")
        if fragment.count("xrt_native_array_update_place(") != 1 or fragment.count("xrl_array_ready = true;") != 1:
            raise ValueError("only complete original update return may establish readiness")
        owners = re.search(rf"xrl_owners_{function}\[\] = \{{(.*?)\}};", text, re.DOTALL)
        if not owners:
            raise ValueError("missing actual immutable Frame plan")
        fields = []
        for raw in (part.strip() for part in owners[1].split(",") if part.strip()):
            atom = re.fullmatch(r"UINT64_C\((\d+)\)", raw)
            if raw != "SIZE_MAX" and not atom:
                raise ValueError("invalid Frame offset representation")
            fields.append(int(atom[1]) if atom else None)
        if fields[binding] != offset or fields[workspace] != workspace_offset:
            raise ValueError("candidate does not match actual Frame owner fields")
        selected.setdefault(match[3], (fragment, function, (binding, offset, workspace, workspace_offset),
                                      int(full[3]), match[1], match[2]))
    if len(selected) != 2:
        raise ValueError("real generated input must contain both local SET and PUSH")
    direct_calls = 0
    for function in re.finditer(r"^static [^\n]*\bxrl_f\d+\([^\n]*\) \{\n(.*?)^\}", text, re.MULTILINE | re.DOTALL):
        body = function[1]
        if "bool xrl_array_ready = false;" not in body:
            continue
        for call in re.finditer(r"\bxrl_f\d+\(", body):
            assignments = re.findall(r"^    xrl_array_ready = (true|false);", body[:call.start()], re.MULTILINE)
            if not assignments or assignments[-1] != "false":
                raise ValueError("ordinary call must clear readiness before either return path")
            direct_calls += 1
    if not direct_calls:
        raise ValueError("actual input did not exercise ordinary-call invalidation")
    code = r'''#include "runtime/native/xnative_array_ops.h"
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_values.h"
#include "xray_native_array_local.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr,"Array local observer line %d: %s\n",__LINE__,#c); abort(); } } while (0)
'''
    code += f'#include "{generated.name}"\n'
    rows = [selected[kind] for kind in ("XRT_ARRAY_UPDATE_SET", "XRT_ARRAY_UPDATE_PUSH")]
    code += "static const size_t geometry[2][4]={" + ",".join("{"+",".join(map(str, row[2]))+"}" for row in rows) + "};\n"
    code += "static const XrtManagedFrameSpec *const plans[2]={" + ",".join(f"&xrl_frame_{row[1]}" for row in rows) + "};\n"
    code += "static const uint32_t sources[2]={" + ",".join(str(row[3]) for row in rows) + "};\n"
    code += r'''
enum LocalMode { READY, COLD, COLD_SETUP_OOM, SNAPSHOT, SNAPSHOT_OOM, BOUNDARY, BOUNDARY_OOM,
    SOURCE_PENDING, WORKSPACE_PENDING, WORKSPACE_OLD, EMPTY_SOURCE, SHARED_ONE, LOCAL_MODE_COUNT };
typedef struct LocalObservation {
    XrtValueRoot values;
    XrtBillingOwner *billing;
    XrtManagedFrameLocations view;
    const XrtBackingTypes *external;
    XrtManagedHeader *snapshot;
    size_t operation, kernels, full_calls;
    enum LocalMode mode;
    int64_t index, item, after_value;
    size_t after_length;
    bool fast, completed, ready_after;
} LocalObservation;
static LocalObservation observation;
static XrtManagedHeader *binding_owner(void) {
    return *(XrtManagedHeader **)((unsigned char *)observation.view.payload+geometry[observation.operation][1]);
}
static int64_t element_at(const XrtArrayBacking *array, size_t index) {
    int64_t value=0; REQUIRE(index<array->length);
    memcpy(&value,array->elements+index*sizeof(value),sizeof(value)); return value;
}
static bool observed_local(XrtManagedFrame *frame, void *payload, const XrtManagedFrameLocations *view,
    const XrtManagedFrameSpec *plan, XrtArrayI64LocalUpdate update) {
    XrtArrayUpdateKind expected=observation.operation?XRT_ARRAY_UPDATE_PUSH:XRT_ARRAY_UPDATE_SET;
    REQUIRE(frame==observation.view.frame && payload==observation.view.payload && view->frame==frame && view->payload==payload &&
        plan==plans[observation.operation] && update.binding_slot==geometry[observation.operation][0] &&
        update.binding_offset==geometry[observation.operation][1] && update.workspace_slot==geometry[observation.operation][2] &&
        update.workspace_offset==geometry[observation.operation][3] && update.operation==expected &&
        update.index==(observation.operation?0:observation.index) && update.value==observation.item);
    XrtBillingStats before=xrt_billing_owner_stats(observation.billing);
    observation.fast=xrt_array_i64_try_update_local(frame,payload,view,plan,update); ++observation.kernels;
    XrtBillingStats after=xrt_billing_owner_stats(observation.billing);
    REQUIRE(before.bytes==after.bytes && before.blocks==after.blocks && before.allocations==after.allocations);
    return observation.fast;
}
static void observed_full(XrtPlace binding, XrtBackingArrayKind kind, XrtManagedFrame *frame,
    size_t workspace, const XrtArrayUpdate *update, uint32_t source) {
    XrtArrayUpdateKind expected=observation.operation?XRT_ARRAY_UPDATE_PUSH:XRT_ARRAY_UPDATE_SET;
    REQUIRE(binding.frame==observation.view.frame && !binding.external && binding.slot==geometry[observation.operation][0] &&
        frame==observation.view.frame && workspace==geometry[observation.operation][2] && kind==XRT_BACKING_ARRAY_I64 &&
        source==sources[observation.operation] && update->operation==expected &&
        update->index==(observation.operation?0:observation.index) && update->item.kind==kind && update->item.value.i64==observation.item);
    ++observation.full_calls;
    xrt_native_array_update_place(binding,kind,frame,workspace,update,source);
}
'''
    for number, row in enumerate(rows):
        fragment, _, _, _, index, item = row
        code += f'''static void emitted_update_{number}(XrtManagedFrame *xrl_frame, void *xrl_payload,
    const XrtManagedFrameLocations *factory, bool ready) {{
    XrtManagedFrameLocations xrl_frame_locations=*factory;
    bool xrl_array_ready=ready;
'''
        if index != "0":
            code += f"    const int64_t {index}=observation.index;\n"
        code += f"    const int64_t {item}=observation.item;\n"
        code += fragment.replace("xrt_array_i64_try_update_local(", "observed_local(").replace("xrt_native_array_update_place(", "observed_full(")
        code += "\n    observation.ready_after=xrl_array_ready;\n}\n"
    code += r'''
static void execute_update(void *unused) {
    (void)unused;
    bool ready=observation.mode!=COLD && observation.mode!=COLD_SETUP_OOM;
    if (ready) { const XrtBackingTypes *types=NULL;
        REQUIRE(xrt_native_values_backings(&observation.values,&types)==XRT_MANAGED_OK && types); }
    XrtManagedRoot *root=xrt_native_managed_root(sources[observation.operation]);
    REQUIRE(xrt_managed_frame_enter_locations(root,plans[observation.operation],&observation.view)==XRT_MANAGED_OK);
    const size_t *g=geometry[observation.operation];
    bool boundary=observation.mode==BOUNDARY || observation.mode==BOUNDARY_OOM;
    size_t count=boundary?4:2; int64_t values[]={11,13,17,19};
    /* Real independent Ops can supply a live owned argument while the current
     * execution's backing directory is still cold. No Header or owner is faked. */
    if (observation.mode!=EMPTY_SOURCE)
        REQUIRE(xrt_array_backing_i64_create(observation.view.frame,g[0],observation.external,values,count,4)==XRT_MANAGED_OK);
    if (observation.mode==SNAPSHOT || observation.mode==SNAPSHOT_OOM) {
        observation.snapshot=binding_owner(); REQUIRE(xrt_managed_retain(observation.snapshot));
    }
    if (observation.mode==SHARED_ONE)
        atomic_store_explicit(&binding_owner()->references,-1,memory_order_relaxed); /* explicit restored sign fault */
    if (observation.mode==SOURCE_PENDING || observation.mode==WORKSPACE_PENDING) {
        size_t slot=observation.mode==SOURCE_PENDING?g[0]:g[2]; XrtManagedHeader *pending=NULL;
        REQUIRE(xrt_managed_slot_prepare(observation.view.frame,slot,observation.external->arrays[XRT_BACKING_ARRAY_I64],
            sizeof(XrtArrayBacking)+sizeof(int64_t),&pending)==XRT_MANAGED_OK);
        ((XrtArrayBacking *)pending)->length=1;
    }
    if (observation.mode==WORKSPACE_OLD) {
        int64_t old=23;
        REQUIRE(xrt_array_backing_i64_create(observation.view.frame,g[2],observation.external,&old,1,1)==XRT_MANAGED_OK);
    }
    observation.index=boundary?4:0; observation.item=31;
    if (observation.mode==COLD_SETUP_OOM || observation.mode==SNAPSHOT_OOM || observation.mode==BOUNDARY_OOM)
        xrt_billing_owner_fail_after(observation.billing,0);
    if (!observation.operation) emitted_update_0(observation.view.frame,observation.view.payload,&observation.view,ready);
    else emitted_update_1(observation.view.frame,observation.view.payload,&observation.view,ready);
    XrtArrayBacking *array=(XrtArrayBacking *)binding_owner(); REQUIRE(array);
    observation.after_length=array->length;
    observation.after_value=element_at(array,observation.operation?count:(size_t)observation.index);
    REQUIRE(observation.after_value==31 && array->length==count+(observation.operation?1u:0u) &&
        atomic_load_explicit(&array->managed.references,memory_order_relaxed)==1 && observation.ready_after);
    REQUIRE(observation.fast==(observation.mode==READY)); observation.completed=true;
}
int main(void) {
    XrtLocationOwner *locations=NULL;
    REQUIRE(xrt_locations_create(xrt_standalone_source_map.locations,xrt_standalone_source_map.count,&locations)==XRT_LOCATION_OK);
    XrtBillingOwner *control=NULL,*external=NULL;
    REQUIRE(xrt_billing_owner_new(NULL,SIZE_MAX,&control)==XRT_MANAGED_OK);
    REQUIRE(xrt_billing_owner_new(NULL,SIZE_MAX,&external)==XRT_MANAGED_OK);
    XrtBackingTypes types={0}; REQUIRE(xrt_backing_types_init(external,&types)==XRT_MANAGED_OK);
    XrtBillingStats empty=xrt_billing_owner_stats(control), external_baseline=xrt_billing_owner_stats(external);
    for (size_t operation=0;operation<2;++operation) for (unsigned mode=0;mode<LOCAL_MODE_COUNT;++mode) {
        memset(&observation,0,sizeof(observation)); observation.operation=operation;
        observation.mode=(enum LocalMode)mode; observation.external=&types;
        REQUIRE(xrt_native_values_prepare(&observation.values,&observation.billing)==XRT_MANAGED_OK &&
            xrt_billing_owner_retain(observation.billing));
        XrtExecutionFailure failure={0};
        XrtExecutionStatus result=xrt_run_embedded_owned(locations,&failure,execute_update,NULL,&observation.values);
        REQUIRE(xrt_value_root_empty(&observation.values));
        if (observation.snapshot) {
            const XrtArrayBacking *saved=(const XrtArrayBacking *)observation.snapshot;
            REQUIRE(saved->length==2 && element_at(saved,0)==11 && element_at(saved,1)==13 &&
                atomic_load_explicit(&saved->managed.references,memory_order_relaxed)==1);
            xrt_managed_drop(observation.snapshot); observation.snapshot=NULL;
        }
        bool fails=mode==COLD_SETUP_OOM || mode==SNAPSHOT_OOM || mode==BOUNDARY_OOM ||
            (!operation && mode==BOUNDARY) || mode==SOURCE_PENDING || mode==WORKSPACE_PENDING || mode==WORKSPACE_OLD || mode==EMPTY_SOURCE;
        if (fails) {
            XrXirValueStatus expected=(mode==COLD_SETUP_OOM || mode==SNAPSHOT_OOM || (operation && mode==BOUNDARY_OOM))?
                XR_XIR_VALUE_OOM:!operation && (mode==BOUNDARY || mode==BOUNDARY_OOM)?XR_XIR_VALUE_BOUNDS:XR_XIR_VALUE_BAD_ARGUMENT;
            REQUIRE(result==XRT_EXECUTION_FAILURE && !observation.completed && failure.kind==XRT_FAILURE_VALUE &&
                failure.detail.value.status==expected && failure.detail.value.source_id==sources[operation]);
        } else REQUIRE(result==XRT_EXECUTION_COMPLETED && observation.completed);
        REQUIRE(observation.kernels==(mode==COLD || mode==COLD_SETUP_OOM?0u:1u) &&
            observation.full_calls==(mode==READY?0u:1u));
        XrtBillingStats final=xrt_billing_owner_stats(observation.billing), guest=xrt_billing_owner_stats(external);
        REQUIRE(final.bytes==empty.bytes && final.blocks==empty.blocks &&
            guest.bytes==external_baseline.bytes && guest.blocks==external_baseline.blocks);
        printf("{\"operation\":%zu,\"mode\":%u,\"fast\":%u,\"kernels\":%zu,\"fullCalls\":%zu,\"completed\":%u,\"bytes\":%zu,\"blocks\":%zu,\"controlBytes\":%zu,\"controlBlocks\":%zu}\n",
            operation,mode,(unsigned)observation.fast,observation.kernels,observation.full_calls,(unsigned)observation.completed,
            final.bytes,final.blocks,empty.bytes,empty.blocks);
        REQUIRE(fflush(stdout)==0); xrt_billing_owner_drop(observation.billing);
    }
    xrt_backing_types_drop(&types); xrt_billing_owner_drop(external);
    xrt_billing_owner_drop(control); xrt_locations_release(locations); return 0;
}
'''
    return code
