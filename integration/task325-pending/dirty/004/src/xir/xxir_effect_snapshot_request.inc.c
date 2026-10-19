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

/* A signature projection copies only the fields used by formal and result
 * consumers. Every request retains the complete head storage preconditions. */
typedef struct EffectBodyFunctionSignature {
    const XrXirType *parameters;
    uint32_t parameter_count;
    XrXirType result;
} EffectBodyFunctionSignature;

typedef struct EffectBodyInstructionShape {
    XrXirOp op;
    XrXirType type;
} EffectBodyInstructionShape;

/* This stack view belongs to one uninterrupted compile-time read. It contains
 * only the actual Input identity, never a semantic result or stored verdict. */
typedef struct EffectBodyCurrentRead {
    const XrXirCompileContext *work;
    const EffectBodyMaterial *material;
} EffectBodyCurrentRead;
static XrXirStatus effect_body_current_open(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,EffectBodyCurrentRead *output);

typedef struct EffectBodyRequest EffectBodyRequest;
struct EffectBodyRequest {
    XirMetadataDomain domain;
    EffectBodyRead read;
    const void *owner;
    XrXirStatus (*head)(const EffectBodyRequest *,uint32_t,EffectBodyFunctionHead *,EffectBodyFunctionDimensions *,EffectBodyFunctionSignature *);
    XrXirStatus (*instruction)(const EffectBodyRequest *,uint32_t,uint32_t,
        XrXirInstruction *,const XrXirInstruction **,EffectBodyInstructionFields *);
    XrXirStatus (*type)(const EffectBodyRequest *,uint32_t,uint32_t,XrXirType *);
    XrXirStatus (*shape)(const EffectBodyRequest *,uint32_t,uint32_t,EffectBodyInstructionShape *);
    const EffectBodyCurrentRead *current;
};
static XrXirStatus effect_body_current_record(const EffectBodyRequest *request,
    uint32_t function,uint32_t instruction,const EffectBodyRun **output);

static XrXirStatus effect_body_raw_head(const EffectBodyRequest *request,uint32_t f,
    EffectBodyFunctionHead *output,EffectBodyFunctionDimensions *dimensions,EffectBodyFunctionSignature *signature) {
    const XrXirModule *module=request->owner;
    if (!module || (!!output+!!dimensions+!!signature)!=1 ||
        f>=module->function_count || !module->functions)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *a=&module->functions[f];
    if (signature) {
        if (!effect_body_read_work(&request->read,11+sizeof(*signature))) return XR_XIR_BUDGET;
        *signature=(EffectBodyFunctionSignature){a->parameters,a->parameter_count,a->result};return XR_XIR_OK;
    }
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
    EffectBodyFunctionHead *output,EffectBodyFunctionDimensions *dimensions,EffectBodyFunctionSignature *signature) {
    const EffectBodyMaterial *material=request->owner;
    if (!effect_body_material_shape(material) || (!!output+!!dimensions+!!signature)!=1 ||
        f>=effect_body_count(material))
        return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *a=&material->functions[f];
    if ((a->name_length && !a->name) || (a->parameter_count && !a->parameters) ||
        (a->block_count && !a->blocks) || (a->operand_count && !a->operands) ||
        (a->instruction_count && (!a->runs || !a->run_count)) ||
        a->run_count>a->run_capacity) return XR_XIR_BAD_STRUCTURE;
    if (signature) {
        if (!effect_body_read_work(&request->read,11+sizeof(*signature))) return XR_XIR_BUDGET;
        *signature=(EffectBodyFunctionSignature){a->parameters,a->parameter_count,a->result};return XR_XIR_OK;
    }
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
    if(request->current && !fields) {
        const EffectBodyRun *ref=NULL;
        XrXirStatus status=effect_body_current_record(request,f,i,&ref);
        if(status==XR_XIR_OK) status=effect_body_instruction_ref_value(&request->read,ref,i,storage);
        if(status!=XR_XIR_OK) return status;
        if(!effect_body_read_work(&request->read,sizeof(*output))) return XR_XIR_BUDGET;
        *output=storage;return XR_XIR_OK;
    }
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
    const EffectBodyFunction *head=&material->functions[f];
    if(request->current && output && value>=head->parameter_count &&
        (uint64_t)value<(uint64_t)head->parameter_count+head->instruction_count) {
        const EffectBodyRun *ref=NULL;uint32_t type=0,instruction=value-head->parameter_count;
        XrXirStatus status=effect_body_current_record(request,f,instruction,&ref);
        if(status==XR_XIR_OK) status=effect_body_run_word(&request->read,ref,instruction,1,&type);
        if(status!=XR_XIR_OK) return status;
        if(!effect_body_read_work(&request->read,sizeof(*output))) return XR_XIR_BUDGET;
        *output=(XrXirType)type;return XR_XIR_OK;
    }
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
        effect_body_raw_head,effect_body_raw_instruction,effect_body_raw_type,effect_body_raw_shape,NULL};
}
static inline EffectBodyRequest effect_body_owned_request(const XrXirCompileContext *work,
    const EffectBodyMaterial *body) {
    if (!xir_compile_context_valid(work) || !effect_body_material_shape(body) ||
        body->resources!=work->resources) return (EffectBodyRequest){0};
    return (EffectBodyRequest){body->domain,effect_body_compile_read(work),body,
        effect_body_owned_head,effect_body_owned_instruction,effect_body_owned_type,effect_body_owned_shape,NULL};
}
/* This is only the identity of the sole current storage adapter. Every
 * permission and final semantic fact is independently replayed by its consumer. */
static bool effect_body_request_owned_matches(const XrXirCompileContext *work,
    const EffectBodyRequest *request,const EffectBodyMaterial *material) {
    return xir_compile_context_valid(work) && request && effect_body_material_shape(material) &&
        material->resources==work->resources && request->owner==material &&
        request->read.owner==work && request->read.consume==effect_body_compile_consume &&
        request->head==effect_body_owned_head && request->instruction==effect_body_owned_instruction &&
        request->type==effect_body_owned_type && request->shape==effect_body_owned_shape &&
        request->domain.types==material->domain.types &&
        request->domain.function_count==effect_body_count(material) &&
        request->domain.declarations==material->domain.declarations &&
        request->domain.generics==material->domain.generics && request->domain.defaults==material->domain.defaults;
}

/* An unpublished projection has no live field, even if its owner is NULL.
 * Field checks avoid reading structure padding or an uninitialized tail. */
static bool effect_body_request_empty(const EffectBodyRequest *request) {
    return request && !request->owner && !request->head && !request->instruction &&
        !request->type && !request->shape && !request->current && !request->read.owner && !request->read.consume &&
        !request->domain.types && !request->domain.declarations && !request->domain.generics &&
        !request->domain.defaults && !request->domain.function_count;
}

static inline XrXirStatus effect_body_request_head(const EffectBodyRequest *request,
    uint32_t f,EffectBodyFunctionHead *output) {
    if (!request || !request->owner || !request->head || !request->domain.types ||
        !output || f>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    return request->head(request,f,output,NULL,NULL);
}
static inline XrXirStatus effect_body_request_signature(const EffectBodyRequest *request,
    uint32_t f,EffectBodyFunctionSignature *output) {
    if (!request || !request->owner || !request->head || !request->domain.types ||
        !output || f>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    return request->head(request,f,NULL,NULL,output);
}

static inline XrXirStatus effect_body_request_dimensions(const EffectBodyRequest *request,
    uint32_t f,EffectBodyFunctionDimensions *output) {
    if (!request || !request->owner || !request->head || !request->domain.types ||
        !output || f>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    return request->head(request,f,NULL,output,NULL);
}
static inline XrXirStatus effect_body_request_instruction(const EffectBodyRequest *request,
    uint32_t f,uint32_t i,XrXirInstruction *storage,const XrXirInstruction **output) {
    if (!request || !request->owner || !request->instruction || !request->domain.types ||
        !storage || !output || f>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    return request->instruction(request,f,i,storage,output,NULL);
}
/* Callback identity selects a storage adapter only. Every canonical read
 * still checks the current owner, metadata domain and complete physical row.
 * Delegated and raw requests retain their original reader and fees. */
static inline bool effect_body_request_sequence_owned(const EffectBodyRequest *request) {
    return request && request->head==effect_body_owned_head &&
        request->instruction==effect_body_owned_instruction && request->type==effect_body_owned_type &&
        request->shape==effect_body_owned_shape && request->read.consume==effect_body_compile_consume;
}
static XrXirStatus effect_body_request_sequence_binding(const EffectBodyRequest *request) {
    const EffectBodyMaterial *material=request->owner;
    const XrXirCompileContext *work=request->read.owner;
    if (!xir_compile_context_valid(work) || !effect_body_material_shape(material) ||
        material->resources!=work->resources || request->domain.types!=material->domain.types ||
        request->domain.declarations!=material->domain.declarations ||
        request->domain.generics!=material->domain.generics ||
        request->domain.defaults!=material->domain.defaults ||
        request->domain.function_count!=material->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    return effect_body_read_work(&request->read,6)?XR_XIR_OK:XR_XIR_BUDGET;
}
/* A constant record and the first row of any affine record already own the
 * complete physical Instruction. Borrow
 * that immutable row only for this request; no row, permission or selected
 * cause is cached. Other affine rows still use the unique full value decoder, and
 * delegated requests keep their original callback and failure sequence. */
static XrXirStatus effect_body_operation_ref(const EffectBodyRead *read,
    const EffectBodyRun *ref,uint32_t i,XrXirInstruction *storage,const XrXirInstruction **output) {
    if (ref->mask && i!=ref->first) {
        XrXirStatus status=effect_body_instruction_ref_value(read,ref,i,storage);
        if (status!=XR_XIR_OK) return status;
        if (!effect_body_read_work(read,sizeof(*output))) return XR_XIR_BUDGET;
        *output=storage;return XR_XIR_OK;
    }
    if (!effect_body_read_work(read,1+sizeof(ref->base.op)+sizeof(*output))) return XR_XIR_BUDGET;
    if (ref->base.op<=XR_XIR_INVALID || ref->base.op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    *output=&ref->base;return XR_XIR_OK;
}

static XrXirStatus effect_body_request_operation(const EffectBodyRequest *request,
    uint32_t f,uint32_t i,XrXirInstruction *storage,const XrXirInstruction **output) {
    if (!request || !request->owner || !request->instruction || !request->domain.types ||
        !storage || !output || f>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_request_sequence_owned(request))
        return effect_body_request_instruction(request,f,i,storage,output);
    XrXirStatus status=effect_body_request_sequence_binding(request);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyMaterial *material=request->owner;
    const EffectBodyRun *ref=NULL;
    status=effect_body_record_ref(&request->read,&material->functions[f],i,&ref);
    if (status!=XR_XIR_OK) return status;
    return effect_body_operation_ref(&request->read,ref,i,storage,output);
}

static XrXirStatus effect_body_request_sequence_begin(const EffectBodyRequest *request,
    EffectBodyRecordCursor *cursor) {
    if (!request || !request->owner || !request->instruction || !request->domain.types || !cursor)
        return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_request_sequence_owned(request)) return XR_XIR_OK;
    XrXirStatus status=effect_body_request_sequence_binding(request);
    if (status!=XR_XIR_OK) return status;
    if (!effect_body_read_work(&request->read,sizeof(*cursor))) return XR_XIR_BUDGET;
    *cursor=(EffectBodyRecordCursor){0,0,UINT32_MAX};return XR_XIR_OK;
}

/* Only numeric location survives one ascending pass. Failed scratch is
 * discarded by that pass; no decoded value or permission is retained. The
 * unique full-row decoder also reads all unused and signed physical fields. */
static XrXirStatus effect_body_request_sequence_instruction(const EffectBodyRequest *request,
    uint32_t f,uint32_t i,EffectBodyRecordCursor *cursor,XrXirInstruction *storage,
    const XrXirInstruction **output) {
    if (!request || !request->owner || !request->instruction || !request->domain.types ||
        !storage || !output || f>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_request_sequence_owned(request))
        return effect_body_request_instruction(request,f,i,storage,output);
    if (!cursor) return XR_XIR_BAD_STRUCTURE;
    if (cursor->last==UINT32_MAX?i!=0:i!=cursor->last+1) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_body_request_sequence_binding(request);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyMaterial *material=request->owner;
    if (!effect_body_read_work(&request->read,2)) return XR_XIR_BUDGET;
    const EffectBodyRun *ref=NULL;
    status=effect_body_record_cursor_ref(&request->read,&material->functions[f],i,cursor,&ref);
    if (status==XR_XIR_OK) status=effect_body_instruction_ref_value(&request->read,ref,i,storage);
    if (status!=XR_XIR_OK) return status;
    if (!effect_body_read_work(&request->read,sizeof(*output))) return XR_XIR_BUDGET;
    *output=storage;return XR_XIR_OK;
}

/* Local consumers may borrow the complete immutable base row. The copied
 * sequence reader retains its storage identity for callers that require it. */
static XrXirStatus effect_body_request_sequence_operation(const EffectBodyRequest *request,
    uint32_t f,uint32_t i,EffectBodyRecordCursor *cursor,XrXirInstruction *storage,
    const XrXirInstruction **output) {
    if (!request || !request->owner || !request->instruction || !request->domain.types ||
        !storage || !output || f>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_request_sequence_owned(request))
        return effect_body_request_instruction(request,f,i,storage,output);
    if (!cursor) return XR_XIR_BAD_STRUCTURE;
    if (cursor->last==UINT32_MAX?i!=0:i!=cursor->last+1) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_body_request_sequence_binding(request);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyMaterial *material=request->owner;
    if (!effect_body_read_work(&request->read,2)) return XR_XIR_BUDGET;
    const EffectBodyRun *ref=NULL;
    status=effect_body_record_cursor_ref(&request->read,&material->functions[f],i,cursor,&ref);
    if (status!=XR_XIR_OK) return status;
    return effect_body_operation_ref(&request->read,ref,i,storage,output);
}

/* Re-read the current final run and exact EOF independently of the cursor.
 * Completion describes this storage walk; Source, type, ROOT and terminal
 * correspondence remain the caller's independent full obligations. */
static XrXirStatus effect_body_request_sequence_finish(const EffectBodyRequest *request,
    uint32_t f,const EffectBodyRecordCursor *cursor) {
    if (!request || !request->owner || !request->instruction || !request->domain.types ||
        f>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_request_sequence_owned(request)) return XR_XIR_OK;
    if (!cursor) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_body_request_sequence_binding(request);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyMaterial *material=request->owner;
    const EffectBodyFunction *function=&material->functions[f];
    if (!effect_body_read_work(&request->read,7)) return XR_XIR_BUDGET;
    if (!function->instruction_count)
        return !function->run_count && !function->run_capacity && !function->runs &&
            !cursor->first && !cursor->run && cursor->last==UINT32_MAX?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    if (!function->run_count || cursor->run>=function->run_count ||
        cursor->run+1!=function->run_count || cursor->last!=function->instruction_count-1)
        return XR_XIR_BAD_STRUCTURE;
    const EffectBodyRun *ref=NULL;
    status=effect_body_run_ref(&request->read,function,cursor->run,cursor->first,&ref);
    if (status!=XR_XIR_OK) return status;
    if (!effect_body_read_work(&request->read,2)) return XR_XIR_BUDGET;
    return ref->first+ref->count==function->instruction_count?
        XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
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
