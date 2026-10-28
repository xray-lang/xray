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
 *   ledger-owned DFS separates internal graph reception from exportable data.
 */
#include "xxir_operand_roles.h"
#include "xxir_constraint_proof.h"
static bool ctfe_slice_u8_type(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    return node && node->kind==XR_XIR_TYPE_SLICE && node->element==XR_XIR_U8;
}
static bool ctfe_view_opcode(XrXirOp op) {
    return op==XR_XIR_VIEW_NEW || op==XR_XIR_VIEW_REBORROW || op==XR_XIR_VIEW_LEN ||
        op==XR_XIR_VIEW_GET || op==XR_XIR_VIEW_END || op==XR_XIR_VIEW_CLEANUP;
}
typedef struct CtfeTypeInfo { bool dynamic,pure_arrays,admitted; } CtfeTypeInfo;
typedef struct CtfeTypeFrame { uint32_t index,next;bool dynamic,pure_arrays; } CtfeTypeFrame;
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
    if (!node && internal) { node=xr_xir_type_node(types,type);if (node && node->kind!=XR_XIR_TYPE_CALLABLE && !xr_xir_type_is_class(types,type)) node=NULL; }
    return node;
}
static uint32_t ctfe_type_fields(const XrXirTypeNode *node) {
    return node->kind==XR_XIR_TYPE_CALLABLE?node->parameter_count+1:ctfe_composite_fields(node);
}
static XrXirCtfeStatus ctfe_nominal_owner_admit(const XrXirArtifact *artifact,uint32_t owner) {
    if (!owner) return XR_XIR_CTFE_OK;

    const XrXirNominalTable *table=artifact->module.types?artifact->module.types->nominals:NULL;
    if (!table || owner>table->count) return XR_XIR_CTFE_BAD_ARTIFACT;
    uint32_t kind=table->declarations?table->declarations[owner-1].kind:table->identities[owner-1].kind;
    /* A lexical declaration owner is not a class value or an effect grant.
     * Every reachable body and value family still requires separate admission. */
    return kind==XR_XIR_NOMINAL_STRUCT || kind==XR_XIR_NOMINAL_ENUM || kind==XR_XIR_NOMINAL_CLASS?
        XR_XIR_CTFE_OK:XR_XIR_CTFE_NOT_SUPPORTED;
}
static XrXirCtfeStatus ctfe_type_cycle_admit(
    const XrXirTypes *types,const CtfeTypeFrame *stack,uint32_t depth,uint32_t index) {
    bool reference=false,array_only=true;
    while (depth) {

        const CtfeTypeFrame *frame=&stack[--depth];const XrXirTypeNode *node=&types->nodes[frame->index];
        reference|=node->kind==XR_XIR_TYPE_ARRAY || node->kind==XR_XIR_TYPE_CALLABLE ||
            xr_xir_type_is_class(types,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+frame->index));
        array_only&=node->kind==XR_XIR_TYPE_ARRAY;
        if (frame->index==index) {
            /* Pure Array chains have no finite terminal admission query.
             * Inline cycles have no finite storage. Other reference cycles
             * describe types only; actual value graphs require independent admission. */
            return reference && !array_only?XR_XIR_CTFE_OK:XR_XIR_CTFE_NOT_SUPPORTED;
        }
    }
    return XR_XIR_CTFE_BAD_ARTIFACT;
}
/* Numeric shape information include the complete enum admission performed later by
 * the shared Value operation, even for None and empty Array operands. */
static XrXirCtfeStatus ctfe_unit_enum_admit(
    const XrXirTypes *types, const XrXirTypeNode *node) {

    const XrXirNominalTable *table=types->nominals;
    if (!table || node->nominal.declaration>=table->count) return XR_XIR_CTFE_BAD_ARTIFACT;
    const XrXirNominalVariant *variants;
    uint32_t count,arity,fields,kind;
    if (table->declarations) {

        const XrXirNominalDeclaration *d=&table->declarations[node->nominal.declaration];
        variants=d->variants; count=d->variant_count; arity=d->parameter_count;
        fields=d->field_count; kind=d->kind;
    } else if (table->identities) {

        const XrXirNominalIdentity *d=&table->identities[node->nominal.declaration];
        variants=d->variants; count=d->variant_count; arity=d->arity;
        fields=d->field_count; kind=d->kind;
    } else return XR_XIR_CTFE_BAD_ARTIFACT;
    if (kind!=XR_XIR_NOMINAL_ENUM || fields || node->nominal.field_count ||
        node->nominal.argument_count!=arity || (arity && !node->nominal.arguments) ||
        (count && !variants)) return XR_XIR_CTFE_NOT_SUPPORTED;
    for (uint32_t a=0;a<arity;++a) {

        if (node->nominal.arguments[a]==XR_XIR_UNIT) return XR_XIR_CTFE_NOT_SUPPORTED;
    }
    for (uint32_t v=0;v<count;++v) {

        if (variants[v].field_begin || variants[v].field_count) return XR_XIR_CTFE_NOT_SUPPORTED;
    }
    return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_type_info(const XrXirCompileContext *context,
    const XrXirTypes *types,XrXirType type,bool internal,CtfeTypeInfo *output) {
    if (!xir_compile_context_valid(context)) return XR_XIR_CTFE_BAD_ARGUMENT;
    if (internal && ctfe_slice_u8_type(types,type)) {
        *output=(CtfeTypeInfo){true,false,true};return XR_XIR_CTFE_OK;
    }
    if (internal && type==XR_XIR_ERROR) { *output=(CtfeTypeInfo){true,false,true};return XR_XIR_CTFE_OK; }
    if (internal && type==XR_XIR_PANIC_INFO) { *output=(CtfeTypeInfo){false,true,true};return XR_XIR_CTFE_OK; }
    if (ctfe_result_type(type)) { *output=(CtfeTypeInfo){false,type!=XR_XIR_UNIT,true};return XR_XIR_CTFE_OK; }
    const XrXirTypeNode *root=ctfe_execution_node(types,type,internal);
    if (!root || !types || !types->count) return XR_XIR_CTFE_NOT_SUPPORTED;
    uint64_t bytes=(uint64_t)types->count*(sizeof(CtfeTypeFrame)+sizeof(CtfeTypeInfo)+1);
    if (bytes>SIZE_MAX) return XR_XIR_CTFE_OVERFLOW;
    void *memory=NULL;
    XrXirCtfeStatus status=ctfe_compile_status(xir_compile_resource_status(
        xr_compile_resources_calloc(context->resources,1,(size_t)bytes,&memory)));
    if (status!=XR_XIR_CTFE_OK) return status;
    CtfeTypeFrame *stack=memory;CtfeTypeInfo *information=(CtfeTypeInfo *)(stack+types->count);
    uint8_t *colors=(uint8_t *)(information+types->count);uint32_t depth=1;

    uint32_t index=(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE;
    bool identity=internal && xr_xir_type_is_class(types,type);
    stack[0]=(CtfeTypeFrame){index,0,identity || root->kind==XR_XIR_TYPE_CALLABLE,root->kind==XR_XIR_TYPE_ARRAY};colors[index]=1;
    while (depth) {

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

            if (native && native->native_id && !xr_xir_nominal_native_ordering(types,actual)) {
                if (!internal || native->native_id!=XR_NATIVE_DECLARATION_UTF8_ERROR) {
                    status=XR_XIR_CTFE_NOT_SUPPORTED;break;
                }
                status=ctfe_compile_status(xr_xir_compile_nominal_structure_verify(context,types->nominals,types));
                if (status!=XR_XIR_CTFE_OK) break;
            }
            if (node->nominal.field_count && !node->nominal.fields) { status=XR_XIR_CTFE_NOT_SUPPORTED;break; }
            if (!frame->next && !node->nominal.field_count && xr_xir_type_is_enum(types,actual)) {
                status=ctfe_unit_enum_admit(types,node);
                if (status!=XR_XIR_CTFE_OK) break;
            }
        }
        if (frame->next==ctfe_type_fields(node)) {
            /* Pure data may contain variable descendants. Actual receiving
             * observes each node under its original owner; static family facts
             * never replace complete variable-graph admission. */
            colors[frame->index]=2;information[frame->index]=(CtfeTypeInfo){
                node->kind==XR_XIR_TYPE_ARRAY?!frame->pure_arrays:frame->dynamic,frame->pure_arrays,true};--depth;
            if (!depth) { *output=information[frame->index];break; }
            CtfeTypeFrame *parent=&stack[depth-1];
            parent->dynamic|=information[frame->index].dynamic;
            if (types->nodes[parent->index].kind==XR_XIR_TYPE_ARRAY) parent->pure_arrays=information[frame->index].pure_arrays;continue;
        }

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
            frame->dynamic=true;frame->pure_arrays=false;continue;
        }
        if (internal && field.type==XR_XIR_PANIC_INFO) {
            continue;
        }
        if (internal && xr_xir_type_is_class(types,field.type)) {
            /* An identity validates its fixed header, not its field graph.
             * Actual fields are observed separately before shared receiving. */
            frame->dynamic=true;frame->pure_arrays=false;continue;
        }
        if (ctfe_result_type(field.type)) {
            if (node->kind==XR_XIR_TYPE_ARRAY) frame->pure_arrays=field.type!=XR_XIR_UNIT;continue;
        }
        const XrXirTypeNode *child=ctfe_execution_node(types,field.type,internal);
        if (!child) { status=XR_XIR_CTFE_NOT_SUPPORTED;break; }
        index=(uint32_t)field.type-XR_XIR_CONSTRUCTED_TYPE_BASE;
        /* Ordinary composite descriptors refer only to earlier pool entries.
         * Nominal fields alone may close a separately verified recursive type;
         * a gray edge cannot legitimize malformed Tuple or Nullable metadata. */
        if (!nominal && index>=frame->index) { status=XR_XIR_CTFE_NOT_SUPPORTED;break; }
        if (colors[index]==1) {
            status=ctfe_type_cycle_admit(types,stack,depth,index);if (status!=XR_XIR_CTFE_OK) break;
            frame->dynamic=true;frame->pure_arrays=false;continue;
        }
        if (colors[index]==2) {
            frame->dynamic|=information[index].dynamic;
            if (node->kind==XR_XIR_TYPE_ARRAY) frame->pure_arrays=information[index].pure_arrays;continue;
        }
        if (depth>=types->count) { status=XR_XIR_CTFE_BAD_ARTIFACT;break; }

        stack[depth++]=(CtfeTypeFrame){index,0,child->kind==XR_XIR_TYPE_CALLABLE,child->kind==XR_XIR_TYPE_ARRAY};colors[index]=1;
    }
    xr_compile_resources_free(memory);return status;
}
/* Family summaries belong only to this synchronous reachable-body scan. Every
 * request verifies its complete Artifact, bodies and permissions afresh; these
 * immutable metadata information never authorize a Value, call or external export. */
static XrXirCtfeStatus ctfe_internal_type_summary(const XrXirArtifact *artifact,
    XrXirType type,CtfeTypeInfo **summaries,CtfeTypeInfo *output) {
    const XrXirTypes *types=artifact->module.types;
    if (!summaries || !ctfe_execution_node(types,type,true))
        return ctfe_type_info(&artifact->context,types,type,true,output);

    if (!*summaries) {
        void *memory=NULL;
        XrXirCtfeStatus status=ctfe_compile_status(xir_compile_resource_status(
            xr_compile_resources_calloc(artifact->context.resources,types->count,sizeof(**summaries),&memory)));
        if (status!=XR_XIR_CTFE_OK) return status;
        *summaries=memory;
    }
    uint32_t index=(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE;
    if ((*summaries)[index].admitted) { *output=(*summaries)[index];return XR_XIR_CTFE_OK; }
    XrXirCtfeStatus status=ctfe_type_info(&artifact->context,types,type,true,output);
    if (status==XR_XIR_CTFE_OK) (*summaries)[index]=*output;
    return status;
}
static XrXirCtfeStatus ctfe_equal_admit(const XrXirArtifact *artifact,XrXirType type) {
    for (;;) {

        if (ctfe_parameter_type(type)) return XR_XIR_CTFE_OK;
        const XrXirTypeNode *node=xr_xir_type_node(artifact->module.types,type);
        if (node && node->kind==XR_XIR_TYPE_NOMINAL && !node->parameter_span) {
            XrXirProofContext proof={&artifact->module,{XR_XIR_CONTEXT_FUNCTION,0,0}};
            XrXirStatus status=xr_xir_compile_type_markers_prove(&artifact->context,&proof,type,XR_XIR_CONSTRAINT_EQUAL);
            return status==XR_XIR_BAD_TYPE ? XR_XIR_CTFE_NOT_SUPPORTED : ctfe_compile_status(status);
        }
        if (!node || (node->kind!=XR_XIR_TYPE_NULLABLE && node->kind!=XR_XIR_TYPE_ARRAY)) return XR_XIR_CTFE_NOT_SUPPORTED;
        type=node->element;
    }
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_type_admit(const XrXirCompileContext *context,
    const XrXirTypes *types,XrXirType type) {
    CtfeTypeInfo shape={0};return ctfe_type_info(context,types,type,false,&shape);
}

static XrXirCtfeStatus ctfe_body_type_admit(const XrXirArtifact *artifact,XrXirType type,bool parameter,CtfeTypeInfo **summaries) {
    if (parameter && type==XR_XIR_UNIT) return XR_XIR_CTFE_NOT_SUPPORTED;
    if (parameter && xr_xir_type_is_cell(artifact->module.types,type)) type=xr_xir_cell_element(artifact->module.types,type);
    CtfeTypeInfo shape={0};return ctfe_internal_type_summary(artifact,type,summaries,&shape);
}
static XrXirCtfeStatus ctfe_tuple_instruction_admit(const XrXirArtifact *artifact,
    const XrXirFunction *body,const XrXirInstruction *op,CtfeTypeInfo **summaries) {
    if (op->op==XR_XIR_TO_STRING && !ctfe_scalar_type(xr_xir_operand_type(body,op->args[0]))) return XR_XIR_CTFE_NOT_SUPPORTED;
    bool array_receiver=op->op==XR_XIR_CELL_PROJECT || op->op==XR_XIR_ARRAY_GET || op->op==XR_XIR_ARRAY_LEN || op->op==XR_XIR_ARRAY_CAPACITY ||
        op->op==XR_XIR_ARRAY_SET || op->op==XR_XIR_ARRAY_PUSH || op->op==XR_XIR_ARRAY_RESERVE ||
        op->op==XR_XIR_PLACE_READ || op->op==XR_XIR_PLACE_WRITE || op->op==XR_XIR_INDEX_PLACE || op->op==XR_XIR_FIELD_PLACE || op->op==XR_XIR_STRUCT_SET;
    if (array_receiver) {
        const uint32_t *ids=op->op==XR_XIR_ARRAY_SET?&body->operands[op->args[0]]:op->args;
        XrXirPlaceKind kind=xr_xir_place_kind(body,ids[0]);
        if ((op->op==XR_XIR_CELL_PROJECT || op->op==XR_XIR_PLACE_READ || op->op==XR_XIR_PLACE_WRITE || op->op==XR_XIR_INDEX_PLACE || op->op==XR_XIR_FIELD_PLACE || op->op==XR_XIR_STRUCT_SET) &&
            kind!=XR_XIR_PLACE_OBJECT && kind!=XR_XIR_PLACE_CELL && kind!=XR_XIR_PLACE_INDEX && kind!=XR_XIR_PLACE_FIELD) return XR_XIR_CTFE_NOT_SUPPORTED;
        if (kind!=XR_XIR_PLACE_NONE && kind!=XR_XIR_PLACE_OBJECT && kind!=XR_XIR_PLACE_CELL && kind!=XR_XIR_PLACE_INDEX && kind!=XR_XIR_PLACE_FIELD) return XR_XIR_CTFE_NOT_SUPPORTED;
        uint32_t root=ids[0],depth=0;
        while (xr_xir_place_kind(body,root)==XR_XIR_PLACE_INDEX || xr_xir_place_kind(body,root)==XR_XIR_PLACE_FIELD) {

            if (++depth>body->instruction_count) return XR_XIR_CTFE_BAD_ARTIFACT;
            root=body->instructions[root-body->parameter_count].args[0];
        }
        if (depth && xr_xir_place_kind(body,root)!=XR_XIR_PLACE_CELL && xr_xir_place_kind(body,root)!=XR_XIR_PLACE_OBJECT) return XR_XIR_CTFE_NOT_SUPPORTED;
        XrXirCtfeStatus receiver=ctfe_body_type_admit(artifact,xr_xir_operand_type(body,ids[0]),false,summaries);
        if (receiver!=XR_XIR_CTFE_OK) return receiver;
    }
    if (op->op==XR_XIR_CELL_PLACE && !xr_xir_type_is_cell(artifact->module.types,xr_xir_operand_type(body,op->args[0]))) return XR_XIR_CTFE_NOT_SUPPORTED;
    if (op->op==XR_XIR_OBJECT_PLACE && !xr_xir_type_is_class(artifact->module.types,xr_xir_operand_type(body,op->args[0]))) return XR_XIR_CTFE_NOT_SUPPORTED;
    XrXirType type=op->type;
    if (op->op==XR_XIR_CELL_READ || op->op==XR_XIR_CELL_WRITE || op->op==XR_XIR_CELL_LOCAL_WRITE)
        type=xr_xir_operand_type(body,op->args[0]);
    if (xr_xir_type_is_cell(artifact->module.types,type))
        type=xr_xir_cell_element(artifact->module.types,type);
    XrXirCtfeStatus status=ctfe_body_type_admit(artifact,type,false,summaries);
    if (status==XR_XIR_CTFE_OK && (op->op==XR_XIR_TUPLE_FIELD || op->op==XR_XIR_NULLABLE_IS_SOME || op->op==XR_XIR_NULLABLE_UNWRAP ||
        op->op==XR_XIR_CLASS_GET || op->op==XR_XIR_CLASS_SET || op->op==XR_XIR_STRUCT_GET || op->op==XR_XIR_ENUM_TAG || op->op==XR_XIR_ENUM_GET))
        status=ctfe_body_type_admit(artifact,xr_xir_operand_type(body,op->args[0]),false,summaries);
    return status;
}
static XrXirCtfeStatus ctfe_tuple_scratch(const XrXirArtifact *artifact,uint32_t function,uint64_t *bytes) {
    *bytes=0;
    const XrXirFunction *body=&artifact->module.functions[function];uint32_t count=0;
    for (uint32_t i=0;i<body->instruction_count;++i) {

        if (ctfe_view_opcode(body->instructions[i].op)) {
            if (body->instructions[i].op!=XR_XIR_VIEW_CLEANUP && count<2) count=2;continue;
        }
        if (body->instructions[i].op==XR_XIR_ARRAY_NEW || body->instructions[i].op==XR_XIR_CLASS_NEW || body->instructions[i].op==XR_XIR_STRUCT_NEW || body->instructions[i].op==XR_XIR_ENUM_NEW || body->instructions[i].op==XR_XIR_FUNCTION_REF) {
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
