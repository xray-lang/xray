/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_snapshot_request.inc.c - Lifetime adapters for one physical replay
 *
 * Complete heads and status-returning reads contain no executable admission.
 * Raw construction and owned certificate replay consume the same algorithm.
 */
typedef struct EffectBodyFunctionHead {
    const char *name;
    uint32_t name_length;
    const XrXirType *parameters;
    uint32_t parameter_count;
    XrXirType result;
    const XrXirBlock *blocks;
    uint32_t block_count,instruction_count;
    const uint32_t *operands;
    uint32_t operand_count;
} EffectBodyFunctionHead;
/* A partial head request retains the same storage preconditions. It writes
 * only the two real dimensions consumed by this call edge, never a complete
 * unused head or an independently trusted metadata view. */
typedef struct EffectBodyFunctionDimensions {
    uint32_t parameter_count,instruction_count;
} EffectBodyFunctionDimensions;

typedef struct EffectBodyInstructionShape {
    XrXirOp op;
    XrXirType type;
} EffectBodyInstructionShape;

typedef struct EffectBodyRequest EffectBodyRequest;
struct EffectBodyRequest {
    XirMetadataDomain domain;
    EffectBodyRead read;
    const void *owner;
    XrXirStatus (*head)(const EffectBodyRequest *,uint32_t,EffectBodyFunctionHead *,EffectBodyFunctionDimensions *);
    XrXirStatus (*instruction)(const EffectBodyRequest *,uint32_t,uint32_t,
        XrXirInstruction *,const XrXirInstruction **,EffectBodyInstructionFields *);
    XrXirStatus (*type)(const EffectBodyRequest *,uint32_t,uint32_t,XrXirType *);
    XrXirStatus (*shape)(const EffectBodyRequest *,uint32_t,uint32_t,EffectBodyInstructionShape *);
};

static XrXirStatus effect_body_raw_head(const EffectBodyRequest *request,uint32_t f,
    EffectBodyFunctionHead *output,EffectBodyFunctionDimensions *dimensions) {
    const XrXirModule *module=request->owner;
    if (!module || (!output && !dimensions) || (output && dimensions) ||
        f>=module->function_count || !module->functions)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *a=&module->functions[f];
    if (dimensions) {
        if (!effect_body_read_work(&request->read,11+sizeof(*dimensions))) return XR_XIR_BUDGET;
        *dimensions=(EffectBodyFunctionDimensions){a->parameter_count,a->instruction_count};return XR_XIR_OK;
    }
    if (!effect_body_read_work(&request->read,11+sizeof(*output))) return XR_XIR_BUDGET;
    *output=(EffectBodyFunctionHead){a->name,a->name_length,a->parameters,a->parameter_count,
        a->result,a->blocks,a->block_count,a->instruction_count,a->operands,a->operand_count};
    return XR_XIR_OK;
}
static XrXirStatus effect_body_raw_instruction(const EffectBodyRequest *request,
    uint32_t f,uint32_t i,XrXirInstruction *storage,const XrXirInstruction **output,
    EffectBodyInstructionFields *fields) {
    (void)fields;
    const XrXirModule *module=request->owner;
    if (!module || !storage || !output || f>=module->function_count || !module->functions)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *body=&module->functions[f];
    if (i>=body->instruction_count || !body->instructions) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(&request->read,2+sizeof(*output))) return XR_XIR_BUDGET;
    *output=&body->instructions[i];return XR_XIR_OK;
}
static XrXirStatus effect_body_raw_type(const EffectBodyRequest *request,uint32_t f,
    uint32_t value,XrXirType *output) {
    const XrXirModule *module=request->owner;
    if (!module || !output || f>=module->function_count || !module->functions)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *body=&module->functions[f];
    if (value>=(uint64_t)body->parameter_count+body->instruction_count ||
        (value<body->parameter_count?!body->parameters:!body->instructions))
        return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(&request->read,1+sizeof(*output))) return XR_XIR_BUDGET;
    *output=xr_xir_operand_type(body,value);return XR_XIR_OK;
}
static XrXirStatus effect_body_raw_shape(const EffectBodyRequest *request,uint32_t f,
    uint32_t i,EffectBodyInstructionShape *output) {
    const XrXirModule *module=request->owner;
    if (!module || !output || f>=module->function_count || !module->functions ||
        i>=module->functions[f].instruction_count || !module->functions[f].instructions)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *operation=&module->functions[f].instructions[i];
    if (!effect_body_read_work(&request->read,4)) return XR_XIR_BUDGET;
    XrXirOp opcode=operation->op;XrXirType type=operation->type;
    if (opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(&request->read,sizeof(*output))) return XR_XIR_BUDGET;
    *output=(EffectBodyInstructionShape){opcode,type};return XR_XIR_OK;
}
static XrXirStatus effect_body_owned_head(const EffectBodyRequest *request,uint32_t f,
    EffectBodyFunctionHead *output,EffectBodyFunctionDimensions *dimensions) {
    const EffectBodyMaterial *material=request->owner;
    if (!effect_body_material_shape(material) || (!output && !dimensions) || (output && dimensions) ||
        f>=effect_body_count(material))
        return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *a=&material->functions[f];
    if ((a->name_length && !a->name) || (a->parameter_count && !a->parameters) ||
        (a->block_count && !a->blocks) || (a->operand_count && !a->operands) ||
        (a->instruction_count && (!a->stream || !a->run_count)) ||
        a->stream_words>a->stream_capacity) return XR_XIR_BAD_STRUCTURE;
    if (dimensions) {
        if (!effect_body_read_work(&request->read,11+sizeof(*dimensions))) return XR_XIR_BUDGET;
        *dimensions=(EffectBodyFunctionDimensions){a->parameter_count,a->instruction_count};return XR_XIR_OK;
    }
    if (!effect_body_read_work(&request->read,11+sizeof(*output))) return XR_XIR_BUDGET;
    *output=(EffectBodyFunctionHead){a->name,a->name_length,a->parameters,a->parameter_count,
        a->result,a->blocks,a->block_count,a->instruction_count,a->operands,a->operand_count};
    return XR_XIR_OK;
}
static XrXirStatus effect_body_owned_instruction(const EffectBodyRequest *request,
    uint32_t f,uint32_t i,XrXirInstruction *storage,const XrXirInstruction **output,
    EffectBodyInstructionFields *fields) {
    const EffectBodyMaterial *material=request->owner;
    if (!effect_body_material_shape(material) || !storage || !output || f>=effect_body_count(material))
        return XR_XIR_BAD_STRUCTURE;
    if (fields) return effect_body_instruction_fields_get(&request->read,&material->functions[f],i,fields);
    XrXirStatus status=effect_body_instruction_get(&request->read,&material->functions[f],i,storage);
    if (status!=XR_XIR_OK) return status;
    if (!effect_body_read_work(&request->read,sizeof(*output))) return XR_XIR_BUDGET;
    *output=storage;return XR_XIR_OK;
}
static XrXirStatus effect_body_owned_type(const EffectBodyRequest *request,uint32_t f,
    uint32_t value,XrXirType *output) {
    const EffectBodyMaterial *material=request->owner;
    if (!effect_body_material_shape(material) || f>=effect_body_count(material))
        return XR_XIR_BAD_STRUCTURE;
    return effect_body_operand_type(&request->read,&material->functions[f],value,output);
}
static XrXirStatus effect_body_owned_shape(const EffectBodyRequest *request,uint32_t f,
    uint32_t i,EffectBodyInstructionShape *output) {
    const EffectBodyMaterial *material=request->owner;
    if (!effect_body_material_shape(material) || !output || f>=effect_body_count(material))
        return XR_XIR_BAD_STRUCTURE;
    uint32_t opcode=XR_XIR_INVALID,type=XR_XIR_UNIT;
    XrXirStatus status=effect_body_instruction_shape_words(&request->read,&material->functions[f],i,&opcode,&type);
    if (status!=XR_XIR_OK) return status;
    if (!effect_body_read_work(&request->read,sizeof(*output))) return XR_XIR_BUDGET;
    *output=(EffectBodyInstructionShape){(XrXirOp)opcode,(XrXirType)type};return XR_XIR_OK;
}
static inline EffectBodyRequest effect_body_raw_request(const XrXirCompileContext *work,
    const XrXirModule *module) {
    XirMetadataDomain domain=xir_metadata_domain_from_module(module);
    if (!domain.types) domain.types=&effect_empty_types;
    return (EffectBodyRequest){domain,effect_body_compile_read(work),module,
        effect_body_raw_head,effect_body_raw_instruction,effect_body_raw_type,effect_body_raw_shape};
}
static inline EffectBodyRequest effect_body_owned_request(const XrXirCompileContext *work,
    const EffectBodyMaterial *body) {
    if (!xir_compile_context_valid(work) || !effect_body_material_shape(body) ||
        body->resources!=work->resources) return (EffectBodyRequest){0};
    return (EffectBodyRequest){body->domain,effect_body_compile_read(work),body,
        effect_body_owned_head,effect_body_owned_instruction,effect_body_owned_type,effect_body_owned_shape};
}
static inline XrXirStatus effect_body_request_head(const EffectBodyRequest *request,
    uint32_t f,EffectBodyFunctionHead *output) {
    if (!request || !request->owner || !request->head || !request->domain.types ||
        !output || f>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    return request->head(request,f,output,NULL);
}
static inline XrXirStatus effect_body_request_dimensions(const EffectBodyRequest *request,
    uint32_t f,EffectBodyFunctionDimensions *output) {
    if (!request || !request->owner || !request->head || !request->domain.types ||
        !output || f>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    return request->head(request,f,NULL,output);
}
static inline XrXirStatus effect_body_request_instruction(const EffectBodyRequest *request,
    uint32_t f,uint32_t i,XrXirInstruction *storage,const XrXirInstruction **output) {
    if (!request || !request->owner || !request->instruction || !request->domain.types ||
        !storage || !output || f>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    return request->instruction(request,f,i,storage,output,NULL);
}
/* Raw storage still returns its original row pointer and cost. Owned
 * storage publishes only the requested fields after complete record checks. */
static inline XrXirStatus effect_body_request_fields(const EffectBodyRequest *request,
    uint32_t f,uint32_t i,XrXirInstruction *storage,const XrXirInstruction **output,
    EffectBodyInstructionFields *fields) {
    if (!request || !request->owner || !request->instruction || !request->domain.types ||
        !storage || !output || *output || !fields || f>=request->domain.function_count)
        return XR_XIR_BAD_STRUCTURE;
    return request->instruction(request,f,i,storage,output,fields);
}
static inline XrXirStatus effect_body_request_type(const EffectBodyRequest *request,
    uint32_t f,uint32_t value,XrXirType *output) {
    if (!request || !request->owner || !request->type || !request->domain.types ||
        !output || f>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    return request->type(request,f,value,output);
}
static inline XrXirStatus effect_body_request_shape(const EffectBodyRequest *request,
    uint32_t f,uint32_t i,EffectBodyInstructionShape *output) {
    if (!request || !request->owner || !request->shape || !request->domain.types ||
        !output || f>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    return request->shape(request,f,i,output);
}
