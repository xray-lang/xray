/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xffi_sig.h - Self-contained extern call signature carried by XrProto.
 *
 * The descriptor table is shared by semantic analysis, Xi lowering, VM
 * libffi/raw-memory execution, and AOT C generation. Extern signatures live on
 * the proto and round-trip through bytecode so the VM can resolve symbols and
 * build an ffi_cif with no compile-time Xi IR present.
 */

#ifndef XR_FFI_SIG_H
#define XR_FFI_SIG_H

#include "../../base/xdefs.h"
#include "../../shared/xr_native_type_core.h"
#include <stdint.h>

struct XrType;

/*
 * Compact, serialization-stable encoding of a C-ABI scalar type. It captures
 * exactly what the libffi marshaller needs (an ffi_type plus argument/return
 * widening). Raw pointers and C function pointers collapse to PTR; any other
 * heap kind crossing the boundary is treated as an opaque pointer.
 */
typedef enum XrFFIType {
#define XR_ABI_SCALAR(name, code, native, bytes, width, sign, floating, pointer, memory, c_type)   \
    XR_FFI_T_##name = code,
#include "xffi_scalar.def"
#undef XR_ABI_SCALAR
    XR_FFI_T_COUNT = 15
} XrFFIType;

typedef enum XrAbiWidthKind {
    XR_ABI_WIDTH_FIXED = 0,
    XR_ABI_WIDTH_POINTER = 1,
} XrAbiWidthKind;

/* One authoritative description of every scalar that may cross the C ABI or
 * be addressed by XI_PTR_LOAD/STORE. fixed_bytes is zero only for target
 * pointer-width scalars. */
typedef struct XrAbiScalarDesc {
    XrFFIType ffi_type;
    uint8_t native_type;
    uint8_t fixed_bytes;
    XrAbiWidthKind width_kind;
    bool is_signed;
    bool is_float;
    bool is_pointer;
    bool is_memory_scalar;
    const char *c_type;
} XrAbiScalarDesc;

XR_FUNC const XrAbiScalarDesc *xr_abi_scalar_desc(uint8_t ffi_type);
XR_FUNC const XrAbiScalarDesc *xr_abi_scalar_desc_for_native(uint8_t native_type);
XR_FUNC uint8_t xr_abi_scalar_width(const XrAbiScalarDesc *desc, uint8_t pointer_width);
XR_FUNC bool xr_ffi_type_is_memory_scalar(uint8_t ffi_type);

#define XR_FFI_PTR_AUX_TYPE_MASK 0x1fu
#define XR_FFI_PTR_AUX_UNALIGNED 0x80u

static inline uint8_t xr_ffi_ptr_aux_type(uint8_t aux) {
    return (uint8_t) (aux & XR_FFI_PTR_AUX_TYPE_MASK);
}

static inline uint8_t xr_ffi_ptr_aux(uint8_t type, bool unaligned) {
    return (uint8_t) ((type & XR_FFI_PTR_AUX_TYPE_MASK) |
                      (unaligned ? XR_FFI_PTR_AUX_UNALIGNED : 0u));
}

typedef struct XrFFICallbackSig {
    uint8_t *params; /* [nparams] XrFFIType codes (owned, NULL when nparams==0) */
    uint8_t nparams;
    uint8_t ret;
} XrFFICallbackSig;

/*
 * Extern call signature. Owned by the XrProto that carries it and freed via
 * xr_ffi_sig_free in xr_instruction_unit_free.
 */
typedef struct XrFFISig {
    char *symbol;                    /* C symbol to resolve (owned, never NULL) */
    char *dylib;                     /* extern library target, or NULL = process/default (owned) */
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
