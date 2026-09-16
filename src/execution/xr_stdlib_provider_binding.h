/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_stdlib_provider_binding.h - Native typed bindings for declared leaves
 */

#ifndef XR_STDLIB_PROVIDER_BINDING_H
#define XR_STDLIB_PROVIDER_BINDING_H

#include "xr_execution.h"
#include "../runtime/abi/xr_stdlib_provider_contract.h"

/* Only immutable generated descriptors can select host code. Failure leaves
 * both outputs unchanged. The instance still verifies the exact target ABI. */
XR_FUNC bool xr_stdlib_provider_operation_binding(const XrStdlibProviderDescriptor *descriptor,
                                                  XrProviderOperationBinding *operation_out,
                                                  uint32_t *behavior_out);

#endif  // XR_STDLIB_PROVIDER_BINDING_H
