/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_ffi.h - VM-side foreign function (FFI) invocation.
 *
 * The AOT backend emits direct C calls for @extern functions. The bytecode
 * VM cannot emit C, so it marshals arguments and invokes the foreign symbol
 * through libffi at runtime. The proto carries `is_extern`; its retained
 * XiFunc holds the C symbol name, optional @dylib, and the typed signature.
 */

#ifndef XI_FFI_H
#define XI_FFI_H

#include <stdint.h>

#include "../base/xdefs.h"
#include "../runtime/value/xvalue.h"

struct XrayIsolate;
struct XrProto;

/* Invoke the @extern function backing `proto` with `nargs` arguments (in
 * XrValue form) and return the result as an XrValue. On failure (missing
 * symbol, unsupported signature, libffi unavailable) a runtime error is
 * raised on the isolate and XR_NULL is returned. */
XR_FUNC XrValue xr_ffi_call_proto(struct XrayIsolate *X, struct XrProto *proto, XrValue *args,
                                  int nargs);

/* FFI raw-pointer scalar load/store backing the VM's OP_PTR_LOAD / OP_PTR_STORE.
 * `ffi_type` is an XrFFIType code naming the pointee width. Plain typed memory
 * access (no libffi); validity of `addr` is the `unsafe` block's contract. */
XR_FUNC XrValue xr_ffi_ptr_load(uintptr_t addr, uint8_t ffi_type);
XR_FUNC void xr_ffi_ptr_store(uintptr_t addr, uint8_t ffi_type, XrValue val);

#endif  // XI_FFI_H
