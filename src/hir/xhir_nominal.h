/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Nominal construction owns declaration shapes, helper identities and uses. */
#ifndef XHIR_NOMINAL_H
#define XHIR_NOMINAL_H
#include "xhir_body.h"
#include "../xir/xxir_nominal.h"

typedef struct XrXirConstruction XrXirConstruction;
typedef struct XrHirNominalShape {const XrHirBody *owner;uint32_t index;} XrHirNominalShape;
typedef struct XrHirNominalPlan {const XrHirBody *owner;uint32_t index;} XrHirNominalPlan;
typedef XrXirStatus (*XrHirNominalResolve)(void *,const XrXirType *,uint32_t,XrXirType,XrXirType *);
typedef struct XrHirNominalPool {
    const XrXirCompileContext *compile;
    const XrXirTypes *types;
    XrCompileRequestControl *control;
    void *resolve_owner;
    XrHirNominalResolve resolve;
} XrHirNominalPool;
/* This independent live-pool borrow creates no executable use. */
typedef struct XrHirNominalView {
    const XrXirTypes *owner;
    XrXirType type;
    XrXirNominalKind kind;
    uint32_t declaration,flags,field_count,variant_count,argument_count;
} XrHirNominalView;
typedef struct XrHirNominalSelection {uint32_t index;bool present;} XrHirNominalSelection;
typedef struct XrHirNominalField {
    XrXirLiteral name;
    XrXirType declared,type;
    uint32_t flags;
    bool ready;
} XrHirNominalField;
typedef struct XrHirNominalInfo {
    XrXirType type;
    XrXirNominalKind kind;
    uint32_t declaration,module,flags,count,variant_count,argument_count;
    const XrHirNominalField *fields;
    const XrXirNominalVariant *variants;
    const XrXirType *arguments;
} XrHirNominalInfo;
typedef struct XrHirNominalImport {
    const XrXirTypes *types;
    const XrXirConstruction *construction;
    uint32_t declaration;
} XrHirNominalImport;
typedef struct XrHirNominalDeclarationInput {
    const XrXirTypes *types;
    uint32_t declaration,module,source_declaration;
    const uint32_t *field_initializers;
    bool explicit_initializer;
    const XrHirNominalImport *imported;
} XrHirNominalDeclarationInput;
typedef enum XrHirNominalFunctionKind {
    XR_HIR_NOMINAL_EXPLICIT,XR_HIR_NOMINAL_IMPLICIT,XR_HIR_NOMINAL_FIELD_DEFAULT
} XrHirNominalFunctionKind;
typedef struct XrHirNominalFunction {const XrHirBody *owner;uint32_t index;} XrHirNominalFunction;
typedef struct XrHirNominalFunctionInput {
    uint32_t declaration,function,field,source_declaration;
    XrHirNominalFunctionKind kind;
    XrHirNominalFunction source;
} XrHirNominalFunctionInput;
typedef struct XrHirNominalDefaults {
    uint32_t constructor,implicit_initializer,count;
    const uint32_t *fields;
    bool available,imported;
} XrHirNominalDefaults;
typedef struct XrHirNominalValue {XrXirType type;uint32_t value;XrHirBodyUse producer;} XrHirNominalValue;
typedef enum XrHirNominalPlanKind {XR_HIR_NOMINAL_STRUCT_LITERAL,XR_HIR_NOMINAL_ENUM_LITERAL} XrHirNominalPlanKind;
typedef struct XrHirNominalPlanInput {
    XrHirNominalShape shape;
    XrHirBodyScope scope;
    XrHirBodyOrigin origin;
    uint32_t variant,count;
    XrHirNominalPlanKind kind;
    bool provided,application_pending;
} XrHirNominalPlanInput;
typedef struct XrHirNominalAction {
    XrHirBodyUse use;
    XrHirNominalShape shape;
    XrXirOp operation;
    XrXirType type;
    uint32_t field,variant,function,count;
    const XrHirNominalValue *values;
    const XrXirType *arguments;
    uint32_t argument_count;
} XrHirNominalAction;
typedef struct XrHirNominalUseInput {
    const XrHirNominalPool *pool;
    XrHirNominalShape shape;
    XrHirBodyScope scope;
    XrHirBodyOrigin origin;
    XrXirType type;
    const XrHirNominalValue *values;
    uint32_t count,variant,receiver,field;
} XrHirNominalUseInput;

XR_FUNC XrXirStatus xr_hir_nominal_view(const XrXirTypes *,XrXirType,XrCompileRequestControl *,XrHirNominalView *,const char **);
XR_FUNC XrXirStatus xr_hir_nominal_field_type(const XrHirNominalPool *,XrXirType,uint32_t,XrXirType *,const char **);
XR_FUNC XrXirStatus xr_hir_nominal_variant(const XrXirTypes *,XrXirType,uint32_t,XrCompileRequestControl *,XrXirNominalVariant *,const char **);
XR_FUNC XrXirStatus xr_hir_nominal_variant_find(const XrXirTypes *,XrXirType,XrXirLiteral,XrCompileRequestControl *,XrHirNominalSelection *);
XR_FUNC XrXirStatus xr_hir_body_nominal_modules(XrHirBody *,XrXirSourceModule *const *,XrXirGeneric *const *);
XR_FUNC XrXirStatus xr_hir_body_nominal_declare(XrHirBody *,const XrHirNominalDeclarationInput *,XrCompileRequestControl *,const char **);
XR_FUNC XrXirStatus xr_hir_body_nominal_schedule(XrHirBody *,uint32_t,uint32_t,XrCompileRequestControl *,uint32_t *,uint32_t *);
XR_FUNC XrXirStatus xr_hir_body_nominal_bind(XrHirBody *,const XrHirNominalFunctionInput *,XrCompileRequestControl *);
XR_FUNC XrXirStatus xr_hir_body_nominal_import_bind(XrHirBody *,uint32_t,const uint32_t *,uint32_t,XrCompileRequestControl *);
XR_FUNC bool xr_hir_body_nominal_defaults(const XrHirBody *,uint32_t,XrHirNominalDefaults *);
XR_FUNC bool xr_hir_body_nominal_defaultable(const XrHirBody *,XrXirType);
XR_FUNC XrXirStatus xr_hir_body_nominal_receive(XrHirBody *,const XrXirTypes *,XrXirType,XrCompileRequestControl *,XrHirNominalShape *,const char **);
XR_FUNC bool xr_hir_body_nominal_info(const XrHirBody *,XrHirNominalShape,XrHirNominalInfo *);
XR_FUNC XrXirStatus xr_hir_body_nominal_field(XrHirBody *,XrHirNominalShape,const XrHirNominalPool *,uint32_t,XrHirNominalField *,const char **);
XR_FUNC XrXirStatus xr_hir_body_nominal_begin(XrHirBody *,const XrHirNominalPlanInput *,XrCompileRequestControl *,XrHirNominalPlan *,const char **);
XR_FUNC XrXirStatus xr_hir_body_nominal_apply(XrHirBody *,XrHirNominalPlan,XrHirNominalShape,XrCompileRequestControl *,const char **);
XR_FUNC XrXirStatus xr_hir_body_nominal_find(const XrHirBody *,XrHirNominalShape,XrXirLiteral,XrCompileRequestControl *,uint32_t *,const char **);
XR_FUNC XrXirStatus xr_hir_body_nominal_select(XrHirBody *,XrHirNominalPlan,XrXirLiteral,XrCompileRequestControl *,uint32_t *,const char **);
XR_FUNC XrXirStatus xr_hir_body_nominal_supply(XrHirBody *,XrHirNominalPlan,const XrHirNominalPool *,uint32_t,XrHirNominalValue,const char **);
XR_FUNC XrXirStatus xr_hir_body_nominal_missing(const XrHirBody *,XrHirNominalPlan,uint32_t,XrCompileRequestControl *,bool *,const char **);
XR_FUNC XrXirStatus xr_hir_body_nominal_complete(XrHirBody *,XrHirNominalPlan,const XrHirNominalPool *,XrHirNominalAction *,const char **);
XR_FUNC XrXirStatus xr_hir_body_nominal_construct(XrHirBody *,const XrHirNominalUseInput *,XrCompileRequestControl *,XrHirNominalAction *,const char **);
XR_FUNC XrXirStatus xr_hir_body_nominal_get(XrHirBody *,const XrHirNominalUseInput *,XrCompileRequestControl *,XrHirNominalAction *,const char **);
XR_FUNC XrXirStatus xr_hir_body_nominal_default(XrHirBody *,const XrHirNominalUseInput *,XrCompileRequestControl *,XrHirNominalAction *,const char **);
XR_FUNC XrXirStatus xr_hir_body_nominal_helper(XrHirBody *,const XrHirNominalUseInput *,XrCompileRequestControl *,XrHirNominalAction *,const char **);
#endif // XHIR_NOMINAL_H
