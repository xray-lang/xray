/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_math_native.h - Canonical scalar math signatures and standard-module authority
 *
 * KEY CONCEPT:
 *   A full generated free declaration and the owning standard module precede execution.
 */
#ifndef XXIR_MATH_NATIVE_H
#define XXIR_MATH_NATIVE_H
#include "xxir_declarations.h"
#include "../shared/xnative_declaration.h"
#include <string.h>

typedef struct XirMathNativeShape {
    const char *name,*signature;
    uint32_t count;
    bool integer_input,integer_result;
} XirMathNativeShape;
static inline bool xir_math_native_id(uint32_t id) {
    return id>=XR_NATIVE_CALLABLE_MATH_ABS && id<=XR_NATIVE_CALLABLE_MATH_EXPM1;
}
static inline bool xir_math_native_random(uint32_t id) {
    return id==XR_NATIVE_CALLABLE_MATH_RANDOM || id==XR_NATIVE_CALLABLE_MATH_RANDOM_INT;
}
static inline const XirMathNativeShape *xir_math_native_shape(uint32_t id) {
    static const XirMathNativeShape shapes[]={
        {"__abs","(x: f64): f64",1,false,false},
        {"__floor","(x: f64): i64",1,false,true},
        {"__ceil","(x: f64): i64",1,false,true},
        {"__round","(x: f64): i64",1,false,true},
        {"__sqrt","(x: f64): f64",1,false,false},
        {"__pow","(base: f64, exp: f64): f64",2,false,false},
        {"__sin","(x: f64): f64",1,false,false},
        {"__cos","(x: f64): f64",1,false,false},
        {"__tan","(x: f64): f64",1,false,false},
        {"__asin","(x: f64): f64",1,false,false},
        {"__acos","(x: f64): f64",1,false,false},
        {"__atan","(x: f64): f64",1,false,false},
        {"__atan2","(y: f64, x: f64): f64",2,false,false},
        {"__log","(x: f64): f64",1,false,false},
        {"__log10","(x: f64): f64",1,false,false},
        {"__log2","(x: f64): f64",1,false,false},
        {"__exp","(x: f64): f64",1,false,false},
        {"__random","(): f64",0,false,false},
        {"__randomInt","(min: i64, max: i64): i64",2,true,true},
        {"__sinh","(x: f64): f64",1,false,false},
        {"__cosh","(x: f64): f64",1,false,false},
        {"__tanh","(x: f64): f64",1,false,false},
        {"__hypot","(x: f64, y: f64): f64",2,false,false},
        {"__cbrt","(x: f64): f64",1,false,false},
        {"__trunc","(x: f64): i64",1,false,true},
        {"__fmod","(x: f64, y: f64): f64",2,false,false},
        {"__log1p","(x: f64): f64",1,false,false},
        {"__expm1","(x: f64): f64",1,false,false},
    };
    return xir_math_native_id(id) && id-XR_NATIVE_CALLABLE_MATH_ABS<sizeof(shapes)/sizeof(*shapes) ?
        &shapes[id-XR_NATIVE_CALLABLE_MATH_ABS] : NULL;
}
static inline bool xir_math_native_signature(uint32_t id,const XrXirType *parameters,
    uint32_t count,XrXirType result,XrXirType error) {
    const XirMathNativeShape *shape=xir_math_native_shape(id);
    const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
    if(!shape || !native || native->owner || native->error_e_type ||
        native->ctfe==xir_math_native_random(id) || !native->namespace_id || strcmp(native->namespace_id,"math") ||
        !native->source_path || strcmp(native->source_path,"stdlib/defs/core.def") || !native->member)
        return false;
    const XrNativeMemberDeclaration *member=native->member;
    if(!member->name || strcmp(member->name,shape->name) || !member->signature || strcmp(member->signature,shape->signature) ||
        member->is_public || member->is_method || member->is_static || member->receiver!=XR_NATIVE_RECEIVER_READ ||
        member->operation!=XR_NATIVE_OPERATION_NONE || member->allocation!=XR_NATIVE_ALLOCATION_UNKNOWN ||
        !member->failures || *member->failures || member->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED ||
        member->parameter_count!=shape->count || count!=shape->count || (!!parameters!=!!count) ||
        (!!member->parameters!=!!count) || error!=XR_XIR_NEVER ||
        member->result!=(shape->integer_result?XR_NATIVE_TERM_I64:XR_NATIVE_TERM_F64) ||
        result!=(shape->integer_result?XR_XIR_I64:XR_XIR_F64)) return false;
    for(uint32_t p=0;p<count;++p) {
        if(member->parameters[p].type!=(shape->integer_input?XR_NATIVE_TERM_I64:XR_NATIVE_TERM_F64) ||
            member->parameters[p].optional || member->parameters[p].variadic ||
            parameters[p]!=(shape->integer_input?XR_XIR_I64:XR_XIR_F64)) return false;
    }
    return true;
}
static inline bool xir_math_native_scope(const XrXirDeclarations *declarations,
    uint32_t function,uint32_t count) {
    static const char identity[]="stdlib-module-v1:module=4:math:path=12:math/math.xr";
    if(!declarations || !declarations->functions || !declarations->modules || function>=count) return false;
    uint32_t owner=declarations->functions[function].module;
    if(owner>=declarations->module_count) return false;
    const XrXirSourceModule *module=declarations->modules+owner;
    return module->name && module->name_length==sizeof(identity)-1 && !module->lexical_scope &&
        !memcmp(module->name,identity,sizeof(identity)-1);
}
#endif // XXIR_MATH_NATIVE_H
