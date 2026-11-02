/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_stage_verify.inc.c - Constructor-owned field transaction boundaries
 *
 * KEY CONCEPT:
 *   Only an exact wrapper or its direct child's transaction may call a field
 *   helper. An inherited class cannot omit the parent transaction entirely.
 */
#include "xxir_operand_roles.h"

typedef struct ClassStagePair {uint32_t helper,constructor,owner;} ClassStagePair;
static uint32_t class_stage_parent(const XrXirNominalTable *table,uint32_t owner) {
    return table->declarations?table->declarations[owner].parent:table->identities[owner].parent;
}
static XrXirStatus class_stage_find(const XrXirModule *module,uint32_t owner,
    uint32_t kind,uint32_t *output,XrCompileRequestControl *control) {
    const XrXirDeclarations *d=module->declarations;uint32_t selected=UINT32_MAX,steps=0;
    if(!d || (module->function_count && (!module->functions || !d->functions)))return XR_XIR_BAD_STRUCTURE;
    for(uint32_t f=0;f<module->function_count;++f) {
        XrXirStatus status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        if(d->functions[f].nominal_owner!=owner+1 || d->functions[f].method_kind!=kind)continue;
        if(selected!=UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
        selected=f;
    }
    if(selected==UINT32_MAX)return XR_XIR_BAD_TYPE;
    *output=selected;return XR_XIR_OK;
}
static XrXirStatus class_stage_pair(const XrXirModule *module,uint32_t helper,
    ClassStagePair *output,XrCompileRequestControl *control) {
    XrXirStatus status=xir_compile_class_stage_signature(module,helper,control);
    if(status!=XR_XIR_OK)return status;
    const XrXirDeclarations *d=module->declarations;
    uint32_t owner=d->functions[helper].nominal_owner-1,found=0,constructor=0;
    status=class_stage_find(module,owner,XR_XIR_CONSTRUCTOR_FIELDS,&found,control);
    if(status!=XR_XIR_OK)return status;
    if(found!=helper)return XR_XIR_BAD_STRUCTURE;
    status=class_stage_find(module,owner,XR_XIR_CONSTRUCTOR,&constructor,control);
    if(status!=XR_XIR_OK)return status;
    const XrXirFunctionIdentity *identity=&d->functions[constructor];
    const XrXirNominalTable *table=module->types->nominals;
    if(!!table->declarations==!!table->identities)return XR_XIR_BAD_STRUCTURE;
    XrXirLiteral name=table->declarations?table->declarations[owner].module:table->identities[owner].module;
    uint32_t exported=table->declarations?table->declarations[owner].exported:table->identities[owner].exported;
    if(identity->module>=d->module_count || !d->modules || identity->cleanup_owner || identity->test_role ||
        identity->native_callable || identity->promises!=XR_XIR_FUNCTION_NO_SUSPEND ||
        identity->exported!=exported || !name.bytes ||
        d->modules[identity->module].name_length!=name.length || !d->modules[identity->module].name ||
        memcmp(name.bytes,d->modules[identity->module].name,name.length))return XR_XIR_BAD_TYPE;
    *output=(ClassStagePair){helper,constructor,owner};return XR_XIR_OK;
}
static bool class_stage_plain(const XrXirInstruction *op) {
    return !op->targets[0] && !op->targets[1] && !op->type_arguments[0] && !op->type_arguments[1];
}
static XrXirStatus class_stage_wrapper(const XrXirModule *module,ClassStagePair pair,
    XrCompileRequestControl *control) {
    const XrXirFunction *body=&module->functions[pair.constructor],*helper=&module->functions[pair.helper];
    const XrXirTypeNode *subject=xr_xir_type_node(module->types,body->result);
    uint32_t fields=subject->nominal.field_count,parameters=body->parameter_count,steps=0;
    uint64_t instructions=(uint64_t)fields+3,operands=(uint64_t)parameters+fields;
    if(instructions>UINT32_MAX || operands>UINT32_MAX || instructions+parameters>UINT32_MAX)return XR_XIR_BUDGET;
    if(helper->error_type!=XR_XIR_NEVER)return XR_XIR_UNSUPPORTED;
    if(body->block_count!=1 || !body->blocks || body->blocks[0].first || body->blocks[0].count!=instructions ||
        body->blocks[0].panic || body->blocks[0].frontier || body->instruction_count!=instructions ||
        !body->instructions || body->operand_count!=operands || (operands && !body->operands))return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *call=body->instructions;
    if(call->op!=XR_XIR_CALL || call->type!=helper->result || call->immediate!=pair.helper ||
        call->args[0] || call->args[1]!=parameters || !class_stage_plain(call))return XR_XIR_BAD_STRUCTURE;
    for(uint32_t p=0;p<parameters;++p) {
        XrXirStatus status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        if(body->operands[p]!=p)return XR_XIR_BAD_STRUCTURE;
    }
    for(uint32_t f=0;f<fields;++f) {
        XrXirStatus status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        const XrXirInstruction *field=&body->instructions[f+1];
        if(field->op!=XR_XIR_TUPLE_FIELD || field->type!=subject->nominal.fields[f] ||
            field->args[0]!=parameters || field->args[1] || field->immediate!=f || !class_stage_plain(field) ||
            body->operands[parameters+f]!=(uint64_t)parameters+f+1)return XR_XIR_BAD_STRUCTURE;
    }
    const XrXirInstruction *construct=&body->instructions[fields+1],*end=construct+1;
    if(construct->op!=XR_XIR_CLASS_NEW || construct->type!=body->result || construct->args[0]!=parameters ||
        construct->args[1]!=fields || construct->immediate || !class_stage_plain(construct) ||
        end->op!=XR_XIR_RETURN || end->type!=XR_XIR_UNIT || end->args[0]!=(uint64_t)parameters+fields+1 ||
        end->args[1] || end->immediate || !class_stage_plain(end))return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}
static XrXirStatus class_stage_prefix(const XrXirModule *module,ClassStagePair child,
    ClassStagePair parent,uint32_t call,XrCompileRequestControl *control) {
    const XrXirFunction *body=&module->functions[child.helper];
    const XrXirTypeNode *derived=xr_xir_type_node(module->types,module->functions[child.constructor].result);
    const XrXirTypeNode *base=xr_xir_type_node(module->types,module->functions[parent.constructor].result);
    uint32_t fields=base->nominal.field_count,slots=0,steps=0;
    if(!fields)return XR_XIR_OK;
    if(fields>derived->nominal.field_count || (uint64_t)call+1+2*(uint64_t)fields>=body->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirNominalTable *table=module->types->nominals;
    uint32_t declared=table->declarations?table->declarations[child.owner].field_count:
        table->identities[child.owner].field_count;
    if(declared!=derived->nominal.field_count || (declared && !(table->declarations?
        (const void *)table->declarations[child.owner].fields:(const void *)table->identities[child.owner].fields)))
        return XR_XIR_BAD_STRUCTURE;
    for(uint32_t i=0;i<call;++i) {
        XrXirStatus status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        const XrXirInstruction *op=&body->instructions[i];
        if(op->op!=XR_XIR_LOCAL_UNINIT)continue;
        if(slots>=derived->nominal.field_count)return XR_XIR_UNSUPPORTED;
        uint32_t flags=table->declarations?table->declarations[child.owner].fields[slots].flags:
            table->identities[child.owner].fields[slots].flags;
        if(op->type!=derived->nominal.fields[slots] || op->immediate!=((flags&XR_XIR_FIELD_MUTABLE)?0:1))
            return XR_XIR_BAD_STRUCTURE;
        ++slots;
    }
    if(slots!=derived->nominal.field_count)return XR_XIR_BAD_STRUCTURE;
    uint32_t place=0;
    for(uint32_t f=0;f<fields;++f) {
        XrXirStatus status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        while(place<call && body->instructions[place].op!=XR_XIR_LOCAL_UNINIT) {
            status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
            ++place;
        }
        if(place>=call || base->nominal.fields[f]!=derived->nominal.fields[f])return XR_XIR_BAD_TYPE;
        uint32_t projection=call+1+2*f;const XrXirInstruction *field=&body->instructions[projection],*write=field+1;
        if(field->op!=XR_XIR_TUPLE_FIELD || field->type!=base->nominal.fields[f] ||
            field->args[0]!=(uint64_t)body->parameter_count+call || field->args[1] ||
            field->immediate!=f || !class_stage_plain(field) || write->op!=XR_XIR_LOCAL_WRITE ||
            write->type!=XR_XIR_UNIT || write->args[0]!=(uint64_t)body->parameter_count+place ||
            write->args[1]!=(uint64_t)body->parameter_count+projection || write->immediate ||
            !class_stage_plain(write))return XR_XIR_BAD_STRUCTURE;
        ++place;
    }
    return XR_XIR_OK;
}
/* A single block makes the parent call's exactly-once normal-path property
 * explicit. More involved field transactions need their own CFG proof. */
static XrXirStatus class_stage_parent_body(const XrXirModule *module,ClassStagePair pair,
    ClassStagePair parent,XrCompileRequestControl *control) {
    const XrXirFunction *body=&module->functions[pair.helper];
    if(body->block_count!=1 || !body->blocks || body->blocks[0].first ||
        body->blocks[0].count!=body->instruction_count || body->blocks[0].panic || body->blocks[0].frontier)
        return XR_XIR_UNSUPPORTED;
    if(!body->instruction_count || !body->instructions)return XR_XIR_BAD_STRUCTURE;
    if((uint64_t)body->parameter_count+body->instruction_count>UINT32_MAX)return XR_XIR_BUDGET;
    uint32_t calls=0,call=0,steps=0;
    for(uint32_t i=0;i<body->instruction_count;++i) {
        XrXirStatus status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        const XrXirInstruction *op=&body->instructions[i];
        if(op->op==XR_XIR_JUMP || op->op==XR_XIR_BRANCH || op->op==XR_XIR_INVOKE ||
            op->op==XR_XIR_INVOKE_INDIRECT || op->op==XR_XIR_CLEANUP_REGISTER ||
            op->op==XR_XIR_CLEANUP_LEAVE || op->op==XR_XIR_CLEANUP_ERROR)return XR_XIR_UNSUPPORTED;
        if(op->op==XR_XIR_RETURN && (i+1!=body->instruction_count || calls!=1))return XR_XIR_BAD_TYPE;
        if(!calls && (op->op==XR_XIR_LOCAL_WRITE || op->op==XR_XIR_CELL_LOCAL_WRITE))return XR_XIR_UNSUPPORTED;
        if(xr_xir_op_references_function(op->op) && op->immediate==parent.helper) {
            if(op->op!=XR_XIR_CALL || ++calls!=1)return XR_XIR_BAD_TYPE;
            call=i;
        }
    }
    if(calls!=1 || body->instructions[body->instruction_count-1].op!=XR_XIR_RETURN)return XR_XIR_BAD_TYPE;
    return class_stage_prefix(module,pair,parent,call,control);
}
static XrXirStatus class_stage_arguments(const XrXirModule *module,uint32_t caller,
    uint32_t instruction,uint32_t helper,XrCompileRequestControl *control) {
    const XrXirFunction *body=&module->functions[caller],*target=&module->functions[helper];
    const XrXirInstruction *op=&body->instructions[instruction];uint32_t steps=0;
    if(op->type!=target->result || op->type_arguments[0] || op->type_arguments[1] ||
        op->args[1]!=target->parameter_count || op->args[0]>body->operand_count ||
        op->args[1]>body->operand_count-op->args[0] || (op->args[1] && !body->operands) ||
        (body->parameter_count && !body->parameters))return XR_XIR_BAD_STRUCTURE;
    for(uint32_t p=0;p<op->args[1];++p) {
        XrXirStatus status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        uint32_t value=body->operands[op->args[0]+p];
        if(value>=(uint64_t)body->parameter_count+instruction)return XR_XIR_BAD_STRUCTURE;
        if(xr_xir_operand_type(body,value)!=target->parameters[p])return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
static XrXirStatus class_stage_use(const XrXirModule *module,uint32_t caller,uint32_t instruction,
    XrCompileRequestControl *control) {
    if(!module || !module->declarations || !module->declarations->functions || !module->functions ||
        caller>=module->function_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *body=&module->functions[caller];
    if(!body->instructions || instruction>=body->instruction_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&body->instructions[instruction];
    if(op->immediate<0 || (uint64_t)op->immediate>=module->function_count)return XR_XIR_BAD_STRUCTURE;
    if(op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE)return XR_XIR_BAD_TYPE;
    ClassStagePair target={0};
    XrXirStatus status=class_stage_pair(module,(uint32_t)op->immediate,&target,control);
    if(status==XR_XIR_OK)status=class_stage_wrapper(module,target,control);
    if(status==XR_XIR_OK)status=class_stage_arguments(module,caller,instruction,target.helper,control);
    if(status!=XR_XIR_OK)return status;
    if(caller==target.constructor)return instruction==0?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    const XrXirFunctionIdentity *identity=&module->declarations->functions[caller];
    if(identity->method_kind!=XR_XIR_CONSTRUCTOR_FIELDS)return XR_XIR_BAD_TYPE;
    ClassStagePair child={0};status=class_stage_pair(module,caller,&child,control);
    if(status==XR_XIR_OK)status=class_stage_wrapper(module,child,control);
    if(status!=XR_XIR_OK)return status;
    const XrXirNominalTable *table=module->types->nominals;
    if(class_stage_parent(table,child.owner)!=target.owner+1)return XR_XIR_BAD_TYPE;
    const XrXirDeclarations *d=module->declarations;
    uint32_t from=identity->module,to=d->functions[target.constructor].module;
    if(!xr_xir_module_imports(d,from,to) || (!xr_xir_module_same_scope(d,from,to) &&
        !d->functions[target.constructor].exported))return XR_XIR_BAD_TYPE;
    return class_stage_parent_body(module,child,target,control);
}
static XrXirStatus class_stage_exposed(const XrXirModule *module,uint32_t helper,
    XrCompileRequestControl *control) {
    const XrXirDeclarations *d=module->declarations;uint32_t steps=0;
    if(module->linkage_kind==XR_XIR_PROGRAM && d->entry_function==helper)return XR_XIR_BAD_TYPE;
    for(uint32_t m=0;m<d->module_count;++m) {
        XrXirStatus status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        if(d->modules[m].initializer==helper)return XR_XIR_BAD_TYPE;
    }
    if(module->defaults) {
        if(module->defaults->count && !module->defaults->records)return XR_XIR_BAD_STRUCTURE;
        for(uint32_t r=0;r<module->defaults->count;++r) {
            XrXirStatus status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
            if(module->defaults->records[r].function==helper || module->defaults->records[r].owner==helper)
                return XR_XIR_BAD_TYPE;
        }
    }
    if(d->implementations) {
        if(d->implementations->count && !d->implementations->records)return XR_XIR_BAD_STRUCTURE;
        for(uint32_t r=0;r<d->implementations->count;++r) {
            const XrXirImplementation *record=&d->implementations->records[r];
            if(record->binding_count && !record->bindings)return XR_XIR_BAD_STRUCTURE;
            for(uint32_t b=0;b<record->binding_count;++b) {
                XrXirStatus status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
                if(record->bindings[b].function==helper)return XR_XIR_BAD_TYPE;
            }
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus class_stage_derived(const XrXirModule *module,uint32_t owner,
    XrCompileRequestControl *control) {
    const XrXirNominalTable *table=module->types->nominals;uint32_t parent=class_stage_parent(table,owner),steps=0;
    for(uint32_t depth=0,ancestor=parent;ancestor;++depth) {
        XrXirStatus status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        if(depth>=table->count || ancestor>table->count || ancestor==owner+1)return XR_XIR_BAD_STRUCTURE;
        ancestor=class_stage_parent(table,ancestor-1);
    }
    uint32_t helper=0,parent_helper=0;
    XrXirStatus status=class_stage_find(module,owner,XR_XIR_CONSTRUCTOR_FIELDS,&helper,control);
    if(status==XR_XIR_OK)status=class_stage_find(module,parent-1,XR_XIR_CONSTRUCTOR_FIELDS,&parent_helper,control);
    ClassStagePair pair={0},base={0};
    if(status==XR_XIR_OK)status=class_stage_pair(module,helper,&pair,control);
    if(status==XR_XIR_OK)status=class_stage_pair(module,parent_helper,&base,control);
    if(status==XR_XIR_OK)status=class_stage_parent_body(module,pair,base,control);
    return status;
}
XR_FUNC XrXirStatus xir_compile_class_stage_verify(const XrXirModule *module,
    XrCompileRequestControl *control) {
    XrXirStatus status=xir_receive_poll(control);if(status!=XR_XIR_OK)return status;
    if(!module)return XR_XIR_BAD_STRUCTURE;
    const XrXirNominalTable *table=module->types?module->types->nominals:NULL;
    if(table && table->count && (!!table->declarations==!!table->identities))return XR_XIR_BAD_STRUCTURE;
    uint32_t steps=0;
    for(uint32_t n=0;table && n<table->count;++n) {
        status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        if(class_stage_parent(table,n)) {
            status=class_stage_derived(module,n,control);if(status!=XR_XIR_OK)return status;
        }
    }
    const XrXirDeclarations *d=module->declarations;
    if(!d || !d->functions)return XR_XIR_OK;
    if(module->function_count && !module->functions)return XR_XIR_BAD_STRUCTURE;
    for(uint32_t f=0;f<module->function_count;++f) {
        status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        if(d->functions[f].method_kind==XR_XIR_CONSTRUCTOR_FIELDS) {
            ClassStagePair pair={0};status=class_stage_pair(module,f,&pair,control);
            if(status==XR_XIR_OK)status=class_stage_wrapper(module,pair,control);
            if(status==XR_XIR_OK)status=class_stage_exposed(module,f,control);
            if(status!=XR_XIR_OK)return status;
        }
        const XrXirFunction *body=&module->functions[f];
        if(body->instruction_count && !body->instructions)return XR_XIR_BAD_STRUCTURE;
        for(uint32_t i=0;i<body->instruction_count;++i) {
            status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
            const XrXirInstruction *op=&body->instructions[i];
            if(!xr_xir_op_references_function(op->op) && op->op!=XR_XIR_CLASS_METHOD_REF)continue;
            if(op->immediate<0 || (uint64_t)op->immediate>=module->function_count)return XR_XIR_BAD_STRUCTURE;
            if(d->functions[op->immediate].method_kind!=XR_XIR_CONSTRUCTOR_FIELDS)continue;
            status=class_stage_use(module,f,i,control);if(status!=XR_XIR_OK)return status;
        }
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_compile_class_stage_call(const XrXirModule *module,
    uint32_t caller,uint32_t instruction,XrCompileRequestControl *control) {
    XrXirStatus status=xir_receive_poll(control);
    return status==XR_XIR_OK?class_stage_use(module,caller,instruction,control):status;
}
