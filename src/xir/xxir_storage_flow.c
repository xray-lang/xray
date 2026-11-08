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
    uint32_t *blocks, *predecessors;
    StoragePredecessor *edges;
    uint32_t edge_count, edge_capacity, flow_capacity;
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
    uint32_t crossflow_capacity, application_capacity, binding_capacity;
    uint32_t identity_capacity, allocation_capacity, input_capacity, unknown_capacity;
    StorageExecution *sites;
    uint32_t site_count, site_capacity, site_index_capacity, generation;
    uint32_t *site_index;
    uint32_t progress;
    uint64_t revision;
    XrXirStatus status;
    bool sealed, sealing, local_finished;
};
static bool storage_close_applications(XirStorageFlow *flow);

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

static bool storage_relation(XirStorageFlow *flow, uint32_t function, uint32_t source, uint32_t target) {
    XirStorageFlowFunction *row = (XirStorageFlowFunction *)&flow->view.functions[function];
    StorageFunction *work = &flow->rows[function];
    if (source >= row->value_count || target >= row->value_count) {
        flow->status = XR_XIR_BAD_STRUCTURE; return false;
    }
    void *values = (void *)row->flows;
    if (!storage_reserve(flow, &values, row->flow_count, &work->flow_capacity, sizeof(*row->flows))) return false;
    row->flows = values;
    ((XrXirValueFlow *)values)[row->flow_count++] = (XrXirValueFlow){source, target};
    ++flow->revision;
    return true;
}

static bool storage_unknown(XirStorageFlow *flow, uint32_t function, uint32_t value, XrXirType type) {
    void *values = (void *)flow->view.unknowns;
    if (!storage_reserve(flow, &values, flow->view.unknown_count, &flow->unknown_capacity,
        sizeof(*flow->view.unknowns))) return false;
    flow->view.unknowns = values;
    ((XirStorageUnknown *)values)[flow->view.unknown_count++] = (XirStorageUnknown){function, value, type, UINT32_MAX};
    ++flow->revision;
    return true;
}

static bool storage_allocation(XirStorageFlow *flow, uint32_t function, uint32_t value) {
    void *values = (void *)flow->view.allocations;
    if (!storage_reserve(flow, &values, flow->view.allocation_count, &flow->allocation_capacity,
        sizeof(*flow->view.allocations))) return false;
    flow->view.allocations = values;
    ((XirStorageAllocation *)values)[flow->view.allocation_count++] = (XirStorageAllocation){function, value};
    return true;
}

static bool storage_input(XirStorageFlow *flow, uint32_t function, uint32_t value, uint32_t handle) {
    void *values = (void *)flow->view.inputs;
    if (!storage_reserve(flow, &values, flow->view.input_count, &flow->input_capacity,
        sizeof(*flow->view.inputs))) return false;
    flow->view.inputs = values;
    ((XirStorageInput *)values)[flow->view.input_count++] = (XirStorageInput){function, value, handle, XIR_STORAGE_CONTENTS};
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

static uint32_t storage_point(XirStorageFlow *flow, uint32_t function, uint32_t point,
    uint32_t handle, uint32_t kind, uint32_t order) {
    StorageFunction *row = &flow->rows[function];
    if (point >= row->point_count ||
        handle >= flow->view.functions[function].physical_values) {
        flow->status = XR_XIR_BAD_STRUCTURE; return UINT32_MAX;
    }
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
    uint32_t value = storage_aux(flow, function);
    if (value == UINT32_MAX) return value;
    uint32_t index = row->state_count++;
    row->states[index] = (StorageState){point, handle, value, kind, order, 0}; row->state_index[slot] = index + 1;
    flow->work[flow->work_count++] = (StorageWork){function, index};
    return value;
}

static uint32_t storage_state(XirStorageFlow *flow, uint32_t function, uint32_t point, uint32_t handle) {
    return storage_point(flow, function, point, handle, 0, 0);
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
    uint32_t handle, XirStoragePointKind kind) {
    if (kind == XIR_STORAGE_BEFORE) return storage_contents(flow, function, point, handle);
    StorageAddress address;
    if (!storage_address(flow, function, handle, &address)) return UINT32_MAX;
    uint32_t root = address.count == 1 ? flow->rows[function].address_values[address.first] : handle;
    if (kind == XIR_STORAGE_NORMAL) return storage_point(flow, function, point, root, 5, 0);
    XrXirOp op = flow->rows[function].operations[point].op;
    if (!storage_execution(op)) {
        if (kind == XIR_STORAGE_PANIC) return storage_state(flow, function, point, root);
        flow->status = XR_XIR_BAD_STRUCTURE; return UINT32_MAX;
    }
    return storage_point(flow, function, point, root, kind == XIR_STORAGE_ERROR ? 2 : 4, UINT32_MAX);
}

static bool storage_request_read(XirStorageFlow *flow, uint32_t function, StorageRead read) {
    StorageFunction *row = &flow->rows[function];
    if (!storage_reserve(flow, (void **)&row->reads, row->read_count,
        &row->read_capacity, sizeof(*row->reads))) return false;
    row->reads[row->read_count++] = read;
    return true;
}

static bool storage_observe_values(XirStorageFlow *flow, uint32_t function, uint32_t i,
    const XrXirInstruction *op) {
    const XrXirFunction *body = flow->rows[function].body;
    uint32_t value = body->parameter_count + i;
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
    bool preserve = true;
    if (edge.kind != XIR_INSTRUCTION_FLOW_PANIC && storage_new_place(op->op) &&
        state.handle == body->parameter_count + edge.source) {
        preserve = false;
        if (op->op == XR_XIR_LOCAL_UNINIT)
            return storage_unknown(flow, function, state.value, storage_payload(flow, function, state.handle));
        if (storage_payload(flow, function, state.handle) != XR_XIR_UNIT &&
            !storage_relation(flow, function, op->source, state.value)) return false;
    } else if (edge.kind != XIR_INSTRUCTION_FLOW_PANIC && storage_write(op->op)) {
        StorageAddress address;
        if (!storage_address(flow, function, op->destination, &address)) return false;
        for (uint32_t a = 0; a < address.count; ++a) {
            if (flow->rows[function].address_values[address.first + a] != state.handle) continue;
            if (storage_payload(flow, function, state.handle) != XR_XIR_UNIT &&
                !storage_relation(flow, function, op->source, state.value)) return false;
            preserve = address.count != 1;
            break;
        }
    } else if (edge.kind != XIR_INSTRUCTION_FLOW_PANIC &&
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
        source = storage_point(flow, function, flow->rows[function].blocks[edge.source], state.handle, 3, UINT32_MAX);
    else if (storage_execution(op->op))
        source = storage_point(flow, function, edge.source, state.handle,
            edge.ordinal || op->op == XR_XIR_CLEANUP_ERROR || op->op == XR_XIR_THROW ? 2 : 1, UINT32_MAX);
    else source = storage_state(flow, function, edge.source, state.handle);
    return source != UINT32_MAX && storage_relation(flow, function, source, state.value);
}

static bool storage_drain(XirStorageFlow *flow) {
    while (flow->work_first < flow->work_count && storage_step(flow)) {
        StorageWork item = flow->work[flow->work_first++];
        StorageFunction *row = &flow->rows[item.function];
        StorageState state = row->states[item.state];
        if (state.kind == 5) {
            StoragePredecessor edge = {state.point, UINT32_MAX, 0, XIR_INSTRUCTION_FLOW_NEXT};
            if (!storage_transfer(flow, item.function, state, edge)) return false;
            continue;
        }
        if (state.kind) continue;
        StorageAddress address;
        if (!storage_address(flow, item.function, state.handle, &address)) return false;
        if (address.count > 1) {
            for (uint32_t a = 0; a < address.count && storage_step(flow); ++a) {
                uint32_t root = row->address_values[address.first + a];
                uint32_t source = storage_state(flow, item.function, state.point, root);
                if (source == UINT32_MAX || !storage_relation(flow, item.function, source, state.value)) return false;
            }
            continue;
        }
        if (!state.point && !storage_unknown(flow, item.function, state.value,
            storage_payload(flow, item.function, state.handle))) return false;
        for (uint32_t e = row->predecessors[state.point]; e != UINT32_MAX && storage_step(flow); e = row->edges[e].next)
            if (!storage_transfer(flow, item.function, state, row->edges[e])) return false;
    }
    return flow->status == XR_XIR_OK;
}

static bool storage_cross(XirStorageFlow *flow, XirStorageCrossFlow edge) {
    if (edge.source_function >= flow->view.function_count || edge.target_function >= flow->view.function_count ||
        edge.source >= flow->view.functions[edge.source_function].value_count ||
        edge.target >= flow->view.functions[edge.target_function].value_count) {
        flow->status = XR_XIR_BAD_STRUCTURE; return false;
    }
    void *values = (void *)flow->view.crossflows;
    if (!storage_reserve(flow, &values, flow->view.crossflow_count, &flow->crossflow_capacity,
        sizeof(*flow->view.crossflows))) return false;
    flow->view.crossflows = values;
    ((XirStorageCrossFlow *)values)[flow->view.crossflow_count++] = edge;
    ++flow->revision;
    return true;
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
        .instruction = site->instruction, .declaration = site->declaration,
        .target = site->target, .caller_instance = site->caller_instance,
        .environment = site->environment, .order = site->order, .port = site->port,
        .outcome = outcome, .protocol = site->protocol};
    return index;
}

static bool storage_site_index_grow(XirStorageFlow *flow) {
    uint64_t next = flow->site_index_capacity ? (uint64_t)flow->site_index_capacity * 2u : 16u;
    if (next > UINT32_MAX) { flow->status = XR_XIR_OVERFLOW; return false; }
    uint32_t capacity = (uint32_t)next;
    uint32_t *index = xir_compile_calloc(&flow->memory, capacity, sizeof(*index), &flow->status);
    if (!index) return false;
    for (uint32_t n = 0; n < flow->site_count && storage_step(flow); ++n) {
        XirExecutionSite site = flow->sites[n].site;
        uint32_t slot = storage_state_hash(site.index, (uint32_t)site.kind, 0, 0) & (capacity - 1);
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
    uint32_t slot = storage_state_hash(site->index, (uint32_t)site->kind, 0, 0) & (flow->site_index_capacity - 1);
    while (flow->site_index[slot]) {
        uint32_t index = flow->site_index[slot] - 1;
        if (flow->sites[index].site.kind == site->kind && flow->sites[index].site.index == site->index) {
            XirExecutionSite previous = flow->sites[index].site;
            if (previous.function != site->function || previous.instruction != site->instruction ||
                previous.target != site->target || previous.declaration != site->declaration ||
                previous.protocol != site->protocol || previous.order != site->order || previous.port != site->port)
                return flow->status = XR_XIR_BAD_STRUCTURE;
            flow->sites[index].site = *site;
            flow->sites[index].active = true;
            if (previous.environment != site->environment || previous.caller_instance != site->caller_instance)
                if (++flow->generation == 0) return flow->status = XR_XIR_OVERFLOW;
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

static uint32_t storage_call_input(XirStorageFlow *flow, const XirExecutionSite *site, uint32_t handle) {
    StorageAddress address;
    if (!storage_address(flow, site->function, handle, &address)) return UINT32_MAX;
    uint32_t root = address.count == 1 ? flow->rows[site->function].address_values[address.first] : handle;
    if (site->kind != XIR_EXECUTION_CLEANUP_USE)
        return storage_contents(flow, site->function, site->instruction, handle);
    uint32_t point = site->instruction == UINT32_MAX ? site->block : site->instruction;
    return storage_point(flow, site->function, point, root, (uint32_t)site->port + 1, site->order);
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
        uint32_t formal = storage_state(flow, site.declaration, 0, p);
        uint32_t contents = storage_call_input(flow, &site, handle);
        if (formal == UINT32_MAX || contents == UINT32_MAX || !storage_binding(flow,
            (XirStorageBinding){site.declaration, formal, site.function, contents, UINT32_MAX})) return false;
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
    uint32_t point = state.kind == 3 ? row->body->instruction_count + state.point : state.point;
    return row->site_heads[point];
}

static XirExecutionPort storage_activation(const XirStorageFlow *flow, uint32_t function, StorageState state) {
    if (state.kind == 3) return XIR_EXECUTION_PANIC;
    XrXirOp op = flow->rows[function].operations[state.point].op;
    return op == XR_XIR_CLEANUP_ERROR || op == XR_XIR_THROW ? XIR_EXECUTION_ERROR : XIR_EXECUTION_NORMAL;
}

static bool storage_site_port(const XirStorageFlow *flow, uint32_t function,
    const XirExecutionSite *site, StorageState state) {
    return site->kind != XIR_EXECUTION_CLEANUP_USE || site->port == storage_activation(flow, function, state);
}

static uint32_t storage_stage_count(const XirStorageFlow *flow, uint32_t function, StorageState state) {
    bool cleanup = state.kind == 3;
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
    bool affected = false, exact = record.bindings_ready;
    for (uint32_t p = 0; p < site.argument_count && storage_step(flow); ++p) {
        XirExecutionArgument actual = {0};
        XrXirStatus status = xir_execution_argument(flow->execution, &site, p, &actual);
        if (status == XR_XIR_UNRESOLVED) { exact = false; continue; }
        if (status != XR_XIR_OK) { flow->status = status; return false; }
        if (!actual.cell) continue;
        bool match_exact;
        if (!storage_argument_matches(flow, &site, actual, state.handle, &match_exact)) continue;
        if (flow->status != XR_XIR_OK) return false;
        affected = true; exact &= match_exact;
        uint32_t exit = callee->instruction_count + (uint32_t)outcome;
        uint32_t source = storage_contents(flow, site.declaration, exit, p);
        if (source == UINT32_MAX || !storage_cross(flow, (XirStorageCrossFlow){
            site.declaration, source, function, state.value, record.applications[outcome], XIR_STORAGE_CONTENTS})) return false;
    }
    return affected && exact ? true : storage_relation(flow, function, input, state.value);
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
    if (!flow->sealing && !site->native_callable) return true;
    const XrXirFunction *body = flow->rows[function].body;
    bool affected = !site->native_callable && state.handle < body->parameter_count;
    if (state.handle >= body->parameter_count)
        affected = !site->native_callable && !storage_new_place(flow->rows[function].operations[
            state.handle - body->parameter_count].op);
    StorageOpaqueCapture query = {.flow=flow, .use=*site, .target=state.value, .handle=state.handle};
    if (!storage_opaque_environment(flow, site, &query, &affected)) return false;
    if (affected && flow->sealing) {
        query.publish = true;
        if (!storage_opaque_environment(flow, site, &query, &affected)) return false;
    }
    if (!affected || flow->sealing) {
        if (!storage_relation(flow, function, input, state.value)) return false;
        if (affected && !storage_unknown(flow, function, state.value,
            storage_payload(flow, function, state.handle))) return false;
    }
    return flow->status == XR_XIR_OK;
}

static bool storage_process_stage(XirStorageFlow *flow, uint32_t function, uint32_t state_index) {
    StorageState state = flow->rows[function].states[state_index];
    if (!state.kind || state.kind == 5 || state.generation == flow->generation) return true;
    uint32_t generation = flow->generation;
    if (state.order == UINT32_MAX) {
        uint32_t count = storage_stage_count(flow, function, state);
        if (state.kind == 4) {
            uint32_t prefix = (uint32_t)storage_activation(flow, function, state) + 1;
            uint32_t source = storage_point(flow, function, state.point, state.handle, prefix, 0);
            if (source == UINT32_MAX || !storage_relation(flow, function, source, state.value)) return false;
            for (uint32_t order = 1; order <= count && storage_step(flow); ++order) {
                source = storage_point(flow, function, state.point, state.handle, 4, order);
                if (source == UINT32_MAX || !storage_relation(flow, function, source, state.value)) return false;
            }
        } else {
            uint32_t source = storage_point(flow, function, state.point, state.handle, state.kind, count);
            if (source == UINT32_MAX || !storage_relation(flow, function, source, state.value)) return false;
        }
    } else if (!state.order) {
        if (state.kind == 3) {
            XrXirBlock block = flow->rows[function].body->blocks[state.point];
            for (uint32_t i = block.first; i < block.first + block.count && storage_step(flow); ++i) {
                XrXirOp op = flow->rows[function].operations[i].op;
                uint32_t source = storage_execution(op) ?
                    storage_point(flow, function, i, state.handle, 4, UINT32_MAX) :
                    storage_state(flow, function, i, state.handle);
                if (source == UINT32_MAX || !storage_relation(flow, function, source, state.value)) return false;
            }
        } else {
            uint32_t source = storage_state(flow, function, state.point, state.handle);
            if (source == UINT32_MAX || !storage_relation(flow, function, source, state.value)) return false;
        }
    } else {
        uint32_t prefix = state.kind == 4 ? (uint32_t)storage_activation(flow, function, state) + 1 : state.kind;
        uint32_t input = storage_point(flow, function, state.point, state.handle, prefix, state.order - 1);
        if (input == UINT32_MAX) return false;
        for (uint32_t s = storage_state_sites(flow, function, state); s != UINT32_MAX; s = flow->sites[s].next) {
            StorageExecution record = flow->sites[s];
            if (!storage_site_port(flow, function, &record.site, state) || record.site.order != state.order - 1) continue;
            if (record.applications[0] == UINT32_MAX) {
                if (!storage_apply_opaque(flow, &record.site, function, state, input)) return false;
            } else {
                XirExecutionPort outcome = state.kind == 4 ? XIR_EXECUTION_PANIC :
                    state.kind == 2 ? XIR_EXECUTION_ERROR : XIR_EXECUTION_NORMAL;
                if (!storage_apply_known(flow, s, function, state, input, outcome)) return false;
                if (record.site.kind == XIR_EXECUTION_CLEANUP_USE && state.kind != 4) {
                    if (outcome != XIR_EXECUTION_NORMAL &&
                        !storage_apply_known(flow, s, function, state, input, XIR_EXECUTION_NORMAL)) return false;
                    if (outcome != XIR_EXECUTION_ERROR &&
                        !storage_apply_known(flow, s, function, state, input, XIR_EXECUTION_ERROR)) return false;
                    if (state.kind == 3 &&
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
    for (uint32_t p = 0; p < body->parameter_count; ++p)
        row->handles[p] = xr_xir_type_is_cell(flow->module->types, row->types[p]);
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
        xr_compile_resources_free(row->blocks); xr_compile_resources_free(row->predecessors);
        xr_compile_resources_free(row->edges); xr_compile_resources_free(row->identity_heads);
        xr_compile_resources_free(row->identity_links); xr_compile_resources_free(row->address_values);
        xr_compile_resources_free(row->addresses); xr_compile_resources_free(row->visited);
        xr_compile_resources_free(row->queue); xr_compile_resources_free(row->states);
        xr_compile_resources_free(row->state_index); xr_compile_resources_free(row->site_heads);
    }
    xr_compile_resources_free(flow->rows); flow->rows = NULL;
    xr_compile_resources_free(flow->work); flow->work = NULL;
    for (uint32_t s = 0; flow->sites && s < flow->site_count; ++s)
        xr_compile_resources_free(flow->sites[s].identity_parameters);
    xr_compile_resources_free(flow->sites); flow->sites = NULL;
    xr_compile_resources_free(flow->site_index); flow->site_index = NULL;
    flow->site_count = 0; flow->work_count = 0; flow->work_first = 0;
    flow->module = NULL; flow->execution = NULL; flow->control = NULL;
}

XR_FUNC void xir_storage_flow_free(XirStorageFlow *flow) {
    if (!flow) return;
    storage_retire_construction(flow);
    for (uint32_t f = 0; f < flow->view.function_count; ++f)
        xr_compile_resources_free((void *)flow->view.functions[f].flows);
    xr_compile_resources_free((void *)flow->view.functions);
    xr_compile_resources_free((void *)flow->view.crossflows);
    xr_compile_resources_free((void *)flow->view.applications);
    xr_compile_resources_free((void *)flow->view.bindings);
    xr_compile_resources_free((void *)flow->view.identities);
    xr_compile_resources_free((void *)flow->view.allocations);
    xr_compile_resources_free((void *)flow->view.inputs);
    xr_compile_resources_free((void *)flow->view.unknowns);
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
        for (uint32_t p = 0; p < row->body->parameter_count && storage_step(flow); ++p) {
            if (!xr_xir_type_is_cell(flow->module->types, row->types[p])) continue;
            uint32_t value = storage_state(flow, f, 0, p);
            if (value == UINT32_MAX || !storage_input(flow, f, value, p)) return flow->status;
        }
        for (uint32_t r = 0; r < row->read_count && storage_step(flow); ++r) {
            StorageRead read = row->reads[r];
            if (read.source >= flow->view.functions[f].physical_values) return flow->status = XR_XIR_BAD_STRUCTURE;
            uint32_t source = read.source;
            if (read.force || storage_is_handle(flow, f, read.source)) {
                if (!storage_is_handle(flow, f, read.source)) return flow->status = XR_XIR_BAD_STRUCTURE;
                source = storage_contents_point(flow, f, read.point, read.source, read.kind);
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

XR_FUNC XrXirStatus xir_storage_flow_view(const XirStorageFlow *flow, XirStorageFlowView *output) {
    if (!flow || !output) return XR_XIR_BAD_STRUCTURE;
    if (flow->status != XR_XIR_OK) return flow->status;
    *output = flow->view;
    return XR_XIR_OK;
}

static bool storage_close_applications(XirStorageFlow *flow) {
    do {
        if (!storage_drain(flow)) return false;
        for (uint32_t f = 0; f < flow->view.function_count && storage_step(flow); ++f)
            for (uint32_t s = 0; s < flow->rows[f].state_count && storage_step(flow); ++s)
                if (!storage_process_stage(flow, f, s)) return false;
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
