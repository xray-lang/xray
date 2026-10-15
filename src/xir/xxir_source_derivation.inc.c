/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_derivation.inc.c - Definition-owned ordinary generator startup
 *
 * KEY CONCEPT:
 *   Authentic declaration policies precede evaluation. Logical facts cross
 *   pools; generated bodies enter the original complete Built checker.
 */
struct SourceDeriveJob {
    SourceDeriveJob *next;
    SourceName *owner;
    XrAttribute *attribute;
    uint32_t policy,generator;
    SourceLibraryUnit *library;
    XrXirInterfaceClosure *closure;
    XrXirImplementationBinding *requirements;
    uint32_t requirement_count;
    XrXirValue value;
    XrXirGeneratedData *data;
    uint32_t *callees,callee_count;
    bool owns_value;
};
static void source_derivations_free(SourceContext *ctx) {
    for (SourceDeriveJob *job=ctx->derive_jobs;job;job=job->next) {
        if (job->owns_value) xr_xir_value_drop(&job->value);
        xr_xir_compile_generated_data_free(job->data);
        xr_xir_compile_interface_closure_free(job->closure);
    }
}
static XrXirGeneratedOrigin source_derive_origin(SourceContext *ctx,SourceDeriveJob *job,int32_t field) {
    SourceName *owner=job->owner;AstNode *node=owner->node;
    const XrModuleSpec *spec=&ctx->graph->specs[owner->module];
    const char *path=owner->name;
    if (field>=0 && node->type!=AST_ENUM_DECL) {
        ClassDeclNode *d=node->type==AST_CLASS_DECL?&node->as.class_decl:&node->as.struct_decl;
        if ((uint32_t)field<(uint32_t)d->field_count) {
            node=d->fields[field];const char *name=node->as.field_decl.name;
            size_t owner_length=source_text_size(ctx,owner->name),length=source_text_size(ctx,name);
            if (owner_length>SIZE_MAX-2 || length>SIZE_MAX-owner_length-2) {
                source_fail(ctx,node,XR_XIR_BUDGET,"derived field path size exhausted");return (XrXirGeneratedOrigin){0};
            }
            char *joined=source_alloc(ctx,owner_length+length+2,1);
            if (!joined) return (XrXirGeneratedOrigin){0};
            if (source_format(ctx,joined,owner_length+length+2,"%s.%s",owner->name,name)!=XR_DIAG_OK) {
                source_fail(ctx,node,XR_XIR_BUDGET,"derived field path formatting exhausted");return (XrXirGeneratedOrigin){0};
            }
            path=joined;
        }
    }
    if (field<0) return (XrXirGeneratedOrigin){
        {spec->canonical,(uint32_t)source_text_size(ctx,spec->canonical)},
        {spec->source_path,(uint32_t)source_text_size(ctx,spec->source_path)},
        {path,(uint32_t)source_text_size(ctx,path)},(uint32_t)job->attribute->line,(uint32_t)job->attribute->column};
    return (XrXirGeneratedOrigin){
        {spec->canonical,(uint32_t)source_text_size(ctx,spec->canonical)},
        {spec->source_path,(uint32_t)source_text_size(ctx,spec->source_path)},
        {path,(uint32_t)source_text_size(ctx,path)},(uint32_t)node->line,(uint32_t)node->column};
}
static bool source_derive_members(SourceContext *ctx,SourceDeriveJob *job,XrXirDerivationPolicy *policy) {
    const XrDerivationSyntax *syntax=job->attribute->derivation;
    const XrXirNominalDeclaration *nominal=&ctx->nominals.declarations[job->owner->index];
    if (syntax->field_count<0 || (syntax->field_count && !syntax->fields))
        return source_fail(ctx,job->owner->node,XR_XIR_BAD_STRUCTURE,"derive field intent is malformed");
    uint32_t *operations=source_alloc(ctx,nominal->field_count,sizeof(*operations));
    if (!operations) return false;
    for (int i=0;i<syntax->field_count;++i) {
        const XrDeriveFieldSyntax *intent=&syntax->fields[i];uint32_t found=UINT32_MAX;
        if (!intent->name || intent->operation>XR_DERIVE_FIELD_WRITE)
            return source_fail(ctx,job->owner->node,XR_XIR_BAD_STRUCTURE,"derive field operation is malformed");
        for (uint32_t f=0;f<nominal->field_count;++f) {
            const XrXirLiteral name=nominal->fields[f].name;
            size_t length=source_text_size(ctx,intent->name);
            if (!source_work(ctx,job->owner->node)) return false;
            if (length==name.length && source_span_same(ctx,job->owner->node,name.bytes,intent->name,length)) found=f;
        }
        if (found==UINT32_MAX)
            return source_fail(ctx,job->owner->node,XR_XIR_BAD_TYPE,"derive intent names no field of its target");
        uint32_t bit=1u<<intent->operation;
        if (operations[found]&bit)
            return source_fail(ctx,job->owner->node,XR_XIR_BAD_TYPE,"duplicate derive field operation");
        operations[found]|=bit;
    }
    XrXirDerivationMember *members=source_alloc(ctx,nominal->field_count,sizeof(*members));
    if (!members) return false;
    for (uint32_t f=0;f<nominal->field_count;++f) {
        if (!source_work(ctx,job->owner->node)) return false;
        if (operations[f]) members[policy->member_count++]=(XrXirDerivationMember){f,operations[f]};
    }
    policy->members=policy->member_count?members:NULL;return true;
}
static bool source_derive_generator(SourceContext *ctx,SourceDeriveJob *job) {
    const XrDerivationSyntax *syntax=job->attribute->derivation;
    SourceName *binding=visible_name(ctx,syntax->generator_module?syntax->generator_module:syntax->generator);
    if (syntax->generator_module) {
        if (!binding || binding->kind!=SOURCE_MODULE)
            return source_fail(ctx,job->owner->node,XR_XIR_BAD_TYPE,"derive generator qualifier is not an imported module");
        binding=imported_declaration(ctx,binding,syntax->generator);
    } else if (binding && binding->kind==SOURCE_IMPORT)
        binding=imported_declaration(ctx,binding,binding->imported);
    if (!binding || binding->kind!=SOURCE_FUNCTION)
        return source_fail(ctx,job->owner->node,XR_XIR_BAD_TYPE,"derive requires an ordinary visible function");
    job->generator=UINT32_MAX;
    for (SourceLibraryUnit *unit=ctx->library_units;unit;unit=unit->next) {
        for (uint32_t f=0;f<unit->function_count;++f) {
            if (!source_work(ctx,job->owner->node)) return false;
            if (unit->functions[f]==binding->index) {job->library=unit;job->generator=f;}
        }
    }
    if (!job->library || job->generator==UINT32_MAX)
        return source_fail(ctx,job->owner->node,XR_XIR_BAD_TYPE,"derive generator requires a complete Checked dependency closure");
    ((XrXirDerivationPolicy *)ctx->derivations.policies)[job->policy].generator=binding->index;
    return true;
}
static bool source_derive_policy(SourceContext *ctx,SourceDeriveJob *job) {
    SourceName *owner=job->owner;ctx->module=owner->module;
    SourceNominalDeclaration syntax;
    if (!source_nominal_declaration(ctx,owner->node,&syntax)) return false;
    ctx->type_scope=(SourceTypeScope){true,owner->node,syntax.parameters,(uint32_t)syntax.parameter_count,
        syntax.parameter_count?owner->declaration:0,0};
    XrXirDerivationPolicy *policy=(XrXirDerivationPolicy *)&ctx->derivations.policies[job->policy];
    policy->grantor_module=owner->module;policy->target=owner->type;
    policy->origin=source_derive_origin(ctx,job,-1);
    if (!source_interface_application(ctx,owner->node,job->attribute->derivation->protocol,&policy->protocol) ||
        !source_derive_members(ctx,job,policy) || !source_derive_generator(ctx,job)) return false;
    if (job->attribute->derivation->construct) {
        uint32_t found=UINT32_MAX;
        for (uint32_t f=0;f<ctx->function_count;++f) {
            if (!source_work(ctx,owner->node)) return false;
            const XrXirFunctionIdentity *id=&ctx->identities[f];
            if (id->nominal_owner!=owner->index+1 || id->method_kind!=XR_XIR_CONSTRUCTOR) continue;
            if (found!=UINT32_MAX) return source_fail(ctx,owner->node,XR_XIR_BAD_TYPE,"derive construction requires one exact initializer");
            found=f;
        }
        if (found==UINT32_MAX) return source_fail(ctx,owner->node,XR_XIR_BAD_TYPE,"derive construction target has no initializer");
        policy->constructor=found+1;
    }
    for (uint32_t p=0;p<job->policy;++p) {
        const XrXirDerivationPolicy *other=&ctx->derivations.policies[p];bool same=false;
        if (other->target!=policy->target) continue;
        if (!source_constraint_same(ctx,owner->node,&other->protocol,&policy->protocol,&same)) return false;
        if (same) return source_fail(ctx,owner->node,XR_XIR_BAD_TYPE,"duplicate derive protocol application");
    }
    return true;
}
static bool source_derivations_prepare(SourceContext *ctx) {
    if (!ctx->derive_present) return true;
    SourceDeriveJob **tail=&ctx->derive_jobs;uint32_t count=0;
    for (uint32_t n=0;n<ctx->nominals.count;++n) {
        SourceName *owner=ctx->nominal_sources[n];if (owner->checked_library) continue;
        AstNode *node=owner->node;XrAttribute **attributes;int attribute_count;
        if (node->type==AST_ENUM_DECL) {attributes=node->as.enum_decl.attributes;attribute_count=node->as.enum_decl.attr_count;}
        else {ClassDeclNode *d=node->type==AST_CLASS_DECL?&node->as.class_decl:&node->as.struct_decl;
            attributes=d->attributes;attribute_count=d->attr_count;}
        for (int a=0;a<attribute_count;++a) {
            if (!source_work(ctx,node)) return false;
            if (count==UINT32_MAX) return source_fail(ctx,node,XR_XIR_BUDGET,"derive policy inventory exhausted");
            SourceDeriveJob *job=source_alloc(ctx,1,sizeof(*job));if (!job) return false;
            job->owner=owner;job->attribute=attributes[a];job->policy=count++;*tail=job;tail=&job->next;
        }
    }
    if (!count) return true;
    ctx->derivations.policies=source_alloc(ctx,count,sizeof(*ctx->derivations.policies));
    if (!ctx->derivations.policies) return false;
    ctx->derivations.policy_count=count;
    SourceTypeScope saved=ctx->type_scope;bool ok=true;
    for (SourceDeriveJob *job=ctx->derive_jobs;job && ok;job=job->next) ok=source_derive_policy(ctx,job);
    ctx->type_scope=saved;
    if (!ok) return false;
    for (SourceDeriveJob *job=ctx->derive_jobs;job;job=job->next) {
        XrXirDeclarations declarations;XrXirModule built=source_module_view(ctx,&declarations);
        XrXirDerivationTable single={&ctx->derivations.policies[job->policy],1,NULL,0};
        declarations.derivations=&single;
        XrXirStatus status=xir_derivations_verify(&ctx->compile,&built);
        if (status==XR_XIR_OK) continue;
        ctx->module=job->owner->module;
        source_fail(ctx,job->owner->node,status,"derive definition policy failed independent validation");
        ctx->diagnostic.line=job->attribute->line;ctx->diagnostic.column=job->attribute->column;
        return false;
    }
    return true;
}
#include "xxir_source_derive_context.inc.c"
#include "xxir_source_derive_execute.inc.c"
#include "xxir_source_derive_install.inc.c"
static bool source_derive_check_failure(SourceContext *ctx,const XrXirDiagnostic *location,XrXirStatus status) {
    if (location->function>=ctx->function_count) return false;
    uint32_t index=ctx->identities[location->function].derivation;
    if (!index || index>ctx->derivations.count) return false;
    const XrXirDerivationGrant *binding=&ctx->derivations.records[index-1];
    const XrXirGeneratedOrigin *origin=&binding->origins.entry;
    if (location->instruction<binding->origins.instruction_count) origin=&binding->origins.instructions[location->instruction];
    char message[224]={0};
    if (status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY &&
        source_format(ctx,message,sizeof(message),"derived %s failed checking for %.*s at instruction %u",
            ctx->functions[location->function].name,(int)origin->field_path.length,origin->field_path.bytes,
            location->instruction)!=XR_DIAG_OK) status=XR_XIR_BUDGET;
    ctx->module=ctx->bodies[location->function].module;
    source_fail(ctx,ctx->bodies[location->function].node,status,message);
    ctx->diagnostic.line=(int)origin->line;ctx->diagnostic.column=(int)origin->column;
    return true;
}
