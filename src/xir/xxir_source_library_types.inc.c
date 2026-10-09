/* Closed declaration import. All arrays and strings belong to SourceMemory;
 * input pointers are temporary lookup keys, never part of the published XIR. */
struct SourceLibraryUnit {
    SourceLibraryUnit *next;
    const XrXirModule *library;
    const XrXirArtifact *artifact;
    const XrXirConstruction *construction;
    uint32_t *nominals, *interfaces;
    XrXirType *types;
    uint32_t type_count;
    XrXirImplementationTable implementations;
};
static bool source_library_literal_same(SourceContext *ctx, XrXirLiteral a,
    const char *b, uint32_t count) {
    if (a.length != count) return false;
    return source_span_same(ctx,NULL,a.bytes,b,count);
}
static bool source_library_declaration_inventory(SourceContext *ctx, uint32_t module,
    const XrXirModule *library, uint32_t *nominals, uint32_t *interfaces) {
    if (!library->types) return true;
    const XrModuleResourceBinding *resource=ctx->graph->specs[module].resource;
    const XrXirSourceModule *owner=&library->declarations->modules[resource->checked_module];
    const XrXirNominalTable *nt=library->types->nominals;
    const XrXirInterfaceTable *it=library->types->interfaces;
    for (uint32_t n=0; nt && n<nt->count; ++n) {
        if (!source_work(ctx,NULL)) return false;
        if (!source_library_literal_same(ctx,nt->declarations[n].module,owner->name,owner->name_length)) continue;
        if (*nominals==UINT32_MAX) return source_fail(ctx,NULL,XR_XIR_BUDGET,"library nominal inventory exhausted");
        ++*nominals;
    }
    for (uint32_t i=0; it && i<it->count; ++i) {
        if (!source_work(ctx,NULL)) return false;
        if (!source_library_literal_same(ctx,it->declarations[i].module,owner->name,owner->name_length)) continue;
        if (*interfaces==UINT32_MAX) return source_fail(ctx,NULL,XR_XIR_BUDGET,"library interface inventory exhausted");
        ++*interfaces;
    }
    return ctx->diagnostic.status==XR_XIR_OK;
}
static SourceLibraryUnit *source_library_unit(SourceContext *ctx, const XrXirModule *library) {
    for (SourceLibraryUnit *u=ctx->library_units; u; u=u->next) {
        if (!source_work(ctx,NULL)) return NULL;
        if (u->library==library) return u;
    }
    return NULL;
}
static uint32_t source_library_declaration_module(SourceContext *ctx,
    const SourceLibraryUnit *unit, XrXirLiteral module) {
    for (uint32_t m=0; m<(uint32_t)ctx->graph->spec_count; ++m) {
        if (!source_work(ctx,NULL)) return UINT32_MAX;
        const XrModuleSpec *spec=&ctx->graph->specs[m];
        const XrModuleResourceBinding *r=spec->resource;
        if (spec->representation!=XR_MODULE_CHECKED_LIBRARY || !r || r->checked!=unit->artifact) continue;
        const XrXirSourceModule *owner=&unit->library->declarations->modules[r->checked_module];
        if (source_library_literal_same(ctx,module,owner->name,owner->name_length)) return m;
    }
    return UINT32_MAX;
}
static char *source_library_name(SourceContext *ctx, XrXirLiteral literal) {
    char *s=source_alloc(ctx,(uint64_t)literal.length+1,1);
    if (!s || !source_copy_bytes(ctx,NULL,s,literal.bytes,literal.length)) return NULL;
    s[literal.length]=0; return s;
}
static bool source_library_reserve(SourceContext *ctx, uint32_t module) {
    const XrModuleResourceBinding *resource=ctx->graph->specs[module].resource;
    const XrXirModule *library=xr_xir_compile_artifact_module(resource->checked);
    if (!library->types) return true;
    SourceLibraryUnit *unit=source_alloc(ctx,1,sizeof(*unit));
    if (!unit) return false;
    unit->library=library; unit->artifact=resource->checked;
    unit->construction=xr_xir_compile_artifact_construction(unit->artifact);
    const XrXirNominalTable *source_nominals=library->types->nominals;
    uint32_t source_count=source_nominals ? source_nominals->count : 0;
    if (!unit->construction || xr_xir_compile_construction_count(unit->construction)!=source_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library construction owner is missing or incomplete");
    unit->next=ctx->library_units; ctx->library_units=unit;
    const XrXirNominalTable *nt=library->types->nominals;
    const XrXirInterfaceTable *it=library->types->interfaces;
    unit->nominals=nt && nt->count ? source_alloc(ctx,nt->count,sizeof(*unit->nominals)) : NULL;
    unit->interfaces=it && it->count ? source_alloc(ctx,it->count,sizeof(*unit->interfaces)) : NULL;
    unit->type_count=library->types->count;
    unit->types=unit->type_count ? source_alloc(ctx,unit->type_count,sizeof(*unit->types)) : NULL;
    if ((nt && nt->count && !unit->nominals) || (it && it->count && !unit->interfaces) ||
        (unit->type_count && !unit->types)) return false;
    for (uint32_t t=0; t<unit->type_count; ++t) {
        if (!source_work(ctx,NULL)) return false;
        unit->types[t]=(XrXirType)UINT32_MAX;
    }
    for (uint32_t n=0; nt && n<nt->count; ++n) {
        if (!source_work(ctx,NULL)) return false;
        const XrXirNominalDeclaration *in=&nt->declarations[n];
        uint32_t m=source_library_declaration_module(ctx,unit,in->module);
        /* This bounded slice requires the unit's declaration closure to lie in
         * actually reached module views. It adds no initialization edges. */
        if (m==UINT32_MAX) return source_fail(ctx,NULL,XR_XIR_BAD_STAGE,"library nominal declaration is outside selected module closure");
        ctx->module=m;
        char *name=source_library_name(ctx,in->name);
        if (!name) return false;
        SourceName *symbol=add_name(ctx,&ctx->names[m],name,NULL);
        if (!symbol) return false;
        symbol->kind=SOURCE_NOMINAL; symbol->module=m; symbol->index=ctx->nominals.count++;
        unit->nominals[n]=symbol->index; ctx->nominal_sources[symbol->index]=symbol;
        XrXirNominalDeclaration *out=(XrXirNominalDeclaration *)&ctx->nominals.declarations[symbol->index];
        *out=*in;
        out->module=(XrXirLiteral){ctx->graph->specs[m].canonical,(uint32_t)source_text_size(ctx,ctx->graph->specs[m].canonical)};
        out->name=(XrXirLiteral){name,in->name.length};
        out->constraints=NULL; out->fields=NULL; out->field_count=0; out->variants=NULL;
        const XrXirConstructionRow *construction=xr_xir_compile_construction_row(unit->construction,n);
        if (!construction || construction->field_count!=in->field_count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library nominal construction row is incomplete");
        ctx->nominal_defaultable[symbol->index]=construction->default_initializer!=0;
        if (!source_query_declare(ctx,symbol,XR_XIR_SOURCE_TYPE,0,(XrXirSourceRange){m,0,0,0,0})) return false;
        ((XrXirSourceDeclaration *)ctx->query.declarations)[symbol->declaration-1].exported=in->exported;
        ((XrXirSourceDeclaration *)ctx->query.declarations)[symbol->declaration-1].native_identity=in->native.native_id;
    }
    for (uint32_t i=0; it && i<it->count; ++i) {
        if (!source_work(ctx,NULL)) return false;
        const XrXirInterfaceDeclaration *in=&it->declarations[i];
        uint32_t m=source_library_declaration_module(ctx,unit,in->module);
        if (m==UINT32_MAX) return source_fail(ctx,NULL,XR_XIR_BAD_STAGE,"library interface declaration is outside selected module closure");
        ctx->module=m;
        char *name=source_library_name(ctx,in->name);
        if (!name) return false;
        SourceName *symbol=add_name(ctx,&ctx->names[m],name,NULL);
        if (!symbol) return false;
        symbol->kind=SOURCE_INTERFACE; symbol->module=m; symbol->index=ctx->interfaces.count++;
        unit->interfaces[i]=symbol->index; ctx->interface_sources[symbol->index]=symbol;
        XrXirInterfaceDeclaration *out=(XrXirInterfaceDeclaration *)&ctx->interfaces.declarations[symbol->index];
        *out=*in;
        out->module=(XrXirLiteral){ctx->graph->specs[m].canonical,(uint32_t)source_text_size(ctx,ctx->graph->specs[m].canonical)};
        out->name=(XrXirLiteral){name,in->name.length};
        out->constraints=NULL;out->parents=NULL;out->methods=NULL;out->method_count=0;
        if (!source_query_declare(ctx,symbol,XR_XIR_SOURCE_TYPE,0,(XrXirSourceRange){m,0,0,0,0})) return false;
        ((XrXirSourceDeclaration *)ctx->query.declarations)[symbol->declaration-1].exported=in->exported;
    }
    return ctx->diagnostic.status==XR_XIR_OK;
}
static bool source_library_type(SourceContext *ctx, const SourceLibraryUnit *unit,
    XrXirType input, XrXirType *output) {
    if (!source_work(ctx,NULL)) return false;
    uint32_t t=(uint32_t)input;
    if (t>=XR_XIR_CONSTRUCTED_TYPE_BASE && t<XR_XIR_CONSTRUCTED_TYPE_LIMIT) {
        t-=XR_XIR_CONSTRUCTED_TYPE_BASE;
        if (!unit || t>=unit->type_count || (uint32_t)unit->types[t]==UINT32_MAX)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library constructed type is not mapped");
        *output=unit->types[t];
    } else *output=input;
    return true;
}
static bool source_library_type_arguments(SourceContext *ctx,const SourceLibraryUnit *unit,
    const XrXirType *input,uint32_t count,const XrXirType **output) {
    if (count && !input) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library type argument storage is missing");
    XrXirType *arguments=count ? source_alloc(ctx,count,sizeof(*arguments)) : NULL;
    if (count && !arguments) return false;
    for (uint32_t a=0; a<count; ++a)
        if (!source_library_type(ctx,unit,input[a],&arguments[a])) return false;
    *output=arguments;return true;
}
static bool source_library_applications(SourceContext *ctx,const SourceLibraryUnit *unit,
    const XrXirInterfaceApplication *input,uint32_t count,const XrXirInterfaceApplication **output) {
    if (count && (!input || !unit || !unit->library->types || !unit->library->types->interfaces))
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library interface application storage is missing");
    XrXirInterfaceApplication *applications=count ? source_alloc(ctx,count,sizeof(*applications)) : NULL;
    if (count && !applications) return false;
    for (uint32_t a=0; a<count; ++a) {
        if (!source_work(ctx,NULL)) return false;
        if (input[a].declaration>=unit->library->types->interfaces->count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library interface application is not mapped");
        applications[a].declaration=unit->interfaces[input[a].declaration];
        applications[a].argument_count=input[a].argument_count;
        if (!source_library_type_arguments(ctx,unit,input[a].arguments,input[a].argument_count,
            &applications[a].arguments)) return false;
    }
    *output=applications;return true;
}
static bool source_library_constraints(SourceContext *ctx,const SourceLibraryUnit *unit,
    const XrXirConstraint *input,uint32_t count,const XrXirConstraint **output) {
    if (count && !input) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library constraint storage is missing");
    XrXirConstraint *constraints=count ? source_alloc(ctx,count,sizeof(*constraints)) : NULL;
    if (count && !constraints) return false;
    for (uint32_t p=0; p<count; ++p) {
        if (!source_work(ctx,NULL)) return false;
        constraints[p].markers=input[p].markers;
        constraints[p].interface_count=input[p].interface_count;
        if (!source_library_applications(ctx,unit,input[p].interfaces,input[p].interface_count,
            &constraints[p].interfaces)) return false;
    }
    *output=constraints;return true;
}
static void source_library_query_type(SourceContext *ctx,uint32_t declaration,
    XrXirType type,uint32_t owner) {
    XrXirSourceDeclaration *record=(XrXirSourceDeclaration *)&ctx->query.declarations[declaration-1];
    record->type=(XrXirSourceType){type,xr_xir_type_span(&ctx->types,type) ? owner : 0,true};
}
static bool source_library_intern_types(SourceContext *ctx, SourceLibraryUnit *unit) {
    const XrXirTypes *pool=unit->library->types;
    for (uint32_t t=0; t<unit->type_count; ++t) {
        if (!source_work(ctx,NULL)) return false;
        const XrXirTypeNode *in=&pool->nodes[t]; XrXirTypeNode out=*in;
        out.parameters=NULL;out.nominal.arguments=NULL;out.nominal.fields=NULL;
        out.nominal.field_count=0;
        if (in->kind==XR_XIR_TYPE_NOMINAL) {
            if (!pool->nominals || in->nominal.declaration>=pool->nominals->count)
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library nominal type identity is invalid");
            out.nominal.declaration=unit->nominals[in->nominal.declaration];
            if (!source_library_type_arguments(ctx,unit,in->nominal.arguments,in->nominal.argument_count,
                &out.nominal.arguments)) return false;
        } else if (in->kind==XR_XIR_TYPE_CALLABLE || in->kind==XR_XIR_TYPE_TUPLE) {
            XrXirCallableParameter *parameters=in->parameter_count ? source_alloc(ctx,in->parameter_count,sizeof(*parameters)) : NULL;
            if (in->parameter_count && !parameters) return false;
            for (uint32_t p=0; p<in->parameter_count; ++p) {
                parameters[p]=in->parameters[p];
                if (!source_library_type(ctx,unit,in->parameters[p].type,&parameters[p].type)) return false;
            }
            out.parameters=parameters;
            if (!source_library_type(ctx,unit,in->result,&out.result)) return false;
        } else if (in->kind==XR_XIR_TYPE_ARRAY || in->kind==XR_XIR_TYPE_CELL ||
            in->kind==XR_XIR_TYPE_NULLABLE || in->kind==XR_XIR_TYPE_ATOMIC || in->kind==XR_XIR_TYPE_TASK) {
            if (!source_library_type(ctx,unit,in->element,&out.element)) return false;
        } else return source_fail(ctx,NULL,XR_XIR_BAD_STAGE,"library type remapping is not admitted");
        if (!source_intern_type(ctx,out,&unit->types[t])) return false;
    }
    return true;
}
/* Skeleton IDs exist for all declarations before any application is copied.
 * Type IDs above are canonical intern results, not an arithmetic pool offset. */
static bool source_library_declaration_constraints(SourceContext *ctx,SourceLibraryUnit *unit) {
    const XrXirNominalTable *nominals=unit->library->types->nominals;
    const XrXirInterfaceTable *interfaces=unit->library->types->interfaces;
    for (uint32_t n=0; nominals && n<nominals->count; ++n) {
        if (!source_work(ctx,NULL)) return false;
        uint32_t target=unit->nominals[n];
        XrXirNominalDeclaration *to=(XrXirNominalDeclaration *)&ctx->nominals.declarations[target];
        const XrXirNominalDeclaration *from=&nominals->declarations[n];
        if (!source_library_constraints(ctx,unit,from->constraints,from->parameter_count,&to->constraints)) return false;
        XrXirSourceDeclaration *query=(XrXirSourceDeclaration *)&ctx->query.declarations[ctx->nominal_sources[target]->declaration-1];
        query->generic_parameter_count=to->parameter_count;query->generic_constraints=to->constraints;
    }
    for (uint32_t i=0; interfaces && i<interfaces->count; ++i) {
        if (!source_work(ctx,NULL)) return false;
        uint32_t target=unit->interfaces[i];
        XrXirInterfaceDeclaration *to=(XrXirInterfaceDeclaration *)&ctx->interfaces.declarations[target];
        const XrXirInterfaceDeclaration *from=&interfaces->declarations[i];
        if (!source_library_constraints(ctx,unit,from->constraints,from->parameter_count,&to->constraints) ||
            !source_library_applications(ctx,unit,from->parents,from->parent_count,&to->parents)) return false;
        XrXirSourceDeclaration *query=(XrXirSourceDeclaration *)&ctx->query.declarations[ctx->interface_sources[target]->declaration-1];
        query->generic_parameter_count=to->parameter_count;query->generic_constraints=to->constraints;
    }
    return true;
}
/* Variant and payload query identities have the same parent relation as Source
 * declarations, but every fact and name comes from the verified owned table. */
static bool source_library_variants(SourceContext *ctx,SourceName *owner,
    const XrXirNominalDeclaration *input,XrXirNominalDeclaration *output) {
    if (input->kind!=XR_XIR_NOMINAL_ENUM) return true;
    if (input->variant_count>UINT32_MAX-3)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"library enum query inventory exhausted");
    XrXirNominalVariant *variants=source_alloc(ctx,input->variant_count,sizeof(*variants));
    uint32_t *ids=source_alloc(ctx,(uint64_t)input->variant_count+3,sizeof(*ids));
    if (!variants || !ids) return false;
    for (uint32_t v=0; v<input->variant_count; ++v) {
        if (!source_work(ctx,NULL)) return false;
        const XrXirNominalVariant *from=&input->variants[v];
        char *name=source_library_name(ctx,from->name);
        if (!name) return false;
        variants[v]=(XrXirNominalVariant){{name,from->name.length},from->field_begin,from->field_count};
        SourceName symbol={0};symbol.name=name;
        if (!source_query_declare(ctx,&symbol,XR_XIR_SOURCE_MEMBER,owner->declaration,
            (XrXirSourceRange){owner->module,0,0,0,0})) return false;
        ids[v]=symbol.declaration;
    }
    ctx->nominal_variants[owner->index]=ids;
    output->variants=variants;output->variant_count=input->variant_count;
    return true;
}
static bool source_library_enum_query(SourceContext *ctx,SourceName *owner) {
    const XrXirNominalDeclaration *record=&ctx->nominals.declarations[owner->index];
    if (record->kind!=XR_XIR_NOMINAL_ENUM) return true;
    uint32_t *ids=ctx->nominal_variants[owner->index];
    for (uint32_t v=0; v<record->variant_count; ++v) {
        if (!source_work(ctx,NULL)) return false;
        source_library_query_type(ctx,ids[v],owner->type,owner->declaration);
    }
    const char *names[]={"ordinal","name","toString"};
    for (uint32_t i=0; i<3; ++i) {
        if (!source_work(ctx,NULL)) return false;
        SourceName symbol={0};symbol.name=source_owned_text(ctx,names[i]);
        symbol.type=i ? XR_XIR_STRING : XR_XIR_I64;
        if (!symbol.name || !source_query_declare(ctx,&symbol,XR_XIR_SOURCE_INTRINSIC,owner->declaration,
            (XrXirSourceRange){owner->module,0,0,0,0})) return false;
        source_library_query_type(ctx,symbol.declaration,symbol.type,0);
        ids[record->variant_count+i]=symbol.declaration;
    }
    return true;
}
static bool source_library_fields(SourceContext *ctx, SourceLibraryUnit *unit) {
    const XrXirNominalTable *table=unit->library->types->nominals;
    for (uint32_t n=0; table && n<table->count; ++n) {
        if (!source_work(ctx,NULL)) return false;
        uint32_t target=unit->nominals[n];SourceName *symbol=ctx->nominal_sources[target];
        ctx->module=symbol->module;ctx->function=symbol->module;
        const XrXirNominalDeclaration *in=&table->declarations[n];
        XrXirNominalDeclaration *out=(XrXirNominalDeclaration *)&ctx->nominals.declarations[target];
        uint32_t count=in->field_count;
        if (!source_library_variants(ctx,symbol,in,out)) return false;
        XrXirNominalField *fields=count ? source_alloc(ctx,count,sizeof(*fields)) : NULL;
        XrXirType *types=count ? source_alloc(ctx,count,sizeof(*types)) : NULL;
        uint32_t *members=count ? source_alloc(ctx,count,sizeof(*members)) : NULL;
        uint32_t *defaults=count ? source_alloc(ctx,count,sizeof(*defaults)) : NULL;
        if (count && (!fields || !types || !members || !defaults)) return false;
        for (uint32_t f=0; f<count; ++f) {
            if (!source_work(ctx,NULL)) return false;
            char *name=source_library_name(ctx,in->fields[f].name);
            if (!name || !source_library_type(ctx,unit,in->fields[f].type,&types[f])) return false;
            fields[f]=(XrXirNominalField){{name,in->fields[f].name.length},types[f],in->fields[f].flags};
            SourceName member={0};member.name=name;member.type=types[f];
            member.mutable=(fields[f].flags & XR_XIR_FIELD_MUTABLE)!=0;
            uint32_t parent=symbol->declaration;
            if (in->kind==XR_XIR_NOMINAL_ENUM) {
                bool found=false;
                for (uint32_t v=0; v<in->variant_count; ++v) {
                    if (!source_work(ctx,NULL)) return false;
                    const XrXirNominalVariant *variant=&in->variants[v];
                    if (f>=variant->field_begin && f-variant->field_begin<variant->field_count) {
                        parent=ctx->nominal_variants[target][v];found=true;break;
                    }
                }
                if (!found) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library enum payload is outside variant ownership");
            }
            if (!source_query_declare(ctx,&member,XR_XIR_SOURCE_MEMBER,parent,
                (XrXirSourceRange){symbol->module,0,0,0,0})) return false;
            source_query_binding_type(ctx,&member);members[f]=member.declaration;
            source_library_query_type(ctx,member.declaration,types[f],symbol->declaration);
        }
        out->fields=fields;out->field_count=count;
        ctx->nominal_members[target]=members;ctx->nominal_defaults[target]=defaults;
        XrXirTypeNode type={0};type.kind=XR_XIR_TYPE_NOMINAL;
        XrXirType *arguments=in->parameter_count ? source_alloc(ctx,in->parameter_count,sizeof(*arguments)) : NULL;
        if (in->parameter_count && !arguments) return false;
        for (uint32_t a=0; a<in->parameter_count; ++a) {
            if (!source_work(ctx,NULL)) return false;
            arguments[a]=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+a);
        }
        type.nominal=(XrXirNominalType){target,arguments,in->parameter_count,types,count};
        if (!source_intern_type(ctx,type,&symbol->type)) return false;
        /* Interning may have returned the skeleton, and may have grown the pool.
         * Reacquire by ID; never retain a pool element pointer over interning. */
        XrXirTypeNode *owned=(XrXirTypeNode *)&ctx->types.nodes[(uint32_t)symbol->type-XR_XIR_CONSTRUCTED_TYPE_BASE];
        owned->nominal.fields=types;owned->nominal.field_count=count;
        source_query_binding_type(ctx,symbol);
        source_library_query_type(ctx,symbol->declaration,symbol->type,symbol->declaration);
        symbol->checked_library=true;
        if (!source_library_enum_query(ctx,symbol)) return false;
    }
    return true;
}
static bool source_library_interfaces(SourceContext *ctx, SourceLibraryUnit *unit) {
    const XrXirInterfaceTable *table=unit->library->types->interfaces;
    for (uint32_t i=0; table && i<table->count; ++i) {
        if (!source_work(ctx,NULL)) return false;
        uint32_t target=unit->interfaces[i];SourceName *owner=ctx->interface_sources[target];
        ctx->module=owner->module;ctx->function=owner->module;
        const XrXirInterfaceDeclaration *in=&table->declarations[i];
        XrXirInterfaceDeclaration *out=(XrXirInterfaceDeclaration *)&ctx->interfaces.declarations[target];
        uint32_t count=in->method_count;
        XrXirInterfaceMethod *methods=count ? source_alloc(ctx,count,sizeof(*methods)) : NULL;
        uint32_t *ids=count ? source_alloc(ctx,count,sizeof(*ids)) : NULL;
        if (count && (!methods || !ids)) return false;
        ctx->interface_member_declarations[target]=ids;
        out->methods=methods;out->method_count=count;
        for (uint32_t m=0; m<count; ++m) {
            if (!source_work(ctx,NULL)) return false;
            methods[m]=in->methods[m];
            char *name=source_library_name(ctx,in->methods[m].name);
            if (!name || !source_library_type(ctx,unit,in->methods[m].signature,&methods[m].signature)) return false;
            methods[m].name=(XrXirLiteral){name,in->methods[m].name.length};
            if (!source_library_constraints(ctx,unit,in->methods[m].constraints,
                in->methods[m].own_parameter_count,&methods[m].constraints)) return false;
            SourceName *member=add_name(ctx,&ctx->interface_members[target],name,NULL);
            if (!member) return false;
            member->kind=SOURCE_INTERFACE;member->index=m;member->module=owner->module;
            if (!source_query_declare(ctx,member,XR_XIR_SOURCE_MEMBER,owner->declaration,
                (XrXirSourceRange){owner->module,0,0,0,0})) return false;
            ids[m]=member->declaration;
            if (!source_interface_method_query(ctx,owner,m,&methods[m])) return false;
            /* Query binders come from installed metadata, without synthesizing AST names. */
            uint32_t generic_owner=methods[m].own_parameter_count ? member->declaration :
                in->parameter_count ? owner->declaration : 0;
            const XrXirTypeNode *signature=xr_xir_callable_signature(&ctx->types,methods[m].signature);
            XrXirSourceDeclaration *query=(XrXirSourceDeclaration *)&ctx->query.declarations[member->declaration-1];
            source_library_query_type(ctx,member->declaration,signature->result,generic_owner);
            XrXirSourceType *parameters=(XrXirSourceType *)query->parameters;
            for (uint32_t p=0; p<signature->parameter_count; ++p) {
                if (!source_work(ctx,NULL)) return false;
                parameters[p].generic_owner=xr_xir_type_span(&ctx->types,signature->parameters[p].type) ? generic_owner : 0;
            }
        }
        owner->checked_library=true;
    }
    return true;
}
static bool source_library_metadata(SourceContext *ctx) {
    uint32_t module=ctx->module,function=ctx->function;SourceTypeScope scope=ctx->type_scope;
    ctx->type_scope=(SourceTypeScope){0};
    bool ok=true;
    for (SourceLibraryUnit *u=ctx->library_units; u && ok; u=u->next)
        ok=source_library_intern_types(ctx,u);
    for (SourceLibraryUnit *u=ctx->library_units; u && ok; u=u->next)
        ok=source_library_declaration_constraints(ctx,u);
    for (SourceLibraryUnit *u=ctx->library_units; u && ok; u=u->next)
        ok=source_library_fields(ctx,u) && source_library_interfaces(ctx,u);
    ctx->module=module;ctx->function=function;ctx->type_scope=scope;
    return ok;
}
