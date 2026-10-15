/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_types.inc.c - Iterative compiler value-family admission
 *
 * KEY CONCEPT:
 *   Local IDs have meaning only in their verified immutable pool. An explicit
 *   ledger-owned DFS rejects cyclic or authority-bearing Tuple/Nullable fields.
 */
#include "xxir_operand_roles.h"
typedef struct CtfeTypeFrame { uint64_t work;uint32_t index,next;bool dynamic,pure_arrays; } CtfeTypeFrame;
static const XrXirTypeNode *ctfe_composite_node(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    bool nominal=node && (xr_xir_type_is_struct(types,type) || xr_xir_type_is_enum(types,type));
    return node && (node->kind==XR_XIR_TYPE_TUPLE || node->kind==XR_XIR_TYPE_NULLABLE || node->kind==XR_XIR_TYPE_ARRAY || nominal)?node:NULL;
}
static uint32_t ctfe_composite_fields(const XrXirTypeNode *node) {
    return node->kind==XR_XIR_TYPE_TUPLE?node->parameter_count:node->kind==XR_XIR_TYPE_NOMINAL?node->nominal.field_count:1;
}
static const XrXirTypeNode *ctfe_execution_node(const XrXirTypes *types,XrXirType type,bool internal) {
    const XrXirTypeNode *node=ctfe_composite_node(types,type);
    if (!node && internal) { node=xr_xir_type_node(types,type);if (node && node->kind!=XR_XIR_TYPE_CALLABLE) node=NULL; }
    return node;
}
static uint32_t ctfe_type_fields(const XrXirTypeNode *node) {
    return node->kind==XR_XIR_TYPE_CALLABLE?node->parameter_count+1:ctfe_composite_fields(node);
}
static XrXirCtfeStatus ctfe_nominal_owner_admit(const XrXirArtifact *artifact,uint32_t owner) {
    if (!owner) return XR_XIR_CTFE_OK;
    if (!xir_compile_work(&artifact->context,4)) return XR_XIR_CTFE_BUDGET;
    const XrXirNominalTable *table=artifact->module.types?artifact->module.types->nominals:NULL;
    if (!table || owner>table->count) return XR_XIR_CTFE_BAD_ARTIFACT;
    uint32_t kind=table->declarations?table->declarations[owner-1].kind:table->identities[owner-1].kind;
    /* A lexical declaration owner is not a class value or an effect grant.
     * Every reachable body and value family still requires separate admission. */
    return kind==XR_XIR_NOMINAL_STRUCT || kind==XR_XIR_NOMINAL_ENUM || kind==XR_XIR_NOMINAL_CLASS?
        XR_XIR_CTFE_OK:XR_XIR_CTFE_NOT_SUPPORTED;
}
static XrXirCtfeStatus ctfe_type_info(const XrXirCompileContext *context,
    const XrXirTypes *types,XrXirType type,bool internal,XrXirCtfeTypeShape *output) {
    if (!xir_compile_context_valid(context)) return XR_XIR_CTFE_BAD_ARGUMENT;
    if (internal && type==XR_XIR_ERROR) { *output=(XrXirCtfeTypeShape){1,true,false};return XR_XIR_CTFE_OK; }
    if (internal && type==XR_XIR_PANIC_INFO) { *output=(XrXirCtfeTypeShape){32,false,true};return XR_XIR_CTFE_OK; }
    if (ctfe_result_type(type)) { *output=(XrXirCtfeTypeShape){1,false,type!=XR_XIR_UNIT};return XR_XIR_CTFE_OK; }
    const XrXirTypeNode *root=ctfe_execution_node(types,type,internal);
    if (!root || !types || !types->count) return XR_XIR_CTFE_NOT_SUPPORTED;
    uint64_t bytes=(uint64_t)types->count*(sizeof(CtfeTypeFrame)+sizeof(XrXirCtfeTypeShape)+1);
    if (bytes>SIZE_MAX) return XR_XIR_CTFE_BUDGET;
    void *memory=NULL;
    XrXirCtfeStatus status=ctfe_compile_status(xir_compile_resource_status(
        xr_compile_resources_calloc(context->resources,1,(size_t)bytes,&memory)));
    if (status!=XR_XIR_CTFE_OK) return status;
    CtfeTypeFrame *stack=memory;XrXirCtfeTypeShape *costs=(XrXirCtfeTypeShape *)(stack+types->count);
    uint8_t *colors=(uint8_t *)(costs+types->count);uint32_t depth=1;
    if (!xir_compile_work(context,sizeof(*stack)+1)) { status=XR_XIR_CTFE_BUDGET;goto done; }
    uint32_t index=(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE;
    stack[0]=(CtfeTypeFrame){(uint64_t)ctfe_type_fields(root)+1,index,0,root->kind==XR_XIR_TYPE_CALLABLE,root->kind==XR_XIR_TYPE_ARRAY};colors[index]=1;
    while (depth) {
        if (!xir_compile_work(context,4)) { status=XR_XIR_CTFE_BUDGET;break; }
        CtfeTypeFrame *frame=&stack[depth-1];
        const XrXirTypeNode *node=&types->nodes[frame->index];
        bool single=node->kind==XR_XIR_TYPE_NULLABLE || node->kind==XR_XIR_TYPE_ARRAY;
        bool nominal=node->kind==XR_XIR_TYPE_NOMINAL,callable=node->kind==XR_XIR_TYPE_CALLABLE;
        if ((!single && !nominal && !callable && (node->kind!=XR_XIR_TYPE_TUPLE || !node->parameter_count || !node->parameters)) ||
            (callable && node->parameter_count && !node->parameters) || node->parameter_span) {
            status=XR_XIR_CTFE_NOT_SUPPORTED;break;
        }
        if (nominal) {
            XrXirType actual=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+frame->index);
            const XrXirNominalNativeRecord *native=xr_xir_nominal_native_record(types,actual);
            if (native && native->native_id && !xir_compile_work(context,64)) { status=XR_XIR_CTFE_BUDGET;break; }
            if ((native && native->native_id && !xr_xir_nominal_native_ordering(types,actual)) ||
                (node->nominal.field_count && !node->nominal.fields)) { status=XR_XIR_CTFE_NOT_SUPPORTED;break; }
        }
        if (frame->next==ctfe_type_fields(node)) {
            if (node->kind==XR_XIR_TYPE_ARRAY && frame->dynamic) { status=XR_XIR_CTFE_NOT_SUPPORTED;break; }
            if (nominal && xr_xir_type_is_struct(types,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+frame->index)) && frame->dynamic) {
                status=XR_XIR_CTFE_NOT_SUPPORTED;break;
            }
            colors[frame->index]=2;costs[frame->index]=(XrXirCtfeTypeShape){frame->work,
                node->kind==XR_XIR_TYPE_ARRAY?!frame->pure_arrays:frame->dynamic,frame->pure_arrays};--depth;
            if (!depth) { *output=costs[frame->index];break; }
            CtfeTypeFrame *parent=&stack[depth-1];
            if (frame->work>UINT64_MAX-parent->work) { status=XR_XIR_CTFE_BUDGET;break; }
            parent->work+=frame->work;parent->dynamic|=costs[frame->index].dynamic;
            if (types->nodes[parent->index].kind==XR_XIR_TYPE_ARRAY) parent->pure_arrays=costs[frame->index].pure_arrays;continue;
        }
        if (!xir_compile_work(context,sizeof(XrXirCallableParameter)+2)) { status=XR_XIR_CTFE_BUDGET;break; }
        XrXirCallableParameter field=single?(XrXirCallableParameter){node->element,0}:
            nominal?(XrXirCallableParameter){node->nominal.fields[frame->next],0}:
            callable && frame->next==node->parameter_count?(XrXirCallableParameter){node->result,0}:node->parameters[frame->next];
        ++frame->next;
        if (field.mode) {
            if (!internal || !callable || field.mode!=XR_PARAM_REF || !xr_xir_type_is_cell(types,field.type)) {
                status=XR_XIR_CTFE_NOT_SUPPORTED;break;
            }
            field.type=xr_xir_cell_element(types,field.type);
        }
        if (single && field.type==XR_XIR_UNIT) { status=XR_XIR_CTFE_NOT_SUPPORTED;break; }
        if (internal && field.type==XR_XIR_ERROR) {
            if (frame->work==UINT64_MAX) { status=XR_XIR_CTFE_BUDGET;break; }
            ++frame->work;frame->dynamic=true;frame->pure_arrays=false;continue;
        }
        if (internal && field.type==XR_XIR_PANIC_INFO) {
            if (frame->work>UINT64_MAX-32) { status=XR_XIR_CTFE_BUDGET;break; }
            frame->work+=32;continue;
        }
        if (ctfe_result_type(field.type)) {
            if (frame->work==UINT64_MAX) { status=XR_XIR_CTFE_BUDGET;break; }
            ++frame->work;if (node->kind==XR_XIR_TYPE_ARRAY) frame->pure_arrays=field.type!=XR_XIR_UNIT;continue;
        }
        const XrXirTypeNode *child=ctfe_execution_node(types,field.type,internal);
        if (!child) { status=XR_XIR_CTFE_NOT_SUPPORTED;break; }
        index=(uint32_t)field.type-XR_XIR_CONSTRUCTED_TYPE_BASE;
        if (colors[index]==1) { status=XR_XIR_CTFE_NOT_SUPPORTED;break; }
        if (colors[index]==2) {
            if (costs[index].work>UINT64_MAX-frame->work) { status=XR_XIR_CTFE_BUDGET;break; }
            frame->work+=costs[index].work;frame->dynamic|=costs[index].dynamic;
            if (node->kind==XR_XIR_TYPE_ARRAY) frame->pure_arrays=costs[index].pure_arrays;continue;
        }
        if (depth>=types->count) { status=XR_XIR_CTFE_BAD_ARTIFACT;break; }
        if (!xir_compile_work(context,sizeof(*stack)+1)) { status=XR_XIR_CTFE_BUDGET;break; }
        stack[depth++]=(CtfeTypeFrame){(uint64_t)ctfe_type_fields(child)+1,index,0,child->kind==XR_XIR_TYPE_CALLABLE,child->kind==XR_XIR_TYPE_ARRAY};colors[index]=1;
    }
done:
    xr_compile_resources_free(memory);return status;
}
static XrXirCtfeStatus ctfe_equal_admit(const XrXirArtifact *artifact,XrXirType type) {
    for (;;) {
        if (!xir_compile_work(&artifact->context,2)) return XR_XIR_CTFE_BUDGET;
        if (ctfe_parameter_type(type)) return XR_XIR_CTFE_OK;
        const XrXirTypeNode *node=xr_xir_type_node(artifact->module.types,type);
        if (!node || (node->kind!=XR_XIR_TYPE_NULLABLE && node->kind!=XR_XIR_TYPE_ARRAY)) return XR_XIR_CTFE_NOT_SUPPORTED;
        type=node->element;
    }
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_type_admit(const XrXirCompileContext *context,
    const XrXirTypes *types,XrXirType type) {
    XrXirCtfeTypeShape shape={0};return ctfe_type_info(context,types,type,false,&shape);
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_type_shape(const XrXirCompileContext *context,
    const XrXirTypes *types,XrXirType type,XrXirCtfeTypeShape *output) {
    if (!output) return XR_XIR_CTFE_BAD_ARGUMENT;
    XrXirCtfeTypeShape shape={0};XrXirCtfeStatus status=ctfe_type_info(context,types,type,false,&shape);
    if (status==XR_XIR_CTFE_OK) *output=shape;return status;
}
static XrXirCtfeStatus ctfe_body_type_admit(const XrXirArtifact *artifact,XrXirType type,bool parameter) {
    if (parameter && type==XR_XIR_UNIT) return XR_XIR_CTFE_NOT_SUPPORTED;
    if (parameter && xr_xir_type_is_cell(artifact->module.types,type)) type=xr_xir_cell_element(artifact->module.types,type);
    XrXirCtfeTypeShape shape={0};return ctfe_type_info(&artifact->context,artifact->module.types,type,true,&shape);
}
static XrXirCtfeStatus ctfe_tuple_instruction_admit(const XrXirArtifact *artifact,
    const XrXirFunction *body,const XrXirInstruction *op) {
    if (op->op==XR_XIR_TO_STRING && !ctfe_scalar_type(xr_xir_operand_type(body,op->args[0]))) return XR_XIR_CTFE_NOT_SUPPORTED;
    bool array_receiver=op->op==XR_XIR_CELL_PROJECT || op->op==XR_XIR_ARRAY_GET || op->op==XR_XIR_ARRAY_LEN || op->op==XR_XIR_ARRAY_CAPACITY ||
        op->op==XR_XIR_ARRAY_SET || op->op==XR_XIR_ARRAY_PUSH || op->op==XR_XIR_ARRAY_RESERVE ||
        op->op==XR_XIR_PLACE_READ || op->op==XR_XIR_PLACE_WRITE || op->op==XR_XIR_INDEX_PLACE || op->op==XR_XIR_FIELD_PLACE || op->op==XR_XIR_STRUCT_SET;
    if (array_receiver) {
        const uint32_t *ids=op->op==XR_XIR_ARRAY_SET?&body->operands[op->args[0]]:op->args;
        XrXirPlaceKind kind=xr_xir_place_kind(body,ids[0]);
        if ((op->op==XR_XIR_CELL_PROJECT || op->op==XR_XIR_PLACE_READ || op->op==XR_XIR_PLACE_WRITE || op->op==XR_XIR_INDEX_PLACE || op->op==XR_XIR_FIELD_PLACE || op->op==XR_XIR_STRUCT_SET) &&
            kind!=XR_XIR_PLACE_CELL && kind!=XR_XIR_PLACE_INDEX && kind!=XR_XIR_PLACE_FIELD) return XR_XIR_CTFE_NOT_SUPPORTED;
        if (kind!=XR_XIR_PLACE_NONE && kind!=XR_XIR_PLACE_CELL && kind!=XR_XIR_PLACE_INDEX && kind!=XR_XIR_PLACE_FIELD) return XR_XIR_CTFE_NOT_SUPPORTED;
        uint32_t root=ids[0],depth=0;
        while (xr_xir_place_kind(body,root)==XR_XIR_PLACE_INDEX || xr_xir_place_kind(body,root)==XR_XIR_PLACE_FIELD) {
            if (!xir_compile_work(&artifact->context,4)) return XR_XIR_CTFE_BUDGET;
            if (++depth>body->instruction_count) return XR_XIR_CTFE_BAD_ARTIFACT;
            root=body->instructions[root-body->parameter_count].args[0];
        }
        if (depth && xr_xir_place_kind(body,root)!=XR_XIR_PLACE_CELL) return XR_XIR_CTFE_NOT_SUPPORTED;
        XrXirCtfeStatus receiver=ctfe_body_type_admit(artifact,xr_xir_operand_type(body,ids[0]),false);
        if (receiver!=XR_XIR_CTFE_OK) return receiver;
    }
    if (op->op==XR_XIR_CELL_PLACE && !xr_xir_type_is_cell(artifact->module.types,xr_xir_operand_type(body,op->args[0]))) return XR_XIR_CTFE_NOT_SUPPORTED;
    XrXirType type=op->type;
    if (op->op==XR_XIR_CELL_READ || op->op==XR_XIR_CELL_WRITE || op->op==XR_XIR_CELL_LOCAL_WRITE)
        type=xr_xir_operand_type(body,op->args[0]);
    if (xr_xir_type_is_cell(artifact->module.types,type))
        type=xr_xir_cell_element(artifact->module.types,type);
    XrXirCtfeStatus status=ctfe_body_type_admit(artifact,type,false);
    if (status==XR_XIR_CTFE_OK && (op->op==XR_XIR_TUPLE_FIELD || op->op==XR_XIR_NULLABLE_IS_SOME || op->op==XR_XIR_NULLABLE_UNWRAP ||
        op->op==XR_XIR_STRUCT_GET || op->op==XR_XIR_ENUM_TAG || op->op==XR_XIR_ENUM_GET))
        status=ctfe_body_type_admit(artifact,xr_xir_operand_type(body,op->args[0]),false);
    return status;
}
static XrXirCtfeStatus ctfe_tuple_scratch(const XrXirArtifact *artifact,uint32_t function,uint64_t *bytes) {
    *bytes=0;
    if (!artifact->layouts[function].outgoing_count) return XR_XIR_CTFE_OK;
    const XrXirFunction *body=&artifact->module.functions[function];uint32_t count=0;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        if (!xir_compile_work(&artifact->context,1)) return XR_XIR_CTFE_BUDGET;
        if (body->instructions[i].op==XR_XIR_ARRAY_NEW || body->instructions[i].op==XR_XIR_STRUCT_NEW || body->instructions[i].op==XR_XIR_ENUM_NEW || body->instructions[i].op==XR_XIR_FUNCTION_REF) {
            uint32_t arity=body->instructions[i].args[1];
            if (arity>count) count=arity;continue;
        }
        if (body->instructions[i].op!=XR_XIR_TUPLE_NEW) continue;
        const XrXirTypeNode *tuple=xr_xir_tuple_signature(artifact->module.types,body->instructions[i].type);
        if (!tuple || tuple->parameter_count>artifact->layouts[function].outgoing_count) return XR_XIR_CTFE_BAD_ARTIFACT;
        if (tuple->parameter_count>count) count=tuple->parameter_count;
    }
    *bytes=(uint64_t)count*sizeof(XrXirValue);return XR_XIR_CTFE_OK;
}
