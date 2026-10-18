/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_type_info.inc.c - Ordinary owned static type-description graphs
 *
 * KEY CONCEPT:
 *   Logical graph references and ordinary Tuple/Array/String recipes carry
 *   canonical facts. They contain no host pointers or operation authority.
 */
typedef struct SourceInfoSchema {
    XrXirType field,fields,parameter,parameters,variant,variants,arguments,node,nodes,info;
} SourceInfoSchema;
typedef struct SourceInfoRows {
    SourceValue *values;
    uint32_t count,capacity;
} SourceInfoRows;
typedef struct SourceInfoGraph {
    SourceContext *ctx,*facts;
    AstNode *syntax;
    SourceInfoSchema schema;
    XrXirType *types;
    uint32_t *ordinals,count,capacity;
    SourceInfoRows fields,arguments,parameters,variants;
    const XrXirDerivationPolicy *policy;
} SourceInfoGraph;

static bool source_info_tuple_type(SourceContext *ctx,const XrXirType *types,
    uint32_t count,XrXirType *output) {
    XrXirCallableParameter *fields=source_alloc(ctx,count,sizeof(*fields));
    if (!fields) return false;
    for (uint32_t i=0;i<count;++i) {
        if (!source_work(ctx,NULL)) return false;
        fields[i].type=types[i];
    }
    return source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,
        .parameters=fields,.parameter_count=count},output);
}
static bool source_info_schema(SourceContext *ctx,SourceInfoSchema *s) {
    const XrXirType field[]={XR_XIR_STRING,XR_XIR_I64,XR_XIR_I64,XR_XIR_I64};
    const XrXirType parameter[]={XR_XIR_I64,XR_XIR_I64};
    if (!source_info_tuple_type(ctx,field,4,&s->field) ||
        !source_array_element_type(ctx,s->field,&s->fields) ||
        !source_info_tuple_type(ctx,parameter,2,&s->parameter) ||
        !source_array_element_type(ctx,s->parameter,&s->parameters) ||
        !source_array_element_type(ctx,XR_XIR_I64,&s->arguments)) return false;
    const XrXirType variant[]={XR_XIR_STRING,XR_XIR_I64,XR_XIR_I64};
    if (!source_info_tuple_type(ctx,variant,3,&s->variant) ||
        !source_array_element_type(ctx,s->variant,&s->variants)) return false;
    const XrXirType node[]={XR_XIR_I64,XR_XIR_I64,XR_XIR_STRING,XR_XIR_STRING,
        XR_XIR_I64,XR_XIR_I64,XR_XIR_I64,XR_XIR_I64,XR_XIR_I64,XR_XIR_I64,
        XR_XIR_I64,XR_XIR_I64,XR_XIR_I64,XR_XIR_I64,XR_XIR_I64,XR_XIR_I64};
    if (!source_info_tuple_type(ctx,node,16,&s->node) ||
        !source_array_element_type(ctx,s->node,&s->nodes)) return false;
    const XrXirType info[]={XR_XIR_I64,s->nodes,s->fields,s->arguments,s->parameters,s->variants};
    return source_info_tuple_type(ctx,info,6,&s->info);
}
static bool source_info_describe(SourceInfoGraph *g,XrXirType type,
    XrXirTypeDescription *description,XrXirTypeNode *copy) {
    XrXirStatus status=xr_xir_compile_type_description(&g->facts->compile,&g->facts->types,type,description);
    if (status!=XR_XIR_OK)
        return source_fail(g->ctx,g->syntax,status,"typeInfo requires an admitted closed static type");
    if (description->node) {
        *copy=*description->node;
        if (copy->parameter_span)
            return source_fail(g->ctx,g->syntax,XR_XIR_BAD_TYPE,"typeInfo cannot inspect an open generic type");
        description->node=copy;
        if (description->instance) description->instance=&copy->nominal;
    }
    return true;
}
static bool source_info_reserve(SourceInfoGraph *g,uint32_t index) {
    if (index>=g->facts->types.count+XR_XIR_CONSTRUCTED_TYPE_BASE)
        return source_fail(g->ctx,g->syntax,XR_XIR_BAD_TYPE,"typeInfo contains an unresolved type reference");
    if (index<g->capacity) return true;
    uint32_t capacity=g->capacity;
    while (capacity<=index) {
        if (!source_work(g->ctx,g->syntax)) return false;
        capacity=capacity>XR_XIR_CONSTRUCTED_TYPE_LIMIT/2?XR_XIR_CONSTRUCTED_TYPE_LIMIT:capacity*2;
    }
    XrXirType *types=source_alloc(g->facts,capacity,sizeof(*types));
    uint32_t *ordinals=source_alloc(g->facts,capacity,sizeof(*ordinals));
    if (!types || !ordinals || !source_work_units(g->ctx,g->syntax,(uint64_t)capacity*sizeof(*ordinals))) return false;
    for (uint32_t i=0;i<capacity;++i) ordinals[i]=UINT32_MAX;
    if (!source_copy_bytes(g->ctx,g->syntax,types,g->types,(size_t)g->count*sizeof(*types)) ||
        !source_copy_bytes(g->ctx,g->syntax,ordinals,g->ordinals,(size_t)g->capacity*sizeof(*ordinals))) return false;
    source_release_private(g->types);source_release_private(g->ordinals);
    g->types=types;g->ordinals=ordinals;g->capacity=capacity;return true;
}
static bool source_info_reference(SourceInfoGraph *g,XrXirType type,uint32_t *output) {
    uint32_t index=(uint32_t)type;
    if (!source_info_reserve(g,index)) return false;
    if (!source_work(g->ctx,g->syntax)) return false;
    if (g->ordinals[index]==UINT32_MAX) {
        if (g->count==g->capacity)
            return source_fail(g->ctx,g->syntax,XR_XIR_BUDGET,"typeInfo graph capacity exhausted");
        g->ordinals[index]=g->count;g->types[g->count++]=type;
    }
    *output=g->ordinals[index];return true;
}
static bool source_info_field(SourceInfoGraph *g,const XrXirTypeDescription *description,
    uint32_t ordinal,XrXirFieldDescription *field,bool *visible) {
    XrXirStatus status=xr_xir_compile_description_field(&g->facts->compile,description,ordinal,field);
    if (status!=XR_XIR_OK)
        return source_fail(g->ctx,g->syntax,status,"typeInfo field description failed");
    if (!field->resolved) {
        if (!description->definition || !description->instance)
            return source_fail(g->ctx,g->syntax,XR_XIR_BAD_TYPE,"typeInfo field has no canonical substitution");
        SourceSubstitution substitution={description->instance->arguments,description->instance->argument_count};
        if (!source_substitute(g->facts,&substitution,field->template_type,0,&field->type))
            return source_fail(g->ctx,g->syntax,g->facts->diagnostic.status,"typeInfo canonical field substitution failed");
        if (xr_xir_type_span(&g->facts->types,field->type))
            return source_fail(g->ctx,g->syntax,XR_XIR_BAD_TYPE,"typeInfo field remains an open type");
        field->resolved=true;
    }
    *visible=true;
    if (!(field->flags&(XR_XIR_FIELD_PRIVATE|XR_XIR_FIELD_PROTECTED))) return true;
    if (g->policy) {
        *visible=false;
        if (!description->instance || description->type!=g->policy->target) return true;
        for (uint32_t i=0;i<g->policy->member_count;++i) {
            if (!source_work(g->ctx,g->syntax)) return false;
            const XrXirDerivationMember *member=&g->policy->members[i];
            if (member->field==ordinal && (member->operations&XR_XIR_DERIVE_DESCRIBE)) *visible=true;
        }
        return true;
    }
    const XrXirSourceModule *owner=&g->facts->modules[g->facts->module];
    *visible=owner->name_length==description->module.length &&
        source_span_same(g->ctx,g->syntax,owner->name,description->module.bytes,owner->name_length);
    return g->ctx->diagnostic.status==XR_XIR_OK;
}
static bool source_info_edges(SourceInfoGraph *g,uint32_t at) {
    XrXirTypeDescription description={0};XrXirTypeNode copy={0};uint32_t reference;
    if (!source_info_describe(g,g->types[at],&description,&copy)) return false;
    uint32_t counts[]={description.field_count,copy.nominal.argument_count,
        copy.kind==XR_XIR_TYPE_CALLABLE?copy.parameter_count:0,description.variant_count};
    SourceInfoRows *rows[]={&g->fields,&g->arguments,&g->parameters,&g->variants};
    for (uint32_t i=0;i<4;++i) {
        if (counts[i]>UINT32_MAX-rows[i]->capacity)
            return source_fail(g->ctx,g->syntax,XR_XIR_BUDGET,"typeInfo fact table capacity exhausted");
        rows[i]->capacity+=counts[i];
    }
    for (uint32_t f=0;f<description.field_count;++f) {
        XrXirFieldDescription field={0};bool visible=false;
        if (!source_info_field(g,&description,f,&field,&visible)) return false;
        if (visible && !source_info_reference(g,field.type,&reference)) return false;
    }
    for (uint32_t a=0;a<copy.nominal.argument_count;++a)
        if (!source_info_reference(g,copy.nominal.arguments[a],&reference)) return false;
    for (uint32_t p=0;copy.kind==XR_XIR_TYPE_CALLABLE && p<copy.parameter_count;++p)
        if (!source_info_reference(g,copy.parameters[p].type,&reference)) return false;
    if (copy.kind==XR_XIR_TYPE_CALLABLE && !source_info_reference(g,copy.result,&reference)) return false;
    if ((copy.kind==XR_XIR_TYPE_ARRAY || copy.kind==XR_XIR_TYPE_CELL ||
        copy.kind==XR_XIR_TYPE_NULLABLE || copy.kind==XR_XIR_TYPE_ATOMIC ||
        copy.kind==XR_XIR_TYPE_TASK) && !source_info_reference(g,copy.element,&reference)) return false;
    return true;
}
static bool source_info_integer(SourceInfoGraph *g,int64_t number,SourceValue *output) {
    return source_recipe_record(g->ctx,(XrXirInstruction){.op=XR_XIR_CONST_INT,
        .type=XR_XIR_I64,.immediate=number},output);
}
static bool source_info_text(SourceInfoGraph *g,XrXirLiteral text,SourceValue *output) {
    return source_string_literal(g->ctx,g->syntax,text.bytes?text.bytes:"",text.length,output);
}
static bool source_info_group(SourceInfoGraph *g,XrXirType type,
    SourceValue *fields,uint32_t count,SourceValue *output) {
    return source_recipe_group(g->ctx,(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=type},fields,count,output);
}
static bool source_info_array(SourceInfoGraph *g,XrXirType type,
    SourceValue *elements,uint32_t count,SourceValue *output) {
    return source_recipe_group(g->ctx,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=type},elements,count,output);
}
static bool source_info_ref_value(SourceInfoGraph *g,XrXirType type,SourceValue *output) {
    uint32_t reference;
    return source_info_reference(g,type,&reference) && source_info_integer(g,reference,output);
}
static bool source_info_fields(SourceInfoGraph *g,const XrXirTypeDescription *description,uint32_t *map) {
    for (uint32_t ordinal=0;ordinal<description->field_count;++ordinal) {
        XrXirFieldDescription field={0};bool visible=false;map[ordinal]=g->fields.count;
        if (!source_info_field(g,description,ordinal,&field,&visible)) return false;
        if (!visible) continue;
        if (g->fields.count==g->fields.capacity)
            return source_fail(g->ctx,g->syntax,XR_XIR_BAD_STRUCTURE,"typeInfo field table changed");
        SourceValue fields[4];
        if (!source_info_text(g,field.name,&fields[0]) || !source_info_ref_value(g,field.type,&fields[1]) ||
            !source_info_integer(g,field.flags,&fields[2]) || !source_info_integer(g,ordinal,&fields[3]) ||
            !source_info_group(g,g->schema.field,fields,4,&g->fields.values[g->fields.count++])) return false;
    }
    map[description->field_count]=g->fields.count;return true;
}
static bool source_info_arguments(SourceInfoGraph *g,const XrXirTypeNode *node) {
    uint32_t count=node->nominal.argument_count;
    for (uint32_t i=0;i<count;++i) {
        if (g->arguments.count==g->arguments.capacity)
            return source_fail(g->ctx,g->syntax,XR_XIR_BAD_STRUCTURE,"typeInfo argument table changed");
        if (!source_info_ref_value(g,node->nominal.arguments[i],&g->arguments.values[g->arguments.count++])) return false;
    }
    return true;
}
static bool source_info_parameters(SourceInfoGraph *g,const XrXirTypeNode *node) {
    uint32_t count=node->kind==XR_XIR_TYPE_CALLABLE?node->parameter_count:0;
    for (uint32_t i=0;i<count;++i) {
        if (g->parameters.count==g->parameters.capacity)
            return source_fail(g->ctx,g->syntax,XR_XIR_BAD_STRUCTURE,"typeInfo parameter table changed");
        SourceValue fields[2];
        if (!source_info_ref_value(g,node->parameters[i].type,&fields[0]) ||
            !source_info_integer(g,node->parameters[i].mode,&fields[1]) ||
            !source_info_group(g,g->schema.parameter,fields,2,&g->parameters.values[g->parameters.count++])) return false;
    }
    return true;
}
static bool source_info_variants(SourceInfoGraph *g,const XrXirTypeDescription *description,
    const uint32_t *map) {
    uint32_t count=description->variant_count;
    for (uint32_t i=0;i<count;++i) {
        if (g->variants.count==g->variants.capacity)
            return source_fail(g->ctx,g->syntax,XR_XIR_BAD_STRUCTURE,"typeInfo variant table changed");
        const XrXirNominalVariant *variant=&description->variants[i];SourceValue fields[3];
        if (variant->field_begin>description->field_count ||
            variant->field_count>description->field_count-variant->field_begin)
            return source_fail(g->ctx,g->syntax,XR_XIR_BAD_STRUCTURE,"typeInfo variant has an invalid field span");
        uint32_t begin=map[variant->field_begin],end=map[variant->field_begin+variant->field_count];
        if (!source_info_text(g,variant->name,&fields[0]) ||
            !source_info_integer(g,begin,&fields[1]) || !source_info_integer(g,end-begin,&fields[2]) ||
            !source_info_group(g,g->schema.variant,fields,3,&g->variants.values[g->variants.count++])) return false;
    }
    return true;
}
static bool source_info_node(SourceInfoGraph *g,uint32_t at,SourceValue *output) {
    XrXirTypeDescription description={0};XrXirTypeNode node={0};SourceValue fields[16];
    if (!source_info_describe(g,g->types[at],&description,&node)) return false;
    XrXirLiteral name=description.name;
    if (!description.kind) {
        const char *scalar=source_scalar_type_name(g->types[at]);
        if (!scalar) scalar=g->types[at]==XR_XIR_ERROR?"Error":"PanicInfo";
        name=(XrXirLiteral){scalar,(uint32_t)source_text_size(g->ctx,scalar)};
    }
    uint32_t flags=description.definition?description.definition->flags:
        description.identity?description.identity->flags:node.flags;
    uint32_t first_field=g->fields.count,first_argument=g->arguments.count;
    uint32_t first_parameter=g->parameters.count,first_variant=g->variants.count;
    uint32_t *map=source_alloc(g->ctx,(uint64_t)description.field_count+1,sizeof(*map));
    if (!map || !source_info_fields(g,&description,map) || !source_info_arguments(g,&node) ||
        !source_info_parameters(g,&node) || !source_info_variants(g,&description,map)) return false;
    source_release_private(map);
    if (!source_info_integer(g,description.kind,&fields[0]) ||
        !source_info_integer(g,description.kind?-1:(int64_t)g->types[at],&fields[1]) ||
        !source_info_text(g,description.module,&fields[2]) || !source_info_text(g,name,&fields[3]) ||
        !source_info_integer(g,description.nominal_kind,&fields[4]) ||
        !source_info_integer(g,flags,&fields[5]) ||
        !source_info_integer(g,first_field,&fields[6]) || !source_info_integer(g,g->fields.count-first_field,&fields[7]) ||
        !source_info_integer(g,first_argument,&fields[8]) || !source_info_integer(g,g->arguments.count-first_argument,&fields[9])) return false;
    bool element=node.kind==XR_XIR_TYPE_ARRAY || node.kind==XR_XIR_TYPE_CELL ||
        node.kind==XR_XIR_TYPE_NULLABLE || node.kind==XR_XIR_TYPE_ATOMIC || node.kind==XR_XIR_TYPE_TASK;
    if (!(element?source_info_ref_value(g,node.element,&fields[10]):source_info_integer(g,-1,&fields[10])) ||
        !source_info_integer(g,first_parameter,&fields[11]) || !source_info_integer(g,g->parameters.count-first_parameter,&fields[12]) ||
        !(node.kind==XR_XIR_TYPE_CALLABLE?source_info_ref_value(g,node.result,&fields[13]):source_info_integer(g,-1,&fields[13])) ||
        !source_info_integer(g,first_variant,&fields[14]) || !source_info_integer(g,g->variants.count-first_variant,&fields[15])) return false;
    return source_info_group(g,g->schema.node,fields,16,output);
}
static bool source_info_query(SourceInfoGraph *g,const XrCoreIntrinsicDesc *descriptor) {
    SourceContext *ctx=g->ctx;
    if (!ctx->type_info_declaration) {
        XrXirSourceQueryModule *modules=source_query_append(ctx,ctx->query.modules,
            &ctx->query.module_count,&ctx->query_module_capacity,sizeof(*modules));
        if (!modules) return false;
        ctx->query.modules=modules;
        modules[ctx->query.module_count-1]=(XrXirSourceQueryModule){"xray-core:prelude",NULL,{{0}}};
        SourceName symbol={.name=source_owned_text(ctx,descriptor->source_name)};
        if (!symbol.name || !source_query_declare(ctx,&symbol,XR_XIR_SOURCE_INTRINSIC,0,
            (XrXirSourceRange){ctx->query.module_count-1,0,0,0,0})) return false;
        ctx->type_info_declaration=symbol.declaration;
        XrXirSourceDeclaration *record=(XrXirSourceDeclaration *)&ctx->query.declarations[symbol.declaration-1];
        record->native_identity=descriptor->id;record->exported=true;
        record->type=source_query_type(ctx,g->schema.info);
        XrXirConstraint *constraint=source_alloc(ctx,1,sizeof(*constraint));
        if (!constraint) return false;
        record->generic_constraints=constraint;record->generic_parameter_count=1;
    }
    return source_query_target_token_reference(ctx,g->syntax->as.call_expr.callee,
        ctx->type_info_declaration,XR_XIR_SOURCE_CALL);
}
/* Fact references belong to the queried Source pool. Recipes belong to the
 * emitting function's pool; both contexts must retain the original ledger. */
static bool source_info_prepare(SourceInfoGraph *g,XrXirType target) {
    SourceContext *ctx=g->ctx;AstNode *node=g->syntax;
    if (ctx->compile.resources!=g->facts->compile.resources)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"typeInfo contexts do not share the original ledger");
    if (!source_info_schema(ctx,&g->schema)) return false;
    if (g->facts->types.count>UINT32_MAX-XR_XIR_CONSTRUCTED_TYPE_BASE)
        return source_fail(ctx,node,XR_XIR_BUDGET,"typeInfo reference capacity exhausted");
    g->capacity=g->facts->types.count+XR_XIR_CONSTRUCTED_TYPE_BASE;
    g->types=source_alloc(g->facts,g->capacity,sizeof(*g->types));
    g->ordinals=source_alloc(g->facts,g->capacity,sizeof(*g->ordinals));
    if (!g->types || !g->ordinals ||
        !source_work_units(ctx,node,(uint64_t)g->capacity*sizeof(*g->ordinals))) return false;
    for (uint32_t i=0;i<g->capacity;++i) g->ordinals[i]=UINT32_MAX;
    uint32_t root;
    return source_info_reference(g,target,&root) && root==0;
}
static bool source_info_materialize(SourceInfoGraph *g,XrXirType target,SourceValue *output) {
    SourceContext *ctx=g->ctx;AstNode *node=g->syntax;
    if (!g->capacity && !source_info_prepare(g,target)) return false;
    for (uint32_t i=0;i<g->count;++i) if (!source_info_edges(g,i)) return false;
    SourceInfoRows *tables[]={&g->fields,&g->arguments,&g->parameters,&g->variants};
    for (uint32_t i=0;i<4;++i) {
        if (!tables[i]->capacity) continue;
        tables[i]->values=source_recipe_storage(ctx,tables[i]->capacity,sizeof(*tables[i]->values));
        if (!tables[i]->values) return false;
    }
    uint32_t count=g->count;
    SourceValue *nodes=source_recipe_storage(ctx,count,sizeof(*nodes));
    if (!nodes) return false;
    for (uint32_t i=0;i<count;++i) if (!source_info_node(g,i,&nodes[i])) return false;
    if (g->count!=count)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"typeInfo graph changed during materialization");
    SourceValue fields[6];
    return source_info_integer(g,0,&fields[0]) &&
        source_info_array(g,g->schema.nodes,nodes,count,&fields[1]) &&
        source_info_array(g,g->schema.fields,g->fields.values,g->fields.count,&fields[2]) &&
        source_info_array(g,g->schema.arguments,g->arguments.values,g->arguments.count,&fields[3]) &&
        source_info_array(g,g->schema.parameters,g->parameters.values,g->parameters.count,&fields[4]) &&
        source_info_array(g,g->schema.variants,g->variants.values,g->variants.count,&fields[5]) &&
        source_info_group(g,g->schema.info,fields,6,output);
}
static bool source_type_info(SourceContext *ctx,AstNode *node,
    const XrCoreIntrinsicDesc *descriptor,SourceValue *output) {
    const CallExprNode *call=&node->as.call_expr;
    if (!descriptor || descriptor->id!=XR_CORE_BUILTIN_TYPE_INFO ||
        !xr_core_intrinsic_descriptor_validate(descriptor) || call->arg_count ||
        call->default_arg_count || call->type_arg_count!=1 || !call->type_args)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"typeInfo requires one explicit closed type and no values");
    XrXirType target=XR_XIR_UNIT;
    if (!source_type(ctx,call->type_args[0],&target)) return false;
    SourceInfoGraph graph={.ctx=ctx,.facts=ctx,.syntax=node};
    return source_info_materialize(&graph,target,output) && source_info_query(&graph,descriptor);
}
