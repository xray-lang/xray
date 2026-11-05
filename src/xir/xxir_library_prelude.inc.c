/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
/* Fixed canonical inventory after complete Checked and construction verification.
 * Native descriptor proof remains the existing common nominal verifier. */
#ifndef XXIR_LIBRARY_PRELUDE_INC
#define XXIR_LIBRARY_PRELUDE_INC
#include "xxir_library_governed.inc.c"
#include "xxir_prelude_source_internal.h"
#include "xxir_interface.h"
#include "xxir_implementation.h"
static XrXirStatus library_prelude_literal(
    XrXirLiteral actual,const char *expected,bool *equal) {
    size_t length=0;
    while (true) {

        if (!expected[length]) break;
        if (length==SIZE_MAX-1) return XR_XIR_BUDGET;
        ++length;
    }

    *equal=actual.length==length && !memcmp(actual.bytes,expected,length); return XR_XIR_OK;
}
static XrXirStatus library_range_record(const XrXirNominalDeclaration *record) {
    if(record->kind!=XR_XIR_NOMINAL_CLASS || record->exported!=1 || record->parameter_count ||
        record->variant_count || record->flags || record->native.native_id || record->field_count!=4 || !record->fields)
        return XR_XIR_BAD_STRUCTURE;
    const char *names[]={"start","end","_step","_inclusive"};
    for(uint32_t f=0;f<4;++f) {
        bool equal=false;XrXirStatus status=library_prelude_literal(record->fields[f].name,names[f],&equal);
        if(status!=XR_XIR_OK) return status;
        if(!equal || record->fields[f].flags!=(uint32_t)(f<2?0:XR_XIR_FIELD_PRIVATE) ||
            record->fields[f].type!=(f==3?XR_XIR_BOOL:XR_XIR_I64)) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
/* Cold prelude reception checks the complete ordinary definition, including
 * data flow and branches. A type name or input hash cannot replace that body. */
static const XrXirInstruction library_range_init_ops[]={
    {XR_XIR_LOCAL_UNINIT,XR_XIR_I64,{0,0},{0,0},1,{0}},
    {XR_XIR_LOCAL_UNINIT,XR_XIR_I64,{0,0},{0,0},1,{0}},
    {XR_XIR_LOCAL_UNINIT,XR_XIR_I64,{0,0},{0,0},1,{0}},
    {XR_XIR_LOCAL_UNINIT,XR_XIR_BOOL,{0,0},{0,0},1,{0}},
    {XR_XIR_LOCAL_WRITE,XR_XIR_UNIT,{3,0},{0,0},0,{0}},
    {XR_XIR_LOCAL_WRITE,XR_XIR_UNIT,{4,1},{0,0},0,{0}},
    {XR_XIR_CONST_INT,XR_XIR_I64,{0,0},{0,0},1,{0}},
    {XR_XIR_LOCAL_WRITE,XR_XIR_UNIT,{5,9},{0,0},0,{0}},
    {XR_XIR_LOCAL_WRITE,XR_XIR_UNIT,{6,2},{0,0},0,{0}},
    {XR_XIR_LOCAL_READ,XR_XIR_I64,{3,0},{0,0},0,{0}},
    {XR_XIR_LOCAL_READ,XR_XIR_I64,{4,0},{0,0},0,{0}},
    {XR_XIR_LOCAL_READ,XR_XIR_I64,{5,0},{0,0},0,{0}},
    {XR_XIR_LOCAL_READ,XR_XIR_BOOL,{6,0},{0,0},0,{0}},
    {XR_XIR_CLASS_NEW,XR_XIR_UNIT,{0,4},{0,0},0,{0}},
    {XR_XIR_RETURN,XR_XIR_UNIT,{16,0},{0,0},0,{0}},
};
static const XrXirBlock library_range_init_blocks[]={
    {0,15,0,0},
};
static const XrXirInstruction library_range_count_ops[]={
    {XR_XIR_CLASS_GET,XR_XIR_I64,{0,0},{0,0},1,{0}},
    {XR_XIR_CLASS_GET,XR_XIR_I64,{0,0},{0,0},0,{0}},
    {XR_XIR_LT_INT,XR_XIR_BOOL,{1,2},{0,0},0,{0}},
    {XR_XIR_BRANCH,XR_XIR_UNIT,{3,0},{1,2},0,{0}},
    {XR_XIR_CONST_INT,XR_XIR_I64,{0,0},{0,0},0,{0}},
    {XR_XIR_RETURN,XR_XIR_UNIT,{5,0},{0,0},0,{0}},
    {XR_XIR_JUMP,XR_XIR_UNIT,{0,0},{3,0},0,{0}},
    {XR_XIR_CONST_INT,XR_XIR_I64,{0,0},{0,0},INT64_MAX,{0}},
    {XR_XIR_CONST_INT,XR_XIR_I64,{0,0},{0,0},0,{0}},
    {XR_XIR_CELL_NEW,XR_XIR_UNIT,{9,0},{0,0},0,{0}},
    {XR_XIR_CLASS_GET,XR_XIR_I64,{0,0},{0,0},0,{0}},
    {XR_XIR_CONST_INT,XR_XIR_I64,{0,0},{0,0},0,{0}},
    {XR_XIR_LT_INT,XR_XIR_BOOL,{11,12},{0,0},0,{0}},
    {XR_XIR_LOCAL_NEW,XR_XIR_BOOL,{13,0},{0,0},0,{0}},
    {XR_XIR_BRANCH,XR_XIR_UNIT,{13,0},{4,5},0,{0}},
    {XR_XIR_CLASS_GET,XR_XIR_I64,{0,0},{0,0},1,{0}},
    {XR_XIR_CONST_INT,XR_XIR_I64,{0,0},{0,0},0,{0}},
    {XR_XIR_GE_INT,XR_XIR_BOOL,{16,17},{0,0},0,{0}},
    {XR_XIR_LOCAL_WRITE,XR_XIR_UNIT,{14,18},{0,0},0,{0}},
    {XR_XIR_JUMP,XR_XIR_UNIT,{0,0},{5,0},0,{0}},
    {XR_XIR_LOCAL_READ,XR_XIR_BOOL,{14,0},{0,0},0,{0}},
    {XR_XIR_BRANCH,XR_XIR_UNIT,{21,0},{6,10},0,{0}},
    {XR_XIR_CLASS_GET,XR_XIR_I64,{0,0},{0,0},0,{0}},
    {XR_XIR_CONST_INT,XR_XIR_I64,{0,0},{0,0},1,{0}},
    {XR_XIR_ADD_INT,XR_XIR_I64,{23,24},{0,0},0,{0}},
    {XR_XIR_CONST_INT,XR_XIR_I64,{0,0},{0,0},0,{0}},
    {XR_XIR_SUB_INT,XR_XIR_I64,{26,25},{0,0},0,{0}},
    {XR_XIR_CLASS_GET,XR_XIR_I64,{0,0},{0,0},1,{0}},
    {XR_XIR_SUB_INT,XR_XIR_I64,{8,27},{0,0},0,{0}},
    {XR_XIR_GE_INT,XR_XIR_BOOL,{28,29},{0,0},0,{0}},
    {XR_XIR_BRANCH,XR_XIR_UNIT,{30,0},{7,8},0,{0}},
    {XR_XIR_RETURN,XR_XIR_UNIT,{8,0},{0,0},0,{0}},
    {XR_XIR_JUMP,XR_XIR_UNIT,{0,0},{9,0},0,{0}},
    {XR_XIR_CLASS_GET,XR_XIR_I64,{0,0},{0,0},1,{0}},
    {XR_XIR_ADD_INT,XR_XIR_I64,{34,27},{0,0},0,{0}},
    {XR_XIR_CONST_INT,XR_XIR_I64,{0,0},{0,0},1,{0}},
    {XR_XIR_ADD_INT,XR_XIR_I64,{35,36},{0,0},0,{0}},
    {XR_XIR_CELL_WRITE,XR_XIR_UNIT,{10,37},{0,0},0,{0}},
    {XR_XIR_JUMP,XR_XIR_UNIT,{0,0},{11,0},0,{0}},
    {XR_XIR_CLASS_GET,XR_XIR_I64,{0,0},{0,0},1,{0}},
    {XR_XIR_CLASS_GET,XR_XIR_I64,{0,0},{0,0},0,{0}},
    {XR_XIR_SUB_INT,XR_XIR_I64,{40,41},{0,0},0,{0}},
    {XR_XIR_CELL_WRITE,XR_XIR_UNIT,{10,42},{0,0},0,{0}},
    {XR_XIR_JUMP,XR_XIR_UNIT,{0,0},{11,0},0,{0}},
    {XR_XIR_CLASS_GET,XR_XIR_BOOL,{0,0},{0,0},3,{0}},
    {XR_XIR_LOCAL_NEW,XR_XIR_BOOL,{45,0},{0,0},0,{0}},
    {XR_XIR_BRANCH,XR_XIR_UNIT,{45,0},{12,13},0,{0}},
    {XR_XIR_CELL_READ,XR_XIR_I64,{10,0},{0,0},0,{0}},
    {XR_XIR_LT_INT,XR_XIR_BOOL,{48,8},{0,0},0,{0}},
    {XR_XIR_LOCAL_WRITE,XR_XIR_UNIT,{46,49},{0,0},0,{0}},
    {XR_XIR_JUMP,XR_XIR_UNIT,{0,0},{13,0},0,{0}},
    {XR_XIR_LOCAL_READ,XR_XIR_BOOL,{46,0},{0,0},0,{0}},
    {XR_XIR_BRANCH,XR_XIR_UNIT,{52,0},{14,15},0,{0}},
    {XR_XIR_CELL_READ,XR_XIR_I64,{10,0},{0,0},0,{0}},
    {XR_XIR_CONST_INT,XR_XIR_I64,{0,0},{0,0},1,{0}},
    {XR_XIR_ADD_INT,XR_XIR_I64,{54,55},{0,0},0,{0}},
    {XR_XIR_RETURN,XR_XIR_UNIT,{56,0},{0,0},0,{0}},
    {XR_XIR_JUMP,XR_XIR_UNIT,{0,0},{16,0},0,{0}},
    {XR_XIR_CELL_READ,XR_XIR_I64,{10,0},{0,0},0,{0}},
    {XR_XIR_RETURN,XR_XIR_UNIT,{59,0},{0,0},0,{0}},
};
static const XrXirBlock library_range_count_blocks[]={
    {0,4,0,0},
    {4,2,0,0},
    {6,1,0,0},
    {7,8,0,0},
    {15,5,0,0},
    {20,2,0,0},
    {22,9,0,0},
    {31,1,0,0},
    {32,1,0,0},
    {33,6,0,0},
    {39,5,0,0},
    {44,3,0,0},
    {47,4,0,0},
    {51,2,0,0},
    {53,4,0,0},
    {57,1,0,0},
    {58,2,0,0},
};
static XrXirStatus library_range_body(const XrXirModule *module,
    const XrXirFunction *body,bool init,XrXirType owner) {
    const XrXirInstruction *expected=init?library_range_init_ops:library_range_count_ops;
    const XrXirBlock *blocks=init?library_range_init_blocks:library_range_count_blocks;
    uint32_t count=init?(uint32_t)(sizeof(library_range_init_ops)/sizeof(*expected)):
        (uint32_t)(sizeof(library_range_count_ops)/sizeof(*expected));
    uint32_t block_count=init?(uint32_t)(sizeof(library_range_init_blocks)/sizeof(*blocks)):
        (uint32_t)(sizeof(library_range_count_blocks)/sizeof(*blocks));
    if(body->instruction_count!=count || body->block_count!=block_count || body->operand_count!=(init?4u:0u))
        return XR_XIR_BAD_STRUCTURE;
    for(uint32_t b=0;b<block_count;++b) {

        const XrXirBlock *actual=&body->blocks[b];
        if(actual->first!=blocks[b].first || actual->count!=blocks[b].count || actual->panic || actual->frontier)
            return XR_XIR_BAD_STRUCTURE;
    }
    for(uint32_t i=0;i<count;++i) {

        const XrXirInstruction *a=&body->instructions[i],*e=&expected[i];
        bool type=a->type==e->type;
        if(e->op==XR_XIR_CLASS_NEW) type=a->type==owner;
        if(e->op==XR_XIR_CELL_NEW) type=xr_xir_type_is_cell(module->types,a->type) &&
            xr_xir_cell_element(module->types,a->type)==XR_XIR_I64;
        if(a->op!=e->op || !type || a->args[0]!=e->args[0] || a->args[1]!=e->args[1] ||
            a->targets[0]!=e->targets[0] || a->targets[1]!=e->targets[1] || a->immediate!=e->immediate ||
            a->type_arguments[0] || a->type_arguments[1]) return XR_XIR_BAD_STRUCTURE;
    }
    for(uint32_t o=0;o<body->operand_count;++o) {

        if(body->operands[o]!=12+o) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus library_range_function(const XrXirModule *module,
    uint32_t function,uint32_t declaration,uint32_t *seen) {
    const XrXirFunctionIdentity *identity=&module->declarations->functions[function];
    const XrXirFunction *body=&module->functions[function];
    bool init=false,count=false;
    XrXirStatus status=library_prelude_literal((XrXirLiteral){body->name,body->name_length},"init",&init);
    if(status==XR_XIR_OK) status=library_prelude_literal((XrXirLiteral){body->name,body->name_length},"count",&count);
    if(status!=XR_XIR_OK) return status;
    uint32_t role=init?1u:count?2u:0;
    if(!role || (*seen&role) || identity->nominal_owner!=declaration+1 ||
        identity->exported!=1 || identity->member_access ||
        identity->method_kind!=(uint32_t)(init?XR_XIR_CONSTRUCTOR:XR_XIR_READ_METHOD) ||
        identity->promises!=(init?XR_XIR_FUNCTION_NO_SUSPEND:0u) || identity->cleanup_owner ||
        identity->test_role || identity->test_timeout_seconds || body->error_type!=XR_XIR_NEVER ||
        body->parameter_count!=(init?3u:1u)) return XR_XIR_BAD_STRUCTURE;
    XrXirType owner_type=init?body->result:body->parameters[0];
    const XrXirTypeNode *owner=xr_xir_type_node(module->types,owner_type);
    if(!owner || owner->kind!=XR_XIR_TYPE_NOMINAL || owner->parameter_span ||
        owner->nominal.declaration!=declaration || owner->nominal.argument_count || owner->nominal.field_count!=4 ||
        (init && (body->parameters[0]!=XR_XIR_I64 || body->parameters[1]!=XR_XIR_I64 || body->parameters[2]!=XR_XIR_BOOL)) ||
        (!init && body->result!=XR_XIR_I64)) return XR_XIR_BAD_STRUCTURE;
    status=library_range_body(module,body,init,owner_type);
    if(status==XR_XIR_OK) *seen|=role;
    return status;
}
static XrXirStatus library_prelude_module(
    const XrXirModule *module,uint32_t ordinal,bool *output) {
    const XrXirDeclarations *d=module->declarations;
    if (!d || ordinal>=d->module_count || !output) return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceModule *view=&d->modules[ordinal]; bool canonical=false;
    XrXirStatus status=library_prelude_literal((XrXirLiteral){view->name,view->name_length},XIR_PRELUDE_CANONICAL,&canonical);
    if (status!=XR_XIR_OK) return status;
    if (!canonical) { *output=false; return XR_XIR_OK; }
    bool governed=false;
    status=library_governed_ordering_module(module,ordinal,&governed);
    if (status!=XR_XIR_OK) return status;
    if (!governed || view->dependency_count || !module->types || !module->types->nominals)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirNominalTable *table=module->types->nominals; uint32_t owned=0;
    uint32_t seen[sizeof(xir_prelude_enums)/sizeof(*xir_prelude_enums)]={0},range=UINT32_MAX;
    if (xir_prelude_source.enum_count!=sizeof(seen)/sizeof(*seen)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i=0;i<table->count;++i) {

        const XrXirNominalDeclaration *record=&table->declarations[i]; bool owner=false;
        status=library_prelude_literal(record->module,XIR_PRELUDE_CANONICAL,&owner);
        if (status!=XR_XIR_OK) return status;
        if (!owner) continue;
        ++owned; uint32_t match=UINT32_MAX;
        bool is_range=false;status=library_prelude_literal(record->name,"Range",&is_range);
        if(status!=XR_XIR_OK) return status;
        if(is_range) {
            if(range!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            status=library_range_record(record);if(status!=XR_XIR_OK) return status;
            range=i;continue;
        }
        for (uint32_t e=0;e<xir_prelude_source.enum_count;++e) {
            bool equal=false; status=library_prelude_literal(record->name,xir_prelude_source.enums[e].name,&equal);
            if (status!=XR_XIR_OK) return status;
            if (equal) match=e;
        }
        if (match==UINT32_MAX || seen[match]++ || record->kind!=XR_XIR_NOMINAL_ENUM ||
            record->exported!=1 || record->flags)
            return XR_XIR_BAD_STRUCTURE;
        const XirPreludeEnum *expected=&xir_prelude_source.enums[match];
        if (record->parameter_count!=expected->parameter_count || record->field_count!=expected->field_count)
            return XR_XIR_BAD_STRUCTURE;
        if (expected->field_count) {
            bool field_name=false;
            status=library_prelude_literal(record->fields[0].name,"value",&field_name);
            if (status!=XR_XIR_OK) return status;
            if (!field_name || record->fields[0].flags || record->fields[0].type!=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE ||
                record->constraints[0].markers || record->constraints[0].interface_count) return XR_XIR_BAD_STRUCTURE;
        }
        if (record->native.native_id!=expected->native_id || record->variant_count!=expected->variant_count)
            return XR_XIR_BAD_STRUCTURE;

        for (size_t byte=0;byte<sizeof(record->native.source_fingerprint);++byte) {
            uint8_t wanted=expected->native_id ? xir_prelude_source.input_sha256[byte] : 0;
            if (record->native.source_fingerprint[byte]!=wanted) return XR_XIR_BAD_STRUCTURE;
        }
        uint32_t begin=0;
        for (uint32_t v=0;v<expected->variant_count;++v) {
            const XrXirNominalVariant *variant=&record->variants[v]; bool equal=false;
            status=library_prelude_literal(variant->name,expected->variants[v],&equal);
            if (status!=XR_XIR_OK) return status;
            uint32_t count=expected->field_count && !v ? 1u : 0u;
            if (!equal || variant->field_begin!=begin || variant->field_count!=count) return XR_XIR_BAD_STRUCTURE;
            begin+=count;
        }
    }
    if (owned!=xir_prelude_source.enum_count+1 || range==UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t e=0;e<xir_prelude_source.enum_count;++e) if (!seen[e]) return XR_XIR_BAD_STRUCTURE;
    const XrXirImplementationTable *implementations=d->implementations;
    for (uint32_t i=0;implementations && i<implementations->count;++i) {
        bool owner=false;
        status=library_prelude_literal(
            table->declarations[implementations->records[i].nominal_declaration].module,XIR_PRELUDE_CANONICAL,&owner);
        if (status!=XR_XIR_OK) return status;
        if (owner) return XR_XIR_BAD_STRUCTURE;
    }
    const XrXirInterfaceTable *interfaces=module->types->interfaces;
    for (uint32_t i=0;interfaces && i<interfaces->count;++i) {
        bool owner=false; status=library_prelude_literal(interfaces->declarations[i].module,XIR_PRELUDE_CANONICAL,&owner);
        if (status!=XR_XIR_OK) return status;
        if (owner) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t s=0;s<d->slot_count;++s) {

        if (d->slots[s].module==ordinal) return XR_XIR_BAD_STRUCTURE;
    }
    uint32_t range_methods=0;
    for (uint32_t f=0;f<module->function_count;++f) {

        if (d->functions[f].module==ordinal && f!=view->initializer) {
            if(d->functions[f].nominal_owner!=range+1) return XR_XIR_BAD_STRUCTURE;
            status=library_range_function(module,f,range,&range_methods);
            if(status!=XR_XIR_OK) return status;
        }
    }
    if(range_methods!=3) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *initializer=&module->functions[view->initializer];
    if (initializer->parameter_count || initializer->result!=XR_XIR_UNIT ||
        initializer->block_count!=1 || initializer->instruction_count!=1 || initializer->operand_count ||
        initializer->blocks[0].first || initializer->blocks[0].count!=1 ||
        initializer->instructions[0].op!=XR_XIR_RETURN || initializer->instructions[0].type!=XR_XIR_UNIT)
        return XR_XIR_BAD_STRUCTURE;
    *output=true; return XR_XIR_OK;
}
#endif
