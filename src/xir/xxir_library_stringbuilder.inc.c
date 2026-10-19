/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_library_stringbuilder.inc.c - Exact ordinary canonical text Class proof
 */
static XrXirStatus library_builder_record(const XrXirCompileContext *context,const XrXirNominalDeclaration *record) {
    if(record->kind!=XR_XIR_NOMINAL_CLASS || record->exported!=1 || record->parameter_count || record->variant_count ||
        record->flags || record->native.native_id || record->field_count!=2 || !record->fields) return XR_XIR_BAD_STRUCTURE;
    const char *names[]={"_text","_empty"};
    for(uint32_t field=0;field<2;++field) {
        bool equal=false;XrXirStatus status=library_prelude_literal(context,record->fields[field].name,names[field],&equal);
        if(status!=XR_XIR_OK) return status;
        uint32_t flags=XR_XIR_FIELD_PRIVATE|(field?0:XR_XIR_FIELD_MUTABLE);
        if(!equal || record->fields[field].type!=XR_XIR_STRING || record->fields[field].flags!=flags) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static bool library_builder_type(const XrXirModule *module,XrXirType type,uint32_t declaration) {
    const XrXirTypeNode *node=xr_xir_type_node(module->types,type);
    return node && !node->parameter_span && node->kind==XR_XIR_TYPE_NOMINAL &&
        node->nominal.declaration==declaration && !node->nominal.argument_count && node->nominal.field_count==2 &&
        node->nominal.fields && node->nominal.fields[0]==XR_XIR_STRING && node->nominal.fields[1]==XR_XIR_STRING;
}
static XrXirStatus library_builder_body(const XrXirCompileContext *context,const XrXirModule *module,
    const XrXirFunction *function,unsigned role) {
    if(function->block_count!=1 || function->blocks[0].first || function->blocks[0].count!=function->instruction_count ||
        function->instruction_count>12 || !function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t counts[XR_XIR_OP_COUNT]={0},get=UINT32_MAX,joined=UINT32_MAX,object=UINT32_MAX,field=UINT32_MAX;
    for(uint32_t i=0;i<function->instruction_count;++i) {
        if(!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirInstruction *op=&function->instructions[i];
        if(op->op>=XR_XIR_OP_COUNT || op->targets[0] || op->targets[1] || op->type_arguments[0] || op->type_arguments[1]) return XR_XIR_BAD_STRUCTURE;
        ++counts[op->op];uint32_t id=function->parameter_count+i;
        if(role==1) {
            if(op->op==XR_XIR_CONST_STRING) {
                if(op->immediate<0 || (uint64_t)op->immediate>=module->declarations->literal_count ||
                    module->declarations->literals[op->immediate].length) return XR_XIR_BAD_STRUCTURE;
            } else if(op->op!=XR_XIR_LOCAL_UNINIT && op->op!=XR_XIR_LOCAL_WRITE && op->op!=XR_XIR_LOCAL_READ &&
                op->op!=XR_XIR_CLASS_NEW && op->op!=XR_XIR_RETURN) return XR_XIR_BAD_STRUCTURE;
            continue;
        }
        switch(op->op) {
        case XR_XIR_CLASS_GET:
            if(op->args[0] || op->immediate!=(role==8?1:0) || op->type!=XR_XIR_STRING) return XR_XIR_BAD_STRUCTURE;
            get=id;break;
        case XR_XIR_CONCAT_STRING:
            if(role!=2 || op->args[0]!=get || op->args[1]!=1 || op->type!=XR_XIR_STRING) return XR_XIR_BAD_STRUCTURE;
            joined=id;break;
        case XR_XIR_OBJECT_PLACE:
            if(op->args[0]) return XR_XIR_BAD_STRUCTURE;
            object=id;break;
        case XR_XIR_FIELD_PLACE:
            if(op->args[0]!=object || op->immediate || op->type!=XR_XIR_STRING) return XR_XIR_BAD_STRUCTURE;
            field=id;break;
        case XR_XIR_PLACE_WRITE:
            if(op->args[0]!=field || op->args[1]!=(role==2?joined:get)) return XR_XIR_BAD_STRUCTURE;
            break;
        case XR_XIR_CLASS_SET:
            if(op->args[0] || op->immediate || op->args[1]!=(role==2?joined:get)) return XR_XIR_BAD_STRUCTURE;
            break;
        case XR_XIR_RETURN:
            if(i+1!=function->instruction_count || op->args[0]!=(role==4?get:0)) return XR_XIR_BAD_STRUCTURE;
            break;
        default:return XR_XIR_BAD_STRUCTURE;
        }
    }
    if(role==1) return counts[XR_XIR_CONST_STRING]==2 && counts[XR_XIR_CLASS_NEW]==1 && counts[XR_XIR_RETURN]==1 &&
        counts[XR_XIR_LOCAL_UNINIT]==2 && counts[XR_XIR_LOCAL_WRITE]==2 && counts[XR_XIR_LOCAL_READ]==2?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    if(counts[XR_XIR_CLASS_GET]!=1 || counts[XR_XIR_RETURN]!=1 || counts[XR_XIR_CONCAT_STRING]!=(role==2?1u:0u)) return XR_XIR_BAD_STRUCTURE;
    if(role==4) return function->instruction_count==2 && !function->operand_count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    bool projected=counts[XR_XIR_PLACE_WRITE]==1 && counts[XR_XIR_OBJECT_PLACE]==1 && counts[XR_XIR_FIELD_PLACE]==1 && !counts[XR_XIR_CLASS_SET];
    bool direct=counts[XR_XIR_CLASS_SET]==1 && !counts[XR_XIR_PLACE_WRITE] && !counts[XR_XIR_OBJECT_PLACE] && !counts[XR_XIR_FIELD_PLACE];
    return (projected || direct) && !function->operand_count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus library_builder_function(const XrXirCompileContext *context,const XrXirModule *module,
    uint32_t function,uint32_t declaration,uint32_t *seen) {
    const XrXirFunctionIdentity *identity=&module->declarations->functions[function];
    const XrXirFunction *body=&module->functions[function];
    const char *names[]={"init","append","toString","clear"};unsigned role=0;
    for(unsigned i=0;i<4;++i) {
        bool equal=false;XrXirStatus status=library_prelude_literal(context,(XrXirLiteral){body->name,body->name_length},names[i],&equal);
        if(status!=XR_XIR_OK) return status;
        if(equal) role=1u<<i;
    }
    if(!role || (*seen&role) || identity->nominal_owner!=declaration+1 || identity->exported!=1 ||
        identity->member_access || identity->cleanup_owner || identity->test_role || identity->test_timeout_seconds ||
        identity->method_kind!=(uint32_t)(role==1?XR_XIR_CONSTRUCTOR:XR_XIR_READ_METHOD) ||
        identity->promises!=(uint32_t)(role==1?XR_XIR_FUNCTION_NO_SUSPEND:0)) return XR_XIR_BAD_STRUCTURE;
    if(body->parameter_count!=(role==1?0u:role==2?2u:1u) ||
        (role!=1 && !library_builder_type(module,body->parameters[0],declaration)) ||
        (role==2 && body->parameters[1]!=XR_XIR_STRING) ||
        (role==4?body->result!=XR_XIR_STRING:!library_builder_type(module,body->result,declaration))) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=library_builder_body(context,module,body,role);
    if(status==XR_XIR_OK) *seen|=role;
    return status;
}
