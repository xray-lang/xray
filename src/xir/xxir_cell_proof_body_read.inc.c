/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_cell_proof_body_read.inc.c - One admitted body request for Cell proofs
 *
 * Raw and owned adapters feed the same finite provider. A callback failure
 * retains the first status and cannot publish an output or partial proof.
 */
static XrXirStatus cell_proof_raw_head(const XrXirCompileContext *context,const void *owner,
    uint32_t f,XrXirCellProofBodyHead *output) {
    const XrXirModule *module=owner;
    if (!module || !output || f>=module->function_count || !module->functions) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=&module->functions[f];
    if (!xir_compile_work(context,8+sizeof(*output))) return XR_XIR_BUDGET;
    *output=(XrXirCellProofBodyHead){function->parameters,function->parameter_count,function->result,
        function->instruction_count,function->operands,function->operand_count};return XR_XIR_OK;
}
static XrXirStatus cell_proof_raw_instruction(const XrXirCompileContext *context,const void *owner,
    uint32_t f,uint32_t i,XrXirInstruction *storage,const XrXirInstruction **output) {
    const XrXirModule *module=owner;
    if (!module || !storage || !output || f>=module->function_count || !module->functions ||
        i>=module->functions[f].instruction_count || !module->functions[f].instructions) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,3+sizeof(*output))) return XR_XIR_BUDGET;
    *output=&module->functions[f].instructions[i];return XR_XIR_OK;
}
static XrXirStatus cell_proof_raw_type(const XrXirCompileContext *context,const void *owner,
    uint32_t f,uint32_t value,XrXirType *output) {
    const XrXirModule *module=owner;
    if (!module || !output || f>=module->function_count || !module->functions) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=&module->functions[f];
    if (value>=(uint64_t)function->parameter_count+function->instruction_count ||
        (value<function->parameter_count ? !function->parameters : !function->instructions)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,2+sizeof(*output))) return XR_XIR_BUDGET;
    *output=xr_xir_operand_type(function,value);return XR_XIR_OK;
}
static XrXirStatus cell_proof_raw_shape(const XrXirCompileContext *context,const void *owner,
    uint32_t f,uint32_t i,XrXirCellProofInstructionShape *output) {
    const XrXirModule *module=owner;
    if (!module || !output || f>=module->function_count || !module->functions ||
        i>=module->functions[f].instruction_count || !module->functions[f].instructions)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *operation=&module->functions[f].instructions[i];
    if (!xir_compile_work(context,3+sizeof(*output))) return XR_XIR_BUDGET;
    *output=(XrXirCellProofInstructionShape){operation->op,operation->type};return XR_XIR_OK;
}
static XrXirCellProofBodyRead cell_proof_raw_body(const XrXirCompileContext *context,const XrXirModule *module) {
    return (XrXirCellProofBodyRead){xir_metadata_domain_from_module(module),context->resources,module,
        cell_proof_raw_head,cell_proof_raw_instruction,cell_proof_raw_type,cell_proof_raw_shape,NULL,NULL};
}
static bool cell_proof_body_valid(const XrXirCompileContext *context,const XrXirCellProofBodyRead *body) {
    return xir_compile_context_valid(context) && body && body->resources==context->resources && body->owner &&
        body->head && body->instruction && body->type && body->shape &&
        (!body->domain.types || !body->domain.types->count || body->domain.types->nodes);
}
static bool cell_proof_head(CellProofBuild *build,uint32_t f,XrXirCellProofBodyHead *output) {
    if (build->status!=XR_XIR_OK) return false;
    if (f>=build->body->domain.function_count) return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
    XrXirCellProofBodyHead head={0};
    XrXirStatus status=build->body->head(build->context,build->body->owner,f,&head);
    if (status!=XR_XIR_OK) return cell_proof_error(build,f,UINT32_MAX,status);
    if (!cell_proof_work(build,6)) return false;
    if ((head.parameter_count && !head.parameters) || (head.operand_count && !head.operands) ||
        (uint64_t)head.parameter_count+head.instruction_count>UINT32_MAX)
        return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
    *output=head;return true;
}
static const XrXirInstruction *cell_proof_instruction(CellProofBuild *build,uint32_t f,uint32_t i,
    XrXirInstruction *storage) {
    if (build->status!=XR_XIR_OK) return NULL;
    if (!cell_proof_work(build,sizeof(const XrXirInstruction *))) return NULL;
    const XrXirInstruction *operation=NULL;
    XrXirStatus status=build->body->instruction(build->context,build->body->owner,f,i,storage,&operation);
    if (status!=XR_XIR_OK) {cell_proof_error(build,f,i,status);return NULL;}
    if (!operation || operation->op<=XR_XIR_INVALID || operation->op>=XR_XIR_OP_COUNT) {
        cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);return NULL;
    }
    return operation;
}

/* One owned walker receives one canonical run at a time. Raw and custom
 * callbacks retain scalar requests to the same semantic row receivers. */
static bool cell_proof_run(CellProofBuild *build,const XrXirCellProofBodyHead *head,
    XrXirCellProofRunCursor *cursor,XrXirCellProofRun *output) {
    if (build->status!=XR_XIR_OK) return false;
    if (!head || !cursor || !output || !build->body->range)
        return cell_proof_error(build,UINT32_MAX,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
    uint32_t first=cursor->next,f=cursor->function;
    if (first>=head->instruction_count)
        return cell_proof_error(build,f,first,XR_XIR_BAD_STRUCTURE);
    XrXirStatus status=build->body->range(build->context,build->body,cursor,output);
    if (status!=XR_XIR_OK) return cell_proof_error(build,f,first,status);
    if (!cell_proof_work(build,7)) return false;
    if (output->first!=first || !output->count || output->count>head->instruction_count-first ||
        cursor->function!=f || cursor->next!=first+output->count || output->varying&~UINT32_C(1023) ||
        output->operation.op<=XR_XIR_INVALID || output->operation.op>=XR_XIR_OP_COUNT)
        return cell_proof_error(build,f,first,XR_XIR_BAD_STRUCTURE);
    return true;
}
/* The actual first row is already owned by this local request. Other rows
 * retain the original complete getter and its precise failure coordinate. */
static const XrXirInstruction *cell_proof_run_operation(CellProofBuild *build,uint32_t f,
    const XrXirCellProofRun *run,uint32_t i,XrXirInstruction *storage) {
    if (!run || i<run->first || i-run->first>=run->count) {
        cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);return NULL;
    }
    return i==run->first?&run->operation:cell_proof_instruction(build,f,i,storage);
}

/* The caller's private row is the sole initialized destination. A failed
 * callback cannot expose this scratch or publish the containing proof. */
static bool cell_proof_shape(CellProofBuild *build,uint32_t f,uint32_t i,
    XrXirCellProofInstructionShape *output) {
    if (build->status!=XR_XIR_OK || !output || !cell_proof_work(build,sizeof(*output))) return false;
    *output=(XrXirCellProofInstructionShape){0};
    XrXirStatus status=build->body->shape(build->context,build->body->owner,f,i,output);
    if (status!=XR_XIR_OK) return cell_proof_error(build,f,i,status);
    if (output->op<=XR_XIR_INVALID || output->op>=XR_XIR_OP_COUNT)
        return cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);
    return true;
}
static XrXirType cell_proof_type(CellProofBuild *build,uint32_t f,uint32_t value) {
    XrXirType type=XR_XIR_UNIT;
    if (build->status!=XR_XIR_OK) return type;
    XrXirStatus status=build->body->type(build->context,build->body->owner,f,value,&type);
    if (status!=XR_XIR_OK) {cell_proof_error(build,f,UINT32_MAX,status);return XR_XIR_UNIT;}
    return type;
}
static uint32_t cell_proof_operand_count(CellProofBuild *build,uint32_t f,
    const XrXirCellProofBodyHead *head,const XrXirInstruction *op) {
    XrXirType cell=XR_XIR_UNIT;
    if (op->op==XR_XIR_CELL_READ || op->op==XR_XIR_CELL_WRITE || op->op==XR_XIR_CELL_LOCAL_WRITE)
        cell=cell_proof_type(build,f,op->args[0]);
    return operand_count_metadata(build->body->domain.types,build->body->domain.declarations,head->result,cell,op);
}
static bool cell_proof_compare_bytes(const XrXirCompileContext *context,const void *a,const void *b,
    size_t count,size_t stride,XrXirStatus *status) {
    if (!!a!=!!count || !!b!=!!count || (stride && count>SIZE_MAX/stride)) {*status=XR_XIR_BAD_STRUCTURE;return false;}
    size_t bytes=count*stride;
    if (!xir_compile_work(context,bytes)) {*status=XR_XIR_BUDGET;return false;}
    if (bytes && memcmp(a,b,bytes)) {*status=XR_XIR_BAD_STRUCTURE;return false;}
    return true;
}
/* Counts are matched before any borrowed owned-array slice is read. Every
 * function record is compared by its real fields, never by C padding bytes. */
static XrXirStatus cell_proof_matches(const XrXirCompileContext *context,
    const XrXirCellProvenance *stored,const XrXirCellProvenance *fresh) {
    if (!xir_compile_work(context,8)) return XR_XIR_BUDGET;
    if (stored->function_count!=fresh->function_count || stored->value_count!=fresh->value_count ||
        stored->scope_count!=fresh->scope_count || stored->class_count!=fresh->class_count ||
        stored->project_count!=fresh->project_count || stored->class_dependency_count!=fresh->class_dependency_count ||
        (!!stored->functions!=!!fresh->function_count)) return XR_XIR_BAD_STRUCTURE;
    size_t words=0;
    for (uint32_t f=0;f<fresh->function_count;++f) {
        if (!xir_compile_work(context,12)) return XR_XIR_BUDGET;
        const CellProofFunction *a=&stored->functions[f],*b=&fresh->functions[f];
        if (a->begin!=b->begin || a->count!=b->count || a->parameters!=b->parameters ||
            a->scope_begin!=b->scope_begin || a->scopes!=b->scopes || a->words!=b->words ||
            a->stored!=b->stored || a->dependency_begin!=b->dependency_begin || a->class_begin!=b->class_begin ||
            a->class_count!=b->class_count || a->class_words!=b->class_words ||
            a->class_dependency_begin!=b->class_dependency_begin) return XR_XIR_BAD_STRUCTURE;
        if (b->words && b->stored>(SIZE_MAX-words)/b->words) return XR_XIR_BAD_STRUCTURE;
        words+=(size_t)b->stored*b->words;
    }
    XrXirStatus status=XR_XIR_OK;
    if (!cell_proof_compare_bytes(context,stored->origins,fresh->origins,fresh->value_count,1,&status) ||
        !cell_proof_compare_bytes(context,stored->roles,fresh->roles,fresh->value_count,1,&status) ||
        !cell_proof_compare_bytes(context,stored->tracked,fresh->tracked,fresh->value_count,1,&status) ||
        !cell_proof_compare_bytes(context,stored->value_functions,fresh->value_functions,fresh->value_count,sizeof(uint32_t),&status) ||
        !cell_proof_compare_bytes(context,stored->scope_parameters,fresh->scope_parameters,fresh->scope_count,sizeof(uint32_t),&status) ||
        !cell_proof_compare_bytes(context,stored->dependencies,fresh->dependencies,words,sizeof(uint64_t),&status) ||
        !cell_proof_compare_bytes(context,stored->class_dependencies,fresh->class_dependencies,fresh->class_dependency_count,sizeof(uint64_t),&status))
        return status;
    if (!!stored->class_roots!=!!fresh->class_count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t r=0;r<fresh->class_count;++r) {
        if (!xir_compile_work(context,4)) return XR_XIR_BUDGET;
        if (stored->class_roots[r].place!=fresh->class_roots[r].place ||
            stored->class_roots[r].value!=fresh->class_roots[r].value) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
