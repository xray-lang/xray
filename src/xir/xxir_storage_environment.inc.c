/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_storage_environment.inc.c - Bounded symbolic environment transport
 *
 * KEY CONCEPT:
 *   Callable origins come from the execution owner. A hidden callable can
 *   transport explicit environment inputs after every known binding has a
 *   closed dependency. An unbound ordinary callable formal is a caller-held
 *   loan contract, not an empty environment. Mutable Cells and unresolved
 *   recursive dependencies stay opaque.
 */

typedef enum StorageEnvironmentKind {
    STORAGE_ENVIRONMENT_VALUE, STORAGE_ENVIRONMENT_CONTRACT, STORAGE_ENVIRONMENT_HIDDEN
} StorageEnvironmentKind;

typedef struct StorageEnvironmentNode {
    XirStorageReference reference;
    uint32_t target, head, pending, remainder;
    StorageEnvironmentKind kind;
} StorageEnvironmentNode;

typedef struct StorageEnvironmentEdge { uint32_t dependent, next; } StorageEnvironmentEdge;

typedef struct StorageEnvironmentProof {
    XirStorageFlow *flow;
    StorageEnvironmentNode *nodes;
    StorageEnvironmentEdge *edges;
    uint32_t *index, *queue;
    uint32_t count, capacity, index_capacity, edge_count, edge_capacity;
    uint32_t queue_count, queue_capacity, current, remainder;
} StorageEnvironmentProof;

static uint32_t storage_environment_hash(StorageEnvironmentNode node) {
    uint32_t fields[] = {node.reference.function, node.reference.value, node.reference.environment,
        node.target, (uint32_t)node.kind};
    return xr_hash_bytes(fields, sizeof(fields));
}

static bool storage_environment_index(StorageEnvironmentProof *proof) {
    XirStorageFlow *flow = proof->flow;
    uint64_t next = proof->index_capacity ? (uint64_t)proof->index_capacity * 2 : 16;
    if (next > UINT32_MAX) { flow->status = XR_XIR_OVERFLOW; return false; }
    uint32_t capacity = (uint32_t)next;
    uint32_t *index = xir_compile_calloc(&flow->memory, capacity, sizeof(*index), &flow->status);
    if (!index) return false;
    for (uint32_t n = 0; n < proof->count && storage_step(flow); ++n) {
        uint32_t slot = storage_environment_hash(proof->nodes[n]) & (capacity - 1);
        while (index[slot] && storage_step(flow)) slot = (slot + 1) & (capacity - 1);
        index[slot] = n + 1;
    }
    if (flow->status != XR_XIR_OK) { xr_compile_resources_free(index); return false; }
    xr_compile_resources_free(proof->index); proof->index = index; proof->index_capacity = capacity;
    return true;
}

static bool storage_environment_push(StorageEnvironmentProof *proof, StorageEnvironmentNode node) {
    XirStorageFlow *flow = proof->flow;
    if (!proof->index_capacity || proof->count >= proof->index_capacity / 2)
        if (!storage_environment_index(proof)) return false;
    uint32_t slot = storage_environment_hash(node) & (proof->index_capacity - 1), index = UINT32_MAX;
    while (proof->index[slot] && storage_step(flow)) {
        StorageEnvironmentNode found = proof->nodes[proof->index[slot] - 1];
        if (found.kind == node.kind && found.target == node.target &&
            found.reference.function == node.reference.function && found.reference.value == node.reference.value &&
            found.reference.environment == node.reference.environment) { index = proof->index[slot] - 1; break; }
        slot = (slot + 1) & (proof->index_capacity - 1);
    }
    if (flow->status != XR_XIR_OK) return false;
    if (index == UINT32_MAX) {
        if (!storage_reserve(flow, (void **)&proof->nodes, proof->count, &proof->capacity, sizeof(*proof->nodes))) return false;
        index = proof->count++;
        node.head = UINT32_MAX; node.pending = 0;
        proof->nodes[index] = node; proof->index[slot] = index + 1;
    }
    if (proof->current == UINT32_MAX) return true;
    if (proof->nodes[proof->current].pending == UINT32_MAX) { flow->status = XR_XIR_OVERFLOW; return false; }
    if (!storage_reserve(flow, (void **)&proof->edges, proof->edge_count,
        &proof->edge_capacity, sizeof(*proof->edges))) return false;
    uint32_t edge = proof->edge_count++;
    proof->edges[edge] = (StorageEnvironmentEdge){proof->current, proof->nodes[index].head};
    proof->nodes[index].head = edge;
    ++proof->nodes[proof->current].pending;
    return true;
}

static XrXirStatus storage_environment_visit(void *opaque, const XirExecutionSite *site) {
    StorageEnvironmentProof *proof = opaque;
    XirStorageFlow *flow = proof->flow;
    if (site->kind == XIR_EXECUTION_STORABLE) {
        proof->remainder |= XIR_EXECUTION_REMAINDER_OPAQUE;
        return XR_XIR_OK;
    }
    if (site->kind == XIR_EXECUTION_FORMAL || site->kind == XIR_EXECUTION_CAPTURE_INPUT) {
        XirExecutionArgument formal = {0}; XirExecutionParameter parameter = {0};
        XrXirStatus status = xir_execution_argument(flow->execution, site, 0, &formal);
        if (status == XR_XIR_OK) status = xir_execution_parameter(flow->execution, site, 0, &parameter);
        if (status != XR_XIR_OK) return status;
        if (parameter.cell || parameter.generic_storage ||
            xr_xir_type_is_c_callable(xir_execution_types(flow->execution), parameter.type)) {
            proof->remainder |= XIR_EXECUTION_REMAINDER_OPAQUE;
            return XR_XIR_OK;
        }
        return storage_environment_push(proof, (StorageEnvironmentNode){
            .reference={formal.function, formal.value, formal.environment}, .target=site->caller_instance,
            .kind=parameter.hidden ? STORAGE_ENVIRONMENT_HIDDEN : STORAGE_ENVIRONMENT_CONTRACT}) ? XR_XIR_OK : flow->status;
    }
    if (site->kind != XIR_EXECUTION_CALL || site->protocol != 1) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p = 0; p < site->argument_count && storage_step(flow); ++p) {
        XirExecutionArgument actual = {0};
        XrXirStatus status = xir_execution_argument(flow->execution, site, p, &actual);
        if (status == XR_XIR_UNRESOLVED) { proof->remainder |= XIR_EXECUTION_REMAINDER_STORAGE; continue; }
        if (status != XR_XIR_OK) return status;
        if (actual.cell) { proof->remainder |= XIR_EXECUTION_REMAINDER_OPAQUE; continue; }
        const XrXirTypes *types = xir_execution_types(flow->execution);
        if (xr_xir_type_is_c_callable(types, actual.type)) continue;
        if (!xr_xir_type_is_callable(types, actual.type)) {
            /* This bounded proof admits only capture identities whose complete
             * value has no borrowed or opaque environment. Composite values
             * and unsubstituted binders require the general loan summary. */
            if (actual.type != XR_XIR_UNIT && actual.type != XR_XIR_BOOL &&
                actual.type != XR_XIR_RUNE && !xr_xir_type_is_number(actual.type) &&
                actual.type != XR_XIR_STRING) proof->remainder |= XIR_EXECUTION_REMAINDER_OPAQUE;
            continue;
        }
        if (!storage_environment_push(proof, (StorageEnvironmentNode){
            .reference={actual.function, actual.value, actual.environment}, .target=UINT32_MAX,
            .kind=STORAGE_ENVIRONMENT_VALUE})) return flow->status;
    }
    return flow->status;
}

static bool storage_environment_expand(StorageEnvironmentProof *proof, uint32_t index) {
    XirStorageFlow *flow = proof->flow;
    StorageEnvironmentNode node = proof->nodes[index];
    if (node.kind == STORAGE_ENVIRONMENT_VALUE) {
        bool complete = false; uint32_t remainder = 0;
        XrXirStatus status = xir_execution_visit_value_environment_inputs_in(flow->execution, &node.reference,
            storage_environment_visit, proof, &complete, &remainder);
        if (status != XR_XIR_OK) { flow->status = status; return false; }
        proof->remainder |= remainder;
        if (!complete && !remainder) proof->remainder |= XIR_EXECUTION_REMAINDER_STORAGE;
        return true;
    }
    bool bound = false;
    uint32_t head = node.target < flow->target_head_count ? flow->target_heads[node.target] : UINT32_MAX;
    for (uint32_t s = head; s != UINT32_MAX && storage_step(flow); s = flow->sites[s].binding_next) {
        XirExecutionSite site = flow->sites[s].site;
        if (site.declaration != node.reference.function || node.reference.value >= site.argument_count) continue;
        bound = true;
        XirExecutionArgument actual = {0};
        XrXirStatus status = xir_execution_argument(flow->execution, &site, node.reference.value, &actual);
        if (status == XR_XIR_UNRESOLVED) { proof->remainder |= XIR_EXECUTION_REMAINDER_STORAGE; continue; }
        if (status != XR_XIR_OK) { flow->status = status; return false; }
        if (actual.cell) { proof->remainder |= XIR_EXECUTION_REMAINDER_OPAQUE; continue; }
        if (!xr_xir_type_is_callable(xir_execution_types(flow->execution), actual.type) ||
            xr_xir_type_is_c_callable(xir_execution_types(flow->execution), actual.type)) {
            proof->remainder |= XIR_EXECUTION_REMAINDER_OPAQUE; continue;
        }
        if (!storage_environment_push(proof, (StorageEnvironmentNode){
            .reference={actual.function, actual.value, actual.environment}, .target=UINT32_MAX,
            .kind=STORAGE_ENVIRONMENT_VALUE})) return false;
    }
    /* A definition is checked even when it has no callers. Its ordinary
     * callable formal retains a symbolic environment leaf, substituted by
     * the actual current footprint at each application. Hidden formals have
     * no declaration contract and still require a real capture binding. */
    if (!bound && node.kind != STORAGE_ENVIRONMENT_CONTRACT)
        proof->remainder |= XIR_EXECUTION_REMAINDER_PARAMETER;
    return flow->status == XR_XIR_OK;
}

static bool storage_environment_acyclic(StorageEnvironmentProof *proof) {
    XirStorageFlow *flow = proof->flow;
    for (uint32_t n = 0; n < proof->count && storage_step(flow); ++n) {
        if (proof->nodes[n].pending) continue;
        if (!storage_reserve(flow, (void **)&proof->queue, proof->queue_count,
            &proof->queue_capacity, sizeof(*proof->queue))) return false;
        proof->queue[proof->queue_count++] = n;
    }
    for (uint32_t n = 0; n < proof->queue_count && storage_step(flow); ++n) {
        uint32_t source = proof->queue[n];
        for (uint32_t e = proof->nodes[source].head; e != UINT32_MAX && storage_step(flow); e = proof->edges[e].next) {
            uint32_t target = proof->edges[e].dependent;
            if (--proof->nodes[target].pending) continue;
            if (!storage_reserve(flow, (void **)&proof->queue, proof->queue_count,
                &proof->queue_capacity, sizeof(*proof->queue))) return false;
            proof->queue[proof->queue_count++] = target;
        }
    }
    if (proof->queue_count != proof->count) proof->remainder |= XIR_EXECUTION_REMAINDER_PARAMETER;
    return flow->status == XR_XIR_OK;
}

static XrXirStatus storage_hidden_environment(XirStorageFlow *flow, const XirExecutionSite *site) {
    StorageEnvironmentProof proof = {.flow=flow, .current=UINT32_MAX};
    XrXirStatus status = storage_environment_visit(&proof, site);
    for (uint32_t n = 0; status == XR_XIR_OK && !proof.remainder && n < proof.count; ++n) {
        proof.current = n;
        if (!storage_environment_expand(&proof, n)) status = flow->status;
    }
    if (status == XR_XIR_OK && !proof.remainder && !storage_environment_acyclic(&proof)) status = flow->status;
    if (status == XR_XIR_OK && proof.remainder) status = storage_reference_pending(flow, proof.remainder);
    xr_compile_resources_free(proof.nodes); xr_compile_resources_free(proof.edges);
    xr_compile_resources_free(proof.index); xr_compile_resources_free(proof.queue);
    return status;
}
