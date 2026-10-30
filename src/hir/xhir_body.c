/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_body.c - Owned typed uses consumed during their real pool lifetime
 *
 * KEY CONCEPT:
 *   Stable use indices survive growth; closing a construction ends pool access.
 */
#include "xhir_body.h"
#include "xhir_tuple.h"
#include "xhir_nominal.h"
#include "xhir_nominal_source.h"
#include "../xir/xxir_construction.h"
#include "../xir/xxir_types.h"
#include "../xir/xxir_map.h"
#include "../xir/xxir_interface.h"
#include "../xir/xxir_compile_memory.h"
#include "../base/xchecks.h"
#include "../base/xhash.h"
#include "../shared/xnative_declaration.h"
#include <string.h>
typedef struct HirBodyUse {
    XrHirBodyOrigin origin,definition;
    XrHirBodyAction action;
    XrHirBodyProjection projection;
    uint32_t source_value,selector,field,next_argument,source_operand;
    uint32_t default_value,default_call;
    XrXirOp default_conversion;
    XrHirBodySignature signature;
    XrHirBodyBinding binding;
    XrHirBindingUseKind binding_kind;
    XrHirBodyPlace place;
    XrHirTupleShape tuple;
    uint32_t *tuple_arguments;
    uint32_t tuple_argument_count;
    bool tuple_construct;
    XrHirNominalShape nominal;
    XrHirNominalValue *nominal_values;
    uint32_t nominal_count,nominal_variant,nominal_field,nominal_function;
    uint32_t nominal_result,nominal_call;
    XrXirOp nominal_call_operation;
    bool nominal_construct,nominal_helper,nominal_has_result,nominal_has_call,nominal_emitted;
    bool initializes,index_group;
    bool has_second_operand,has_immediate;
    uint32_t second_operand;
    int64_t expected_immediate;
    uint32_t expected_arguments[3];
    bool consumed,projected,retired,has_source,has_selector,has_field,required_instruction,foreign_slot,is_call,has_operand,defaulted;
} HirBodyUse;
typedef struct HirBodyBinding {
    XrHirBindingInput declaration;
    char *name;
    uint32_t location;
    bool materialized,is_declaration;
} HirBodyBinding;
typedef struct HirBodyScope {XrHirBodyScope parent;XrHirBodyOrigin origin;uint32_t nominal_owner;} HirBodyScope;
typedef struct HirBodyPlace {
    XrHirBodyBinding root;
    XrHirBodyPlace parent;
    XrHirBodyScope scope;
    XrHirBodyOrigin origin;
    XrXirType type,key;
    uint32_t selector,field,depth;
    bool reference,object_root,materialized,terminal,writable,identity,constructor,field_mutable,initializing,has_field;
    uint32_t value;
} HirBodyPlace;
typedef struct HirBodySignature {
    XrHirSignatureInput declaration;
    XrXirCallableParameter *parameters;
    XrXirType *default_types;
    uint32_t default_type_count;
    unsigned char *accesses;
    XrHirArrayCallbackShape callback;
    uint32_t first_argument,last_argument,argument_count,access_count,callback_use;
    bool finished,callback_ready,callback_received,default_types_ready;
} HirBodySignature;
typedef struct HirBodyVector {void *items;uint32_t count,capacity;} HirBodyVector;
typedef struct HirBodyTuple {
    XrXirType type;
    XrHirTupleField *fields;
    uint32_t *physical_fields;
    uint32_t count,physical_count;
} HirBodyTuple;
typedef struct HirBodyTupleContext {
    XrHirTupleExpected *fields;
    uint32_t count;
    XrHirTupleContextKind kind;
} HirBodyTupleContext;
typedef struct HirBodyNominalDeclaration {
    XrHirNominalField *fields;
    XrXirNominalVariant *variants;
    uint32_t *field_origins,*field_functions,*field_producers,*field_sources,*foreign_fields;
    uint32_t module,source_declaration,count,variant_count,constructor,foreign_constructor,source_capacity;
    XrXirNominalKind kind;
    uint32_t flags;
    bool explicit_initializer,imported,available,received;
} HirBodyNominalDeclaration;
typedef struct HirBodyNominal {
    XrXirType type;
    XrXirType *arguments;
    XrHirNominalField *fields;
    uint32_t declaration,count,argument_count;
} HirBodyNominal;
typedef struct HirBodyNominalPlan {
    XrHirNominalPlanInput input;
    XrHirNominalValue *values;
    unsigned char *present;
    uint32_t count,begin,selected;
    bool complete,applied;
} HirBodyNominalPlan;
typedef struct HirBodyNominalFunctionSource {uint32_t function,declaration,field,origin;} HirBodyNominalFunctionSource;
struct XrHirBody {
    XrCompileResources *resources;
    /* owned: The surrounding Source construction owns both this body and this
     * stable descriptor, and closes the body before replacing its pool view. */
    const XrXirTypes *types;
    HirBodyUse *uses;
    uint32_t count,capacity;
    HirBodyBinding *bindings;
    uint32_t binding_count,binding_capacity;
    uint32_t *binding_map;
    HirBodyVector scopes,places,signatures;
    HirBodyVector tuples,tuple_contexts;
    uint32_t *tuple_map;
    uint32_t tuple_map_capacity;
    XrXirSourceModule *const *nominal_modules;
    XrXirGeneric *const *nominal_generics;
    HirBodyVector nominal_declarations,nominals,nominal_plans,nominal_functions;
    uint32_t *nominal_map;
    uint32_t nominal_map_capacity;
    unsigned char *nominal_available;
    XrHirBodyDeclarations declarations;
    bool ended,nominal_sealed,nominal_shape_only;
};
static void body_nominal_free(XrHirBody *);
static XrXirStatus body_poll(XrCompileRequestControl *control) {
    return control?xir_compile_resource_status(xr_compile_request_control_poll(control)):XR_XIR_OK;
}
static XrXirStatus body_bytes(void *to,const void *from,size_t bytes,XrCompileRequestControl *control) {
    for(size_t at=0;at<bytes;) {
        XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
        size_t count=bytes-at<4096?bytes-at:4096;
        if(from)memcpy((unsigned char *)to+at,(const unsigned char *)from+at,count);
        else memset((unsigned char *)to+at,0,count);
        at+=count;
    }
    return body_poll(control);
}
static XrXirStatus body_grow(XrHirBody *body,XrCompileRequestControl *control) {
    if(body->count<body->capacity)return XR_XIR_OK;
    if(body->count==UINT32_MAX)return XR_XIR_OVERFLOW;
    uint32_t capacity=body->capacity?body->capacity>UINT32_MAX/2?UINT32_MAX:body->capacity*2:16;
    if(sizeof(HirBodyUse)>SIZE_MAX/capacity)return XR_XIR_OVERFLOW;
    HirBodyUse *uses=NULL;
    XrXirStatus status=xir_compile_resource_status(xr_compile_resources_alloc(body->resources,
        (size_t)capacity*sizeof(*uses),(void **)&uses));
    if(status!=XR_XIR_OK)return status;
    status=body_bytes(uses,NULL,(size_t)capacity*sizeof(*uses),control);
    if(status==XR_XIR_OK)status=body_bytes(uses,body->uses,(size_t)body->count*sizeof(*uses),control);
    if(status!=XR_XIR_OK){xr_compile_resources_free(uses);return status;}
    xr_compile_resources_free(body->uses);body->uses=uses;body->capacity=capacity;return XR_XIR_OK;
}
static XrXirStatus body_vector_stage(XrHirBody *body,HirBodyVector original,size_t element,
    XrCompileRequestControl *control,HirBodyVector *staged) {
    *staged=original;if(original.count<original.capacity)return body_poll(control);
    if(original.count==UINT32_MAX)return XR_XIR_OVERFLOW;
    uint32_t capacity=original.capacity?original.capacity>UINT32_MAX/2?UINT32_MAX:original.capacity*2:16;
    if(capacity>SIZE_MAX/element)return XR_XIR_OVERFLOW;
    void *items=NULL;
    XrXirStatus status=xir_compile_resource_status(xr_compile_resources_alloc(body->resources,(size_t)capacity*element,&items));
    if(status!=XR_XIR_OK)return status;
    status=body_bytes(items,NULL,(size_t)capacity*element,control);
    if(status==XR_XIR_OK)status=body_bytes(items,original.items,(size_t)original.count*element,control);
    if(status!=XR_XIR_OK){xr_compile_resources_free(items);return status;}
    *staged=(HirBodyVector){items,original.count,capacity};return XR_XIR_OK;
}
static void body_vector_commit(HirBodyVector *original,HirBodyVector staged) {
    if(original->items!=staged.items)xr_compile_resources_free(original->items);*original=staged;
}
static XrXirStatus body_vector_reserve(XrHirBody *body,HirBodyVector *vector,size_t element,XrCompileRequestControl *control) {
    HirBodyVector staged={0};XrXirStatus status=body_vector_stage(body,*vector,element,control,&staged);
    if(status==XR_XIR_OK)body_vector_commit(vector,staged);return status;
}
static XrXirStatus body_place_reserve(XrHirBody *body,XrCompileRequestControl *control) {
    HirBodyVector uses={body->uses,body->count,body->capacity},new_uses=uses,new_places=body->places;
    XrXirStatus status=body_vector_stage(body,body->places,sizeof(HirBodyPlace),control,&new_places);
    if(status==XR_XIR_OK)status=body_vector_stage(body,uses,sizeof(HirBodyUse),control,&new_uses);
    if(status==XR_XIR_OK)status=body_poll(control);
    if(status!=XR_XIR_OK) {
        if(new_places.items!=body->places.items)xr_compile_resources_free(new_places.items);
        if(new_uses.items!=uses.items)xr_compile_resources_free(new_uses.items);
        return status;
    }
    body_vector_commit(&body->places,new_places);body_vector_commit(&uses,new_uses);
    body->uses=uses.items;body->capacity=uses.capacity;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_new(XrCompileResources *resources,const XrXirTypes *types,XrHirBody **output) {
    if(!resources||!types||!output||*output)return XR_XIR_BAD_STRUCTURE;
    XrHirBody *body=NULL;
    XrXirStatus status=xir_compile_resource_status(xr_compile_resources_calloc(resources,1,sizeof(*body),(void **)&body));
    if(status!=XR_XIR_OK)return status;
    body->resources=resources;body->types=types;*output=body;return XR_XIR_OK;
}
XR_FUNC void xr_hir_body_free(XrHirBody *body) {
    if(!body)return;
    body_nominal_free(body);
    for(uint32_t i=0;i<body->count;++i)xr_compile_resources_free(body->uses[i].tuple_arguments);
    for(uint32_t i=0;i<body->tuples.count;++i)xr_compile_resources_free(((HirBodyTuple *)body->tuples.items)[i].fields);
    for(uint32_t i=0;i<body->tuple_contexts.count;++i)xr_compile_resources_free(((HirBodyTupleContext *)body->tuple_contexts.items)[i].fields);
    xr_compile_resources_free(body->tuples.items);xr_compile_resources_free(body->tuple_contexts.items);xr_compile_resources_free(body->tuple_map);
    for(uint32_t i=0;i<body->binding_count;++i)if(body->bindings[i].is_declaration)xr_compile_resources_free(body->bindings[i].name);
    for(uint32_t i=0;i<body->signatures.count;++i) {
        xr_compile_resources_free(((HirBodySignature *)body->signatures.items)[i].parameters);
        xr_compile_resources_free(((HirBodySignature *)body->signatures.items)[i].default_types);
    }
    xr_compile_resources_free(body->signatures.items);
    xr_compile_resources_free(body->places.items);xr_compile_resources_free(body->scopes.items);xr_compile_resources_free(body->binding_map);xr_compile_resources_free(body->bindings);
    xr_compile_resources_free(body->uses);xr_compile_resources_free(body);
}
/* Return eligibility and inference stay with their existing owner. This use
 * projects the real declaration result and its mechanical RETURN operand. */
XR_FUNC XrXirStatus xr_hir_body_return(XrHirBody *body,XrHirBodyOrigin origin,uint32_t value,
    XrCompileRequestControl *control,XrHirBodyUse *use,XrHirBodyAction *output) {
    if(!body||body->ended||!use||!output||!body->declarations.functions||!*body->declarations.functions||
        origin.function>=*body->declarations.function_count)return XR_XIR_BAD_STRUCTURE;
    XrXirType result=(*body->declarations.functions)[origin.function].result;
    XrHirBodyInput input={.origin=origin,.rule=XR_HIR_BODY_PROJECTION,.source=result,.target=XR_XIR_UNIT};
    XrXirStatus status=xr_hir_body_check(body,body->types,&input,control,use,NULL);if(status!=XR_XIR_OK)return status;
    HirBodyUse *record=&body->uses[use->index];record->action.operation=XR_XIR_RETURN;
    record->required_instruction=true;record->has_operand=result!=XR_XIR_UNIT;record->source_operand=value;
    *output=record->action;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_emit(XrHirBody *body,XrHirBodyUse handle,XrXirType type,uint32_t value) {
    if(!body||body->ended||handle.owner!=body||handle.index>=body->count)return XR_XIR_BAD_STRUCTURE;
    HirBodyUse *use=&body->uses[handle.index];
    if(!use->consumed||use->retired||use->projected||use->action.target!=type)return XR_XIR_BAD_STRUCTURE;
    if(use->nominal_helper) {
        if(use->nominal_emitted)return XR_XIR_BAD_STRUCTURE;use->nominal_emitted=true;
        if(type!=XR_XIR_UNIT){use->nominal_result=value;use->nominal_has_result=true;}
    }
    if(use->binding.owner&&use->initializes) {
        if(use->binding.owner!=body||use->binding.index>=body->binding_count)return XR_XIR_BAD_STRUCTURE;
        HirBodyBinding *binding=&body->bindings[use->binding.index];
        bool slot=binding->declaration.storage==XR_HIR_BODY_SLOT||binding->declaration.storage==XR_HIR_BODY_UNIT_SLOT;
        if(binding->materialized&&!slot)return XR_XIR_BAD_STRUCTURE;
        if(slot) {
            if(!body->declarations.slots||!body->declarations.slot_count||!*body->declarations.slots||
                binding->location>=*body->declarations.slot_count)return XR_XIR_BAD_STRUCTURE;
            XrXirSlot declaration=(*body->declarations.slots)[binding->location];
            if(declaration.type!=type||declaration.module!=binding->declaration.origin.module||
                !!declaration.mutable!=binding->declaration.rebindable)return XR_XIR_BAD_STRUCTURE;
        } else binding->location=value;
        binding->materialized=true;
    }
    if(use->place.owner) {
        if(use->place.owner!=body||use->place.index>=body->places.count)return XR_XIR_BAD_STRUCTURE;
        HirBodyPlace *place=&((HirBodyPlace *)body->places.items)[use->place.index];
        if(place->materialized)return XR_XIR_BAD_STRUCTURE;
        place->value=value;place->materialized=true;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_finish(XrHirBody *body,XrCompileRequestControl *control) {
    if(!body||body->ended)return XR_XIR_BAD_STRUCTURE;
    for(uint32_t i=0;i<body->count;++i) {
        if(!(i%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        HirBodyUse use=body->uses[i];
        if(!use.consumed||(!use.projected&&!use.retired))return XR_XIR_BAD_STRUCTURE;
    }
    for(uint32_t i=0;i<body->signatures.count;++i) {
        if(!(i%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        if(!((HirBodySignature *)body->signatures.items)[i].finished)return XR_XIR_BAD_STRUCTURE;
    }
    for(uint32_t i=0;i<body->nominal_plans.count;++i) {
        if(!(i%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        if(!((HirBodyNominalPlan *)body->nominal_plans.items)[i].complete)return XR_XIR_BAD_STRUCTURE;
    }
    XrXirStatus status=body_poll(control);if(status==XR_XIR_OK)body->nominal_sealed=true;return status;
}
XR_FUNC XrXirStatus xr_hir_body_retire(XrHirBody *body,XrHirBodyUse handle) {
    if(!body||body->ended||handle.owner!=body||handle.index>=body->count)return XR_XIR_BAD_STRUCTURE;
    HirBodyUse *use=&body->uses[handle.index];
    if(!use->consumed||use->projected||use->retired)return XR_XIR_BAD_STRUCTURE;
    use->retired=true;return XR_XIR_OK;
}
XR_FUNC void xr_hir_body_end(XrHirBody *body) {
    if(!body)return;body->types=NULL;body->declarations=(XrHirBodyDeclarations){0};body->nominal_modules=NULL;body->nominal_generics=NULL;body->ended=true;
}
XR_FUNC bool xr_hir_body_live(const XrHirBody *body){return body&&!body->ended;}
XR_FUNC XrXirStatus xr_hir_body_scope(XrHirBody *body,XrHirBodyScope parent,
    XrHirBodyOrigin origin,XrCompileRequestControl *control,XrHirBodyScope *out) {
    if(!body||body->ended||!out||(parent.owner&&(parent.owner!=body||parent.index>=body->scopes.count)))return XR_XIR_BAD_STRUCTURE;
    if(parent.owner) {
        HirBodyScope previous=((HirBodyScope *)body->scopes.items)[parent.index];
        if(previous.origin.function!=origin.function||previous.origin.module!=origin.module)return XR_XIR_BAD_STRUCTURE;
    }
    for(uint32_t i=0;i<body->scopes.count;++i) {
        if(!(i%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        HirBodyScope previous=((HirBodyScope *)body->scopes.items)[i];
        if(previous.parent.owner==parent.owner&&previous.parent.index==parent.index&&
            previous.origin.function==origin.function&&previous.origin.module==origin.module&&previous.origin.node==origin.node) {
            *out=(XrHirBodyScope){body,i};return XR_XIR_OK;
        }
    }
    XrXirStatus status=body_vector_reserve(body,&body->scopes,sizeof(HirBodyScope),control);if(status!=XR_XIR_OK)return status;
    uint32_t nominal=0;
    if(parent.owner)nominal=((HirBodyScope *)body->scopes.items)[parent.index].nominal_owner;
    else if(body->declarations.function_count) {
        if(origin.module>=*body->declarations.module_count)return XR_XIR_BAD_STRUCTURE;
        if(origin.function!=UINT32_MAX) {
            if(origin.function>=*body->declarations.function_count||!*body->declarations.identities||
                (*body->declarations.identities)[origin.function].module!=origin.module)return XR_XIR_BAD_STRUCTURE;
            nominal=(*body->declarations.identities)[origin.function].nominal_owner;
        }
    }
    ((HirBodyScope *)body->scopes.items)[body->scopes.count]=(HirBodyScope){parent,origin,nominal};
    *out=(XrHirBodyScope){body,body->scopes.count++};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_declarations(XrHirBody *body,const XrHirBodyDeclarations *input) {
    if(!body||body->ended||body->declarations.functions||!input||!input->functions||
        !input->identities||!input->function_count||!input->module_count)return XR_XIR_BAD_STRUCTURE;
    body->declarations=*input;return XR_XIR_OK;
}
static XrXirStatus body_scope_contains(const XrHirBody *body,XrHirBodyScope ancestor,
    XrHirBodyScope descendant,XrCompileRequestControl *control) {
    if(ancestor.owner!=body||descendant.owner!=body||ancestor.index>=body->scopes.count||descendant.index>=body->scopes.count)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t steps=0;
    for(;;) {
        if(!(steps++%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        if(descendant.index==ancestor.index)return XR_XIR_OK;
        XrHirBodyScope parent=((HirBodyScope *)body->scopes.items)[descendant.index].parent;
        if(parent.owner!=body||parent.index>=descendant.index)return XR_XIR_BAD_STRUCTURE;
        descendant=parent;
    }
}
static XrXirStatus body_substitution(XrHirBody *body,const XrXirType *arguments,uint32_t count,
    XrXirType expected,XrXirType actual,XrCompileRequestControl *control) {
    XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
    XrXirCompileContext context={body->resources,xr_xir_compile_default_limits()};
    status=xr_xir_compile_type_substitution_matches(&context,body->types,arguments,count,expected,actual);
    return status==XR_XIR_OK?body_poll(control):status;
}
static XrXirStatus body_narrowing(const XrHirBody *body,XrXirType original,XrXirType actual,XrCompileRequestControl *control) {
    while(original!=actual&&xr_xir_type_is_nullable(body->types,original)) {
        XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
        original=xr_xir_nullable_element(body->types,original);
    }
    return original==actual?XR_XIR_OK:XR_XIR_BAD_TYPE;
}
#include "xhir_body_binding.inc.c"
#include "xhir_body_place.inc.c"
#include "xhir_body_default.inc.c"
#include "xhir_body_tuple.inc.c"
#include "xhir_body_nominal.inc.c"
#include "xhir_body_call.inc.c"
XR_FUNC XrXirStatus xr_hir_body_check(XrHirBody *body,const XrXirTypes *types,
    const XrHirBodyInput *input,XrCompileRequestControl *control,XrHirBodyUse *output,const char **reason) {
    if(reason)*reason=NULL;
    if(!body||body->ended||types!=body->types||!input||!output)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
    XrHirBodyAction action={.source=input->source,.target=input->target,.key=input->key,
        .formal=input->formal,.receiver_count=input->receiver_count};
    switch(input->rule) {
    case XR_HIR_BODY_IDENTITY:break;
    case XR_HIR_BODY_EQUAL:
        if(input->source!=input->target)status=XR_XIR_BAD_TYPE;break;
    case XR_HIR_BODY_PROJECTION:break;
    default:status=XR_XIR_BAD_STRUCTURE;break;
    }
    if(status!=XR_XIR_OK){if(reason)*reason=input->failure;return status;}
    status=body_grow(body,control);if(status!=XR_XIR_OK)return status;
    HirBodyUse use={.origin=input->origin,.definition=input->definition,.action=action};
    body->uses[body->count]=use;*output=(XrHirBodyUse){body,body->count++};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_consume(XrHirBody *body,XrHirBodyUse handle,XrHirBodyAction *output) {
    uint32_t use=handle.index;
    if(handle.owner!=body)return XR_XIR_BAD_STRUCTURE;

    if(!body||body->ended||use>=body->count||!output)return XR_XIR_BAD_STRUCTURE;
    body->uses[use].consumed=true;*output=body->uses[use].action;return XR_XIR_OK;
}
static XrXirStatus body_projection_failure(const char **reason,const char *message) {
    if(reason)*reason=message;return XR_XIR_BAD_STRUCTURE;
}
XR_FUNC XrXirStatus xr_hir_body_project(XrHirBody *body,XrHirBodyUse handle,const XrHirBodyProjection *projection,XrCompileRequestControl *control,const char **reason) {
    if(reason)*reason="typed projection does not match its owned use";
    uint32_t use=handle.index;
    if(handle.owner!=body)return XR_XIR_BAD_STRUCTURE;

    if(!body||body->ended||use>=body->count||!projection||!body->uses[use].consumed||
        projection->function!=body->uses[use].origin.function||
        projection->type!=body->uses[use].action.target)return XR_XIR_BAD_STRUCTURE;
    HirBodyUse *row=&body->uses[use];
    if(row->tuple.owner) {
        XrXirStatus status=body_tuple_project(body,row,projection,control);if(status!=XR_XIR_OK)return status;
    }
    if(row->nominal.owner) {
        XrXirStatus status=body_nominal_project(body,row,projection,control);if(status!=XR_XIR_OK)return status;
    }
    if(row->projected||row->retired||row->action.formal!=projection->formal||row->action.argument!=projection->argument)
        return XR_XIR_BAD_STRUCTURE;
    if(row->has_source&&projection->source_value!=row->source_value)return XR_XIR_BAD_STRUCTURE;
    if(row->required_instruction&&!projection->has_instruction)return XR_XIR_BAD_STRUCTURE;
    if(projection->has_instruction&&row->action.operation!=XR_XIR_INVALID&&projection->operation!=row->action.operation) {
        bool invoke=row->is_call&&((row->action.operation==XR_XIR_CALL&&projection->operation==XR_XIR_INVOKE)||
            (row->action.operation==XR_XIR_CALL_INDIRECT&&projection->operation==XR_XIR_INVOKE_INDIRECT));
        bool helper_invoke=row->nominal_helper&&(projection->operation==XR_XIR_INVOKE||projection->operation==XR_XIR_INVOKE_RESULT);
        if(!invoke&&!helper_invoke&&(!row->foreign_slot||(projection->operation!=XR_XIR_CALL&&projection->operation!=XR_XIR_INVOKE_RESULT&&
            projection->operation!=XR_XIR_INVOKE)))return XR_XIR_BAD_STRUCTURE;
    }
    if(row->is_call) {
        if(row->signature.owner!=body||row->signature.index>=body->signatures.count)return XR_XIR_BAD_STRUCTURE;
        HirBodySignature call=((HirBodySignature *)body->signatures.items)[row->signature.index];
        if(!projection->has_call||projection->call.op!=projection->operation)
            return body_projection_failure(reason,"typed call projection has no actual callee");
        if(call.declaration.family==XR_HIR_CALL_REQUIREMENT) {
            if(projection->call.targets[0]!=call.declaration.interface_declaration||
                projection->call.targets[1]!=call.declaration.interface_member)return body_projection_failure(reason,"typed requirement projection changed its target");
        } else if(projection->call.immediate!=(int64_t)(call.declaration.family==XR_HIR_CALL_INDIRECT?
            call.declaration.callee_value:call.declaration.function))return body_projection_failure(reason,"typed call projection changed its callee");
        if(!call.finished||call.argument_count!=projection->source_argument_count||
            (call.argument_count&&!projection->source_arguments))return XR_XIR_BAD_STRUCTURE;
        uint32_t argument=call.first_argument;
        for(uint32_t p=0;p<call.argument_count;++p) {
            if(!(p%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
            if(argument>=body->count)return XR_XIR_BAD_STRUCTURE;
            HirBodyUse actual=body->uses[argument];
            if(actual.signature.owner!=body||actual.signature.index!=row->signature.index||
                projection->source_arguments[p]!=actual.source_value)return XR_XIR_BAD_STRUCTURE;
            argument=actual.next_argument;
        }
        if(argument!=UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
    }
    if(row->index_group) {
        if(projection->source_argument_count!=3||!projection->source_arguments)return XR_XIR_BAD_STRUCTURE;
        for(uint32_t p=0;p<3;++p)if(projection->source_arguments[p]!=row->expected_arguments[p])return XR_XIR_BAD_STRUCTURE;
    }
    if(row->has_operand&&projection->source_operands[0]!=row->source_operand)return body_projection_failure(reason,"typed storage projection changed its first operand");
    if(row->has_second_operand&&projection->source_operands[1]!=row->second_operand)return body_projection_failure(reason,"typed storage projection changed its written value");
    if(row->has_immediate&&projection->immediate!=row->expected_immediate)return body_projection_failure(reason,"typed slot projection changed its location");
    if(row->binding.owner&&projection->has_instruction&&!row->initializes) {
        HirBodyBinding binding=body->bindings[row->binding.index];
        bool slot=binding.declaration.storage==XR_HIR_BODY_SLOT||binding.declaration.storage==XR_HIR_BODY_UNIT_SLOT;
        if(row->foreign_slot&&(row->binding_kind==XR_HIR_BINDING_READ||row->binding_kind==XR_HIR_BINDING_WRITE)) {
            uint32_t accessor=row->binding_kind==XR_HIR_BINDING_READ?binding.declaration.reader:binding.declaration.writer;
            if(!accessor||!projection->has_call||projection->call.immediate!=(int64_t)(accessor-1))
                return body_projection_failure(reason,"typed history projection changed its received accessor");
            uint32_t count=row->binding_kind==XR_HIR_BINDING_WRITE&&binding.declaration.logical!=XR_XIR_UNIT?1u:0u;
            if(projection->source_argument_count!=count||(count&&(!projection->source_arguments||
                projection->source_arguments[0]!=row->second_operand)))return body_projection_failure(reason,"typed history projection changed its stored value");
        } else if(slot&&(row->action.operation==XR_XIR_CELL_READ||row->action.operation==XR_XIR_CELL_WRITE||row->action.operation==XR_XIR_CELL_PLACE)) {
            if(projection->operand_operation!=XR_XIR_SLOT_LOAD||projection->operand_immediate!=binding.location||
                projection->operand_type!=binding.declaration.physical)return body_projection_failure(reason,"typed Cell projection lost its actual slot producer");
        }
    }
    if(row->defaulted) {
        if(row->signature.owner!=body||row->signature.index>=body->signatures.count)return XR_XIR_BAD_STRUCTURE;
        HirBodySignature call=((HirBodySignature *)body->signatures.items)[row->signature.index];
        XrXirStatus status=body_default_project(&call,row,projection,control);
        if(status!=XR_XIR_OK)return status;
    }
    if(row->has_selector&&projection->source_operands[1]!=row->selector)return XR_XIR_BAD_STRUCTURE;
    if(row->has_field&&projection->immediate!=(int64_t)row->field)return XR_XIR_BAD_STRUCTURE;
    row->projection=*projection;row->projection.source_arguments=NULL;row->projection.source_value_map=NULL;
    row->projection.default_type_arguments=NULL;
    row->projected=true;if(reason)*reason=NULL;return XR_XIR_OK;
}
XR_FUNC uint32_t xr_hir_body_use_count(const XrHirBody *body){return body?body->count:0;}
XR_FUNC bool xr_hir_body_origin(const XrHirBody *body,uint32_t use,XrHirBodyOrigin *out) {
    if(!body||use>=body->count||!out)return false;*out=body->uses[use].origin;return true;
}
