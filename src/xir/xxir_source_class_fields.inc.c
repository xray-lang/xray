/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_class_fields.inc.c - Definition-owned public parent field prefixes
 *
 * KEY CONCEPT:
 *   Parent fields keep their original query identity and complete type projection.
 */
typedef struct SourceFieldBuild {
    SourceName *symbol;
    ClassDeclNode *syntax;
    uint32_t prefix;
    XrXirNominalField *fields;
    XrXirType *types;
    uint32_t *members;
} SourceFieldBuild;
typedef struct SourceFieldFrame { SourceName *symbol,*parent; } SourceFieldFrame;
static SourceName *source_class_parent(SourceContext *ctx,SourceName *symbol,ClassDeclNode *syntax) {
    if(!syntax->super_name)return NULL;
    SourceName *parent=NULL;
    if(syntax->super_module) {
        SourceName *module=visible_name(ctx,syntax->super_module);
        if(module && module->kind==SOURCE_MODULE)parent=imported_declaration(ctx,module,syntax->super_name);
    } else parent=source_nominal_name(ctx,syntax->super_name);
    if(!parent || parent->kind!=SOURCE_NOMINAL || parent->index>=ctx->nominals.count) {
        source_fail(ctx,symbol->node,XR_XIR_BAD_TYPE,"parent requires an actual visible class declaration");return NULL;
    }
    const XrXirNominalDeclaration *base=&ctx->nominals.declarations[parent->index];
    if(base->kind!=XR_XIR_NOMINAL_CLASS || base->parameter_count || syntax->type_param_count ||
        base->native.native_id || (base->flags&(XR_XIR_NOMINAL_FINAL|XR_XIR_NOMINAL_SENDABLE))) {
        source_fail(ctx,symbol->node,XR_XIR_BAD_TYPE,"parent requires a non-final ordinary non-generic public class");return NULL;
    }
    return parent;
}
static bool source_field_build_rows(SourceContext *ctx,SourceFieldBuild *build) {
    SourceName *symbol=build->symbol;ClassDeclNode *syntax=build->syntax;
    for(uint32_t own=0;own<(uint32_t)syntax->field_count;++own) {
        AstNode *node=syntax->fields[own];uint32_t f=build->prefix+own;
        if(node->type!=AST_FIELD_DECL)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"field declaration is missing");
        FieldDeclNode *field=&node->as.field_decl;
        if(!field->field_type || field->is_static || field->is_final || field->is_flexible || field->is_weak)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"stored field contract is not admitted");
        if(!source_type(ctx,field->field_type,&build->types[f]) || build->types[f]==XR_XIR_UNIT)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"field requires an admitted value type");
        const XrXirNominalDeclaration *record=&ctx->nominals.declarations[symbol->index];
        build->fields[f]=(XrXirNominalField){{field->name,(uint32_t)source_text_size(ctx,field->name)},build->types[f],
            (field->is_private?XR_XIR_FIELD_PRIVATE:0)|(field->is_protected?XR_XIR_FIELD_PROTECTED:0)|
            (field->is_const || (record->kind==XR_XIR_NOMINAL_CLASS &&
             (record->flags&XR_XIR_NOMINAL_SENDABLE))?0:XR_XIR_FIELD_MUTABLE)};
        for(uint32_t prior=0;prior<build->prefix;++prior)
            if(build->fields[f].name.length==build->fields[prior].name.length &&
                source_span_same(ctx,node,field->name,build->fields[prior].name.bytes,
                    build->fields[prior].name.length))
                return source_fail(ctx,node,XR_XIR_BAD_TYPE,"a derived field cannot hide an inherited member");
        SourceName member={0};member.name=source_owned_text(ctx,field->name);member.node=node;
        if(!member.name)return false;
        member.type=build->types[f];member.mutable=(build->fields[f].flags&XR_XIR_FIELD_MUTABLE)!=0;
        if(!source_query_declare(ctx,&member,XR_XIR_SOURCE_MEMBER,symbol->declaration,
            source_query_range(ctx,node,field->name)))return false;
        source_query_binding_type(ctx,&member);build->members[f]=member.declaration;
    }
    return true;
}
static bool source_nominal_field_rows(SourceContext *ctx,SourceFieldFrame frame,uint8_t *state) {
    SourceName *symbol=frame.symbol,*parent=frame.parent;
    ctx->module=symbol->module;ctx->function=symbol->module;
    ClassDeclNode *syntax=symbol->node->type==AST_CLASS_DECL?&symbol->node->as.class_decl:&symbol->node->as.struct_decl;
    uint32_t prefix=0;
    if(parent) {
        XrXirNominalDeclaration base=ctx->nominals.declarations[parent->index];prefix=base.field_count;
        for(uint32_t f=0;f<prefix;++f)
            if(base.fields[f].flags&(XR_XIR_FIELD_PRIVATE|XR_XIR_FIELD_PROTECTED))
                return source_fail(ctx,symbol->node,XR_XIR_BAD_TYPE,"non-public inherited fields are not admitted");
        ((XrXirNominalDeclaration *)ctx->nominals.declarations)[symbol->index].parent=parent->index+1;
    }
    ctx->module=symbol->module;ctx->function=symbol->module;
    if((uint32_t)syntax->field_count>UINT32_MAX-prefix)return source_fail(ctx,symbol->node,XR_XIR_BUDGET,"inherited field count exhausted");
    uint32_t total=prefix+(uint32_t)syntax->field_count;
    SourceFieldBuild build={symbol,syntax,prefix,total?source_alloc(ctx,total,sizeof(*build.fields)):NULL,
        total?source_alloc(ctx,total,sizeof(*build.types)):NULL,total?source_alloc(ctx,total,sizeof(*build.members)):NULL};
    if(total && (!build.fields || !build.types || !build.members))return false;
    if(parent)for(uint32_t f=0;f<prefix;++f) {
        build.fields[f]=ctx->nominals.declarations[parent->index].fields[f];build.types[f]=build.fields[f].type;
        build.members[f]=ctx->nominal_members[parent->index][f];
    }
    XrXirNominalDeclaration *record=(XrXirNominalDeclaration *)&ctx->nominals.declarations[symbol->index];
    record->fields=build.fields;record->field_count=total;ctx->nominal_members[symbol->index]=build.members;
    SourceTypeScope saved=ctx->type_scope;
    ctx->type_scope=(SourceTypeScope){true,symbol->node,syntax->type_params,(uint32_t)syntax->type_param_count,
        syntax->type_param_count?symbol->declaration:0,0};
    bool ok=source_field_build_rows(ctx,&build);ctx->type_scope=saved;
    if(!ok)return false;
    XrXirTypeNode *type=(XrXirTypeNode *)&ctx->types.nodes[(uint32_t)symbol->type-XR_XIR_CONSTRUCTED_TYPE_BASE];
    type->nominal.fields=build.types;type->nominal.field_count=total;state[symbol->index]=2;return true;
}
static bool source_nominal_fields(SourceContext *ctx,SourceName *symbol,uint8_t *state,
    SourceFieldFrame *stack,uint32_t count) {
    uint32_t depth=0;
    while(symbol && !symbol->checked_library) {
        if(symbol->index>=count)return source_fail(ctx,symbol->node,XR_XIR_BAD_STRUCTURE,"field declaration owner changed");
        if(state[symbol->index]==2)break;
        if(state[symbol->index] || depth==count)
            return source_fail(ctx,symbol->node,XR_XIR_BAD_TYPE,"class ancestry contains a cycle");
        if(!source_request_poll(ctx,symbol->node))return false;
        state[symbol->index]=1;ctx->module=symbol->module;ctx->function=symbol->module;
        SourceName *parent=NULL;
        if(symbol->node->type==AST_CLASS_DECL && symbol->node->as.class_decl.super_name) {
            parent=source_class_parent(ctx,symbol,&symbol->node->as.class_decl);
            if(!parent)return false;
        }
        stack[depth++]=(SourceFieldFrame){symbol,parent};symbol=parent;
    }
    while(depth) {
        if(!source_request_poll(ctx,stack[depth-1].symbol->node) ||
            !source_nominal_field_rows(ctx,stack[--depth],state))return false;
    }
    return true;
}
