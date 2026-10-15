/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_materialization.inc.c - Bound private literal-data constructor edges
 *
 * KEY CONCEPT:
 *   An owned catalogue restricts every use of a private zero-argument helper.
 *   Complete ordinary checking still proves its declaration, types and access.
 */
#include "xxir_operand_roles.h"
typedef struct MaterializationTypeFrame { XrXirType type;uint32_t next; } MaterializationTypeFrame;
typedef struct MaterializationCallFrame { uint32_t function,next; } MaterializationCallFrame;
static bool materialization_scalar(XrXirType type) {
    return type==XR_XIR_UNIT || type==XR_XIR_BOOL || type==XR_XIR_RUNE ||
        type==XR_XIR_STRING || xr_xir_type_is_number(type);
}
static XrXirStatus materialization_data_type(const XrXirCompileContext *context,
    const XrXirTypes *types,XrXirType type) {
    if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
    if (materialization_scalar(type)) return XR_XIR_OK;
    if (!types || !types->count || !xr_xir_type_node(types,type)) return XR_XIR_BAD_TYPE;
    uint64_t bytes=(uint64_t)types->count*(sizeof(MaterializationTypeFrame)+1);
    if (bytes>SIZE_MAX) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    MaterializationTypeFrame *stack=xir_compile_calloc(context,1,(size_t)bytes,&status);
    if (!stack) return status;
    uint8_t *colors=(uint8_t *)(stack+types->count);uint32_t depth=1;
    stack[0]=(MaterializationTypeFrame){type,0};colors[(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE]=1;
    while (depth) {
        if (!xir_compile_work(context,8)) { status=XR_XIR_BUDGET;break; }
        MaterializationTypeFrame *frame=&stack[depth-1];
        const XrXirTypeNode *node=xr_xir_type_node(types,frame->type);
        bool single=node->kind==XR_XIR_TYPE_ARRAY || node->kind==XR_XIR_TYPE_NULLABLE;
        bool nominal=xr_xir_type_is_struct(types,frame->type) || xr_xir_type_is_enum(types,frame->type);
        if (node->parameter_span || (!single && !nominal && node->kind!=XR_XIR_TYPE_TUPLE)) { status=XR_XIR_BAD_TYPE;break; }
        if (nominal) {
            const XrXirNominalTable *table=types->nominals;uint32_t declaration=node->nominal.declaration;
            if (!table || declaration>=table->count || (!table->declarations && !table->identities)) { status=XR_XIR_BAD_STRUCTURE;break; }
            uint32_t fields=table->declarations?table->declarations[declaration].field_count:table->identities[declaration].field_count;
            if (node->nominal.field_count!=fields) { status=XR_XIR_BAD_TYPE;break; }
        }
        uint32_t count=single?1:nominal?node->nominal.field_count:node->parameter_count;
        if (frame->next==count) { colors[(uint32_t)frame->type-XR_XIR_CONSTRUCTED_TYPE_BASE]=2;--depth;continue; }
        if (!xir_compile_work(context,sizeof(XrXirCallableParameter)+1)) { status=XR_XIR_BUDGET;break; }
        XrXirType child=single?node->element:nominal?node->nominal.fields[frame->next]:node->parameters[frame->next].type;
        if (!single && !nominal && node->parameters[frame->next].mode) { status=XR_XIR_BAD_TYPE;break; }
        ++frame->next;
        if (materialization_scalar(child)) {
            if (single && child==XR_XIR_UNIT) { status=XR_XIR_BAD_TYPE;break; }
            continue;
        }
        if (!xr_xir_type_node(types,child)) { status=XR_XIR_BAD_TYPE;break; }
        uint32_t index=(uint32_t)child-XR_XIR_CONSTRUCTED_TYPE_BASE;
        if (colors[index]==1) { status=XR_XIR_BAD_TYPE;break; }
        if (colors[index]==2) continue;
        if (depth==types->count) { status=XR_XIR_BAD_STRUCTURE;break; }
        colors[index]=1;stack[depth++]=(MaterializationTypeFrame){child,0};
    }
    xr_compile_resources_free(stack);return status;
}
static bool materialization_opcode(XrXirOp op) {
    switch (op) {
    case XR_XIR_CONST_BOOL:case XR_XIR_CONST_INT:case XR_XIR_CONST_FLOAT:
    case XR_XIR_CONST_RUNE:case XR_XIR_CONST_STRING:case XR_XIR_COPY:
    case XR_XIR_SCALAR_COPY:case XR_XIR_OWNED_RETAIN:case XR_XIR_RETURN:
    case XR_XIR_TUPLE_NEW:case XR_XIR_NULLABLE_NONE:case XR_XIR_NULLABLE_SOME:
    case XR_XIR_STRUCT_NEW:case XR_XIR_ENUM_NEW:case XR_XIR_ARRAY_NEW:
    case XR_XIR_ARRAY_WITH_CAPACITY:case XR_XIR_ARRAY_RESERVE:case XR_XIR_ARRAY_PUSH:
    case XR_XIR_CELL_NEW:case XR_XIR_CELL_READ:case XR_XIR_CELL_PLACE:
    case XR_XIR_CALL:return true;
    default:return false;
    }
}
static XrXirStatus materialization_target(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirMaterializationBinding *binding) {
    const XrXirFunction *body=&module->functions[binding->function];
    const XrXirFunctionIdentity *id=&module->declarations->functions[binding->function];
    if (id->method_kind!=XR_XIR_MATERIALIZER || id->nominal_owner!=binding->nominal+1 ||
        id->exported || id->member_access || id->cleanup_owner || id->test_role || id->promises ||
        body->parameter_count || body->parameters || body->block_count!=1 || !body->blocks ||
        !body->instruction_count || !body->instructions) return XR_XIR_BAD_STRUCTURE;
    if (module->generics && (module->generics[binding->function].parameter_count ||
        module->generics[binding->function].argument_count)) return XR_XIR_BAD_TYPE;
    const XrXirTypeNode *result=xr_xir_type_node(module->types,body->result);
    if (!result || result->kind!=XR_XIR_TYPE_NOMINAL || result->nominal.declaration!=binding->nominal)
        return XR_XIR_BAD_TYPE;
    XrXirStatus status=materialization_data_type(context,module->types,body->result);
    for (uint32_t i=0;status==XR_XIR_OK && i<body->instruction_count;++i) {
        if (!xir_compile_work(context,sizeof(XrXirInstruction))) return XR_XIR_BUDGET;
        const XrXirInstruction *op=&body->instructions[i];
        if (!materialization_opcode(op->op)) return XR_XIR_BAD_TYPE;
        XrXirType type=op->type;
        if (xr_xir_type_is_cell(module->types,type)) {
            type=xr_xir_cell_element(module->types,type);
            if (!xr_xir_type_is_array(module->types,type)) return XR_XIR_BAD_TYPE;
        }
        status=materialization_data_type(context,module->types,type);
        if (op->op==XR_XIR_CALL) {
            if (op->immediate<0 || (uint64_t)op->immediate>=module->function_count || op->args[1] ||
                module->declarations->functions[op->immediate].method_kind!=XR_XIR_MATERIALIZER)
                return XR_XIR_BAD_TYPE;
        }
    }
    return status;
}
/* All lookups have at most32 comparisons over uint32 coordinates. The caller
 * prepays their complete fixed reserve before observing catalogue storage. */
static uint32_t materialization_first(const XrXirConstruction *owner,uint32_t caller) {
    uint32_t low=0,high=owner->materialization_count;
    while (low<high) {
        uint32_t middle=low+(high-low)/2;
        if (owner->materializations[middle].caller<caller) low=middle+1;else high=middle;
    }
    return low;
}
static XrXirStatus materialization_acyclic(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirConstruction *owner) {
    uint64_t bytes=(uint64_t)module->function_count*(sizeof(MaterializationCallFrame)+1);
    if (bytes>SIZE_MAX) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    MaterializationCallFrame *stack=xir_compile_calloc(context,1,(size_t)bytes,&status);
    if (!stack) return status;
    uint8_t *colors=(uint8_t *)(stack+module->function_count);uint32_t depth=0;
    for (uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f) {
        if (!xir_compile_work(context,4)) { status=XR_XIR_BUDGET;break; }
        if (module->declarations->functions[f].method_kind!=XR_XIR_MATERIALIZER || colors[f]) continue;
        if (!xir_compile_work(context,256)) { status=XR_XIR_BUDGET;break; }
        colors[f]=1;stack[depth++]=(MaterializationCallFrame){f,materialization_first(owner,f)};
        while (depth) {
            if (!xir_compile_work(context,sizeof(XrXirMaterializationBinding)+8)) { status=XR_XIR_BUDGET;break; }
            MaterializationCallFrame *frame=&stack[depth-1];
            if (frame->next==owner->materialization_count || owner->materializations[frame->next].caller!=frame->function) {
                colors[frame->function]=2;--depth;continue;
            }
            uint32_t child=owner->materializations[frame->next++].function;
            if (colors[child]==1) { status=XR_XIR_BAD_TYPE;break; }
            if (colors[child]==2) continue;
            if (depth==module->function_count) { status=XR_XIR_BAD_STRUCTURE;break; }
            if (!xir_compile_work(context,256)) { status=XR_XIR_BUDGET;break; }
            colors[child]=1;stack[depth++]=(MaterializationCallFrame){child,materialization_first(owner,child)};
        }
    }
    xr_compile_resources_free(stack);return status;
}
XR_FUNC XrXirStatus xir_materialization_verify(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirConstruction *owner) {
    const XrXirDeclarations *d=module->declarations;uint32_t helpers=0;
    for (uint32_t f=0;d && f<module->function_count;++f) {
        if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
        if (d->functions[f].method_kind==XR_XIR_MATERIALIZER) ++helpers;
    }
    if (!helpers) return owner->materialization_count?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
    if (!d || !owner->materialization_count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i=0;module->defaults && i<module->defaults->count;++i) {
        if (!xir_compile_work(context,4)) return XR_XIR_BUDGET;
        uint32_t function=module->defaults->records[i].function;
        if (function<module->function_count && d->functions[function].method_kind==XR_XIR_MATERIALIZER)
            return XR_XIR_BAD_TYPE;
    }
    const XrXirImplementationTable *implementations=d->implementations;
    for (uint32_t i=0;implementations && i<implementations->count;++i) {
        const XrXirImplementation *implementation=&implementations->records[i];
        for (uint32_t j=0;j<implementation->binding_count;++j) {
            if (!xir_compile_work(context,4)) return XR_XIR_BUDGET;
            uint32_t function=implementation->bindings[j].function;
            if (function<module->function_count && d->functions[function].method_kind==XR_XIR_MATERIALIZER)
                return XR_XIR_BAD_TYPE;
        }
    }
    XrXirStatus status=XR_XIR_OK;
    uint8_t *covered=xir_compile_calloc(context,module->function_count,1,&status);
    if (!covered) return status;
    for (uint32_t i=0;i<owner->materialization_count;++i) {
        if (!xir_compile_work(context,sizeof(XrXirMaterializationBinding)+8)) { status=XR_XIR_BUDGET;break; }
        const XrXirMaterializationBinding *row=&owner->materializations[i];
        if (row->caller>=module->function_count || row->function>=module->function_count) { status=XR_XIR_BAD_STRUCTURE;break; }
        const XrXirFunction *caller=&module->functions[row->caller];
        if (!caller->instructions || row->instruction>=caller->instruction_count) { status=XR_XIR_BAD_STRUCTURE;break; }
        const XrXirInstruction *edge=&caller->instructions[row->instruction];
        if (edge->op!=XR_XIR_CALL || edge->immediate!=row->function || edge->args[0] || edge->args[1]) { status=XR_XIR_BAD_STRUCTURE;break; }
        if (!covered[row->function]) status=materialization_target(context,module,row);
        else if (d->functions[row->function].nominal_owner!=row->nominal+1) status=XR_XIR_BAD_STRUCTURE;
        if (status!=XR_XIR_OK) break;
        covered[row->function]=1;
    }
    for (uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f) {
        if (!xir_compile_work(context,4)) { status=XR_XIR_BUDGET;break; }
        const XrXirFunction *body=&module->functions[f];
        if (d->functions[f].method_kind==XR_XIR_MATERIALIZER && !covered[f]) { status=XR_XIR_BAD_STRUCTURE;break; }
        if (body->instruction_count && !body->instructions) { status=XR_XIR_BAD_STRUCTURE;break; }
        for (uint32_t i=0;i<body->instruction_count;++i) {
            if (!xir_compile_work(context,4)) { status=XR_XIR_BUDGET;break; }
            const XrXirInstruction *op=&body->instructions[i];
            bool call=xr_xir_op_references_function(op->op);
            if (!call || op->immediate<0 || (uint64_t)op->immediate>=module->function_count ||
                d->functions[op->immediate].method_kind!=XR_XIR_MATERIALIZER) continue;
            if (!xir_compile_work(context,256)) { status=XR_XIR_BUDGET;break; }
            const XrXirMaterializationBinding *row=xir_materialization_binding(owner,f,i);
            if (op->op!=XR_XIR_CALL || !row || row->function!=(uint32_t)op->immediate) { status=XR_XIR_BAD_TYPE;break; }
        }
    }
    if (status==XR_XIR_OK) status=materialization_acyclic(context,module,owner);
    xr_compile_resources_free(covered);return status;
}
