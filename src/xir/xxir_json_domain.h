/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * The folded JSON domain has seven exact arms, independently of any carrier.
 */
#ifndef XXIR_JSON_DOMAIN_H
#define XXIR_JSON_DOMAIN_H
#include "xxir_types.h"
typedef enum XrXirJsonArm {
    XR_XIR_JSON_NULL_ARM, XR_XIR_JSON_BOOL_ARM, XR_XIR_JSON_I64_ARM,
    XR_XIR_JSON_F64_ARM, XR_XIR_JSON_STRING_ARM, XR_XIR_JSON_ARRAY_ARM,
    XR_XIR_JSON_OBJECT_ARM, XR_XIR_JSON_ARM_COUNT
} XrXirJsonArm;
/* The recursive edge is the unique builtin atom. Ordinary ARRAY/MAP nodes
 * point to it; no arbitrary cyclic descriptor graph is admitted. */
static inline bool xr_xir_json_object_type(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    return node && node->kind==XR_XIR_TYPE_MAP && node->element==XR_XIR_STRING &&
        node->result==XR_XIR_JSON_VALUE && !node->parameters && !node->parameter_count &&
        !node->flags && !node->parameter_span && !node->nominal.declaration &&
        !node->nominal.arguments && !node->nominal.argument_count &&
        !node->nominal.fields && !node->nominal.field_count;
}
static inline bool xr_xir_json_array_type(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    return node && node->kind==XR_XIR_TYPE_ARRAY && node->element==XR_XIR_JSON_VALUE &&
        !node->result && !node->parameters && !node->parameter_count &&
        !node->flags && !node->parameter_span && !node->nominal.declaration &&
        !node->nominal.arguments && !node->nominal.argument_count &&
        !node->nominal.fields && !node->nominal.field_count;
}
/* Scalar widening is exact. Unit is never JSON null; JSON_NULL constructs that
 * arm directly. Typed composites require the explicit JSON.value API. */
static inline bool xr_xir_json_implicit_scalar(XrXirType type) {
    return type==XR_XIR_BOOL || type==XR_XIR_I64 || type==XR_XIR_F64 || type==XR_XIR_STRING;
}
/* Common injection authenticates an exact governed member. This relation does
 * not grant Source implicit conversion of composite values or codec authority. */
static inline XrXirJsonArm xr_xir_json_inject_arm(const XrXirTypes *types,XrXirType type) {
    if(type==XR_XIR_BOOL)return XR_XIR_JSON_BOOL_ARM;
    if(type==XR_XIR_I64)return XR_XIR_JSON_I64_ARM;
    if(type==XR_XIR_F64)return XR_XIR_JSON_F64_ARM;
    if(type==XR_XIR_STRING)return XR_XIR_JSON_STRING_ARM;
    if(xr_xir_json_array_type(types,type))return XR_XIR_JSON_ARRAY_ARM;
    if(xr_xir_json_object_type(types,type))return XR_XIR_JSON_OBJECT_ARM;
    return XR_XIR_JSON_ARM_COUNT;
}
#endif
