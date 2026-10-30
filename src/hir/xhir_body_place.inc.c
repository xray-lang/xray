/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Places own the ordered root and projections; selectors are evaluated IDs. */
static XrXirStatus body_place_failure(const XrHirPlaceInput *input,const char **reason,const char *text) {
    if(reason)*reason=input->failure?input->failure:text;return XR_XIR_BAD_TYPE;
}
static XrXirStatus body_place_root(XrHirBody *body,const XrHirPlaceInput *input,HirBodyPlace *row,
    XrHirPlaceAction *action,const char **reason,XrCompileRequestControl *control) {
    if(!input->root.owner)return body_place_failure(input,reason,input->reference?
        "projected ref requires an actual stored root":"value mutation requires a mutable named root");
    if(input->root.owner!=body||input->root.index>=body->binding_count)return XR_XIR_BAD_STRUCTURE;
    if(!body->bindings[input->root.index].materialized)return XR_XIR_BAD_STRUCTURE;
    XrHirBindingInput decl=body->bindings[input->root.index].declaration;
    if(decl.storage==XR_HIR_BODY_LOCAL||decl.storage==XR_HIR_BODY_UNIT_LOCAL) {
        XrXirStatus status=body_scope_contains(body,decl.scope,input->scope,control);
        if(status!=XR_XIR_OK)return status;
        if(decl.origin.function!=input->origin.function||decl.origin.module!=input->origin.module)return XR_XIR_BAD_STRUCTURE;
    }
    if(input->kind!=XR_HIR_PLACE_BORROW_ROOT) {
        XrXirStatus status=body_narrowing(body,decl.logical,input->type,control);if(status!=XR_XIR_OK)return status;
    }
    bool stored=decl.storage==XR_HIR_BODY_LOCAL||decl.storage==XR_HIR_BODY_SLOT;
    if(input->kind==XR_HIR_PLACE_BORROW_ROOT) {
        if(!stored||(decl.storage==XR_HIR_BODY_LOCAL&&!decl.rebindable))
            return body_place_failure(input,reason,"Array read requires a stored root");
        row->root=input->root;row->type=decl.logical;row->writable=decl.rebindable;
        action->type=decl.logical;action->location=body->bindings[input->root.index].location;
        action->operation=decl.rebindable?XR_XIR_CELL_PLACE:XR_XIR_SLOT_PLACE;action->immediate=decl.rebindable?0:action->location;return XR_XIR_OK;
    }
    bool object=stored&&xr_xir_type_is_class(body->types,input->type);
    if(input->reference&&(input->type!=decl.logical||decl.construction||!stored||
        (!xr_xir_type_is_struct(body->types,input->type)&&!xr_xir_type_is_array(body->types,input->type)&&
        !xr_xir_type_is_map(body->types,input->type)&&!object)))
        return body_place_failure(input,reason,"projected ref requires an actual stored root");
    if(!stored||(!decl.rebindable&&!object))return body_place_failure(input,reason,"value mutation requires a mutable named root");
    if(object&&decl.construction)return body_place_failure(input,reason,"class constructor field mutation is not implemented in XIR");
    row->root=input->root;row->type=input->type;row->object_root=object;row->reference=input->reference;row->writable=true;
    action->type=row->type;action->object_root=object;action->operation=object?XR_XIR_OBJECT_PLACE:XR_XIR_CELL_PLACE;
    return XR_XIR_OK;
}
static XrXirStatus body_place_field(XrHirBody *body,const XrHirPlaceInput *input,HirBodyPlace *row,
    XrHirPlaceAction *action,const char **reason,XrCompileRequestControl *control) {
    const XrXirTypeNode *type=xr_xir_type_node(body->types,row->type);
    if(xr_xir_type_is_nullable(body->types,row->type))return body_place_failure(input,reason,
        "receiver may be null: check it for null, unwrap it with !, or use ?.");
    if(!type||(!xr_xir_type_is_struct(body->types,row->type)&&!xr_xir_type_is_class(body->types,row->type)))
        return body_place_failure(input,reason,"member receiver is not a nominal value");
    if(!body->types->nominals||type->nominal.declaration>=body->types->nominals->count)return XR_XIR_BAD_STRUCTURE;
    const XrXirNominalDeclaration *decl=&body->types->nominals->declarations[type->nominal.declaration];
    if(input->field==UINT32_MAX){action->type=row->type;action->declared=row->type;return XR_XIR_OK;}
    if(input->field>=decl->field_count)return XR_XIR_BAD_STRUCTURE;
    XrXirNominalField field=decl->fields[input->field];
    if(input->kind==XR_HIR_PLACE_SELECT) {
        if(input->type!=field.type)return XR_XIR_BAD_TYPE;
    } else {
        XrXirStatus status=body_substitution(body,type->nominal.arguments,type->nominal.argument_count,
            field.type,input->type,control);if(status!=XR_XIR_OK)return status;
    }
    uint32_t owner=((HirBodyScope *)body->scopes.items)[input->scope.index].nominal_owner;
    bool own=owner==type->nominal.declaration+1;
    if((!own&&(field.flags&(XR_XIR_FIELD_PRIVATE|XR_XIR_FIELD_PROTECTED)))||
        ((input->access==1||input->access==3)&&!(field.flags&XR_XIR_FIELD_MUTABLE))||(input->access==2&&!own))
        return body_place_failure(input,reason,"field access is not permitted");
    if(row->reference&&!xr_xir_type_is_struct(body->types,row->type)&&!(row->object_root&&!row->depth))
        return body_place_failure(input,reason,"projected ref cannot cross a Class identity");
    row->has_field=true;row->field_mutable=(field.flags&XR_XIR_FIELD_MUTABLE)!=0;row->initializing=input->access==2&&own;
    action->declared=field.type;action->field=input->field;action->immediate=input->field;action->location=row->value;action->type=input->type;
    action->operation=XR_XIR_FIELD_PLACE;row->type=input->type;row->field=input->field;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_place(XrHirBody *body,const XrHirPlaceInput *input,
    XrCompileRequestControl *control,XrHirPlaceAction *output,const char **reason) {
    if(reason)*reason=NULL;
    if(!body||body->ended||!input||input->types!=body->types||!output||input->scope.owner!=body||input->scope.index>=body->scopes.count)
        return XR_XIR_BAD_STRUCTURE;
    HirBodyPlace row={.scope=input->scope,.origin=input->origin,.type=input->type,.parent=input->parent};
    XrHirPlaceAction action={.type=input->type};XrXirStatus status=body_poll(control);
    if(status!=XR_XIR_OK)return status;
    HirBodyScope scope=((HirBodyScope *)body->scopes.items)[input->scope.index];
    if(scope.origin.function!=input->origin.function||scope.origin.module!=input->origin.module)return XR_XIR_BAD_STRUCTURE;
    if(input->parent.owner) {
        if(input->parent.owner!=body||input->parent.index>=body->places.count)return XR_XIR_BAD_STRUCTURE;
        row=((HirBodyPlace *)body->places.items)[input->parent.index];
        if(row.origin.function!=input->origin.function)return XR_XIR_BAD_STRUCTURE;
        status=body_scope_contains(body,row.scope,input->scope,control);if(status!=XR_XIR_OK)return status;
        if(row.terminal||!row.materialized)return XR_XIR_BAD_STRUCTURE;
        row.parent=input->parent;row.origin=input->origin;row.scope=input->scope;
    }
    switch(input->kind) {
    case XR_HIR_PLACE_ROOT:case XR_HIR_PLACE_BORROW_ROOT:status=body_place_root(body,input,&row,&action,reason,control);break;
    case XR_HIR_PLACE_ROUTE:
        action.object_root=input->type==XR_XIR_UNIT||xr_xir_type_is_class(body->types,input->type);break;
    case XR_HIR_PLACE_TEMPORARY:
        if(!xr_xir_type_is_class(body->types,input->type))return body_place_failure(input,reason,"temporary member mutation requires a class identity");
        row.object_root=true;row.writable=true;row.value=input->base;action.object_root=true;break;
    case XR_HIR_PLACE_FIELD_BASE:
        if(row.reference&&!xr_xir_type_is_struct(body->types,row.type)&&!(row.object_root&&!row.depth))
            return body_place_failure(input,reason,"projected ref cannot cross a Class identity");
        action.type=row.type;break;
    case XR_HIR_PLACE_FIELD:case XR_HIR_PLACE_SELECT:case XR_HIR_PLACE_RESOLVED_FIELD:case XR_HIR_PLACE_MEMBER:case XR_HIR_PLACE_CONSTRUCTOR:
        if(input->kind==XR_HIR_PLACE_SELECT||input->kind==XR_HIR_PLACE_RESOLVED_FIELD||input->kind==XR_HIR_PLACE_CONSTRUCTOR)row.type=input->value;
        status=body_place_field(body,input,&row,&action,reason,control);
        if(status==XR_XIR_OK&&(input->kind==XR_HIR_PLACE_SELECT||input->kind==XR_HIR_PLACE_RESOLVED_FIELD))action.operation=XR_XIR_INVALID;
        if(status==XR_XIR_OK&&(input->kind==XR_HIR_PLACE_MEMBER||input->kind==XR_HIR_PLACE_CONSTRUCTOR)) {
            row.identity=input->kind==XR_HIR_PLACE_MEMBER;row.constructor=input->kind==XR_HIR_PLACE_CONSTRUCTOR;
            row.writable=true;action.operation=XR_XIR_INVALID;
        }
        break;
    case XR_HIR_PLACE_HINT_SHAPE:case XR_HIR_PLACE_ARRAY_SHAPE:case XR_HIR_PLACE_SHAPE:case XR_HIR_PLACE_INDEX:case XR_HIR_PLACE_INDEX_READ:case XR_HIR_PLACE_INDEX_WRITE: {
        bool map=xr_xir_type_is_map(body->types,row.type);
        if(input->kind==XR_HIR_PLACE_ARRAY_SHAPE&&!xr_xir_type_is_array(body->types,row.type))
            return body_place_failure(input,reason,"indexed receiver is not an Array");
        if(!map&&!xr_xir_type_is_array(body->types,row.type)) {
            status=body_place_failure(input,reason,"indexed place requires an Array or Map");break;
        }
        action.key=map?xr_xir_map_key(body->types,row.type):XR_XIR_I64;
        action.type=map?xr_xir_map_element(body->types,row.type):xr_xir_array_element(body->types,row.type);
        if(input->kind!=XR_HIR_PLACE_SHAPE&&input->kind!=XR_HIR_PLACE_ARRAY_SHAPE&&input->kind!=XR_HIR_PLACE_HINT_SHAPE&&input->key!=action.key){status=body_place_failure(input,reason,"indexed place requires its exact key type");break;}
        if(input->kind==XR_HIR_PLACE_INDEX_WRITE&&(!row.writable||input->value!=action.type)) {
            status=body_place_failure(input,reason,"indexed assignment requires exact key and element types");break;
        }
        row.type=action.type;row.key=action.key;row.selector=input->selector;
        action.operation=input->kind==XR_HIR_PLACE_INDEX_READ?(map?XR_XIR_MAP_INDEX:XR_XIR_ARRAY_GET):
            input->kind==XR_HIR_PLACE_INDEX_WRITE?(map?XR_XIR_MAP_SET:XR_XIR_ARRAY_SET):XR_XIR_INDEX_PLACE;
        if(input->kind==XR_HIR_PLACE_INDEX_WRITE){action.declared=action.type;action.type=XR_XIR_UNIT;}
        break;
    }
    case XR_HIR_PLACE_READ:action.type=row.type;action.field=row.field;action.location=row.value;action.immediate=row.identity?row.field:0;
        action.operation=row.constructor?XR_XIR_INVALID:row.identity?XR_XIR_CLASS_GET:XR_XIR_PLACE_READ;break;
    case XR_HIR_PLACE_WRITE:
        if(!row.writable||(row.has_field&&!row.field_mutable&&!row.initializing))
            return body_place_failure(input,reason,"field access is not permitted");
        if(input->value!=row.type){status=body_place_failure(input,reason,"field assignment type mismatch");break;}
        action.type=XR_XIR_UNIT;action.field=row.field;action.location=row.value;action.immediate=row.identity?row.field:0;
        action.operation=row.constructor?XR_XIR_INVALID:row.identity?XR_XIR_CLASS_SET:XR_XIR_PLACE_WRITE;break;
    case XR_HIR_PLACE_REF_LEAF:case XR_HIR_PLACE_REFERENCE:
        if(row.type==XR_XIR_UNIT){status=body_place_failure(input,reason,"projected ref requires an admitted stored leaf type");break;}
        if(xr_xir_type_is_class(body->types,row.type)||xr_xir_type_is_cell(body->types,row.type)) {
            status=body_place_failure(input,reason,"projected Class ref requires its own object borrow owner");break;
        }
        if(input->kind==XR_HIR_PLACE_REF_LEAF){action.type=row.type;break;}
        if(!xr_xir_type_is_cell(body->types,input->type)||xr_xir_cell_element(body->types,input->type)!=row.type)return XR_XIR_BAD_STRUCTURE;
        action.type=input->type;action.operation=XR_XIR_CELL_PROJECT;break;
    default:return XR_XIR_BAD_STRUCTURE;
    }
    if(status!=XR_XIR_OK)return status;
    if(input->kind==XR_HIR_PLACE_HINT_SHAPE){*output=action;return body_poll(control);}
    uint32_t parent_value=input->parent.owner?row.value:input->base;
    bool carrier=input->kind==XR_HIR_PLACE_TEMPORARY||input->kind==XR_HIR_PLACE_MEMBER||input->kind==XR_HIR_PLACE_CONSTRUCTOR;
    row.materialized=carrier;row.value=carrier?parent_value:0;
    row.terminal=input->kind==XR_HIR_PLACE_READ||input->kind==XR_HIR_PLACE_WRITE||input->kind==XR_HIR_PLACE_REFERENCE;
    status=body_place_reserve(body,control);if(status!=XR_XIR_OK)return status;
    body->uses[body->count]=(HirBodyUse){.origin=input->origin,
        .action={.source=input->value,.target=action.type,.operation=action.operation}};
    action.use=(XrHirBodyUse){body,body->count++};
    HirBodyUse *record=&body->uses[action.use.index];record->action.operation=action.operation;
    if(input->kind==XR_HIR_PLACE_ROOT||input->kind==XR_HIR_PLACE_BORROW_ROOT) {
        record->binding=input->root;
        HirBodyBinding binding=body->bindings[input->root.index];
        if(action.operation==XR_XIR_CELL_PLACE&&binding.declaration.storage==XR_HIR_BODY_LOCAL) {
            record->has_operand=true;record->source_operand=binding.location;
        } else if(action.operation==XR_XIR_SLOT_PLACE) {
            record->has_immediate=true;record->expected_immediate=binding.location;
        }
    }
    record->required_instruction=action.operation!=XR_XIR_INVALID&&input->kind!=XR_HIR_PLACE_SELECT&&input->kind!=XR_HIR_PLACE_SHAPE&&input->kind!=XR_HIR_PLACE_ARRAY_SHAPE;
    record->index_group=input->kind==XR_HIR_PLACE_INDEX_WRITE;
    if(record->index_group) {
        record->expected_arguments[0]=parent_value;record->expected_arguments[1]=input->selector;
        record->expected_arguments[2]=input->value_id;
    }
    if((input->parent.owner||input->kind==XR_HIR_PLACE_INDEX_READ)&&record->required_instruction&&!record->index_group) {
        record->has_operand=true;record->source_operand=parent_value;
    }
    record->has_selector=input->kind==XR_HIR_PLACE_INDEX||input->kind==XR_HIR_PLACE_INDEX_READ;record->selector=input->selector;
    record->has_field=input->kind==XR_HIR_PLACE_FIELD||((input->kind==XR_HIR_PLACE_READ||input->kind==XR_HIR_PLACE_WRITE)&&row.identity);record->field=(input->kind==XR_HIR_PLACE_READ||input->kind==XR_HIR_PLACE_WRITE)?row.field:input->field;
    row.depth+=input->parent.owner?1u:0u;
    ((HirBodyPlace *)body->places.items)[body->places.count]=row;
    action.place=(XrHirBodyPlace){body,body->places.count++};
    if(record->required_instruction)record->place=action.place;if(input->kind!=XR_HIR_PLACE_ROUTE)action.object_root=row.object_root;*output=action;return XR_XIR_OK;
}
