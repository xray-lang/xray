/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Ordered tuple facts belong to the actual type pool and construction body. */
#ifndef XHIR_TUPLE_H
#define XHIR_TUPLE_H
#include "xhir_body.h"
#include "xhir_conversion.h"

typedef enum XrHirTupleKind {XR_HIR_TUPLE_OTHER,XR_HIR_TUPLE_UNIT,XR_HIR_TUPLE_ORDERED} XrHirTupleKind;
typedef XrXirStatus (*XrHirTupleIntern)(void *,const XrXirTypeNode *,XrXirType *);
typedef struct XrHirTuplePool {
    const XrXirCompileContext *compile;
    const XrXirTypes *types;
    XrCompileRequestControl *control;
    void *intern_owner;
    XrHirTupleIntern intern;
} XrHirTuplePool;
/* A structural view is a synchronous borrow, never an executable use. */
typedef struct XrHirTupleView {
    const XrXirTypes *owner;
    const XrXirCallableParameter *fields;
    XrXirType type;
    uint32_t count,physical_count;
} XrHirTupleView;
typedef struct XrHirTupleShape {const XrHirBody *owner;uint32_t index;} XrHirTupleShape;
typedef struct XrHirTupleContext {const XrHirBody *owner;uint32_t index;} XrHirTupleContext;
typedef struct XrHirTupleExpected {XrXirType type;bool present,infer_result,inferred;} XrHirTupleExpected;
typedef enum XrHirTupleContextKind {XR_HIR_TUPLE_EXACT,XR_HIR_TUPLE_PREFIX} XrHirTupleContextKind;
typedef struct XrHirTupleContextInput {
    const XrXirTypes *types;
    const XrHirTupleExpected *fields;
    XrXirType type;
    uint32_t count;
    XrHirTupleContextKind kind;
    bool from_type,inferred;
} XrHirTupleContextInput;
typedef struct XrHirTupleField {XrXirType type;uint32_t physical;} XrHirTupleField;
typedef struct XrHirTupleInfo {
    XrXirType type;
    const XrHirTupleField *fields;
    const uint32_t *physical_fields;
    uint32_t count,physical_count;
} XrHirTupleInfo;
typedef struct XrHirTupleValue {XrXirType type;uint32_t value;} XrHirTupleValue;
typedef struct XrHirTupleConstructInput {
    const XrXirTypes *types;
    XrHirBodyScope scope;
    XrHirBodyOrigin origin;
    XrHirTupleShape shape;
    XrHirTupleContext context;
    const XrHirTupleValue *values;
    const XrHirConversionContext *conversion; /* Synchronous existing A2 owner. */
    uint32_t count,context_offset;
} XrHirTupleConstructInput;
typedef struct XrHirTupleFieldInput {
    const XrXirTypes *types;
    XrHirBodyScope scope;
    XrHirBodyOrigin origin;
    XrHirTupleShape shape;
    XrXirType receiver_type;
    uint32_t receiver,field;
} XrHirTupleFieldInput;
typedef struct XrHirTupleAction {
    XrHirBodyUse use;
    XrHirTupleShape shape;
    XrHirTupleInfo tuple;
    XrXirType type;
    XrXirOp operation;
    uint32_t field;
} XrHirTupleAction;

XR_FUNC XrHirTupleKind xr_hir_tuple_kind(const XrXirTypes *,XrXirType);
XR_FUNC XrXirStatus xr_hir_tuple_view(const XrXirTypes *,XrXirType,XrCompileRequestControl *,XrHirTupleView *,const char **);
XR_FUNC XrXirStatus xr_hir_tuple_field_type(const XrXirTypes *,XrXirType,uint32_t,XrCompileRequestControl *,XrXirType *,const char **);
XR_FUNC XrXirStatus xr_hir_tuple_arity(const XrXirTypes *,XrXirType,uint32_t,XrCompileRequestControl *,const char **);
/* The interner is synchronous. The returned identity is checked in its actual
 * receiving pool; input fields are copied before a callback can grow it. */
XR_FUNC XrXirStatus xr_hir_tuple_type(const XrHirTuplePool *,const XrXirCallableParameter *,uint32_t,XrXirType *,const char **);
XR_FUNC XrXirStatus xr_hir_tuple_index(const char *,size_t,XrCompileRequestControl *,uint32_t *,const char **);
XR_FUNC XrXirStatus xr_hir_body_tuple_receive(XrHirBody *,const XrXirTypes *,XrXirType,XrCompileRequestControl *,XrHirTupleShape *,const char **);
XR_FUNC bool xr_hir_body_tuple_info(const XrHirBody *,XrHirTupleShape,XrHirTupleInfo *);
XR_FUNC XrXirStatus xr_hir_body_tuple_context(XrHirBody *,const XrHirTupleContextInput *,XrCompileRequestControl *,XrHirTupleContext *,const char **);
XR_FUNC bool xr_hir_body_tuple_expected(const XrHirBody *,XrHirTupleContext,uint32_t,XrHirTupleExpected *);
XR_FUNC bool xr_hir_body_tuple_context_size(const XrHirBody *,XrHirTupleContext,uint32_t *);
XR_FUNC XrXirStatus xr_hir_body_tuple_context_slice(XrHirBody *,XrHirTupleContext,uint32_t,uint32_t,XrCompileRequestControl *,XrHirTupleContext *);
XR_FUNC XrXirStatus xr_hir_body_tuple_arity(const XrHirBody *,XrHirTupleShape,uint32_t,const char **);
XR_FUNC XrXirStatus xr_hir_body_tuple_construct(XrHirBody *,const XrHirTupleConstructInput *,XrCompileRequestControl *,XrHirTupleAction *,const char **);
XR_FUNC XrXirStatus xr_hir_body_tuple_field(XrHirBody *,const XrHirTupleFieldInput *,XrCompileRequestControl *,XrHirTupleAction *,const char **);
#endif // XHIR_TUPLE_H
