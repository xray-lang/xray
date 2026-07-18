/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_simd_lower.c - Portable simd surface -> typed Xi vector operations
 */

#include "xi_simd_lower.h"
#include "../ir/xi_effect.h"
#include "../runtime/class/xclass_info.h"
#include "../shared/xr_native_type_core.h"

#include <string.h>

typedef struct XiSimdShape {
    uint8_t lanes;
    uint8_t native_type;
} XiSimdShape;

static bool simd_shape_named(const char *name, XiSimdShape *out) {
    XiSimdShape shape = {0, 0};
    if (!name)
        return false;
    if (strcmp(name, "U8x16") == 0)
        shape = (XiSimdShape) {16, XR_NATIVE_U8};
    else if (strcmp(name, "U32x4") == 0)
        shape = (XiSimdShape) {4, XR_NATIVE_U32};
    else if (strcmp(name, "U64x2") == 0)
        shape = (XiSimdShape) {2, XR_NATIVE_U64};
    else
        return false;
    if (out)
        *out = shape;
    return true;
}

static bool path_is_simd(const char *path) {
    static const char suffix[] = "stdlib/simd/simd.xr";
    if (!path)
        return false;
    size_t n = strlen(path);
    size_t sn = sizeof(suffix) - 1;
    return n >= sn && strcmp(path + n - sn, suffix) == 0;
}

static bool simd_shape_type(const XrType *type, XiSimdShape *out) {
    if (!type)
        return false;
    if (type->kind == XR_KIND_POINTER)
        return simd_shape_type(type->container.element_type, out);
    if (type->kind != XR_KIND_INSTANCE && type->kind != XR_KIND_CLASS)
        return false;
    const XrClassInfo *info = type->instance.class_ref;
    const char *name = info && info->name ? info->name : type->instance.class_name;
    if (!simd_shape_named(name, out))
        return false;
    return info && path_is_simd(info->location.file);
}

static bool simd_shape_static_receiver(const XiModule *module, const XiValue *receiver,
                                       XiSimdShape *out);

static bool simd_shape_value(const XaotBundle *bundle, const XiModule *module, const XiValue *value,
                             XiSimdShape *out) {
    if (!value)
        return false;
    for (unsigned depth = 0; value && depth < 8; depth++) {
        if ((value->op == XI_COPY || value->op == XI_MOVE || value->op == XI_LOCAL_ADDR ||
             value->op == XI_PLACE_LOAD) &&
            value->nargs == 1 && value->args[0]) {
            value = value->args[0];
            continue;
        }
        break;
    }
    if (value->op >= XI_VEC_LOAD && value->op <= XI_VEC_REDUCE_ADD &&
        xi_vec_shape_is_explicit(value->aux_int)) {
        if (out) {
            out->lanes = xi_vec_shape_lanes(value->aux_int);
            out->native_type = xi_vec_shape_native_type(value->aux_int);
        }
        return true;
    }
    if (simd_shape_type(value->type, out))
        return true;
    const XaotValuePlan *plan = xaot_bundle_find_value_plan(bundle, value);
    if (plan && simd_shape_type(plan->rep.type, out))
        return true;
    /* fromLanes deliberately remains an ordinary scalar semantic call, but
     * its exact imported class receiver still proves the produced shape for
     * subsequent typed operations. */
    return value->op == XI_CALL_METHOD && value->nargs == 2 && value->aux &&
           strcmp((const char *) value->aux, "fromLanes") == 0 &&
           simd_shape_static_receiver(module, value->args[0], out);
}

static bool simd_shape_static_receiver(const XiModule *module, const XiValue *receiver,
                                       XiSimdShape *out) {
    if (!module || !receiver)
        return false;
    while ((receiver->op == XI_COPY || receiver->op == XI_MOVE) && receiver->nargs == 1)
        receiver = receiver->args[0];
    if (receiver->op != XI_GET_SHARED || receiver->aux_int < 0 ||
        receiver->aux_int >= module->nslots || !module->slot_imports)
        return false;
    const XiImportRef *ref = module->slot_imports[receiver->aux_int];
    return ref && ref->module_path && strcmp(ref->module_path, "simd") == 0 &&
           simd_shape_named(ref->member_name, out);
}

static bool const_lane(const XiValue *value, uint8_t lanes, uint8_t *out) {
    if (!value || value->op != XI_CONST || value->aux_int < 0 || value->aux_int >= lanes)
        return false;
    if (out)
        *out = (uint8_t) value->aux_int;
    return true;
}

static void rewrite_vec(XiValue *value, XiOp op, uint16_t drop_prefix, uint16_t nargs,
                        XiSimdShape result_shape, int64_t extra) {
    if (drop_prefix != 0) {
        for (uint16_t i = 0; i < nargs; i++)
            value->args[i] = value->args[i + drop_prefix];
    }
    value->op = (uint16_t) op;
    value->nargs = nargs;
    value->aux_int = xi_vec_shape_encode(result_shape.native_type, result_shape.lanes) | extra;
    value->aux = NULL;
    value->aux_kind = XI_AUX_KIND_NONE;
    value->call_plan = NULL;
    value->flags = xi_op_default_effects(op);
    value->xg_callsite_id = 0;
    value->xg_method_id = 0;
    value->xg_interface_dispatch_slot = UINT32_MAX;
}

static bool lower_static_call(const XiModule *module, XiValue *value, const char *method,
                              XiSimdShape shape) {
    (void) module;
    if (strcmp(method, "splat") == 0 && value->nargs == 2) {
        rewrite_vec(value, XI_VEC_SPLAT, 1, 1, shape, 0);
        return true;
    }
    if (strcmp(method, "load") == 0 && value->nargs == 3) {
        rewrite_vec(value, XI_VEC_LOAD, 1, 2, shape, 0);
        return true;
    }
    return false;
}

static bool lower_instance_call(const XaotBundle *bundle, const XiModule *module, XiValue *value,
                                const char *method, XiSimdShape input) {
    XiSimdShape result = input;
    XiOp op = XI_OP_COUNT;
    int64_t extra = 0;
    uint16_t nargs = value->nargs;

    if (strcmp(method, "store") == 0 && nargs == 3)
        op = XI_VEC_STORE;
    else if (strcmp(method, "extract") == 0 && nargs == 2)
        op = XI_VEC_EXTRACT;
    else if (strcmp(method, "replace") == 0 && nargs == 3)
        op = XI_VEC_REPLACE;
    else if (strcmp(method, "add") == 0 && nargs == 2)
        op = XI_VEC_ADD;
    else if (strcmp(method, "sub") == 0 && nargs == 2)
        op = XI_VEC_SUB;
    else if (strcmp(method, "mul") == 0 && nargs == 2)
        op = XI_VEC_MUL;
    else if (strcmp(method, "bitAnd") == 0 && nargs == 2)
        op = XI_VEC_BIT_AND;
    else if (strcmp(method, "bitOr") == 0 && nargs == 2)
        op = XI_VEC_BIT_OR;
    else if (strcmp(method, "bitXor") == 0 && nargs == 2)
        op = XI_VEC_BIT_XOR;
    else if (strcmp(method, "bitNot") == 0 && nargs == 1)
        op = XI_VEC_BIT_NOT;
    else if (strcmp(method, "shiftLeft") == 0 && nargs == 2)
        op = XI_VEC_SHL;
    else if (strcmp(method, "shiftRight") == 0 && nargs == 2)
        op = XI_VEC_SHR;
    else if (strcmp(method, "reduceAdd") == 0 && nargs == 1)
        op = XI_VEC_REDUCE_ADD;
    else if (strcmp(method, "widenMulEven") == 0 && nargs == 2) {
        op = XI_VEC_WIDEN_MUL;
        result = (XiSimdShape) {2, XR_NATIVE_U64};
    } else if (strcmp(method, "widenMulOdd") == 0 && nargs == 2) {
        op = XI_VEC_WIDEN_MUL;
        result = (XiSimdShape) {2, XR_NATIVE_U64};
        extra = XI_VEC_SHAPE_ODD_LANES;
    } else if (strcmp(method, "reinterpretU8") == 0 && nargs == 1) {
        op = XI_VEC_REINTERPRET;
        result = (XiSimdShape) {16, XR_NATIVE_U8};
    } else if (strcmp(method, "reinterpretU32") == 0 && nargs == 1) {
        op = XI_VEC_REINTERPRET;
        result = (XiSimdShape) {4, XR_NATIVE_U32};
    } else if (strcmp(method, "reinterpretU64") == 0 && nargs == 1) {
        op = XI_VEC_REINTERPRET;
        result = (XiSimdShape) {2, XR_NATIVE_U64};
    } else if (strcmp(method, "swapLanes") == 0 && nargs == 1 && input.lanes == 2) {
        op = XI_VEC_SHUFFLE;
        extra = (INT64_C(1) << XI_VEC_SHAPE_SHUFFLE_SHIFT);
    } else if (strcmp(method, "swapAdjacent") == 0 && nargs == 1 && input.lanes == 4) {
        op = XI_VEC_SHUFFLE;
        extra = (INT64_C(1) << XI_VEC_SHAPE_SHUFFLE_SHIFT) |
                (INT64_C(0) << (XI_VEC_SHAPE_SHUFFLE_SHIFT + 4)) |
                (INT64_C(3) << (XI_VEC_SHAPE_SHUFFLE_SHIFT + 8)) |
                (INT64_C(2) << (XI_VEC_SHAPE_SHUFFLE_SHIFT + 12));
    } else if (strcmp(method, "shuffle") == 0 && nargs == (uint16_t) (input.lanes + 1)) {
        op = XI_VEC_SHUFFLE;
        for (uint8_t lane = 0; lane < input.lanes; lane++) {
            uint8_t selected = 0;
            if (!const_lane(value->args[lane + 1], input.lanes, &selected))
                return false;
            extra |= (int64_t) selected << (XI_VEC_SHAPE_SHUFFLE_SHIFT + lane * 4);
        }
        nargs = 1;
    } else {
        return false;
    }

    if (op == XI_VEC_REINTERPRET || op == XI_VEC_WIDEN_MUL) {
        XiSimdShape prepared_result;
        if (simd_shape_value(bundle, module, value, &prepared_result))
            result = prepared_result;
    }
    rewrite_vec(value, op, 0, nargs, result, extra);
    return true;
}

static uint32_t lower_func(const XaotBundle *bundle, const XiModule *module, XiFunc *func) {
    uint32_t count = 0;
    if (!func)
        return 0;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            XiValue *value = block->values[vi];
            if (!value || value->op != XI_CALL_METHOD || value->nargs < 1 || !value->aux)
                continue;
            const char *method = (const char *) value->aux;
            XiSimdShape shape;
            if (simd_shape_static_receiver(module, value->args[0], &shape)) {
                if (lower_static_call(module, value, method, shape))
                    count++;
                continue;
            }
            if (simd_shape_value(bundle, module, value->args[0], &shape) &&
                lower_instance_call(bundle, module, value, method, shape))
                count++;
        }
    }
    for (uint16_t i = 0; i < func->nchildren; i++)
        count += lower_func(bundle, module, func->children[i]);
    return count;
}

XR_FUNC bool xi_simd_lower_bundle(XaotBundle *bundle, uint32_t *lowered_count) {
    if (!bundle || !bundle->modules)
        return false;
    uint32_t count = 0;
    for (uint32_t i = 0; i < bundle->nmodules; i++) {
        XiModule *module = bundle->modules[i];
        if (!module || !module->init || path_is_simd(module->path))
            continue;
        count += lower_func(bundle, module, module->init);
    }
    if (lowered_count)
        *lowered_count = count;
    return true;
}
