/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xffi_sig.h - Self-contained @extern call signature carried by XrProto.
 *
 * The AOT backend emits direct C calls for @extern functions and never needs
 * this. The bytecode VM, however, marshals @extern calls through libffi at
 * runtime — and the bytecode that backs an embedded ("xray build") binary is
 * serialized without the compile-time Xi IR. This signature therefore lives on
 * the proto and round-trips through bytecode so the VM can resolve the symbol
 * and build an ffi_cif with no IR present.
 */

#ifndef XR_FFI_SIG_H
#define XR_FFI_SIG_H

#include "../../base/xdefs.h"
#include <stdint.h>

struct XrType;

/*
 * Compact, serialization-stable encoding of a C-ABI scalar type. It captures
 * exactly what the libffi marshaller needs (an ffi_type plus argument/return
 * widening). Raw pointers and C function pointers collapse to PTR; any other
 * heap kind crossing the boundary is treated as an opaque pointer.
 */
typedef enum XrFFIType {
    XR_FFI_T_VOID = 0,
    XR_FFI_T_BOOL = 1,
    XR_FFI_T_I8 = 2,
    XR_FFI_T_U8 = 3,
    XR_FFI_T_I16 = 4,
    XR_FFI_T_U16 = 5,
    XR_FFI_T_I32 = 6,
    XR_FFI_T_U32 = 7,
    XR_FFI_T_I64 = 8,
    XR_FFI_T_U64 = 9,
    XR_FFI_T_F32 = 10,
    XR_FFI_T_F64 = 11,
    XR_FFI_T_PTR = 12
} XrFFIType;

typedef struct XrFFICallbackSig {
    uint8_t *params; /* [nparams] XrFFIType codes (owned, NULL when nparams==0) */
    uint8_t nparams;
    uint8_t ret;
} XrFFICallbackSig;

/*
 * @extern call signature. Owned by the XrProto that carries it and freed via
 * xr_ffi_sig_free in xr_proto_free.
 */
typedef struct XrFFISig {
    char *symbol;                    /* C symbol to resolve (owned, never NULL) */
    char *dylib;                     /* @dylib library name, or NULL = process/default (owned) */
    uint8_t *params;                 /* [nparams] XrFFIType codes (owned, NULL when nparams==0) */
    XrFFICallbackSig **param_cbacks; /* [nparams] optional CFn signatures */
    uint8_t nparams;                 /* declared parameter count */
    uint8_t ret;                     /* XrFFIType return code */
} XrFFISig;

/* Allocate a signature for `symbol` with `nparams` parameter slots. `symbol`
 * and `dylib` are copied (dylib may be NULL). Param codes default to I64 and
 * the return code to VOID; the caller fills them in. Returns NULL on OOM. */
XR_FUNC XrFFISig *xr_ffi_sig_new(const char *symbol, const char *dylib, uint8_t nparams);

XR_FUNC void xr_ffi_sig_free(XrFFISig *sig);

XR_FUNC bool xr_ffi_sig_set_param_callback(XrFFISig *sig, uint8_t index,
                                           const struct XrType *fn_type);

XR_FUNC bool xr_ffi_sig_set_param_callback_codes(XrFFISig *sig, uint8_t index,
                                                 const uint8_t *params, uint8_t nparams,
                                                 uint8_t ret);

/* Map an Xray static type to its compact C-ABI code. NULL maps to VOID on the
 * return side and I64 on the argument side. */
XR_FUNC uint8_t xr_ffi_type_from_xrtype(const struct XrType *t, bool is_return);

#endif  // XR_FFI_SIG_H
