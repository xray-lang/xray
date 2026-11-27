/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License.
 *
 * xxir_builtin_stdlib.h - Private module interface generated from Checked XIR
 */
#ifndef XXIR_BUILTIN_STDLIB_H
#define XXIR_BUILTIN_STDLIB_H

#include "xxir_effects.h"
#include "../base/xstable_id.h"
#include "../shared/xr_param_mode.h"

/* The first module uses scalar types and Array<F64>. An array carries its
 * element type, so a producer's constructed type ordinal never crosses builds. */
typedef struct XrXirBuiltinType {
    XrXirType scalar;
    bool array;
} XrXirBuiltinType;

typedef struct XrXirBuiltinParameter {
    XrXirLiteral name;
    XrXirBuiltinType type;
    XrParamMode mode;
} XrXirBuiltinParameter;

typedef struct XrXirBuiltinFunction {
    XrXirLiteral name;
    const XrXirBuiltinParameter *parameters;
    uint32_t parameter_count;
    XrXirBuiltinType result;
    XrXirType error;
    uint32_t promises;
    XrXirFunctionEffects control;
    XrXirRootEffects root;
} XrXirBuiltinFunction;

typedef struct XrXirBuiltinSlot {
    XrXirLiteral name;
    XrXirType type;
    XrXirConstantKey constant_key;
} XrXirBuiltinSlot;

typedef enum XrXirBuiltinScalarAbi {
    XR_XIR_BUILTIN_F64_UNARY = 1,
    XR_XIR_BUILTIN_F64_TERNARY = 3
} XrXirBuiltinScalarAbi;

typedef struct XrXirBuiltinScalarBinding {
    uint32_t function_row;
    XrXirBuiltinScalarAbi abi;
    union {
        double (*unary)(double);
        double (*ternary)(double, double, double);
    } entry;
} XrXirBuiltinScalarBinding;

/* The initializer's slot indexes and the bindings' function indexes belong
 * to this table. A consumer must prove the same module, target and ABI before
 * using a binding. The Source fingerprint covers the dependency-free module
 * bytes checked by this producer. It does not authenticate a dependency graph. */
typedef struct XrXirBuiltinStdlibModule {
    XrXirLiteral canonical, logical_path;
    XrXirTarget producer_target;
    XrFingerprint source_fingerprint;
    const XrXirBuiltinFunction *functions;
    uint32_t function_count;
    const XrXirBuiltinSlot *slots;
    uint32_t slot_count;
    const XrXirInstruction *initializer;
    uint32_t initializer_count;
    XrXirType initializer_error;
    const XrXirBuiltinScalarBinding *bindings;
    uint32_t binding_count;
} XrXirBuiltinStdlibModule;

extern const XrXirBuiltinStdlibModule xray_stdlib_math_interface;

#endif
