/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_body.h - Typed binding, place and call uses on one live type owner
 */
#ifndef XHIR_BODY_H
#define XHIR_BODY_H
#include "../xir/xxir.h"
#include "../base/xcompile_request.h"
#include "../shared/xr_param_mode.h"

typedef struct XrHirBody XrHirBody;
typedef struct XrHirBodyUse {const XrHirBody *owner;uint32_t index;} XrHirBodyUse;
typedef struct XrHirBodyBinding {const XrHirBody *owner;uint32_t index;} XrHirBodyBinding;
typedef struct XrHirBodySignature {const XrHirBody *owner;uint32_t index;} XrHirBodySignature;
typedef struct XrHirBodyPlace {const XrHirBody *owner;uint32_t index;} XrHirBodyPlace;
typedef struct XrHirBodyScope {const XrHirBody *owner;uint32_t index;} XrHirBodyScope;
typedef struct XrHirBodyOrigin {
    uint32_t function,module,node,binding;
    int line,column,end_line,end_column;
} XrHirBodyOrigin;
typedef enum XrHirBodyRule {XR_HIR_BODY_IDENTITY,XR_HIR_BODY_EQUAL,XR_HIR_BODY_PROJECTION} XrHirBodyRule;
typedef enum XrHirBodyStorage {
    XR_HIR_BODY_TEMPORARY,XR_HIR_BODY_LOCAL,XR_HIR_BODY_SLOT,XR_HIR_BODY_UNIT_LOCAL,
    XR_HIR_BODY_UNIT_SLOT
} XrHirBodyStorage;
typedef struct XrHirBodyInput {
    XrHirBodyOrigin origin,definition;
    XrHirBodyRule rule;
    XrXirType source,target,key;
    uint32_t formal,receiver_count;
    const char *failure; /* Static diagnostic spelling; never retained. */
} XrHirBodyInput;
typedef struct XrHirBodyAction {
    XrXirType source,target,key;
    XrXirOp operation;
    uint32_t formal,argument,receiver_count,argument_count,field;
    bool reference;
} XrHirBodyAction;
typedef struct XrHirBindingKey {uint32_t module,declaration,node,parent_declaration,function,storage;} XrHirBindingKey;
typedef struct XrHirBindingInput {
    const XrXirTypes *types;
    XrHirBindingKey key;
    XrHirBodyScope scope;
    XrHirBodyOrigin origin;
    const char *name;
    size_t name_length;
    XrXirType logical,physical;
    XrXirType annotation;
    XrHirBodyStorage storage;
    XrParamMode mode;
    uint32_t parameter,location,reader,writer;
    bool rebindable,construction,direct_borrowed,has_annotation,is_parameter,history;
} XrHirBindingInput;
typedef enum XrHirBindingUseKind {
    XR_HIR_BINDING_READ,XR_HIR_BINDING_ASSIGN,XR_HIR_BINDING_WRITE,XR_HIR_BINDING_CELL,
    XR_HIR_BINDING_REFERENCE,XR_HIR_BINDING_INITIALIZE,XR_HIR_BINDING_PARAMETER,XR_HIR_BINDING_CAPTURE,XR_HIR_BINDING_COMPOUND,XR_HIR_BINDING_INCREMENT,XR_HIR_BINDING_PUBLISH
} XrHirBindingUseKind;
typedef struct XrHirBindingUseInput {
    const XrXirTypes *types;
    XrHirBodyScope scope;
    XrHirBodyOrigin origin;
    XrHirBindingUseKind kind;
    XrXirType value,root_type;
    uint32_t value_id;
    bool projected;
    const char *type_failure;
} XrHirBindingUseInput;
typedef enum XrHirBindingRoute {
    XR_HIR_BINDING_DIRECT,XR_HIR_BINDING_UNIT,XR_HIR_BINDING_LOCAL_CELL,
    XR_HIR_BINDING_SLOT_VALUE,XR_HIR_BINDING_SLOT_CELL
} XrHirBindingRoute;
typedef struct XrHirBindingAction {
    XrHirBodyUse use;
    XrHirBodyBinding binding;
    XrHirBindingRoute route;
    XrXirType logical,physical;
    XrXirOp operation;
    uint32_t location,accessor;
    bool history;
} XrHirBindingAction;
/* These views are the real surrounding construction's descriptors. They are
 * synchronous borrows and close with the type view; no numeric revision grants
 * declaration authority. Owned scopes and signatures snapshot received facts. */
typedef struct XrHirBodyDeclarations {
    XrXirFunction *const *functions;
    XrXirFunctionIdentity *const *identities;
    const uint32_t *function_count,*module_count;
    const XrXirDefaultTable *defaults;
    const XrXirInterfaceTable *interfaces;
    XrXirSlot *const *slots;
    const uint32_t *slot_count;
} XrHirBodyDeclarations;
typedef enum XrHirPlaceKind {
    XR_HIR_PLACE_ROOT,XR_HIR_PLACE_BORROW_ROOT,XR_HIR_PLACE_TEMPORARY,XR_HIR_PLACE_MEMBER,XR_HIR_PLACE_CONSTRUCTOR,
    XR_HIR_PLACE_ROUTE,XR_HIR_PLACE_FIELD,XR_HIR_PLACE_INDEX,XR_HIR_PLACE_INDEX_READ,XR_HIR_PLACE_INDEX_WRITE,XR_HIR_PLACE_SHAPE,XR_HIR_PLACE_ARRAY_SHAPE,XR_HIR_PLACE_HINT_SHAPE,XR_HIR_PLACE_FIELD_BASE,XR_HIR_PLACE_REF_LEAF,
    XR_HIR_PLACE_SELECT,XR_HIR_PLACE_RESOLVED_FIELD,XR_HIR_PLACE_READ,XR_HIR_PLACE_WRITE,XR_HIR_PLACE_REFERENCE
} XrHirPlaceKind;
typedef struct XrHirPlaceInput {
    const XrXirTypes *types;
    XrHirBodyOrigin origin;
    XrHirBodyScope scope;
    XrHirBodyBinding root;
    XrHirBodyPlace parent;
    XrHirPlaceKind kind;
    XrXirType type,key,value;
    uint32_t field,access,selector,base,value_id;
    bool reference,narrowed,projected;
    const char *failure;
} XrHirPlaceInput;
typedef struct XrHirPlaceAction {
    XrHirBodyPlace place;
    XrHirBodyUse use;
    XrXirType type,key,declared;
    XrXirOp operation;
    uint32_t field,location;
    int64_t immediate;
    bool object_root;
} XrHirPlaceAction;
typedef enum XrHirCallFamily {
    XR_HIR_CALL_FUNCTION,XR_HIR_CALL_REQUIREMENT,XR_HIR_CALL_CONSTRUCTOR,
    XR_HIR_CALL_INDIRECT,XR_HIR_CALL_NATIVE,XR_HIR_CALL_VARIADIC_READ,XR_HIR_CALL_ARRAY
} XrHirCallFamily;
typedef struct XrHirArrayCallbackShape {
    XrXirType parameters[2],result,error;
    uint32_t minimum,maximum;
    bool has_result;
} XrHirArrayCallbackShape;
typedef struct XrHirSignatureInput {
    const XrXirTypes *types;
    XrHirBodyScope scope;
    XrHirBodyOrigin origin;
    XrHirCallFamily family;
    uint32_t function,count,receiver_count,parameter_offset,interface_declaration,interface_member,callee_value;
    XrXirType callable,receiver_type;
    const XrXirType *native_parameters;
    const XrCallArgAccess *constructor_accesses;
    uint32_t constructor_argument_count;
    XrXirType array,accumulator,callback_error;
    const struct XrNativeMemberDeclaration *array_member;
    uint32_t array_value;
} XrHirSignatureInput;
typedef enum XrHirCallUseKind {XR_HIR_CALL_PLAN,XR_HIR_CALL_ACCESS,XR_HIR_CALL_VALUE,XR_HIR_CALL_EXACT,XR_HIR_CALL_FINAL,XR_HIR_CALL_SPREAD} XrHirCallUseKind;
typedef struct XrHirCallUseInput {
    const XrXirTypes *types;
    XrHirBodyScope scope;
    XrHirBodyOrigin origin;
    XrHirCallUseKind kind;
    uint32_t formal,argument,spread_field,default_function;
    XrXirType type,expected;
    const XrXirType *substitution;
    uint32_t substitution_count;
    uint32_t value;
    bool spread,defaulted,check_type;
    XrCallArgAccess access;
    const char *failure;
    const char **reason; /* Synchronous diagnostic output; never retained. */
} XrHirCallUseInput;
XR_FUNC XrXirStatus xr_hir_body_signature(XrHirBody *,const XrHirSignatureInput *,
    XrCompileRequestControl *,XrHirBodySignature *,const char **);
XR_FUNC XrXirStatus xr_hir_body_call_finish(XrHirBody *,XrHirBodySignature,
    const XrXirInstruction *,XrCompileRequestControl *,XrHirBodyUse *,XrHirBodyAction *);
XR_FUNC XrXirStatus xr_hir_body_call_argument(XrHirBody *,XrHirBodySignature,
    const XrHirCallUseInput *,XrCompileRequestControl *,XrHirBodyUse *,XrHirBodyAction *);
XR_FUNC XrXirStatus xr_hir_body_array_context(const XrHirBody *,XrHirBodySignature,XrXirType,uint32_t,XrHirArrayCallbackShape *);
typedef struct XrHirBodyProjection {
    uint32_t function,instruction,block,argument;
    uint32_t value,source_value,formal,source_operands[2];
    XrXirType type;
    XrXirOp operation;
    int64_t immediate;
    XrXirOp operand_operation;
    int64_t operand_immediate;
    XrXirType operand_type;
    XrXirInstruction call;
    bool has_call;
    const uint32_t *source_arguments;
    uint32_t source_argument_count,default_owner,default_parameter;
    bool has_default;
    bool has_instruction,has_value;
} XrHirBodyProjection;
/* The actual pool descriptor belongs to the same construction owner and may
 * grow. End closes every read before that owner replaces or retires the pool.
 * No request, AST, caller revision number or callback is stored in this owner. */
XR_FUNC XrXirStatus xr_hir_body_new(XrCompileResources *,const XrXirTypes *,XrHirBody **);
XR_FUNC XrXirStatus xr_hir_body_declarations(XrHirBody *,const XrHirBodyDeclarations *);
XR_FUNC XrXirStatus xr_hir_body_place(XrHirBody *,const XrHirPlaceInput *,
    XrCompileRequestControl *,XrHirPlaceAction *,const char **);
XR_FUNC void xr_hir_body_free(XrHirBody *);
XR_FUNC XrXirStatus xr_hir_body_return(XrHirBody *,XrHirBodyOrigin,uint32_t,
    XrCompileRequestControl *,XrHirBodyUse *,XrHirBodyAction *);
XR_FUNC XrXirStatus xr_hir_body_emit(XrHirBody *,XrHirBodyUse,XrXirType,uint32_t);
XR_FUNC XrXirStatus xr_hir_body_finish(XrHirBody *,XrCompileRequestControl *);
XR_FUNC XrXirStatus xr_hir_body_retire(XrHirBody *,XrHirBodyUse);
XR_FUNC void xr_hir_body_end(XrHirBody *);
XR_FUNC bool xr_hir_body_live(const XrHirBody *);
XR_FUNC XrXirStatus xr_hir_body_scope(XrHirBody *,XrHirBodyScope,
    XrHirBodyOrigin,XrCompileRequestControl *,XrHirBodyScope *);
/* A declaration is received once; later uses retrieve its owned facts rather
 * than supplying fresh mutability, storage or parameter-mode assertions. */
XR_FUNC XrXirStatus xr_hir_body_binding_find(const XrHirBody *,XrHirBindingKey,
    XrCompileRequestControl *,XrHirBodyBinding *,bool *);
XR_FUNC bool xr_hir_body_binding_info(const XrHirBody *,XrHirBodyBinding,XrHirBindingInput *);
XR_FUNC XrXirStatus xr_hir_body_binding_instance(XrHirBody *,XrHirBodyBinding,
    XrHirBodyScope,XrCompileRequestControl *,XrHirBodyBinding *);
XR_FUNC XrXirStatus xr_hir_body_binding_declare(XrHirBody *,const XrHirBindingInput *,
    XrCompileRequestControl *,XrHirBodyBinding *,const char **);
XR_FUNC XrXirStatus xr_hir_body_binding_use(XrHirBody *,XrHirBodyBinding,
    const XrHirBindingUseInput *,XrCompileRequestControl *,XrHirBindingAction *,const char **);
/* Input borrows are synchronous. A successful use owns its origin and action;
 * failed checks and interrupted growth preserve the output and prior uses. */
XR_FUNC XrXirStatus xr_hir_body_check(XrHirBody *,const XrXirTypes *,
    const XrHirBodyInput *,XrCompileRequestControl *,XrHirBodyUse *,const char **);
/* Only a real handle from this owner can supply the emitter's typed action. */
XR_FUNC XrXirStatus xr_hir_body_consume(XrHirBody *,XrHirBodyUse,XrHirBodyAction *);
XR_FUNC XrXirStatus xr_hir_body_project(XrHirBody *,XrHirBodyUse,const XrHirBodyProjection *,XrCompileRequestControl *,const char **);
XR_FUNC uint32_t xr_hir_body_use_count(const XrHirBody *);
XR_FUNC bool xr_hir_body_origin(const XrHirBody *,uint32_t,XrHirBodyOrigin *);
#endif // XHIR_BODY_H
