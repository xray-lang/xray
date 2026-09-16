/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_stdlib_provider_binding.c - Immutable descriptor to typed host binding
 */

#include "xr_stdlib_provider_binding.h"
#include "xr_stdlib_provider_bindings_gen.inc.c"

static uint32_t native_platform(void) {
#if defined(XR_OS_WINDOWS)
    return XR_PROVIDER_PLATFORM_WINDOWS;
#elif defined(XR_OS_MACOS)
    return XR_PROVIDER_PLATFORM_MACOS;
#elif defined(XR_OS_LINUX)
    return XR_PROVIDER_PLATFORM_LINUX;
#else
    return 0u;
#endif
}

XR_FUNCDEF bool xr_stdlib_provider_operation_binding(const XrStdlibProviderDescriptor *descriptor,
                                                     XrProviderOperationBinding *operation_out,
                                                     uint32_t *behavior_out) {
    if (!descriptor || !operation_out || !behavior_out ||
        xr_stdlib_provider_find(descriptor->contract_id, descriptor->operation_id) != descriptor ||
        xr_stdlib_provider_count() != XR_STDLIB_PROVIDER_BINDING_COUNT)
        return false;
    XrProviderLogicalContract logical;
    if (!xr_stdlib_provider_logical(descriptor, &logical) ||
        (logical.platforms & native_platform()) == 0u ||
        (logical.runtime_profiles & XR_PROVIDER_LOGICAL_PROFILE_HOSTED) == 0u)
        return false;
    uint32_t behavior = 0u;
    if (logical.threads == XR_PROVIDER_THREADS_ANY)
        behavior |= XR_PROVIDER_BEHAVIOR_THREAD_SAFE;
    if (logical.reentry == XR_PROVIDER_REENTRY_ALLOWED)
        behavior |= XR_PROVIDER_BEHAVIOR_REENTRANT;
    if (logical.callbacks == XR_PROVIDER_CALLBACK_SYNCHRONOUS)
        behavior |= XR_PROVIDER_BEHAVIOR_CALLBACK_SAFE;
    for (size_t index = 0u; index < XR_STDLIB_PROVIDER_BINDING_COUNT; ++index) {
        if (xr_stdlib_provider_at(index) != descriptor)
            continue;
        XrProviderOperationBinding operation = xr_stdlib_provider_bindings[index];
        if (!xr_stable_id_equal(operation.operation_id, descriptor->operation_id))
            return false;
        *operation_out = operation;
        *behavior_out = behavior;
        return true;
    }
    return false;
}
