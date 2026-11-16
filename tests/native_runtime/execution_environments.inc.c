/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * execution_environments.inc.c - Function borrows outlive cold execution setup
 */
typedef struct EnvironmentEntry { int64_t (*invoke)(uintptr_t); } EnvironmentEntry;
typedef struct EnvironmentCode {
    size_t references;
    size_t retain_calls, fail_retain_at;
    unsigned *retired;
    XrtValueImage image;
    EnvironmentEntry captured, empty;
} EnvironmentCode;
typedef struct EnvironmentCall {
    XrtValueRoot *owner;
    EnvironmentCode *code;
    XrtModuleState *state;
    XrtFunction escaped[2];
    unsigned after, invalid;
} EnvironmentCall;
static int64_t environment_captured(uintptr_t context) {
    int64_t value = 0; memcpy(&value,(const void *)context,sizeof(value)); return value + 1;
}
static int64_t environment_empty(uintptr_t context) { CHECK(context); return 17; }
static bool environment_code_retain(void *context) {
    EnvironmentCode *code = context;
    if (++code->retain_calls == code->fail_retain_at) return false;
    CHECK(code->references && code->references < SIZE_MAX); ++code->references; return true;
}
static void environment_code_drop(void *context) {
    EnvironmentCode *code = context; CHECK(code->references);
    if (!--code->references) { ++*code->retired; xr_free(code); }
}
static XrtManagedLease environment_code_lease(EnvironmentCode *code) {
    return (XrtManagedLease){code,environment_code_retain,environment_code_drop};
}
static EnvironmentCode *environment_code_new(unsigned *retired) {
    static const size_t owner = offsetof(XrtFunction,owner);
    static const XrtValueImageSlot capture = {.type=1,.offset=0};
    static const XrtValueImageType rows[] = {
        {.kind=XRT_VALUE_I64,.size=8,.alignment=8},
        {.kind=XRT_VALUE_FUNCTION,.size=sizeof(XrtFunction),.alignment=_Alignof(XrtFunction),.owned_count=1,.result=1},
        {.kind=XRT_VALUE_ENVIRONMENT,.size=8,.alignment=8,.capture_count=1,.entry_ordinal=7,.entry_signature=2},
        {.kind=XRT_VALUE_ENVIRONMENT,.alignment=1,.entry_ordinal=8,.entry_signature=2}};
    EnvironmentCode *code = xr_malloc(sizeof(*code)); CHECK(code);
    *code = (EnvironmentCode){.references=1,.retired=retired,
        .image={.types=rows,.type_count=4,.slots=&capture,.slot_count=1,.owned_offsets=&owner,.owned_count=1},
        .captured={environment_captured},.empty={environment_empty}};
    return code;
}
static int64_t environment_invoke(XrtFunction value) {
    const EnvironmentEntry *entry = value.entry;
    CHECK(entry && value.environment && value.owner); return entry->invoke(value.environment);
}
static void environment_root(void *context) {
    EnvironmentCall *call = context;
    XrtManagedRoot *root = xrt_native_managed_root(1); XrtManagedFrame *frame = NULL;
    managed_require(xrt_managed_frame_enter(root,3,&frame));
    int64_t seed = 41;
    XrtNativeEnvironmentInput input = {.image=call->state ? NULL : &call->code->image,.state=call->state,
        .code=call->state ? (XrtManagedLease){0} : environment_code_lease(call->code),
        .entry=&call->code->captured,.captures=&seed,
        .type=call->invalid ? 2u : 3u};
    size_t state_code_references = call->code->references;
    XrtFunction first = xrt_native_environment_create(frame,0,&input,1);
    if (call->state) CHECK(call->code->references == state_code_references);
    CHECK(environment_invoke(first) == 42);
    input.type=4; input.entry=&call->code->empty; input.captures=NULL;
    XrtFunction empty = xrt_native_environment_create(frame,1,&input,1);
    CHECK(environment_invoke(empty) == 17 && environment_invoke(first) == 42);
    XrtBillingOwner *billing = NULL; managed_require(xrt_native_values_prepare(call->owner,&billing));
    XrtBillingStats before = xrt_billing_owner_stats(billing);
    size_t code_references = call->code->references;
    seed=19; input.type=3; input.entry=&call->code->captured; input.captures=&seed;
    XrtFunction next = xrt_native_environment_create(frame,2,&input,1);
    XrtBillingStats after = xrt_billing_owner_stats(billing);
    CHECK(environment_invoke(next) == 20 && environment_invoke(first) == 42 &&
        after.blocks == before.blocks+1 && call->code->references == code_references);
    managed_require(xrt_managed_slot_take(frame,0,&call->escaped[0].owner));
    call->escaped[0].entry=first.entry; call->escaped[0].environment=first.environment;
    managed_require(xrt_managed_slot_take(frame,1,&call->escaped[1].owner));
    call->escaped[1].entry=empty.entry; call->escaped[1].environment=empty.environment;
    managed_require(xrt_managed_frame_leave(root,frame)); ++call->after;
}
static void environment_escaped_drop(EnvironmentCall *call, unsigned *retired) {
    environment_code_drop(call->code);
    CHECK(!*retired && environment_invoke(call->escaped[0]) == 42 && environment_invoke(call->escaped[1]) == 17);
    xrt_managed_drop(call->escaped[0].owner); CHECK(!*retired);
    xrt_managed_drop(call->escaped[1].owner); CHECK(*retired == 1);
}
static void managed_environments(void) {
    XrtLocationOwner *map = locations("environment-execution.xr");
    for (unsigned stateful=0; stateful<2; ++stateful) {
        unsigned retired=0; EnvironmentCode *code=environment_code_new(&retired);
        XrtBillingOwner *state_billing=NULL; XrtModuleLayout *layout=NULL; XrtModuleState *state=NULL;
        if (stateful) {
            XrtModuleSpec module={0};
            XrtModuleLayoutSpec spec={.values=&code->image,.modules=&module,.module_count=1,.payload_alignment=1};
            managed_require(xrt_billing_owner_new(NULL,SIZE_MAX,&state_billing));
            managed_require(xrt_module_layout_new(state_billing,&spec,environment_code_lease(code),&layout));
            managed_require(xrt_module_state_new(layout,state_billing,&state)); xrt_module_layout_drop(&layout);
        }
        XrtValueRoot owner={0}; XrtExecutionFailure failure={0};
        EnvironmentCall call={.owner=&owner,.code=code,.state=state};
        CHECK(xrt_run_embedded_owned(map,&failure,environment_root,&call,&owner) == XRT_EXECUTION_COMPLETED && call.after == 1);
        CHECK(xrt_value_root_empty(&owner) && !xrt_value_root_active());
        xrt_module_state_drop(&state); xrt_billing_owner_drop(state_billing);
        environment_escaped_drop(&call,&retired);
    }
    unsigned retired=0; EnvironmentCode *code=environment_code_new(&retired);
    XrtValueRoot owner={0}; XrtExecutionFailure failure={0};
    EnvironmentCall invalid={.owner=&owner,.code=code,.invalid=1};
    CHECK(xrt_run_embedded_owned(map,&failure,environment_root,&invalid,&owner) == XRT_EXECUTION_FAILURE &&
        !invalid.after && failure.kind == XRT_FAILURE_VALUE && xrt_value_root_empty(&owner));
    environment_code_drop(code); CHECK(retired == 1);
#if defined(XR_NATIVE_VALUE_ALLOCATIONS)
    size_t allowance=0;
    for (; allowance<128; ++allowance) {
        retired=0; code=environment_code_new(&retired);
        owner=(XrtValueRoot){0}; XrtBillingOwner *billing=NULL;
        managed_require(xrt_native_values_prepare(&owner,&billing)); CHECK(xrt_billing_owner_retain(billing));
        xrt_billing_owner_fail_after(billing,allowance);
        EnvironmentCall call={.owner=&owner,.code=code};
        XrtExecutionStatus status=xrt_run_embedded_owned(map,&failure,environment_root,&call,&owner);
        CHECK(xrt_value_root_empty(&owner));
        if (status == XRT_EXECUTION_COMPLETED) {
            CHECK(call.after == 1); environment_escaped_drop(&call,&retired);
        } else {
            CHECK(status == XRT_EXECUTION_FAILURE && !call.after && !call.escaped[0].owner && !call.escaped[1].owner &&
                failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_OOM);
            environment_code_drop(code); CHECK(retired == 1);
        }
        CHECK(xrt_billing_owner_stats(billing).blocks == 1); xrt_billing_owner_drop(billing);
        if (status == XRT_EXECUTION_COMPLETED) break;
    }
    CHECK(allowance && allowance<128);
    printf("execution Env allocation rollback sites: %zu\n",allowance);
#endif
    xrt_locations_release(map);
    puts("execution Env setup reuses State plans and preserves Function borrows after execution/code retirement");
}

typedef struct CellCall {
    XrtValueRoot *owner;
    EnvironmentCode *code;
    XrtModuleState *state;
    XrtManagedHeader *escaped[2];
    unsigned after;
    bool invalid;
} CellCall;
static void cell_factory_root(void *context) {
    CellCall *call = context;
    XrtManagedRoot *root = xrt_native_managed_root(1); XrtManagedFrame *frame = NULL;
    managed_require(xrt_managed_frame_enter(root,8,&frame));
    XrtNativeCellInput input = {.image=call->state ? NULL : &call->code->image,.state=call->state,
        .code=call->state ? (XrtManagedLease){0} : environment_code_lease(call->code),
        .type=call->invalid ? 1u : 3u,.initial.i64=41};
    size_t references = call->code->references;
    xrt_native_cell_create(frame,0,&input,1);
    CHECK(call->code->references == references+(call->state ? 0u : 1u));
    references=call->code->references;
    XrtManagedHeader *integer = NULL; managed_require(xrt_managed_slot_borrow(frame,0,&integer));
    CHECK(xrt_native_cell_i64_read(integer,1) == 41); xrt_native_cell_i64_write(integer,42,1);
    XrtBillingOwner *billing = NULL; managed_require(xrt_native_values_prepare(call->owner,&billing));
    XrtBillingStats before = xrt_billing_owner_stats(billing);
    xrt_native_cell_create(frame,7,&input,1);
    CHECK(xrt_billing_owner_stats(billing).blocks == before.blocks+1 && call->code->references == references);
    managed_require(xrt_managed_slot_drop(frame,7));
    xrt_native_map_create(frame,1,&(XrtMapInput){0},1);
    XrtManagedHeader *empty = NULL; managed_require(xrt_managed_slot_borrow(frame,1,&empty));
    input.type=4; input.initial.map=empty; xrt_native_cell_create(frame,3,&input,1);
    XrtManagedHeader *cell = NULL; managed_require(xrt_managed_slot_borrow(frame,3,&cell));
    xrt_native_cell_map_read_copy(frame,4,cell,1);
    xrt_native_string_create(frame,2,"key",3,1);
    XrtManagedHeader *key = NULL; managed_require(xrt_managed_slot_borrow(frame,2,&key));
    XrtMapUpdate update = {XRT_MAP_UPDATE_SET,(const XrtStringBacking *)key,42}; bool removed = true;
    xrt_native_cell_map_update(cell,frame,5,&update,&removed,1); CHECK(!removed);
    XrtExternalOwner binding = xrt_native_cell_map_binding(cell,1);
    const XrtManagedHeader *current = NULL;
    managed_require(xrt_place_borrow((XrtPlace){.external=&binding,.nulls=XRT_PLACE_NONNULL},&current));
    CHECK(binding.cell_domain == cell && !binding.busy && xrt_native_map_index(current,(const XrtStringBacking *)key,1) == 42);
    xrt_native_cell_map_write(cell,empty,1); xrt_native_cell_map_update(cell,frame,5,&update,&removed,1);
    xrt_native_cell_map_read_copy(frame,6,cell,1);
    XrtManagedHeader *snapshot = NULL; managed_require(xrt_managed_slot_borrow(frame,4,&snapshot));
    CHECK(!((const XrtMapBacking *)snapshot)->length && xrt_native_cell_i64_read(integer,1) == 42);
    managed_require(xrt_managed_slot_take(frame,0,&call->escaped[0]));
    managed_require(xrt_managed_slot_take(frame,3,&call->escaped[1]));
    managed_require(xrt_managed_frame_leave(root,frame)); ++call->after;
}
static void cell_receiver_root(void *context) {
    CellCall *call = context;
    XrtManagedRoot *root = xrt_native_managed_root(1); XrtManagedFrame *frame = NULL;
    managed_require(xrt_managed_frame_enter(root,4,&frame));
    CHECK(xrt_native_cell_i64_read(call->escaped[0],1) == 42);
    xrt_native_cell_i64_write(call->escaped[0],43,1);
    xrt_native_cell_map_read_copy(frame,0,call->escaped[1],1);
    XrtMapUpdate update = {.operation=XRT_MAP_UPDATE_CLEAR}; bool removed = true;
    xrt_native_cell_map_update(call->escaped[1],frame,1,&update,&removed,1); CHECK(!removed);
    XrtManagedHeader *snapshot = NULL; managed_require(xrt_managed_slot_borrow(frame,0,&snapshot));
    CHECK(((const XrtMapBacking *)snapshot)->length == 1);
    xrt_native_cell_map_read_copy(frame,2,call->escaped[1],1);
    XrtManagedHeader *current = NULL; managed_require(xrt_managed_slot_borrow(frame,2,&current));
    CHECK(!((const XrtMapBacking *)current)->length);
    managed_require(xrt_managed_frame_leave(root,frame)); ++call->after;
}
static EnvironmentCode *cell_code_new(unsigned *retired) {
    static const size_t owned = 0;
    static const XrtValueImageType rows[] = {
        {.kind=XRT_VALUE_I64,.size=8,.alignment=8},
        {.kind=XRT_VALUE_MAP_STRING_I64,.size=8,.alignment=8,.owned_count=1},
        {.kind=XRT_VALUE_CELL,.size=8,.alignment=8,.owned_count=1,.element=1},
        {.kind=XRT_VALUE_CELL,.size=8,.alignment=8,.owned_count=1,.element=2}};
    EnvironmentCode *code = environment_code_new(retired);
    code->image = (XrtValueImage){.types=rows,.type_count=4,.owned_offsets=&owned,.owned_count=1};
    return code;
}
static void managed_cells(void) {
    XrtLocationOwner *map = locations("cell-execution.xr");
    for (unsigned stateful=0;stateful<2;++stateful) {
        unsigned retired = 0; EnvironmentCode *code = cell_code_new(&retired);
        XrtBillingOwner *billing = NULL; XrtModuleLayout *layout = NULL; XrtModuleState *state = NULL;
        if (stateful) {
            XrtModuleSpec module = {0};
            XrtModuleLayoutSpec spec = {.values=&code->image,.modules=&module,.module_count=1,.payload_alignment=1};
            managed_require(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
            managed_require(xrt_module_layout_new(billing,&spec,environment_code_lease(code),&layout));
            managed_require(xrt_module_state_new(layout,billing,&state)); xrt_module_layout_drop(&layout);
        }
        XrtValueRoot owner = {0}; XrtExecutionFailure failure = {0};
        CellCall call = {.owner=&owner,.code=code,.state=state};
        CHECK(xrt_run_embedded_owned(map,&failure,cell_factory_root,&call,&owner) == XRT_EXECUTION_COMPLETED && call.after == 1);
        CHECK(xrt_value_root_empty(&owner)); xrt_module_state_drop(&state); xrt_billing_owner_drop(billing);
        environment_code_drop(code); CHECK(retired == 1); call.code=NULL; call.state=NULL;
        CHECK(xrt_run_embedded_owned(map,&failure,cell_receiver_root,&call,&owner) == XRT_EXECUTION_COMPLETED && call.after == 2);
        CHECK(xrt_value_root_empty(&owner)); xrt_managed_drop(call.escaped[0]); xrt_managed_drop(call.escaped[1]);
    }
    unsigned retired = 0; EnvironmentCode *code = cell_code_new(&retired);
    XrtValueRoot owner = {0}; XrtExecutionFailure failure = {0};
    CellCall invalid = {.owner=&owner,.code=code,.invalid=true};
    CHECK(xrt_run_embedded_owned(map,&failure,cell_factory_root,&invalid,&owner) == XRT_EXECUTION_FAILURE &&
        !invalid.after && !invalid.escaped[0] && !invalid.escaped[1] && failure.kind == XRT_FAILURE_VALUE && xrt_value_root_empty(&owner));
    environment_code_drop(code); CHECK(retired == 1);
#if defined(XR_NATIVE_VALUE_ALLOCATIONS)
    size_t allowance = 0;
    for (size_t point=1;point<=2;++point) {
        retired=0; code=cell_code_new(&retired); code->fail_retain_at=point;
        owner=(XrtValueRoot){0}; CellCall call={.owner=&owner,.code=code};
        CHECK(xrt_run_embedded_owned(map,&failure,cell_factory_root,&call,&owner) == XRT_EXECUTION_FAILURE &&
            !call.after && !call.escaped[0] && !call.escaped[1] && xrt_value_root_empty(&owner));
        CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_REFCOUNT_LIMIT &&
            code->references == 1 && code->retain_calls == point);
        environment_code_drop(code); CHECK(retired == 1);
    }
    for (;allowance<128;++allowance) {
        retired=0; code=cell_code_new(&retired); owner=(XrtValueRoot){0}; XrtBillingOwner *billing=NULL;
        managed_require(xrt_native_values_prepare(&owner,&billing)); CHECK(xrt_billing_owner_retain(billing));
        xrt_billing_owner_fail_after(billing,allowance); CellCall call={.owner=&owner,.code=code};
        XrtExecutionStatus status=xrt_run_embedded_owned(map,&failure,cell_factory_root,&call,&owner);
        CHECK(xrt_value_root_empty(&owner)); environment_code_drop(code); CHECK(retired == 1);
        if (status == XRT_EXECUTION_COMPLETED) {
            CHECK(call.after == 1); xrt_managed_drop(call.escaped[0]); xrt_managed_drop(call.escaped[1]);
        } else CHECK(status == XRT_EXECUTION_FAILURE && !call.after && !call.escaped[0] && !call.escaped[1] &&
            failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_OOM);
        CHECK(xrt_billing_owner_stats(billing).blocks == 1); xrt_billing_owner_drop(billing);
        if (status == XRT_EXECUTION_COMPLETED) break;
    }
    CHECK(allowance && allowance<128);
    printf("execution Cell allocation rollback sites: %zu\n",allowance);
#endif
    xrt_locations_release(map);
    puts("execution Cell factories reuse bound plans and survive creator and instance retirement");
}
