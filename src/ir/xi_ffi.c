/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_ffi.c - VM-side foreign function invocation via libffi.
 */

#include "xi_ffi.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xffi_sig.h"
#include "../runtime/xisolate_api.h"
#include "../os/os_dylib.h"

#include <string.h>

#define XR_FFI_MAX_ARGS 32

/* FFI raw-pointer scalar load/store. The pointee width is the XrFFIType code
 * recorded on XI_PTR_LOAD/STORE; these back the VM's OP_PTR_LOAD / OP_PTR_STORE.
 * Independent of libffi (plain typed memory access). No bounds or null check:
 * validity of `addr` is the `unsafe` block's contract. */
XrValue xr_ffi_ptr_load(uintptr_t addr, uint8_t ffi_type) {
    switch ((XrFFIType) ffi_type) {
        case XR_FFI_T_I8:
            return xr_int(*(const int8_t *) addr);
        case XR_FFI_T_U8:
            return xr_int(*(const uint8_t *) addr);
        case XR_FFI_T_I16:
            return xr_int(*(const int16_t *) addr);
        case XR_FFI_T_U16:
            return xr_int(*(const uint16_t *) addr);
        case XR_FFI_T_I32:
            return xr_int(*(const int32_t *) addr);
        case XR_FFI_T_U32:
            return xr_int(*(const uint32_t *) addr);
        case XR_FFI_T_I64:
            return xr_int(*(const int64_t *) addr);
        case XR_FFI_T_U64:
            return xr_int((xr_Integer) * (const uint64_t *) addr);
        case XR_FFI_T_F32:
            return xr_float((double) *(const float *) addr);
        case XR_FFI_T_F64:
            return xr_float(*(const double *) addr);
        case XR_FFI_T_BOOL:
            return xr_bool(*(const uint8_t *) addr != 0);
        case XR_FFI_T_PTR:
            return xr_int((xr_Integer) (uintptr_t) *(void *const *) addr);
        case XR_FFI_T_VOID:
        default:
            return xr_null();
    }
}

void xr_ffi_ptr_store(uintptr_t addr, uint8_t ffi_type, XrValue val) {
    double f = XR_IS_FLOAT(val) ? XR_TO_FLOAT(val) : (double) XR_TO_INT(val);
    xr_Integer iv = XR_IS_FLOAT(val) ? (xr_Integer) XR_TO_FLOAT(val) : XR_TO_INT(val);
    switch ((XrFFIType) ffi_type) {
        case XR_FFI_T_I8:
            *(int8_t *) addr = (int8_t) iv;
            break;
        case XR_FFI_T_U8:
            *(uint8_t *) addr = (uint8_t) iv;
            break;
        case XR_FFI_T_I16:
            *(int16_t *) addr = (int16_t) iv;
            break;
        case XR_FFI_T_U16:
            *(uint16_t *) addr = (uint16_t) iv;
            break;
        case XR_FFI_T_I32:
            *(int32_t *) addr = (int32_t) iv;
            break;
        case XR_FFI_T_U32:
            *(uint32_t *) addr = (uint32_t) iv;
            break;
        case XR_FFI_T_I64:
            *(int64_t *) addr = (int64_t) iv;
            break;
        case XR_FFI_T_U64:
            *(uint64_t *) addr = (uint64_t) iv;
            break;
        case XR_FFI_T_F32:
            *(float *) addr = (float) f;
            break;
        case XR_FFI_T_F64:
            *(double *) addr = f;
            break;
        case XR_FFI_T_BOOL:
            *(uint8_t *) addr = (uint8_t) (iv != 0);
            break;
        case XR_FFI_T_PTR:
            *(void **) addr = (void *) (uintptr_t) iv;
            break;
        case XR_FFI_T_VOID:
            break;
    }
}

#ifdef XRAY_HAVE_LIBFFI
#include <ffi.h>
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

/* Map a compact C-ABI type code to the matching libffi type. The proto's
 * XrFFISig stores these codes so the mapping is identical for in-process and
 * embedded (serialized) bytecode. */
static ffi_type *ffi_type_for_code(uint8_t code) {
    switch (code) {
        case XR_FFI_T_VOID:
            return &ffi_type_void;
        case XR_FFI_T_BOOL:
        case XR_FFI_T_U8:
            return &ffi_type_uint8;
        case XR_FFI_T_I8:
            return &ffi_type_sint8;
        case XR_FFI_T_I16:
            return &ffi_type_sint16;
        case XR_FFI_T_U16:
            return &ffi_type_uint16;
        case XR_FFI_T_I32:
            return &ffi_type_sint32;
        case XR_FFI_T_U32:
            return &ffi_type_uint32;
        case XR_FFI_T_I64:
            return &ffi_type_sint64;
        case XR_FFI_T_U64:
            return &ffi_type_uint64;
        case XR_FFI_T_F32:
            return &ffi_type_float;
        case XR_FFI_T_F64:
            return &ffi_type_double;
        case XR_FFI_T_PTR:
        default:
            return &ffi_type_pointer;
    }
}

/* Resolve `symbol` from a foreign library (or the running process when
 * `dylib` is NULL). Returns NULL when the symbol cannot be found. */
static void *ffi_resolve_symbol(struct XrayIsolate *X, const char *symbol, const char *dylib) {
    if (!symbol || !symbol[0])
        return NULL;
    if (dylib && dylib[0]) {
        /* Map a bare library name ("m") to a platform file name and open it.
         * A leading '/' or an existing extension is treated as an explicit
         * path. The handle leaks for the process lifetime by design (foreign
         * libraries stay resident while bound symbols may be called). */
        char path[512];
        bool explicit_path = strchr(dylib, '/') != NULL || strstr(dylib, ".so") != NULL ||
                             strstr(dylib, ".dylib") != NULL || strstr(dylib, ".dll") != NULL;
        if (explicit_path) {
            snprintf(path, sizeof(path), "%s", dylib);
        } else {
#if defined(__APPLE__)
            snprintf(path, sizeof(path), "lib%s.dylib", dylib);
#elif defined(_WIN32)
            snprintf(path, sizeof(path), "%s.dll", dylib);
#else
            snprintf(path, sizeof(path), "lib%s.so", dylib);
#endif
        }
        XrDylib *lib = xr_dylib_open(path);
        if (!lib) {
            xr_runtime_error(X, "FFI: cannot load library '%s': %s\n", dylib,
                             xr_dylib_last_error());
            return NULL;
        }
        return xr_dylib_sym(lib, symbol);
    }
#if defined(_WIN32)
    /* Default (process) resolution on Windows is added with @dylib support. */
    (void) X;
    return NULL;
#else
    return dlsym(RTLD_DEFAULT, symbol);
#endif
}

XrValue xr_ffi_call_proto(struct XrayIsolate *X, struct XrProto *proto, XrValue *args, int nargs) {
    const XrFFISig *sig = proto ? proto->ffi_sig : NULL;
    if (!sig) {
        xr_runtime_error(X, "FFI: missing extern metadata for foreign call\n");
        return xr_null();
    }

    const char *symbol = sig->symbol;
    int np = (int) sig->nparams;
    if (np > XR_FFI_MAX_ARGS) {
        xr_runtime_error(X, "FFI: '%s' has too many parameters (%d > %d)\n", symbol ? symbol : "?",
                         np, XR_FFI_MAX_ARGS);
        return xr_null();
    }
    if (nargs < np) {
        xr_runtime_error(X, "FFI: '%s' expects %d arguments, got %d\n", symbol ? symbol : "?", np,
                         nargs);
        return xr_null();
    }

    void *fn = ffi_resolve_symbol(X, symbol, sig->dylib);
    if (!fn) {
        xr_runtime_error(X, "FFI: symbol '%s' not found%s%s\n", symbol ? symbol : "?",
                         sig->dylib ? " in library " : "", sig->dylib ? sig->dylib : "");
        return xr_null();
    }

    ffi_type *atypes[XR_FFI_MAX_ARGS];
    void *avalues[XR_FFI_MAX_ARGS];
    union FfiSlot {
        int64_t i64;
        uint64_t u64;
        double f64;
        float f32;
        void *ptr;
    } slots[XR_FFI_MAX_ARGS];

    for (int i = 0; i < np; i++) {
        uint8_t code = sig->params[i];
        atypes[i] = ffi_type_for_code(code);
        XrValue a = args[i];
        if (code == XR_FFI_T_F32 || code == XR_FFI_T_F64) {
            double d =
                XR_IS_FLOAT(a) ? XR_TO_FLOAT(a) : (XR_IS_INT(a) ? (double) XR_TO_INT(a) : 0.0);
            if (code == XR_FFI_T_F32)
                slots[i].f32 = (float) d;
            else
                slots[i].f64 = d;
        } else if (code == XR_FFI_T_PTR) {
            /* Raw pointers / function pointers travel as address-sized ints. */
            slots[i].ptr = (void *) (intptr_t) (XR_IS_INT(a) ? XR_TO_INT(a) : 0);
        } else {
            slots[i].i64 =
                XR_IS_INT(a) ? XR_TO_INT(a) : (XR_IS_FLOAT(a) ? (int64_t) XR_TO_FLOAT(a) : 0);
        }
        avalues[i] = &slots[i];
    }

    ffi_type *rtype = ffi_type_for_code(sig->ret);

    ffi_cif cif;
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned) np, rtype, atypes) != FFI_OK) {
        xr_runtime_error(X, "FFI: failed to prepare call for '%s'\n", symbol ? symbol : "?");
        return xr_null();
    }

    /* libffi widens small integer/float returns to at least ffi_arg width. */
    union {
        ffi_arg a;
        int64_t i64;
        uint64_t u64;
        double f64;
        float f32;
        void *ptr;
    } ret;
    ret.u64 = 0;
    ffi_call(&cif, FFI_FN(fn), &ret, avalues);

    switch (sig->ret) {
        case XR_FFI_T_VOID:
            return xr_null();
        case XR_FFI_T_F32:
            return xr_float((double) ret.f32);
        case XR_FFI_T_F64:
            return xr_float(ret.f64);
        case XR_FFI_T_BOOL:
            return xr_bool((int64_t) ret.a != 0);
        case XR_FFI_T_PTR:
            return xr_int((int64_t) (intptr_t) ret.ptr);
        default:
            /* Integer-like: ffi_arg holds the (sign/zero-extended) value. */
            return xr_int((int64_t) ret.a);
    }
}

#else /* !XRAY_HAVE_LIBFFI */

XrValue xr_ffi_call_proto(struct XrayIsolate *X, struct XrProto *proto, XrValue *args, int nargs) {
    (void) proto;
    (void) args;
    (void) nargs;
    xr_runtime_error(
        X, "FFI: this build has no libffi; @extern calls are unsupported in the VM (use AOT)\n");
    return xr_null();
}

#endif /* XRAY_HAVE_LIBFFI */
