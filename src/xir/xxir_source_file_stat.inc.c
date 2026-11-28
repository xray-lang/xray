/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_file_stat.inc.c - Owned private metadata without a language constructor
 *
 * KEY CONCEPT:
 *   Native results have actual query identities and no source declaration AST.
 */
static bool source_file_stat_declare(SourceContext *ctx,XrXirType *output) {
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_FILE_STAT);
    if(!xr_native_declaration_validate(native) || native->member_count!=XR_NATIVE_FILE_STAT_FIELD_COUNT)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"file metadata declaration is missing");
    if(find_name(ctx,ctx->names[ctx->module],native->name))
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"file metadata identity is reserved to its native declaration");
    if(!source_nominal_reserve(ctx))return false;
    uint32_t d=ctx->nominals.count;
    const char *name=source_owned_text(ctx,native->name);if(!name)return false;
    SourceName *owner=add_module_name(ctx,ctx->module,name,NULL);if(!owner)return false;
    owner->kind=SOURCE_NOMINAL;owner->index=d;owner->module=ctx->module;
    XrXirNominalField *fields=source_alloc(ctx,XR_NATIVE_FILE_STAT_FIELD_COUNT,sizeof(*fields));
    XrXirType *types=source_alloc(ctx,XR_NATIVE_FILE_STAT_FIELD_COUNT,sizeof(*types));
    uint32_t *members=source_alloc(ctx,XR_NATIVE_FILE_STAT_FIELD_COUNT,sizeof(*members));if(!fields || !types || !members)return false;
    XrXirType optional=XR_XIR_UNIT;
    if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64},&optional))return false;
    for(uint32_t f=0;f<XR_NATIVE_FILE_STAT_FIELD_COUNT;++f) {
        if(!source_request_poll(ctx,NULL))return false;
        const char *member=source_owned_text(ctx,native->members[f].name);if(!member)return false;
        XrNativeTypeTerm term=native->members[f].result;
        if(term!=XR_NATIVE_TERM_I64 && term!=XR_NATIVE_TERM_NULLABLE_I64 && term!=XR_NATIVE_TERM_BOOL)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"file metadata field has no source type");
        types[f]=term==XR_NATIVE_TERM_I64?XR_XIR_I64:term==XR_NATIVE_TERM_NULLABLE_I64?optional:XR_XIR_BOOL;
        fields[f]=(XrXirNominalField){{member,(uint32_t)strlen(member)},types[f],0};
    }
    XrXirSourceModule module=ctx->modules[ctx->module];
    XrXirNominalDeclaration record={.module={module.name,module.name_length},.name={name,(uint32_t)strlen(name)},
        .kind=XR_XIR_NOMINAL_CLASS,.flags=XR_XIR_NOMINAL_FINAL,.fields=fields,.field_count=XR_NATIVE_FILE_STAT_FIELD_COUNT};
    record.native.native_id=native->id;
    if(!source_copy_bytes(ctx,NULL,record.native.source_fingerprint,native->source_fingerprint.bytes,32))return false;
    ((XrXirNominalDeclaration *)ctx->nominals.declarations)[d]=record;
    ctx->nominal_sources[d]=owner;ctx->nominal_members[d]=members;++ctx->nominals.count;
    XrXirTypeNode node={.kind=XR_XIR_TYPE_NOMINAL};
    node.nominal=(XrXirNominalType){d,NULL,0,types,XR_NATIVE_FILE_STAT_FIELD_COUNT};
    if(!source_intern_type(ctx,node,&owner->type) || !xir_file_stat_type(&ctx->types,owner->type) ||
        !source_query_declare(ctx,owner,XR_XIR_SOURCE_TYPE,0,(XrXirSourceRange){ctx->module,0,0,0,0}))return false;
    XrXirSourceDeclaration *query=(XrXirSourceDeclaration *)ctx->query.declarations+owner->declaration-1;
    query->native_identity=native->id;query->type=(XrXirSourceType){owner->type,0,true};
    for(uint32_t f=0;f<XR_NATIVE_FILE_STAT_FIELD_COUNT;++f) {
        SourceName member={.name=fields[f].name.bytes,.type=types[f],.module=ctx->module};
        if(!source_query_declare(ctx,&member,XR_XIR_SOURCE_MEMBER,owner->declaration,
            (XrXirSourceRange){ctx->module,0,0,0,0}))return false;
        members[f]=member.declaration;source_query_binding_type(ctx,&member);
    }
    return source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=owner->type},output);
}
