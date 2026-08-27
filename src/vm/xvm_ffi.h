/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_ffi.h - VM-side foreign function (FFI) invocation.
 *
 * The AOT backend emits direct C calls for extern functions. The bytecode
 * VM cannot emit C, so it marshals arguments and invokes the foreign symbol
 * through libffi at runtime. The proto carries `is_extern`; its retained
 * XiFunc holds the C symbol name, optional extern library target, and the typed signature.
 */

#ifndef XVM_FFI_H
#define XVM_FFI_H

#include <stdint.h>

#include "../base/xdefs.h"
#include "../runtime/value/xffi_sig.h"
#include "../runtime/value/xvalue.h"

struct XrVMRuntime;
struct XrProto;

typedef enum {
    XR_FFI_CALL_OK = 0,
    XR_FFI_CALL_FAILED,
    XR_FFI_CALL_PROVIDER_UNAVAILABLE,
} XrFfiCallStatus;

/* Invoke the extern function backing `proto` with `nargs` arguments (in
 * XrValue form). Success initializes `out_result`; failure leaves it untouched
 * and has already emitted the source-owned diagnostic. XR_NULL is a valid void
 * result and is never an error sentinel. */
XR_FUNC XrFfiCallStatus xr_ffi_call_proto(struct XrVMRuntime *X, struct XrProto *proto,
                                          XrValue *args, int nargs, XrValue *out_result);

/* FFI raw-pointer scalar load/store backing the VM's OP_PTR_LOAD / OP_PTR_STORE.
 * `ffi_type` is an XrFFIType code plus optional pointer-load flags. Plain typed
 * memory access (no libffi); `endian` is the evaluated Endian argument and
 * validity of `addr` is the `unsafe` block's contract. */
XR_FUNC XrValue xr_ffi_ptr_load(uintptr_t addr, uint8_t ffi_type, int64_t endian);
XR_FUNC void xr_ffi_ptr_store(uintptr_t addr, uint8_t ffi_type, XrValue val, int64_t endian);

#endif  // XVM_FFI_H
