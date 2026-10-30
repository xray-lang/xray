/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Declaration facts and instantiated fields have one receiving body owner. */
#include "../xir/xxir_task_result.h"
static XrXirStatus body_nominal_failure(const char **reason,XrXirStatus status,const char *text) {
    if(reason)*reason=text;return status;
}
XR_FUNC XrXirStatus xr_hir_nominal_view(const XrXirTypes *types,XrXirType type,
    XrCompileRequestControl *control,XrHirNominalView *out,const char **reason) {
    if(reason)*reason=NULL;if(!types||!out)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
    const XrXirTypeNode *node=xr_xir_type_node(types,type);const XrXirNominalTable *table=types->nominals;
    if(!node||node->kind!=XR_XIR_TYPE_NOMINAL||!table||node->nominal.declaration>=table->count)
        return body_nominal_failure(reason,XR_XIR_BAD_TYPE,"nominal value requires an admitted declaration");
    uint32_t d=node->nominal.declaration;XrHirNominalView view={.owner=types,.type=type,.declaration=d,.argument_count=node->nominal.argument_count};
    uint32_t arity;
    if(table->declarations) {
        XrXirNominalDeclaration declaration=table->declarations[d];view.kind=(XrXirNominalKind)declaration.kind;
        view.flags=declaration.flags;view.field_count=declaration.field_count;view.variant_count=declaration.variant_count;arity=declaration.parameter_count;
        if((view.field_count&&!declaration.fields)||(view.variant_count&&!declaration.variants))return XR_XIR_BAD_STRUCTURE;
    } else if(table->identities) {
        XrXirNominalIdentity declaration=table->identities[d];view.kind=(XrXirNominalKind)declaration.kind;
        view.flags=declaration.flags;view.field_count=declaration.field_count;view.variant_count=declaration.variant_count;arity=declaration.arity;
        if((view.field_count&&!declaration.fields)||(view.variant_count&&!declaration.variants))return XR_XIR_BAD_STRUCTURE;
    } else return XR_XIR_BAD_STRUCTURE;
    if(view.argument_count!=arity||(arity&&!node->nominal.arguments))return XR_XIR_BAD_TYPE;
    if(view.kind!=XR_XIR_NOMINAL_STRUCT&&view.kind!=XR_XIR_NOMINAL_CLASS&&view.kind!=XR_XIR_NOMINAL_ENUM)return XR_XIR_BAD_STRUCTURE;
    status=body_poll(control);if(status==XR_XIR_OK)*out=view;return status;
}
XR_FUNC XrXirStatus xr_hir_nominal_variant(const XrXirTypes *types,XrXirType type,uint32_t variant,
    XrCompileRequestControl *control,XrXirNominalVariant *out,const char **reason) {
    XrHirNominalView view={0};if(!out)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=xr_hir_nominal_view(types,type,control,&view,reason);if(status!=XR_XIR_OK)return status;
    if(view.kind!=XR_XIR_NOMINAL_ENUM||variant>=view.variant_count)return XR_XIR_BAD_TYPE;
    const XrXirNominalTable *table=types->nominals;
    XrXirNominalVariant row=table->declarations?table->declarations[view.declaration].variants[variant]:table->identities[view.declaration].variants[variant];
    if(row.field_begin>view.field_count||row.field_count>view.field_count-row.field_begin)return XR_XIR_BAD_STRUCTURE;
    *out=row;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_nominal_field_type(const XrHirNominalPool *pool,XrXirType type,uint32_t field,
    XrXirType *out,const char **reason) {
    if(!pool||!pool->types||!out)return XR_XIR_BAD_STRUCTURE;XrHirNominalView view={0};
    XrXirStatus status=xr_hir_nominal_view(pool->types,type,pool->control,&view,reason);if(status!=XR_XIR_OK)return status;
    if(field>=view.field_count)return XR_XIR_BAD_STRUCTURE;
    XrXirNominalType nominal=xr_xir_type_node(pool->types,type)->nominal;
    const XrXirNominalTable *table=pool->types->nominals;XrXirType actual;
    if(!table->declarations) {
        if(nominal.field_count!=view.field_count||!nominal.fields)return XR_XIR_BAD_STRUCTURE;
        actual=nominal.fields[field];
    } else {
        if(!pool->compile||!pool->resolve)return XR_XIR_BAD_STRUCTURE;
        XrXirType declared=table->declarations[view.declaration].fields[field].type;
        status=pool->resolve(pool->resolve_owner,nominal.arguments,nominal.argument_count,declared,&actual);
        if(status!=XR_XIR_OK)return status;
        /* Resolution can move nodes. The actual type descriptor is stable. */
        nominal=xr_xir_type_node(pool->types,type)->nominal;
        status=xr_xir_compile_type_substitution_matches(pool->compile,pool->types,nominal.arguments,nominal.argument_count,declared,actual);
        if(status!=XR_XIR_OK)return status;
    }
    status=body_poll(pool->control);if(status==XR_XIR_OK)*out=actual;return status;
}
static void body_nominal_declaration_free(HirBodyNominalDeclaration *row) {
    if(row->fields)for(uint32_t f=0;f<row->count;++f)xr_compile_resources_free((void *)row->fields[f].name.bytes);
    if(row->variants)for(uint32_t v=0;v<row->variant_count;++v)xr_compile_resources_free((void *)row->variants[v].name.bytes);
    xr_compile_resources_free(row->fields);xr_compile_resources_free(row->variants);
    xr_compile_resources_free(row->field_origins);xr_compile_resources_free(row->field_functions);
    xr_compile_resources_free(row->field_producers);xr_compile_resources_free(row->field_sources);xr_compile_resources_free(row->foreign_fields);
}
static void body_nominal_free(XrHirBody *body) {
    for(uint32_t i=0;i<body->nominal_declarations.count;++i)body_nominal_declaration_free(&((HirBodyNominalDeclaration *)body->nominal_declarations.items)[i]);
    for(uint32_t i=0;i<body->nominals.count;++i) {
        HirBodyNominal *row=&((HirBodyNominal *)body->nominals.items)[i];xr_compile_resources_free(row->arguments);xr_compile_resources_free(row->fields);
    }
    for(uint32_t i=0;i<body->nominal_plans.count;++i) {
        HirBodyNominalPlan *row=&((HirBodyNominalPlan *)body->nominal_plans.items)[i];xr_compile_resources_free(row->values);xr_compile_resources_free(row->present);
    }
    for(uint32_t i=0;i<body->count;++i)xr_compile_resources_free(body->uses[i].nominal_values);
    xr_compile_resources_free(body->nominal_declarations.items);xr_compile_resources_free(body->nominals.items);
    xr_compile_resources_free(body->nominal_plans.items);xr_compile_resources_free(body->nominal_map);xr_compile_resources_free(body->nominal_available);
    xr_compile_resources_free(body->nominal_functions.items);
}
static XrXirStatus body_nominal_allocate(XrHirBody *body,size_t bytes,XrCompileRequestControl *control,void **out) {
    if(!bytes){*out=NULL;return body_poll(control);}void *value=NULL;
    XrXirStatus status=xir_compile_resource_status(xr_compile_resources_alloc(body->resources,bytes,&value));
    if(status==XR_XIR_OK)status=body_bytes(value,NULL,bytes,control);
    if(status!=XR_XIR_OK){xr_compile_resources_free(value);return status;}*out=value;return XR_XIR_OK;
}
static XrXirStatus body_nominal_name(XrHirBody *body,XrXirLiteral from,XrCompileRequestControl *control,XrXirLiteral *out) {
    if(from.length&&!from.bytes)return XR_XIR_BAD_STRUCTURE;char *text=NULL;
    size_t length=from.length;if(length==SIZE_MAX)return XR_XIR_OVERFLOW;
    XrXirStatus status=body_nominal_allocate(body,(size_t)from.length+1,control,(void **)&text);
    if(status==XR_XIR_OK)status=body_bytes(text,from.bytes,from.length,control);
    if(status!=XR_XIR_OK){xr_compile_resources_free(text);return status;}
    *out=(XrXirLiteral){text,from.length};return XR_XIR_OK;
}
static XrXirStatus body_nominal_same(XrXirLiteral a,XrXirLiteral b,XrCompileRequestControl *control,bool *same) {
    *same=false;if(a.length!=b.length)return body_poll(control);
    if(a.length&&(!a.bytes||!b.bytes))return XR_XIR_BAD_STRUCTURE;
    for(uint32_t at=0;at<a.length;) {
        XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
        uint32_t n=a.length-at<4096?a.length-at:4096;if(memcmp(a.bytes+at,b.bytes+at,n))return XR_XIR_OK;at+=n;
    }
    *same=true;return body_poll(control);
}
XR_FUNC XrXirStatus xr_hir_nominal_variant_find(const XrXirTypes *types,XrXirType type,XrXirLiteral name,
    XrCompileRequestControl *control,XrHirNominalSelection *out) {
    XrHirNominalView view={0};if(!out||!name.bytes)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=xr_hir_nominal_view(types,type,control,&view,NULL);if(status!=XR_XIR_OK)return status;
    if(view.kind!=XR_XIR_NOMINAL_ENUM)return XR_XIR_BAD_TYPE;
    const XrXirNominalTable *table=types->nominals;const XrXirNominalVariant *variants=
        table->declarations?table->declarations[view.declaration].variants:table->identities[view.declaration].variants;
    for(uint32_t v=0;v<view.variant_count;++v) {
        if(!(v%64)){status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        bool same=false;status=body_nominal_same(variants[v].name,name,control,&same);if(status!=XR_XIR_OK)return status;
        if(same){*out=(XrHirNominalSelection){v,true};return XR_XIR_OK;}
    }
    *out=(XrHirNominalSelection){0};return body_poll(control);
}
XR_FUNC XrXirStatus xr_hir_body_nominal_modules(XrHirBody *body,XrXirSourceModule *const *modules,XrXirGeneric *const *generics) {
    if(!body||body->ended||!modules||!generics||body->nominal_modules)return XR_XIR_BAD_STRUCTURE;
    body->nominal_modules=modules;body->nominal_generics=generics;return XR_XIR_OK;
}
static XrXirStatus body_nominal_declaration_copy(XrHirBody *body,const XrHirNominalDeclarationInput *input,
    XrCompileRequestControl *control,HirBodyNominalDeclaration *row) {
    XrXirNominalDeclaration from=body->types->nominals->declarations[input->declaration];
    *row=(HirBodyNominalDeclaration){.module=input->module,.source_declaration=input->source_declaration,
        .count=from.field_count,.variant_count=from.variant_count,.kind=(XrXirNominalKind)from.kind,.flags=from.flags,
        .explicit_initializer=input->explicit_initializer,.imported=input->imported!=NULL,.received=true};
    if((from.field_count&&!from.fields)||(from.variant_count&&!from.variants)||
        (row->count&&SIZE_MAX/row->count<sizeof(*row->fields))||
        (row->variant_count&&SIZE_MAX/row->variant_count<sizeof(*row->variants)))return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_nominal_allocate(body,(size_t)row->count*sizeof(*row->fields),control,(void **)&row->fields);
    if(status==XR_XIR_OK)status=body_nominal_allocate(body,(size_t)row->variant_count*sizeof(*row->variants),control,(void **)&row->variants);
    if(status==XR_XIR_OK)status=body_nominal_allocate(body,(size_t)row->count*sizeof(uint32_t),control,(void **)&row->field_origins);
    if(status==XR_XIR_OK)status=body_nominal_allocate(body,(size_t)row->count*sizeof(uint32_t),control,(void **)&row->field_functions);
    if(status==XR_XIR_OK)status=body_nominal_allocate(body,(size_t)row->count*sizeof(uint32_t),control,(void **)&row->field_producers);
    if(status==XR_XIR_OK&&input->field_initializers&&row->count) {
        uint32_t capacity=4;
        while(capacity/2<row->count){if(capacity>UINT32_MAX/2)return XR_XIR_OVERFLOW;capacity*=2;}
        if(SIZE_MAX/capacity<sizeof(uint32_t))return XR_XIR_OVERFLOW;
        status=body_nominal_allocate(body,(size_t)capacity*sizeof(uint32_t),control,(void **)&row->field_sources);
        if(status==XR_XIR_OK)row->source_capacity=capacity;
    }
    for(uint32_t f=0;status==XR_XIR_OK&&f<row->count;++f) {
        if(!(f%64))status=body_poll(control);if(status!=XR_XIR_OK)break;
        XrXirNominalField field=from.fields[f];row->fields[f]=(XrHirNominalField){.declared=field.type,.flags=field.flags};
        status=body_nominal_name(body,field.name,control,&row->fields[f].name);
        row->field_origins[f]=input->field_initializers?input->field_initializers[f]:0;
        if(row->field_origins[f]) {
            uint32_t slot=(row->field_origins[f]*UINT32_C(2654435761))&(row->source_capacity-1),probes=0;
            while(row->field_sources[slot]) {
                if(!(probes++%64)){status=body_poll(control);if(status!=XR_XIR_OK)break;}
                if(row->field_origins[row->field_sources[slot]-1]==row->field_origins[f]){status=XR_XIR_BAD_STRUCTURE;break;}
                slot=(slot+1)&(row->source_capacity-1);
            }
            if(status==XR_XIR_OK)row->field_sources[slot]=f+1;
        }
    }
    for(uint32_t v=0;status==XR_XIR_OK&&v<row->variant_count;++v) {
        if(!(v%64))status=body_poll(control);if(status!=XR_XIR_OK)break;
        XrXirNominalVariant variant=from.variants[v];
        if(variant.field_begin>row->count||variant.field_count>row->count-variant.field_begin){status=XR_XIR_BAD_STRUCTURE;break;}
        row->variants[v]=variant;row->variants[v].name=(XrXirLiteral){0};
        status=body_nominal_name(body,variant.name,control,&row->variants[v].name);
    }
    return status;
}
static XrXirStatus body_nominal_import_copy(XrHirBody *body,const XrHirNominalImport *input,
    HirBodyNominalDeclaration *row,XrCompileRequestControl *control) {
    if(!input||!input->types||!input->types->nominals||!input->types->nominals->declarations||
        input->declaration>=input->types->nominals->count)return XR_XIR_BAD_STRUCTURE;
    XrXirNominalDeclaration foreign=input->types->nominals->declarations[input->declaration];
    const XrXirConstructionRow *from=xr_xir_compile_construction_row(input->construction,input->declaration);
    if(!from||from->field_count!=row->count||foreign.field_count!=row->count||foreign.kind!=(uint32_t)row->kind||
        (row->count&&!from->field_initializers))return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_nominal_allocate(body,(size_t)row->count*sizeof(uint32_t),control,(void **)&row->foreign_fields);
    for(uint32_t f=0;status==XR_XIR_OK&&f<row->count;++f) {
        if(!(f%64))status=body_poll(control);if(status!=XR_XIR_OK)break;bool same=false;
        status=body_nominal_same(row->fields[f].name,foreign.fields[f].name,control,&same);
        if(status==XR_XIR_OK&&(!same||row->fields[f].flags!=foreign.fields[f].flags))status=XR_XIR_BAD_STRUCTURE;
        row->foreign_fields[f]=from->field_initializers[f];
    }
    if(status==XR_XIR_OK){row->foreign_constructor=from->default_initializer;row->available=from->default_initializer!=0;}
    return status;
}
XR_FUNC XrXirStatus xr_hir_body_nominal_declare(XrHirBody *body,const XrHirNominalDeclarationInput *input,
    XrCompileRequestControl *control,const char **reason) {
    if(reason)*reason=NULL;
    if(!body||body->ended||body->nominal_shape_only||!input||input->types!=body->types||!body->types->nominals||!body->types->nominals->declarations||
        input->declaration!=body->nominal_declarations.count||input->declaration>=body->types->nominals->count||
        !body->nominal_modules||!*body->nominal_modules||input->module>=*body->declarations.module_count||!input->source_declaration)return XR_XIR_BAD_STRUCTURE;
    XrXirSourceModule module=(*body->nominal_modules)[input->module];bool same=false;
    XrXirStatus status=body_nominal_same(body->types->nominals->declarations[input->declaration].module,
        (XrXirLiteral){module.name,module.name_length},control,&same);
    if(status!=XR_XIR_OK||!same)return status!=XR_XIR_OK?status:XR_XIR_BAD_STRUCTURE;
    HirBodyNominalDeclaration row={0};status=body_nominal_declaration_copy(body,input,control,&row);
    if(status==XR_XIR_OK&&input->imported)status=body_nominal_import_copy(body,input->imported,&row,control);
    if(status==XR_XIR_OK)status=body_vector_reserve(body,&body->nominal_declarations,sizeof(row),control);
    if(status!=XR_XIR_OK){body_nominal_declaration_free(&row);return status;}
    ((HirBodyNominalDeclaration *)body->nominal_declarations.items)[body->nominal_declarations.count++]=row;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_nominal_checked(XrHirBody *body,const XrXirModule *definition,XrCompileRequestControl *control) {
    if(!body||body->ended||body->nominal_declarations.count||!definition||definition->stage!=XR_XIR_CHECKED||
        !definition->declarations||!body->nominal_modules||*body->nominal_modules!=definition->declarations->modules||
        *body->declarations.module_count!=definition->declarations->module_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirNominalTable *table=body->types->nominals,*from=definition->types?definition->types->nominals:NULL;
    if(!table||!from){if(table!=from)return XR_XIR_BAD_STRUCTURE;XrXirStatus status=body_poll(control);if(status==XR_XIR_OK)body->nominal_shape_only=true;return status;}
    if(table->count!=from->count||table->declarations!=from->declarations||(table->count&&!table->declarations))return XR_XIR_BAD_STRUCTURE;
    /* A partial receive may only be cleaned up, never upgraded into Source
     * default authority after an interrupted metadata copy. */
    body->nominal_shape_only=true;
    for(uint32_t d=0;d<table->count;++d) {
        uint32_t module=UINT32_MAX;
        for(uint32_t m=0;m<*body->declarations.module_count;++m) {
            XrXirSourceModule entry=(*body->nominal_modules)[m];bool same=false;
            XrXirStatus status=body_nominal_same(table->declarations[d].module,(XrXirLiteral){entry.name,entry.name_length},control,&same);
            if(status!=XR_XIR_OK)return status;if(same){if(module!=UINT32_MAX)return XR_XIR_BAD_STRUCTURE;module=m;}
        }
        if(module==UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
        XrHirNominalDeclarationInput input={.types=body->types,.declaration=d,.module=module};
        HirBodyNominalDeclaration row={0};XrXirStatus status=body_nominal_declaration_copy(body,&input,control,&row);row.imported=true;
        if(status==XR_XIR_OK)status=body_vector_reserve(body,&body->nominal_declarations,sizeof(row),control);
        if(status!=XR_XIR_OK){body_nominal_declaration_free(&row);return status;}
        ((HirBodyNominalDeclaration *)body->nominal_declarations.items)[body->nominal_declarations.count++]=row;
    }
    XrXirStatus status=body_poll(control);if(status==XR_XIR_OK)body->nominal_shape_only=true;return status;
}
static bool body_nominal_type_defaultable(const XrHirBody *body,XrXirType type,const unsigned char *available) {
    if(body->nominal_shape_only)return false;
    if(type==XR_XIR_BOOL||type==XR_XIR_RUNE||xr_xir_type_is_number(type))return true;
    const XrXirTypeNode *node=xr_xir_type_node(body->types,type);
    if(!node||node->kind!=XR_XIR_TYPE_NOMINAL||node->nominal.declaration>=body->nominal_declarations.count)return false;
    HirBodyNominalDeclaration row=((HirBodyNominalDeclaration *)body->nominal_declarations.items)[node->nominal.declaration];
    const unsigned char *facts=available?available:body->nominal_available;
    return row.kind!=XR_XIR_NOMINAL_CLASS&&(facts?facts[node->nominal.declaration]!=0:row.available);
}
XR_FUNC bool xr_hir_body_nominal_defaultable(const XrHirBody *body,XrXirType type) {
    return body&&!body->ended&&body_nominal_type_defaultable(body,type,NULL);
}
XR_FUNC XrXirStatus xr_hir_body_nominal_schedule(XrHirBody *body,uint32_t functions,uint32_t limit,
    XrCompileRequestControl *control,uint32_t *added,uint32_t *failed) {
    if(!body||body->ended||body->nominal_shape_only||!added||!failed||functions>limit)return XR_XIR_BAD_STRUCTURE;
    if(!body->types->nominals) {
        if(body->nominal_declarations.count)return XR_XIR_BAD_STRUCTURE;
        XrXirStatus empty=body_poll(control);if(empty==XR_XIR_OK)*added=0;return empty;
    }
    if(body->nominal_declarations.count!=body->types->nominals->count)return XR_XIR_BAD_STRUCTURE;
    unsigned char *available=NULL;uint32_t count=body->nominal_declarations.count,total=0;
    XrXirStatus status=body_nominal_allocate(body,count,control,(void **)&available);
    for(uint32_t d=0;status==XR_XIR_OK&&d<count;++d) {
        if(!(d%64))status=body_poll(control);available[d]=((HirBodyNominalDeclaration *)body->nominal_declarations.items)[d].available;
    }
    bool changed=true;
    while(status==XR_XIR_OK&&changed) {
        changed=false;
        for(uint32_t d=0;status==XR_XIR_OK&&d<count;++d) {
            if(!(d%64))status=body_poll(control);if(status!=XR_XIR_OK)break;
            HirBodyNominalDeclaration row=((HirBodyNominalDeclaration *)body->nominal_declarations.items)[d];
            if(row.imported||row.explicit_initializer||available[d]||(row.kind!=XR_XIR_NOMINAL_STRUCT&&row.kind!=XR_XIR_NOMINAL_CLASS))continue;
            bool ready=true;
            for(uint32_t f=0;f<row.count;++f) {
                if(!(f%64))status=body_poll(control);if(status!=XR_XIR_OK){ready=false;break;}
                if(!row.field_origins[f]&&!body_nominal_type_defaultable(body,row.fields[f].declared,available)){ready=false;break;}
            }
            if(!ready)continue;
            if(total==limit-functions){*failed=d;status=XR_XIR_BUDGET;break;}
            available[d]=1;++total;changed=true;
        }
    }
    if(status==XR_XIR_OK)status=body_poll(control);
    if(status==XR_XIR_OK) {
        xr_compile_resources_free(body->nominal_available);body->nominal_available=available;available=NULL;
        *added=total;
    }
    xr_compile_resources_free(available);return status;
}
XR_FUNC bool xr_hir_body_nominal_defaults(const XrHirBody *body,uint32_t declaration,XrHirNominalDefaults *out) {
    if(!body||body->ended||body->nominal_shape_only||!out||declaration>=body->nominal_declarations.count)return false;
    HirBodyNominalDeclaration row=((HirBodyNominalDeclaration *)body->nominal_declarations.items)[declaration];
    bool available=body->nominal_available?body->nominal_available[declaration]!=0:row.available;
    *out=(XrHirNominalDefaults){row.constructor,available?row.constructor:0,row.count,row.field_functions,available,row.imported};return true;
}
static XrXirStatus body_nominal_function(const XrHirBody *body,uint32_t declaration,uint32_t function,
    uint32_t field,XrCompileRequestControl *control) {
    if(!body->declarations.functions||!*body->declarations.functions||!body->declarations.identities||!*body->declarations.identities||
        function>=*body->declarations.function_count||!function)return XR_XIR_BAD_STRUCTURE;
    HirBodyNominalDeclaration row=((HirBodyNominalDeclaration *)body->nominal_declarations.items)[declaration];
    XrXirFunction signature=(*body->declarations.functions)[function];XrXirFunctionIdentity identity=(*body->declarations.identities)[function];
    if(identity.nominal_owner!=declaration+1||identity.module!=row.module)return XR_XIR_BAD_STRUCTURE;
    if(field==UINT32_MAX) {
        const XrXirTypeNode *result=xr_xir_type_node(body->types,signature.result);
        if(identity.method_kind!=XR_XIR_CONSTRUCTOR||!result||result->kind!=XR_XIR_TYPE_NOMINAL||result->nominal.declaration!=declaration)return XR_XIR_BAD_STRUCTURE;
    } else if(field>=row.count||identity.method_kind!=XR_XIR_MEMBER_HELPER||signature.parameter_count||signature.result!=row.fields[field].declared)
        return XR_XIR_BAD_STRUCTURE;
    return body_poll(control);
}
XR_FUNC XrXirStatus xr_hir_body_nominal_function_source(XrHirBody *body,const XrHirNominalFunctionSource *input,
    XrCompileRequestControl *control,XrHirNominalFunction *out) {
    if(!body||body->ended||body->nominal_sealed||!input||!out||!input->function||!input->declaration||
        !body->declarations.identities||!*body->declarations.identities||input->function>=*body->declarations.function_count)return XR_XIR_BAD_STRUCTURE;
    XrXirFunctionIdentity identity=(*body->declarations.identities)[input->function];
    if(!identity.nominal_owner||identity.nominal_owner>body->nominal_declarations.count)return XR_XIR_BAD_STRUCTURE;
    uint32_t declaration=identity.nominal_owner-1,field=UINT32_MAX;
    HirBodyNominalDeclaration row=((HirBodyNominalDeclaration *)body->nominal_declarations.items)[declaration];
    if(row.imported)return XR_XIR_BAD_STRUCTURE;
    if(!row.source_capacity||!row.field_sources)return XR_XIR_BAD_STRUCTURE;
    uint32_t slot=(input->declaration*UINT32_C(2654435761))&(row.source_capacity-1),probes=0;
    while(row.field_sources[slot]) {
        if(!(probes++%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        uint32_t f=row.field_sources[slot]-1;
        if(row.field_origins[f]==input->declaration){field=f;break;}
        slot=(slot+1)&(row.source_capacity-1);
    }
    if(field==UINT32_MAX||row.field_producers[field])return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_nominal_function(body,declaration,input->function,field,control);if(status!=XR_XIR_OK)return status;
    uint32_t count=*body->declarations.function_count;
    if(count>body->nominal_functions.count) {
        if(count&&SIZE_MAX/count<sizeof(HirBodyNominalFunctionSource))return XR_XIR_OVERFLOW;
        HirBodyNominalFunctionSource *values=NULL;
        status=body_nominal_allocate(body,(size_t)count*sizeof(*values),control,(void **)&values);
        if(status==XR_XIR_OK)status=body_bytes(values,body->nominal_functions.items,(size_t)body->nominal_functions.count*sizeof(*values),control);
        if(status!=XR_XIR_OK){xr_compile_resources_free(values);return status;}
        xr_compile_resources_free(body->nominal_functions.items);body->nominal_functions=(HirBodyVector){values,count,count};
    }
    HirBodyNominalFunctionSource *owned=&((HirBodyNominalFunctionSource *)body->nominal_functions.items)[input->function];
    if(owned->function)return XR_XIR_BAD_STRUCTURE;
    status=body_poll(control);if(status!=XR_XIR_OK)return status;
    *owned=(HirBodyNominalFunctionSource){input->function,declaration,field,input->declaration};
    ((HirBodyNominalDeclaration *)body->nominal_declarations.items)[declaration].field_producers[field]=input->function;
    *out=(XrHirNominalFunction){body,input->function};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_nominal_bind(XrHirBody *body,const XrHirNominalFunctionInput *input,
    XrCompileRequestControl *control) {
    if(!body||body->ended||body->nominal_shape_only||!input||input->declaration>=body->nominal_declarations.count||
        (input->kind!=XR_HIR_NOMINAL_EXPLICIT&&input->kind!=XR_HIR_NOMINAL_IMPLICIT&&input->kind!=XR_HIR_NOMINAL_FIELD_DEFAULT))return XR_XIR_BAD_STRUCTURE;
    HirBodyNominalDeclaration row=((HirBodyNominalDeclaration *)body->nominal_declarations.items)[input->declaration];
    uint32_t field=input->kind==XR_HIR_NOMINAL_FIELD_DEFAULT?input->field:UINT32_MAX;
    if(field!=UINT32_MAX) {
        if(field>=row.count||input->source.owner!=body||input->source.index>=body->nominal_functions.count||row.field_functions[field])return XR_XIR_BAD_STRUCTURE;
        HirBodyNominalFunctionSource source=((HirBodyNominalFunctionSource *)body->nominal_functions.items)[input->source.index];
        if(source.function!=input->function||source.declaration!=input->declaration||source.field!=field||
            source.origin!=row.field_origins[field])return XR_XIR_BAD_STRUCTURE;
    } else {
        if(row.constructor)return XR_XIR_BAD_STRUCTURE;
        bool available=body->nominal_available&&body->nominal_available[input->declaration];
        if(!row.imported&&((input->kind==XR_HIR_NOMINAL_IMPLICIT&&(!available||row.explicit_initializer))||
            (input->kind==XR_HIR_NOMINAL_EXPLICIT&&!row.explicit_initializer)))return XR_XIR_BAD_STRUCTURE;
    }
    XrXirStatus status=body_nominal_function(body,input->declaration,input->function,field,control);if(status!=XR_XIR_OK)return status;
    HirBodyNominalDeclaration *owned=&((HirBodyNominalDeclaration *)body->nominal_declarations.items)[input->declaration];
    if(field!=UINT32_MAX)owned->field_functions[field]=input->function;
    else {
        owned->constructor=input->function;
        owned->flags=body->types->nominals->declarations[input->declaration].flags;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_nominal_import_bind(XrHirBody *body,uint32_t declaration,
    const uint32_t *functions,uint32_t count,XrCompileRequestControl *control) {
    if(!body||body->ended||body->nominal_shape_only||declaration>=body->nominal_declarations.count||(count&&!functions))return XR_XIR_BAD_STRUCTURE;
    HirBodyNominalDeclaration row=((HirBodyNominalDeclaration *)body->nominal_declarations.items)[declaration];
    if(!row.imported)return XR_XIR_BAD_STRUCTURE;
    if(row.foreign_constructor&&(row.foreign_constructor-1>=count||functions[row.foreign_constructor-1]==UINT32_MAX||
        functions[row.foreign_constructor-1]!=row.constructor))return XR_XIR_BAD_STRUCTURE;
    uint32_t *fields=NULL;XrXirStatus status=body_nominal_allocate(body,(size_t)row.count*sizeof(*fields),control,(void **)&fields);
    for(uint32_t f=0;status==XR_XIR_OK&&f<row.count;++f) {
        if(!(f%64))status=body_poll(control);if(status!=XR_XIR_OK)break;
        if(!row.foreign_fields[f])continue;uint32_t original=row.foreign_fields[f]-1;
        if(original>=count||functions[original]==UINT32_MAX){status=XR_XIR_BAD_STRUCTURE;break;}
        fields[f]=functions[original];status=body_nominal_function(body,declaration,fields[f],f,control);
    }
    if(status==XR_XIR_OK)status=body_poll(control);
    if(status!=XR_XIR_OK){xr_compile_resources_free(fields);return status;}
    HirBodyNominalDeclaration *owned=&((HirBodyNominalDeclaration *)body->nominal_declarations.items)[declaration];
    xr_compile_resources_free(owned->field_functions);owned->field_functions=fields;return XR_XIR_OK;
}
XR_FUNC bool xr_hir_body_nominal_info(const XrHirBody *body,XrHirNominalShape shape,XrHirNominalInfo *out) {
    if(!body||body->ended||shape.owner!=body||shape.index>=body->nominals.count||!out)return false;
    HirBodyNominal row=((HirBodyNominal *)body->nominals.items)[shape.index];
    if(row.declaration>=body->nominal_declarations.count)return false;
    HirBodyNominalDeclaration declaration=((HirBodyNominalDeclaration *)body->nominal_declarations.items)[row.declaration];
    *out=(XrHirNominalInfo){row.type,declaration.kind,row.declaration,declaration.module,declaration.flags,row.count,
        declaration.variant_count,row.argument_count,row.fields,declaration.variants,row.arguments};return true;
}
static XrXirStatus body_nominal_map_stage(XrHirBody *body,uint32_t key,XrCompileRequestControl *control,uint32_t **out,uint32_t *size) {
    *out=body->nominal_map;*size=body->nominal_map_capacity;if(key<body->nominal_map_capacity)return body_poll(control);
    uint32_t capacity=body->nominal_map_capacity?body->nominal_map_capacity:16;
    while(capacity<=key) {
        XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
        if(capacity>UINT32_MAX/2)return XR_XIR_OVERFLOW;capacity*=2;
    }
    if(capacity&&SIZE_MAX/capacity<sizeof(uint32_t))return XR_XIR_OVERFLOW;uint32_t *map=NULL;
    XrXirStatus status=body_nominal_allocate(body,(size_t)capacity*sizeof(*map),control,(void **)&map);
    if(status==XR_XIR_OK)status=body_bytes(map,body->nominal_map,(size_t)body->nominal_map_capacity*sizeof(*map),control);
    if(status!=XR_XIR_OK){xr_compile_resources_free(map);return status;}*out=map;*size=capacity;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_nominal_receive(XrHirBody *body,const XrXirTypes *types,XrXirType type,
    XrCompileRequestControl *control,XrHirNominalShape *out,const char **reason) {
    if(!body||body->ended||types!=body->types||!out)return XR_XIR_BAD_STRUCTURE;XrHirNominalView view={0};
    XrXirStatus status=xr_hir_nominal_view(types,type,control,&view,reason);if(status!=XR_XIR_OK)return status;
    if(view.declaration>=body->nominal_declarations.count)return XR_XIR_BAD_STRUCTURE;
    uint32_t key=(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE;
    if(key<body->nominal_map_capacity&&body->nominal_map[key]){*out=(XrHirNominalShape){body,body->nominal_map[key]-1};return XR_XIR_OK;}
    HirBodyNominalDeclaration declaration=((HirBodyNominalDeclaration *)body->nominal_declarations.items)[view.declaration];
    HirBodyNominal row={.type=type,.declaration=view.declaration,.count=view.field_count,.argument_count=view.argument_count};
    if(row.count!=declaration.count||(row.count&&SIZE_MAX/row.count<sizeof(*row.fields))||
        (row.argument_count&&SIZE_MAX/row.argument_count<sizeof(*row.arguments)))return XR_XIR_BAD_STRUCTURE;
    status=body_nominal_allocate(body,(size_t)row.count*sizeof(*row.fields),control,(void **)&row.fields);
    if(status==XR_XIR_OK)status=body_bytes(row.fields,declaration.fields,(size_t)row.count*sizeof(*row.fields),control);
    if(status==XR_XIR_OK)status=body_nominal_allocate(body,(size_t)row.argument_count*sizeof(*row.arguments),control,(void **)&row.arguments);
    if(status==XR_XIR_OK)status=body_bytes(row.arguments,xr_xir_type_node(types,type)->nominal.arguments,(size_t)row.argument_count*sizeof(*row.arguments),control);
    uint32_t *map=body->nominal_map,capacity=body->nominal_map_capacity;
    if(status==XR_XIR_OK)status=body_nominal_map_stage(body,key,control,&map,&capacity);
    if(status==XR_XIR_OK)status=body_vector_reserve(body,&body->nominals,sizeof(row),control);
    if(status!=XR_XIR_OK) {
        xr_compile_resources_free(row.fields);xr_compile_resources_free(row.arguments);
        if(map!=body->nominal_map)xr_compile_resources_free(map);return status;
    }
    uint32_t index=body->nominals.count;((HirBodyNominal *)body->nominals.items)[index]=row;++body->nominals.count;
    if(map!=body->nominal_map){xr_compile_resources_free(body->nominal_map);body->nominal_map=map;body->nominal_map_capacity=capacity;}
    body->nominal_map[key]=index+1;*out=(XrHirNominalShape){body,index};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_nominal_field(XrHirBody *body,XrHirNominalShape shape,const XrHirNominalPool *pool,
    uint32_t field,XrHirNominalField *out,const char **reason) {
    XrHirNominalInfo info={0};
    if(!body||body->ended||!pool||!pool->compile||pool->compile->resources!=body->resources||pool->types!=body->types||
        !xr_hir_body_nominal_info(body,shape,&info)||field>=info.count||!out)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_poll(pool->control);if(status!=XR_XIR_OK)return status;
    HirBodyNominal *row=&((HirBodyNominal *)body->nominals.items)[shape.index];
    if(!row->fields[field].ready) {
        XrXirType type;status=xr_hir_nominal_field_type(pool,info.type,field,&type,reason);if(status!=XR_XIR_OK)return status;
        row=&((HirBodyNominal *)body->nominals.items)[shape.index];row->fields[field].type=type;row->fields[field].ready=true;
    }
    *out=row->fields[field];return XR_XIR_OK;
}
static XrXirStatus body_nominal_origin(const XrHirBody *body,XrHirBodyScope scope,XrHirBodyOrigin origin) {
    if(!body||body->ended||body->nominal_sealed||scope.owner!=body||scope.index>=body->scopes.count)return XR_XIR_BAD_STRUCTURE;
    HirBodyScope row=((HirBodyScope *)body->scopes.items)[scope.index];
    return row.origin.function==origin.function&&row.origin.module==origin.module?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static bool body_nominal_pool(const XrHirBody *body,const XrHirNominalPool *pool,XrCompileRequestControl *control) {
    return body&&pool&&pool->types==body->types&&pool->compile&&pool->compile->resources==body->resources&&pool->control==control;
}
static XrXirStatus body_nominal_payload(const XrHirNominalInfo *info,uint32_t variant,uint32_t *begin,uint32_t *count) {
    *begin=0;*count=info->count;
    if(info->kind!=XR_XIR_NOMINAL_ENUM)return variant?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
    if(variant>=info->variant_count)return XR_XIR_BAD_TYPE;
    XrXirNominalVariant row=info->variants[variant];
    if(row.field_begin>info->count||row.field_count>info->count-row.field_begin)return XR_XIR_BAD_STRUCTURE;
    *begin=row.field_begin;*count=row.field_count;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_nominal_construct(XrHirBody *body,const XrHirNominalUseInput *input,
    XrCompileRequestControl *control,XrHirNominalAction *out,const char **reason) {
    XrHirNominalInfo info={0};
    if(!input||!out||!body_nominal_pool(body,input->pool,control)||!xr_hir_body_nominal_info(body,input->shape,&info)||input->type!=info.type||
        body_nominal_origin(body,input->scope,input->origin)!=XR_XIR_OK||(input->count&&!input->values))return XR_XIR_BAD_STRUCTURE;
    uint32_t begin=0,count=0;XrXirStatus status=body_nominal_payload(&info,input->variant,&begin,&count);
    if(status!=XR_XIR_OK)return status;
    if(count!=input->count)return body_nominal_failure(reason,XR_XIR_BAD_TYPE,"nominal construction requires its complete ordered fields");
    uint32_t physical=count;
    for(uint32_t f=0;f<count;++f) {
        if(!(f%64)){status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        XrHirNominalField field={0};status=xr_hir_body_nominal_field(body,input->shape,input->pool,begin+f,&field,reason);
        if(status!=XR_XIR_OK)return status;
        if(input->values[f].type!=field.type)return body_nominal_failure(reason,XR_XIR_BAD_TYPE,
            info.kind==XR_XIR_NOMINAL_ENUM?"enum payload type mismatch":"struct field type mismatch");
    }
    if(info.kind==XR_XIR_NOMINAL_ENUM&&count==1&&input->values[0].type==XR_XIR_UNIT) {
        XrXirTaskResultType result={0};bool unit=xr_xir_nominal_native_recv(body->types,info.type,XR_XIR_UNIT)||
            (xr_xir_task_result_type_info(body->types,info.type,&result)&&input->variant==XR_XIR_TASK_RESULT_SUCCESS&&result.element==XR_XIR_UNIT);
        if(!unit)return body_nominal_failure(reason,XR_XIR_BAD_TYPE,"Unit payload requires an admitted native value variant");physical=0;
    }
    XrHirNominalValue *values=NULL;status=body_nominal_allocate(body,(size_t)physical*sizeof(*values),control,(void **)&values);
    if(status==XR_XIR_OK)status=body_bytes(values,input->values,(size_t)physical*sizeof(*values),control);
    XrHirBodyInput typed={.origin=input->origin,.rule=XR_HIR_BODY_PROJECTION,.source=info.type,.target=info.type};XrHirBodyUse use={0};
    if(status==XR_XIR_OK)status=xr_hir_body_check(body,body->types,&typed,control,&use,reason);
    if(status!=XR_XIR_OK){xr_compile_resources_free(values);return status;}
    HirBodyUse *row=&body->uses[use.index];row->nominal=input->shape;row->nominal_construct=true;
    row->nominal_values=values;row->nominal_count=physical;row->nominal_variant=input->variant;row->required_instruction=true;
    row->action.operation=info.kind==XR_XIR_NOMINAL_ENUM?XR_XIR_ENUM_NEW:info.kind==XR_XIR_NOMINAL_CLASS?XR_XIR_CLASS_NEW:XR_XIR_STRUCT_NEW;
    *out=(XrHirNominalAction){.use=use,.shape=input->shape,.operation=row->action.operation,.type=info.type,
        .variant=input->variant,.count=physical,.values=values};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_nominal_get(XrHirBody *body,const XrHirNominalUseInput *input,
    XrCompileRequestControl *control,XrHirNominalAction *out,const char **reason) {
    XrHirNominalInfo info={0};
    if(!input||!out||!body_nominal_pool(body,input->pool,control)||!xr_hir_body_nominal_info(body,input->shape,&info)||input->type!=info.type||
        input->receiver==UINT32_MAX||body_nominal_origin(body,input->scope,input->origin)!=XR_XIR_OK)return XR_XIR_BAD_STRUCTURE;
    uint32_t begin=0,count=0;XrXirStatus status=body_nominal_payload(&info,input->variant,&begin,&count);
    if(status!=XR_XIR_OK||input->field>=count)return status!=XR_XIR_OK?status:XR_XIR_BAD_STRUCTURE;
    XrHirNominalField field={0};status=xr_hir_body_nominal_field(body,input->shape,input->pool,begin+input->field,&field,reason);
    if(status!=XR_XIR_OK)return status;
    XrHirBodyInput typed={.origin=input->origin,.rule=XR_HIR_BODY_PROJECTION,.source=info.type,.target=field.type};XrHirBodyUse use={0};
    status=xr_hir_body_check(body,body->types,&typed,control,&use,reason);if(status!=XR_XIR_OK)return status;
    HirBodyUse *row=&body->uses[use.index];row->nominal=input->shape;row->nominal_variant=input->variant;row->nominal_field=input->field;
    row->required_instruction=true;row->has_operand=true;row->source_operand=input->receiver;
    row->action.operation=info.kind==XR_XIR_NOMINAL_ENUM?XR_XIR_ENUM_GET:info.kind==XR_XIR_NOMINAL_CLASS?XR_XIR_CLASS_GET:XR_XIR_STRUCT_GET;
    *out=(XrHirNominalAction){.use=use,.shape=input->shape,.operation=row->action.operation,.type=field.type,.field=input->field,.variant=input->variant};return XR_XIR_OK;
}
static XrXirStatus body_nominal_helper_use(XrHirBody *body,const XrHirNominalUseInput *input,
    uint32_t function,XrXirType result,XrCompileRequestControl *control,XrHirNominalAction *out,const char **reason) {
    XrHirNominalInfo info={0};if(!xr_hir_body_nominal_info(body,input->shape,&info))return XR_XIR_BAD_STRUCTURE;
    if(body_nominal_origin(body,input->scope,input->origin)!=XR_XIR_OK||!body->nominal_generics||!*body->nominal_generics)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_nominal_function(body,info.declaration,function,input->field,control);if(status!=XR_XIR_OK)return status;
    XrXirFunction target=(*body->declarations.functions)[function];
    if(target.parameter_count||(*body->nominal_generics)[function].parameter_count!=info.argument_count)return XR_XIR_BAD_STRUCTURE;
    status=body_substitution(body,info.arguments,info.argument_count,target.result,result,control);if(status!=XR_XIR_OK)return status;
    XrHirBodyInput typed={.origin=input->origin,.rule=XR_HIR_BODY_PROJECTION,.source=info.type,.target=result};XrHirBodyUse use={0};
    status=xr_hir_body_check(body,body->types,&typed,control,&use,reason);if(status!=XR_XIR_OK)return status;
    HirBodyUse *row=&body->uses[use.index];row->nominal=input->shape;row->nominal_helper=true;row->nominal_function=function;
    row->nominal_field=input->field;row->required_instruction=true;row->action.operation=XR_XIR_CALL;
    *out=(XrHirNominalAction){.use=use,.shape=input->shape,.operation=XR_XIR_CALL,.type=result,.field=input->field,
        .function=function,.arguments=info.arguments,.argument_count=info.argument_count};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_nominal_helper(XrHirBody *body,const XrHirNominalUseInput *input,
    XrCompileRequestControl *control,XrHirNominalAction *out,const char **reason) {
    XrHirNominalInfo info={0};if(!input||!out||!body_nominal_pool(body,input->pool,control)||!xr_hir_body_nominal_info(body,input->shape,&info)||input->type!=info.type||
        body_nominal_origin(body,input->scope,input->origin)!=XR_XIR_OK)return XR_XIR_BAD_STRUCTURE;
    XrHirNominalDefaults defaults={0};if(!xr_hir_body_nominal_defaults(body,info.declaration,&defaults)||input->field>=defaults.count)return XR_XIR_BAD_STRUCTURE;
    uint32_t function=defaults.fields[input->field];
    uint32_t owner=((HirBodyScope *)body->scopes.items)[input->scope.index].nominal_owner;
    if(!function||(owner!=info.declaration+1&&(info.fields[input->field].flags&(XR_XIR_FIELD_PRIVATE|XR_XIR_FIELD_PROTECTED))))
        return body_nominal_failure(reason,XR_XIR_BAD_TYPE,"omitted field requires an accessible declaration default");
    XrHirNominalField field={0};XrXirStatus status=xr_hir_body_nominal_field(body,input->shape,input->pool,input->field,&field,reason);
    if(status!=XR_XIR_OK)return status;return body_nominal_helper_use(body,input,function,field.type,control,out,reason);
}
XR_FUNC XrXirStatus xr_hir_body_nominal_default(XrHirBody *body,const XrHirNominalUseInput *input,
    XrCompileRequestControl *control,XrHirNominalAction *out,const char **reason) {
    if(!body||body->ended||!input||!out||!body_nominal_pool(body,input->pool,control)||body_nominal_origin(body,input->scope,input->origin)!=XR_XIR_OK)return XR_XIR_BAD_STRUCTURE;
    XrXirType type=input->type;XrXirOp operation=XR_XIR_INVALID;
    if(type==XR_XIR_UNIT)operation=XR_XIR_INVALID;
    else if(type==XR_XIR_RUNE)operation=XR_XIR_CONST_RUNE;
    else if(type==XR_XIR_BOOL)operation=XR_XIR_CONST_BOOL;
    else if(xr_xir_type_is_nullable(body->types,type))operation=XR_XIR_NULLABLE_NONE;
    else if(xr_xir_type_is_number(type))operation=type==XR_XIR_F32||type==XR_XIR_F64?XR_XIR_CONST_FLOAT:XR_XIR_CONST_INT;
    else {
        XrHirNominalInfo info={0};XrHirNominalDefaults defaults={0};
        if(!xr_hir_body_nominal_info(body,input->shape,&info)||info.type!=type||
            !xr_hir_body_nominal_defaults(body,info.declaration,&defaults)||!defaults.implicit_initializer||!xr_hir_body_nominal_defaultable(body,type))
            return body_nominal_failure(reason,XR_XIR_BAD_TYPE,"type has no admitted default initializer");
        XrHirNominalUseInput helper=*input;helper.field=UINT32_MAX;
        return body_nominal_helper_use(body,&helper,defaults.implicit_initializer,type,control,out,reason);
    }
    XrHirBodyInput typed={.origin=input->origin,.rule=XR_HIR_BODY_PROJECTION,.source=type,.target=type};XrHirBodyUse use={0};
    XrXirStatus status=xr_hir_body_check(body,body->types,&typed,control,&use,reason);if(status!=XR_XIR_OK)return status;
    HirBodyUse *row=&body->uses[use.index];row->action.operation=operation;row->required_instruction=operation!=XR_XIR_INVALID;
    if(operation!=XR_XIR_INVALID){row->has_immediate=true;row->expected_immediate=0;}
    *out=(XrHirNominalAction){.use=use,.operation=operation,.type=type};return XR_XIR_OK;
}
static XrXirStatus body_nominal_plan_admit(const XrHirBody *body,const XrHirNominalPlanInput *input,
    const XrHirNominalInfo *info,const char **reason) {
    if(input->kind!=XR_HIR_NOMINAL_STRUCT_LITERAL&&input->kind!=XR_HIR_NOMINAL_ENUM_LITERAL)return XR_XIR_BAD_STRUCTURE;
    if(input->kind==XR_HIR_NOMINAL_ENUM_LITERAL&&input->application_pending)return XR_XIR_BAD_STRUCTURE;
    if(input->kind==XR_HIR_NOMINAL_STRUCT_LITERAL) {
        if(info->kind!=XR_XIR_NOMINAL_STRUCT)return body_nominal_failure(reason,XR_XIR_BAD_TYPE,"struct literal requires an admitted nominal declaration");
        if(input->application_pending)return XR_XIR_OK;
        if((info->flags&XR_XIR_NOMINAL_MODULE_LITERAL)&&input->origin.module!=info->module)
            return body_nominal_failure(reason,XR_XIR_BAD_TYPE,"struct literal with init is restricted to its declaring module");
        if(input->count>info->count)return body_nominal_failure(reason,XR_XIR_BAD_TYPE,"struct literal has too many fields");
    } else {
        if(info->kind!=XR_XIR_NOMINAL_ENUM||input->variant>=info->variant_count)return XR_XIR_BAD_STRUCTURE;
        uint32_t count=info->variants[input->variant].field_count;
        if(input->count!=count||(!count&&input->provided))return body_nominal_failure(reason,XR_XIR_BAD_TYPE,"enum variant requires exactly its declared named payload");
    }
    return body_nominal_origin(body,input->scope,input->origin);
}
XR_FUNC XrXirStatus xr_hir_body_nominal_begin(XrHirBody *body,const XrHirNominalPlanInput *input,
    XrCompileRequestControl *control,XrHirNominalPlan *out,const char **reason) {
    XrHirNominalInfo info={0};if(!input||!out||!xr_hir_body_nominal_info(body,input->shape,&info)||
        body_nominal_origin(body,input->scope,input->origin)!=XR_XIR_OK)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_nominal_plan_admit(body,input,&info,reason);if(status!=XR_XIR_OK)return status;
    HirBodyNominalPlan row={.input=*input,.applied=!input->application_pending,.selected=UINT32_MAX};
    status=body_nominal_payload(&info,input->variant,&row.begin,&row.count);if(status!=XR_XIR_OK)return status;
    if(row.count&&SIZE_MAX/row.count<sizeof(*row.values))return XR_XIR_OVERFLOW;
    status=body_nominal_allocate(body,(size_t)row.count*sizeof(*row.values),control,(void **)&row.values);
    if(status==XR_XIR_OK)status=body_nominal_allocate(body,row.count,control,(void **)&row.present);
    if(status==XR_XIR_OK)status=body_vector_reserve(body,&body->nominal_plans,sizeof(row),control);
    if(status!=XR_XIR_OK){xr_compile_resources_free(row.values);xr_compile_resources_free(row.present);return status;}
    uint32_t index=body->nominal_plans.count;((HirBodyNominalPlan *)body->nominal_plans.items)[index]=row;++body->nominal_plans.count;
    *out=(XrHirNominalPlan){body,index};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_nominal_apply(XrHirBody *body,XrHirNominalPlan plan,XrHirNominalShape shape,
    XrCompileRequestControl *control,const char **reason) {
    XrHirNominalInfo info={0},original={0};
    if(!body||body->ended||plan.owner!=body||plan.index>=body->nominal_plans.count||!xr_hir_body_nominal_info(body,shape,&info))return XR_XIR_BAD_STRUCTURE;
    HirBodyNominalPlan row=((HirBodyNominalPlan *)body->nominal_plans.items)[plan.index];
    if(row.applied||row.complete||!xr_hir_body_nominal_info(body,row.input.shape,&original)||original.declaration!=info.declaration)return XR_XIR_BAD_STRUCTURE;
    XrHirNominalPlanInput input=row.input;input.shape=shape;input.application_pending=false;
    XrXirStatus status=body_nominal_plan_admit(body,&input,&info,reason);if(status==XR_XIR_OK)status=body_poll(control);
    if(status==XR_XIR_OK){((HirBodyNominalPlan *)body->nominal_plans.items)[plan.index].input=input;((HirBodyNominalPlan *)body->nominal_plans.items)[plan.index].applied=true;}
    return status;
}
XR_FUNC XrXirStatus xr_hir_body_nominal_find(const XrHirBody *body,XrHirNominalShape shape,XrXirLiteral name,
    XrCompileRequestControl *control,uint32_t *out,const char **reason) {
    XrHirNominalInfo info={0};if(!name.bytes||!out||!xr_hir_body_nominal_info(body,shape,&info))return XR_XIR_BAD_STRUCTURE;
    for(uint32_t f=0;f<info.count;++f) {
        if(!(f%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        bool same=false;XrXirStatus status=body_nominal_same(info.fields[f].name,name,control,&same);
        if(status!=XR_XIR_OK)return status;if(same){*out=f;return XR_XIR_OK;}
    }
    return body_nominal_failure(reason,XR_XIR_BAD_TYPE,"unknown struct field");
}
XR_FUNC XrXirStatus xr_hir_body_nominal_select(XrHirBody *body,XrHirNominalPlan plan,XrXirLiteral name,
    XrCompileRequestControl *control,uint32_t *out,const char **reason) {
    if(!body||body->ended||plan.owner!=body||plan.index>=body->nominal_plans.count||!out||!name.bytes)return XR_XIR_BAD_STRUCTURE;
    HirBodyNominalPlan row=((HirBodyNominalPlan *)body->nominal_plans.items)[plan.index];XrHirNominalInfo info={0};
    if(!row.applied||row.complete||!xr_hir_body_nominal_info(body,row.input.shape,&info)||row.selected!=UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
    for(uint32_t f=0;f<row.count;++f) {
        if(!(f%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        bool same=false;XrXirStatus status=body_nominal_same(info.fields[row.begin+f].name,name,control,&same);
        if(status!=XR_XIR_OK)return status;if(!same)continue;
        if(row.input.kind==XR_HIR_NOMINAL_STRUCT_LITERAL&&
            ((HirBodyScope *)body->scopes.items)[row.input.scope.index].nominal_owner!=info.declaration+1&&
            (info.fields[row.begin+f].flags&(XR_XIR_FIELD_PRIVATE|XR_XIR_FIELD_PROTECTED)))
            return body_nominal_failure(reason,XR_XIR_BAD_TYPE,"field access is not permitted");
        if(row.present[f])return body_nominal_failure(reason,XR_XIR_BAD_TYPE,info.kind==XR_XIR_NOMINAL_ENUM?"unknown or duplicate enum payload field":"duplicate struct literal field");
        row.present[f]=1;((HirBodyNominalPlan *)body->nominal_plans.items)[plan.index].selected=f;*out=f;return XR_XIR_OK;
    }
    return body_nominal_failure(reason,XR_XIR_BAD_TYPE,info.kind==XR_XIR_NOMINAL_ENUM?"unknown or duplicate enum payload field":"unknown struct field");
}
XR_FUNC XrXirStatus xr_hir_body_nominal_supply(XrHirBody *body,XrHirNominalPlan plan,const XrHirNominalPool *pool,
    uint32_t field,XrHirNominalValue value,const char **reason) {
    if(!body||body->ended||plan.owner!=body||plan.index>=body->nominal_plans.count)return XR_XIR_BAD_STRUCTURE;
    HirBodyNominalPlan row=((HirBodyNominalPlan *)body->nominal_plans.items)[plan.index];
    if(!row.applied||row.complete||field>=row.count||(row.selected!=field&&row.selected!=UINT32_MAX)||row.present[field]==2)return XR_XIR_BAD_STRUCTURE;
    if(!row.present[field]) {
        if(value.producer.owner!=body||value.producer.index>=body->count)return XR_XIR_BAD_STRUCTURE;
        HirBodyUse producer=body->uses[value.producer.index];XrHirNominalInfo expected={0},actual={0};
        if(!producer.nominal_helper||!producer.consumed||!producer.nominal_emitted||!producer.nominal_has_call||producer.action.target!=value.type||
            (value.type!=XR_XIR_UNIT&&(!producer.nominal_has_result||producer.nominal_result!=value.value))||
            !xr_hir_body_nominal_info(body,producer.nominal,&actual)||!xr_hir_body_nominal_info(body,row.input.shape,&expected)||
            actual.type!=expected.type||producer.nominal_field!=row.begin+field||producer.origin.function!=row.input.origin.function||
            producer.origin.module!=row.input.origin.module)return XR_XIR_BAD_STRUCTURE;
    }
    XrHirNominalField expected={0};XrXirStatus status=xr_hir_body_nominal_field(body,row.input.shape,pool,row.begin+field,&expected,reason);
    if(status!=XR_XIR_OK)return status;XrHirNominalInfo info={0};if(!xr_hir_body_nominal_info(body,row.input.shape,&info))return XR_XIR_BAD_STRUCTURE;
    if(expected.type!=value.type)return body_nominal_failure(reason,XR_XIR_BAD_TYPE,info.kind==XR_XIR_NOMINAL_ENUM?"enum payload type mismatch":"struct field type mismatch");
    HirBodyNominalPlan *owned=&((HirBodyNominalPlan *)body->nominal_plans.items)[plan.index];
    owned->values[field]=value;owned->present[field]=2;owned->selected=UINT32_MAX;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_nominal_missing(const XrHirBody *body,XrHirNominalPlan plan,uint32_t field,
    XrCompileRequestControl *control,bool *out,const char **reason) {
    (void)reason;if(!body||body->ended||plan.owner!=body||plan.index>=body->nominal_plans.count||!out)return XR_XIR_BAD_STRUCTURE;
    HirBodyNominalPlan row=((HirBodyNominalPlan *)body->nominal_plans.items)[plan.index];
    if(!row.applied||row.complete||field>=row.count||row.selected!=UINT32_MAX||row.present[field]==1)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_poll(control);if(status==XR_XIR_OK)*out=!row.present[field];return status;
}
XR_FUNC XrXirStatus xr_hir_body_nominal_complete(XrHirBody *body,XrHirNominalPlan plan,const XrHirNominalPool *pool,
    XrHirNominalAction *out,const char **reason) {
    if(!body||body->ended||plan.owner!=body||plan.index>=body->nominal_plans.count||!pool)return XR_XIR_BAD_STRUCTURE;
    HirBodyNominalPlan row=((HirBodyNominalPlan *)body->nominal_plans.items)[plan.index];
    if(!row.applied||row.complete||row.selected!=UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
    for(uint32_t f=0;f<row.count;++f) {
        if(!(f%64)){XrXirStatus status=body_poll(pool->control);if(status!=XR_XIR_OK)return status;}
        if(row.present[f]!=2)return XR_XIR_BAD_STRUCTURE;
    }
    XrHirNominalInfo info={0};if(!xr_hir_body_nominal_info(body,row.input.shape,&info))return XR_XIR_BAD_STRUCTURE;
    XrHirNominalUseInput input={.pool=pool,.shape=row.input.shape,.scope=row.input.scope,.origin=row.input.origin,.type=info.type,
        .values=row.values,.count=row.count,.variant=row.input.variant};
    XrXirStatus status=xr_hir_body_nominal_construct(body,&input,pool->control,out,reason);
    if(status==XR_XIR_OK)((HirBodyNominalPlan *)body->nominal_plans.items)[plan.index].complete=true;return status;
}
static XrXirStatus body_nominal_project(const XrHirBody *body,const HirBodyUse *row,
    const XrHirBodyProjection *projection,XrCompileRequestControl *control) {
    XrHirNominalInfo info={0};if(!xr_hir_body_nominal_info(body,row->nominal,&info)||!projection->has_instruction||
        !body->declarations.functions||!*body->declarations.functions||projection->function>=*body->declarations.function_count)return XR_XIR_BAD_STRUCTURE;
    XrXirFunction function=(*body->declarations.functions)[projection->function];
    if(projection->instruction>=function.instruction_count||!function.instructions)return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction actual=function.instructions[projection->instruction];
    if(actual.type!=row->action.target)return XR_XIR_BAD_STRUCTURE;
    if(row->nominal_helper) {
        if(!row->nominal_has_call||row->nominal_call>=projection->source_value_count||!projection->source_value_map)return XR_XIR_BAD_STRUCTURE;
        uint32_t anchor=projection->source_value_map[row->nominal_call];
        if(anchor<function.parameter_count||anchor-function.parameter_count>=function.instruction_count)return XR_XIR_BAD_STRUCTURE;
        anchor-=function.parameter_count;
        if(row->action.target!=XR_XIR_UNIT&&(!row->nominal_has_result||!projection->has_value||
            projection->source_value!=row->nominal_result||row->nominal_result>=projection->source_value_count||
            projection->value!=projection->source_value_map[row->nominal_result]))return XR_XIR_BAD_STRUCTURE;
        if(actual.op==XR_XIR_INVOKE_RESULT) {
            if(actual.immediate!=(int64_t)anchor)return XR_XIR_BAD_STRUCTURE;
            actual=function.instructions[anchor];
        } else if(projection->instruction!=anchor)return XR_XIR_BAD_STRUCTURE;
        if((actual.op!=XR_XIR_CALL&&actual.op!=XR_XIR_INVOKE)||!projection->has_call||
            actual.op!=row->nominal_call_operation||actual.type!=row->action.target||
            actual.immediate!=(int64_t)row->nominal_function||projection->call.immediate!=(int64_t)row->nominal_function||actual.args[1]||
            actual.type_arguments[1]!=info.argument_count||!body->nominal_generics||!*body->nominal_generics)return XR_XIR_BAD_STRUCTURE;
        XrXirGeneric arguments=(*body->nominal_generics)[projection->function];
        if(actual.type_arguments[0]>arguments.argument_count||info.argument_count>arguments.argument_count-actual.type_arguments[0]||
            (info.argument_count&&!arguments.arguments))return XR_XIR_BAD_STRUCTURE;
        for(uint32_t p=0;p<info.argument_count;++p) {
            if(!(p%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
            if(arguments.arguments[actual.type_arguments[0]+p]!=info.arguments[p])return XR_XIR_BAD_STRUCTURE;
        }
    } else if(row->nominal_construct) {
        if(actual.op!=row->action.operation||actual.immediate!=(int64_t)row->nominal_variant||
            actual.args[1]!=row->nominal_count||projection->source_argument_count!=row->nominal_count||
            actual.args[0]>function.operand_count||actual.args[1]>function.operand_count-actual.args[0]||
            (row->nominal_count&&(!function.operands||!projection->source_arguments)))return XR_XIR_BAD_STRUCTURE;
        for(uint32_t p=0;p<row->nominal_count;++p) {
            if(!(p%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
            XrHirNominalValue value=row->nominal_values[p];
            if(projection->source_arguments[p]!=value.value||value.value>=projection->source_value_count||!projection->source_value_map||
                function.operands[actual.args[0]+p]!=projection->source_value_map[value.value]||
                !body_tuple_value_type(&function,function.operands[actual.args[0]+p],value.type))return XR_XIR_BAD_STRUCTURE;
        }
    } else {
        if(actual.op!=row->action.operation||row->source_operand>=projection->source_value_count||!projection->source_value_map||
            actual.args[0]!=projection->source_value_map[row->source_operand]||!body_tuple_value_type(&function,actual.args[0],info.type))return XR_XIR_BAD_STRUCTURE;
        if(info.kind==XR_XIR_NOMINAL_ENUM) {
            if(actual.immediate!=(int64_t)row->nominal_variant||actual.args[1]!=row->nominal_field)return XR_XIR_BAD_STRUCTURE;
        } else if(actual.immediate!=(int64_t)row->nominal_field)return XR_XIR_BAD_STRUCTURE;
    }
    return body_poll(control);
}
XR_FUNC XrXirStatus xr_hir_body_nominal_call_emitted(XrHirBody *body,XrHirBodyUse handle,const XrHirNominalCallEmission *input) {
    if(!body||body->ended||!input||handle.owner!=body||handle.index>=body->count)return XR_XIR_BAD_STRUCTURE;
    HirBodyUse *row=&body->uses[handle.index];XrHirNominalInfo info={0};
    if(!row->nominal_helper||row->nominal_has_call||!xr_hir_body_nominal_info(body,row->nominal,&info)||
        (input->call.op!=XR_XIR_CALL&&input->call.op!=XR_XIR_INVOKE)||input->call.immediate!=(int64_t)row->nominal_function||
        input->call.type!=row->action.target||input->call.args[1]||input->call.type_arguments[1]!=info.argument_count||
        input->anchor==UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
    row->nominal_call=input->anchor;row->nominal_call_operation=input->call.op;row->nominal_has_call=true;return XR_XIR_OK;
}
