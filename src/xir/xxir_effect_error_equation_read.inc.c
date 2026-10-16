/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_error_equation_read.inc.c - Read-only local Error equations
 *
 * KEY CONCEPT:
 *   Current Input defines every transfer. Proposed block entries and call
 *   summaries remain operands whose closure and rootedness require proof.
 */
typedef struct EffectErrorEquationRead {
    EffectBodyRequest body;
    EffectBodyFunctionHead head;
    const XrXirCompileContext *work;
    const EffectErrorAtom *atoms;
    uint32_t context,function,atom_count,words,rank_limit;
    const void *owner;
    XrXirStatus (*entry)(const struct EffectErrorEquationRead *,uint32_t,uint32_t,uint32_t,uint64_t *);
    XrXirStatus (*call)(const struct EffectErrorEquationRead *,uint32_t,uint32_t,uint64_t *);
} EffectErrorEquationRead;

static inline XrXirStatus error_equation_operation(const EffectErrorEquationRead *read,
    uint32_t i,XrXirInstruction *storage) {
    const XrXirInstruction *actual=NULL;
    XrXirStatus status=effect_body_request_instruction(&read->body,read->function,i,storage,&actual);
    if (status==XR_XIR_OK && !actual) return XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK && actual!=storage) {
        if (!xir_compile_work(read->work,sizeof(*storage))) return XR_XIR_BUDGET;
        *storage=*actual;
    }
    return status;
}
static inline XrXirStatus error_equation_type(const EffectErrorEquationRead *read,
    uint32_t value,XrXirType *output) {
    return effect_body_request_type(&read->body,read->function,value,output);
}
static inline XrXirStatus error_equation_membership(const EffectErrorEquationRead *read,
    uint32_t value,bool *active,XrXirType *type) {
    XrXirCompileContext current=*read->work;
    ErrorFlow view={.body=read->body,.function=&read->head,.function_index=read->function,.remaining=&current};
    bool copy=false;uint32_t source=UINT32_MAX;
    return error_row_active(&view,value,active,type,&copy,&source);
}
static inline bool error_equation_invalidates_cells(XrXirOp op) {
    return op==XR_XIR_CALL_DEFAULT || op==XR_XIR_CALL || op==XR_XIR_CALL_INDIRECT ||
        op==XR_XIR_CALL_REQUIREMENT || op==XR_XIR_INVOKE_DEFAULT || op==XR_XIR_INVOKE ||
        op==XR_XIR_INVOKE_INDIRECT || op==XR_XIR_SUSPEND || op==XR_XIR_TIMER_AFTER_MS ||
        op==XR_XIR_OUTPUT || op==XR_XIR_WRITE_STREAM || op==XR_XIR_PRINT || op==XR_XIR_GO ||
        op==XR_XIR_TASK_AWAIT || op==XR_XIR_CLEANUP_LEAVE || op==XR_XIR_CLEANUP_ERROR ||
        op==XR_XIR_CELL_LOCAL_WRITE;
}
static inline XrXirStatus error_equation_root(const EffectErrorEquationRead *read,
    uint32_t value,uint32_t *output) {
    uint64_t values=(uint64_t)read->head.parameter_count+read->head.instruction_count;
    for (uint64_t walked=0;walked<=values;++walked) {
        if (!xir_compile_work(read->work,2)) return XR_XIR_BUDGET;
        if (value>=values) return XR_XIR_BAD_STRUCTURE;
        if (value<read->head.parameter_count) { *output=value;return XR_XIR_OK; }
        XrXirInstruction op;
        XrXirStatus status=error_equation_operation(read,value-read->head.parameter_count,&op);
        if (status!=XR_XIR_OK) return status;
        if (!error_copy_op(op.op)) { *output=value;return XR_XIR_OK; }
        value=op.args[0];
    }
    return XR_XIR_BAD_STRUCTURE;
}
static inline XrXirStatus error_equation_type_word(const EffectErrorEquationRead *read,
    XrXirType type,uint32_t word,uint64_t *output) {
    if (word>=read->words) return XR_XIR_BAD_STRUCTURE;
    const XrXirTypes *types=read->body.domain.types;
    if (xr_xir_type_is_cell(types,type)) type=xr_xir_cell_element(types,type);
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    uint64_t result=0;
    if (type==XR_XIR_ERROR || (node && node->kind==XR_XIR_TYPE_TASK)) {
        if (!word) result=UINT64_C(2);
    } else for (uint32_t a=0;a<read->atom_count;++a) {
        if (!xir_compile_work(read->work,2)) return XR_XIR_BUDGET;
        if (read->atoms[a].type==type && (a+2)/64==word)
            result|=UINT64_C(1)<<((a+2)%64);
    }
    *output=result;return XR_XIR_OK;
}

/* A query follows one real assignment source at a time. It stops at a current
 * entry, an actual type/enum seed, or a proposed call summary. No SSA array,
 * interpreter row, recursion stack or saved local transfer survives the call. */
static inline XrXirStatus error_equation_value(const EffectErrorEquationRead *read,
    uint32_t block,uint32_t cursor,uint32_t value,uint32_t word,uint64_t *output) {
    if (!read || !output || !read->entry || !read->call || !read->owner ||
        !xir_compile_context_valid(read->work) || word>=read->words ||
        block>=read->head.block_count || !read->head.blocks) return XR_XIR_BAD_STRUCTURE;
    const XrXirBlock *bounds=&read->head.blocks[block];
    if (bounds->first>UINT32_MAX-bounds->count || cursor<bounds->first ||
        cursor-bounds->first>bounds->count) return XR_XIR_BAD_STRUCTURE;
    uint32_t first=bounds->first;
    for (;;) {
        bool active=false;XrXirType type=XR_XIR_UNIT;
        XrXirStatus status=error_equation_membership(read,value,&active,&type);
        if (status!=XR_XIR_OK) return status;
        if (!active) { *output=0;return XR_XIR_OK; }
        bool cell=xr_xir_type_is_cell(read->body.domain.types,type),follow=false;
        while (cursor>first) {
            uint32_t i=--cursor;XrXirInstruction op;
            status=error_equation_operation(read,i,&op);
            if (status!=XR_XIR_OK) return status;
            if (!xir_compile_work(read->work,8)) return XR_XIR_BUDGET;
            if (cell && error_equation_invalidates_cells(op.op))
                return error_equation_type_word(read,type,word,output);
            if (cell && op.op==XR_XIR_CELL_WRITE) {
                uint32_t left=UINT32_MAX,right=UINT32_MAX;
                status=error_equation_root(read,value,&left);
                if (status==XR_XIR_OK) status=error_equation_root(read,op.args[0],&right);
                if (status!=XR_XIR_OK) return status;
                if (left!=right) return error_equation_type_word(read,type,word,output);
                XrXirType target=XR_XIR_UNIT;
                status=error_equation_type(read,op.args[0],&target);
                if (status!=XR_XIR_OK) return status;
                if (xr_xir_cell_payload_operands(read->body.domain.types,target,op.op)==1) {
                    *output=0;return XR_XIR_OK;
                }
                value=op.args[1];follow=true;break;
            }
            if ((op.op==XR_XIR_LOCAL_WRITE || op.op==XR_XIR_OWNED_LOCAL_WRITE ||
                op.op==XR_XIR_SCALAR_LOCAL_WRITE) && op.args[0]==value) {
                value=op.args[1];follow=true;break;
            }
            if (value!=(uint64_t)read->head.parameter_count+i || op.op==XR_XIR_PHI) continue;
            if (error_copy_op(op.op) || op.op==XR_XIR_LOCAL_NEW || op.op==XR_XIR_OWNED_LOCAL_NEW ||
                op.op==XR_XIR_SCALAR_LOCAL_NEW || op.op==XR_XIR_LOCAL_READ ||
                op.op==XR_XIR_OWNED_LOCAL_READ || op.op==XR_XIR_SCALAR_LOCAL_READ ||
                op.op==XR_XIR_CELL_NEW || (op.op==XR_XIR_CELL_READ && !cell)) {
                if (op.op==XR_XIR_CELL_NEW &&
                    !xr_xir_cell_payload_operands(read->body.domain.types,op.type,op.op)) {
                    *output=0;return XR_XIR_OK;
                }
                value=op.args[0];follow=true;break;
            }
            if (op.op==XR_XIR_ENUM_NEW) {
                uint64_t result=0;
                for (uint32_t a=0;a<read->atom_count;++a) {
                    if (!xir_compile_work(read->work,3)) return XR_XIR_BUDGET;
                    if (read->atoms[a].type==op.type && read->atoms[a].variant==(uint32_t)op.immediate &&
                        (a+2)/64==word) result|=UINT64_C(1)<<((a+2)%64);
                }
                *output=result;return XR_XIR_OK;
            }
            if (op.op==XR_XIR_GO) return read->call(read,i,word,output);
            if (op.op==XR_XIR_INVOKE_ERROR) {
                if (op.immediate<0 || (uint64_t)op.immediate>=read->head.instruction_count)
                    return XR_XIR_BAD_STRUCTURE;
                uint32_t call=(uint32_t)op.immediate;XrXirInstruction producer;
                status=error_equation_operation(read,call,&producer);
                if (status!=XR_XIR_OK) return status;
                if (producer.op==XR_XIR_TASK_AWAIT) { value=producer.args[0];follow=true;break; }
                return read->call(read,call,word,output);
            }
            return error_equation_type_word(read,type,word,output);
        }
        if (!follow) return read->entry(read,block,value,word,output);
    }
}

typedef struct EffectErrorFilterRead {
    uint32_t value;XrXirType type;
    int64_t variant;
    bool recognized,yes,possible;
} EffectErrorFilterRead;
static inline XrXirStatus error_equation_definition(const EffectErrorEquationRead *read,
    uint32_t value,XrXirInstruction *output,bool *present) {
    if (value>=(uint64_t)read->head.parameter_count+read->head.instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    *present=value>=read->head.parameter_count;
    return *present?error_equation_operation(read,value-read->head.parameter_count,output):XR_XIR_OK;
}
static inline XrXirStatus error_equation_filter_read(const EffectErrorEquationRead *read,
    const XrXirInstruction *branch,bool yes,EffectErrorFilterRead *output) {
    *output=(EffectErrorFilterRead){.variant=-1,.yes=yes,.possible=true};
    XrXirInstruction test;bool present=false;
    XrXirStatus status=error_equation_definition(read,branch->args[0],&test,&present);
    if (status!=XR_XIR_OK || !present) return status;
    if (test.op==XR_XIR_ERROR_IS) {
        output->value=test.args[0];output->type=(XrXirType)test.immediate;
    } else if (test.op==XR_XIR_EQ_INT || test.op==XR_XIR_NE_INT) {
        XrXirInstruction tag={0},constant={0};bool left=false,right=false;
        status=error_equation_definition(read,test.args[0],&tag,&left);
        if (status==XR_XIR_OK) status=error_equation_definition(read,test.args[1],&constant,&right);
        if (status!=XR_XIR_OK || !left || !right) return status;
        if (tag.op==XR_XIR_CONST_INT) { XrXirInstruction swap=tag;tag=constant;constant=swap; }
        if (tag.op!=XR_XIR_ENUM_TAG || constant.op!=XR_XIR_CONST_INT) return XR_XIR_OK;
        if (constant.immediate<0) { output->possible=test.op==XR_XIR_EQ_INT?!yes:yes;return XR_XIR_OK; }
        output->value=tag.args[0];output->variant=constant.immediate;
        status=error_equation_type(read,output->value,&output->type);
        if (status!=XR_XIR_OK) return status;
        if (test.op==XR_XIR_NE_INT) output->yes=!yes;
    } else return XR_XIR_OK;
    output->recognized=true;return XR_XIR_OK;
}
static inline XrXirStatus error_equation_filter_mask(const EffectErrorEquationRead *read,
    const EffectErrorFilterRead *filter,uint32_t value,uint32_t word,uint64_t *output) {
    *output=UINT64_MAX;
    if (!filter->recognized) return XR_XIR_OK;
    uint32_t left=UINT32_MAX,right=UINT32_MAX;
    XrXirStatus status=error_equation_root(read,value,&left);
    if (status==XR_XIR_OK) status=error_equation_root(read,filter->value,&right);
    if (status!=XR_XIR_OK || left!=right) return status;
    for (uint32_t a=0;a<read->atom_count;++a) {
        if (!xir_compile_work(read->work,4)) return XR_XIR_BUDGET;
        const EffectErrorAtom *atom=&read->atoms[a];
        if ((a+2)/64!=word || atom->variant==XR_XIR_ERROR_SYMBOLIC_VARIANT) continue;
        bool match=atom->type==filter->type && (filter->variant<0 || atom->variant==(uint64_t)filter->variant);
        if (match!=filter->yes) *output&=~(UINT64_C(1)<<((a+2)%64));
    }
    return XR_XIR_OK;
}

typedef enum EffectErrorEdgeKind {
    EFFECT_ERROR_EDGE_NORMAL,EFFECT_ERROR_EDGE_ALTERNATE,EFFECT_ERROR_EDGE_PANIC
} EffectErrorEdgeKind;
typedef struct EffectErrorEdgeRead {
    uint32_t from,to,cursor;
    EffectErrorEdgeKind kind;
} EffectErrorEdgeRead;

/* Coordinates name an actual Input edge, never an arbitrary state transfer.
 * Source reachability and positive support remain separate proof obligations. */
static inline XrXirStatus error_equation_edge_classify(const EffectErrorEquationRead *read,
    const EffectErrorEdgeRead *edge,XrXirInstruction *terminal,EffectErrorFilterRead *filter,bool *exceptional) {
    if(edge->from>=read->head.block_count || edge->to>=read->head.block_count ||
        edge->kind<EFFECT_ERROR_EDGE_NORMAL || edge->kind>EFFECT_ERROR_EDGE_PANIC)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirBlock *from=&read->head.blocks[edge->from];
    if(from->first>UINT32_MAX-from->count || !from->count) return XR_XIR_BAD_STRUCTURE;
    uint32_t end=from->first+from->count;
    *filter=(EffectErrorFilterRead){.variant=-1,.possible=true};*exceptional=false;
    if(edge->kind==EFFECT_ERROR_EDGE_PANIC)
        return from->panic && from->panic==edge->to && edge->cursor>=from->first && edge->cursor<end?
            XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    if(edge->kind>EFFECT_ERROR_EDGE_ALTERNATE || edge->cursor!=end) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=error_equation_operation(read,end-1,terminal);
    if(status!=XR_XIR_OK) return status;
    bool alternate=edge->kind==EFFECT_ERROR_EDGE_ALTERNATE;
    if(terminal->op==XR_XIR_JUMP || terminal->op==XR_XIR_CLEANUP_REGISTER ||
        terminal->op==XR_XIR_CLEANUP_LEAVE || terminal->op==XR_XIR_CLEANUP_ERROR)
        return !alternate && terminal->targets[0]==edge->to?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    if(terminal->op==XR_XIR_BRANCH) {
        if(terminal->targets[alternate?1:0]!=edge->to) return XR_XIR_BAD_STRUCTURE;
        return error_equation_filter_read(read,terminal,!alternate,filter);
    }
    if(terminal->op==XR_XIR_INVOKE || terminal->op==XR_XIR_INVOKE_DEFAULT ||
        terminal->op==XR_XIR_INVOKE_INDIRECT || terminal->op==XR_XIR_TASK_AWAIT) {
        *exceptional=alternate;
        return terminal->targets[alternate?1:0]==edge->to?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_BAD_STRUCTURE;
}

static inline XrXirStatus error_equation_edge_source(const EffectErrorEquationRead *read,
    const EffectErrorEdgeRead *edge,const EffectErrorFilterRead *filter,
    uint32_t value,uint32_t word,uint64_t *output) {
    XrXirType type=XR_XIR_UNIT;XrXirStatus status=error_equation_type(read,value,&type);
    if(status!=XR_XIR_OK) return status;
    const XrXirBlock *from=&read->head.blocks[edge->from],*to=&read->head.blocks[edge->to];
    uint64_t row=0,mask=UINT64_MAX;
    if(from->panic==edge->to && from->frontier!=to->frontier &&
        xr_xir_type_is_cell(read->body.domain.types,type))
        status=error_equation_type_word(read,type,word,&row);
    else status=error_equation_value(read,edge->from,edge->cursor,value,word,&row);
    if(status==XR_XIR_OK) status=error_equation_filter_mask(read,filter,value,word,&mask);
    if(status==XR_XIR_OK) *output=row&mask;
    return status;
}

static inline XrXirStatus error_equation_edge_possible(const EffectErrorEquationRead *read,
    const EffectErrorEdgeRead *edge,EffectErrorFilterRead *filter,bool *possible) {
    XrXirInstruction terminal={0};bool exceptional=false;
    XrXirStatus status=error_equation_edge_classify(read,edge,&terminal,filter,&exceptional);
    if(status!=XR_XIR_OK) return status;
    *possible=filter->possible;
    if(!*possible || (!exceptional && !filter->recognized)) return XR_XIR_OK;
    *possible=false;
    for(uint32_t word=0;word<read->words;++word) {
        uint64_t row=0;
        if(!xir_compile_work(read->work,1)) return XR_XIR_BUDGET;
        if(exceptional) {
            if(terminal.op==XR_XIR_TASK_AWAIT)
                status=error_equation_value(read,edge->from,edge->cursor,terminal.args[0],word,&row);
            else status=read->call(read,edge->cursor-1,word,&row);
        } else status=error_equation_edge_source(read,edge,filter,filter->value,word,&row);
        if(status!=XR_XIR_OK) return status;
        *possible|=row!=0;
    }
    return XR_XIR_OK;
}

/* Phi inputs read the same filtered predecessor snapshot. No earlier Phi
 * assignment can replace a later one's source, including self and back edges. */
static inline XrXirStatus error_equation_edge_word(const EffectErrorEquationRead *read,
    const EffectErrorEdgeRead *edge,uint32_t value,uint32_t word,uint64_t *output,bool *possible) {
    EffectErrorFilterRead filter;
    XrXirStatus status=error_equation_edge_possible(read,edge,&filter,possible);
    if(status!=XR_XIR_OK) return status;
    if(!*possible) { *output=0;return XR_XIR_OK; }
    uint32_t source=value;
    const XrXirBlock *to=&read->head.blocks[edge->to];
    if(value>=read->head.parameter_count) {
        uint32_t instruction=value-read->head.parameter_count;
        if(instruction>=to->first && instruction-to->first<to->count) {
            XrXirInstruction phi;
            status=error_equation_operation(read,instruction,&phi);
            if(status!=XR_XIR_OK) return status;
            if(phi.op==XR_XIR_PHI) {
                for(uint32_t i=to->first;i<instruction;++i) {
                    XrXirInstruction earlier;status=error_equation_operation(read,i,&earlier);
                    if(status!=XR_XIR_OK) return status;
                    if(earlier.op!=XR_XIR_PHI) return XR_XIR_BAD_STRUCTURE;
                }
                if(phi.args[1]%2 || phi.args[0]>read->head.operand_count ||
                    phi.args[1]>read->head.operand_count-phi.args[0]) return XR_XIR_BAD_STRUCTURE;
                uint32_t matches=0;
                for(uint32_t p=0;p<phi.args[1];p+=2) {
                    if(!xir_compile_work(read->work,2)) return XR_XIR_BUDGET;
                    if(read->head.operands[phi.args[0]+p]==edge->from) {
                        source=read->head.operands[phi.args[0]+p+1];++matches;
                    }
                }
                if(matches!=1) return XR_XIR_BAD_STRUCTURE;
            }
        }
    }
    bool active=false;XrXirType type=XR_XIR_UNIT;
    status=error_equation_membership(read,value,&active,&type);
    if(status!=XR_XIR_OK) return status;
    if(!active) { *output=0;return XR_XIR_OK; }
    return error_equation_edge_source(read,edge,&filter,source,word,output);
}
