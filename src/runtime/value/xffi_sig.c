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

static XrFFICallbackSig *xr_ffi_callback_sig_new(const uint8_t *params, uint8_t nparams,
                                                 uint8_t ret) {
    XrFFICallbackSig *cb = (XrFFICallbackSig *) xr_malloc(sizeof(XrFFICallbackSig));
    if (!cb)
        return NULL;
    cb->params = NULL;
    cb->nparams = nparams;
    cb->ret = ret;
    if (nparams > 0) {
        cb->params = (uint8_t *) xr_malloc((size_t) nparams * sizeof(uint8_t));
        if (!cb->params) {
            xr_free(cb);
            return NULL;
        }
        if (params) {
            memcpy(cb->params, params, (size_t) nparams * sizeof(uint8_t));
        } else {
            for (uint8_t i = 0; i < nparams; i++)
                cb->params[i] = XR_FFI_T_I64;
        }
    }
    return cb;
}

static void xr_ffi_callback_sig_free(XrFFICallbackSig *cb) {
    if (!cb)
        return;
    xr_free(cb->params);
    xr_free(cb);
}

XrFFISig *xr_ffi_sig_new(const char *symbol, const char *dylib, uint8_t nparams) {
    XrFFISig *sig = (XrFFISig *) xr_malloc(sizeof(XrFFISig));
    if (!sig)
        return NULL;
    sig->symbol = xr_strdup(symbol ? symbol : "");
    sig->dylib = (dylib && dylib[0]) ? xr_strdup(dylib) : NULL;
    sig->nparams = nparams;
    sig->ret = XR_FFI_T_VOID;
    sig->params = NULL;
    sig->param_cbacks = NULL;
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
    if (sig->param_cbacks) {
        for (uint8_t i = 0; i < sig->nparams; i++)
            xr_ffi_callback_sig_free(sig->param_cbacks[i]);
        xr_free(sig->param_cbacks);
    }
    xr_free(sig);
}

bool xr_ffi_sig_set_param_callback_codes(XrFFISig *sig, uint8_t index, const uint8_t *params,
                                         uint8_t nparams, uint8_t ret) {
    if (!sig || index >= sig->nparams)
        return false;
    if (!sig->param_cbacks) {
        sig->param_cbacks =
            (XrFFICallbackSig **) xr_calloc((size_t) sig->nparams, sizeof(XrFFICallbackSig *));
        if (!sig->param_cbacks)
            return false;
    }

    XrFFICallbackSig *cb = xr_ffi_callback_sig_new(params, nparams, ret);
    if (!cb)
        return false;

    xr_ffi_callback_sig_free(sig->param_cbacks[index]);
    sig->param_cbacks[index] = cb;
    if (sig->params)
        sig->params[index] = XR_FFI_T_PTR;
    return true;
}

bool xr_ffi_sig_set_param_callback(XrFFISig *sig, uint8_t index, const struct XrType *fn_type) {
    if (!sig || index >= sig->nparams || !fn_type || fn_type->kind != XR_KIND_FUNCTION ||
        !fn_type->function.is_c_abi || fn_type->function.param_count < 0 ||
        fn_type->function.param_count > UINT8_MAX) {
        return false;
    }

    uint8_t nparams = (uint8_t) fn_type->function.param_count;
    uint8_t stack_params[16];
    uint8_t *params = nparams <= 16 ? stack_params : (uint8_t *) xr_malloc(nparams);
    if (nparams > 0 && !params)
        return false;

    for (uint8_t i = 0; i < nparams; i++) {
        const struct XrType *pt =
            fn_type->function.param_types ? fn_type->function.param_types[i] : NULL;
        params[i] = xr_ffi_type_from_xrtype(pt, false);
    }
    uint8_t ret = xr_ffi_type_from_xrtype(fn_type->function.return_type, true);
    bool ok = xr_ffi_sig_set_param_callback_codes(sig, index, params, nparams, ret);
    if (params != stack_params)
        xr_free(params);
    return ok;
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
