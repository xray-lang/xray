/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_storage_identity_inputs.inc.c - Qualified captured Cell identities
 *
 * KEY CONCEPT:
 *   Identity support follows the existing capture and execution bindings.
 *   Parameter forwarding retains its symbolic frame until each application
 *   substitutes its actual contents and loan inputs.
 */
static StorageEnvironmentNode storage_identity_key(StorageIdentitySupport value) {
    return (StorageEnvironmentNode){.reference=value.reference, .target=value.instance,
        .kind=(StorageEnvironmentKind)value.kind};
}

static uint32_t storage_identity_support_slot(XirStorageFlow *flow, StorageIdentitySupportCache *cache,
    StorageEnvironmentNode node) {
    uint32_t slot = storage_environment_hash(node) & (cache->index_capacity - 1);
    while (cache->index[slot] && storage_step(flow)) {
        StorageEnvironmentNode found = storage_identity_key(cache->values[cache->index[slot] - 1]);
        if (found.kind == node.kind && found.target == node.target &&
            found.reference.function == node.reference.function &&
            found.reference.value == node.reference.value && found.reference.environment == node.reference.environment) break;
        slot = (slot + 1) & (cache->index_capacity - 1);
    }
    return slot;
}

static bool storage_identity_support_index(XirStorageFlow *flow, StorageIdentitySupportCache *cache) {
    uint64_t next = cache->index_capacity ? (uint64_t)cache->index_capacity * 2 : 16;
    if (next > UINT32_MAX || next > SIZE_MAX / sizeof(*cache->index)) {
        flow->status = XR_XIR_OVERFLOW;
        return false;
    }
    uint32_t *index = xir_compile_calloc(&flow->memory, (size_t)next, sizeof(*index), &flow->status);
    if (!index) return false;
    for (uint32_t n = 0; n < cache->count && storage_step(flow); ++n) {
        uint32_t slot = storage_environment_hash(storage_identity_key(cache->values[n])) & ((uint32_t)next - 1);
        while (index[slot] && storage_step(flow)) slot = (slot + 1) & ((uint32_t)next - 1);
        index[slot] = n + 1;
    }
    if (flow->status != XR_XIR_OK) {
        xr_compile_resources_free(index);
        return false;
    }
    xr_compile_resources_free(cache->index);
    cache->index = index;
    cache->index_capacity = (uint32_t)next;
    return true;
}

static bool storage_identity_support_get(XirStorageFlow *flow, StorageIdentitySupportCache *cache,
    StorageEnvironmentNode node, uint32_t *remainder) {
    if (!cache->index_capacity) return false;
    uint32_t slot = storage_identity_support_slot(flow, cache, node);
    if (!cache->index[slot]) return false;
    *remainder = cache->values[cache->index[slot] - 1].remainder;
    return true;
}

static bool storage_identity_support_put(XirStorageFlow *flow, StorageIdentitySupportCache *cache,
    StorageEnvironmentNode node) {
    if (!cache->index_capacity || cache->count >= cache->index_capacity / 2)
        if (!storage_identity_support_index(flow, cache)) return false;
    uint32_t slot = storage_identity_support_slot(flow, cache, node);
    if (flow->status != XR_XIR_OK) return false;
    if (!cache->index[slot]) {
        if (!storage_reserve(flow, (void **)&cache->values, cache->count, &cache->capacity, sizeof(*cache->values))) return false;
        cache->index[slot] = ++cache->count;
    }
    cache->values[cache->index[slot] - 1] = (StorageIdentitySupport){
        node.reference, node.target, node.remainder, node.kind};
    return true;
}

static void storage_identity_support_prepare(XirStorageFlow *flow, StorageIdentitySupportCache *cache) {
    /* The binding index invalidates these property facts on each linking
     * round. No cached fact freezes point contents or environment loans. */
    if (cache->sites == flow->site_count && cache->identities == flow->view.identity_count &&
        cache->applications == flow->view.application_count) return;
    cache->count = 0;
    if (cache->index_capacity) memset(cache->index, 0, (size_t)cache->index_capacity * sizeof(*cache->index));
    cache->sites = flow->site_count;
    cache->identities = flow->view.identity_count;
    cache->applications = flow->view.application_count;
}

static bool storage_identity_input_expand(StorageEnvironmentProof *proof, uint32_t index) {
    XirStorageFlow *flow = proof->flow;
    XR_DCHECK(index < proof->count, "identity query node belongs to its owner");
    StorageEnvironmentNode node = proof->nodes[index];
    if (storage_identity_support_get(flow, &flow->identity_support, node, &proof->nodes[index].remainder))
        return flow->status == XR_XIR_OK;
    if (flow->status != XR_XIR_OK) return false;
    StorageFunction *row = &flow->rows[node.reference.function];
    StorageAddress address;
    if (!storage_address(flow, node.reference.function, node.reference.value, &address)) return false;
    if (address.count != 1) {
        proof->nodes[index].remainder = XIR_EXECUTION_REMAINDER_OPAQUE;
        return true;
    }
    uint32_t root = row->address_values[address.first];
    if (root != node.reference.value) return storage_environment_push(proof, (StorageEnvironmentNode){
        .reference={node.reference.function, root, XIR_EXECUTION_CURRENT_ENVIRONMENT},
        .target=node.target, .kind=STORAGE_ENVIRONMENT_VALUE});
    if (root >= row->body->parameter_count) {
        XrXirOp op = row->operations[root - row->body->parameter_count].op;
        if (op != XR_XIR_CELL_NEW && op != XR_XIR_CELL_NEW_TAKE)
            proof->nodes[index].remainder = XIR_EXECUTION_REMAINDER_OPAQUE;
        return true;
    }
    const XrXirDeclarations *declarations = flow->module->declarations;
    uint32_t captures = declarations && declarations->functions ?
        declarations->functions[node.reference.function].capture_count : 0;
    if (root >= captures) {
        proof->nodes[index].remainder = XIR_EXECUTION_REMAINDER_PARAMETER;
        return true;
    }
    bool found = false;
    uint32_t head = node.target < flow->target_head_count ? flow->target_heads[node.target] : UINT32_MAX;
    for (uint32_t s = head; s != UINT32_MAX && storage_step(flow); s = flow->sites[s].binding_next) {
        XirExecutionSite site = flow->sites[s].site;
        if (site.kind == XIR_EXECUTION_CALLBACK_ENTRY || site.declaration != node.reference.function) continue;
        found = true;
        XirExecutionArgument actual;
        XrXirStatus status = xir_execution_argument(flow->execution, &site, root, &actual);
        if (status == XR_XIR_UNRESOLVED) {
            proof->nodes[index].remainder |= XIR_EXECUTION_REMAINDER_STORAGE;
            continue;
        }
        if (status != XR_XIR_OK) {
            flow->status = status;
            return false;
        }
        if (site.capture_environment != XIR_EXECUTION_CURRENT_ENVIRONMENT || !actual.cell ||
            actual.function != site.function || actual.environment != XIR_EXECUTION_CURRENT_ENVIRONMENT ||
            actual.caller_handle != actual.value) {
            proof->nodes[index].remainder |= XIR_EXECUTION_REMAINDER_OPAQUE;
            continue;
        }
        if (!storage_environment_push(proof, (StorageEnvironmentNode){
            .reference={actual.function, actual.value, actual.environment},
            .target=site.caller_instance, .kind=STORAGE_ENVIRONMENT_VALUE})) return false;
    }
    if (!found) proof->nodes[index].remainder |= XIR_EXECUTION_REMAINDER_PARAMETER;
    return flow->status == XR_XIR_OK;
}

static bool storage_identity_input_close(StorageEnvironmentProof *proof) {
    XirStorageFlow *flow = proof->flow;
    for (uint32_t n = 0; n < proof->count && storage_step(flow); ++n) {
        if (proof->nodes[n].pending) continue;
        if (!storage_reserve(flow, (void **)&proof->queue, proof->queue_count, &proof->queue_capacity, sizeof(*proof->queue))) return false;
        proof->queue[proof->queue_count++] = n;
    }
    for (uint32_t q = 0; q < proof->queue_count && storage_step(flow); ++q) {
        uint32_t source = proof->queue[q];
        for (uint32_t e = proof->nodes[source].head; e != UINT32_MAX && storage_step(flow); e = proof->edges[e].next) {
            uint32_t target = proof->edges[e].dependent;
            XR_DCHECK(proof->nodes[target].pending, "identity dependency completes once");
            proof->nodes[target].remainder |= proof->nodes[source].remainder;
            if (--proof->nodes[target].pending) continue;
            if (!storage_reserve(flow, (void **)&proof->queue, proof->queue_count, &proof->queue_capacity, sizeof(*proof->queue))) return false;
            proof->queue[proof->queue_count++] = target;
        }
    }
    for (uint32_t n = 0; n < proof->count && storage_step(flow); ++n) {
        if (proof->nodes[n].pending) proof->nodes[n].remainder |= XIR_EXECUTION_REMAINDER_PARAMETER;
        if (!(proof->nodes[n].remainder & XIR_EXECUTION_REMAINDER_STORAGE) &&
            !storage_identity_support_put(flow, &flow->identity_support, proof->nodes[n])) return false;
    }
    return flow->status == XR_XIR_OK;
}

static bool storage_identity_input(XirStorageFlow *flow, const XirExecutionSite *input) {
    storage_identity_support_prepare(flow, &flow->identity_support);
    StorageEnvironmentProof proof = {.flow=flow, .current=UINT32_MAX};
    StorageEnvironmentNode initial = {
        .reference={input->receiver_function, input->index, XIR_EXECUTION_CURRENT_ENVIRONMENT},
        .target=input->caller_instance, .kind=STORAGE_ENVIRONMENT_VALUE};
    uint32_t remainder = 0;
    if (storage_identity_support_get(flow, &flow->identity_support, initial, &remainder))
        return !remainder && flow->status == XR_XIR_OK;
    storage_environment_push(&proof, initial);
    for (uint32_t n = 0; n < proof.count && storage_step(flow); ++n) {
        proof.current = n;
        if (!storage_identity_input_expand(&proof, n)) break;
    }
    bool complete = flow->status == XR_XIR_OK && storage_identity_input_close(&proof) && !proof.nodes[0].remainder;
    xr_compile_resources_free(proof.nodes); xr_compile_resources_free(proof.edges);
    xr_compile_resources_free(proof.index); xr_compile_resources_free(proof.queue);
    return complete;
}
