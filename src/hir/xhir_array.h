/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_array.h - Owned Array shapes, ordered contexts and executable uses
 *
 * KEY CONCEPT:
 *   Structural metadata grants no emission authority. Each executable Array
 *   use belongs to its real type pool, lexical scope and ordered operands.
 */
#ifndef XHIR_ARRAY_H
#define XHIR_ARRAY_H
#include "xhir_body.h"
#include "../shared/xnative_declaration.h"

typedef struct XrHirArrayShape {const XrHirBody *owner;uint32_t index;} XrHirArrayShape;
typedef struct XrHirArrayLiteral {const XrHirBody *owner;uint32_t index;} XrHirArrayLiteral;
typedef struct XrHirArrayMember {const XrHirBody *owner;uint32_t index;} XrHirArrayMember;
typedef enum XrHirArrayRecipe {
    XR_HIR_ARRAY_NONE,XR_HIR_ARRAY_MAP,XR_HIR_ARRAY_FILTER,XR_HIR_ARRAY_REDUCE,XR_HIR_ARRAY_FOR_EACH,
    XR_HIR_ARRAY_FIND,XR_HIR_ARRAY_FIND_INDEX,XR_HIR_ARRAY_EVERY,XR_HIR_ARRAY_SOME,
    XR_HIR_ARRAY_CONTAINS,XR_HIR_ARRAY_INDEX_OF,XR_HIR_ARRAY_JOIN,XR_HIR_ARRAY_CLEAR,
    XR_HIR_ARRAY_REVERSE,XR_HIR_ARRAY_UNSHIFT,XR_HIR_ARRAY_POP,XR_HIR_ARRAY_SHIFT,XR_HIR_ARRAY_RESIZE,
    XR_HIR_ARRAY_ENTRIES,XR_HIR_ARRAY_FILL,XR_HIR_ARRAY_CONCAT
} XrHirArrayRecipe;
typedef enum XrHirArrayMemberRole {XR_HIR_ARRAY_INSTANCE,XR_HIR_ARRAY_STATIC,XR_HIR_ARRAY_PROPERTY} XrHirArrayMemberRole;
typedef struct XrHirArrayMemberInfo {
    uint32_t ordinal,native_id,parameter_count;
    XrNativeOperation operation;
    XrNativeReceiverMode receiver;
    XrHirArrayRecipe recipe;
    XrHirArrayMemberRole role;
} XrHirArrayMemberInfo;
typedef struct XrHirArrayInfo {XrXirType type,element;} XrHirArrayInfo;
typedef struct XrHirArrayValue {XrXirType type;uint32_t value;} XrHirArrayValue;
typedef struct XrHirArrayExpected {XrXirType type;bool present,inferred;} XrHirArrayExpected;
typedef XrXirStatus (*XrHirArrayIntern)(void *,const XrXirTypeNode *,XrXirType *);
typedef enum XrHirArrayTypeScope {
    XR_HIR_ARRAY_NOMINAL_DECLARATION,XR_HIR_ARRAY_INTERFACE_DECLARATION,
    XR_HIR_ARRAY_FUNCTION_SIGNATURE,XR_HIR_ARRAY_FUNCTION_BODY
} XrHirArrayTypeScope;
typedef struct XrHirArrayPool {
    const XrXirCompileContext *compile;
    const XrXirTypes *types;
    const XrXirModule *module;
    XrCompileRequestControl *control;
    void *intern_owner;
    XrHirArrayIntern intern;
} XrHirArrayPool;
typedef struct XrHirArrayTypeInput {
    XrHirArrayTypeScope scope;
    XrXirType element;
    uint32_t declaration,function;
    /* Unpublished interface-method parameters are an H1 declaration fact.
     * This pending type does not qualify an executable operation. */
    uint32_t extra_parameters;
} XrHirArrayTypeInput;
typedef struct XrHirArrayLiteralInput {
    const XrXirTypes *types;
    XrHirBodyScope scope;
    XrHirBodyOrigin origin;
    XrXirType expected;
    uint32_t count;
    bool has_expected,inferred;
} XrHirArrayLiteralInput;
typedef struct XrHirArrayPrimitiveInput {
    const XrXirTypes *types;
    XrHirBodyScope scope;
    XrHirBodyOrigin origin;
    XrHirArrayShape shape;
    XrXirOp operation;
    const XrHirArrayValue *values;
    uint32_t count;
} XrHirArrayPrimitiveInput;
typedef struct XrHirArrayAction {
    XrHirBodyUse use;
    XrHirArrayShape shape;
    XrXirType type;
    XrXirOp operation;
} XrHirArrayAction;

/* Structural metadata does not create an executable use or borrow a body. */
XR_FUNC XrXirStatus xr_hir_array_view(const XrXirTypes *,XrXirType,
    XrCompileRequestControl *,XrHirArrayInfo *,const char **);
XR_FUNC XrXirStatus xr_hir_array_type(const XrHirArrayPool *,const XrHirArrayTypeInput *,
    XrXirType *,const char **);
XR_FUNC XrXirStatus xr_hir_body_array_receive(XrHirBody *,const XrXirTypes *,XrXirType,
    XrCompileRequestControl *,XrHirArrayShape *,const char **);
XR_FUNC bool xr_hir_body_array_info(const XrHirBody *,XrHirArrayShape,XrHirArrayInfo *);
XR_FUNC XrXirStatus xr_hir_body_array_primitive(XrHirBody *,const XrHirArrayPrimitiveInput *,
    XrCompileRequestControl *,XrHirArrayAction *,const char **);
XR_FUNC XrXirStatus xr_hir_body_array_literal(XrHirBody *,const XrHirArrayLiteralInput *,
    XrCompileRequestControl *,XrHirArrayLiteral *,const char **);
XR_FUNC bool xr_hir_body_array_expected(const XrHirBody *,XrHirArrayLiteral,XrHirArrayExpected *);
XR_FUNC XrXirStatus xr_hir_body_array_append(XrHirBody *,XrHirArrayLiteral,uint32_t,XrHirArrayValue,
    XrCompileRequestControl *,const char **);
XR_FUNC XrXirStatus xr_hir_body_array_literal_finish(XrHirBody *,XrHirArrayLiteral,XrHirArrayShape,
    XrCompileRequestControl *,XrHirArrayAction *,const char **);
XR_FUNC XrXirStatus xr_hir_array_member_view(const XrNativeMemberDeclaration *,XrCompileRequestControl *,
    XrHirArrayMemberInfo *,const char **);
XR_FUNC XrHirArrayRecipe xr_hir_array_recipe(const XrNativeMemberDeclaration *);
XR_FUNC XrXirStatus xr_hir_body_array_member(XrHirBody *,const XrXirTypes *,const XrNativeMemberDeclaration *,
    XrHirArrayMemberRole,XrCompileRequestControl *,XrHirArrayMember *,XrHirArrayMemberInfo *,const char **);
XR_FUNC bool xr_hir_body_array_member_info(const XrHirBody *,XrHirArrayMember,XrHirArrayMemberInfo *);
#endif // XHIR_ARRAY_H
