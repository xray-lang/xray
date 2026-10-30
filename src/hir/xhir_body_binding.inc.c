/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Binding actions separate logical types from mutable physical storage. */
static bool body_binding_key_equal(XrHirBindingKey a,XrHirBindingKey b) {
    return a.module==b.module&&a.declaration==b.declaration&&a.node==b.node&&a.parent_declaration==b.parent_declaration&&a.function==b.function&&a.storage==b.storage;
}
static uint32_t body_binding_hash(XrHirBindingKey key) {
    uint32_t fields[]={key.module,key.declaration,key.node,key.parent_declaration,key.function,key.storage};return xr_hash_bytes(fields,sizeof(fields));
}
XR_FUNC XrXirStatus xr_hir_body_binding_find(const XrHirBody *body,XrHirBindingKey key,
    XrCompileRequestControl *control,XrHirBodyBinding *out,bool *found) {
    if(!body||body->ended||!out||!found)return XR_XIR_BAD_STRUCTURE;
    *found=false;if(!body->binding_capacity)return body_poll(control);
    uint32_t mask=body->binding_capacity*2-1,slot=body_binding_hash(key)&mask;
    for(uint32_t probe=0;probe<=mask;++probe) {
        if(!(probe%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        uint32_t value=body->binding_map[slot];if(!value)return XR_XIR_OK;
        if(body_binding_key_equal(body->bindings[value-1].declaration.key,key)){
            *out=(XrHirBodyBinding){body,value-1};*found=true;return XR_XIR_OK;
        }
        slot=(slot+1)&mask;
    }
    return XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus body_binding_reserve(XrHirBody *body,XrHirBindingKey key,
    XrCompileRequestControl *control,uint32_t *position) {
    if(body->binding_count<body->binding_capacity) {
        uint32_t slot=body_binding_hash(key)&(body->binding_capacity*2-1),probe=0;
        while(body->binding_map[slot]) {
            if(!(probe++%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
            slot=(slot+1)&(body->binding_capacity*2-1);
        }
        XrXirStatus status=body_poll(control);if(status==XR_XIR_OK)*position=slot;return status;
    }
    if(body->binding_capacity>=UINT32_MAX/4)return XR_XIR_OVERFLOW;
    uint32_t capacity=body->binding_capacity?body->binding_capacity*2:16,map_count=capacity*2;
    if(sizeof(HirBodyBinding)>SIZE_MAX/capacity||sizeof(uint32_t)>SIZE_MAX/map_count)return XR_XIR_OVERFLOW;
    HirBodyBinding *rows=NULL;uint32_t *map=NULL;
    XrXirStatus status=xir_compile_resource_status(xr_compile_resources_alloc(body->resources,
        (size_t)capacity*sizeof(*rows),(void **)&rows));
    if(status!=XR_XIR_OK)return status;
    status=body_bytes(rows,NULL,(size_t)capacity*sizeof(*rows),control);
    if(status==XR_XIR_OK)status=body_bytes(rows,body->bindings,(size_t)body->binding_count*sizeof(*rows),control);
    if(status==XR_XIR_OK)status=xir_compile_resource_status(xr_compile_resources_alloc(body->resources,
        (size_t)map_count*sizeof(*map),(void **)&map));
    if(status==XR_XIR_OK)status=body_bytes(map,NULL,(size_t)map_count*sizeof(*map),control);
    for(uint32_t i=0;status==XR_XIR_OK&&i<body->binding_count;++i) {
        if(!(i%64))status=body_poll(control);if(status!=XR_XIR_OK)break;
        if(!rows[i].is_declaration)continue;
        uint32_t slot=body_binding_hash(rows[i].declaration.key)&(map_count-1);
        while(map[slot]){status=body_poll(control);if(status!=XR_XIR_OK)break;slot=(slot+1)&(map_count-1);}
        if(status==XR_XIR_OK)map[slot]=i+1;
    }
    uint32_t position_value=body_binding_hash(key)&(map_count-1);
    while(status==XR_XIR_OK&&map[position_value]) {
        status=body_poll(control);position_value=(position_value+1)&(map_count-1);
    }
    if(status==XR_XIR_OK)status=body_poll(control);
    if(status!=XR_XIR_OK){xr_compile_resources_free(rows);xr_compile_resources_free(map);return status;}
    xr_compile_resources_free(body->bindings);xr_compile_resources_free(body->binding_map);
    body->bindings=rows;body->binding_map=map;body->binding_capacity=capacity;*position=position_value;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_binding_declare(XrHirBody *body,const XrHirBindingInput *input,
    XrCompileRequestControl *control,XrHirBodyBinding *out,const char **reason) {
    if(reason)*reason=NULL;
    if(!body||body->ended||!input||input->types!=body->types||!out||input->scope.owner!=body||input->scope.index>=body->scopes.count||
        (!input->name&&input->name_length)||input->name_length==SIZE_MAX)
        return XR_XIR_BAD_STRUCTURE;
    HirBodyScope scope=((HirBodyScope *)body->scopes.items)[input->scope.index];
    if(input->origin.module!=scope.origin.module||input->origin.function!=scope.origin.function||
        input->key.module!=input->origin.module||input->key.declaration!=input->origin.binding||
        input->key.node!=input->origin.node||input->key.function!=input->origin.function)return XR_XIR_BAD_STRUCTURE;
    bool slot_input=input->storage==XR_HIR_BODY_SLOT||input->storage==XR_HIR_BODY_UNIT_SLOT;
    if(slot_input) {
        if(input->key.storage!=input->location||!body->declarations.slots||!body->declarations.slot_count||
            !*body->declarations.slots||input->location>=*body->declarations.slot_count)return XR_XIR_BAD_STRUCTURE;
        XrXirSlot slot=(*body->declarations.slots)[input->location];
        if(slot.module!=input->origin.module||!!slot.mutable!=input->rebindable)return XR_XIR_BAD_STRUCTURE;
    } else if(input->key.storage!=UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
    if(input->storage==XR_HIR_BODY_UNIT_LOCAL&&(input->logical!=XR_XIR_UNIT||input->physical!=XR_XIR_UNIT||input->rebindable))
        return XR_XIR_BAD_STRUCTURE;
    if(input->has_annotation&&input->annotation!=input->logical) {
        if(reason)*reason="binding annotation mismatch";return XR_XIR_BAD_TYPE;
    }
    if(input->mode!=XR_PARAM_READ&&input->mode!=XR_PARAM_REF)return XR_XIR_BAD_TYPE;
    for(uint32_t write=0;write<2;++write) {
        uint32_t accessor=write?input->writer:input->reader;if(!accessor)continue;
        if(!slot_input||!body->declarations.functions||!*body->declarations.functions||
            accessor-1>=*body->declarations.function_count)return XR_XIR_BAD_STRUCTURE;
        XrXirFunction function=(*body->declarations.functions)[accessor-1];
        XrXirFunctionIdentity identity=(*body->declarations.identities)[accessor-1];
        uint32_t count=write&&input->logical!=XR_XIR_UNIT?1u:0u;
        if(identity.module!=input->origin.module||!identity.exported||function.parameter_count!=count||
            function.result!=(write?XR_XIR_UNIT:input->logical)||(count&&(!function.parameters||function.parameters[0]!=input->logical)))
            return XR_XIR_BAD_STRUCTURE;
    }
    if(input->rebindable&&!input->direct_borrowed&&
        (!xr_xir_type_is_cell(body->types,input->physical)||xr_xir_cell_element(body->types,input->physical)!=input->logical))
        return XR_XIR_BAD_STRUCTURE;
    if((!input->rebindable||input->direct_borrowed)&&input->physical!=input->logical)return XR_XIR_BAD_STRUCTURE;
    XrHirBodyBinding previous={0};
    bool found=false;XrXirStatus status=xr_hir_body_binding_find(body,input->key,control,&previous,&found);
    if(status!=XR_XIR_OK)return status;if(found)return XR_XIR_BAD_STRUCTURE;
    char *name=NULL;uint32_t slot=0;
    if(status==XR_XIR_OK)status=xir_compile_resource_status(xr_compile_resources_alloc(body->resources,
        input->name_length+1,(void **)&name));
    if(status==XR_XIR_OK)status=body_bytes(name,input->name,input->name_length,control);
    if(status==XR_XIR_OK){name[input->name_length]=0;status=body_binding_reserve(body,input->key,control,&slot);}
    if(status!=XR_XIR_OK){xr_compile_resources_free(name);return status;}
    HirBodyBinding row={.declaration=*input,.name=name,.location=input->location,.is_declaration=true};
    bool slot_binding=input->storage==XR_HIR_BODY_SLOT||input->storage==XR_HIR_BODY_UNIT_SLOT;
    if(slot_binding&&body->declarations.slots&&body->declarations.slot_count&&*body->declarations.slots&&input->location<*body->declarations.slot_count) {
        XrXirSlot canonical=(*body->declarations.slots)[input->location];
        row.materialized=canonical.type==input->physical&&canonical.module==input->origin.module&&!!canonical.mutable==input->rebindable;
    }row.declaration.name=name;row.declaration.types=NULL;
    body->bindings[body->binding_count]=row;
    body->binding_map[slot]=body->binding_count+1;*out=(XrHirBodyBinding){body,body->binding_count++};return XR_XIR_OK;
}
XR_FUNC bool xr_hir_body_binding_info(const XrHirBody *body,XrHirBodyBinding binding,XrHirBindingInput *output) {
    if(!body||binding.owner!=body||binding.index>=body->binding_count||!output)return false;
    *output=body->bindings[binding.index].declaration;return true;
}
XR_FUNC XrXirStatus xr_hir_body_binding_instance(XrHirBody *body,XrHirBodyBinding source,
    XrHirBodyScope scope,XrCompileRequestControl *control,XrHirBodyBinding *output) {
    if(!body||body->ended||source.owner!=body||source.index>=body->binding_count||scope.owner!=body||
        scope.index>=body->scopes.count||!output)return XR_XIR_BAD_STRUCTURE;
    HirBodyBinding row=body->bindings[source.index];
    HirBodyScope target=((HirBodyScope *)body->scopes.items)[scope.index];
    if(row.declaration.rebindable||row.declaration.origin.function!=target.origin.function||
        row.declaration.origin.module!=target.origin.module)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_scope_contains(body,row.declaration.scope,scope,control);if(status!=XR_XIR_OK)return status;
    uint32_t ignored=0;status=body_binding_reserve(body,row.declaration.key,control,&ignored);
    if(status!=XR_XIR_OK)return status;
    row.is_declaration=false;row.materialized=false;row.declaration.scope=scope;
    body->bindings[body->binding_count]=row;*output=(XrHirBodyBinding){body,body->binding_count++};return XR_XIR_OK;
}
static XrXirStatus body_binding_failure(const char **reason,const char *message) {
    if(reason)*reason=message;return XR_XIR_BAD_TYPE;
}
XR_FUNC XrXirStatus xr_hir_body_binding_use(XrHirBody *body,XrHirBodyBinding binding,
    const XrHirBindingUseInput *input,XrCompileRequestControl *control,XrHirBindingAction *output,const char **reason) {
    if(reason)*reason=NULL;
    if(!body||body->ended||binding.owner!=body||binding.index>=body->binding_count||!input||input->types!=body->types||!output)return XR_XIR_BAD_STRUCTURE;
    /* Snapshot before use-table growth; declaration storage is independent. */
    HirBodyBinding owned=body->bindings[binding.index];XrHirBindingInput decl=owned.declaration;
    bool local=decl.storage==XR_HIR_BODY_LOCAL||decl.storage==XR_HIR_BODY_UNIT_LOCAL,slot=decl.storage==XR_HIR_BODY_SLOT||decl.storage==XR_HIR_BODY_UNIT_SLOT;
    if(input->scope.owner!=body||input->scope.index>=body->scopes.count)return XR_XIR_BAD_STRUCTURE;
    HirBodyScope use_scope=((HirBodyScope *)body->scopes.items)[input->scope.index];
    if(use_scope.origin.function!=input->origin.function||use_scope.origin.module!=input->origin.module)return XR_XIR_BAD_STRUCTURE;
    if(local) {
        XrXirStatus status=body_scope_contains(body,decl.scope,input->scope,control);if(status!=XR_XIR_OK)return status;
        if(input->origin.function!=decl.origin.function)return XR_XIR_BAD_STRUCTURE;
    }
    bool initialization=input->kind==XR_HIR_BINDING_INITIALIZE||input->kind==XR_HIR_BINDING_PARAMETER||input->kind==XR_HIR_BINDING_CAPTURE;
    XrHirBindingAction action={.binding=binding,.logical=decl.logical,.physical=decl.physical,.location=owned.location};
    action.route=decl.storage==XR_HIR_BODY_UNIT_LOCAL?XR_HIR_BINDING_UNIT:
        decl.rebindable&&!decl.direct_borrowed?(local?XR_HIR_BINDING_LOCAL_CELL:XR_HIR_BINDING_SLOT_CELL):
        local?XR_HIR_BINDING_DIRECT:XR_HIR_BINDING_SLOT_VALUE;
    switch(input->kind) {
    case XR_HIR_BINDING_CAPTURE:
        if(input->value!=decl.physical)return XR_XIR_BAD_STRUCTURE;break;
    case XR_HIR_BINDING_PARAMETER:
        if(!decl.is_parameter)return XR_XIR_BAD_STRUCTURE;
        if((decl.mode==XR_PARAM_REF)!=xr_xir_type_is_cell(body->types,decl.physical))return XR_XIR_BAD_STRUCTURE;
        break;
    case XR_HIR_BINDING_INITIALIZE:
        if(input->value!=decl.logical)return body_binding_failure(reason,input->type_failure?input->type_failure:"binding annotation mismatch");
        action.operation=decl.rebindable&&!decl.direct_borrowed?XR_XIR_CELL_NEW:XR_XIR_INVALID;
        break;
    case XR_HIR_BINDING_READ:
        if(decl.rebindable&&!decl.direct_borrowed&&
            (decl.construction||(!local&&!slot)))return body_binding_failure(reason,"mutable Cell owner required");
        if(action.route==XR_HIR_BINDING_SLOT_CELL&&(!xr_xir_type_is_cell(body->types,decl.physical)||
            xr_xir_cell_element(body->types,decl.physical)!=decl.logical))
            return body_binding_failure(reason,"module Cell element differs from its logical binding");
        action.operation=action.route==XR_HIR_BINDING_LOCAL_CELL||action.route==XR_HIR_BINDING_SLOT_CELL?
            XR_XIR_CELL_READ:action.route==XR_HIR_BINDING_SLOT_VALUE?XR_XIR_SLOT_LOAD:XR_XIR_INVALID;
        break;
    case XR_HIR_BINDING_ASSIGN:
        if(!decl.rebindable||(!local&&!slot))return body_binding_failure(reason,"assignment requires a mutable binding");
        break;
    case XR_HIR_BINDING_WRITE:
        if(decl.direct_borrowed)return body_binding_failure(reason,"direct borrowed local cannot be reassigned");
        if(input->value!=decl.logical)return body_binding_failure(reason,input->type_failure?input->type_failure:"binding write changes its logical type");
        if(!decl.rebindable||decl.construction||(!local&&!slot))return body_binding_failure(reason,"mutable Cell owner required");
        action.operation=XR_XIR_CELL_WRITE;break;
    case XR_HIR_BINDING_CELL:
        if(decl.direct_borrowed)return body_binding_failure(reason,"direct borrowed local cannot expose a Cell");
        if(!decl.rebindable||decl.construction)return body_binding_failure(reason,"mutable Cell owner required");
        if(!local&&!slot)return body_binding_failure(reason,"binding has no stored Cell owner");
        if(!local&&(!xr_xir_type_is_cell(body->types,decl.physical)||
            xr_xir_cell_element(body->types,decl.physical)!=decl.logical))
            return body_binding_failure(reason,"module Cell element differs from its logical binding");
        action.operation=slot?XR_XIR_SLOT_LOAD:XR_XIR_INVALID;break;
    case XR_HIR_BINDING_REFERENCE: {
        XrXirStatus status=body_narrowing(body,decl.logical,input->root_type,control);if(status!=XR_XIR_OK)return status;
        bool object=input->projected&&xr_xir_type_is_class(body->types,input->root_type);
        if((!decl.rebindable&&!object)||decl.construction||(!local&&!slot))
            return body_binding_failure(reason,input->type_failure?input->type_failure:"ref requires a mutable stored binding");
        break;
    }
    case XR_HIR_BINDING_COMPOUND:
        if(!decl.rebindable||(!local&&!slot)||(slot&&decl.logical==XR_XIR_UNIT))
            return body_binding_failure(reason,"compound assignment requires a mutable binding");
        break;
    case XR_HIR_BINDING_INCREMENT:
        if(!decl.rebindable||!xr_xir_type_is_integer(decl.logical)||(!local&&!slot))
            return body_binding_failure(reason,"increment requires a mutable integer binding");
        break;
    case XR_HIR_BINDING_PUBLISH:
        if(!slot||input->value!=decl.physical)return XR_XIR_BAD_STRUCTURE;
        action.operation=XR_XIR_SLOT_INIT;break;
    default:return XR_XIR_BAD_STRUCTURE;
    }
    if(!initialization&&!owned.materialized)return XR_XIR_BAD_STRUCTURE;
    bool history=slot&&decl.history&&input->origin.module!=decl.origin.module&&
        (input->kind==XR_HIR_BINDING_READ||input->kind==XR_HIR_BINDING_WRITE);
    if(history) {
        uint32_t accessor=input->kind==XR_HIR_BINDING_READ?decl.reader:decl.writer;
        if(!accessor)return body_binding_failure(reason,"foreign binding accessor is not in its exact received Checked owner");
        action.operation=XR_XIR_CALL;action.accessor=accessor-1;action.history=true;
    }
    XrHirBodyInput use={.origin=input->origin,.definition=decl.origin,.rule=XR_HIR_BODY_IDENTITY,
        .source=input->value,.target=decl.logical};
    XrXirStatus status=xr_hir_body_check(body,body->types,&use,control,&action.use,reason);
    if(status!=XR_XIR_OK)return status;
    body->uses[action.use.index].action.operation=action.operation;
    body->uses[action.use.index].action.target=input->kind==XR_HIR_BINDING_CELL||input->kind==XR_HIR_BINDING_PARAMETER||input->kind==XR_HIR_BINDING_CAPTURE||
        (input->kind==XR_HIR_BINDING_INITIALIZE&&action.operation==XR_XIR_CELL_NEW)?decl.physical:decl.logical;
    HirBodyUse *record=&body->uses[action.use.index];
    if(input->kind==XR_HIR_BINDING_WRITE||input->kind==XR_HIR_BINDING_PUBLISH)record->action.target=XR_XIR_UNIT;
    record->binding=binding;record->binding_kind=input->kind;record->initializes=initialization;
    record->required_instruction=action.operation!=XR_XIR_INVALID;
    record->foreign_slot=history;
    if(!record->foreign_slot&&(action.operation==XR_XIR_SLOT_LOAD||action.operation==XR_XIR_SLOT_INIT)) {
        record->has_immediate=true;record->expected_immediate=owned.location;
    }
    if(!slot&&(action.operation==XR_XIR_CELL_READ||action.operation==XR_XIR_CELL_WRITE)) {
        record->has_operand=true;record->source_operand=owned.location;
    }
    if(input->kind==XR_HIR_BINDING_WRITE) {
        record->has_second_operand=!record->foreign_slot;record->second_operand=decl.logical==XR_XIR_UNIT?0:input->value_id;
    }
    if(initialization&&action.operation==XR_XIR_CELL_NEW) {
        record->has_operand=true;record->source_operand=decl.logical==XR_XIR_UNIT?0:input->value_id;
    }
    if(input->kind==XR_HIR_BINDING_PUBLISH) {
        record->has_operand=true;record->source_operand=decl.physical==XR_XIR_UNIT?0:input->value_id;
    }
    if(input->kind==XR_HIR_BINDING_READ&&action.route==XR_HIR_BINDING_DIRECT) {
        record->has_source=decl.logical!=XR_XIR_UNIT;record->source_value=owned.location;
    }
    *output=action;return XR_XIR_OK;
}
