/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_ffi.c - VM-side foreign function invocation via libffi.
 */

#include "xvm_ffi.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xffi_sig.h"
#include "../runtime/closure/xclosure.h"
#include "../runtime/xvm_call.h"
#include "../runtime/xisolate_api.h"
#include "../vm/xvm_closure.h"
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

typedef union XrFFISlot {
    int8_t i8;
    uint8_t u8;
    int16_t i16;
    uint16_t u16;
    int32_t i32;
    uint32_t u32;
    int64_t i64;
    uint64_t u64;
    double f64;
    float f32;
    void *ptr;
} XrFFISlot;

typedef struct XrFFICallbackBridge {
    struct XrayIsolate *X;
    XrClosure *closure;
    const XrFFICallbackSig *sig;
    ffi_cif cif;
    ffi_type *atypes[XR_FFI_MAX_ARGS];
    ffi_closure *closure_mem;
    void *code;
} XrFFICallbackBridge;

static int64_t ffi_value_as_i64(XrValue v) {
    if (XR_IS_INT(v))
        return XR_TO_INT(v);
    if (XR_IS_BOOL(v))
        return XR_TO_BOOL(v) ? 1 : 0;
    if (XR_IS_FLOAT(v))
        return (int64_t) XR_TO_FLOAT(v);
    return 0;
}

static double ffi_value_as_f64(XrValue v) {
    if (XR_IS_FLOAT(v))
        return XR_TO_FLOAT(v);
    if (XR_IS_INT(v))
        return (double) XR_TO_INT(v);
    if (XR_IS_BOOL(v))
        return XR_TO_BOOL(v) ? 1.0 : 0.0;
    return 0.0;
}

static void *ffi_value_as_ptr(XrValue v) {
    if (XR_IS_INT(v))
        return (void *) (uintptr_t) XR_TO_INT(v);
    if (XR_IS_PTR(v))
        return XR_TO_PTR(v);
    return NULL;
}

static void *ffi_store_arg_slot(XrFFISlot *slot, uint8_t code, XrValue v) {
    int64_t iv = ffi_value_as_i64(v);
    switch (code) {
        case XR_FFI_T_BOOL:
            slot->u8 = (uint8_t) (iv != 0);
            return &slot->u8;
        case XR_FFI_T_I8:
            slot->i8 = (int8_t) iv;
            return &slot->i8;
        case XR_FFI_T_U8:
            slot->u8 = (uint8_t) iv;
            return &slot->u8;
        case XR_FFI_T_I16:
            slot->i16 = (int16_t) iv;
            return &slot->i16;
        case XR_FFI_T_U16:
            slot->u16 = (uint16_t) iv;
            return &slot->u16;
        case XR_FFI_T_I32:
            slot->i32 = (int32_t) iv;
            return &slot->i32;
        case XR_FFI_T_U32:
            slot->u32 = (uint32_t) iv;
            return &slot->u32;
        case XR_FFI_T_I64:
            slot->i64 = (int64_t) iv;
            return &slot->i64;
        case XR_FFI_T_U64:
            slot->u64 = (uint64_t) iv;
            return &slot->u64;
        case XR_FFI_T_F32:
            slot->f32 = (float) ffi_value_as_f64(v);
            return &slot->f32;
        case XR_FFI_T_F64:
            slot->f64 = ffi_value_as_f64(v);
            return &slot->f64;
        case XR_FFI_T_PTR:
            slot->ptr = ffi_value_as_ptr(v);
            return &slot->ptr;
        case XR_FFI_T_VOID:
            slot->i64 = 0;
            return &slot->i64;
    }
    slot->i64 = 0;
    return &slot->i64;
}

static XrValue ffi_value_from_c_arg(uint8_t code, void *addr) {
    switch (code) {
        case XR_FFI_T_BOOL:
            return xr_bool(*(const uint8_t *) addr != 0);
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
        case XR_FFI_T_PTR:
            return xr_int((xr_Integer) (uintptr_t) *(void *const *) addr);
        case XR_FFI_T_VOID:
            return xr_null();
    }
    return xr_null();
}

static void ffi_store_c_return(uint8_t code, XrValue v, void *ret) {
    if (!ret || code == XR_FFI_T_VOID)
        return;
    int64_t iv = ffi_value_as_i64(v);
    switch (code) {
        case XR_FFI_T_BOOL:
            *(uint8_t *) ret = (uint8_t) (iv != 0);
            break;
        case XR_FFI_T_I8:
            *(int8_t *) ret = (int8_t) iv;
            break;
        case XR_FFI_T_U8:
            *(uint8_t *) ret = (uint8_t) iv;
            break;
        case XR_FFI_T_I16:
            *(int16_t *) ret = (int16_t) iv;
            break;
        case XR_FFI_T_U16:
            *(uint16_t *) ret = (uint16_t) iv;
            break;
        case XR_FFI_T_I32:
            *(int32_t *) ret = (int32_t) iv;
            break;
        case XR_FFI_T_U32:
            *(uint32_t *) ret = (uint32_t) iv;
            break;
        case XR_FFI_T_I64:
            *(int64_t *) ret = (int64_t) iv;
            break;
        case XR_FFI_T_U64:
            *(uint64_t *) ret = (uint64_t) iv;
            break;
        case XR_FFI_T_F32:
            *(float *) ret = (float) ffi_value_as_f64(v);
            break;
        case XR_FFI_T_F64:
            *(double *) ret = ffi_value_as_f64(v);
            break;
        case XR_FFI_T_PTR:
            *(void **) ret = ffi_value_as_ptr(v);
            break;
        case XR_FFI_T_VOID:
            break;
    }
}

static void ffi_callback_bridge_invoke(ffi_cif *cif, void *ret, void **c_args, void *user_data) {
    (void) cif;
    XrFFICallbackBridge *bridge = (XrFFICallbackBridge *) user_data;
    if (!bridge || !bridge->X || !bridge->closure || !bridge->sig) {
        ffi_store_c_return(XR_FFI_T_VOID, xr_null(), ret);
        return;
    }

    XrValue args[XR_FFI_MAX_ARGS];
    int nargs = (int) bridge->sig->nparams;
    for (int i = 0; i < nargs; i++)
        args[i] = ffi_value_from_c_arg(bridge->sig->params[i], c_args[i]);

    XrValue result = xr_vm_call_closure(bridge->X, bridge->closure, args, nargs);
    ffi_store_c_return(bridge->sig->ret, result, ret);
}

static void ffi_callback_bridge_free(XrFFICallbackBridge *bridge) {
    if (!bridge || !bridge->closure_mem)
        return;
    ffi_closure_free(bridge->closure_mem);
    bridge->closure_mem = NULL;
    bridge->code = NULL;
}

static void ffi_callback_bridges_free(XrFFICallbackBridge *bridges, int count) {
    if (!bridges)
        return;
    for (int i = 0; i < count; i++)
        ffi_callback_bridge_free(&bridges[i]);
}

static bool ffi_callback_bridge_prepare(struct XrayIsolate *X, const XrFFICallbackSig *sig,
                                        XrClosure *closure, XrFFICallbackBridge *bridge) {
    if (!sig || !closure || !bridge)
        return false;
    if (sig->nparams > XR_FFI_MAX_ARGS) {
        xr_runtime_error(X, "FFI: CFn callback has too many parameters (%u > %d)\n", sig->nparams,
                         XR_FFI_MAX_ARGS);
        return false;
    }

    memset(bridge, 0, sizeof(*bridge));
    bridge->X = X;
    bridge->closure = closure;
    bridge->sig = sig;
    for (uint8_t i = 0; i < sig->nparams; i++)
        bridge->atypes[i] = ffi_type_for_code(sig->params[i]);

    ffi_type *rtype = ffi_type_for_code(sig->ret);
    if (ffi_prep_cif(&bridge->cif, FFI_DEFAULT_ABI, sig->nparams, rtype, bridge->atypes) !=
        FFI_OK) {
        xr_runtime_error(X, "FFI: failed to prepare CFn callback trampoline\n");
        return false;
    }

    bridge->closure_mem = (ffi_closure *) ffi_closure_alloc(sizeof(ffi_closure), &bridge->code);
    if (!bridge->closure_mem || !bridge->code) {
        ffi_callback_bridge_free(bridge);
        xr_runtime_error(X, "FFI: failed to allocate CFn callback trampoline\n");
        return false;
    }

    if (ffi_prep_closure_loc(bridge->closure_mem, &bridge->cif, ffi_callback_bridge_invoke, bridge,
                             bridge->code) != FFI_OK) {
        ffi_callback_bridge_free(bridge);
        xr_runtime_error(X, "FFI: failed to prepare CFn callback trampoline\n");
        return false;
    }
    return true;
}

/* Resolve `symbol` from a foreign library (or the running process when
 * `dylib` is NULL). Returns NULL when the symbol cannot be found. When loading
 * the requested library itself fails, sets `library_error` so the caller does
 * not emit a misleading second "symbol not found" diagnostic. */
static void *ffi_resolve_symbol(struct XrayIsolate *X, const char *symbol, const char *dylib,
                                bool *library_error) {
    if (library_error)
        *library_error = false;
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
            if (library_error)
                *library_error = true;
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

    bool library_error = false;
    void *fn = ffi_resolve_symbol(X, symbol, sig->dylib, &library_error);
    if (!fn) {
        if (library_error)
            return xr_null();
        xr_runtime_error(X, "FFI: symbol '%s' not found%s%s\n", symbol ? symbol : "?",
                         sig->dylib ? " in library " : "", sig->dylib ? sig->dylib : "");
        return xr_null();
    }

    ffi_type *atypes[XR_FFI_MAX_ARGS];
    void *avalues[XR_FFI_MAX_ARGS];
    XrFFISlot slots[XR_FFI_MAX_ARGS];
    XrFFICallbackBridge callbacks[XR_FFI_MAX_ARGS];
    memset(callbacks, 0, sizeof(callbacks));

    for (int i = 0; i < np; i++) {
        uint8_t code = sig->params[i];
        atypes[i] = ffi_type_for_code(code);
        XrValue a = args[i];

        const XrFFICallbackSig *cb_sig =
            (sig->param_cbacks && code == XR_FFI_T_PTR) ? sig->param_cbacks[i] : NULL;
        if (cb_sig) {
            XrClosure *closure = xr_vm_closure_from_arg(X, a, "FFI CFn callback");
            if (!closure) {
                ffi_callback_bridges_free(callbacks, np);
                return xr_null();
            }
            if (!ffi_callback_bridge_prepare(X, cb_sig, closure, &callbacks[i])) {
                ffi_callback_bridges_free(callbacks, np);
                return xr_null();
            }
            slots[i].ptr = callbacks[i].code;
            avalues[i] = &slots[i].ptr;
            continue;
        }

        avalues[i] = ffi_store_arg_slot(&slots[i], code, a);
    }

    ffi_type *rtype = ffi_type_for_code(sig->ret);

    ffi_cif cif;
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned) np, rtype, atypes) != FFI_OK) {
        xr_runtime_error(X, "FFI: failed to prepare call for '%s'\n", symbol ? symbol : "?");
        ffi_callback_bridges_free(callbacks, np);
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

    XrValue result;
    switch (sig->ret) {
        case XR_FFI_T_VOID:
            result = xr_null();
            break;
        case XR_FFI_T_F32:
            result = xr_float((double) ret.f32);
            break;
        case XR_FFI_T_F64:
            result = xr_float(ret.f64);
            break;
        case XR_FFI_T_BOOL:
            result = xr_bool((int64_t) ret.a != 0);
            break;
        case XR_FFI_T_PTR:
            result = xr_int((int64_t) (intptr_t) ret.ptr);
            break;
        default:
            /* Integer-like: ffi_arg holds the (sign/zero-extended) value. */
            result = xr_int((int64_t) ret.a);
            break;
    }
    ffi_callback_bridges_free(callbacks, np);
    return result;
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
