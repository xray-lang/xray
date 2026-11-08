/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_storage_flow.c - Storage versions reaching individual instructions
 *
 * KEY CONCEPT:
 *   Strong writes replace a version. Joins name numeric phi values, so loops
 *   close without adding future writes to an earlier read or to a Cell handle.
 */
#include "xxir_storage_flow.h"
#include "xxir_instruction_flow.h"
#include "xxir_compile_memory.h"
#include "xxir_receive_control.h"
#include "xxir_operand_roles.h"
#include "xxir_declarations.h"
#include "../base/xhash.h"

typedef struct StoragePredecessor {
    uint32_t source, next, ordinal;
    XirInstructionFlowKind kind;
} StoragePredecessor;

typedef struct StorageAddress { uint32_t first, count; bool ready; } StorageAddress;
typedef struct StorageState { uint32_t point, handle, value, kind, order, generation; } StorageState;
typedef struct StorageWork { uint32_t function, state; } StorageWork;
typedef struct StorageIdentityLink { uint32_t identity, next; } StorageIdentityLink;
typedef struct StorageOperation { XrXirOp op; uint32_t source, destination, receiver; } StorageOperation;
typedef struct StorageRead {
    uint32_t source, target, point;
    XirStoragePointKind kind;
    bool force;
} StorageRead;
typedef struct StorageWrite { uint32_t point, handle; } StorageWrite;
typedef struct StoragePublication {
    uint32_t point, handle, condition_point, condition_handle;
    XirStoragePointKind kind, condition_kind;
    bool conditional;
} StoragePublication;
enum { STORAGE_EXPOSURE_KIND = 16 };
typedef struct StorageExecution {
    XirExecutionSite site;
    uint32_t applications[3], next;
    uint8_t *identity_parameters;
    bool bindings_ready, active;
} StorageExecution;

typedef struct StorageFunction {
    XrXirFunction header;
    const XrXirFunction *body;
    XrXirType *types;
    uint8_t *handles;
    StorageOperation *operations;
    StorageRead *reads;
    uint32_t read_count, read_capacity, observed, current_block;
    StorageWrite *writes;
    uint32_t write_count, write_capacity;
    uint32_t *write_offsets, *write_points, *calls, *content_inputs;
    uint32_t *exposure_inputs, *exposure_offsets, *exposure_points;
    uint32_t call_count, call_capacity;
    StoragePublication *publications;
    uint32_t *publication_index, publication_index_capacity;
    uint32_t publication_count, publication_capacity, exposure_capacity;
    bool events_ready;
    uint32_t *blocks, *predecessors;
    StoragePredecessor *edges;
    uint32_t edge_count, edge_capacity, flow_capacity, footprint_capacity;
    uint32_t *flow_index, *exposure_index, *footprint_index;
    uint32_t flow_index_capacity, exposure_index_capacity, footprint_index_capacity;
    uint32_t point_count, site_head_count;
    uint32_t *identity_heads;
    StorageIdentityLink *identity_links;
    uint32_t identity_count, identity_capacity;
    uint32_t *address_values, address_count, address_capacity;
    StorageAddress *addresses;
    uint32_t *visited, *queue;
    uint32_t generation;
    StorageState *states;
    uint32_t state_count, state_capacity;
    uint32_t *state_index, index_capacity;
    uint32_t *site_heads;
} StorageFunction;

struct XirStorageFlow {
    XrXirCompileContext memory;
    const XrXirModule *module;
    const XirExecutionOwner *execution;
    XrCompileRequestControl *control;
    XirStorageFlowView view;
    StorageFunction *rows;
    StorageWork *work;
    uint32_t work_count, work_capacity, work_first;
    StorageWork *stages;
    uint32_t stage_count, stage_capacity, stage_first, stage_generation;
    uint32_t crossflow_capacity, application_capacity, binding_capacity;
    uint32_t point_value_capacity, point_index_capacity;
    uint32_t *point_index, *point_value_index;
    uint32_t identity_capacity, allocation_capacity, input_capacity, unknown_capacity, exposure_seed_capacity;
    uint32_t *cross_index, *unknown_index, *exposure_seed_index;
    uint32_t cross_index_capacity, unknown_index_capacity, exposure_seed_index_capacity;
    StorageExecution *sites;
    uint32_t site_count, site_capacity, site_index_capacity, generation;
    uint32_t *site_index;
    uint32_t progress, request_remainder;
    uint64_t revision;
    XrXirStatus status;
    bool sealed, sealing, local_finished, exposure_started;
};
static bool storage_close_applications(XirStorageFlow *flow);
static bool storage_start_exposure(XirStorageFlow *flow);

static uint32_t storage_base_kind(StorageState state) { return state.kind & (STORAGE_EXPOSURE_KIND - 1); }
static XirStorageFlowBank storage_bank(StorageState state) {
    return state.kind & STORAGE_EXPOSURE_KIND ? XIR_STORAGE_EXPOSURE : XIR_STORAGE_CONTENTS;
}
static uint32_t storage_kind(uint32_t base, XirStorageFlowBank bank) {
    return base | (bank == XIR_STORAGE_EXPOSURE ? STORAGE_EXPOSURE_KIND : 0);
}

static bool storage_step(XirStorageFlow *flow) {
    if (flow->status == XR_XIR_OK)
        flow->status = xir_receive_step(flow->control, &flow->progress);
    return flow->status == XR_XIR_OK;
}

static bool storage_reserve(XirStorageFlow *flow, void **data, uint32_t count,
    uint32_t *capacity, size_t size) {
    if (flow->status != XR_XIR_OK) return false;
    if (count < *capacity) return true;
    void *grown = xir_compile_array_grow(&flow->memory, *data, count, size, capacity, &flow->status);
    if (!grown) return false;
    *data = grown;
    return true;
}

static uint32_t storage_aux(XirStorageFlow *flow, uint32_t function) {
    XirStorageFlowFunction *row = (XirStorageFlowFunction *)&flow->view.functions[function];
    if (row->value_count == UINT32_MAX) { flow->status = XR_XIR_OVERFLOW; return UINT32_MAX; }
    return row->value_count++;
}

#include "xxir_storage_facts.inc.c"

static bool storage_allocation(XirStorageFlow *flow, uint32_t function, uint32_t value) {
    void *values = (void *)flow->view.allocations;
    if (!storage_reserve(flow, &values, flow->view.allocation_count, &flow->allocation_capacity,
        sizeof(*flow->view.allocations))) return false;
    flow->view.allocations = values;
    ((XirStorageAllocation *)values)[flow->view.allocation_count++] = (XirStorageAllocation){function, value};
    return true;
}

static bool storage_input(XirStorageFlow *flow, uint32_t function, uint32_t value, uint32_t handle,
    XirStorageFlowBank bank) {
    void *values = (void *)flow->view.inputs;
    if (!storage_reserve(flow, &values, flow->view.input_count, &flow->input_capacity,
        sizeof(*flow->view.inputs))) return false;
    flow->view.inputs = values;
    ((XirStorageInput *)values)[flow->view.input_count++] = (XirStorageInput){function, value, handle, bank};
    return true;
}

static XrXirType storage_payload(const XirStorageFlow *flow, uint32_t function, uint32_t value) {
    XrXirType type = flow->rows[function].types[value];
    return xr_xir_type_is_cell(flow->module->types, type) ? xr_xir_cell_element(flow->module->types, type) : type;
}

static bool storage_identity(XirStorageFlow *flow, uint32_t function, uint32_t source, uint32_t target) {
    StorageFunction *row = &flow->rows[function];
    uint32_t physical = flow->view.functions[function].physical_values;
    if (source >= physical || target >= physical) { flow->status = XR_XIR_BAD_STRUCTURE; return false; }
    void *values = (void *)flow->view.identities;
    if (!storage_reserve(flow, &values, flow->view.identity_count, &flow->identity_capacity,
        sizeof(*flow->view.identities))) return false;
    flow->view.identities = values;
    if (!storage_reserve(flow, (void **)&row->identity_links, row->identity_count,
        &row->identity_capacity, sizeof(*row->identity_links))) return false;
    /* The local list stores global identity ordinals, followed by its previous
     * head. Two words avoid retaining pointers into either growable array. */
    uint32_t index = flow->view.identity_count++;
    ((XirStorageIdentity *)values)[index] = (XirStorageIdentity){function, source, function, target, UINT32_MAX};
    uint32_t local = row->identity_count++;
    row->identity_links[local] = (StorageIdentityLink){index, row->identity_heads[target]};
    row->identity_heads[target] = local;
    return true;
}

static bool storage_address(XirStorageFlow *flow, uint32_t function, uint32_t handle,
    StorageAddress *output) {
    StorageFunction *row = &flow->rows[function];
    uint32_t physical = flow->view.functions[function].physical_values;
    if (handle >= physical) { flow->status = XR_XIR_BAD_STRUCTURE; return false; }
    if (row->addresses[handle].ready) { *output = row->addresses[handle]; return true; }
    if (++row->generation == 0) {
        memset(row->visited, 0, (size_t)physical * sizeof(*row->visited));
        row->generation = 1;
    }
    uint32_t first = row->address_count, pending = 1, next = 0;
    row->queue[0] = handle; row->visited[handle] = row->generation;
    while (next < pending && storage_step(flow)) {
        uint32_t value = row->queue[next++], head = row->identity_heads[value];
        if (head == UINT32_MAX) {
            if (!storage_reserve(flow, (void **)&row->address_values, row->address_count,
                &row->address_capacity, sizeof(*row->address_values))) return false;
            row->address_values[row->address_count++] = value;
        }
        for (uint32_t e = head; e != UINT32_MAX && storage_step(flow); e = row->identity_links[e].next) {
            XirStorageIdentity edge = flow->view.identities[row->identity_links[e].identity];
            if (row->visited[edge.source] == row->generation) continue;
            row->visited[edge.source] = row->generation; row->queue[pending++] = edge.source;
        }
    }
    if (flow->status != XR_XIR_OK) return false;
    if (row->address_count == first) {
        if (!storage_reserve(flow, (void **)&row->address_values, row->address_count,
            &row->address_capacity, sizeof(*row->address_values))) return false;
        row->address_values[row->address_count++] = handle;
    }
    row->addresses[handle] = (StorageAddress){first, row->address_count - first, true};
    *output = row->addresses[handle];
    return true;
}

static uint32_t storage_state_hash(uint32_t point, uint32_t handle, uint32_t kind, uint32_t order) {
    uint32_t fields[] = {point, handle, kind, order};
    return xr_hash_bytes(fields, sizeof(fields));
}

static bool storage_index_grow(XirStorageFlow *flow, uint32_t function) {
    StorageFunction *row = &flow->rows[function];
    uint64_t next = row->index_capacity ? (uint64_t)row->index_capacity * 2u : 16u;
    if (next > UINT32_MAX) { flow->status = XR_XIR_OVERFLOW; return false; }
    uint32_t capacity = (uint32_t)next;
    uint32_t *index = xir_compile_calloc(&flow->memory, capacity, sizeof(*index), &flow->status);
    if (!index) return false;
    for (uint32_t n = 0; n < row->state_count && storage_step(flow); ++n) {
        StorageState state = row->states[n];
        uint32_t slot = storage_state_hash(state.point, state.handle, state.kind, state.order) & (capacity - 1);
        while (index[slot]) slot = (slot + 1) & (capacity - 1);
        index[slot] = n + 1;
    }
    if (flow->status != XR_XIR_OK) { xr_compile_resources_free(index); return false; }
    xr_compile_resources_free(row->state_index); row->state_index = index; row->index_capacity = capacity;
    return true;
}

static uint32_t storage_lower(const uint32_t *points, uint32_t first, uint32_t count, uint32_t point) {
    uint32_t begin = first, end = first + count;
    while (begin < end) {
        uint32_t middle = begin + (end - begin) / 2;
        if (points[middle] < point) begin = middle + 1;
        else end = middle;
    }
    return begin;
}

static uint32_t storage_before(const uint32_t *points, uint32_t first, uint32_t count, uint32_t point) {
    uint32_t next = storage_lower(points, first, count, point);
    return next == first ? UINT32_MAX : points[next - 1];
}

static uint32_t storage_anchor(const XirStorageFlow *flow, uint32_t function,
    uint32_t point, uint32_t handle, uint32_t kind) {
    const StorageFunction *row = &flow->rows[function];
    if ((kind & (STORAGE_EXPOSURE_KIND - 1)) || !row->events_ready || point >= row->body->instruction_count)
        return point;
    StorageAddress address = row->addresses[handle];
    if (!address.ready || address.count != 1 || row->address_values[address.first] != handle) return point;
    uint32_t block = row->blocks[point], first = row->body->blocks[block].first;
    uint32_t begin = row->write_offsets[handle], end = row->write_offsets[handle + 1];
    uint32_t write = storage_before(row->write_points, begin, end - begin, point);
    uint32_t call = storage_before(row->calls, 0, row->call_count, point);
    uint32_t anchor = first;
    if (write != UINT32_MAX && write >= first) anchor = write + 1;
    if (call != UINT32_MAX && call >= first && call + 1 > anchor) anchor = call + 1;
    if ((kind & STORAGE_EXPOSURE_KIND) && row->exposure_offsets) {
        begin = row->exposure_offsets[handle]; end = row->exposure_offsets[handle + 1];
        uint32_t publication = storage_before(row->exposure_points, begin, end - begin, point + 1);
        if (publication != UINT32_MAX && publication >= first && publication > anchor) anchor = publication;
    }
    return anchor;
}

static uint32_t storage_point(XirStorageFlow *flow, uint32_t function, uint32_t point,
    uint32_t handle, uint32_t kind, uint32_t order) {
    StorageFunction *row = &flow->rows[function];
    if (point >= row->point_count ||
        handle >= flow->view.functions[function].physical_values) {
        flow->status = XR_XIR_BAD_STRUCTURE; return UINT32_MAX;
    }
    point = storage_anchor(flow, function, point, handle, kind);
    if (!row->index_capacity || row->state_count >= row->index_capacity / 2)
        if (!storage_index_grow(flow, function)) return UINT32_MAX;
    uint32_t slot = storage_state_hash(point, handle, kind, order) & (row->index_capacity - 1);
    while (row->state_index[slot]) {
        StorageState found = row->states[row->state_index[slot] - 1];
        if (found.point == point && found.handle == handle && found.kind == kind && found.order == order)
            return found.value;
        slot = (slot + 1) & (row->index_capacity - 1);
    }
    if (!storage_reserve(flow, (void **)&row->states, row->state_count,
        &row->state_capacity, sizeof(*row->states)) ||
        !storage_reserve(flow, (void **)&flow->work, flow->work_count,
        &flow->work_capacity, sizeof(*flow->work))) return UINT32_MAX;
    bool stage = (kind & (STORAGE_EXPOSURE_KIND - 1)) && (kind & (STORAGE_EXPOSURE_KIND - 1)) != 5;
    if (stage && !storage_reserve(flow, (void **)&flow->stages, flow->stage_count,
        &flow->stage_capacity, sizeof(*flow->stages))) return UINT32_MAX;
    uint32_t value = storage_aux(flow, function);
    if (value == UINT32_MAX) return value;
    uint32_t index = row->state_count++;
    row->states[index] = (StorageState){point, handle, value, kind, order, 0}; row->state_index[slot] = index + 1;
    flow->work[flow->work_count++] = (StorageWork){function, index};
    if (stage) flow->stages[flow->stage_count++] = (StorageWork){function, index};
    return value;
}

static uint32_t storage_state(XirStorageFlow *flow, uint32_t function, uint32_t point, uint32_t handle) {
    return storage_point(flow, function, point, handle, 0, 0);
}

static uint32_t storage_bank_state(XirStorageFlow *flow, uint32_t function, uint32_t point,
    uint32_t handle, XirStorageFlowBank bank) {
    return storage_point(flow, function, point, handle, storage_kind(0, bank), 0);
}

static bool storage_predecessor(XirStorageFlow *flow, uint32_t function, uint32_t source,
    uint32_t target, XirInstructionFlowKind kind, uint32_t ordinal) {
    StorageFunction *row = &flow->rows[function];
    if (target >= row->point_count) { flow->status = XR_XIR_BAD_STRUCTURE; return false; }
    if (!storage_reserve(flow, (void **)&row->edges, row->edge_count,
        &row->edge_capacity, sizeof(*row->edges))) return false;
    uint32_t index = row->edge_count++;
    row->edges[index] = (StoragePredecessor){source, row->predecessors[target], ordinal, kind};
    row->predecessors[target] = index;
    return true;
}

typedef struct StorageConnect { XirStorageFlow *flow; uint32_t function; } StorageConnect;
static XrXirStatus storage_successor(void *opaque, const XirInstructionFlowEdge *edge) {
    StorageConnect *connect = opaque;
    storage_predecessor(connect->flow, connect->function, edge->source, edge->target, edge->kind, edge->ordinal);
    return connect->flow->status;
}

static bool storage_is_handle(const XirStorageFlow *flow, uint32_t function, uint32_t value) {
    return value < flow->view.functions[function].physical_values && flow->rows[function].handles[value] != 0;
}

static bool storage_new_place(XrXirOp op) {
    return op == XR_XIR_LOCAL_NEW || op == XR_XIR_SCALAR_LOCAL_NEW || op == XR_XIR_OWNED_LOCAL_NEW ||
        op == XR_XIR_LOCAL_UNINIT || op == XR_XIR_CELL_NEW || op == XR_XIR_CELL_NEW_TAKE;
}

static bool storage_read(XrXirOp op) {
    return op == XR_XIR_LOCAL_READ || op == XR_XIR_SCALAR_LOCAL_READ || op == XR_XIR_OWNED_LOCAL_READ ||
        op == XR_XIR_CELL_READ || op == XR_XIR_CELL_TAKE || op == XR_XIR_PLACE_READ;
}

static bool storage_copy(XrXirOp op) {
    return op == XR_XIR_COPY || op == XR_XIR_SCALAR_COPY || op == XR_XIR_OWNED_RETAIN ||
        op == XR_XIR_FUNCTION_WEAKEN || op == XR_XIR_VALUE_TAKE;
}

static bool storage_write(XrXirOp op) {
    return op == XR_XIR_LOCAL_WRITE || op == XR_XIR_SCALAR_LOCAL_WRITE || op == XR_XIR_OWNED_LOCAL_WRITE ||
        op == XR_XIR_CELL_WRITE || op == XR_XIR_CELL_WRITE_TAKE || op == XR_XIR_CELL_LOCAL_WRITE ||
        op == XR_XIR_PLACE_WRITE;
}

static bool storage_execution(XrXirOp op) {
    return op == XR_XIR_CALL || op == XR_XIR_CALL_DEFAULT || op == XR_XIR_CALL_INDIRECT ||
        op == XR_XIR_CALL_REQUIREMENT || op == XR_XIR_INVOKE || op == XR_XIR_INVOKE_DEFAULT ||
        op == XR_XIR_INVOKE_INDIRECT || op == XR_XIR_CLEANUP_LEAVE || op == XR_XIR_CLEANUP_ERROR ||
        op == XR_XIR_RETURN || op == XR_XIR_THROW;
}

static bool storage_observe_identity(XirStorageFlow *flow, uint32_t function, uint32_t i,
    const XrXirInstruction *op) {
    const XrXirFunction *body = flow->rows[function].body;
    uint32_t value = body->parameter_count + i;
    if (storage_new_place(op->op) && !storage_allocation(flow, function, value)) return false;
    if (!storage_is_handle(flow, function, value)) return true;
    if (storage_copy(op->op) || op->op == XR_XIR_CELL_PLACE || op->op == XR_XIR_CELL_PROJECT) {
        if (!storage_identity(flow, function, op->args[0], value)) return false;
    } else if (op->op == XR_XIR_PHI) {
        if (op->args[0] > body->operand_count || op->args[1] > body->operand_count - op->args[0] ||
            op->args[1] % 2) { flow->status = XR_XIR_BAD_STRUCTURE; return false; }
        for (uint32_t p = 1; p < op->args[1]; p += 2)
            if (!storage_identity(flow, function, body->operands[op->args[0] + p], value)) return false;
    }
    return flow->status == XR_XIR_OK;
}

static uint32_t storage_contents(XirStorageFlow *flow, uint32_t function, uint32_t point, uint32_t handle) {
    StorageAddress address;
    if (!storage_address(flow, function, handle, &address)) return UINT32_MAX;
    if (address.count == 1)
        return storage_state(flow, function, point, flow->rows[function].address_values[address.first]);
    return storage_state(flow, function, point, handle);
}

static uint32_t storage_contents_point(XirStorageFlow *flow, uint32_t function, uint32_t point,
    uint32_t handle, XirStoragePointKind kind, XirStorageFlowBank bank) {
    StorageAddress address;
    if (!storage_address(flow, function, handle, &address)) return UINT32_MAX;
    uint32_t root = address.count == 1 ? flow->rows[function].address_values[address.first] : handle;
    if (kind == XIR_STORAGE_BEFORE) return storage_bank_state(flow, function, point, root, bank);
    if (kind == XIR_STORAGE_NORMAL) return storage_point(flow, function, point, root, storage_kind(5, bank), 0);
    XrXirOp op = flow->rows[function].operations[point].op;
    if (!storage_execution(op)) {
        if (kind == XIR_STORAGE_PANIC) return storage_bank_state(flow, function, point, root, bank);
        flow->status = XR_XIR_BAD_STRUCTURE; return UINT32_MAX;
    }
    return storage_point(flow, function, point, root,
        storage_kind(kind == XIR_STORAGE_ERROR ? 2 : 4, bank), UINT32_MAX);
}

static bool storage_request_read(XirStorageFlow *flow, uint32_t function, StorageRead read) {
    StorageFunction *row = &flow->rows[function];
    if (!storage_reserve(flow, (void **)&row->reads, row->read_count,
        &row->read_capacity, sizeof(*row->reads))) return false;
    row->reads[row->read_count++] = read;
    return true;
}

static bool storage_record_write(XirStorageFlow *flow, uint32_t function, uint32_t point, uint32_t handle) {
    StorageFunction *row = &flow->rows[function];
    if (!storage_reserve(flow, (void **)&row->writes, row->write_count,
        &row->write_capacity, sizeof(*row->writes))) return false;
    row->writes[row->write_count++] = (StorageWrite){point, handle};
    return true;
}

static bool storage_record_call(XirStorageFlow *flow, uint32_t function, uint32_t point) {
    StorageFunction *row = &flow->rows[function];
    if (!storage_reserve(flow, (void **)&row->calls, row->call_count,
        &row->call_capacity, sizeof(*row->calls))) return false;
    row->calls[row->call_count++] = point;
    return true;
}

static bool storage_index_events(XirStorageFlow *flow, uint32_t function,
    const StorageWrite *events, uint32_t event_count, uint32_t **offsets_output, uint32_t **points_output) {
    StorageFunction *row = &flow->rows[function];
    uint32_t physical = flow->view.functions[function].physical_values;
    uint64_t offset_count = (uint64_t)physical + 1;
    uint32_t *offsets = NULL, *points = NULL;
    if (offset_count > SIZE_MAX / sizeof(*offsets)) { flow->status = XR_XIR_OVERFLOW; return false; }
    offsets = xir_compile_calloc(&flow->memory, (size_t)offset_count,
        sizeof(*offsets), &flow->status);
    if (!offsets) return false;
    for (uint32_t w = 0; w < event_count && storage_step(flow); ++w) {
        uint32_t handle = events[w].handle;
        if (!storage_is_handle(flow, function, handle)) continue;
        StorageAddress address;
        if (!storage_address(flow, function, handle, &address)) {
            xr_compile_resources_free(offsets); return false;
        }
        for (uint32_t a = 0; a < address.count && storage_step(flow); ++a) {
            uint32_t root = row->address_values[address.first + a];
            if (offsets[root + 1] == UINT32_MAX) {
                xr_compile_resources_free(offsets); flow->status = XR_XIR_OVERFLOW; return false;
            }
            ++offsets[root + 1];
        }
    }
    uint64_t total = 0;
    for (uint32_t v = 0; v < physical && storage_step(flow); ++v) {
        total += offsets[v + 1];
        if (total > UINT32_MAX || total > SIZE_MAX / sizeof(*points)) {
            xr_compile_resources_free(offsets); flow->status = XR_XIR_OVERFLOW; return false;
        }
        offsets[v + 1] = (uint32_t)total;
    }
    points = xir_compile_calloc(&flow->memory, (size_t)total,
        sizeof(*points), &flow->status);
    uint32_t *cursor = xir_compile_copy(&flow->memory, offsets,
        (size_t)physical * sizeof(*cursor), &flow->status);
    for (uint32_t w = 0; w < event_count && storage_step(flow); ++w) {
        uint32_t handle = events[w].handle;
        if (!storage_is_handle(flow, function, handle)) continue;
        StorageAddress address = row->addresses[handle];
        for (uint32_t a = 0; a < address.count && storage_step(flow); ++a) {
            uint32_t root = row->address_values[address.first + a];
            points[cursor[root]++] = events[w].point;
        }
    }
    xr_compile_resources_free(cursor);
    if (flow->status != XR_XIR_OK) {
        xr_compile_resources_free(offsets); xr_compile_resources_free(points); return false;
    }
    *offsets_output = offsets; *points_output = points;
    return true;
}


static bool storage_index_writes(XirStorageFlow *flow, uint32_t function) {
    StorageFunction *row = &flow->rows[function];
    row->events_ready = storage_index_events(flow, function, row->writes, row->write_count,
        &row->write_offsets, &row->write_points);
    return row->events_ready;
}

static bool storage_index_publications(XirStorageFlow *flow, uint32_t function) {
    StorageFunction *row = &flow->rows[function];
    if (!row->publication_count) return true;
    uint32_t points = row->body->instruction_count + 1;
    uint32_t *heads = xir_compile_calloc(&flow->memory, points, sizeof(*heads), &flow->status);
    uint32_t *next = xir_compile_calloc(&flow->memory, row->publication_count, sizeof(*next), &flow->status);
    StorageWrite *events = xir_compile_calloc(&flow->memory, row->publication_count, sizeof(*events), &flow->status);
    if (flow->status == XR_XIR_OK) {
        for (uint32_t p = 0; p < points; ++p) heads[p] = UINT32_MAX;
        for (uint32_t p = 0; p < row->publication_count && storage_step(flow); ++p) {
            StoragePublication event = row->publications[p];
            if (event.kind != XIR_STORAGE_BEFORE && event.kind != XIR_STORAGE_NORMAL) continue;
            uint32_t point = event.point + (event.kind == XIR_STORAGE_NORMAL);
            next[p] = heads[point]; heads[point] = p;
        }
        uint32_t count = 0;
        for (uint32_t point = 0; point < points && storage_step(flow); ++point)
            for (uint32_t p = heads[point]; p != UINT32_MAX && storage_step(flow); p = next[p])
                events[count++] = (StorageWrite){point, row->publications[p].handle};
        if (flow->status == XR_XIR_OK)
            storage_index_events(flow, function, events, count, &row->exposure_offsets, &row->exposure_points);
    }
    xr_compile_resources_free(heads); xr_compile_resources_free(next); xr_compile_resources_free(events);
    return flow->status == XR_XIR_OK;
}

static uint32_t storage_publication_hash(StoragePublication event) {
    uint32_t fields[] = {event.point, event.handle, event.condition_point, event.condition_handle,
        (uint32_t)event.kind, (uint32_t)event.condition_kind, event.conditional};
    return xr_hash_bytes(fields, sizeof(fields));
}

static bool storage_record_publication(XirStorageFlow *flow, uint32_t function, StoragePublication event) {
    StorageFunction *row = &flow->rows[function];
    if (!row->publication_index_capacity || row->publication_count >= row->publication_index_capacity / 2) {
        uint64_t next = row->publication_index_capacity ? (uint64_t)row->publication_index_capacity * 2 : 16;
        if (next > UINT32_MAX) { flow->status = XR_XIR_OVERFLOW; return false; }
        uint32_t capacity = (uint32_t)next;
        uint32_t *index = xir_compile_calloc(&flow->memory, capacity, sizeof(*index), &flow->status);
        if (!index) return false;
        for (uint32_t p = 0; p < row->publication_count && storage_step(flow); ++p) {
            uint32_t slot = storage_publication_hash(row->publications[p]) & (capacity - 1);
            while (index[slot]) slot = (slot + 1) & (capacity - 1);
            index[slot] = p + 1;
        }
        if (flow->status != XR_XIR_OK) { xr_compile_resources_free(index); return false; }
        xr_compile_resources_free(row->publication_index);
        row->publication_index = index; row->publication_index_capacity = capacity;
    }
    uint32_t slot = storage_publication_hash(event) & (row->publication_index_capacity - 1);
    while (row->publication_index[slot]) {
        StoragePublication found = row->publications[row->publication_index[slot] - 1];
        if (found.point == event.point && found.handle == event.handle && found.kind == event.kind &&
            found.conditional == event.conditional && found.condition_point == event.condition_point &&
            found.condition_handle == event.condition_handle && found.condition_kind == event.condition_kind) return true;
        slot = (slot + 1) & (row->publication_index_capacity - 1);
    }
    if (!storage_reserve(flow, (void **)&row->publications, row->publication_count,
        &row->publication_capacity, sizeof(*row->publications))) return false;
    row->publications[row->publication_count] = event; row->publication_index[slot] = ++row->publication_count;
    ++flow->revision;
    return true;
}

static bool storage_observe_values(XirStorageFlow *flow, uint32_t function, uint32_t i,
    const XrXirInstruction *op) {
    const XrXirFunction *body = flow->rows[function].body;
    uint32_t value = body->parameter_count + i;
    if (op->op == XR_XIR_CHANNEL_RECV_OR) {
        uint32_t count = op->type == XR_XIR_UNIT ? 1u : 2u;
        if (op->args[0] > body->operand_count || op->args[1] != count ||
            count > body->operand_count - op->args[0]) { flow->status = XR_XIR_BAD_STRUCTURE; return false; }
        if (count == 1) return true;
        return storage_request_read(flow, function, (StorageRead){body->operands[op->args[0] + 1], value, i,
            XIR_STORAGE_BEFORE, false});
    }
    if (storage_read(op->op) || (storage_copy(op->op) && !storage_is_handle(flow, function, value)))
        return storage_request_read(flow, function, (StorageRead){op->args[0], value, i,
            XIR_STORAGE_BEFORE, storage_read(op->op)});
    if (op->op == XR_XIR_PHI && !storage_is_handle(flow, function, value)) {
        if (op->args[0] > body->operand_count || op->args[1] > body->operand_count - op->args[0] ||
            op->args[1] % 2) { flow->status = XR_XIR_BAD_STRUCTURE; return false; }
        for (uint32_t p = 0; p < op->args[1]; p += 2) {
            uint32_t predecessor = body->operands[op->args[0] + p];
            if (predecessor >= body->block_count || !body->blocks[predecessor].count) {
                flow->status = XR_XIR_BAD_STRUCTURE; return false;
            }
            XrXirBlock block = body->blocks[predecessor];
            if (!storage_request_read(flow, function, (StorageRead){body->operands[op->args[0] + p + 1],
                value, block.first + block.count - 1, XIR_STORAGE_NORMAL, false})) return false;
        }
    }
    return flow->status == XR_XIR_OK;
}

static bool storage_transfer(XirStorageFlow *flow, uint32_t function, StorageState state,
    StoragePredecessor edge) {
    const XrXirFunction *body = flow->rows[function].body;
    const StorageOperation *op = &flow->rows[function].operations[edge.source];
    XirStorageFlowBank bank = storage_bank(state);
    if (bank == XIR_STORAGE_EXPOSURE && edge.kind != XIR_INSTRUCTION_FLOW_PANIC && storage_base_kind(state) != 5) {
        uint32_t source = storage_point(flow, function, edge.source, state.handle,
            storage_kind(5, bank), 0);
        return source != UINT32_MAX && storage_state_relation(flow, function, state, source);
    }
    bool preserve = true;
    if (edge.kind != XIR_INSTRUCTION_FLOW_PANIC && storage_new_place(op->op) &&
        state.handle == body->parameter_count + edge.source) {
        preserve = false;
        if (bank == XIR_STORAGE_EXPOSURE) return true;
        if (op->op == XR_XIR_LOCAL_UNINIT)
            return storage_unknown(flow, function, state.value, storage_payload(flow, function, state.handle));
        if (storage_payload(flow, function, state.handle) != XR_XIR_UNIT &&
            !storage_relation(flow, function, op->source, state.value)) return false;
    } else if (bank == XIR_STORAGE_CONTENTS && edge.kind != XIR_INSTRUCTION_FLOW_PANIC && storage_write(op->op)) {
        StorageAddress address;
        if (!storage_address(flow, function, op->destination, &address)) return false;
        for (uint32_t a = 0; a < address.count; ++a) {
            if (flow->rows[function].address_values[address.first + a] != state.handle) continue;
            if (storage_payload(flow, function, state.handle) != XR_XIR_UNIT &&
                !storage_relation(flow, function, op->source, state.value)) return false;
            preserve = address.count != 1;
            break;
        }
    } else if (bank == XIR_STORAGE_CONTENTS && edge.kind != XIR_INSTRUCTION_FLOW_PANIC &&
        (op->op == XR_XIR_CELL_TAKE || op->op == XR_XIR_VALUE_TAKE) &&
        storage_is_handle(flow, function, op->destination)) {
        StorageAddress address;
        if (!storage_address(flow, function, op->destination, &address)) return false;
        for (uint32_t a = 0; a < address.count; ++a) {
            if (flow->rows[function].address_values[address.first + a] != state.handle) continue;
            if (!storage_unknown(flow, function, state.value, storage_payload(flow, function, state.handle))) return false;
            preserve = address.count != 1;
            break;
        }
    }
    if (!preserve) return true;
    uint32_t source;
    if (edge.kind == XIR_INSTRUCTION_FLOW_PANIC)
        source = storage_point(flow, function, flow->rows[function].blocks[edge.source], state.handle,
            storage_kind(3, bank), UINT32_MAX);
    else if (storage_execution(op->op))
        source = storage_point(flow, function, edge.source, state.handle,
            storage_kind(edge.ordinal || op->op == XR_XIR_CLEANUP_ERROR || op->op == XR_XIR_THROW ? 2 : 1, bank), UINT32_MAX);
    else source = storage_bank_state(flow, function, edge.source, state.handle, bank);
    return source != UINT32_MAX && storage_state_relation(flow, function, state, source);
}

static bool storage_drain(XirStorageFlow *flow) {
    while (flow->work_first < flow->work_count && storage_step(flow)) {
        StorageWork item = flow->work[flow->work_first++];
        StorageFunction *row = &flow->rows[item.function];
        StorageState state = row->states[item.state];
        XirStorageFlowBank bank = storage_bank(state);
        if (storage_base_kind(state) == 5) {
            StorageAddress address;
            if (!storage_address(flow, item.function, state.handle, &address)) return false;
            if (address.count > 1) {
                for (uint32_t a = 0; a < address.count && storage_step(flow); ++a) {
                    uint32_t root = row->address_values[address.first + a];
                    uint32_t source = storage_point(flow, item.function, state.point, root, state.kind, 0);
                    if (source == UINT32_MAX || !storage_state_relation(flow, item.function, state, source)) return false;
                }
                continue;
            }
            StoragePredecessor edge = {state.point, UINT32_MAX, 0, XIR_INSTRUCTION_FLOW_NEXT};
            if (!storage_transfer(flow, item.function, state, edge)) return false;
            continue;
        }
        if (storage_base_kind(state)) continue;
        StorageAddress address;
        if (!storage_address(flow, item.function, state.handle, &address)) return false;
        if (address.count > 1) {
            for (uint32_t a = 0; a < address.count && storage_step(flow); ++a) {
                uint32_t root = row->address_values[address.first + a];
                uint32_t source = storage_bank_state(flow, item.function, state.point, root, bank);
                if (source == UINT32_MAX || !storage_state_relation(flow, item.function, state, source)) return false;
            }
            continue;
        }
        if (!state.point) {
            const uint32_t *inputs = bank == XIR_STORAGE_EXPOSURE ? row->exposure_inputs : row->content_inputs;
            if (state.handle < row->body->parameter_count && inputs[state.handle] != UINT32_MAX) {
                if (!storage_state_relation(flow, item.function, state, inputs[state.handle])) return false;
            } else if (state.handle < row->body->parameter_count ||
                !storage_new_place(row->operations[state.handle - row->body->parameter_count].op)) {
                if (bank == XIR_STORAGE_EXPOSURE) {
                    if (!storage_exposure_seed(flow, item.function, state.value, UINT32_MAX)) return false;
                } else if (!storage_unknown(flow, item.function, state.value,
                    storage_payload(flow, item.function, state.handle))) return false;
            }
        }
        for (uint32_t e = row->predecessors[state.point]; e != UINT32_MAX && storage_step(flow); e = row->edges[e].next)
            if (!storage_transfer(flow, item.function, state, row->edges[e])) return false;
    }
    return flow->status == XR_XIR_OK;
}

static bool storage_binding(XirStorageFlow *flow, XirStorageBinding binding) {
    void *values = (void *)flow->view.bindings;
    if (!storage_reserve(flow, &values, flow->view.binding_count, &flow->binding_capacity,
        sizeof(*flow->view.bindings))) return false;
    flow->view.bindings = values;
    ((XirStorageBinding *)values)[flow->view.binding_count++] = binding;
    return true;
}

static uint32_t storage_application(XirStorageFlow *flow, const XirExecutionSite *site, XirExecutionPort outcome) {
    if (flow->view.application_count >= UINT32_MAX - 1) {
        flow->status = XR_XIR_OVERFLOW; return UINT32_MAX;
    }
    void *values = (void *)flow->view.applications;
    if (!storage_reserve(flow, &values, flow->view.application_count, &flow->application_capacity,
        sizeof(*flow->view.applications))) return UINT32_MAX;
    flow->view.applications = values;
    uint32_t index = flow->view.application_count++;
    ((XirStorageApplication *)values)[index] = (XirStorageApplication){
        .kind = site->kind, .site = site->index, .function = site->function,
        .instruction = site->instruction, .block = site->block, .declaration = site->declaration,
        .target = site->target, .caller_instance = site->caller_instance,
        .reference = site->reference, .capture_environment = site->capture_environment,
        .environment = site->environment, .order = site->order, .port = site->port,
        .outcome = outcome, .protocol = site->protocol};
    return index;
}

static uint32_t storage_site_hash(XirExecutionSite site) {
    uint32_t fields[] = {(uint32_t)site.kind, site.index, site.function, site.instruction, site.block,
        site.caller_instance, site.environment, site.reference, site.capture_environment,
        site.target, site.declaration, site.order, (uint32_t)site.port};
    return xr_hash_bytes(fields, sizeof(fields));
}

static bool storage_same_site(XirExecutionSite a, XirExecutionSite b) {
    return a.kind == b.kind && a.index == b.index && a.function == b.function && a.instruction == b.instruction &&
        a.block == b.block && a.caller_instance == b.caller_instance && a.environment == b.environment &&
        a.reference == b.reference && a.capture_environment == b.capture_environment && a.target == b.target &&
        a.declaration == b.declaration && a.order == b.order && a.port == b.port;
}

static bool storage_site_index_grow(XirStorageFlow *flow) {
    uint64_t next = flow->site_index_capacity ? (uint64_t)flow->site_index_capacity * 2u : 16u;
    if (next > UINT32_MAX) { flow->status = XR_XIR_OVERFLOW; return false; }
    uint32_t capacity = (uint32_t)next;
    uint32_t *index = xir_compile_calloc(&flow->memory, capacity, sizeof(*index), &flow->status);
    if (!index) return false;
    for (uint32_t n = 0; n < flow->site_count && storage_step(flow); ++n) {
        XirExecutionSite site = flow->sites[n].site;
        uint32_t slot = storage_site_hash(site) & (capacity - 1);
        while (index[slot]) slot = (slot + 1) & (capacity - 1);
        index[slot] = n + 1;
    }
    if (flow->status != XR_XIR_OK) { xr_compile_resources_free(index); return false; }
    xr_compile_resources_free(flow->site_index); flow->site_index = index; flow->site_index_capacity = capacity;
    return true;
}

static bool storage_environment_site(const XirExecutionSite *site) {
    return site->kind != XIR_EXECUTION_CLEANUP_USE && (site->protocol == 1 || site->protocol == 4);
}

static XrXirStatus storage_import_site(void *opaque, const XirExecutionSite *site) {
    XirStorageFlow *flow = opaque;
    if (!storage_step(flow)) return flow->status;
    if (site->function >= flow->view.function_count) return flow->status = XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *body = flow->rows[site->function].body;
    if (site->order >= flow->rows[site->function].point_count) return flow->status = XR_XIR_BAD_STRUCTURE;
    if ((site->instruction == UINT32_MAX && (site->kind != XIR_EXECUTION_CLEANUP_USE ||
        site->port != XIR_EXECUTION_PANIC || site->block >= body->block_count)) ||
        (site->instruction != UINT32_MAX && site->instruction >= body->instruction_count))
        return flow->status = XR_XIR_BAD_STRUCTURE;
    if (!flow->site_index_capacity || flow->site_count >= flow->site_index_capacity / 2)
        if (!storage_site_index_grow(flow)) return flow->status;
    uint32_t slot = storage_site_hash(*site) & (flow->site_index_capacity - 1);
    while (flow->site_index[slot]) {
        uint32_t index = flow->site_index[slot] - 1;
        if (storage_same_site(flow->sites[index].site, *site)) {
            XirExecutionSite previous = flow->sites[index].site;
            if (previous.protocol != site->protocol || previous.argument_count != site->argument_count ||
                previous.capture_count != site->capture_count || previous.native_callable != site->native_callable ||
                previous.unresolved != site->unresolved)
                return flow->status = XR_XIR_BAD_STRUCTURE;
            flow->sites[index].site = *site;
            flow->sites[index].active = true;
            return XR_XIR_OK;
        }
        slot = (slot + 1) & (flow->site_index_capacity - 1);
    }
    if (!storage_reserve(flow, (void **)&flow->sites, flow->site_count,
        &flow->site_capacity, sizeof(*flow->sites))) return flow->status;
    uint32_t index = flow->site_count++;
    flow->sites[index] = (StorageExecution){.site = *site,
        .applications = {UINT32_MAX, UINT32_MAX, UINT32_MAX}, .next = UINT32_MAX, .active = true};
    flow->site_index[slot] = index + 1;
    if (!site->unresolved && !site->native_callable && site->declaration < flow->view.function_count) {
        uint32_t outcomes = storage_environment_site(site) ? 1 : 3;
        for (uint32_t o = 0; o < outcomes; ++o) {
            uint32_t application = storage_application(flow, site, (XirExecutionPort)o);
            if (application == UINT32_MAX) return flow->status;
            flow->sites[index].applications[o] = application;
        }
        flow->sites[index].identity_parameters = xir_compile_calloc(&flow->memory, site->argument_count,
            sizeof(*flow->sites[index].identity_parameters), &flow->status);
        if (flow->status != XR_XIR_OK) return flow->status;
    }
    ++flow->generation;
    if (!flow->generation) return flow->status = XR_XIR_OVERFLOW;
    return flow->status;
}

static uint32_t storage_call_input_bank(XirStorageFlow *flow, const XirExecutionSite *site,
    uint32_t handle, XirStorageFlowBank bank) {
    StorageAddress address;
    if (!storage_address(flow, site->function, handle, &address)) return UINT32_MAX;
    uint32_t root = address.count == 1 ? flow->rows[site->function].address_values[address.first] : handle;
    if (site->kind != XIR_EXECUTION_CLEANUP_USE)
        return storage_bank_state(flow, site->function, site->instruction, root, bank);
    uint32_t point = site->instruction == UINT32_MAX ? site->block : site->instruction;
    return storage_point(flow, site->function, point, root, storage_kind((uint32_t)site->port + 1, bank), site->order);
}

static uint32_t storage_call_input(XirStorageFlow *flow, const XirExecutionSite *site, uint32_t handle) {
    return storage_call_input_bank(flow, site, handle, XIR_STORAGE_CONTENTS);
}

static bool storage_bindings_equal(XirStorageBinding a, XirStorageBinding b) {
    return a.formal_function == b.formal_function && a.formal_value == b.formal_value &&
        a.actual_function == b.actual_function && a.actual_value == b.actual_value &&
        a.actual_environment == b.actual_environment;
}

static bool storage_bound_identity(XirStorageFlow *flow, uint32_t site_index,
    uint32_t parameter, XirExecutionArgument actual, XirExecutionPort outcome) {
    StorageExecution record = flow->sites[site_index];
    uint8_t mask = (uint8_t)(1u << outcome);
    if (record.identity_parameters[parameter] & mask) return true;
    void *values = (void *)flow->view.identities;
    if (!storage_reserve(flow, &values, flow->view.identity_count, &flow->identity_capacity,
        sizeof(*flow->view.identities))) return false;
    flow->view.identities = values;
    ((XirStorageIdentity *)values)[flow->view.identity_count++] = (XirStorageIdentity){
        actual.function, actual.value, record.site.declaration, parameter, record.applications[outcome]};
    flow->sites[site_index].identity_parameters[parameter] |= mask;
    return true;
}

static bool storage_bind_application(XirStorageFlow *flow, uint32_t site_index, XirExecutionPort outcome) {
    StorageExecution record = flow->sites[site_index];
    uint32_t application_index = record.applications[outcome];
    if (application_index == UINT32_MAX) return true;
    XirExecutionSite site = record.site;
    const XrXirFunction *callee = flow->rows[site.declaration].body;
    uint32_t first = flow->view.binding_count;
    bool complete = true;
    if (site.argument_count > callee->parameter_count) { flow->status = XR_XIR_BAD_STRUCTURE; return false; }
    for (uint32_t p = 0; p < site.argument_count && storage_step(flow); ++p) {
        XirExecutionArgument actual = {0};
        XrXirStatus status = xir_execution_argument(flow->execution, &site, p, &actual);
        if (status == XR_XIR_UNRESOLVED) { complete = false; continue; }
        if (status != XR_XIR_OK) { flow->status = status; return false; }
        if (actual.function >= flow->view.function_count) {
            flow->status = XR_XIR_BAD_STRUCTURE; return false;
        }
        if (actual.value >= flow->view.functions[actual.function].physical_values) {
            complete = false; continue;
        }
        if (!storage_binding(flow, (XirStorageBinding){site.declaration, p,
            actual.function, actual.value, actual.environment})) return false;
        if (!actual.cell) continue;
        if (!storage_bound_identity(flow, site_index, p, actual, outcome)) return false;
        /* A function reference captures the Cell identity. Its contents are
         * observed when the closure is called or published, never here. */
        if (storage_environment_site(&site)) continue;
        uint32_t handle = actual.caller_handle;
        if (handle == UINT32_MAX && actual.function == site.function && actual.environment == UINT32_MAX)
            handle = actual.value;
        if (handle == UINT32_MAX) { complete = false; continue; }
        if (handle >= flow->view.functions[site.function].physical_values ||
            !storage_is_handle(flow, site.function, handle)) { flow->status = XR_XIR_BAD_STRUCTURE; return false; }
        uint32_t formal = flow->rows[site.declaration].content_inputs[p];
        if (formal == UINT32_MAX) { complete = false; continue; }
        uint32_t contents = storage_call_input(flow, &site, handle);
        if (formal == UINT32_MAX || contents == UINT32_MAX || !storage_binding(flow,
            (XirStorageBinding){site.declaration, formal, site.function, contents, UINT32_MAX})) return false;
        if (flow->exposure_started && p < site.capture_count) {
            formal = flow->rows[site.declaration].exposure_inputs[p];
            uint32_t exposure = storage_call_input_bank(flow, &site, handle, XIR_STORAGE_EXPOSURE);
            if (formal == UINT32_MAX || exposure == UINT32_MAX || !storage_binding(flow,
                (XirStorageBinding){site.declaration, formal, site.function, exposure, UINT32_MAX})) return false;
        }
    }
    if (flow->status != XR_XIR_OK) return false;
    XirStorageApplication previous = flow->view.applications[application_index];
    uint32_t count = flow->view.binding_count - first;
    bool same = previous.count == count;
    for (uint32_t b = 0; same && b < count; ++b)
        same = storage_bindings_equal(flow->view.bindings[first + b], flow->view.bindings[previous.first + b]);
    if (same) flow->view.binding_count = first;
    else {
        XirStorageApplication *application = (XirStorageApplication *)&flow->view.applications[application_index];
        application->first = first; application->count = count;
        if (++flow->generation == 0) { flow->status = XR_XIR_OVERFLOW; return false; }
    }
    flow->sites[site_index].bindings_ready = complete;
    return true;
}

static bool storage_reindex_sites(XirStorageFlow *flow) {
    for (uint32_t f = 0; f < flow->view.function_count && storage_step(flow); ++f) {
        StorageFunction *row = &flow->rows[f];
        for (uint32_t p = 0; p < row->site_head_count; ++p) row->site_heads[p] = UINT32_MAX;
    }
    for (uint32_t s = 0; s < flow->site_count && storage_step(flow); ++s) {
        XirExecutionSite site = flow->sites[s].site;
        if (!flow->sites[s].active || storage_environment_site(&site)) continue;
        StorageFunction *row = &flow->rows[site.function];
        uint32_t point = site.instruction == UINT32_MAX ? row->body->instruction_count + site.block : site.instruction;
        flow->sites[s].next = row->site_heads[point]; row->site_heads[point] = s;
    }
    return flow->status == XR_XIR_OK;
}

static uint32_t storage_state_sites(const XirStorageFlow *flow, uint32_t function, StorageState state) {
    const StorageFunction *row = &flow->rows[function];
    uint32_t point = storage_base_kind(state) == 3 ? row->body->instruction_count + state.point : state.point;
    return row->site_heads[point];
}

static XirExecutionPort storage_activation(const XirStorageFlow *flow, uint32_t function, StorageState state) {
    if (storage_base_kind(state) == 3) return XIR_EXECUTION_PANIC;
    XrXirOp op = flow->rows[function].operations[state.point].op;
    return op == XR_XIR_CLEANUP_ERROR || op == XR_XIR_THROW ? XIR_EXECUTION_ERROR : XIR_EXECUTION_NORMAL;
}

static bool storage_site_port(const XirStorageFlow *flow, uint32_t function,
    const XirExecutionSite *site, StorageState state) {
    return site->kind != XIR_EXECUTION_CLEANUP_USE || site->port == storage_activation(flow, function, state);
}

static uint32_t storage_stage_count(const XirStorageFlow *flow, uint32_t function, StorageState state) {
    bool cleanup = storage_base_kind(state) == 3;
    if (!cleanup) {
        XrXirOp op = flow->rows[function].operations[state.point].op;
        cleanup = op == XR_XIR_CLEANUP_LEAVE || op == XR_XIR_CLEANUP_ERROR ||
            op == XR_XIR_RETURN || op == XR_XIR_THROW;
    }
    uint32_t count = cleanup ? 0 : 1;
    for (uint32_t s = storage_state_sites(flow, function, state); s != UINT32_MAX; s = flow->sites[s].next) {
        XirExecutionSite site = flow->sites[s].site;
        if (storage_site_port(flow, function, &site, state) && site.order >= count) count = site.order + 1;
    }
    return count;
}

static bool storage_argument_matches(XirStorageFlow *flow, const XirExecutionSite *site,
    XirExecutionArgument actual, uint32_t root, bool *exact) {
    *exact = false;
    uint32_t handle = actual.caller_handle;
    if (handle == UINT32_MAX && actual.function == site->function && actual.environment == UINT32_MAX)
        handle = actual.value;
    if (handle == UINT32_MAX) {
        /* A foreign environment keeps its symbolic application. Without a
         * current-frame representative it may alias an external Cell root. */
        const XrXirFunction *body = flow->rows[site->function].body;
        if (actual.environment != UINT32_MAX && actual.environment >= flow->view.application_count) return true;
        if (actual.function == site->function && actual.value == root) return true;
        if (root < body->parameter_count) return true;
        return !storage_new_place(flow->rows[site->function].operations[root - body->parameter_count].op);
    }
    StorageAddress address;
    if (!storage_address(flow, site->function, handle, &address)) return false;
    for (uint32_t a = 0; a < address.count; ++a)
        if (flow->rows[site->function].address_values[address.first + a] == root) {
            *exact = address.count == 1; return true;
        }
    return false;
}

static bool storage_apply_known(XirStorageFlow *flow, uint32_t site_index, uint32_t function,
    StorageState state, uint32_t input, XirExecutionPort outcome) {
    StorageExecution record = flow->sites[site_index];
    XirExecutionSite site = record.site;
    const XrXirFunction *callee = flow->rows[site.declaration].body;
    XirStorageFlowBank bank = storage_bank(state);
    bool affected = false, exact = record.bindings_ready;
    for (uint32_t p = 0; p < site.argument_count && storage_step(flow); ++p) {
        XirExecutionArgument actual = {0};
        XrXirStatus status = xir_execution_argument(flow->execution, &site, p, &actual);
        if (status == XR_XIR_UNRESOLVED) { exact = false; continue; }
        if (status != XR_XIR_OK) { flow->status = status; return false; }
        if (!actual.cell) continue;
        /* Ordinary ref formals require storable contents. Their exposure bound
         * is a local write contract, not publication of the caller's Cell. */
        if (bank == XIR_STORAGE_EXPOSURE && p >= site.capture_count) continue;
        bool match_exact;
        bool matches = storage_argument_matches(flow, &site, actual, state.handle, &match_exact);
        if (flow->status != XR_XIR_OK) return false;
        if (!matches) continue;
        affected = true; exact &= match_exact;
        uint32_t exit = callee->instruction_count + (uint32_t)outcome;
        uint32_t source = storage_bank_state(flow, site.declaration, exit, p, bank);
        if (source == UINT32_MAX || !storage_cross(flow, (XirStorageCrossFlow){
            site.declaration, source, function, state.value, record.applications[outcome], bank})) return false;
    }
    return affected && exact ? true : storage_state_relation(flow, function, state, input);
}

typedef struct StorageOpaqueCapture {
    XirStorageFlow *flow;
    XirExecutionSite use;
    uint32_t target, handle;
    bool reachable, publish;
} StorageOpaqueCapture;

static XrXirStatus storage_opaque_capture(void *opaque, const XirExecutionSite *capture) {
    StorageOpaqueCapture *query = opaque;
    XirStorageFlow *flow = query->flow;
    for (uint32_t p = 0; p < capture->argument_count && storage_step(flow); ++p) {
        XirExecutionArgument actual = {0};
        XrXirStatus status = xir_execution_argument(flow->execution, capture, p, &actual);
        if (status == XR_XIR_UNRESOLVED && !query->publish) { query->reachable = true; continue; }
        if (status != XR_XIR_OK) return flow->status = status;
        if (actual.cell) {
            XirExecutionSite use = query->use;
            if (capture->function != use.function) actual.caller_handle = UINT32_MAX;
            bool exact;
            if (storage_argument_matches(flow, &use, actual, query->handle, &exact)) query->reachable = true;
            if (flow->status != XR_XIR_OK) return flow->status;
            if (!query->publish) continue;
            uint32_t handle = actual.caller_handle;
            if (handle == UINT32_MAX && actual.function == use.function && actual.environment == UINT32_MAX)
                handle = actual.value;
            if (handle == UINT32_MAX) return flow->status = XR_XIR_UNRESOLVED;
            uint32_t contents = storage_call_input(flow, &use, handle);
            if (contents == UINT32_MAX || !storage_relation(flow, use.function, contents, query->target))
                return flow->status;
        } else if (query->publish) {
            if (actual.environment != UINT32_MAX && actual.environment >= flow->view.application_count)
                return flow->status = XR_XIR_UNRESOLVED;
            if (!storage_cross(flow, (XirStorageCrossFlow){actual.function, actual.value,
                query->use.function, query->target, actual.environment, XIR_STORAGE_CONTENTS})) return flow->status;
        }
    }
    return flow->status;
}

static bool storage_observe_opaque_value(XirStorageFlow *flow, uint32_t function,
    uint32_t value, StorageOpaqueCapture *query) {
    bool complete = false;
    XrXirStatus status = xir_execution_visit_value_captures(flow->execution, function, value,
        storage_opaque_capture, query, &complete);
    if (status != XR_XIR_OK) { flow->status = status; return false; }
    return true;
}

static bool storage_opaque_environment(XirStorageFlow *flow, const XirExecutionSite *site,
    StorageOpaqueCapture *query, bool *affected) {
    for (uint32_t p = 0; p < site->argument_count && storage_step(flow); ++p) {
        XirExecutionArgument actual = {0};
        XrXirStatus status = xir_execution_argument(flow->execution, site, p, &actual);
        if (status == XR_XIR_UNRESOLVED) continue;
        if (status != XR_XIR_OK) { flow->status = status; return false; }
        if (actual.cell) {
            bool exact;
            if (storage_argument_matches(flow, site, actual, query->handle, &exact)) *affected = true;
            if (flow->status != XR_XIR_OK) return false;
            if (query->publish) {
                uint32_t handle = actual.caller_handle;
                if (handle == UINT32_MAX && actual.function == site->function && actual.environment == UINT32_MAX)
                    handle = actual.value;
                if (handle == UINT32_MAX) { flow->status = XR_XIR_UNRESOLVED; return false; }
                uint32_t contents = storage_call_input(flow, site, handle);
                if (contents == UINT32_MAX || !storage_relation(flow, site->function, contents, query->target)) return false;
            }
        } else if (xr_xir_callable_signature(xir_execution_types(flow->execution), actual.type)) {
            if (!storage_observe_opaque_value(flow, actual.function, actual.value, query)) return false;
        }
    }
    if (site->instruction != UINT32_MAX) {
        const StorageOperation *op = &flow->rows[site->function].operations[site->instruction];
        if (op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_INVOKE_INDIRECT)
            if (!storage_observe_opaque_value(flow, site->function, op->receiver, query)) return false;
    }
    *affected |= query->reachable;
    return flow->status == XR_XIR_OK;
}

static bool storage_apply_opaque(XirStorageFlow *flow, const XirExecutionSite *site,
    uint32_t function, StorageState state, uint32_t input) {
    bool terminal = site->native_callable ||
        (site->remainder & (XIR_EXECUTION_REMAINDER_PARAMETER | XIR_EXECUTION_REMAINDER_OPAQUE));
    if (storage_bank(state) == XIR_STORAGE_EXPOSURE) {
        if (!terminal) { flow->status = XR_XIR_UNRESOLVED; return false; }
        if (!storage_state_relation(flow, function, state, input)) return false;
        /* A native or opaque body may retain any reachable owned environment.
         * Ref actuals remain scoped and do not publish the referenced identity. */
        StorageOpaqueCapture exposure = {.flow=flow, .use=*site, .target=state.value, .handle=state.handle};
        for (uint32_t p = 0; p < site->argument_count && storage_step(flow); ++p) {
            XirExecutionArgument actual = {0};
            XrXirStatus status = xir_execution_argument(flow->execution, site, p, &actual);
            if (status != XR_XIR_OK) { flow->status = status; return false; }
            if (!actual.cell && actual.escaping &&
                xr_xir_callable_signature(xir_execution_types(flow->execution), actual.type))
                if (!storage_observe_opaque_value(flow, actual.function, actual.value, &exposure)) return false;
        }
        return !exposure.reachable || storage_exposure_seed(flow, function, state.value, UINT32_MAX);
    }
    if (!terminal && !flow->sealing) return true;
    const XrXirFunction *body = flow->rows[function].body;
    bool affected = !site->native_callable && state.handle < body->parameter_count;
    if (state.handle >= body->parameter_count)
        affected = !site->native_callable && !storage_new_place(flow->rows[function].operations[
            state.handle - body->parameter_count].op);
    StorageOpaqueCapture query = {.flow=flow, .use=*site, .target=state.value, .handle=state.handle};
    if (!storage_opaque_environment(flow, site, &query, &affected)) return false;
    if (affected) {
        query.publish = true;
        if (!storage_opaque_environment(flow, site, &query, &affected)) return false;
    }
    if (!storage_relation(flow, function, input, state.value)) return false;
    if (affected && !storage_unknown(flow, function, state.value,
        storage_payload(flow, function, state.handle))) return false;
    return flow->status == XR_XIR_OK;
}

static bool storage_panic_join(XirStorageFlow *flow, uint32_t function, StorageState state) {
    StorageFunction *row = &flow->rows[function];
    XrXirBlock block = row->body->blocks[state.point];
    uint32_t end = block.first + block.count;
    XirStorageFlowBank bank = storage_bank(state);
    uint32_t source = storage_bank_state(flow, function, block.first, state.handle, bank);
    if (source == UINT32_MAX || !storage_state_relation(flow, function, state, source)) return false;
    uint32_t first = row->write_offsets[state.handle], count = row->write_offsets[state.handle + 1] - first;
    for (uint32_t w = storage_lower(row->write_points, first, count, block.first);
        w < first + count && row->write_points[w] < end && storage_step(flow); ++w) {
        uint32_t point = row->write_points[w] + 1;
        if (point == end) continue;
        source = storage_bank_state(flow, function, point, state.handle, bank);
        if (source == UINT32_MAX || !storage_state_relation(flow, function, state, source)) return false;
    }
    for (uint32_t c = storage_lower(row->calls, 0, row->call_count, block.first);
        c < row->call_count && row->calls[c] < end && storage_step(flow); ++c) {
        uint32_t point = row->calls[c];
        source = storage_point(flow, function, point, state.handle, storage_kind(4, bank), UINT32_MAX);
        if (source == UINT32_MAX || !storage_state_relation(flow, function, state, source)) return false;
        if (point + 1 == end) continue;
        source = storage_bank_state(flow, function, point + 1, state.handle, bank);
        if (source == UINT32_MAX || !storage_state_relation(flow, function, state, source)) return false;
    }
    if (bank == XIR_STORAGE_EXPOSURE && row->exposure_offsets) {
        first = row->exposure_offsets[state.handle]; count = row->exposure_offsets[state.handle + 1] - first;
        for (uint32_t p = storage_lower(row->exposure_points, first, count, block.first);
            p < first + count && row->exposure_points[p] < end && storage_step(flow); ++p) {
            source = storage_bank_state(flow, function, row->exposure_points[p], state.handle, bank);
            if (source == UINT32_MAX || !storage_state_relation(flow, function, state, source)) return false;
        }
    }
    return flow->status == XR_XIR_OK;
}

static bool storage_process_stage(XirStorageFlow *flow, uint32_t function, uint32_t state_index) {
    StorageState state = flow->rows[function].states[state_index];
    uint32_t kind = storage_base_kind(state);
    XirStorageFlowBank bank = storage_bank(state);
    if (!kind || kind == 5 || state.generation == flow->generation) return true;
    uint32_t generation = flow->generation;
    StorageAddress address;
    if (!storage_address(flow, function, state.handle, &address)) return false;
    if (address.count > 1) {
        for (uint32_t a = 0; a < address.count && storage_step(flow); ++a) {
            uint32_t root = flow->rows[function].address_values[address.first + a];
            uint32_t source = storage_point(flow, function, state.point, root, state.kind, state.order);
            if (source == UINT32_MAX || !storage_state_relation(flow, function, state, source)) return false;
        }
        flow->rows[function].states[state_index].generation = generation;
        return flow->status == XR_XIR_OK;
    }
    if (state.order == UINT32_MAX) {
        uint32_t count = storage_stage_count(flow, function, state);
        if (kind == 4) {
            uint32_t prefix = (uint32_t)storage_activation(flow, function, state) + 1;
            uint32_t source = storage_point(flow, function, state.point, state.handle, storage_kind(prefix, bank), 0);
            if (source == UINT32_MAX || !storage_state_relation(flow, function, state, source)) return false;
            for (uint32_t order = 1; order <= count && storage_step(flow); ++order) {
                source = storage_point(flow, function, state.point, state.handle, storage_kind(4, bank), order);
                if (source == UINT32_MAX || !storage_state_relation(flow, function, state, source)) return false;
            }
        } else {
            uint32_t source = storage_point(flow, function, state.point, state.handle, state.kind, count);
            if (source == UINT32_MAX || !storage_state_relation(flow, function, state, source)) return false;
        }
    } else if (!state.order) {
        if (kind == 3) {
            if (!storage_panic_join(flow, function, state)) return false;
        } else {
            uint32_t source = storage_bank_state(flow, function, state.point, state.handle, bank);
            if (source == UINT32_MAX || !storage_state_relation(flow, function, state, source)) return false;
        }
    } else {
        uint32_t prefix = kind == 4 ? (uint32_t)storage_activation(flow, function, state) + 1 : kind;
        uint32_t input = storage_point(flow, function, state.point, state.handle, storage_kind(prefix, bank), state.order - 1);
        if (input == UINT32_MAX) return false;
        for (uint32_t s = storage_state_sites(flow, function, state); s != UINT32_MAX; s = flow->sites[s].next) {
            StorageExecution record = flow->sites[s];
            if (!storage_site_port(flow, function, &record.site, state) || record.site.order != state.order - 1) continue;
            if (record.applications[0] == UINT32_MAX) {
                if (!storage_apply_opaque(flow, &record.site, function, state, input)) return false;
            } else {
                XirExecutionPort outcome = kind == 4 ? XIR_EXECUTION_PANIC :
                    kind == 2 ? XIR_EXECUTION_ERROR : XIR_EXECUTION_NORMAL;
                if (!storage_apply_known(flow, s, function, state, input, outcome)) return false;
                if (record.site.kind == XIR_EXECUTION_CLEANUP_USE && kind != 4) {
                    if (outcome != XIR_EXECUTION_NORMAL &&
                        !storage_apply_known(flow, s, function, state, input, XIR_EXECUTION_NORMAL)) return false;
                    if (outcome != XIR_EXECUTION_ERROR &&
                        !storage_apply_known(flow, s, function, state, input, XIR_EXECUTION_ERROR)) return false;
                    if (kind == 3 &&
                        !storage_apply_known(flow, s, function, state, input, XIR_EXECUTION_PANIC)) return false;
                }
            }
        }
    }
    flow->rows[function].states[state_index].generation = generation;
    return flow->status == XR_XIR_OK;
}

static bool storage_prepare_function(XirStorageFlow *flow, uint32_t function) {
    StorageFunction *row = &flow->rows[function];
    const XrXirFunction *body = &flow->module->functions[function];
    uint64_t physical = (uint64_t)body->parameter_count + body->instruction_count;
    uint64_t points = (uint64_t)body->instruction_count + 3;
    uint64_t heads = (uint64_t)body->instruction_count + body->block_count;
    if (physical > UINT32_MAX || points > UINT32_MAX || heads > UINT32_MAX ||
        points > SIZE_MAX / sizeof(*row->predecessors) || heads > SIZE_MAX / sizeof(*row->site_heads) ||
        (body->parameter_count && !body->parameters) || (body->operand_count && !body->operands) ||
        (body->block_count && !body->blocks) || (body->instruction_count && !body->block_count)) {
        flow->status = XR_XIR_BAD_STRUCTURE; return false;
    }
    row->header = *body;
    row->header.name = NULL; row->header.name_length = 0; row->header.instructions = NULL;
    row->header.consume_parameters = NULL; row->header.consume_parameter_count = 0;
    row->header.escaping_parameters = NULL; row->header.escaping_parameter_count = 0;
    row->types = xir_compile_calloc(&flow->memory, (size_t)physical, sizeof(*row->types), &flow->status);
    row->handles = xir_compile_calloc(&flow->memory, (size_t)physical, sizeof(*row->handles), &flow->status);
    row->operations = xir_compile_calloc(&flow->memory, body->instruction_count, sizeof(*row->operations), &flow->status);
    row->content_inputs = xir_compile_calloc(&flow->memory, body->parameter_count,
        sizeof(*row->content_inputs), &flow->status);
    row->exposure_inputs = xir_compile_calloc(&flow->memory, body->parameter_count,
        sizeof(*row->exposure_inputs), &flow->status);
    row->header.blocks = xir_compile_copy(&flow->memory, body->blocks,
        (size_t)body->block_count * sizeof(*body->blocks), &flow->status);
    row->header.parameters = row->types;
    row->body = &row->header;
    row->point_count = (uint32_t)points; row->site_head_count = (uint32_t)heads;
    XirStorageFlowFunction *view = (XirStorageFlowFunction *)&flow->view.functions[function];
    view->physical_values = (uint32_t)physical; view->value_count = (uint32_t)physical;
    view->parameter_count = body->parameter_count;
    row->blocks = xir_compile_calloc(&flow->memory, body->instruction_count, sizeof(*row->blocks), &flow->status);
    row->predecessors = xir_compile_calloc(&flow->memory, row->point_count,
        sizeof(*row->predecessors), &flow->status);
    row->identity_heads = xir_compile_calloc(&flow->memory, (size_t)physical, sizeof(*row->identity_heads), &flow->status);
    row->addresses = xir_compile_calloc(&flow->memory, (size_t)physical, sizeof(*row->addresses), &flow->status);
    row->visited = xir_compile_calloc(&flow->memory, (size_t)physical, sizeof(*row->visited), &flow->status);
    row->queue = xir_compile_calloc(&flow->memory, (size_t)physical, sizeof(*row->queue), &flow->status);
    row->site_heads = xir_compile_calloc(&flow->memory, row->site_head_count,
        sizeof(*row->site_heads), &flow->status);
    if (flow->status != XR_XIR_OK) return false;
    if (body->parameter_count) memcpy(row->types, body->parameters,
        (size_t)body->parameter_count * sizeof(*row->types));
    for (uint32_t p = 0; p < body->parameter_count; ++p) {
        row->handles[p] = xr_xir_type_is_cell(flow->module->types, row->types[p]);
        row->content_inputs[p] = UINT32_MAX; row->exposure_inputs[p] = UINT32_MAX;
    }
    for (uint32_t b = 0; b < body->block_count; ++b) {
        XrXirBlock block = body->blocks[b];
        if (block.first > body->instruction_count || block.count > body->instruction_count - block.first) {
            flow->status = XR_XIR_BAD_STRUCTURE; return false;
        }
    }
    for (uint32_t p = 0; p < row->point_count; ++p) row->predecessors[p] = UINT32_MAX;
    for (uint32_t v = 0; v < physical; ++v) row->identity_heads[v] = UINT32_MAX;
    for (uint32_t p = 0; p < row->site_head_count; ++p) row->site_heads[p] = UINT32_MAX;
    return true;
}

static void storage_retire_construction(XirStorageFlow *flow) {
    for (uint32_t f = 0; flow->rows && f < flow->view.function_count; ++f) {
        StorageFunction *row = &flow->rows[f];
        xr_compile_resources_free((void *)row->header.blocks);
        xr_compile_resources_free(row->types); xr_compile_resources_free(row->handles);
        xr_compile_resources_free(row->operations); xr_compile_resources_free(row->reads);
        xr_compile_resources_free(row->writes); xr_compile_resources_free(row->write_offsets);
        xr_compile_resources_free(row->write_points); xr_compile_resources_free(row->calls);
        xr_compile_resources_free(row->content_inputs); xr_compile_resources_free(row->exposure_inputs);
        xr_compile_resources_free(row->publications); xr_compile_resources_free(row->exposure_offsets);
        xr_compile_resources_free(row->publication_index);
        xr_compile_resources_free(row->exposure_points);
        xr_compile_resources_free(row->blocks); xr_compile_resources_free(row->predecessors);
        xr_compile_resources_free(row->edges); xr_compile_resources_free(row->identity_heads);
        xr_compile_resources_free(row->flow_index); xr_compile_resources_free(row->exposure_index);
        xr_compile_resources_free(row->footprint_index);
        xr_compile_resources_free(row->identity_links); xr_compile_resources_free(row->address_values);
        xr_compile_resources_free(row->addresses); xr_compile_resources_free(row->visited);
        xr_compile_resources_free(row->queue); xr_compile_resources_free(row->states);
        xr_compile_resources_free(row->state_index); xr_compile_resources_free(row->site_heads);
    }
    xr_compile_resources_free(flow->rows); flow->rows = NULL;
    xr_compile_resources_free(flow->work); flow->work = NULL;
    xr_compile_resources_free(flow->stages); flow->stages = NULL;
    for (uint32_t s = 0; flow->sites && s < flow->site_count; ++s)
        xr_compile_resources_free(flow->sites[s].identity_parameters);
    xr_compile_resources_free(flow->sites); flow->sites = NULL;
    xr_compile_resources_free(flow->site_index); flow->site_index = NULL;
    xr_compile_resources_free(flow->point_index); flow->point_index = NULL;
    xr_compile_resources_free(flow->point_value_index); flow->point_value_index = NULL;
    xr_compile_resources_free(flow->cross_index); flow->cross_index = NULL;
    xr_compile_resources_free(flow->unknown_index); flow->unknown_index = NULL;
    xr_compile_resources_free(flow->exposure_seed_index); flow->exposure_seed_index = NULL;
    flow->site_count = 0; flow->work_count = 0; flow->work_first = 0;
    flow->stage_count = 0; flow->stage_first = 0;
    flow->module = NULL; flow->execution = NULL; flow->control = NULL;
}

XR_FUNC void xir_storage_flow_free(XirStorageFlow *flow) {
    if (!flow) return;
    storage_retire_construction(flow);
    for (uint32_t f = 0; flow->view.functions && f < flow->view.function_count; ++f) {
        xr_compile_resources_free((void *)flow->view.functions[f].flows);
        xr_compile_resources_free((void *)flow->view.functions[f].exposure_flows);
        xr_compile_resources_free((void *)flow->view.functions[f].footprint_flows);
    }
    xr_compile_resources_free((void *)flow->view.functions);
    xr_compile_resources_free((void *)flow->view.crossflows);
    xr_compile_resources_free((void *)flow->view.applications);
    xr_compile_resources_free((void *)flow->view.bindings);
    xr_compile_resources_free((void *)flow->view.identities);
    xr_compile_resources_free((void *)flow->view.allocations);
    xr_compile_resources_free((void *)flow->view.inputs);
    xr_compile_resources_free((void *)flow->view.unknowns);
    xr_compile_resources_free((void *)flow->view.exposure_seeds);
    xr_compile_resources_free((void *)flow->view.point_values);
    xr_compile_resources_free(flow);
}

XR_FUNC XrXirStatus xir_storage_flow_new(const XrXirCompileContext *memory, const XrXirModule *module,
    const XirExecutionOwner *execution, XrCompileRequestControl *control, XirStorageFlow **output) {
    if (!xir_compile_context_valid(memory) || !module || !execution || !output || *output ||
        (module->function_count && !module->functions)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = XR_XIR_OK;
    XirStorageFlow *flow = xir_compile_calloc(memory, 1, sizeof(*flow), &status);
    if (!flow) return status;
    flow->memory = *memory; flow->module = module; flow->execution = execution; flow->control = control;
    flow->generation = 1;
    flow->view.functions = xir_compile_calloc(memory, module->function_count, sizeof(*flow->view.functions), &flow->status);
    flow->rows = xir_compile_calloc(memory, module->function_count, sizeof(*flow->rows), &flow->status);
    if (flow->status == XR_XIR_OK) flow->view.function_count = module->function_count;
    for (uint32_t f = 0; f < flow->view.function_count && storage_step(flow); ++f)
        if (!storage_prepare_function(flow, f)) break;
    status = flow->status;
    if (status != XR_XIR_OK) { xir_storage_flow_free(flow); return status; }
    *output = flow;
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_storage_flow_observe(XirStorageFlow *flow, uint32_t function,
    uint32_t instruction, const XrXirInstruction *op) {
    if (!flow || flow->sealed || flow->local_finished || !op || function >= flow->view.function_count)
        return XR_XIR_BAD_STRUCTURE;
    if (flow->status != XR_XIR_OK) return flow->status;
    StorageFunction *row = &flow->rows[function];
    const XrXirFunction *body = row->body;
    if (instruction != row->observed || instruction >= body->instruction_count ||
        !op->op || (uint32_t)op->op >= XR_XIR_OP_COUNT) return flow->status = XR_XIR_BAD_STRUCTURE;
    while (row->current_block + 1 < body->block_count &&
        body->blocks[row->current_block + 1].first <= instruction) ++row->current_block;
    uint32_t value = body->parameter_count + instruction;
    row->types[value] = op->type;
    row->handles[value] = xr_xir_place_op_kind(op->op) != XR_XIR_PLACE_NONE ||
        xr_xir_type_is_cell(flow->module->types, op->type);
    StorageOperation record = {op->op, op->args[0], UINT32_MAX, UINT32_MAX};
    if (storage_write(op->op)) { record.destination = op->args[0]; record.source = op->args[1]; }
    if (storage_new_place(op->op)) record.destination = value;
    if (op->op == XR_XIR_CELL_TAKE || op->op == XR_XIR_VALUE_TAKE) record.destination = op->args[0];
    if (op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_INVOKE_INDIRECT) {
        if (op->immediate < 0 || (uint64_t)op->immediate >= flow->view.functions[function].physical_values)
            return flow->status = XR_XIR_BAD_STRUCTURE;
        record.receiver = (uint32_t)op->immediate;
    }
    row->operations[instruction] = record; row->blocks[instruction] = row->current_block;
    if (record.destination != UINT32_MAX && !storage_record_write(flow, function, instruction, record.destination))
        return flow->status;
    if (storage_execution(op->op) && !storage_record_call(flow, function, instruction)) return flow->status;
    StorageConnect connect = {flow, function};
    XirInstructionFlowPoint point = {body, op, instruction, row->current_block};
    flow->status = xr_xir_instruction_flow_point(&point, XIR_INSTRUCTION_FLOW_ALL, storage_successor, &connect);
    if (flow->status != XR_XIR_OK) return flow->status;
    if (op->op == XR_XIR_RETURN && !storage_predecessor(flow, function, instruction,
        body->instruction_count, XIR_INSTRUCTION_FLOW_NEXT, 0)) return flow->status;
    if (op->op == XR_XIR_THROW && !storage_predecessor(flow, function, instruction,
        body->instruction_count + 1, XIR_INSTRUCTION_FLOW_NEXT, 0)) return flow->status;
    if (!body->blocks[row->current_block].panic && !storage_predecessor(flow, function, instruction,
        body->instruction_count + 2, XIR_INSTRUCTION_FLOW_PANIC, 0)) return flow->status;
    if (!storage_observe_identity(flow, function, instruction, op) ||
        !storage_observe_values(flow, function, instruction, op)) return flow->status;
    ++row->observed;
    return flow->status;
}

XR_FUNC XrXirStatus xir_storage_flow_finish_local(XirStorageFlow *flow) {
    if (!flow || flow->sealed || flow->local_finished) return XR_XIR_BAD_STRUCTURE;
    if (flow->status != XR_XIR_OK) return flow->status;
    for (uint32_t f = 0; f < flow->view.function_count && storage_step(flow); ++f) {
        StorageFunction *row = &flow->rows[f];
        if (row->observed != row->body->instruction_count) return flow->status = XR_XIR_BAD_STRUCTURE;
        if (!storage_index_writes(flow, f)) return flow->status;
        for (uint32_t p = 0; p < row->body->parameter_count && storage_step(flow); ++p) {
            if (!xr_xir_type_is_cell(flow->module->types, row->types[p])) continue;
            uint32_t value = storage_aux(flow, f);
            if (value == UINT32_MAX || !storage_input(flow, f, value, p, XIR_STORAGE_CONTENTS) ||
                !storage_unknown(flow, f, value, storage_payload(flow, f, p))) return flow->status;
            row->content_inputs[p] = value;
        }
        for (uint32_t r = 0; r < row->read_count && storage_step(flow); ++r) {
            StorageRead read = row->reads[r];
            if (read.source >= flow->view.functions[f].physical_values) return flow->status = XR_XIR_BAD_STRUCTURE;
            uint32_t source = read.source;
            if (read.force || storage_is_handle(flow, f, read.source)) {
                if (!storage_is_handle(flow, f, read.source)) return flow->status = XR_XIR_BAD_STRUCTURE;
                source = storage_contents_point(flow, f, read.point, read.source, read.kind, XIR_STORAGE_CONTENTS);
            }
            if (source == UINT32_MAX || !storage_relation(flow, f, source, read.target)) return flow->status;
        }
        row->header.operands = NULL; row->header.operand_count = 0;
    }
    if (!storage_drain(flow)) return flow->status;
    flow->local_finished = true;
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_storage_flow_contents(XirStorageFlow *flow, uint32_t function,
    uint32_t instruction, uint32_t handle, uint32_t *value) {
    if (!flow || flow->sealed || !flow->local_finished || !value || function >= flow->view.function_count ||
        instruction >= flow->rows[function].body->instruction_count ||
        handle >= flow->view.functions[function].physical_values ||
        !storage_is_handle(flow, function, handle)) return XR_XIR_BAD_STRUCTURE;
    if (flow->status != XR_XIR_OK) return flow->status;
    uint32_t contents = storage_contents(flow, function, instruction, handle);
    if (contents != UINT32_MAX && storage_drain(flow)) *value = contents;
    return flow->status;
}

static bool storage_valid_observation(const XirStorageFlow *flow, uint32_t function,
    uint32_t instruction, uint32_t handle, XirStoragePointKind point) {
    return flow && !flow->sealed && flow->local_finished && function < flow->view.function_count &&
        instruction < flow->rows[function].body->instruction_count &&
        handle < flow->view.functions[function].physical_values && storage_is_handle(flow, function, handle) &&
        (uint32_t)point <= XIR_STORAGE_PANIC;
}

XR_FUNC XrXirStatus xir_storage_flow_contents_at(XirStorageFlow *flow, uint32_t function,
    uint32_t instruction, uint32_t handle, XirStoragePointKind point, uint32_t *value) {
    if (!value || !storage_valid_observation(flow, function, instruction, handle, point)) return XR_XIR_BAD_STRUCTURE;
    if (flow->status != XR_XIR_OK) return flow->status;
    uint32_t contents = storage_contents_point(flow, function, instruction, handle, point, XIR_STORAGE_CONTENTS);
    if (contents != UINT32_MAX && storage_close_applications(flow)) *value = contents;
    return flow->status;
}

XR_FUNC XrXirStatus xir_storage_flow_expose(XirStorageFlow *flow, uint32_t function,
    uint32_t instruction, uint32_t handle, XirStoragePointKind point) {
    if (!storage_valid_observation(flow, function, instruction, handle, point) || flow->exposure_started)
        return XR_XIR_BAD_STRUCTURE;
    if (flow->status != XR_XIR_OK) return flow->status;
    storage_record_publication(flow, function,
        (StoragePublication){.point=instruction, .handle=handle, .kind=point});
    return flow->status;
}

static bool storage_start_exposure(XirStorageFlow *flow) {
    if (flow->exposure_started) return true;
    flow->exposure_started = true;
    for (uint32_t f = 0; f < flow->view.function_count && storage_step(flow); ++f) {
        StorageFunction *row = &flow->rows[f];
        if (!storage_index_publications(flow, f)) return false;
        uint32_t captures = flow->module->declarations && flow->module->declarations->functions ?
            flow->module->declarations->functions[f].capture_count : 0;
        for (uint32_t p = 0; p < row->body->parameter_count && storage_step(flow); ++p) {
            if (!storage_is_handle(flow, f, p)) continue;
            uint32_t value = storage_aux(flow, f);
            if (value == UINT32_MAX || !storage_input(flow, f, value, p, XIR_STORAGE_EXPOSURE)) return false;
            row->exposure_inputs[p] = value;
            if (p >= captures && !storage_exposure_seed(flow, f, value, UINT32_MAX)) return false;
        }
        for (uint32_t p = 0; p < row->publication_count && storage_step(flow); ++p) {
            StoragePublication event = row->publications[p];
            uint32_t condition = UINT32_MAX;
            if (event.conditional) {
                condition = storage_contents_point(flow, f, event.condition_point, event.condition_handle,
                    event.condition_kind, XIR_STORAGE_EXPOSURE);
                if (condition == UINT32_MAX) return false;
            }
            StorageAddress address;
            if (!storage_address(flow, f, event.handle, &address)) return false;
            for (uint32_t a = 0; a < address.count && storage_step(flow); ++a) {
                uint32_t root = row->address_values[address.first + a];
                uint32_t value = storage_contents_point(flow, f, event.point, root,
                    event.kind, XIR_STORAGE_EXPOSURE);
                if (value == UINT32_MAX) return false;
                if (event.conditional) {
                    if (!storage_relation_bank(flow, f, condition, value, XIR_STORAGE_EXPOSURE)) return false;
                } else if (!storage_exposure_seed(flow, f, value, UINT32_MAX)) return false;
            }
        }
    }
    if (++flow->generation == 0) { flow->status = XR_XIR_OVERFLOW; return false; }
    for (uint32_t s = 0; s < flow->site_count && storage_step(flow); ++s) {
        if (!flow->sites[s].active) continue;
        uint32_t outcomes = storage_environment_site(&flow->sites[s].site) ? 1 : 3;
        for (uint32_t o = 0; o < outcomes; ++o)
            if (!storage_bind_application(flow, s, (XirExecutionPort)o)) return false;
    }
    return storage_close_applications(flow);
}

XR_FUNC XrXirStatus xir_storage_flow_exposure_at(XirStorageFlow *flow, uint32_t function,
    uint32_t instruction, uint32_t handle, XirStoragePointKind point, uint32_t *value) {
    if (!value || !storage_valid_observation(flow, function, instruction, handle, point)) return XR_XIR_BAD_STRUCTURE;
    if (flow->status != XR_XIR_OK || !storage_start_exposure(flow)) return flow->status;
    uint32_t exposure = storage_contents_point(flow, function, instruction, handle, point, XIR_STORAGE_EXPOSURE);
    if (exposure != UINT32_MAX && storage_close_applications(flow)) *value = exposure;
    return flow->status;
}

#include "xxir_storage_point.inc.c"
#include "xxir_storage_capture.inc.c"

XR_FUNC XrXirStatus xir_storage_flow_view(const XirStorageFlow *flow, XirStorageFlowView *output) {
    if (!flow || !output) return XR_XIR_BAD_STRUCTURE;
    if (flow->status != XR_XIR_OK) return flow->status;
    *output = flow->view;
    return XR_XIR_OK;
}

static bool storage_close_applications(XirStorageFlow *flow) {
    if (flow->stage_generation != flow->generation) {
        flow->stage_first = 0; flow->stage_generation = flow->generation;
    }
    do {
        if (!storage_drain(flow)) return false;
        while (flow->stage_first < flow->stage_count && storage_step(flow)) {
            StorageWork item = flow->stages[flow->stage_first++];
            if (!storage_process_stage(flow, item.function, item.state)) return false;
        }
    } while (flow->work_first < flow->work_count && flow->status == XR_XIR_OK);
    return flow->status == XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_storage_flow_link(XirStorageFlow *flow, const XirExecutionOwner *execution, bool *changed) {
    if (!flow || flow->sealed || !flow->local_finished || !execution || execution != flow->execution || !changed)
        return XR_XIR_BAD_STRUCTURE;
    if (flow->status != XR_XIR_OK) return flow->status;
    uint32_t generation = flow->generation, work = flow->work_count;
    uint64_t revision = flow->revision;
    for (uint32_t s = 0; s < flow->site_count; ++s) flow->sites[s].active = false;
    flow->status = xir_execution_visit_captures(execution, storage_import_site, flow);
    if (flow->status == XR_XIR_OK) flow->status = xir_execution_visit_calls(execution, storage_import_site, flow);
    if (flow->status != XR_XIR_OK || !storage_reindex_sites(flow)) return flow->status;
    for (uint32_t s = 0; s < flow->site_count && storage_step(flow); ++s) {
        if (!flow->sites[s].active) continue;
        uint32_t outcomes = storage_environment_site(&flow->sites[s].site) ? 1 : 3;
        for (uint32_t o = 0; o < outcomes; ++o)
            if (!storage_bind_application(flow, s, (XirExecutionPort)o)) return flow->status;
    }
    if (!storage_close_applications(flow)) return flow->status;
    *changed = generation != flow->generation || work != flow->work_count || revision != flow->revision;
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_storage_flow_seal(XirStorageFlow *flow, XirStorageFlowView *output) {
    if (!flow || !output) return XR_XIR_BAD_STRUCTURE;
    if (flow->status != XR_XIR_OK) return flow->status;
    if (flow->sealed) return xir_storage_flow_view(flow, output);
    if (!storage_start_exposure(flow)) return flow->status;
    flow->sealing = true;
    if (++flow->generation == 0) return flow->status = XR_XIR_OVERFLOW;
    bool changed = false;
    XrXirStatus status = xir_storage_flow_link(flow, flow->execution, &changed);
    if (status != XR_XIR_OK) return status;
    for (uint32_t s = 0; s < flow->site_count; ++s)
        if (flow->sites[s].active && flow->sites[s].applications[0] != UINT32_MAX &&
            !flow->sites[s].bindings_ready) return flow->status = XR_XIR_UNRESOLVED;
    storage_retire_construction(flow); flow->sealed = true; flow->sealing = false;
    *output = flow->view;
    return XR_XIR_OK;
}

static void *storage_numeric_copy(const XrXirCompileContext *memory, const void *source,
    uint32_t count, size_t size, XrCompileRequestControl *control, XrXirStatus *status) {
    if (*status != XR_XIR_OK) return NULL;
    *status = xir_receive_poll(control);
    uint64_t bytes = (uint64_t)count * size;
    if (bytes > SIZE_MAX) { *status = XR_XIR_OVERFLOW; return NULL; }
    return xir_compile_copy(memory, source, (size_t)bytes, status);
}

static XirStorageFlow *storage_numeric_clone(const XirStorageFlow *source,
    XrCompileRequestControl *control, XrXirStatus *status) {
    XirStorageFlow *copy = xir_compile_calloc(&source->memory, 1, sizeof(*copy), status);
    if (!copy) return NULL;
    copy->memory = source->memory; copy->sealed = true; copy->view = source->view;
    copy->view.functions = NULL; copy->view.crossflows = NULL; copy->view.applications = NULL;
    copy->view.bindings = NULL; copy->view.identities = NULL; copy->view.allocations = NULL;
    copy->view.inputs = NULL; copy->view.unknowns = NULL; copy->view.exposure_seeds = NULL;
    copy->view.point_values = NULL;
    XirStorageFlowFunction *functions = xir_compile_calloc(&copy->memory, copy->view.function_count,
        sizeof(*functions), status);
    copy->view.functions = functions;
    for (uint32_t f = 0; f < copy->view.function_count && *status == XR_XIR_OK; ++f) {
        const XirStorageFlowFunction *from = &source->view.functions[f];
        functions[f] = *from; functions[f].flows = NULL; functions[f].exposure_flows = NULL;
        functions[f].footprint_flows = NULL;
        functions[f].flows = storage_numeric_copy(&copy->memory, from->flows, from->flow_count,
            sizeof(*from->flows), control, status);
        functions[f].exposure_flows = storage_numeric_copy(&copy->memory, from->exposure_flows,
            from->exposure_flow_count, sizeof(*from->exposure_flows), control, status);
        functions[f].footprint_flows = storage_numeric_copy(&copy->memory, from->footprint_flows,
            from->footprint_flow_count, sizeof(*from->footprint_flows), control, status);
    }
    copy->view.crossflows = storage_numeric_copy(&copy->memory, source->view.crossflows,
        source->view.crossflow_count, sizeof(*source->view.crossflows), control, status);
    copy->view.applications = storage_numeric_copy(&copy->memory, source->view.applications,
        source->view.application_count, sizeof(*source->view.applications), control, status);
    copy->view.bindings = storage_numeric_copy(&copy->memory, source->view.bindings,
        source->view.binding_count, sizeof(*source->view.bindings), control, status);
    copy->view.identities = storage_numeric_copy(&copy->memory, source->view.identities,
        source->view.identity_count, sizeof(*source->view.identities), control, status);
    copy->view.allocations = storage_numeric_copy(&copy->memory, source->view.allocations,
        source->view.allocation_count, sizeof(*source->view.allocations), control, status);
    copy->view.inputs = storage_numeric_copy(&copy->memory, source->view.inputs,
        source->view.input_count, sizeof(*source->view.inputs), control, status);
    copy->view.unknowns = storage_numeric_copy(&copy->memory, source->view.unknowns,
        source->view.unknown_count, sizeof(*source->view.unknowns), control, status);
    copy->view.exposure_seeds = storage_numeric_copy(&copy->memory, source->view.exposure_seeds,
        source->view.exposure_seed_count, sizeof(*source->view.exposure_seeds), control, status);
    copy->view.point_values = storage_numeric_copy(&copy->memory, source->view.point_values,
        source->view.point_value_count, sizeof(*source->view.point_values), control, status);
    if (*status != XR_XIR_OK) { xir_storage_flow_free(copy); return NULL; }
    return copy;
}

typedef struct StorageRelocate {
    const XirStorageFlowView *source;
    const XirStorageRelocation *maps;
    const uint32_t *index;
    XrCompileRequestControl *control;
    uint32_t progress;
    XrXirStatus status;
} StorageRelocate;

static bool storage_remap_value(StorageRelocate *map, uint32_t function, uint32_t *value) {
    if (map->status != XR_XIR_OK) return false;
    map->status = xir_receive_step(map->control, &map->progress);
    if (map->status != XR_XIR_OK) return false;
    if (function >= map->source->function_count || *value >= map->source->functions[function].value_count) {
        map->status = XR_XIR_BAD_STRUCTURE; return false;
    }
    uint32_t index = map->index[function];
    if (index == UINT32_MAX) return true;
    const XirStorageRelocation *change = &map->maps[index];
    uint32_t physical = map->source->functions[function].physical_values;
    *value = *value < physical ? change->values[*value] : change->physical_values + (*value - physical);
    return true;
}

static bool storage_remap_rows(StorageRelocate *map, XirStorageFlowView *target) {
    for (uint32_t f = 0; f < target->function_count && map->status == XR_XIR_OK; ++f) {
        uint32_t index = map->index[f];
        if (index == UINT32_MAX) continue;
        XirStorageFlowFunction *row = (XirStorageFlowFunction *)&target->functions[f];
        const XirStorageFlowFunction *from = &map->source->functions[f];
        row->physical_values = map->maps[index].physical_values;
        row->value_count = row->physical_values + (from->value_count - from->physical_values);
        for (uint32_t e = 0; e < row->flow_count; ++e) {
            XrXirValueFlow *edge = (XrXirValueFlow *)&row->flows[e];
            if (!storage_remap_value(map, f, &edge->source) || !storage_remap_value(map, f, &edge->target)) return false;
        }
        for (uint32_t e = 0; e < row->exposure_flow_count; ++e) {
            XrXirValueFlow *edge = (XrXirValueFlow *)&row->exposure_flows[e];
            if (!storage_remap_value(map, f, &edge->source) || !storage_remap_value(map, f, &edge->target)) return false;
        }
        for (uint32_t e = 0; e < row->footprint_flow_count; ++e) {
            XrXirValueFlow *edge = (XrXirValueFlow *)&row->footprint_flows[e];
            if (!storage_remap_value(map, f, &edge->source) || !storage_remap_value(map, f, &edge->target)) return false;
        }
    }
    return map->status == XR_XIR_OK;
}

static bool storage_remap_relations(StorageRelocate *map, XirStorageFlowView *target) {
    for (uint32_t e = 0; e < target->crossflow_count; ++e) {
        XirStorageCrossFlow *edge = (XirStorageCrossFlow *)&target->crossflows[e];
        if (!storage_remap_value(map, edge->source_function, &edge->source) ||
            !storage_remap_value(map, edge->target_function, &edge->target)) return false;
    }
    for (uint32_t b = 0; b < target->binding_count; ++b) {
        XirStorageBinding *binding = (XirStorageBinding *)&target->bindings[b];
        if (!storage_remap_value(map, binding->formal_function, &binding->formal_value) ||
            !storage_remap_value(map, binding->actual_function, &binding->actual_value)) return false;
    }
    for (uint32_t e = 0; e < target->identity_count; ++e) {
        XirStorageIdentity *edge = (XirStorageIdentity *)&target->identities[e];
        if (!storage_remap_value(map, edge->source_function, &edge->source) ||
            !storage_remap_value(map, edge->target_function, &edge->target)) return false;
    }
    for (uint32_t a = 0; a < target->application_count; ++a) {
        XirStorageApplication *application = (XirStorageApplication *)&target->applications[a];
        if (application->function >= target->function_count) { map->status = XR_XIR_BAD_STRUCTURE; return false; }
        uint32_t index = map->index[application->function];
        if (index == UINT32_MAX || application->instruction == UINT32_MAX) continue;
        uint32_t old_count = map->source->functions[application->function].physical_values -
            map->source->functions[application->function].parameter_count;
        if (application->instruction >= old_count) { map->status = XR_XIR_BAD_STRUCTURE; return false; }
        application->instruction = map->maps[index].instructions[application->instruction];
    }
    return map->status == XR_XIR_OK;
}

static bool storage_remap_leaves(StorageRelocate *map, XirStorageFlowView *target) {
    for (uint32_t a = 0; a < target->allocation_count; ++a) {
        XirStorageAllocation *allocation = (XirStorageAllocation *)&target->allocations[a];
        if (!storage_remap_value(map, allocation->function, &allocation->value)) return false;
    }
    for (uint32_t i = 0; i < target->input_count; ++i) {
        XirStorageInput *input = (XirStorageInput *)&target->inputs[i];
        if (!storage_remap_value(map, input->function, &input->value) ||
            !storage_remap_value(map, input->function, &input->handle)) return false;
    }
    for (uint32_t u = 0; u < target->unknown_count; ++u) {
        XirStorageUnknown *unknown = (XirStorageUnknown *)&target->unknowns[u];
        if (!storage_remap_value(map, unknown->function, &unknown->value)) return false;
    }
    for (uint32_t e = 0; e < target->exposure_seed_count; ++e) {
        XirStorageExposureSeed *seed = (XirStorageExposureSeed *)&target->exposure_seeds[e];
        if (!storage_remap_value(map, seed->function, &seed->value)) return false;
    }
    return map->status == XR_XIR_OK;
}

static bool storage_remap_points(StorageRelocate *map, XirStorageFlowView *target) {
    for (uint32_t p = 0; p < target->point_value_count; ++p) {
        XirStoragePointValue *point = (XirStoragePointValue *)&target->point_values[p];
        if (point->function >= map->source->function_count || (uint32_t)point->point > XIR_STORAGE_PANIC ||
            (point->reference.environment < XIR_EXECUTION_UNKNOWN_ENVIRONMENT &&
                point->reference.environment >= target->application_count)) {
            map->status = XR_XIR_BAD_STRUCTURE; return false;
        }
        XirStorageFlowFunction source = map->source->functions[point->function];
        uint32_t instructions = source.physical_values - source.parameter_count;
        if (point->value < source.physical_values || point->instruction >= instructions ||
            (point->footprint_input != UINT32_MAX &&
                (point->footprint_input < source.physical_values || point->footprint_input == point->value)) ||
            !storage_remap_value(map, point->function, &point->value) ||
            (point->footprint_input != UINT32_MAX &&
                !storage_remap_value(map, point->function, &point->footprint_input)) ||
            !storage_remap_value(map, point->reference.function, &point->reference.value)) {
            if (map->status == XR_XIR_OK) map->status = XR_XIR_BAD_STRUCTURE;
            return false;
        }
        uint32_t index = map->index[point->function];
        if (index != UINT32_MAX) point->instruction = map->maps[index].instructions[point->instruction];
    }
    return map->status == XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_storage_flow_relocate(XirStorageFlow *flow, const XirStorageRelocation *maps,
    uint32_t count, XrCompileRequestControl *control) {
    if (!flow || !flow->sealed || (count && !maps) || flow->status != XR_XIR_OK) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = XR_XIR_OK;
    uint32_t *index = xir_compile_calloc(&flow->memory, flow->view.function_count, sizeof(*index), &status);
    if (status != XR_XIR_OK) return status;
    for (uint32_t f = 0; f < flow->view.function_count; ++f) index[f] = UINT32_MAX;
    StorageRelocate map = {&flow->view, maps, index, control, 0, XR_XIR_OK};
    for (uint32_t c = 0; c < count && map.status == XR_XIR_OK; ++c) {
        const XirStorageRelocation *change = &maps[c];
        if (change->function >= flow->view.function_count || index[change->function] != UINT32_MAX) {
            map.status = XR_XIR_BAD_STRUCTURE; break;
        }
        const XirStorageFlowFunction *row = &flow->view.functions[change->function];
        uint32_t instructions = row->physical_values - row->parameter_count;
        uint64_t values = (uint64_t)change->physical_values + row->value_count - row->physical_values;
        if (change->physical_values < row->parameter_count ||
            change->physical_values - row->parameter_count != change->instruction_count || values > UINT32_MAX ||
            (row->physical_values && !change->values) || (instructions && !change->instructions)) {
            map.status = XR_XIR_BAD_STRUCTURE; break;
        }
        for (uint32_t v = 0; v < row->physical_values && map.status == XR_XIR_OK; ++v) {
            map.status = xir_receive_step(control, &map.progress);
            if (map.status != XR_XIR_OK) break;
            if (change->values[v] >= change->physical_values || (v < row->parameter_count && change->values[v] != v))
                map.status = XR_XIR_BAD_STRUCTURE;
        }
        for (uint32_t i = 0; i < instructions && map.status == XR_XIR_OK; ++i) {
            map.status = xir_receive_step(control, &map.progress);
            if (map.status != XR_XIR_OK) break;
            if (change->instructions[i] >= change->instruction_count ||
                (i && change->instructions[i - 1] >= change->instructions[i])) map.status = XR_XIR_BAD_STRUCTURE;
        }
        index[change->function] = c;
    }
    XirStorageFlow *copy = map.status == XR_XIR_OK ? storage_numeric_clone(flow, control, &map.status) : NULL;
    if (copy && storage_remap_rows(&map, &copy->view) && storage_remap_relations(&map, &copy->view) &&
        storage_remap_leaves(&map, &copy->view) && storage_remap_points(&map, &copy->view)) {
        XirStorageFlowView previous = flow->view; flow->view = copy->view; copy->view = previous;
    }
    xir_storage_flow_free(copy); xr_compile_resources_free(index);
    return map.status;
}
