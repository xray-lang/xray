/*
 * xray - Lightweight typed scripting with native concurrency
 * https://xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_optional_error.inc.c - Remove checked error joins that only produce None
 */

static bool lower_optional_error_join(const Lower *c,uint32_t block) {
    const LowerOptionalErrorBlock *plan=c->optional_errors ? c->optional_errors[c->function] : NULL;
    return plan && plan[block].join;
}

static uint32_t lower_optional_error_predecessor(const Lower *c,uint32_t block) {
    const LowerOptionalErrorBlock *plan=c->optional_errors ? c->optional_errors[c->function] : NULL;
    return plan ? plan[block].predecessor : 0;
}

static void lower_optional_uses(const Lower *c,const XrXirFunction *f,uint8_t *uses) {
    static const uint8_t arity[] = {0,
#define XR_XIR_OP(name, stages, rule, operands, edges, terminal) operands,
#include "../xir/xxir_ops.def"
#undef XR_XIR_OP
    };
    for (uint32_t i=0; i<f->instruction_count; ++i) {
        const XrXirInstruction *op=&f->instructions[i];
        if (xr_xir_op_uses_owner_slots(op->op)) continue;
        bool table=xr_xir_op_uses_operand_table(op->op);
        const uint32_t *args=op->args;
        uint32_t count=arity[op->op];
        if (table) {
            count=op->args[1];
            args=count ? f->operands+op->args[0] : NULL;
        } else if (op->op==XR_XIR_RETURN) {
            count=f->result!=XR_XIR_UNIT &&
                !xr_xir_function_is_generator(c->source,c->function);
        } else if (op->op==XR_XIR_GENERATOR_YIELD) {
            count=xr_xir_iterator_element(c->source->types,f->result)!=XR_XIR_UNIT;
        } else if (op->op==XR_XIR_NULLABLE_SOME && xr_xir_nullable_is_unit(c->source->types,op->type)) {
            count=0;
        } else if (op->op==XR_XIR_SLOT_INIT || op->op==XR_XIR_SLOT_STORE) {
            const XrXirDeclarations *d=c->source->declarations;
            count=xr_xir_slot_payload_operands(d->slots,d->slot_count,op);
        } else if (op->op==XR_XIR_CELL_NEW || op->op==XR_XIR_CELL_NEW_TAKE ||
                   op->op==XR_XIR_CELL_READ || op->op==XR_XIR_CELL_TAKE ||
                   op->op==XR_XIR_CELL_WRITE || op->op==XR_XIR_CELL_WRITE_TAKE ||
                   op->op==XR_XIR_CELL_LOCAL_WRITE) {
            XrXirType cell=op->op==XR_XIR_CELL_NEW || op->op==XR_XIR_CELL_NEW_TAKE ?
                op->type : xr_xir_operand_type(f,args[0]);
            count=xr_xir_cell_payload_operands(c->source->types,cell,op->op);
        }
        uint32_t first=op->op==XR_XIR_PHI ? 1u : 0u;
        for (uint32_t a=first; a<count; a+=first+1) {
            uint32_t value=args[a];
            if (value>=f->parameter_count && value-f->parameter_count<f->instruction_count) {
                uint8_t *use=&uses[value-f->parameter_count];
                if (*use<2) ++*use;
            }
        }
    }
}

/* The sealed Checked graph verifies the predecessor set of every PHI and
 * gives each INVOKE landing block exactly one predecessor. */
static const XrXirInstruction *lower_forwarded_error_call(const XrXirFunction *f,
    uint32_t block,uint32_t value,uint32_t join) {
    const XrXirBlock *landing=&f->blocks[block];
    const XrXirInstruction *marker=&f->instructions[landing->first];
    if (landing->count!=2 || landing->panic || landing->frontier ||
        marker->op!=XR_XIR_INVOKE_ERROR || marker->type!=XR_XIR_ERROR ||
        marker->immediate<0 || (uint64_t)marker->immediate>=f->instruction_count ||
        value!=f->parameter_count+landing->first ||
        marker[1].op!=XR_XIR_JUMP || marker[1].targets[0]!=join) return NULL;
    const XrXirInstruction *call=&f->instructions[marker->immediate];
    return call->op==XR_XIR_INVOKE && call->targets[1]==block ? call : NULL;
}

static bool lower_optional_result_join(const XrXirFunction *f,uint32_t block,
    const XrXirInstruction *none) {
    const XrXirInstruction *phi=&f->instructions[f->blocks[none[1].targets[0]].first];
    if (phi->op!=XR_XIR_PHI || phi->type!=none->type || phi->args[1]!=4) return false;
    const uint32_t *values=f->operands+phi->args[0];
    uint32_t value=f->parameter_count+(uint32_t)(none-f->instructions);
    return (values[0]==block && values[1]==value) || (values[2]==block && values[3]==value);
}

static bool lower_optional_error_shape(const Lower *c,const XrXirFunction *f,
    const uint8_t *uses,uint32_t block) {
    const XrXirBlock *join=&f->blocks[block];
    const XrXirInstruction *phi=&f->instructions[join->first];
    if (join->count!=3 || join->panic || join->frontier || uses[join->first] ||
        phi[1].op!=XR_XIR_NULLABLE_NONE || !xr_xir_type_is_nullable(c->source->types,phi[1].type) ||
        phi[2].op!=XR_XIR_JUMP ||
        !lower_optional_result_join(f,block,phi+1))
        return false;
    for (uint32_t a=0; a<phi->args[1]; a+=2) {
        const uint32_t *input=f->operands+phi->args[0]+a;
        if (!lower_forwarded_error_call(f,input[0],input[1],block) ||
            uses[f->blocks[input[0]].first]!=1) return false;
    }
    return phi->args[1]!=0;
}

static bool lower_optional_errors(Lower *c,const XrXirFunction *f) {
    if (c->optional_errors && c->optional_errors[c->function]) return true;
    uint8_t *uses=NULL;
    LowerOptionalErrorBlock *plan=NULL;
    for (uint32_t b=0; b<f->block_count; ++b) {
        const XrXirInstruction *head=&f->instructions[f->blocks[b].first];
        if (head->op!=XR_XIR_PHI || head->type!=XR_XIR_ERROR) continue;
        if (!uses) {
            uses=lower_alloc(c,f->instruction_count,1);
            if (!uses) return false;
            lower_optional_uses(c,f,uses);
        }
        if (!lower_optional_error_shape(c,f,uses,b)) continue;
        if (!plan) {
            if (!c->optional_errors)
                c->optional_errors=lower_alloc(c,c->source->function_count,sizeof(*c->optional_errors));
            if (c->optional_errors) plan=lower_alloc(c,f->block_count,sizeof(*plan));
            if (!plan) { xr_free(uses); return false; }
            c->optional_errors[c->function]=plan;
        }
        plan[b].join=true;
    }
    xr_free(uses);
    if (!plan) return true;
    for (uint32_t b=0; b<f->block_count; ++b) {
        const XrXirBlock *block=&f->blocks[b];
        const XrXirInstruction *term=&f->instructions[block->first+block->count-1];
        if (term->op!=XR_XIR_INVOKE) continue;
        uint32_t failure=term->targets[1];
        const XrXirBlock *landing=&f->blocks[failure];
        const XrXirInstruction *exit=&f->instructions[landing->first+landing->count-1];
        if (exit->op==XR_XIR_JUMP && plan[exit->targets[0]].join)
            plan[failure].predecessor=b+1;
    }
    return true;
}
