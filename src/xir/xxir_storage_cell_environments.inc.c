/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_storage_cell_environments.inc.c - Environments of all Cell writers
 *
 * KEY CONCEPT:
 *   An entry callable identity can outlive its entry environment. Only a
 *   complete environment proof for every writer can remove that additional
 *   environment dependency. Reaching restriction and identity loans remain.
 */

static bool storage_cell_use(XirStorageFlow *flow, const XirExecutionSite *site, uint32_t parameter,
                             XirExecutionArgument actual) {
    StorageCellEnvironmentIndex *index = &flow->cell_environments;
    uint32_t handle = actual.caller_handle;
    if (handle == UINT32_MAX && actual.function == site->function &&
        actual.environment == XIR_EXECUTION_CURRENT_ENVIRONMENT)
        handle = actual.value;
    if (handle == UINT32_MAX) {
        index->unresolved[site->function] = true;
        return true;
    }
    StorageAddress address;
    if (!storage_address(flow, site->function, handle, &address))
        return false;
    for (uint32_t a = 0; a < address.count && storage_step(flow); ++a) {
        uint32_t root = flow->rows[site->function].address_values[address.first + a];
        uint32_t value = index->offsets[site->function] + root;
        XR_DCHECK(value < index->offsets[site->function + 1],
                  "Cell use indexes its template frame");
        if (!storage_reserve(flow, (void **) &index->uses, index->use_count, &index->use_capacity,
                             sizeof(*index->uses)))
            return false;
        uint32_t use = index->use_count++;
        index->uses[use] =
            (StorageCellUse) {site->declaration, parameter, site->target, index->heads[value],
                              site->native_callable || site->unresolved};
        index->heads[value] = use;
    }
    return flow->status == XR_XIR_OK;
}

static bool storage_cell_environment_index(XirStorageFlow *flow) {
    StorageCellEnvironmentIndex *index = &flow->cell_environments;
    if (index->ready)
        return true;
    if (!index->offsets) {
        index->offsets = xir_compile_calloc(&flow->memory, (size_t) flow->view.function_count + 1,
                                            sizeof(*index->offsets), &flow->status);
        index->unresolved = xir_compile_calloc(&flow->memory, flow->view.function_count,
                                               sizeof(*index->unresolved), &flow->status);
        uint64_t count = 0;
        for (uint32_t f = 0; flow->status == XR_XIR_OK && f < flow->view.function_count; ++f) {
            count += flow->view.functions[f].physical_values;
            if (count > UINT32_MAX || count > SIZE_MAX / sizeof(*index->heads)) {
                flow->status = XR_XIR_OVERFLOW;
                return false;
            }
            index->offsets[f + 1] = (uint32_t) count;
        }
        index->heads =
            xir_compile_calloc(&flow->memory, (size_t) count, sizeof(*index->heads), &flow->status);
        if (flow->status != XR_XIR_OK)
            return false;
    }
    uint32_t count = index->offsets[flow->view.function_count];
    for (uint32_t v = 0; v < count && storage_step(flow); ++v)
        index->heads[v] = UINT32_MAX;
    memset(index->unresolved, 0, flow->view.function_count * sizeof(*index->unresolved));
    index->use_count = 0;
    for (uint32_t s = 0; s < flow->site_count && storage_step(flow); ++s) {
        if (!flow->sites[s].active)
            continue;
        XirExecutionSite site = flow->sites[s].site;
        for (uint32_t p = 0; p < site.argument_count && storage_step(flow); ++p) {
            XirExecutionArgument actual = {0};
            XrXirStatus status = xir_execution_argument(flow->execution, &site, p, &actual);
            if (status == XR_XIR_UNRESOLVED) {
                index->unresolved[site.function] = true;
                continue;
            }
            if (status != XR_XIR_OK) {
                flow->status = status;
                return false;
            }
            if (actual.cell && !storage_cell_use(flow, &site, p, actual))
                return false;
        }
    }
    index->ready = flow->status == XR_XIR_OK;
    return index->ready;
}

static bool storage_cell_environment_writes(StorageEnvironmentProof *proof,
                                            StorageEnvironmentNode node, uint32_t root) {
    XirStorageFlow *flow = proof->flow;
    StorageFunction *row = &flow->rows[node.reference.function];
    uint32_t first = row->write_offsets[root], end = row->write_offsets[root + 1];
    XR_DCHECK(first <= end, "Cell writer intervals are ordered");
    for (uint32_t w = first; w < end && storage_step(flow); ++w) {
        StorageOperation operation = row->operations[row->write_points[w]];
        if ((!storage_write(operation.op) && !storage_new_place(operation.op)) ||
            operation.source == UINT32_MAX) {
            proof->remainder |= XIR_EXECUTION_REMAINDER_OPAQUE;
            continue;
        }
        if (!storage_environment_push(
                proof,
                (StorageEnvironmentNode) {.reference = {node.reference.function, operation.source,
                                                        XIR_EXECUTION_CURRENT_ENVIRONMENT},
                                          .target = UINT32_MAX,
                                          .kind = STORAGE_ENVIRONMENT_VALUE}))
            return false;
    }
    StorageCellEnvironmentIndex *index = &flow->cell_environments;
    uint32_t head = index->heads[index->offsets[node.reference.function] + root];
    for (uint32_t u = head; u != UINT32_MAX && storage_step(flow); u = index->uses[u].next) {
        StorageCellUse use = index->uses[u];
        if (use.opaque) {
            proof->remainder |= XIR_EXECUTION_REMAINDER_OPAQUE;
            continue;
        }
        if (use.function >= flow->view.function_count ||
            use.parameter >= flow->rows[use.function].body->parameter_count) {
            flow->status = XR_XIR_BAD_STRUCTURE;
            return false;
        }
        if (!storage_environment_push(
                proof, (StorageEnvironmentNode) {.reference = {use.function, use.parameter,
                                                               XIR_EXECUTION_CURRENT_ENVIRONMENT},
                                                 .target = use.instance,
                                                 .kind = STORAGE_ENVIRONMENT_CELL}))
            return false;
    }
    return flow->status == XR_XIR_OK;
}

static bool storage_cell_environment_inputs(StorageEnvironmentProof *proof,
                                            StorageEnvironmentNode node, uint32_t root) {
    XirStorageFlow *flow = proof->flow;
    const XrXirDeclarations *declarations = flow->module->declarations;
    uint32_t captures = declarations && declarations->functions
                            ? declarations->functions[node.reference.function].capture_count
                            : 0;
    if (root >= captures) {
        proof->remainder |= XIR_EXECUTION_REMAINDER_PARAMETER;
        return true;
    }
    bool bound = false;
    uint32_t head =
        node.target < flow->target_head_count ? flow->target_heads[node.target] : UINT32_MAX;
    for (uint32_t s = head; s != UINT32_MAX && storage_step(flow);
         s = flow->sites[s].binding_next) {
        XirExecutionSite site = flow->sites[s].site;
        if (site.declaration != node.reference.function)
            continue;
        bound = true;
        XirExecutionArgument actual = {0};
        XrXirStatus status = xir_execution_argument(flow->execution, &site, root, &actual);
        if (status == XR_XIR_UNRESOLVED) {
            proof->remainder |= XIR_EXECUTION_REMAINDER_STORAGE;
            continue;
        }
        if (status != XR_XIR_OK) {
            flow->status = status;
            return false;
        }
        if (!actual.cell || actual.function != site.function ||
            actual.caller_handle != actual.value ||
            actual.environment != XIR_EXECUTION_CURRENT_ENVIRONMENT ||
            site.capture_environment != XIR_EXECUTION_CURRENT_ENVIRONMENT) {
            proof->remainder |= XIR_EXECUTION_REMAINDER_OPAQUE;
            continue;
        }
        if (!storage_environment_push(
                proof, (StorageEnvironmentNode) {
                           .reference = {actual.function, actual.value, actual.environment},
                           .target = site.caller_instance,
                           .kind = STORAGE_ENVIRONMENT_CELL}))
            return false;
    }
    if (!bound)
        proof->remainder |= XIR_EXECUTION_REMAINDER_PARAMETER;
    return flow->status == XR_XIR_OK;
}

static bool storage_cell_environment_expand(StorageEnvironmentProof *proof,
                                            StorageEnvironmentNode node) {
    XirStorageFlow *flow = proof->flow;
    XR_DCHECK(node.reference.environment == XIR_EXECUTION_CURRENT_ENVIRONMENT,
              "Cell writer nodes retain a symbolic template frame");
    StorageFunction *row = &flow->rows[node.reference.function];
    if (flow->cell_environments.unresolved[node.reference.function])
        proof->remainder |= XIR_EXECUTION_REMAINDER_STORAGE;
    StorageAddress address;
    if (!storage_address(flow, node.reference.function, node.reference.value, &address))
        return false;
    for (uint32_t a = 0; a < address.count && storage_step(flow); ++a) {
        uint32_t root = row->address_values[address.first + a];
        if (!xr_xir_type_is_callable(xir_execution_types(flow->execution),
                                     storage_payload(flow, node.reference.function, root))) {
            proof->remainder |= XIR_EXECUTION_REMAINDER_OPAQUE;
            continue;
        }
        if (root < row->body->parameter_count) {
            if (!storage_cell_environment_inputs(proof, node, root))
                return false;
        } else {
            XrXirOp operation = row->operations[root - row->body->parameter_count].op;
            if (operation != XR_XIR_CELL_NEW && operation != XR_XIR_CELL_NEW_TAKE)
                proof->remainder |= XIR_EXECUTION_REMAINDER_OPAQUE;
        }
        if (!storage_cell_environment_writes(proof, node, root))
            return false;
    }
    return flow->status == XR_XIR_OK;
}

/* All writes and all direct Cell uses participate, including writes after the
 * observation and bodies reachable only through a captured function value.
 * The property says nothing about the restriction of the stored value. */
static bool storage_cell_closed_environments(XirStorageFlow *flow, const XirExecutionSite *input) {
    if (!storage_identity_input(flow, input))
        return false;
    StorageIdentitySupportCache *cache = &flow->closed_cell_environments;
    storage_identity_support_prepare(flow, cache);
    StorageEnvironmentNode initial = {
        .reference = {input->receiver_function, input->index, XIR_EXECUTION_CURRENT_ENVIRONMENT},
        .target = input->caller_instance,
        .kind = STORAGE_ENVIRONMENT_CELL};
    uint32_t remainder = 0;
    if (storage_identity_support_get(flow, cache, initial, &remainder))
        return !remainder && flow->status == XR_XIR_OK;
    if (!storage_cell_environment_index(flow))
        return false;
    StorageEnvironmentProof proof = {.flow = flow, .current = UINT32_MAX, .closed_values = true};
    storage_environment_push(&proof, initial);
    for (uint32_t n = 0; n < proof.count && !proof.remainder && storage_step(flow); ++n) {
        proof.current = n;
        StorageEnvironmentNode node = proof.nodes[n];
        if (node.kind == STORAGE_ENVIRONMENT_CELL) {
            if (!storage_cell_environment_expand(&proof, node))
                break;
        } else if (!storage_environment_expand(&proof, n))
            break;
    }
    bool complete = flow->status == XR_XIR_OK && !proof.remainder;
    if (complete)
        complete = storage_environment_acyclic(&proof) && !proof.remainder;
    if (complete)
        for (uint32_t n = 0; n < proof.count && storage_step(flow); ++n)
            if (proof.nodes[n].kind == STORAGE_ENVIRONMENT_CELL &&
                !storage_identity_support_put(flow, cache, proof.nodes[n]))
                break;
    complete &= flow->status == XR_XIR_OK;
    xr_compile_resources_free(proof.nodes);
    xr_compile_resources_free(proof.edges);
    xr_compile_resources_free(proof.index);
    xr_compile_resources_free(proof.queue);
    return complete;
}

XR_FUNC XrXirStatus xir_storage_flow_environment_closed(XirStorageFlow *flow, uint32_t function,
                                                        uint32_t value, uint32_t instance,
                                                        bool *closed) {
    if (!flow || !closed || function >= flow->view.function_count ||
        value >= flow->view.functions[function].value_count)
        return XR_XIR_BAD_STRUCTURE;
    *closed = false;
    if (flow->status != XR_XIR_OK)
        return flow->status;
    if (flow->sealed)
        return XR_XIR_OK;
    StorageFunction *row = &flow->rows[function];
    uint32_t first = 0, end = row->state_count;
    while (first < end) {
        uint32_t middle = first + (end - first) / 2;
        if (row->states[middle].value < value)
            first = middle + 1;
        else
            end = middle;
    }
    if (first == row->state_count || row->states[first].value != value ||
        storage_bank(row->states[first]) != XIR_STORAGE_CONTENTS)
        return XR_XIR_OK;
    XR_DCHECK(row->states[first].handle < flow->view.functions[function].physical_values,
              "A reaching version retains its physical storage identity");
    XirExecutionSite input = {.receiver_function = function,
                              .index = row->states[first].handle,
                              .caller_instance = instance};
    *closed = storage_cell_closed_environments(flow, &input);
    return flow->status;
}
