/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_rbc_value_homes.c - Real compiler homes and stable physical owner fields
 */
#include "lir/xlir_builder.h"
#include "lir/xlir_ownership.h"
#include "vm/register/xrbc_lir.h"
#include "vm/register/xrbc_value_homes.h"
#include "runtime/native/xnative_string_backing.h"
#include "base/xmalloc.h"
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)
#define MANAGED(c) CHECK((c) == XRT_MANAGED_OK)
static size_t home_attempts, home_failure = SIZE_MAX, home_live;
static struct { void *pointer; size_t bytes; } home_allocations[8192];
static size_t home_index(void *pointer) {
    for (size_t i = 0; i < 8192; ++i) if (home_allocations[i].pointer == pointer) return i;
    CHECK(false); return 0;
}
static void *home_allocate(size_t bytes) {
    if (++home_attempts == home_failure) return NULL;
    void *memory = xr_malloc(bytes);
    if (memory) { size_t i = home_index(NULL); home_allocations[i].pointer = memory; home_allocations[i].bytes = bytes; ++home_live; }
    return memory;
}
static void *home_callocate(size_t count, size_t bytes) {
    CHECK(!bytes || count <= SIZE_MAX / bytes);
    void *memory = home_allocate(count * bytes); if (memory) memset(memory, 0, count * bytes); return memory;
}
static void home_release(void *memory) {
    if (!memory) return;
    size_t i = home_index(memory); home_allocations[i].pointer = NULL; CHECK(home_live); --home_live; xr_free(memory);
}
static void *home_resize(void *memory, size_t bytes) {
    if (++home_attempts == home_failure) return NULL;
    void *next = xr_malloc(bytes); if (!next) return NULL;
    size_t i = home_index(memory);
    if (memory) { memcpy(next, memory, bytes < home_allocations[i].bytes ? bytes : home_allocations[i].bytes); xr_free(memory); }
    else ++home_live;
    home_allocations[i].pointer = next; home_allocations[i].bytes = bytes; return next;
}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(bytes) home_allocate(bytes)
#define xr_calloc(count, bytes) home_callocate(count, bytes)
#define xr_realloc(memory, bytes) home_resize(memory, bytes)
#define xr_free(memory) home_release(memory)
#include "lir/xlir_value_homes.c"
#include "../lir/lir_record_fixture.h"
#include "../lir/lir_array_ref_fixture.h"

static XrRbcProgram *compile(XrLirModule *module, XrLirFunction root) {
    XrRbcProgram *program = NULL; XrRbcCompileDiagnostic diagnostic = {0};
    XrRbcCompileStatus status = xr_rbc_compile(module, root, &program, &diagnostic);
    if (status != XR_RBC_COMPILE_OK) fprintf(stderr, "f%u b%u i%u: %s\n", diagnostic.function,
        diagnostic.block, diagnostic.instruction, diagnostic.message);
    CHECK(status == XR_RBC_COMPILE_OK && program);
    return program;
}
static uint64_t run(XrRbcProgram *program) {
    XrRbcExecution *execution = NULL; CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution); uint64_t result = UINT64_MAX;
    CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome, &result));
    xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome); return result;
}
static void scalar_zero_frame(void) {
    XrLirModule *module = NULL; OK(xr_lir_module_create(NULL, &module));
    XrLirFunction function; XrLirFunctionSpec spec = {.name="scalar", .name_length=6, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(module, &spec, &function)); OK(xr_lir_define_function(module, function));
    XrLirBlock block; OK(xr_lir_add_block(module, function, NULL, 0, &block)); OK(xr_lir_set_entry(module, function, block));
    XrLirValue result = record_fixture_value(module, block, XR_LIR_CONST, XR_LIR_I64, NULL, 0, 42, (XrLirFunction){0});
    XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=&result, .operand_count=1};
    OK(xr_lir_set_terminator(module, block, &ret)); OK(xr_lir_module_finish(module, NULL));
    XrRbcProgram *program = compile(module, function); xr_lir_module_destroy(module);
    CHECK(!xr_rbc_module_value_homes(xr_rbc_program_view(program), 0) && run(program) == 42);
    XrtManagedFrame *frame = NULL;
    MANAGED(xr_rbc_value_homes_enter(NULL, NULL, &frame)); CHECK(!frame);
    xr_rbc_program_destroy(program);
}
typedef struct OptionalString { uint8_t tag; XrtManagedHeader *payload; } OptionalString;
typedef struct Record { OptionalString name; int64_t score; } Record;
static void real_compiler_and_root(void) {
    XrLirFunction function; XrLirModule *module = record_fixture(&function);
    XrRbcProgram *program = compile(module, function); xr_lir_module_destroy(module);
    CHECK(run(program) == 100);
    const XrRbcModule *code = xr_rbc_program_view(program);
    CHECK(code->function_count == 2);
    const XrLirValueHomes *homes = xr_rbc_module_value_homes(code, 0), *callee = xr_rbc_module_value_homes(code, 1);
    CHECK(homes && callee && !homes->unmodeled && !callee->unmodeled);
    CHECK(homes->call_count == 1 && homes->calls[0].callee == callee->source_function && homes->calls[0].count == 1);
    uint32_t actual = homes->arguments[homes->calls[0].first], formal = 0, local = 0;
    for (uint32_t v = 1; v <= callee->value_count; ++v) {
        if (callee->values[v].parameter == 1) formal = v;
        else if (callee->values[v].type != 0) local = v;
    }
    CHECK(formal && local && callee->values[formal].offset == SIZE_MAX && callee->values[formal].first_owner == SIZE_MAX);
    CHECK(callee->values[formal].temporary_offset == SIZE_MAX && callee->values[formal].temporary_first_owner == SIZE_MAX);
    CHECK(callee->values[formal].type == homes->values[actual].type && callee->result_type == callee->values[local].type);
    CHECK(callee->owned_frame_needed && callee->frame.slot_count == 2 && callee->frame.payload_size == 2 * sizeof(Record));
    uint32_t record = homes->calls[0].result;
    const XrLirValueHome *home = &homes->values[record];
    CHECK(home->size == sizeof(Record) && home->owner_count == 1);
    CHECK(homes->frame.owned_offsets[home->first_owner] == home->offset + offsetof(Record, name) + offsetof(OptionalString, payload));
    bool saw_return = false;
    for (size_t i = 0; i < callee->event_count; ++i)
        saw_return |= callee->events[i].kind == XR_LIR_OWNERSHIP_RETURN_TRANSFER && callee->events[i].source == local;
    CHECK(saw_return);
    XrRbcModule snapshot = *code;
    XrRbcCodeLease lease = snapshot.code_lease; CHECK(lease.retain(lease.owner));
    CHECK(!xr_rbc_module_value_homes(NULL, 0) && !xr_rbc_module_value_homes(&snapshot, UINT32_MAX));
    XrRbcModule raw = snapshot; raw.code_lease = (XrRbcCodeLease){0};
    CHECK(!xr_rbc_module_value_homes(&raw, 0));
    raw = snapshot; --raw.function_count;
    CHECK(!xr_rbc_module_value_homes(&raw, 0));
    raw = snapshot; raw.functions = NULL;
    CHECK(!xr_rbc_module_value_homes(&raw, 0));
    xr_rbc_program_destroy(program);
    CHECK(xr_rbc_module_value_homes(&snapshot, 0) == homes && xr_rbc_module_value_homes(&snapshot, 1) == callee);
    XrtBillingOwner *billing = NULL; XrtManagedRoot *root = NULL; XrtManagedFrame *frame = NULL;
    XrtBackingTypes types = {0}; MANAGED(xrt_billing_owner_new(NULL, SIZE_MAX, &billing));
    MANAGED(xrt_backing_types_init(billing, &types)); MANAGED(xrt_managed_root_new(billing, &root));
    MANAGED(xr_rbc_value_homes_enter(homes, root, &frame));
    XrRbcHomeLocation location = {homes, frame, record, false}; void *address = NULL;
    MANAGED(xr_rbc_value_home_address(location, &address)); Record *value = address;
    XrtPlace place; MANAGED(xr_rbc_value_home_place(location, 0, XRT_PLACE_NONNULL, &place));
    XrtManagedSlotLocation slot; MANAGED(xrt_managed_slot_location(frame, place.slot, &slot));
    CHECK(slot.owner == &value->name.payload && !*slot.owner);
    MANAGED(xrt_string_backing_create(frame, place.slot, &types, "alice", 5, 0));
    value->name.tag = 1; value->score = 95;
    const XrtManagedHeader *borrowed = NULL; MANAGED(xrt_place_borrow(place, &borrowed));
    CHECK(borrowed == value->name.payload && xr_signed_rc_read(&borrowed->references) == 1);
    CHECK(((const XrtStringBacking *)borrowed)->length == 5 && value->score == 95);
    size_t covered = 0;
    for (size_t i = 0; i < home->byte_count; ++i) {
        XrLirHomeBytes span = homes->bytes[home->first_bytes + i];
        CHECK(span.offset <= home->size && span.size <= home->size - span.offset);
        CHECK(span.offset + span.size <= offsetof(Record, name) + offsetof(OptionalString, payload) ||
            span.offset >= offsetof(Record, name) + offsetof(OptionalString, payload) + sizeof(XrtManagedHeader *));
        covered += span.size;
    }
    CHECK(covered + sizeof(XrtManagedHeader *) == sizeof(Record));
    XrtManagedFrame *child = NULL; MANAGED(xr_rbc_value_homes_enter(callee, root, &child));
    address = NULL;
    CHECK(xr_rbc_value_home_address((XrRbcHomeLocation){callee, child, formal, false}, &address) == XRT_MANAGED_ARGUMENT && !address);
    CHECK(xr_signed_rc_read(&borrowed->references) == 1);
    MANAGED(xrt_managed_frame_leave(root, child)); MANAGED(xrt_managed_root_destroy(&root));
    xrt_backing_types_drop(&types); CHECK(xrt_billing_owner_stats(billing).blocks == 1); xrt_billing_owner_drop(billing);
    lease.release(lease.owner);
}
static void state_snapshots_and_ref_boundaries(void) {
    XrLirModule *module = NULL; OK(array_ref_fixture(XR_LIR_STRING, &module));
    XrLirFunction function; OK(xr_lir_function_at(module, 0, &function));
    XrRbcProgram *program = compile(module, function); xr_lir_module_destroy(module);
    const XrRbcModule *code = xr_rbc_program_view(program);
    bool read_owner = false, raw_ref = false;
    for (size_t f = 0; f < code->function_count; ++f) {
        const XrLirValueHomes *homes = xr_rbc_module_value_homes(code, (uint32_t)f); CHECK(homes);
        raw_ref |= (homes->unmodeled & XR_LIR_OWNERSHIP_UNMODELED_REFERENCE) != 0;
        for (size_t e = 0; e < homes->event_count; ++e) {
            XrLirHomeEvent event = homes->events[e];
            if (event.kind == XR_LIR_OWNERSHIP_RESULT_OWNER && homes->values[event.source].type != 0) {
                CHECK(homes->values[event.source].owner_count == 1 && !homes->values[event.source].parameter);
                read_owner = true;
            }
        }
    }
    CHECK(read_owner && raw_ref); CHECK(run(program) == 0);
    xr_rbc_program_destroy(program);
}
static XrLirModule *selected_edge_fixture(XrLirFunction *output) {
    XrLirModule *module = NULL; OK(xr_lir_module_create(NULL, &module));
    XrLirType type; XrLirRecordField field = {"name", 4, XR_LIR_STRING};
    OK(xr_lir_record_type(module, &field, 1, &type));
    XrLirFunction function; XrLirFunctionSpec spec = {.name="edges", .name_length=5, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(module, &spec, &function)); OK(xr_lir_define_function(module, function));
    XrLirBlock entry, join; XrLirType parameters[] = {type, type};
    OK(xr_lir_add_block(module, function, NULL, 0, &entry)); OK(xr_lir_set_entry(module, function, entry));
    OK(xr_lir_add_block(module, function, parameters, 2, &join));
    XrLirValue owners[2], incoming[2];
    for (uint32_t i = 0; i < 2; ++i) {
        XrLirLiteral literal = {0}; OK(xr_lir_add_literal(module, "aa", i + 1, &literal));
        XrLirValue text = record_fixture_value(module, entry, XR_LIR_STRING_LITERAL, XR_LIR_STRING,
            NULL, 0, literal.id, (XrLirFunction){0});
        owners[i] = record_fixture_value(module, entry, XR_LIR_RECORD_NEW, type, &text, 1, 0, (XrLirFunction){0});
        record_fixture_drop(module, entry, XR_LIR_STRING_DROP, text);
        OK(xr_lir_block_parameter(module, join, i, &incoming[i]));
    }
    XrLirValue condition = record_fixture_value(module, entry, XR_LIR_CONST, XR_LIR_I1, NULL, 0, 1, (XrLirFunction){0});
    XrLirValue reverse[] = {owners[1], owners[0]};
    XrLirInstruction branch = {.op=XR_LIR_CONDBR, .type=XR_LIR_UNIT, .operands=&condition, .operand_count=1,
        .edge_count=2, .edges={{join, owners, 2}, {join, reverse, 2}}};
    OK(xr_lir_set_terminator(module, entry, &branch));
    XrLirValue text = record_fixture_value(module, join, XR_LIR_RECORD_FIELD, XR_LIR_STRING,
        &incoming[0], 1, 0, (XrLirFunction){0});
    record_fixture_drop(module, join, XR_LIR_VALUE_DROP, incoming[0]);
    record_fixture_drop(module, join, XR_LIR_VALUE_DROP, incoming[1]);
    XrLirValue length = record_fixture_value(module, join, XR_LIR_STRING_LEN, XR_LIR_I64,
        &text, 1, 0, (XrLirFunction){0});
    record_fixture_drop(module, join, XR_LIR_STRING_DROP, text);
    XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=&length, .operand_count=1};
    OK(xr_lir_set_terminator(module, join, &ret)); OK(xr_lir_module_finish(module, NULL));
    *output = function; return module;
}
static void selected_edge_sources(void) {
    XrLirFunction function; XrLirModule *module = selected_edge_fixture(&function);
    XrtBillingOwner *billing = NULL; XrLirValueLayouts *layouts = NULL; XrLirOwnershipPlan *ownership = NULL;
    MANAGED(xrt_billing_owner_new(NULL, SIZE_MAX, &billing));
    XrLirValueLayoutKey roots[] = {{XR_LIR_TYPE_COUNT, 0}, {XR_LIR_I64, 0}, {XR_LIR_I1, 0}};
    CHECK(xr_lir_value_layouts_create(module, roots, 3, billing, &layouts) == XR_LIR_VALUE_LAYOUT_OK);
    const XrLirOwnershipTypeFact *facts = NULL; uint32_t count = 0;
    CHECK(xr_lir_value_layouts_ownership(layouts, &facts, &count) == XR_LIR_VALUE_LAYOUT_OK);
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){module, facts, count}, &ownership, NULL));
    XrLirValueHomes *homes = NULL;
    CHECK(xr_lir_value_homes_create(&(XrLirValueHomesRequest){module, function, layouts, ownership}, &homes) == XR_LIR_VALUE_LAYOUT_OK);
    xr_lir_module_destroy(module); xr_lir_ownership_plan_destroy(ownership); xr_lir_value_layouts_destroy(layouts);
    CHECK(homes && homes->edge_count == 2 && homes->transfer_count == 4);
    XrLirHomeEdge first = homes->edges[0], second = homes->edges[1];
    CHECK(first.block == second.block && first.target_block == second.target_block && first.edge == 1 && second.edge == 2);
    CHECK(homes->transfers[first.first].source == homes->transfers[second.first + 1].source &&
        homes->transfers[first.first + 1].source == homes->transfers[second.first].source);
    XrtManagedRoot *root = NULL; XrtManagedFrame *frame = NULL; XrtBackingTypes types = {0};
    MANAGED(xrt_backing_types_init(billing, &types));
    MANAGED(xrt_managed_root_new(billing, &root)); MANAGED(xr_rbc_value_homes_enter(homes, root, &frame));
    XrtPlaceEdge edge[2];
    for (size_t i = 0; i < 2; ++i) {
        XrLirHomeTransfer transfer = homes->transfers[second.first + i];
        MANAGED(xr_rbc_value_home_place((XrRbcHomeLocation){homes, frame, transfer.source, false}, 0, XRT_PLACE_NONNULL, &edge[i].source));
        MANAGED(xr_rbc_value_home_place((XrRbcHomeLocation){homes, frame, transfer.target, false}, 0, XRT_PLACE_NONNULL, &edge[i].destination));
        MANAGED(xr_rbc_value_home_place((XrRbcHomeLocation){homes, frame, transfer.target, true}, 0, XRT_PLACE_NONNULL, &edge[i].temporary));
        CHECK(homes->values[transfer.target].byte_count == 0 && edge[i].temporary.slot != edge[i].source.slot);
        MANAGED(xrt_string_backing_create(frame, edge[i].source.slot, &types, "ab", i + 1, 0));
    }
    MANAGED(xrt_place_edge_move(edge, 2, NULL));
    for (size_t i = 0; i < 2; ++i) {
        const XrtManagedHeader *header = NULL; MANAGED(xrt_place_borrow(edge[i].destination, &header));
        CHECK(((const XrtStringBacking *)header)->length == i + 1 && xr_signed_rc_read(&header->references) == 1);
        XrtManagedHeader *empty = NULL;
        MANAGED(xrt_managed_slot_borrow(frame, edge[i].source.slot, &empty)); CHECK(!empty);
        MANAGED(xrt_managed_slot_borrow(frame, edge[i].temporary.slot, &empty)); CHECK(!empty);
    }
    MANAGED(xrt_managed_root_destroy(&root)); xrt_backing_types_drop(&types);
    CHECK(xrt_billing_owner_stats(billing).blocks == 1); xrt_billing_owner_drop(billing); xr_lir_value_homes_destroy(homes);
}
static void common_home_allocation_failures(void) {
    CHECK(!home_live);
    XrLirFunction function; XrLirModule *module = record_fixture(&function);
    XrtBillingOwner *billing = NULL; XrLirValueLayouts *layouts = NULL; XrLirOwnershipPlan *ownership = NULL;
    MANAGED(xrt_billing_owner_new(NULL, SIZE_MAX, &billing));
    XrLirValueLayoutKey roots[] = {{XR_LIR_TYPE_COUNT, 0}, {XR_LIR_TYPE_COUNT + 1, 0}};
    CHECK(xr_lir_value_layouts_create(module, roots, 2, billing, &layouts) == XR_LIR_VALUE_LAYOUT_OK);
    const XrLirOwnershipTypeFact *types = NULL; uint32_t count = 0;
    CHECK(xr_lir_value_layouts_ownership(layouts, &types, &count) == XR_LIR_VALUE_LAYOUT_OK);
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){module, types, count}, &ownership, NULL));
    XrLirValueHomesRequest request = {module, function, layouts, ownership}; XrLirValueHomes *homes = NULL;
    home_attempts = 0;
    CHECK(xr_lir_value_homes_create(&request, &homes) == XR_LIR_VALUE_LAYOUT_OK && homes);
    size_t attempts = home_attempts; xr_lir_value_homes_destroy(homes); homes = NULL; CHECK(!home_live);
    for (size_t failure = 1; failure <= attempts; ++failure) {
        home_attempts = 0; home_failure = failure;
        CHECK(xr_lir_value_homes_create(&request, &homes) == XR_LIR_VALUE_LAYOUT_OOM && !homes && !home_live);
    }
    home_failure = SIZE_MAX;
    CHECK(xr_lir_value_homes_create(&request, &homes) == XR_LIR_VALUE_LAYOUT_OK && homes);
    XrLirValueHomes *preserved = homes;
    CHECK(xr_lir_value_homes_create(&request, &homes) == XR_LIR_VALUE_LAYOUT_ARGUMENT && homes == preserved);
    xr_lir_module_destroy(module); xr_lir_ownership_plan_destroy(ownership); xr_lir_value_layouts_destroy(layouts);
    CHECK(xrt_billing_owner_stats(billing).blocks == 1); xrt_billing_owner_drop(billing);
    CHECK(homes->call_count == 1 && homes->event_count && homes->owned_frame_needed);
    xr_lir_value_homes_destroy(homes); CHECK(!home_live);
    printf("Common home allocation rollback sites: %zu\n", attempts);
}
static void pod_needs_no_owner_frame(void) {
    XrLirModule *module = NULL; OK(xr_lir_module_create(NULL, &module));
    XrLirType type; XrLirRecordField field = {"score", 5, XR_LIR_I64}; OK(xr_lir_record_type(module, &field, 1, &type));
    XrLirFunction function; XrLirFunctionSpec spec = {.name="pod", .name_length=3, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(module, &spec, &function)); OK(xr_lir_define_function(module, function));
    XrLirBlock block; OK(xr_lir_add_block(module, function, NULL, 0, &block)); OK(xr_lir_set_entry(module, function, block));
    XrLirValue number = record_fixture_value(module, block, XR_LIR_CONST, XR_LIR_I64, NULL, 0, 17, (XrLirFunction){0});
    XrLirValue record = record_fixture_value(module, block, XR_LIR_RECORD_NEW, type, &number, 1, 0, (XrLirFunction){0});
    XrLirValue result = record_fixture_value(module, block, XR_LIR_RECORD_FIELD, XR_LIR_I64, &record, 1, 0, (XrLirFunction){0});
    record_fixture_drop(module, block, XR_LIR_VALUE_DROP, record);
    XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=&result, .operand_count=1};
    OK(xr_lir_set_terminator(module, block, &ret)); OK(xr_lir_module_finish(module, NULL));
    XrRbcProgram *program = compile(module, function); xr_lir_module_destroy(module);
    const XrLirValueHomes *homes = xr_rbc_module_value_homes(xr_rbc_program_view(program), 0);
    CHECK(homes && !homes->owned_frame_needed && !homes->frame.slot_count && !homes->event_count);
    CHECK(homes->values[record.id].size == sizeof(int64_t) && homes->values[record.id].byte_count == 1);
    CHECK(homes->values[record.id].temporary_offset == SIZE_MAX && homes->values[record.id].temporary_first_owner == SIZE_MAX);
    CHECK(run(program) == 17); xr_rbc_program_destroy(program); CHECK(!home_live);
}
typedef struct MultiReturn { OptionalString first; int64_t score; OptionalString second; } MultiReturn;
typedef struct ReturnWatch {
    MultiReturn *destination, *source;
    XrtManagedHeader *old[2], *expected[2];
    size_t observed, leases;
    int64_t score;
    bool active;
} ReturnWatch;
static bool return_retain(void *context) { ++((ReturnWatch *)context)->leases; return true; }
static void return_drop(void *context) { ReturnWatch *watch = context; CHECK(watch->leases); --watch->leases; }
static void *return_allocate(void *context, size_t bytes) { (void)context; return xr_malloc(bytes); }
static void return_free(void *context, void *memory, size_t bytes) {
    (void)bytes; ReturnWatch *watch = context;
    if (watch->active && (memory == watch->old[0] || memory == watch->old[1])) {
        CHECK(watch->destination->first.tag == 1 && watch->destination->second.tag == 1);
        CHECK(watch->destination->first.payload == watch->expected[0] && watch->destination->second.payload == watch->expected[1]);
        CHECK(watch->destination->score == watch->score);
        CHECK(!watch->source->first.tag && !watch->source->second.tag && !watch->source->score);
        CHECK(!watch->source->first.payload && !watch->source->second.payload);
        ++watch->observed;
    }
    xr_free(memory);
}
typedef struct ReturnPublication {
    const XrLirValueHomes *homes;
    const XrLirValueHome *home;
    unsigned char *source, *temporary, *destination;
    size_t published;
} ReturnPublication;
static void return_publish(void *context) {
    ReturnPublication *publication = context;
    for (size_t i = 0; i < publication->home->byte_count; ++i) {
        XrLirHomeBytes span = publication->homes->bytes[publication->home->first_bytes + i];
        memcpy(publication->destination + span.offset, publication->temporary + span.offset, span.size);
        memset(publication->source + span.offset, 0, span.size);
    }
    ++publication->published;
}
static XrRbcProgram *multi_return_fixture(void) {
    XrLirModule *module = NULL; OK(xr_lir_module_create(NULL, &module));
    XrLirType optional, record; OK(xr_lir_nullable_type(module, XR_LIR_STRING, &optional));
    XrLirRecordField fields[] = {{"first",5,optional}, {"score",5,XR_LIR_I64}, {"second",6,optional}};
    OK(xr_lir_record_type(module, fields, 3, &record));
    XrLirFunction root, helper; XrLirFunctionSpec spec = {.name="return_root", .name_length=11, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(module, &spec, &root));
    spec = (XrLirFunctionSpec){.name="return_copy", .name_length=11, .parameters=&record, .parameter_count=1, .result=record};
    OK(xr_lir_declare_function(module, &spec, &helper)); OK(xr_lir_define_function(module, helper));
    XrLirBlock body; OK(xr_lir_add_block(module, helper, NULL, 0, &body)); OK(xr_lir_set_entry(module, helper, body));
    XrLirValue argument; OK(xr_lir_function_parameter(module, helper, 0, &argument));
    XrLirValue copy = record_fixture_value(module, body, XR_LIR_VALUE_RETAIN, record, &argument, 1, 0, (XrLirFunction){0});
    XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=&copy, .operand_count=1};
    OK(xr_lir_set_terminator(module, body, &ret)); OK(xr_lir_define_function(module, root));
    OK(xr_lir_add_block(module, root, NULL, 0, &body)); OK(xr_lir_set_entry(module, root, body));
    XrLirValue some[2];
    for (uint32_t i = 0; i < 2; ++i) {
        XrLirLiteral literal = {0}; OK(xr_lir_add_literal(module, "ab", i + 1, &literal));
        XrLirValue text = record_fixture_value(module, body, XR_LIR_STRING_LITERAL, XR_LIR_STRING, NULL, 0, literal.id, (XrLirFunction){0});
        some[i] = record_fixture_value(module, body, XR_LIR_NULLABLE_SOME, optional, &text, 1, 0, (XrLirFunction){0});
        record_fixture_drop(module, body, XR_LIR_STRING_DROP, text);
    }
    XrLirValue args[] = {some[0], record_fixture_value(module, body, XR_LIR_CONST, XR_LIR_I64, NULL, 0, 42, (XrLirFunction){0}), some[1]};
    XrLirValue initial = record_fixture_value(module, body, XR_LIR_RECORD_NEW, record, args, 3, 0, (XrLirFunction){0});
    for (uint32_t i = 0; i < 2; ++i) record_fixture_drop(module, body, XR_LIR_VALUE_DROP, some[i]);
    copy = record_fixture_value(module, body, XR_LIR_CALL, record, &initial, 1, 0, helper);
    record_fixture_drop(module, body, XR_LIR_VALUE_DROP, initial);
    XrLirValue score = record_fixture_value(module, body, XR_LIR_RECORD_FIELD, XR_LIR_I64, &copy, 1, 1, (XrLirFunction){0});
    record_fixture_drop(module, body, XR_LIR_VALUE_DROP, copy);
    ret.operands = &score; OK(xr_lir_set_terminator(module, body, &ret)); OK(xr_lir_module_finish(module, NULL));
    XrRbcProgram *program = compile(module, root); xr_lir_module_destroy(module); return program;
}
static void complete_multi_leaf_return(void) {
    XrRbcProgram *program = multi_return_fixture(); CHECK(run(program) == 42);
    const XrRbcModule *code = xr_rbc_program_view(program);
    const XrLirValueHomes *caller = xr_rbc_module_value_homes(code, 0), *callee = xr_rbc_module_value_homes(code, 1);
    uint32_t result = caller->calls[0].result, returned = 0;
    for (size_t i = 0; i < callee->event_count; ++i)
        if (callee->events[i].kind == XR_LIR_OWNERSHIP_RETURN_TRANSFER) returned = callee->events[i].source;
    CHECK(returned && caller->values[result].owner_count == 2 && callee->values[returned].owner_count == 2);
    CHECK(caller->values[result].temporary_offset != SIZE_MAX && callee->values[returned].temporary_offset != SIZE_MAX);
    ReturnWatch watch = {.leases=1};
    XrtManagedAllocator provider = {&watch, return_allocate, return_free, {&watch, return_retain, return_drop}};
    XrtBillingOwner *billing = NULL; XrtManagedRoot *root = NULL; XrtManagedFrame *destination = NULL, *source = NULL;
    XrtBackingTypes types = {0}; MANAGED(xrt_billing_owner_new(&provider, SIZE_MAX, &billing));
    MANAGED(xrt_backing_types_init(billing, &types)); MANAGED(xrt_managed_root_new(billing, &root));
    MANAGED(xr_rbc_value_homes_enter(caller, root, &destination)); MANAGED(xr_rbc_value_homes_enter(callee, root, &source));
    XrRbcHomeLocation incoming = {callee, source, returned, false}, landing = {caller, destination, result, false};
    XrRbcHomeLocation staging = {callee, source, returned, true};
    void *source_bytes = NULL, *destination_bytes = NULL, *temporary_bytes = NULL;
    MANAGED(xr_rbc_value_home_address(incoming, &source_bytes)); MANAGED(xr_rbc_value_home_address(landing, &destination_bytes));
    MANAGED(xr_rbc_value_home_address(staging, &temporary_bytes));
    watch.source = source_bytes; watch.destination = destination_bytes;
    XrtPlaceEdge edge[2];
    for (size_t i = 0; i < 2; ++i) {
        MANAGED(xr_rbc_value_home_place(incoming, i, XRT_PLACE_NONNULL, &edge[i].source));
        MANAGED(xr_rbc_value_home_place(landing, i, XRT_PLACE_NONNULL, &edge[i].destination));
        MANAGED(xr_rbc_value_home_place(staging, i, XRT_PLACE_NONNULL, &edge[i].temporary));
        MANAGED(xrt_string_backing_create(destination, edge[i].destination.slot, &types, "old", 3, 0));
    }
    watch.destination->first.tag = watch.destination->second.tag = 1; watch.destination->score = -1;
    ReturnPublication publication = {callee, &callee->values[returned], source_bytes, temporary_bytes, destination_bytes, 0};
    XrtPlacePublication hook = {return_publish, &publication};
    for (unsigned iteration = 0; iteration < 2; ++iteration) {
        for (size_t i = 0; i < 2; ++i) {
            MANAGED(xrt_string_backing_create(source, edge[i].source.slot, &types, "next", i + 1, 0));
            watch.old[i] = i ? watch.destination->second.payload : watch.destination->first.payload;
            watch.expected[i] = i ? watch.source->second.payload : watch.source->first.payload;
        }
        watch.source->first.tag = watch.source->second.tag = 1; watch.source->score = watch.score = 900 + iteration;
        for (size_t i = 0; i < publication.home->byte_count; ++i) {
            XrLirHomeBytes span = callee->bytes[publication.home->first_bytes + i];
            memcpy(publication.temporary + span.offset, publication.source + span.offset, span.size);
        }
        XrtManagedHeader *pending = NULL;
        MANAGED(xrt_managed_slot_prepare(source, edge[0].source.slot, types.string, sizeof(XrtStringBacking) + 1, &pending));
        CHECK(xrt_place_edge_move(edge, 2, &hook) == XRT_MANAGED_ARGUMENT);
        CHECK(watch.destination->first.payload == watch.old[0] && watch.destination->second.payload == watch.old[1]);
        CHECK(publication.published == iteration && watch.destination->score == (iteration ? 900 : -1));
        MANAGED(xrt_managed_slot_abort(source, edge[0].source.slot));
        watch.active = true; size_t observed = watch.observed;
        xrt_billing_owner_fail_after(billing, 0);
        MANAGED(xrt_place_edge_move(edge, 2, &hook));
        xrt_billing_owner_fail_after(billing, SIZE_MAX); watch.active = false;
        CHECK(watch.observed == observed + 2 && publication.published == iteration + 1);
        for (size_t i = 0; i < 2; ++i) {
            XrtManagedHeader *empty = NULL; MANAGED(xrt_managed_slot_borrow(source, edge[i].temporary.slot, &empty)); CHECK(!empty);
        }
    }
    MANAGED(xrt_managed_root_destroy(&root)); xrt_backing_types_drop(&types); xrt_billing_owner_drop(billing);
    return_drop(&watch); CHECK(!watch.leases); xr_rbc_program_destroy(program); CHECK(!home_live);
    puts("multi-leaf return: reused landing, complete publication before old callbacks and pending rejection passed");
}
int main(void) {
    scalar_zero_frame(); real_compiler_and_root(); state_snapshots_and_ref_boundaries(); selected_edge_sources();
    common_home_allocation_failures(); pod_needs_no_owner_frame(); CHECK(!home_live);
    complete_multi_leaf_return();
    puts("RBC raw homes: shared plans, borrowed formals, stable Header fields and zero scalar frames passed");
    return 0;
}
