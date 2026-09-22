/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_provider_host_fixture.h - Real resource host used by both executors
 */
#ifndef XR_TYPED_PROVIDER_HOST_FIXTURE_H
#define XR_TYPED_PROVIDER_HOST_FIXTURE_H
#include "base/xmalloc.h"
#include "execution/xr_execution.h"

typedef struct HostState { unsigned made, freed, reads, calls, mode; } HostState;
typedef struct Payload { HostState *host; int64_t value; } Payload;

static void destroy_payload(void *opaque) {
    Payload *payload = opaque;
    ++payload->host->freed;
    xr_free(payload);
}

static XrProviderCallStatus make_resource(void *opaque, const XrProviderValuePack *arguments,
                                          XrProviderValuePack *result) {
    HostState *host = opaque;
    ++host->calls;
    REQUIRE(arguments->count == 2u);
    REQUIRE(arguments->nodes[0].token == XR_PROVIDER_TYPE_I64);
    REQUIRE(arguments->nodes[1].token == XR_PROVIDER_TYPE_BOOL);
    bool present = arguments->nodes[1].as.boolean;
    result->count = present ? 2u : 1u;
    result->nodes[0] = (XrProviderValueNode) {.token = XR_PROVIDER_TYPE_OPTIONAL, .child_count = present};
    if (present) {
        Payload *payload = xr_malloc(sizeof(*payload));
        REQUIRE(payload != NULL);
        *payload = (Payload) {.host = host, .value = arguments->nodes[0].as.i64};
        ++host->made;
        result->nodes[1] = (XrProviderValueNode) {.token = XR_PROVIDER_TYPE_RESOURCE,
            .as.resource = {.id = typed_resource_id, .payload = payload, .destroy = destroy_payload}};
        if (host->mode == 2u) result->nodes[1].as.resource.id.bytes[0] ^= 1u;
        if (host->mode == 3u) {
            result->nodes[2] = result->nodes[1];
            result->count = 3u;
        }
    }
    return host->mode == 5u ? XR_PROVIDER_CALL_OUT_OF_MEMORY :
           host->mode == 1u ? XR_PROVIDER_CALL_FAILED : XR_PROVIDER_CALL_OK;
}

static XrProviderCallStatus read_resource(void *opaque, const XrProviderValuePack *arguments,
                                          XrProviderValuePack *result) {
    HostState *host = opaque;
    ++host->reads;
    REQUIRE(arguments->count == 1u && arguments->nodes[0].token == XR_PROVIDER_TYPE_RESOURCE);
    REQUIRE(arguments->nodes[0].as.resource.owner == NULL);
    REQUIRE(arguments->nodes[0].as.resource.destroy == NULL);
    Payload *payload = arguments->nodes[0].as.resource.payload;
    REQUIRE(payload && payload->host == host);
    result->count = 1u;
    result->nodes[0] = (XrProviderValueNode) {.token = XR_PROVIDER_TYPE_I64, .as.i64 = payload->value};
    if (host->mode == 4u) {
        /* A malformed host result must never destroy a borrowed argument. */
        result->nodes[0] = (XrProviderValueNode) {.token = XR_PROVIDER_TYPE_RESOURCE,
            .as.resource = {.id = typed_resource_id, .payload = payload, .destroy = destroy_payload}};
    }
    return XR_PROVIDER_CALL_OK;
}

static XrInstance *typed_instance_expect(XrValidatedProgram *program, XrTargetProfile *profile,
                                        HostState *host, XrExecutionStatus expected) {
    XrProviderOperationBinding operations[] = {
        {.operation_id = {{1u}}, .trampoline_kind = XR_PROVIDER_TRAMPOLINE_TYPED,
         .context = host, .entry.typed = make_resource},
        {.operation_id = {{2u}}, .trampoline_kind = XR_PROVIDER_TRAMPOLINE_TYPED,
         .context = host, .entry.typed = read_resource},
    };
    XrProviderBinding binding = {.contract_id = typed_contract_id,
        .behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL, .operations = operations, .operation_count = 2u};
    for (size_t i = 0u; i < xr_target_profile_provider_count(profile); ++i) {
        const XrTargetProviderContract *p = xr_target_profile_provider(profile, i);
        if (memcmp(p->contract_id.bytes, typed_contract_id.bytes, XR_STABLE_ID_BYTES) == 0)
            REQUIRE(xr_target_provider_contract_fingerprint(p, &binding.contract_fingerprint) == XR_RUNTIME_ABI_OK);
    }
    XrExecutionBindingInput input = {.schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program, .profile = profile, .providers = &binding, .provider_count = 1u, .generation = 1u};
    XrInstance *instance = NULL;
    XrExecutionDiagnostic diagnostic = {0};
    XrExecutionStatus status = xr_execution_instance_create(&input, &instance, &diagnostic);
    if (status != expected) fprintf(stderr, "typed instance status %d diagnostic %d\n", status, diagnostic.kind);
    REQUIRE(status == expected);
    if (status != XR_EXECUTION_OK) REQUIRE(instance == NULL);
    return instance;
}

static XrInstance *typed_instance(XrValidatedProgram *program, XrTargetProfile *profile, HostState *host) {
    return typed_instance_expect(program, profile, host, XR_EXECUTION_OK);
}


#endif
