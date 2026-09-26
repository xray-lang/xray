/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_execution_provider_typed.inc.c - Signature-driven host transport and result ownership
 */

_Static_assert(XR_PROVIDER_VALUE_MAX_NODES == XR_PROVIDER_LOGICAL_MAX_TYPE_BYTES,
               "Typed transport must hold every bounded logical type node");

#include "xr_provider_transport.h"

static bool execution_provider_borrow(const void *context, const XrExecutionResource *owner,
                                        XrStableId id, void **payload) {
    return xr_execution_resource_borrow(context, owner, id, payload);
}

void xr_execution_provider_result_dispose(XrProviderValuePack *result) {
    xr_provider_pack_dispose(result, NULL, xr_execution_resource_free);
}

static bool typed_pack_matches(const XrExecutionLease *lease, const XrProviderLogicalContract *logical,
                                XrProviderValuePack *pack, bool input) {
    if (pack->count > XR_PROVIDER_VALUE_MAX_NODES)
        return false;
    XrProviderResourceAccess access = {lease, execution_provider_borrow};
    uint32_t node_offset = 0u;
    uint8_t first = input ? 0u : logical->parameter_count;
    uint8_t limit = input ? logical->parameter_count : logical->parameter_count + 1u;
    for (uint8_t index = first; index < limit; ++index) {
        XrProviderLogicalTypeView type;
        size_t offset = 0u;
        if (input && logical->parameter_modes[index] == XR_PROVIDER_MODE_REF) {
            if (!xr_provider_logical_contract_type(logical, index, &type) || type.size != 1u ||
                type.bytes[0] != XR_PROVIDER_TYPE_U8_ARRAY ||
                logical->parameter_owners[index] != XR_PROVIDER_OWNER_BORROWED ||
                logical->reentry != XR_PROVIDER_REENTRY_FORBIDDEN ||
                logical->callbacks != XR_PROVIDER_CALLBACK_NONE ||
                (logical->effects & XR_PROVIDER_EFFECT_MAY_SUSPEND) != 0u)
                return false;
        }
        if (!xr_provider_logical_contract_type(logical, index, &type) ||
            !xr_provider_value_walk(&access, type, &offset, pack, &node_offset, input,
                                    input ? logical->parameter_modes[index] : 0u) || offset != type.size)
            return false;
    }
    return node_offset == pack->count;
}

static bool typed_result_resources_unique(const XrProviderValuePack *result,
                                          const XrProviderValuePack *arguments) {
    return xr_provider_result_resources_unique(result, arguments);
}

/* The exact admitted host ABI chooses marshaling. This is one typed execution
 * boundary; it does not try a second adapter after a refusal. */
static XrProviderCallStatus invoke_typed_provider(const XrBoundOperation *operation,
                                                  const XrProviderValuePack *arguments,
                                                  XrProviderValuePack *result) {
    switch (operation->trampoline_kind) {
        case XR_PROVIDER_TRAMPOLINE_TYPED:
            return operation->entry.typed(operation->context, arguments, result);
        case XR_PROVIDER_TRAMPOLINE_I64_UNARY:
        case XR_PROVIDER_TRAMPOLINE_I64_NULLARY:
            result->count = 1u;
            result->nodes[0].token = XR_PROVIDER_TYPE_I64;
            return operation->trampoline_kind == XR_PROVIDER_TRAMPOLINE_I64_UNARY
                       ? operation->entry.i64_unary(operation->context, arguments->nodes[0].as.i64,
                                                    &result->nodes[0].as.i64)
                       : operation->entry.i64_nullary(operation->context, &result->nodes[0].as.i64);
        case XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY:
            result->count = 1u;
            result->nodes[0].token = XR_PROVIDER_TYPE_BOOL;
            return operation->entry.bool_i64_unary(operation->context, arguments->nodes[0].as.i64,
                                                    &result->nodes[0].as.boolean);
        case XR_PROVIDER_TRAMPOLINE_OPTIONAL_I64_PAIR_NULLARY: {
            bool present = false;
            int64_t first = 0, second = 0;
            XrProviderCallStatus status = operation->entry.optional_i64_pair_nullary(
                operation->context, &present, &first, &second);
            result->count = present ? 4u : 1u;
            result->nodes[0] = (XrProviderValueNode) {.token = XR_PROVIDER_TYPE_OPTIONAL, .child_count = present ? 1u : 0u};
            if (present) {
                result->nodes[1] = (XrProviderValueNode) {.token = XR_PROVIDER_TYPE_TUPLE, .child_count = 2u};
                result->nodes[2] = (XrProviderValueNode) {.token = XR_PROVIDER_TYPE_I64, .as.i64 = first};
                result->nodes[3] = (XrProviderValueNode) {.token = XR_PROVIDER_TYPE_I64, .as.i64 = second};
            }
            return status;
        }
        default:
            return XR_PROVIDER_CALL_FAILED;
    }
}

XrExecutionProviderCallResult xr_execution_lease_provider_call_typed(
    const XrExecutionLease *lease, uint32_t requirement_index, uint32_t operation_index,
    const XrProviderValuePack *arguments, XrProviderValuePack *result_out) {
    if (!lease || !lease->instance || !lease->ticket)
        return XR_EXECUTION_PROVIDER_CALL_INVALID_LEASE;
    if (!arguments || !result_out || result_out->count != 0u || arguments == result_out ||
        arguments->count > XR_PROVIDER_VALUE_MAX_NODES)
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    XrInstance *instance = lease->instance;
    uint64_t lease_ticket = lease->ticket;
    lease_lock(instance);
    XrExecutionLeaseTicket *ticket = find_lease_ticket_locked(instance, lease_ticket);
    if (!ticket) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_LEASE;
    }
    if (requirement_index >= instance->provider_count ||
        operation_index >= instance->providers[requirement_index].operation_count ||
        instance->providers[requirement_index].operations[operation_index].trampoline_kind ==
            XR_PROVIDER_TRAMPOLINE_OUTPUT_WRITE || ticket->in_flight_calls == UINT32_MAX) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    }
    XrBoundOperation bound = instance->providers[requirement_index].operations[operation_index];
    const XrProviderLogicalContract *logical =
        &instance->program->provider_requirements[requirement_index].operations[operation_index].logical_contract;
    ++ticket->in_flight_calls;
    lease_unlock(instance);

    XrExecutionProviderCallResult status = XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    XrProviderValuePack borrowed = *arguments;
    XrProviderValuePack result = {0};
    bool adopted = false;
    if (typed_pack_matches(lease, logical, &borrowed, true)) {
        status = XR_EXECUTION_PROVIDER_CALL_FAILED;
        XrProviderCallStatus host_status = invoke_typed_provider(&bound, &borrowed, &result);
        if (host_status == XR_PROVIDER_CALL_OUT_OF_MEMORY)
            status = XR_EXECUTION_PROVIDER_CALL_OUT_OF_MEMORY;
        if (host_status == XR_PROVIDER_CALL_OK &&
            typed_pack_matches(lease, logical, &result, false) &&
            typed_result_resources_unique(&result, &borrowed)) {
            status = XR_EXECUTION_PROVIDER_CALL_OK;
            adopted = true;
            for (uint32_t index = 0u; index < result.count; ++index) {
                XrProviderValueNode *node = &result.nodes[index];
                if (node->token != XR_PROVIDER_TYPE_RESOURCE)
                    continue;
                uint16_t type_id = XR_CORE_TYPE_VOID;
                for (uint32_t type = 0u; type < instance->program->type_count; ++type) {
                    const XrValidatedType *row = &instance->program->types[type];
                    if (row->kind == XR_CORE_IR_TYPE_PROVIDER_RESOURCE &&
                        stable_id_equal(row->resource_id, node->as.resource.id)) {
                        type_id = row->type_id;
                        break;
                    }
                }
                XrExecutionStatus admission = xr_execution_resource_adopt(
                    lease, type_id, node->as.resource.id, node->as.resource.payload,
                    node->as.resource.destroy, &node->as.resource.owner);
                if (admission != XR_EXECUTION_OK) {
                    status = admission == XR_EXECUTION_OUT_OF_MEMORY
                                 ? XR_EXECUTION_PROVIDER_CALL_OUT_OF_MEMORY : XR_EXECUTION_PROVIDER_CALL_FAILED;
                    break;
                }
                node->as.resource.payload = NULL;
                node->as.resource.destroy = NULL;
            }
        }
    }
    if (status == XR_EXECUTION_PROVIDER_CALL_OK)
        *result_out = result;
    else
        xr_provider_pack_dispose(&result, &borrowed, adopted ? xr_execution_resource_free : NULL);
    lease_lock(instance);
    ticket = find_lease_ticket_locked(instance, lease_ticket);
    XR_CHECK(ticket && ticket->in_flight_calls != 0u, "typed provider lost its execution lease pin");
    --ticket->in_flight_calls;
    lease_unlock(instance);
    return status;
}
