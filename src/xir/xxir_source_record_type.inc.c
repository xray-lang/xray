/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_record_type.inc.c - Owned exact structural key schemas and type applications
 */
static bool source_record_kind(const SourceContext *ctx,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(&ctx->types,type);
    return node&&node->kind==XR_XIR_TYPE_NOMINAL&&node->nominal.declaration<ctx->nominals.count&&
        (ctx->nominals.declarations[node->nominal.declaration].flags&XR_XIR_NOMINAL_RECORD)!=0;
}
static bool source_record_reserve(SourceContext *ctx) {
    if(ctx->nominals.count<ctx->nominal_capacity)return true;
    uint32_t capacity=ctx->nominal_capacity?ctx->nominal_capacity*2:8;
    if(capacity<ctx->nominal_capacity)return source_fail(ctx,NULL,XR_XIR_OVERFLOW,"record declaration capacity overflow");
    XrXirNominalDeclaration *declarations=source_alloc(ctx,capacity,sizeof(*declarations));
    SourceName **sources=source_alloc(ctx,capacity,sizeof(*sources)),**methods=source_alloc(ctx,capacity,sizeof(*methods));
    uint32_t **members=source_alloc(ctx,capacity,sizeof(*members)),**variants=source_alloc(ctx,capacity,sizeof(*variants));
    if(!declarations||!sources||!methods||!members||!variants)return false;
    size_t n=ctx->nominals.count;
    if(!source_copy_bytes(ctx,NULL,declarations,ctx->nominals.declarations,n*sizeof(*declarations))||
        !source_copy_bytes(ctx,NULL,sources,ctx->nominal_sources,n*sizeof(*sources))||
        !source_copy_bytes(ctx,NULL,methods,ctx->nominal_methods,n*sizeof(*methods))||
        !source_copy_bytes(ctx,NULL,members,ctx->nominal_members,n*sizeof(*members))||
        !source_copy_bytes(ctx,NULL,variants,ctx->nominal_variants,n*sizeof(*variants)))return false;
    source_retire_array(ctx,ctx->nominals.declarations);source_retire_array(ctx,ctx->nominal_sources);
    source_retire_array(ctx,ctx->nominal_methods);source_retire_array(ctx,ctx->nominal_members);source_retire_array(ctx,ctx->nominal_variants);
    ctx->nominals.declarations=declarations;ctx->nominal_sources=sources;ctx->nominal_methods=methods;
    ctx->nominal_members=members;ctx->nominal_variants=variants;ctx->nominal_capacity=capacity;ctx->types.nominals=&ctx->nominals;
    return true;
}
static bool source_record_schema(SourceContext *ctx,AstNode *site,const XrXirLiteral *names,uint32_t count,uint32_t *out) {
    if(count>65536||(count&&!names))return source_fail(ctx,site,XR_XIR_BAD_TYPE,"record requires bounded static fields");
    for(uint32_t d=0;d<ctx->nominals.count;++d) {
        if(!source_request_poll(ctx,site))return false;
        XrXirNominalDeclaration row=ctx->nominals.declarations[d];
        if(row.flags!=XR_XIR_NOMINAL_RECORD||row.kind!=XR_XIR_NOMINAL_STRUCT||row.field_count!=count)continue;
        bool same=true;
        for(uint32_t f=0;f<count&&same;++f) {
            if(!source_request_poll(ctx,site))return false;
            same=row.fields[f].name.length==names[f].length&&
                source_span_same(ctx,site,row.fields[f].name.bytes,names[f].bytes,names[f].length);
        }
        if(ctx->diagnostic.status!=XR_XIR_OK)return false;
        if(same){*out=d;return true;}
    }
    if(!source_record_reserve(ctx))return false;
    uint32_t d=ctx->nominals.count;
    SourceName *owner=source_alloc(ctx,1,sizeof(*owner));
    XrXirNominalField *fields=count?source_alloc(ctx,count,sizeof(*fields)):NULL;
    XrXirConstraint *constraints=count?source_alloc(ctx,count,sizeof(*constraints)):NULL;
    uint32_t *members=count?source_alloc(ctx,count,sizeof(*members)):NULL;
    if(!owner||(count&&(!fields||!constraints||!members)))return false;
    char label[48];snprintf(label,sizeof(label),"$record_%u",d);
    owner->name=source_owned_text(ctx,label);owner->kind=SOURCE_NOMINAL;owner->index=d;owner->module=ctx->module;
    owner->node=site;owner->hidden=true;
    if(!owner->name)return false;
    for(uint32_t f=0;f<count;++f) {
        if(!source_request_poll(ctx,site))return false;
        char *name=source_alloc(ctx,(size_t)names[f].length+1,1);
        if(!name||!source_copy_bytes(ctx,site,name,names[f].bytes,names[f].length))return false;
        fields[f]=(XrXirNominalField){{name,names[f].length},(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+f),XR_XIR_FIELD_MUTABLE};
    }
    XrXirSourceModule module=ctx->modules[ctx->module];
    ((XrXirNominalDeclaration *)ctx->nominals.declarations)[d]=(XrXirNominalDeclaration){
        .module={module.name,module.name_length},.name={owner->name,(uint32_t)strlen(owner->name)},.exported=1,
        .constraints=constraints,.parameter_count=count,.fields=fields,.field_count=count,
        .kind=XR_XIR_NOMINAL_STRUCT,.flags=XR_XIR_NOMINAL_RECORD};
    ctx->nominal_sources[d]=owner;ctx->nominal_members[d]=members;++ctx->nominals.count;
    if(!source_query_declare(ctx,owner,XR_XIR_SOURCE_TYPE,0,source_query_range(ctx,site,NULL)))return false;
    XrXirType *parameters=count?source_alloc(ctx,count,sizeof(*parameters)):NULL;
    if(count&&!parameters)return false;
    for(uint32_t f=0;f<count;++f)parameters[f]=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+f);
    XrXirTypeNode schema={.kind=XR_XIR_TYPE_NOMINAL};schema.nominal=(XrXirNominalType){d,parameters,count,NULL,0};
    if(!source_intern_type(ctx,schema,&owner->type))return false;
    XrXirSourceDeclaration *query=(XrXirSourceDeclaration *)&ctx->query.declarations[owner->declaration-1];
    query->generic_parameter_count=count;query->generic_constraints=constraints;
    query->type=(XrXirSourceType){owner->type,count?owner->declaration:0,true};
    for(uint32_t f=0;f<count;++f) {
        SourceName member={0};member.name=fields[f].name.bytes;member.node=site;member.type=fields[f].type;member.mutable=true;
        if(!source_query_declare(ctx,&member,XR_XIR_SOURCE_MEMBER,owner->declaration,source_query_range(ctx,site,NULL)))return false;
        members[f]=member.declaration;
        ((XrXirSourceDeclaration *)ctx->query.declarations)[member.declaration-1].type=(XrXirSourceType){member.type,owner->declaration,true};
    }
    if(ctx->record_declarations_ready) {
        XrHirNominalDeclarationInput input={.types=&ctx->types,.declaration=d,.module=ctx->module,.source_declaration=owner->declaration};
        const char *reason=NULL;XrXirStatus status=xr_hir_body_nominal_declare(ctx->typed_body,&input,ctx->request,&reason);
        if(status!=XR_XIR_OK)return source_fail(ctx,site,status,reason?reason:"record shape owner receiving failed");
    }
    *out=d;return true;
}
/* Names are canonicalized independently of expression evaluation. */
static bool source_record_order(SourceContext *ctx,AstNode *site,const XrXirLiteral *names,uint32_t count,uint32_t *order) {
    for(uint32_t f=0;f<count;++f) {
        if(!source_request_poll(ctx,site))return false;
        if(!names[f].bytes||!names[f].length)return source_fail(ctx,site,XR_XIR_BAD_TYPE,"record requires nonempty static field names");
        uint32_t at=f;
        while(at) {
            if(!source_request_poll(ctx,site))return false;
            XrXirLiteral a=names[order[at-1]],b=names[f];uint32_t n=a.length<b.length?a.length:b.length;
            int cmp=0;
            for(uint32_t offset=0;offset<n&&!cmp;) {
                if(!source_request_poll(ctx,site))return false;uint32_t part=n-offset<4096?n-offset:4096;
                cmp=memcmp(a.bytes+offset,b.bytes+offset,part);offset+=part;
            }
            if(!cmp)cmp=a.length<b.length?-1:a.length>b.length?1:0;
            if(!cmp)return source_fail(ctx,site,XR_XIR_BAD_TYPE,"duplicate record field");
            if(cmp<0)break;order[at]=order[at-1];--at;
        }
        order[at]=f;
    }
    return true;
}
static bool source_record_type(SourceContext *ctx,AstNode *site,const XrXirLiteral *names,
    const XrXirType *types,uint32_t count,XrXirType *out) {
    uint32_t *order=count?source_alloc(ctx,count,sizeof(*order)):NULL;
    XrXirLiteral *keys=count?source_alloc(ctx,count,sizeof(*keys)):NULL;
    XrXirType *arguments=count?source_alloc(ctx,count,sizeof(*arguments)):NULL;
    if(count&&(!order||!keys||!arguments))return false;
    if(!source_record_order(ctx,site,names,count,order))return false;
    for(uint32_t f=0;f<count;++f) {
        keys[f]=names[order[f]];arguments[f]=types[order[f]];
        uint32_t views=0;XrXirStatus status=xr_xir_compile_view_type_flags(&ctx->compile,&ctx->types,arguments[f],&views);
        if(status!=XR_XIR_OK)return source_fail(ctx,site,status,"record field storage inspection failed");
        if(arguments[f]==XR_XIR_UNIT||xr_xir_type_is_cell(&ctx->types,arguments[f])||views)
            return source_fail(ctx,site,XR_XIR_BAD_TYPE,"record field requires a storable value type");
    }
    uint32_t declaration=0;if(!source_record_schema(ctx,site,keys,count,&declaration))return false;
    XrXirTypeNode node={.kind=XR_XIR_TYPE_NOMINAL};
    node.nominal=(XrXirNominalType){declaration,arguments,count,NULL,0};
    return source_intern_type(ctx,node,out);
}
static bool source_record_type_ref(SourceContext *ctx,XrTypeRef *ref,XrXirType *out) {
    uint32_t count=(uint32_t)ref->nchildren;
    if(count>65536||(count&&(!ref->field_names||!ref->children))||ctx->depth>=128)
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"record annotation requires bounded named fields");
    XrXirLiteral *names=count?source_alloc(ctx,count,sizeof(*names)):NULL;
    XrXirType *types=count?source_alloc(ctx,count,sizeof(*types)):NULL;if(count&&(!names||!types))return false;
    ++ctx->depth;
    for(uint32_t f=0;f<count;++f) {
        if(!source_request_poll(ctx,NULL)){--ctx->depth;return false;}
        if(!ref->field_names[f]||(ref->field_readonly&&ref->field_readonly[f])) {--ctx->depth;return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"record requires mutable named field types");}
        if(!source_type(ctx,ref->children[f],&types[f])){--ctx->depth;return false;}
        names[f]=(XrXirLiteral){ref->field_names[f],(uint32_t)source_text_size(ctx,ref->field_names[f])};
    }
    --ctx->depth;AstNode *site=ctx->type_scope.active?ctx->type_scope.node:ctx->bodies[ctx->function].node;
    return source_record_type(ctx,site,names,types,count,out);
}
