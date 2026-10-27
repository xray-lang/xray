/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_expression_data.inc.c - Structural observations rebuilt by ordinary Source
 *
 * KEY CONCEPT:
 *   Primitive parameters retain their exact bits and String ownership. Source
 *   reconstructs structural data without borrowing a foreign type identity.
 */
#include "../../xir/xxir_compile_domain.h"
#include "../../xir/xxir_type_arena.h"
#include <stdio.h>
typedef struct ExpressionData {
    const ExpressionState *state;
    XdapJsonWriter parameters,body;
    XrXirValue *arguments;
    XrXirDomain *domain;
    uint32_t count,capacity,next;
} ExpressionData;
static XrXirStatus expression_data_status(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
        status==XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_MEMORY_LIMIT :
        status==XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_OVERFLOW :
        status==XR_COMPILE_RESOURCE_CANCELLED ? XR_XIR_CANCELLED :
        status==XR_COMPILE_RESOURCE_DEADLINE ? XR_XIR_DEADLINE :
        status==XR_COMPILE_RESOURCE_CLOCK_FAILURE ? XR_XIR_CLOCK_FAILURE : XR_XIR_BAD_STRUCTURE;
}
static bool expression_data_name(XdapJsonWriter *text,uint32_t id) {
    char name[32];
    int length=snprintf(name,sizeof(name),"_dap_observed_%u",id);
    if (length<=0 || (size_t)length>=sizeof(name)) { text->status=XR_XIR_BAD_STRUCTURE;return false; }
    return xdap_json_text(text,name,(size_t)length);
}
static XrXirStatus expression_data_id(ExpressionData *data,uint32_t *output) {
    const ExpressionState *state=data->state;
    for (;;) {
        if (data->next==UINT32_MAX) return XR_XIR_BUDGET;
        uint32_t candidate=data->next++;XdapJsonWriter name=xdap_json_writer(data->parameters.resources,64);
        expression_data_name(&name,candidate);bool collision=false;
        for (uint32_t i=0;i<state->count && name.status==XR_XIR_OK;++i) {
            if (state->bindings[i].length==name.length &&
                !memcmp(state->bindings[i].name,name.bytes,name.length)) collision=true;
        }
        XrXirStatus status=name.status;xdap_json_writer_free(&name);
        if (status!=XR_XIR_OK) return status;
        if (!collision) { *output=candidate;return XR_XIR_OK; }
    }
}
static XrXirStatus expression_data_argument(ExpressionData *data,const XrXirValue *value,uint32_t id) {
    const XrXirCompileContext *context=xr_source_compiler_context(data->state->request->compiler);
    if (data->count==context->limits.parameters) return XR_XIR_BUDGET;
    if (data->count==data->capacity) {
        uint32_t capacity=data->capacity ? data->capacity*2 : 8;
        if (capacity<data->capacity || capacity>context->limits.parameters) capacity=context->limits.parameters;
        uint64_t bytes=(uint64_t)capacity*sizeof(*data->arguments);
        if (capacity<=data->count) return XR_XIR_BUDGET;
        if (bytes>SIZE_MAX) return XR_XIR_OVERFLOW;
        void *memory=data->arguments;
        XrXirStatus status=expression_data_status(xr_compile_resources_resize(context->resources,
            &memory,(size_t)bytes));
        if (status!=XR_XIR_OK) return status;
        data->arguments=memory;data->capacity=capacity;
    }
    if (data->count) xdap_json_text(&data->parameters,",",1);
    expression_data_name(&data->parameters,id);xdap_json_text(&data->parameters,":",1);
    expression_type(&data->parameters,NULL,(XrXirType)value->type,0);
    if (data->parameters.status!=XR_XIR_OK) return data->parameters.status;
    XrXirValue copy={0};XrXirStatus status=expression_value_status(xr_xir_value_copy(value,&copy));
    if (status==XR_XIR_OK) data->arguments[data->count++]=copy;
    return status;
}
static XrXirStatus expression_data_node(ExpressionData *data,const XrXirValue *value,
    uint32_t depth,uint32_t *output) {
    if (depth==32) return XR_XIR_BUDGET;
    uint32_t id=0;XrXirStatus status=expression_data_id(data,&id);
    if (status!=XR_XIR_OK) return status;
    XrXirType type=(XrXirType)value->type;
    if (expression_primitive(type) && type!=XR_XIR_UNIT) {
        status=expression_data_argument(data,value,id);
        if (status==XR_XIR_OK) *output=id;
        return status;
    }
    const XrXirTypes *types=xr_xir_compile_type_arena_types(xr_xir_value_arena(value));
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    if (type!=XR_XIR_UNIT && (!node || (node->kind!=XR_XIR_TYPE_ARRAY &&
        node->kind!=XR_XIR_TYPE_TUPLE && node->kind!=XR_XIR_TYPE_NULLABLE))) return XR_XIR_BAD_TYPE;
    XdapJsonWriter init=xdap_json_writer(data->parameters.resources,data->state->request->source_limit);
    XdapValueShape shape={0};
    if (type!=XR_XIR_UNIT) {
        if (!data->domain) status=expression_value_status(xr_xir_compile_domain_new(init.resources,
            data->state->request->source_limit,&data->domain));
        XdapValueView view={value,data->domain,data->state->request->lexical->snapshot,
            data->state->request->lexical->frame};
        if (status==XR_XIR_OK) status=expression_value_status(xdap_value_shape(&view,&shape));
        if (node->kind==XR_XIR_TYPE_ARRAY) xdap_json_text(&init,"[",1);
        else if (node->kind==XR_XIR_TYPE_TUPLE) xdap_json_text(&init,"(",1);
        else if (!shape.children) xdap_json_text(&init,"null",4);
        for (uint64_t i=0;i<shape.children && status==XR_XIR_OK && init.status==XR_XIR_OK;++i) {
            XrXirValue child={0};XdapValueChildInfo info={0};uint32_t child_id=0;
            status=expression_value_status(xdap_value_child(&view,i,&child,&info));
            if (status==XR_XIR_OK) status=expression_data_node(data,&child,depth+1,&child_id);
            xr_xir_value_drop(&child);
            if (status!=XR_XIR_OK) break;
            if (i) xdap_json_text(&init,",",1);
            expression_data_name(&init,child_id);
        }
        if (node->kind==XR_XIR_TYPE_ARRAY) xdap_json_text(&init,"]",1);
        else if (node->kind==XR_XIR_TYPE_TUPLE) {
            if (shape.children==1) xdap_json_text(&init,",",1);
            xdap_json_text(&init,")",1);
        }
    } else xdap_json_text(&init,"()",2);
    if (status==XR_XIR_OK) status=init.status;
    if (status==XR_XIR_OK) {
        xdap_json_text(&data->body,"const ",6);expression_data_name(&data->body,id);
        xdap_json_text(&data->body,":",1);expression_type(&data->body,types,type,0);
        xdap_json_text(&data->body," = ",3);xdap_json_text(&data->body,init.bytes,init.length);
        xdap_json_text(&data->body,"\n",1);status=data->body.status;
    }
    xdap_json_writer_free(&init);
    if (status==XR_XIR_OK) *output=id;
    return status;
}
static XrXirStatus expression_data_prepare(const ExpressionState *state,ExpressionData *data) {
    *data=(ExpressionData){.state=state,
        .parameters=xdap_json_writer(state->request->lexical->launch->resources,state->request->source_limit),
        .body=xdap_json_writer(state->request->lexical->launch->resources,state->request->source_limit)};
    for (uint32_t i=0;i<state->count;++i) {
        uint32_t id=0;XrXirStatus status=expression_data_node(data,&state->bindings[i].value,0,&id);
        if (status!=XR_XIR_OK) return status;
        xdap_json_text(&data->body,"const ",6);
        xdap_json_text(&data->body,state->bindings[i].name,state->bindings[i].length);
        xdap_json_text(&data->body," = ",3);expression_data_name(&data->body,id);
        xdap_json_text(&data->body,"\n",1);
        if (data->body.status!=XR_XIR_OK) return data->body.status;
    }
    return XR_XIR_OK;
}
static void expression_data_free(ExpressionData *data) {
    for (uint32_t i=0;i<data->count;++i) xr_xir_value_drop(&data->arguments[i]);
    xr_compile_resources_free(data->arguments);xr_xir_domain_drop(data->domain);
    xdap_json_writer_free(&data->parameters);xdap_json_writer_free(&data->body);
    *data=(ExpressionData){0};
}
