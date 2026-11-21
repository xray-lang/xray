#include "xxir_native_opaque.h"
/* Builtin contract discovery supplies candidate IDs. Common signature and
 * obligation verification supplies their meaning, including conditional bounds. */
static bool source_hashable_marker(SourceContext *ctx,const XrTypeRef *ref) {
    return ref && ref->kind==XR_TREF_NAMED && !ref->nchildren && ref->name &&
        source_predicate_marker(ctx,ref->name)==XR_XIR_CONSTRAINT_HASHABLE;
}
static bool source_hashable_owner_marker(SourceContext *ctx,SourceName *owner,const XrTypeRef *ref) {
    SourceTypeScope scope=ctx->type_scope;uint32_t module=ctx->module;
    SourceNominalDeclaration declaration;
    if(!source_nominal_declaration(ctx,owner->node,&declaration))return false;
    ctx->module=owner->module;
    ctx->type_scope=(SourceTypeScope){true,owner->node,declaration.parameters,
        (uint32_t)declaration.parameter_count,declaration.parameter_count?owner->declaration:0,0};
    bool marker=source_hashable_marker(ctx,ref);
    ctx->type_scope=scope;ctx->module=module;return marker;
}
static bool source_hashable_prepare(SourceContext *ctx) {
    uint32_t capacity=ctx->nominals.count;
    for(SourceLibraryUnit *u=ctx->library_units;u;u=u->next) {
        if(u->hashable_witness_count>UINT32_MAX-capacity)
            return source_fail(ctx,NULL,XR_XIR_BUDGET,"Hashable candidate count exhausted");
        capacity+=u->hashable_witness_count;
    }
    XrXirHashableWitness *rows=capacity?source_alloc(ctx,capacity,sizeof(*rows)):NULL;
    bool *required=ctx->nominals.count?source_alloc(ctx,ctx->nominals.count,sizeof(*required)):NULL;
    if((capacity && !rows) || (ctx->nominals.count && !required))return false;
    uint32_t count=0;
    for(SourceLibraryUnit *u=ctx->library_units;u;u=u->next)
        for(uint32_t i=0;i<u->hashable_witness_count;++i)rows[count++]=u->hashable_witnesses[i];
    uint32_t saved_module=ctx->module;SourceTypeScope saved_scope=ctx->type_scope;
    for(uint32_t d=0;d<ctx->nominals.count;++d) {
        SourceName *owner=ctx->nominal_sources[d];
        if(owner->checked_library)continue;
        if(xir_native_opaque_nominal(ctx->nominals.declarations[d].native.native_id)) {
            /* These governed carriers have no source AST or guest methods. A
             * matching numeric tag alone does not authorize skipping either. */
            if(!xir_native_opaque_storage(ctx->nominals.declarations[d].native.native_id) &&
                !xir_file_stat_type(&ctx->types,owner->type))
                return source_fail(ctx,owner->node,XR_XIR_BAD_STRUCTURE,"Hashable native owner identity is malformed");
            continue;
        }
        SourceNominalDeclaration declaration;
        if(!source_nominal_declaration(ctx,owner->node,&declaration))return false;
        ctx->module=owner->module;
        ctx->type_scope=(SourceTypeScope){true,owner->node,declaration.parameters,
            (uint32_t)declaration.parameter_count,declaration.parameter_count?owner->declaration:0,0};
        for(int i=0;i<declaration.interface_count;++i)
            if(source_hashable_marker(ctx,declaration.interfaces[i]))required[d]=true;
        SourceName *equals=find_name(ctx,ctx->nominal_methods[d],"equals");
        SourceName *hash=find_name(ctx,ctx->nominal_methods[d],"hash");
        if(!equals || !hash)continue;
        XrXirHashableWitness candidate={owner->type,equals->index,hash->index};
        XrXirDeclarations declarations;
        XrXirModule module=source_module_view(ctx,&declarations);
        declarations.hashable_witnesses=&candidate;declarations.hashable_witness_count=1;
        XirMetadataDomain domain=xir_metadata_domain_from_module(&module);
        XirMetadataProofContext proof={&domain,{XR_XIR_CONTEXT_NOMINAL,d,0}};
        XrXirStatus status=xir_hashable_witness_read(&ctx->compile,&proof,owner->type,NULL,ctx->request);
        if(status==XR_XIR_OK)status=xr_xir_compile_method_signature_verify(&ctx->compile,&module,equals->index);
        if(status==XR_XIR_OK)status=xr_xir_compile_method_signature_verify(&ctx->compile,&module,hash->index);
        if(status==XR_XIR_OK)rows[count++]=candidate;
        else if(status!=XR_XIR_BAD_TYPE)return source_fail(ctx,owner->node,status,"Hashable candidate signature could not be read");
    }
    ctx->module=saved_module;ctx->type_scope=saved_scope;
    XrXirStatus status=xir_hashable_witnesses_sort(&ctx->compile,&ctx->types,rows,count,ctx->request);
    if(status!=XR_XIR_OK)return source_fail(ctx,NULL,status,"Hashable candidate order is invalid");
    ctx->hashable_witnesses=count?rows:NULL;ctx->hashable_witness_count=count;
    /* Real method requirements are goals, never facts. Remove optional
     * candidates with stronger bounds and recheck their dependants. */
    uint64_t attempts=ctx->compile.limits.instructions;
    for(uint32_t i=0;i<count;) {
        if(!attempts--)return source_fail(ctx,NULL,XR_XIR_BUDGET,"Hashable candidate proof budget exhausted");
        const XrXirTypeNode *n=xr_xir_type_node(&ctx->types,rows[i].nominal_type);
        XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);
        XrXirProofContext proof={&module,{XR_XIR_CONTEXT_NOMINAL,n->nominal.declaration,0}};
        status=xir_compile_type_markers_prove_controlled(&ctx->compile,&proof,rows[i].nominal_type,XR_XIR_CONSTRAINT_HASHABLE,ctx->request);
        if(status==XR_XIR_OK){++i;continue;}
        if(status!=XR_XIR_BAD_TYPE || required[n->nominal.declaration])
            return source_fail(ctx,ctx->nominal_sources[n->nominal.declaration]->node,status,"Hashable method obligations are not entailed by the nominal declaration");
        for(uint32_t j=i+1;j<count;++j) {
            if(!attempts--)return source_fail(ctx,NULL,XR_XIR_BUDGET,"Hashable candidate removal budget exhausted");
            status=xir_receive_poll(ctx->request);if(status!=XR_XIR_OK)return source_fail(ctx,NULL,status,"Hashable candidate removal interrupted");
            rows[j-1]=rows[j];
        }
        --count;ctx->hashable_witness_count=count;ctx->hashable_witnesses=count?rows:NULL;i=0;
    }
    for(uint32_t d=0;d<ctx->nominals.count;++d)if(required[d]) {
        XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);
        XrXirProofContext proof={&module,{XR_XIR_CONTEXT_NOMINAL,d,0}};
        status=xir_compile_type_markers_prove_controlled(&ctx->compile,&proof,ctx->nominal_sources[d]->type,XR_XIR_CONSTRAINT_HASHABLE,ctx->request);
        if(status!=XR_XIR_OK)return source_fail(ctx,ctx->nominal_sources[d]->node,status,
            "Hashable requires accessible instance equals(other: Self) -> bool and hash() -> i64 methods");
    }
    return true;
}
