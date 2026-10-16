/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_expression_receive.inc.c - Receive structural results in an owned target arena
 *
 * KEY CONCEPT:
 *   Immutable structural data is rebuilt only after complete type comparison.
 *   Identity values never cross arenas through a structural conversion.
 */
#include "../../xir/xxir_array.h"
#include "../../xir/xxir_tuple.h"
#include "../../xir/xxir_nullable.h"
typedef struct ExpressionReceive {
    const ExpressionState *state;
    XrXirValueAdmission admission;
    const XrXirTypes *types;
} ExpressionReceive;
static XrXirStatus expression_receive_work(ExpressionReceive *receiver,uint64_t units) {
    if (units>receiver->admission.work) return XR_XIR_BUDGET;
    receiver->admission.work-=units;
    return xr_compile_resources_work(receiver->state->request->lexical->launch->resources,units)==XR_COMPILE_RESOURCE_OK ? XR_XIR_OK : XR_XIR_BUDGET;
}
static XrXirStatus expression_receive_type(ExpressionReceive *receiver,const XrXirTypes *source,
    XrXirType from,XrXirType to,uint32_t depth) {
    if (depth==32) return XR_XIR_BUDGET;
    XrXirStatus status=expression_receive_work(receiver,1);if (status!=XR_XIR_OK) return status;
    if (expression_primitive(from) || expression_primitive(to)) return from==to ? XR_XIR_OK : XR_XIR_BAD_TYPE;
    const XrXirTypeNode *a=xr_xir_type_node(source,from),*b=xr_xir_type_node(receiver->types,to);
    if (!a || !b || a->kind!=b->kind || a->flags!=b->flags || a->parameter_span || b->parameter_span) return XR_XIR_BAD_TYPE;
    if (a->kind==XR_XIR_TYPE_ARRAY || a->kind==XR_XIR_TYPE_NULLABLE)
        return expression_receive_type(receiver,source,a->element,b->element,depth+1);
    if (a->kind!=XR_XIR_TYPE_TUPLE || a->parameter_count!=b->parameter_count) return XR_XIR_BAD_TYPE;
    for (uint32_t i=0;i<a->parameter_count;++i) {
        if (a->parameters[i].mode!=b->parameters[i].mode) return XR_XIR_BAD_TYPE;
        status=expression_receive_type(receiver,source,a->parameters[i].type,b->parameters[i].type,depth+1);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus expression_receive_node(ExpressionReceive *receiver,const XrXirValue *value,
    XrXirType type,uint32_t depth,XrXirValue *output) {
    if (depth==32) return XR_XIR_BUDGET;
    XrXirStatus status=expression_receive_work(receiver,1);if (status!=XR_XIR_OK) return status;
    if (expression_primitive(type)) return value->type==(uint32_t)type ? expression_value_status(xr_xir_value_copy(value,output)) : XR_XIR_BAD_TYPE;
    const XrXirTypeNode *node=xr_xir_type_node(receiver->types,type);if (!node) return XR_XIR_BAD_TYPE;
    XR_CHECK(node->kind==XR_XIR_TYPE_ARRAY || node->kind==XR_XIR_TYPE_TUPLE || node->kind==XR_XIR_TYPE_NULLABLE,"validated structural receiver changed kind");
    XdapValueView view={value,receiver->admission.domain,NULL,0};XdapValueShape shape={0};
    status=expression_value_status(xdap_value_shape(&view,&shape));if (status!=XR_XIR_OK) return status;
    if ((node->kind==XR_XIR_TYPE_NULLABLE && shape.children>1) ||
        (node->kind==XR_XIR_TYPE_TUPLE && shape.children!=node->parameter_count)) return XR_XIR_BAD_TYPE;
    if (shape.children>SIZE_MAX/sizeof(XrXirValue)) return XR_XIR_BUDGET;
    uint64_t bytes=shape.children*sizeof(XrXirValue);status=expression_receive_work(receiver,bytes);
    if (status!=XR_XIR_OK) return status;
    XrCompileResources *resources=receiver->state->request->lexical->launch->resources;XrXirValue *fields=NULL;
    if (shape.children) {
        status=expression_data_status(xr_compile_resources_calloc(resources,(size_t)shape.children,sizeof(*fields),(void **)&fields));
        if (status!=XR_XIR_OK) return status;
    }
    for (uint64_t i=0;i<shape.children && status==XR_XIR_OK;++i) {
        XrXirValue child={0};XdapValueChildInfo info={0};status=expression_value_status(xdap_value_child(&view,i,&child,&info));
        XrXirType target=node->kind==XR_XIR_TYPE_TUPLE ? node->parameters[i].type : node->element;
        if (status==XR_XIR_OK) status=expression_receive_node(receiver,&child,target,depth+1,&fields[i]);
        xr_xir_value_drop(&child);
    }
    if (status==XR_XIR_OK) {
        XrXirValueStatus made=node->kind==XR_XIR_TYPE_ARRAY ? xr_xir_array_new(type,fields,(size_t)shape.children,&receiver->admission,output) :
            node->kind==XR_XIR_TYPE_TUPLE ? xr_xir_tuple_new(type,fields,(uint32_t)shape.children,&receiver->admission,output) :
            node->kind==XR_XIR_TYPE_NULLABLE ? xr_xir_nullable_new(type,shape.children ? fields : NULL,&receiver->admission,output) : XR_XIR_VALUE_BAD_ARGUMENT;
        status=expression_value_status(made);
    }
    for (uint64_t i=0;i<shape.children;++i) xr_xir_value_drop(&fields[i]);
    xr_compile_resources_free(fields);return status;
}
static XrXirStatus expression_receive(ExpressionState *state,XrXirValue *value) {
    const XrXirValue *expected=state->request->expected;const XrXirTypeArena *arena=xr_xir_value_arena(expected);
    if (expected->type==value->type && arena==xr_xir_value_arena(value)) return XR_XIR_OK;
    ExpressionReceive receiver={state,{.arena=arena,.work=XR_XIR_CTFE_MAX_STEPS,.scratch_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES},xr_xir_compile_type_arena_types(arena)};
    XrXirStatus status=expression_receive_type(&receiver,xr_xir_compile_type_arena_types(xr_xir_value_arena(value)),
        (XrXirType)value->type,(XrXirType)expected->type,0);
    if (status==XR_XIR_OK) status=expression_value_status(xr_xir_compile_domain_new(state->request->lexical->launch->resources,
        state->request->source_limit,&receiver.admission.domain));
    XrXirValue owned={0};if (status==XR_XIR_OK) status=expression_receive_node(&receiver,value,(XrXirType)expected->type,0,&owned);
    xr_xir_domain_drop(receiver.admission.domain);xr_xir_value_drop(value);*value=owned;return status;
}
