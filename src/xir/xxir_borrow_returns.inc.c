/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_borrow_returns.inc.c - Receiver-root facts from complete storage returns
 *
 * KEY CONCEPT:
 *   A governed leaf's input is not its wrapper's receiver. Actual Class field
 *   reads connect the two, and every returned value must repeat that relation.
 */
#include "xxir_native_callable.h"
#include "xxir_receive_control.h"

static bool borrow_return_literal(XrXirLiteral literal,const char *text,size_t size) {
    return literal.bytes && literal.length==size && !memcmp(literal.bytes,text,size);
}
static bool borrow_return_owner(const XrXirModule *m,uint32_t f,uint32_t *nominal) {
    if (!m || !m->declarations || !m->declarations->functions || !m->functions ||
        f>=m->function_count || !m->types || !m->types->nominals ||
        (!m->types->nominals->declarations && !m->types->nominals->identities) ||
        !xir_buffer_storage_scope(m->declarations,f)) return false;
    const XrXirFunctionIdentity *id=&m->declarations->functions[f];
    const XrXirFunction *body=&m->functions[f];
    if (!id->nominal_owner || id->nominal_owner>m->types->nominals->count ||
        id->method_kind!=XR_XIR_READ_METHOD || id->native_callable || id->cleanup_owner ||
        id->test_role || (id->promises&(XR_XIR_FUNCTION_GENERATOR|XR_XIR_FUNCTION_REST)) ||
        body->parameter_count!=1 || !body->parameters || body->error_type!=XR_XIR_NEVER ||
        (m->generics && (m->generics[f].parameter_count || m->generics[f].argument_count))) return false;
    uint32_t ordinal=id->nominal_owner-1u;
    const XrXirNominalTable *table=m->types->nominals;
    const XrXirNominalDeclaration *decl=table->declarations?&table->declarations[ordinal]:NULL;
    const XrXirNominalIdentity *identity=decl?NULL:&table->identities[ordinal];
    const XrXirSourceModule *scope=&m->declarations->modules[id->module];
    const XrXirTypeNode *receiver=xr_xir_type_node(m->types,body->parameters[0]);
    uint32_t fields=decl?decl->field_count:identity->field_count;
    const XrXirNominalNativeRecord *native=decl?&decl->native:&identity->native;
    if ((decl?decl->kind:identity->kind)!=XR_XIR_NOMINAL_CLASS ||
        (decl?decl->flags:identity->flags)!=XR_XIR_NOMINAL_FINAL || !(decl?decl->exported:identity->exported) ||
        (decl?decl->parameter_count:identity->arity) || native->native_id || fields!=1 ||
        (decl?!decl->fields:!identity->fields) ||
        (decl && decl->fields[0].type!=XR_XIR_BUFFER_STORAGE) ||
        ((decl?decl->fields[0].flags:identity->fields[0].flags)&XR_XIR_FIELD_PRIVATE)==0 ||
        !borrow_return_literal(decl?decl->name:identity->name,"Buffer",6) ||
        !borrow_return_literal(decl?decl->module:identity->module,scope->name,scope->name_length) ||
        !receiver || receiver->kind!=XR_XIR_TYPE_NOMINAL || receiver->parameter_span ||
        receiver->nominal.declaration!=ordinal || receiver->nominal.argument_count ||
        receiver->nominal.field_count!=1 || !receiver->nominal.fields ||
        receiver->nominal.fields[0]!=XR_XIR_BUFFER_STORAGE) return false;
    *nominal=ordinal;return true;
}
static XrXirStatus borrow_return_alias(const XrXirFunction *body,uint32_t *value,XrCompileRequestControl *control) {
    uint64_t bound=(uint64_t)body->parameter_count+body->instruction_count;
    if (*value>=bound) return XR_XIR_BAD_STRUCTURE;
    uint32_t progress=0;
    while (*value>=body->parameter_count) {
        XrXirStatus status=xir_receive_step(control,&progress);if (status!=XR_XIR_OK) return status;
        const XrXirInstruction *op=&body->instructions[*value-body->parameter_count];
        if (op->op!=XR_XIR_COPY && op->op!=XR_XIR_OWNED_RETAIN) break;
        if (op->args[0]>=*value) return XR_XIR_BAD_STRUCTURE;
        *value=op->args[0];
    }
    return XR_XIR_OK;
}
static XrXirStatus borrow_return_value(const XrXirModule *m,uint32_t f,uint32_t value,uint32_t leaf,XrCompileRequestControl *control) {
    const XrXirFunction *body=&m->functions[f];
    XrXirStatus status=borrow_return_alias(body,&value,control);
    if (status!=XR_XIR_OK || value<body->parameter_count) return status==XR_XIR_OK?XR_XIR_BAD_TYPE:status;
    const XrXirInstruction *call=&body->instructions[value-body->parameter_count];
    if (call->op!=XR_XIR_CALL || call->immediate<0 || (uint64_t)call->immediate>=m->function_count ||
        call->type!=body->result || call->args[1]!=1 || call->args[0]>=body->operand_count)
        return XR_XIR_BAD_TYPE;
    uint32_t target=(uint32_t)call->immediate;
    if (m->declarations->functions[target].native_callable!=leaf ||
        m->declarations->functions[target].module!=m->declarations->functions[f].module)
        return XR_XIR_BAD_TYPE;
    status=xir_native_callable_verify(m,target);if (status!=XR_XIR_OK) return status;
    uint32_t storage=body->operands[call->args[0]];
    status=borrow_return_alias(body,&storage,control);
    if (status!=XR_XIR_OK || storage<body->parameter_count) return status==XR_XIR_OK?XR_XIR_BAD_TYPE:status;
    const XrXirInstruction *get=&body->instructions[storage-body->parameter_count];
    if (get->op!=XR_XIR_CLASS_GET || get->type!=XR_XIR_BUFFER_STORAGE || get->immediate!=0)
        return XR_XIR_BAD_TYPE;
    uint32_t receiver=get->args[0];status=borrow_return_alias(body,&receiver,control);
    return status!=XR_XIR_OK?status:receiver==0?XR_XIR_OK:XR_XIR_BAD_TYPE;
}
XR_FUNC XrXirStatus xir_borrow_return_describe(const XrXirCompileContext *context,
    const XrXirModule *m,uint32_t f,XrXirBorrowReturn *output,bool *present,XrCompileRequestControl *control) {
    if (!xir_compile_context_valid(context) || !m || !output || !present || f>=m->function_count)
        return XR_XIR_BAD_STRUCTURE;
    *present=false;*output=(XrXirBorrowReturn){0};uint32_t nominal=0;
    if (!borrow_return_owner(m,f,&nominal)) return XR_XIR_OK;
    const XrXirFunction *body=&m->functions[f];
    const XrXirTypeNode *result=xr_xir_type_node(m->types,body->result);
    bool length=body->result==XR_XIR_I64;
    if (!length && (!result || result->element!=XR_XIR_U8 || result->parameter_span ||
        (result->kind!=XR_XIR_TYPE_SLICE && result->kind!=XR_XIR_TYPE_MUT_PTR))) return XR_XIR_OK;
    uint32_t leaf=length?XR_NATIVE_CALLABLE_BUFFER_LENGTH:
        result->kind==XR_XIR_TYPE_SLICE?XR_NATIVE_CALLABLE_BUFFER_AS_BYTES:XR_NATIVE_CALLABLE_BUFFER_BORROW_PTR;
    if (!body->block_count || !body->blocks || !body->instructions ||
        (body->operand_count && !body->operands)) return XR_XIR_BAD_STRUCTURE;
    bool returned=false;uint32_t progress=0;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        XrXirStatus status=xir_receive_step(control,&progress);if (status!=XR_XIR_OK) return status;
        const XrXirInstruction *op=&body->instructions[i];
        switch (op->op) {
        case XR_XIR_CONST_INT:case XR_XIR_COPY:case XR_XIR_OWNED_RETAIN:break;
        case XR_XIR_CLASS_GET: {
            uint32_t receiver=op->args[0];status=borrow_return_alias(body,&receiver,control);
            if (status!=XR_XIR_OK) return status;
            if (op->type!=XR_XIR_BUFFER_STORAGE || op->immediate || receiver) return XR_XIR_BAD_TYPE;
            break;
        }
        case XR_XIR_CALL:
            status=borrow_return_value(m,f,body->parameter_count+i,leaf,control);break;
        case XR_XIR_VIEW_CLEANUP:
            /* Normalization may protect the future returned Slice. These
             * local scope ends neither mutate storage nor install a grant. */
            if ((op->args[1] && (!result || result->kind!=XR_XIR_TYPE_SLICE)) ||
                op->args[0]>body->operand_count || op->args[1]>body->operand_count-op->args[0]) return XR_XIR_BAD_TYPE;
            for (uint32_t p=0;p<op->args[1] && status==XR_XIR_OK;++p) {
                status=xir_receive_step(control,&progress);
                if (status==XR_XIR_OK) status=borrow_return_value(m,f,body->operands[op->args[0]+p],leaf,control);
            }
            break;
        case XR_XIR_RETURN:
            status=borrow_return_value(m,f,op->args[0],leaf,control);returned=true;break;
        default:return XR_XIR_BAD_TYPE;
        }
        if (status!=XR_XIR_OK) return status;
    }
    if (!returned) return XR_XIR_BAD_TYPE;
    *output=(XrXirBorrowReturn){f,0,nominal,0,leaf};*present=!length;return XR_XIR_OK;
}
XR_FUNC const XrXirBorrowReturn *xir_borrow_return_find(const XrXirConstruction *owner,uint32_t function) {
    if (!owner) return NULL;
    uint32_t lo=0,hi=owner->borrow_returns.count;
    while (lo<hi) {
        uint32_t mid=lo+(hi-lo)/2u;
        if (owner->borrow_returns.rows[mid].function<function) lo=mid+1u;else hi=mid;
    }
    return lo<owner->borrow_returns.count && owner->borrow_returns.rows[lo].function==function?
        &owner->borrow_returns.rows[lo]:NULL;
}
XR_FUNC XrXirStatus xir_borrow_returns_verify(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirConstruction *owner,XrCompileRequestControl *control) {
    if (!owner || (!!owner->borrow_returns.rows!=!!owner->borrow_returns.count)) return XR_XIR_BAD_STRUCTURE;
    uint32_t next=0,progress=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        XrXirStatus status=xir_receive_step(control,&progress);if (status!=XR_XIR_OK) return status;
        XrXirBorrowReturn actual={0};bool present=false;
        status=xir_borrow_return_describe(context,module,f,&actual,&present,control);
        if (status!=XR_XIR_OK) return status;
        if (!present) continue;
        if (next>=owner->borrow_returns.count) return XR_XIR_BAD_STRUCTURE;
        const XrXirBorrowReturn *row=&owner->borrow_returns.rows[next++];
        if (row->function!=actual.function || row->receiver!=actual.receiver || row->nominal!=actual.nominal ||
            row->field!=actual.field || row->native_callable!=actual.native_callable) return XR_XIR_BAD_STRUCTURE;
    }
    return next==owner->borrow_returns.count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
