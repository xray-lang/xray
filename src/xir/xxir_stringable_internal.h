/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_stringable_internal.h - Intrinsic facts and executable scalar witnesses
 *
 * KEY CONCEPT:
 *   Metadata conformance never supplies body or execution authority. A witness
 *   independently reads the actual module, signatures and complete helper code.
 */
#ifndef XXIR_STRINGABLE_INTERNAL_H
#define XXIR_STRINGABLE_INTERNAL_H
#include "xxir_interface.h"
#include "xxir_types.h"
#include "xxir_receive_control.h"
#include "xxir_prelude_identity.h"
#include "../shared/xnative_declaration.h"
#include <string.h>
typedef struct XirStringableRead {
    XrCompileRequestControl *control;
    uint32_t rows;
    XrXirStatus status;
} XirStringableRead;
static inline XirStringableRead xir_stringable_read(XrCompileRequestControl *control) {
    return (XirStringableRead){control,0,xir_receive_poll(control)};
}
static inline bool xir_stringable_step(XirStringableRead *read) {
    if (read->status==XR_XIR_OK) read->status=xir_receive_step(read->control,&read->rows);
    return read->status==XR_XIR_OK;
}
static inline bool xir_stringable_literal(XrXirLiteral text,const char *expected) {
    size_t length=strlen(expected);
    return text.length==length && text.bytes && !memcmp(text.bytes,expected,length);
}
static inline bool xir_stringable_native(XrXirType type,XirStringableRead *read) {
    if (type!=XR_XIR_I64 && type!=XR_XIR_STRING) return false;
    const XrNativeTypeDeclaration *owner=xr_native_declaration_by_id(
        type==XR_XIR_I64?XR_NATIVE_DECLARATION_INTEGER:XR_NATIVE_DECLARATION_STRING);
    if (!owner || owner->parameter_count || !owner->members) return false;
    XrNativeOperation operation=type==XR_XIR_I64?XR_NATIVE_OPERATION_INTEGER_TO_STRING:XR_NATIVE_OPERATION_STRING_TO_STRING;
    uint32_t found=0;
    for (uint32_t i=0;i<owner->member_count;++i) {
        if (!xir_stringable_step(read)) return false;
        const XrNativeMemberDeclaration *member=&owner->members[i];
        if (member->operation!=operation) continue;
        if (member->receiver!=XR_NATIVE_RECEIVER_READ || member->is_static || !member->is_method || member->lowered ||
            !member->is_public || member->parameter_count || member->parameters || member->result!=XR_NATIVE_TERM_STRING ||
            member->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED || !member->signature || strcmp(member->signature,"() -> string") ||
            !member->failures || strcmp(member->failures,type==XR_XIR_I64?"allocation,retain,limit":"retain") ||
            member->allocation!=(type==XR_XIR_I64?XR_NATIVE_ALLOCATION_MAY_HEAP:XR_NATIVE_ALLOCATION_NO_HEAP))
            return false;
        ++found;
    }
    return found==1;
}
static inline bool xir_stringable_schema(const XrXirTypes *types,const XrXirDeclarations *d,
    XrXirInterfaceApplication application,uint32_t *module,XirStringableRead *read) {
    if (!types || !types->interfaces || !types->interfaces->declarations || !d || !d->modules ||
        application.declaration>=types->interfaces->count || application.argument_count || application.arguments) return false;
    const XrXirInterfaceDeclaration *interface=&types->interfaces->declarations[application.declaration];
    if (!xir_stringable_literal(interface->module,XIR_PRELUDE_CANONICAL) ||
        !xir_stringable_literal(interface->name,"Stringable") || interface->exported!=1 ||
        interface->parameter_count || interface->constraints || interface->parent_count || interface->parents ||
        interface->method_count!=1 || !interface->methods) return false;
    const XrXirInterfaceMethod *method=interface->methods;
    const XrXirTypeNode *signature=xr_xir_callable_signature(types,method->signature);
    if (!xir_stringable_literal(method->name,"toString") || method->receiver || method->own_parameter_count ||
        method->constraints || !signature || signature->flags!=XR_XIR_CALLABLE_ROOT_UNRESOLVED || signature->parameter_span ||
        signature->parameter_count || signature->parameters || signature->result!=XR_XIR_STRING ||
        signature->element!=XR_XIR_NEVER) return false;
    uint32_t selected=UINT32_MAX;
    for (uint32_t m=0;m<d->module_count;++m) {
        if (!xir_stringable_step(read)) return false;
        const XrXirSourceModule *view=&d->modules[m];
        if (!xir_stringable_literal((XrXirLiteral){view->name,view->name_length},XIR_PRELUDE_CANONICAL)) continue;
        if (selected!=UINT32_MAX || view->lexical_scope || view->storage) return false;
        selected=m;
    }
    if (selected==UINT32_MAX) return false;
    if (module) *module=selected;
    return true;
}
static inline bool xir_stringable_conformance(const XrXirTypes *types,const XrXirDeclarations *d,
    XrXirInterfaceApplication application,XrXirType subject,XirStringableRead *read) {
    return read->status==XR_XIR_OK && xir_stringable_native(subject,read) && xir_stringable_schema(types,d,application,NULL,read);
}
static inline bool xir_stringable_body(const XrXirModule *m,uint32_t index,XrXirType subject,XirStringableRead *read) {
    const XrXirFunction *f=&m->functions[index];
    const XrXirFunctionIdentity *identity=&m->declarations->functions[index];
    if (identity->exported!=1 || identity->nominal_owner || identity->method_kind || identity->member_access ||
        identity->cleanup_owner || identity->native_callable || identity->test_role || identity->test_timeout_seconds ||
        (identity->promises&~XR_XIR_FUNCTION_NO_SUSPEND) ||
        (m->generics && (m->generics[index].parameter_count || m->generics[index].argument_count)) ||
        f->parameter_count!=1 || !f->parameters || f->parameters[0]!=subject || f->result!=XR_XIR_STRING ||
        f->error_type!=XR_XIR_NEVER || f->block_count!=1 || !f->blocks ||
        !f->instruction_count || !f->instructions || f->blocks[0].first ||
        f->blocks[0].count!=f->instruction_count || f->blocks[0].panic || f->blocks[0].frontier || f->operand_count || f->operands) return false;
    uint32_t value=0;XrXirType type=subject;bool converted=subject==XR_XIR_STRING;
    for (uint32_t i=0;i<f->instruction_count;++i) {
        if (!xir_stringable_step(read)) return false;
        const XrXirInstruction *op=&f->instructions[i];
        if (op->args[1] || op->targets[0] || op->targets[1] || op->immediate ||
            op->type_arguments[0] || op->type_arguments[1]) return false;
        if (op->op==XR_XIR_VIEW_CLEANUP && !op->args[0] && op->type==XR_XIR_UNIT) continue;
        if ((op->op==XR_XIR_COPY || (op->op==XR_XIR_OWNED_RETAIN && type==XR_XIR_STRING)) &&
            op->type==type && op->args[0]==value) value=1u+i;
        else if (op->op==XR_XIR_TO_STRING && !converted && type==XR_XIR_I64 &&
            op->type==XR_XIR_STRING && op->args[0]==value) {
            value=1u+i;type=XR_XIR_STRING;converted=true;
        } else if (op->op==XR_XIR_RETURN)
            return converted && op->type==XR_XIR_UNIT && op->args[0]==value && i+1u==f->instruction_count;
        else return false;
    }
    return false;
}
static inline XrXirStatus xir_stringable_witness(const XrXirModule *m,
    XrXirInterfaceApplication application,uint32_t member,XrXirType subject,uint32_t *output,XirStringableRead *read) {
    uint32_t module=0;
    if (!m || !m->functions || !m->declarations || !m->declarations->functions || !output || member ||
        !xir_stringable_conformance(m->types,m->declarations,application,subject,read) ||
        !xir_stringable_schema(m->types,m->declarations,application,&module,read)) return read->status==XR_XIR_OK?XR_XIR_BAD_TYPE:read->status;
    uint32_t integer=UINT32_MAX,string=UINT32_MAX;
    for (uint32_t f=0;f<m->function_count;++f) {
        if (!xir_stringable_step(read)) return read->status;
        const XrXirFunctionIdentity *identity=&m->declarations->functions[f];
        if (identity->module!=module || f==m->declarations->modules[module].initializer || identity->nominal_owner) continue;
        const XrXirFunction *function=&m->functions[f];
        if (function->parameter_count!=1 || !function->parameters) return XR_XIR_BAD_TYPE;
        XrXirType type=function->parameters[0];
        if (!xir_stringable_native(type,read) || !xir_stringable_body(m,f,type,read))
            return read->status==XR_XIR_OK?XR_XIR_BAD_TYPE:read->status;
        uint32_t *selected=type==XR_XIR_I64?&integer:&string;
        if (*selected!=UINT32_MAX) return XR_XIR_BAD_TYPE;
        *selected=f;
    }
    if (integer==UINT32_MAX || string==UINT32_MAX) return XR_XIR_BAD_TYPE;
    *output=subject==XR_XIR_I64?integer:string;return XR_XIR_OK;
}
/* Only the independently verified scalar helper can enter a cold body recipe. */
static inline bool xir_stringable_text_helper(const XrXirModule *module,uint32_t function,
    XirStringableRead *read) {
    const XrXirInterfaceTable *table=module->types?module->types->interfaces:NULL;
    uint32_t selected=UINT32_MAX;
    for (uint32_t i=0;table && i<table->count;++i) {
        if (!xir_stringable_step(read)) return false;
        XrXirInterfaceApplication app={i,NULL,0};
        if (!xir_stringable_schema(module->types,module->declarations,app,NULL,read)) continue;
        if (selected!=UINT32_MAX) return false;
        selected=i;
    }
    uint32_t helper=UINT32_MAX;
    return selected!=UINT32_MAX && xir_stringable_witness(module,
        (XrXirInterfaceApplication){selected,NULL,0},0,XR_XIR_I64,&helper,read)==XR_XIR_OK && helper==function;
}
#endif // XXIR_STRINGABLE_INTERNAL_H
