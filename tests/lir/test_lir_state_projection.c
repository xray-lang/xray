/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_state_projection.c - One raw physical State spec from admitted LIR
 */
#include "lir/xlir_state_projection.h"
#include "lir/xlir_value_image.h"
#include "lir/xlir_internal.h"
#include "runtime/native/xnative_module_state.h"
#include "xray_native_values.h"
#include "xray_native_function.h"
#include "base/xmalloc.h"
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); abort(); } } while (0)
#define LIR(c) CHECK((c) == XR_LIR_OK)
#define LAYOUT(c) CHECK((c) == XR_LIR_VALUE_LAYOUT_OK)
#define MANAGED(c) CHECK((c) == XRT_MANAGED_OK)
XRAY_API_NORET void xrt_native_managed_failure(XrtManagedStatus status, uint32_t source) {
    fprintf(stderr,"unexpected execution failure %d at %u\n",(int)status,source); abort();
}
typedef struct Record { uint8_t flag; XrtManagedHeader *text; } Record;
typedef struct MaybeRecord { uint8_t tag; Record payload; } MaybeRecord;
typedef struct MaybeString { uint8_t tag; XrtManagedHeader *payload; } MaybeString;
typedef struct Empty { uint8_t storage; } Empty;
typedef struct MaybeEmpty { uint8_t tag; Empty payload; } MaybeEmpty;
typedef struct StatePayload {
    Record record;
    MaybeRecord maybe;
    XrtManagedHeader *arrays[4], *map;
    MaybeString string;
    Empty empty;
    MaybeEmpty optional_empty;
    uint8_t fixed[3];
    int64_t tuple[2];
} StatePayload;
enum { SLOTS = 12 };
typedef struct Fixture { XrLirModule *module; XrLirValueLayoutKey roots[SLOTS]; } Fixture;
static void define_entry(XrLirModule *module) {
    XrLirType parameter = XR_LIR_STATE;
    XrLirFunction function; XrLirBlock block;
    XrLirFunctionSpec spec = {.name="root",.name_length=4,.parameters=&parameter,.parameter_count=1,.result=XR_LIR_UNIT};
    LIR(xr_lir_declare_function(module,&spec,&function)); LIR(xr_lir_set_state_entry(module,function));
    LIR(xr_lir_define_function(module,function)); LIR(xr_lir_add_block(module,function,NULL,0,&block));
    LIR(xr_lir_set_entry(module,function,block));
    LIR(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT}));
}
static Fixture fixture_new(bool unsupported) {
    Fixture f = {0}; LIR(xr_lir_module_create(NULL,&f.module));
    XrLirType record, maybe, string, empty, optional_empty, fixed, tuple;
    XrLirRecordField fields[] = {{"flag",4,XR_LIR_I1, 0, 0},{"text",4,XR_LIR_STRING, 0, 0}};
    LIR(xr_lir_record_type(f.module,
        &(XrLirRecordIdentity){.module="tests.lir.state_projection", .module_length=sizeof("tests.lir.state_projection")-1,
            .name="StateRecord", .name_length=sizeof("StateRecord")-1},
        fields,2,&record)); LIR(xr_lir_nullable_type(f.module,record,&maybe));
    LIR(xr_lir_nullable_type(f.module,XR_LIR_STRING,&string)); LIR(xr_lir_record_type(f.module,
        &(XrLirRecordIdentity){.module="tests.lir.state_projection", .module_length=sizeof("tests.lir.state_projection")-1,
            .name="EmptyRecord", .name_length=sizeof("EmptyRecord")-1},
        NULL,0,&empty));
    LIR(xr_lir_nullable_type(f.module,empty,&optional_empty));
    LIR(xr_lir_fixed_array_type(f.module,XR_LIR_U8,3,&fixed)); LIR(xr_lir_tuple_i64_type(f.module,2,&tuple));
    XrLirValueLayoutKey roots[SLOTS] = {{record,0,0},{maybe,0,0},{XR_LIR_ARRAY,XR_LIR_I64,0},
        {XR_LIR_ARRAY,XR_LIR_STRING,0},{XR_LIR_ARRAY,XR_LIR_U8,0},{XR_LIR_ARRAY,XR_LIR_U64,0},
        {XR_LIR_MAP_STRING_I64,0,0},{string,0,0},{empty,0,0},{optional_empty,0,0},{fixed,0,0},{tuple,0,0}};
    memcpy(f.roots,roots,sizeof(roots));
    XrLirStateSlot slots[SLOTS]; size_t end = 0;
    for (uint32_t i = 0; i < SLOTS; ++i) {
        uint32_t size, alignment;
        CHECK(xr_lir_internal_state_payload(f.module,roots[i].type,&size,&alignment));
        end = (end + alignment - 1) & ~(size_t)(alignment - 1);
        slots[i] = (XrLirStateSlot){.module=i % 2,.payload=roots[i].type,.mutable=i % 2 == 0,
            .offset=end,.size=size,.alignment=alignment,.array_element=roots[i].array_element};
        end += size;
    }
    if (unsupported) {
        XrLirType fn; LIR(xr_lir_callable_type(f.module,&(XrLirCallableType){.result=XR_LIR_UNIT},&fn));
        slots[0].payload = fn;
    }
    XrLirStateModule modules[] = {{0,0},{0,1}}; uint32_t dependency = 0;
    XrLirStateLayout layout = {modules,2,&dependency,1,slots,SLOTS,(end + 7) & ~(size_t)7,8};
    LIR(xr_lir_set_state_layout(f.module,&layout)); define_entry(f.module);
    LIR(xr_lir_module_finish(f.module,NULL)); return f;
}
static void physical_projection(void) {
    Fixture f = fixture_new(false); XrtBillingOwner *billing = NULL;
    MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
    XrLirValueLayouts *set = NULL; LAYOUT(xr_lir_value_layouts_create(f.module,f.roots,SLOTS,billing,&set));
    XrLirValueImage *image = NULL; LAYOUT(xr_lir_value_image_create(set,billing,&image));
    const XrtValueImage *raw = xr_lir_value_image_view(image);
    for (size_t i = 0; i < xr_lir_value_layouts_count(set); ++i) {
        XrLirValueLayoutKey key; const XrtValuePlan *plan = NULL, *lookup = NULL; uint32_t id = 0;
        LAYOUT(xr_lir_value_layouts_at(set,i,&key,&plan)); LAYOUT(xr_lir_value_layouts_lookup(set,key,&lookup,&id));
        CHECK(plan == lookup && id == i + 1);
    }
    XrLirStateProjection *projection = NULL;
    XrtBillingStats before = xrt_billing_owner_stats(billing);
    xrt_billing_owner_fail_after(billing,0);
    CHECK(xr_lir_state_projection_create(f.module,set,raw,billing,&projection) == XR_LIR_VALUE_LAYOUT_OOM && !projection);
    XrtBillingStats after = xrt_billing_owner_stats(billing); CHECK(before.bytes == after.bytes && before.blocks == after.blocks);
    xrt_billing_owner_fail_after(billing,SIZE_MAX);
    LAYOUT(xr_lir_state_projection_create(f.module,set,raw,billing,&projection));
    const XrtModuleLayoutSpec *spec = xr_lir_state_projection_view(projection);
    static const size_t offsets[SLOTS] = {offsetof(StatePayload,record),offsetof(StatePayload,maybe),
        offsetof(StatePayload,arrays[0]),offsetof(StatePayload,arrays[1]),offsetof(StatePayload,arrays[2]),
        offsetof(StatePayload,arrays[3]),offsetof(StatePayload,map),offsetof(StatePayload,string),
        offsetof(StatePayload,empty),offsetof(StatePayload,optional_empty),offsetof(StatePayload,fixed),offsetof(StatePayload,tuple)};
    CHECK(spec->values == raw && spec->slot_count == SLOTS && spec->payload_size == sizeof(StatePayload) &&
        spec->payload_alignment == alignof(StatePayload) && spec->dependencies[0] == 0);
    for (size_t i = 0; i < SLOTS; ++i) {
        const XrtValuePlan *plan = NULL; uint32_t id = 0;
        LAYOUT(xr_lir_value_layouts_lookup(set,f.roots[i],&plan,&id));
        CHECK(spec->slots[i].type == id && spec->slots[i].offset == offsets[i] && spec->slots[i].module == i % 2 &&
            spec->slots[i].flags == (i % 2 ? 0u : XRT_MODULE_SLOT_MUTABLE));
    }
    CHECK(spec->slots[0].size == sizeof(Record) && spec->slots[1].size == sizeof(MaybeRecord));
    uint32_t record_id = spec->slots[0].type;
    xr_lir_module_destroy(f.module); xr_lir_value_layouts_destroy(set);
    XrtModuleLayout *layout = NULL; MANAGED(xrt_module_layout_new(billing,spec,(XrtManagedLease){0},&layout));
    xr_lir_state_projection_destroy(projection); xr_lir_value_image_destroy(image);
    const XrtValuePlan *record = NULL; MANAGED(xrt_module_layout_plan(layout,record_id,&record));
    XrtValuePlanSpec view; MANAGED(xrt_value_plan_view(record,&view));
    CHECK(view.field_count == 2 && view.fields[1].offset == offsetof(Record,text) &&
        view.fields[1].name_length == 4 && !memcmp(view.fields[1].name,"text",4));
    XrtModuleState *state = NULL; MANAGED(xrt_module_state_new(layout,billing,&state));
    CHECK(xrt_module_begin(state,1) == XRT_MANAGED_ARGUMENT); MANAGED(xrt_module_begin(state,0));
    xrt_module_state_drop(&state); xrt_module_layout_drop(&layout);
    CHECK(xrt_billing_owner_stats(billing).blocks == 1); xrt_billing_owner_drop(billing);
    puts("one State projection matches native C layout and every image/home type id after source retirement");
}
static void unsupported_and_absent(void) {
    Fixture f = fixture_new(true); XrtBillingOwner *billing = NULL;
    MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
    XrLirValueLayouts *set = NULL; LAYOUT(xr_lir_value_layouts_create(f.module,f.roots,SLOTS,billing,&set));
    XrLirValueImage *image = NULL; LAYOUT(xr_lir_value_image_create(set,billing,&image));
    XrLirStateProjection *projection = NULL; XrtBillingStats before = xrt_billing_owner_stats(billing);
    CHECK(xr_lir_state_projection_create(f.module,set,xr_lir_value_image_view(image),billing,&projection) ==
        XR_LIR_VALUE_LAYOUT_ARGUMENT && !projection);
    XrtBillingStats after = xrt_billing_owner_stats(billing); CHECK(before.bytes == after.bytes && before.blocks == after.blocks);
    xr_lir_value_image_destroy(image); xr_lir_value_layouts_destroy(set); image=NULL; set=NULL;
    XrLirStateLayout logical; LIR(xr_lir_state_layout_view(f.module,&logical));
    f.roots[0]=(XrLirValueLayoutKey){logical.slots[0].payload,0,0};
    LAYOUT(xr_lir_value_layouts_create(f.module,f.roots,SLOTS,billing,&set));
    LAYOUT(xr_lir_value_image_create(set,billing,&image));
    LAYOUT(xr_lir_state_projection_create(f.module,set,xr_lir_value_image_view(image),billing,&projection));
    const XrtModuleLayoutSpec *spec=xr_lir_state_projection_view(projection);
    CHECK(spec->slots[0].size==sizeof(XrtFunction) && spec->slots[0].alignment==alignof(XrtFunction) &&
        spec->values->types[spec->slots[0].type-1].kind==XRT_VALUE_FUNCTION);
    XrtModuleLayout *callable_layout=NULL; before=xrt_billing_owner_stats(billing);
    MANAGED(xrt_module_layout_new(billing,spec,(XrtManagedLease){0},&callable_layout));
    const XrtValuePlan *callable_plan=NULL;
    MANAGED(xrt_module_layout_plan(callable_layout,spec->slots[0].type,&callable_plan));
    XrtValuePlanSpec callable_view;
    MANAGED(xrt_value_plan_view(callable_plan,&callable_view));
    CHECK(callable_view.kind==XRT_VALUE_FUNCTION && callable_view.size==sizeof(XrtFunction));
    xrt_module_layout_drop(&callable_layout); CHECK(!callable_layout);
    after=xrt_billing_owner_stats(billing); CHECK(before.bytes==after.bytes && before.blocks==after.blocks);
    xr_lir_state_projection_destroy(projection); projection=NULL;
    xr_lir_value_image_destroy(image); xr_lir_value_layouts_destroy(set); xr_lir_module_destroy(f.module);
    XrLirModule *empty = NULL; LIR(xr_lir_module_create(NULL,&empty)); LIR(xr_lir_module_finish(empty,NULL));
    xrt_billing_owner_close(billing);
    LAYOUT(xr_lir_state_projection_create(empty,NULL,NULL,billing,&projection)); CHECK(!projection);
    CHECK(!xr_lir_state_projection_view(NULL)); xr_lir_module_destroy(empty);
    CHECK(xrt_billing_owner_stats(billing).blocks == 1); xrt_billing_owner_drop(billing);
}
int main(void) { physical_projection(); unsupported_and_absent(); puts("shared State projection passed"); return 0; }
