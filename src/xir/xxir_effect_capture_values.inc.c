/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_capture_values.inc.c - Current physical value origin clauses
 *
 * KEY CONCEPT:
 *   Advertisements coexist with identities. Fallback never supplies the same
 *   value's raw-presence flag, and initial keys retain a separate positive proof.
 */
static XrXirStatus effect_capture_bit(EffectCaptureRead *read,const EffectCaptureNode *node,
    uint32_t value,uint32_t base,uint32_t bit,uint64_t rank) {
    return effect_capture_emit(read,(EffectCaptureFact){node->node,value,base+bit/64,bit%64,rank});
}
static XrXirStatus effect_capture_opaque(EffectCaptureRead *read,const EffectCaptureNode *node,
    const EffectCaptureFact *destination,uint32_t mask,bool raw) {
    uint32_t opaque=read->final->site_count+node->basis.parameters;
    XrXirStatus status=effect_capture_bit(read,node,destination->value,0,opaque,destination->rank);
    if(status==XR_XIR_OK && (mask&XR_XIR_CALLABLE_ROOT_REQUIRED))
        status=effect_capture_bit(read,node,destination->value,0,opaque+1,destination->rank);
    if(status==XR_XIR_OK && (mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED))
        status=effect_capture_bit(read,node,destination->value,0,opaque+2,destination->rank);
    if(status==XR_XIR_OK && raw) status=effect_capture_emit(read,(EffectCaptureFact){node->node,destination->value,
        EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_RAW,destination->rank});
    return status;
}
static XrXirStatus effect_capture_finish(EffectCaptureRead *read,const EffectCaptureNode *node,uint32_t value) {
    const EffectCaptureFact *fixed=NULL;XrXirType type=0;uint32_t kind=0;
    XrXirStatus status=effect_capture_kind(read,node,value,&kind,&type);if(status!=XR_XIR_OK) return status;
    if(kind!=EFFECT_CAPTURE_CALLABLE || read->fixed_only) return XR_XIR_OK;
    status=effect_capture_find(read,(EffectCaptureFact){node->node,value,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_FIXED,0},&fixed);
    if(status!=XR_XIR_OK) return status;
    if(fixed) {
        EffectCaptureFact destination={node->node,value,0,0,fixed->rank+1};
        const XrXirTypeNode *signature=xr_xir_callable_signature(read->body.domain.types,type);
        if(!signature) return XR_XIR_BAD_STRUCTURE;
        status=effect_capture_opaque(read,node,&destination,signature->flags,true);if(status!=XR_XIR_OK) return status;
    }
    if(read->fallback) {
        const EffectCaptureFact *present=NULL;
        status=effect_capture_find(read,(EffectCaptureFact){node->node,value,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_RAW,0},&present);
        if(status!=XR_XIR_OK) return status;
        if(!present) {
            EffectCaptureFact destination={node->node,value,0,0,0};
            status=effect_capture_opaque(read,node,&destination,XR_XIR_CALLABLE_ROOT_UNRESOLVED,false);
        }
    }
    return status;
}
static XrXirStatus effect_capture_alias(EffectCaptureRead *read,const EffectCaptureNode *node,
    uint32_t source,uint32_t destination) {
    if(source>=node->values || destination>node->values) return XR_XIR_BAD_STRUCTURE;
    uint32_t kind=0,provided=0;XrXirStatus status=effect_capture_kind(read,node,destination,&kind,NULL);
    if(status==XR_XIR_OK) status=effect_capture_kind(read,node,source,&provided,NULL);
    if(status!=XR_XIR_OK || kind==EFFECT_CAPTURE_SCALAR) return status;
    if(kind!=provided) return XR_XIR_BAD_TYPE;
    uint32_t first=kind==EFFECT_CAPTURE_CALLABLE?0:node->basis.fn_words;
    uint32_t words=kind==EFFECT_CAPTURE_CALLABLE?node->basis.fn_words:node->basis.cell_words;
    if(!read->fixed_only) {
        EffectCaptureTransfer edge={.from=node->node,.value=source,.to=node->node,.destination=destination,
            .first=first,.word=first,.words=words};
        status=effect_capture_transfer(read,&edge);if(status!=XR_XIR_OK) return status;
    }
    if((kind==EFFECT_CAPTURE_CALLABLE && destination<node->values) || kind==EFFECT_CAPTURE_CLASS) {
        for(uint32_t flag=kind==EFFECT_CAPTURE_CLASS?EFFECT_CAPTURE_RAW:0;flag<2;++flag) {
            const EffectCaptureFact *fact=NULL;
            status=effect_capture_find(read,(EffectCaptureFact){node->node,source,EFFECT_CAPTURE_FLAGS,flag,0},&fact);
            if(status!=XR_XIR_OK) return status;
            if(fact) {
                status=effect_capture_emit(read,(EffectCaptureFact){node->node,destination,EFFECT_CAPTURE_FLAGS,flag,fact->rank+1});
                if(status!=XR_XIR_OK) return status;
            }
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_capture_barrier(EffectCaptureRead *read,const EffectCaptureNode *node,const XrXirInstruction *op) {
    if(!(effect_physical_opcode(op->op)&EFFECT_PHYSICAL_BARRIER)) return XR_XIR_OK;
    bool table=xr_xir_op_uses_operand_table(op->op);uint32_t count=table?op->args[1]:effect_parameter_operands[op->op];
    if(op->op==XR_XIR_CELL_NEW || op->op==XR_XIR_CELL_WRITE || op->op==XR_XIR_CELL_LOCAL_WRITE) {
        XrXirType type=op->type;
        if(op->op!=XR_XIR_CELL_NEW) {
            XrXirStatus status=effect_body_request_type(&read->body,read->final->nodes[node->node].body,op->args[0],&type);
            if(status!=XR_XIR_OK) return status;
        }
        count=xr_xir_cell_payload_operands(read->body.domain.types,type,op->op);
    } else if(op->op==XR_XIR_SLOT_INIT || op->op==XR_XIR_SLOT_STORE) {
        const XrXirDeclarations *declarations=read->body.domain.declarations;
        if(!declarations) return XR_XIR_BAD_STRUCTURE;
        count=xr_xir_slot_payload_operands(declarations->slots,declarations->slot_count,op);
    }
    if(table && (op->args[0]>node->head.operand_count || count>node->head.operand_count-op->args[0])) return XR_XIR_BAD_STRUCTURE;
    if(!table && count>sizeof(op->args)/sizeof(op->args[0])) return XR_XIR_BAD_STRUCTURE;
    for(uint32_t p=0;p<count;++p) {
        if(!xir_compile_work(read->work,3)) return XR_XIR_BUDGET;
        uint32_t value=table?node->head.operands[op->args[0]+p]:op->args[p],kind=0;
        if(value>=node->values) return XR_XIR_BAD_STRUCTURE;
        /* Kind and descriptor come from the same current SSA lookup. No emit
         * or allocator callback separates these two projections. */
        XrXirType type=0;XrXirStatus status=effect_capture_kind(read,node,value,&kind,&type);
        if(status!=XR_XIR_OK) return status;
        if(kind==EFFECT_CAPTURE_CALLABLE && !effect_physical_iterator(read->body.domain.types,type)) {
            status=effect_capture_emit(read,(EffectCaptureFact){node->node,value,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_FIXED,0});
            if(status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_capture_cell(EffectCaptureRead *read,const EffectCaptureNode *node,uint32_t value);
static XrXirStatus effect_capture_parameters(EffectCaptureRead *read,const EffectCaptureNode *node) {
    if(read->fixed_only) return XR_XIR_OK;
    if(read->selected) {
        uint32_t value=read->selected->value;
        bool formal=value<node->head.parameter_count;
        bool initial=value>node->values && value-node->values-1<node->head.parameter_count;
        /* Parameter clauses cannot support an instruction or return vertex.
         * Complete closure still opens and checks every current root seed. */
        if(!formal && !initial) return XR_XIR_OK;
    }
    uint32_t body=read->final->nodes[node->node].body;bool root=read->final->roots[body]==node->node;
    EffectInvocationSeedRead seed={0};XrXirStatus status=XR_XIR_OK;
    if(root) { status=effect_invocation_seed_open(read->work,&read->body,read->final,body,&seed);if(status!=XR_XIR_OK) return status; }
    for(uint32_t p=0;p<node->head.parameter_count;++p) {
        if(read->selected && read->selected->value!=p && read->selected->value!=node->values+1+p) continue;
        uint32_t kind=0;status=effect_capture_kind(read,node,p,&kind,NULL);if(status!=XR_XIR_OK) return status;
        if(kind==EFFECT_CAPTURE_SCALAR) continue;
        uint32_t initial=node->values+1+p;
        if(root) {
            uint32_t role=0;EffectInvocationSeedBits bits={0};
            status=effect_role_parameter_read(read->work,read->final->material,&read->final->role_proof,body,p,&role);
            if(status==XR_XIR_OK) status=effect_invocation_seed_bits(&seed,p,role,&bits);
            if(status!=XR_XIR_OK) return status;
            for(uint32_t b=0;b<bits.origin_count;++b) {
                status=effect_capture_bit(read,node,initial,0,bits.origins[b],0);if(status!=XR_XIR_OK) return status;
            }
            for(uint32_t b=0;b<bits.cell_count;++b) {
                status=effect_capture_bit(read,node,initial,node->basis.fn_words,bits.cells[b],0);if(status!=XR_XIR_OK) return status;
            }
        }
        if(read->selected && read->selected->value>node->values) continue;
        /* A formal's current Cell proof can establish an owned root without
         * the dependency carried by its separate symbolic initial row. */
        if(kind==EFFECT_CAPTURE_CELL) {
            status=effect_capture_cell(read,node,p);if(status!=XR_XIR_OK) return status;
            continue;
        }
        uint32_t first=kind==EFFECT_CAPTURE_CALLABLE?0:node->basis.fn_words;
        uint32_t words=kind==EFFECT_CAPTURE_CALLABLE?node->basis.fn_words:node->basis.cell_words;
        EffectCaptureTransfer edge={.from=node->node,.value=initial,.to=node->node,.destination=p,
            .first=first,.word=first,.words=words};
        status=effect_capture_transfer(read,&edge);if(status!=XR_XIR_OK) return status;
        if(kind==EFFECT_CAPTURE_CALLABLE) {
            /* This is local initial-carrier shape. Every initial bit separately
             * needs a descending actual cause; presence itself grants no origin. */
            bool present=false;const uint64_t *input=read->final->nodes[node->node].input+(uint64_t)p*node->basis.row_words;
            for(uint32_t w=0;w<node->basis.origin_words;++w) {
                if(!xir_compile_work(read->work,2)) return XR_XIR_BUDGET;
                if(input[w]) present=true;
            }
            if(present) {
                status=effect_capture_emit(read,(EffectCaptureFact){node->node,p,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_RAW,0});
                if(status!=XR_XIR_OK) return status;
            }
            status=effect_capture_finish(read,node,p);if(status!=XR_XIR_OK) return status;
        } else if(kind==EFFECT_CAPTURE_CLASS) {
            uint32_t known=node->basis.parameters+2;
            const uint64_t *input=read->final->nodes[node->node].input+(uint64_t)p*node->basis.row_words;
            if(input[node->basis.fn_words+known/64]&(UINT64_C(1)<<(known%64))) {
                status=effect_capture_emit(read,(EffectCaptureFact){node->node,p,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_RAW,0});
                if(status!=XR_XIR_OK) return status;
            }
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_capture_reference(EffectCaptureRead *read,const EffectCaptureNode *node,
    uint32_t instruction,const XrXirInstruction *op) {
    uint32_t body=read->final->nodes[node->node].body,site=UINT32_MAX;
    for(uint32_t s=0;s<read->final->site_count;++s) {
        if(!xir_compile_work(read->work,2)) return XR_XIR_BUDGET;
        if(read->final->sites[s].function==body && read->final->sites[s].instruction==instruction) {
            if(site!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;site=s;
        }
    }
    if(site==UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    bool genuine=false;XrXirStatus status=effect_capture_reference_seed(read,site,&genuine);
    if(status!=XR_XIR_OK) return status;
    uint32_t value=node->head.parameter_count+instruction;
    if(!genuine) {
        const XrXirTypeNode *signature=xr_xir_callable_signature(read->body.domain.types,op->type);
        EffectCaptureFact destination={node->node,value,0,0,0};
        return signature?effect_capture_opaque(read,node,&destination,signature->flags|XR_XIR_CALLABLE_ROOT_UNRESOLVED,true):XR_XIR_BAD_TYPE;
    }
    status=effect_capture_bit(read,node,value,0,site,0);
    if(status==XR_XIR_OK) status=effect_capture_emit(read,(EffectCaptureFact){node->node,value,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_RAW,0});
    if(status!=XR_XIR_OK) return status;
    const EffectInvocationSite *reference=&read->final->sites[site];EffectBodyFunctionHead target={0};
    status=effect_body_request_head(&read->body,reference->target,&target);if(status!=XR_XIR_OK) return status;
    if(op->args[0]>node->head.operand_count || reference->captures>node->head.operand_count-op->args[0]) return XR_XIR_BAD_STRUCTURE;
    for(uint32_t p=0;p<reference->captures;++p) {
        if(!xir_compile_work(read->work,4)) return XR_XIR_BUDGET;
        uint32_t source=node->head.operands[op->args[0]+p],kind=0;
        status=effect_capture_kind(read,node,source,&kind,NULL);if(status!=XR_XIR_OK) return status;
        uint64_t binding=node->basis.origin_words+((uint64_t)reference->binding+p)*
            (node->basis.origin_words+(uint64_t)node->basis.cell_words);
        if(binding>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
        if(effect_physical_origin(read->body.domain.types,target.parameters[p])) {
            if(kind!=EFFECT_CAPTURE_CALLABLE) return XR_XIR_BAD_TYPE;
            EffectCaptureTransfer prefix={.from=node->node,.value=source,.to=node->node,.destination=value,
                .first=0,.word=(uint32_t)binding,.words=node->basis.origin_words};
            status=effect_capture_transfer(read,&prefix);if(status!=XR_XIR_OK) return status;
            EffectCaptureTransfer nested={.from=node->node,.value=source,.to=node->node,.destination=value,
                .first=node->basis.origin_words,.word=node->basis.origin_words,.words=node->basis.fn_words-node->basis.origin_words};
            status=effect_capture_transfer(read,&nested);if(status!=XR_XIR_OK) return status;
        } else if(xr_xir_type_is_cell(read->body.domain.types,target.parameters[p]) ||
            xr_xir_type_is_class(read->body.domain.types,target.parameters[p])) {
            bool object=xr_xir_type_is_class(read->body.domain.types,target.parameters[p]);
            if(kind!=(uint32_t)(object?EFFECT_CAPTURE_CLASS:EFFECT_CAPTURE_CELL)) return XR_XIR_BAD_TYPE;
            if(!object && xr_xir_cell_provenance_role(read->final->cells,reference->target,p)!=XR_XIR_CELL_PROOF_OWNED_CAPTURE)
                return XR_XIR_BAD_STRUCTURE;
            EffectCaptureTransfer cell={.from=node->node,.value=source,.to=node->node,.destination=value,
                .first=node->basis.fn_words,.word=(uint32_t)binding+node->basis.origin_words,.words=node->basis.cell_words};
            status=effect_capture_transfer(read,&cell);if(status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}

#include "xxir_effect_capture_cells.inc.c"

/* A known Iterator factory has actual returned origins, not a Callable
 * advertisement. Only an opaque callee supplies an opaque result alternative. */
static XrXirStatus effect_capture_iterator_call(EffectCaptureRead *read,const EffectCaptureNode *node,
    uint32_t instruction,const XrXirInstruction *operation) {
    XrXirInstruction storage;const XrXirInstruction *op=operation;
    if(op->op==XR_XIR_INVOKE_RESULT) {
        if(op->immediate<0 || (uint64_t)op->immediate>=node->head.instruction_count) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=effect_body_request_instruction(&read->body,read->final->nodes[node->node].body,
            (uint32_t)op->immediate,&storage,&op);if(status!=XR_XIR_OK) return status;
    }
    if(op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) return XR_XIR_OK;
    if(op->immediate<0 || (uint64_t)op->immediate>=node->values) return XR_XIR_BAD_STRUCTURE;
    uint32_t opaque=read->final->site_count+node->basis.parameters;
    const EffectCaptureFact *source=NULL;
    XrXirStatus status=effect_capture_find(read,
        (EffectCaptureFact){node->node,(uint32_t)op->immediate,opaque/64,opaque%64,0},&source);
    if(status!=XR_XIR_OK || !source) return status;
    EffectCaptureFact destination={node->node,node->head.parameter_count+instruction,0,0,source->rank+1};
    return effect_capture_opaque(read,node,&destination,XR_XIR_CALLABLE_ROOT_UNRESOLVED,true);
}

static XrXirStatus effect_capture_instruction(EffectCaptureRead *read,const EffectCaptureNode *node,
    uint32_t instruction,const XrXirInstruction *op) {
    if(!op || instruction>=node->head.instruction_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t value=node->head.parameter_count+instruction;
    /* This operation was decoded from the current canonical body. Its type
     * is the destination's exact SSA type, so no second directory walk is needed. */
    uint32_t kind=effect_capture_type_kind(read->body.domain.types,op->type);
    XrXirStatus status=effect_capture_barrier(read,node,op);
    if(status!=XR_XIR_OK) return status;
    if(!read->fixed_only && kind==EFFECT_CAPTURE_CELL) {
        status=effect_capture_cell(read,node,value);if(status!=XR_XIR_OK) return status;
    }
    bool alias=op->op==XR_XIR_COPY || op->op==XR_XIR_SCALAR_COPY || op->op==XR_XIR_OWNED_RETAIN ||
        op->op==XR_XIR_LOCAL_NEW || op->op==XR_XIR_SCALAR_LOCAL_NEW || op->op==XR_XIR_OWNED_LOCAL_NEW ||
        op->op==XR_XIR_LOCAL_READ || op->op==XR_XIR_SCALAR_LOCAL_READ || op->op==XR_XIR_OWNED_LOCAL_READ;
    bool write=op->op==XR_XIR_LOCAL_WRITE || op->op==XR_XIR_SCALAR_LOCAL_WRITE || op->op==XR_XIR_OWNED_LOCAL_WRITE;
    if(alias) {
        if(kind==EFFECT_CAPTURE_CALLABLE || kind==EFFECT_CAPTURE_CLASS) {
            status=effect_capture_alias(read,node,op->args[0],value);if(status!=XR_XIR_OK) return status;
        }
    } else if(write) {
        uint32_t destination=op->args[0],target_kind=0;
        status=effect_capture_kind(read,node,destination,&target_kind,NULL);if(status!=XR_XIR_OK) return status;
        if(target_kind==EFFECT_CAPTURE_CALLABLE || target_kind==EFFECT_CAPTURE_CLASS) {
            status=effect_capture_alias(read,node,op->args[1],destination);if(status!=XR_XIR_OK) return status;
        }
    } else if(op->op==XR_XIR_PHI && (kind==EFFECT_CAPTURE_CALLABLE || kind==EFFECT_CAPTURE_CLASS)) {
        if(!op->args[1] || op->args[1]%2 || op->args[0]>node->head.operand_count ||
            op->args[1]>node->head.operand_count-op->args[0]) return XR_XIR_BAD_STRUCTURE;
        for(uint32_t p=1;p<op->args[1];p+=2) {
            status=effect_capture_alias(read,node,node->head.operands[op->args[0]+p],value);if(status!=XR_XIR_OK) return status;
        }
    } else if(!read->fixed_only && (op->op==XR_XIR_FUNCTION_REF || op->op==XR_XIR_GENERATOR_NEW)) {
        status=effect_capture_reference(read,node,instruction,op);if(status!=XR_XIR_OK) return status;
    } else if(!read->fixed_only && kind==EFFECT_CAPTURE_CALLABLE) {
        if(effect_physical_iterator(read->body.domain.types,op->type) &&
            (op->op==XR_XIR_CALL || op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_RESULT))
            status=effect_capture_iterator_call(read,node,instruction,op);
        else {
            const XrXirTypeNode *signature=xr_xir_callable_signature(read->body.domain.types,op->type);
            EffectCaptureFact destination={node->node,value,0,0,0};
            status=effect_capture_opaque(read,node,&destination,signature?signature->flags:XR_XIR_CALLABLE_ROOT_UNRESOLVED,true);
        }
        if(status!=XR_XIR_OK) return status;
    } else if(!read->fixed_only && kind==EFFECT_CAPTURE_CLASS) {
        status=effect_capture_class_intrinsic(read,node,instruction,op);if(status!=XR_XIR_OK) return status;
    }
    if(op->op==XR_XIR_CELL_READ && kind==EFFECT_CAPTURE_CALLABLE) {
        status=effect_capture_callable_cell(read,node,instruction,op);if(status!=XR_XIR_OK) return status;
    }
    if(!read->fixed_only && op->op==XR_XIR_RETURN) {
        uint32_t returned=0;status=effect_capture_kind(read,node,node->values,&returned,NULL);if(status!=XR_XIR_OK) return status;
        if(returned==EFFECT_CAPTURE_CALLABLE || returned==EFFECT_CAPTURE_CLASS) {
            status=effect_capture_alias(read,node,op->args[0],node->values);if(status!=XR_XIR_OK) return status;
        }
    }
    if(!read->fixed_only && kind==EFFECT_CAPTURE_CLASS) {
        const EffectCaptureFact *present=NULL;uint32_t bit=node->basis.parameters+2;
        status=effect_capture_find(read,(EffectCaptureFact){node->node,value,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_RAW,0},&present);
        if(status!=XR_XIR_OK) return status;
        if(read->fallback && !present) {
            status=effect_capture_bit(read,node,value,node->basis.fn_words,bit,0);
            if(status==XR_XIR_OK) status=effect_capture_bit(read,node,value,node->basis.fn_words,bit-1,0);
            if(status!=XR_XIR_OK) return status;
        }
    }
    return kind==EFFECT_CAPTURE_CALLABLE?effect_capture_finish(read,node,value):XR_XIR_OK;
}
