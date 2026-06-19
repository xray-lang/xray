/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xffi_sig.c - @extern call signature carried by XrProto.
 */

#include "xffi_sig.h"
#include "xtype.h"
#include "xstruct_layout.h"
#include "../../base/xmalloc.h"

#include <string.h>

XrFFISig *xr_ffi_sig_new(const char *symbol, const char *dylib, uint8_t nparams) {
    XrFFISig *sig = (XrFFISig *) xr_malloc(sizeof(XrFFISig));
    if (!sig)
        return NULL;
    sig->symbol = xr_strdup(symbol ? symbol : "");
    sig->dylib = (dylib && dylib[0]) ? xr_strdup(dylib) : NULL;
    sig->nparams = nparams;
    sig->ret = XR_FFI_T_VOID;
    sig->params = NULL;
    if (!sig->symbol || (dylib && dylib[0] && !sig->dylib)) {
        xr_ffi_sig_free(sig);
        return NULL;
    }
    if (nparams > 0) {
        sig->params = (uint8_t *) xr_malloc((size_t) nparams * sizeof(uint8_t));
        if (!sig->params) {
            xr_ffi_sig_free(sig);
            return NULL;
        }
        for (uint8_t i = 0; i < nparams; i++)
            sig->params[i] = XR_FFI_T_I64;
    }
    return sig;
}

void xr_ffi_sig_free(XrFFISig *sig) {
    if (!sig)
        return;
    xr_free(sig->symbol);
    xr_free(sig->dylib);
    xr_free(sig->params);
    xr_free(sig);
}

uint8_t xr_ffi_type_from_xrtype(const struct XrType *t, bool is_return) {
    if (!t)
        return is_return ? XR_FFI_T_VOID : XR_FFI_T_I64;
    switch (t->kind) {
        case XR_KIND_UNIT:
            return XR_FFI_T_VOID;
        case XR_KIND_BOOL:
            return XR_FFI_T_BOOL;
        case XR_KIND_FLOAT:
            return (t->native_width == XR_NATIVE_F32) ? XR_FFI_T_F32 : XR_FFI_T_F64;
        case XR_KIND_INT:
            switch (t->native_width) {
                case XR_NATIVE_I8:
                    return XR_FFI_T_I8;
                case XR_NATIVE_U8:
                    return XR_FFI_T_U8;
                case XR_NATIVE_I16:
                    return XR_FFI_T_I16;
                case XR_NATIVE_U16:
                    return XR_FFI_T_U16;
                case XR_NATIVE_I32:
                    return XR_FFI_T_I32;
                case XR_NATIVE_U32:
                    return XR_FFI_T_U32;
                case XR_NATIVE_U64:
                    return XR_FFI_T_U64;
                default:
                    return XR_FFI_T_I64;
            }
        case XR_KIND_FUNCTION:
            return XR_FFI_T_PTR;
        default:
            /* Other heap kinds cross the boundary as opaque pointers. */
            return XR_FFI_T_PTR;
    }
}
