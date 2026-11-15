/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Collection projections authenticate real instructions after producer death. */
typedef struct CollectionProjectionFixture {
    XrXirTypeNode node;
    XrXirTypes types;
    XrHirCollectionEntry entries[2];
    XrHirCollectionValue given[3];
    XrXirType parameters[3];
    uint32_t arguments[3],sources[3],relocation[4],functions,modules;
    XrXirInstruction instruction;
    XrXirFunction function,*function_view;
    XrXirFunctionIdentity identity,*identity_view;
    XrXirMapLiteralOrigin origins[2],received_origins[2];
    char path[12289],received_path[12289];
    XrXirMapLiteral literal,received;
    XrXirSourceModule module;
    XrXirDeclarations declarations;
    XrHirCollectionConstructInput input;
    XrHirBodyProjection projection;
} CollectionProjectionFixture;
static void collection_projection_fixture(CollectionProjectionFixture *f,unsigned mode,const XrXirCompileContext *compile) {
    *f=(CollectionProjectionFixture){0};bool map=mode!=0,empty=mode==3,spread=mode==2;
    uint32_t count=empty?0:spread?3u:2u,entries=empty?0:map&&!spread?1u:2u;
    f->node=(XrXirTypeNode){.kind=map?XR_XIR_TYPE_MAP:XR_XIR_TYPE_ARRAY,
        .element=map?XR_XIR_STRING:XR_XIR_I64,.result=map?XR_XIR_I64:XR_XIR_UNIT};
    f->types=(XrXirTypes){.nodes=&f->node,.count=1};
    if(map) {
        f->entries[0]=(XrHirCollectionEntry){.key=spread?XR_XIR_UNIT:XR_XIR_STRING,
            .value=spread?(XrXirType)256:XR_XIR_I64,.spread=spread};
        f->entries[1]=(XrHirCollectionEntry){.key=XR_XIR_STRING,.value=XR_XIR_I64};
    } else f->entries[0]=f->entries[1]=(XrHirCollectionEntry){.value=XR_XIR_I64};
    f->parameters[0]=spread?(XrXirType)256:map?XR_XIR_STRING:XR_XIR_I64;
    f->parameters[1]=spread?XR_XIR_STRING:XR_XIR_I64;f->parameters[2]=XR_XIR_I64;
    for(uint32_t n=0;n<3;++n) {
        f->given[n]=(XrHirCollectionValue){f->parameters[n],n};
        f->arguments[n]=f->sources[n]=f->relocation[n]=n;
    }
    f->relocation[3]=3;f->functions=f->modules=1;
    f->instruction=(XrXirInstruction){.op=map?XR_XIR_MAP_LITERAL:XR_XIR_ARRAY_NEW,.type=(XrXirType)256,.args={0,count}};
    f->function=(XrXirFunction){.parameters=count?f->parameters:NULL,.parameter_count=count,
        .instructions=&f->instruction,.instruction_count=1,.operands=count?f->arguments:NULL,.operand_count=count};
    f->function_view=&f->function;f->identity_view=&f->identity;
    f->origins[0]=(XrXirMapLiteralOrigin){spread?1u:0u,3,12};f->origins[1]=(XrXirMapLiteralOrigin){0,4,9};
    memcpy(f->received_origins,f->origins,sizeof(f->origins));
    memset(f->path,'x',sizeof(f->path));memcpy(f->received_path,f->path,sizeof(f->path));
    uint32_t path_length=empty?0:spread?(uint32_t)sizeof(f->path):10u;
    f->literal=(XrXirMapLiteral){.file={path_length?f->path:NULL,path_length},.items=entries?f->origins:NULL,.count=entries};
    f->received=f->literal;f->received.file.bytes=path_length?f->received_path:NULL;
    f->received.items=entries?f->received_origins:NULL;
    f->module=(XrXirSourceModule){.name="m",.name_length=1};
    f->declarations=(XrXirDeclarations){.modules=&f->module,.module_count=1,.functions=&f->identity,
        .map_literals=map?&f->received:NULL,.map_literal_count=map?1u:0u};
    f->input=(XrHirCollectionConstructInput){.compile=compile,.types=&f->types,
        .literal={.kind=map?XR_HIR_COLLECTION_MAP:XR_HIR_COLLECTION_ARRAY,.expected=(XrXirType)256,
            .has_expected=empty,.entries=entries?f->entries:NULL,.count=entries},
        .values=count?f->given:NULL,.count=count,.result=(XrXirType)256,.provenance=map?&f->literal:NULL};
    f->projection=(XrHirBodyProjection){.type=(XrXirType)256,.operation=f->instruction.op,.value=count,
        .source_arguments=count?f->sources:NULL,.source_argument_count=count,.source_value_map=f->relocation,
        .source_value_count=count+1,.has_instruction=true,.has_value=true,.collection_declarations=map?&f->declarations:NULL};
}
static XrHirBody *collection_projection_body(CollectionProjectionFixture *f,XrCompileResources *resources) {
    XrHirBody *body=NULL;CHECK(xr_hir_body_new(resources,&f->types,&body)==XR_XIR_OK);
    XrHirBodyDeclarations declarations={.functions=&f->function_view,.identities=&f->identity_view,
        .function_count=&f->functions,.module_count=&f->modules};
    CHECK(xr_hir_body_declarations(body,&declarations)==XR_XIR_OK);
    CHECK(xr_hir_body_scope(body,(XrHirBodyScope){0},f->input.origin,NULL,&f->input.scope)==XR_XIR_OK);
    return body;
}
static void collection_projection_invalid(XrHirBody *body,XrHirBodyUse use,CollectionProjectionFixture *f) {
    CHECK(xr_hir_body_project(body,use,&f->projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
}
static void collection_construct_invalid(XrHirBody *body,CollectionProjectionFixture *f,XrXirStatus expected) {
    XrHirBodyUse use={body,777};XrHirCollectionError error={.kind=XR_HIR_COLLECTION_NONVALUE};
    size_t live=physical_live,bytes=physical_bytes;
    CHECK(xr_hir_body_collection_construct(body,&f->input,NULL,&use,&error)==expected);
    CHECK(use.owner==body && use.index==777 && error.kind==XR_HIR_COLLECTION_NONVALUE && !xr_hir_body_use_count(body));
    CHECK(physical_live==live && physical_bytes==bytes);
}
static void collection_construct_malformed(XrHirBody *body,CollectionProjectionFixture *f) {
    XrXirType result=f->input.result;f->input.result=XR_XIR_I64;
    collection_construct_invalid(body,f,XR_XIR_BAD_TYPE);f->input.result=result;
    if(f->input.count) {
        XrXirType type=f->given[0].type;f->given[0].type=XR_XIR_BOOL;
        collection_construct_invalid(body,f,XR_XIR_BAD_TYPE);f->given[0].type=type;
        f->given[0].value=UINT32_MAX;collection_construct_invalid(body,f,XR_XIR_BAD_TYPE);f->given[0].value=0;
        f->input.values=NULL;collection_construct_invalid(body,f,XR_XIR_BAD_STRUCTURE);f->input.values=f->given;
        ++f->input.count;collection_construct_invalid(body,f,XR_XIR_BAD_TYPE);--f->input.count;
        if(f->input.literal.kind==XR_HIR_COLLECTION_ARRAY) {
            f->entries[0].spread=true;f->entries[0].value=(XrXirType)256;f->given[0].type=(XrXirType)256;
            collection_construct_invalid(body,f,XR_XIR_BAD_STRUCTURE);
            f->entries[0].spread=false;f->entries[0].value=XR_XIR_I64;f->given[0].type=XR_XIR_I64;
        }
    }
    if(f->input.provenance) {
        f->input.provenance=NULL;collection_construct_invalid(body,f,XR_XIR_BAD_STRUCTURE);f->input.provenance=&f->literal;
        ++f->literal.module;collection_construct_invalid(body,f,XR_XIR_BAD_STRUCTURE);--f->literal.module;
        ++f->literal.count;collection_construct_invalid(body,f,XR_XIR_BAD_STRUCTURE);--f->literal.count;
        if(f->literal.count) {
            uint32_t kind=f->origins[0].kind;f->origins[0].kind=2;
            collection_construct_invalid(body,f,XR_XIR_BAD_STRUCTURE);f->origins[0].kind=kind;
            f->origins[0].line=0;collection_construct_invalid(body,f,XR_XIR_BAD_STRUCTURE);f->origins[0].line=3;
        }
        if(f->literal.file.length) {
            ((unsigned char *)f->path)[0]=UINT8_MAX;collection_construct_invalid(body,f,XR_XIR_BAD_STRUCTURE);
            f->path[0]=0;collection_construct_invalid(body,f,XR_XIR_BAD_STRUCTURE);f->path[0]='x';
            f->literal.file.bytes=NULL;collection_construct_invalid(body,f,XR_XIR_BAD_STRUCTURE);f->literal.file.bytes=f->path;
        }
    }
}
static void collection_projection_owner(unsigned mode) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext compile={.resources=resources,.limits=xr_xir_compile_default_limits()};
    CollectionProjectionFixture f;collection_projection_fixture(&f,mode,&compile);
    XrHirBody *body=collection_projection_body(&f,resources);XrHirBodyUse use={0};XrHirCollectionError error={0};
    collection_construct_malformed(body,&f);
    size_t start=physical_attempts;
    CHECK(xr_hir_body_collection_construct(body,&f.input,NULL,&use,&error)==XR_XIR_OK);
    size_t allocations=physical_attempts-start;CHECK(allocations>0);
    memset(f.given,0xa5,sizeof(f.given));memset(f.entries,0xa5,sizeof(f.entries));
    memset(f.path,0xa5,sizeof(f.path));memset(f.origins,0xa5,sizeof(f.origins));
    XrHirBodyAction action;CHECK(xr_hir_body_consume(body,use,&action)==XR_XIR_OK &&
        xr_hir_body_emit(body,use,(XrXirType)256,f.input.count)==XR_XIR_OK);
    XrXirInstruction good=f.instruction;
    f.instruction.op=mode?XR_XIR_MAP_NEW:XR_XIR_MAP_LITERAL;collection_projection_invalid(body,use,&f);f.instruction=good;
    ++f.instruction.immediate;collection_projection_invalid(body,use,&f);f.instruction=good;
    f.instruction.type=XR_XIR_I64;collection_projection_invalid(body,use,&f);f.instruction=good;
    ++f.instruction.args[1];collection_projection_invalid(body,use,&f);f.instruction=good;
    f.instruction.args[0]=f.input.count+1;collection_projection_invalid(body,use,&f);f.instruction=good;
    f.instruction.targets[0]=1;collection_projection_invalid(body,use,&f);f.instruction=good;
    f.instruction.type_arguments[1]=1;collection_projection_invalid(body,use,&f);f.instruction=good;
    if(f.input.count) {
        uint32_t saved=f.arguments[0];f.arguments[0]=UINT32_MAX;collection_projection_invalid(body,use,&f);f.arguments[0]=saved;
        ++f.sources[0];collection_projection_invalid(body,use,&f);--f.sources[0];
        ++f.relocation[0];collection_projection_invalid(body,use,&f);--f.relocation[0];
        XrXirType type=f.parameters[0];f.parameters[0]=XR_XIR_BOOL;collection_projection_invalid(body,use,&f);f.parameters[0]=type;
    }
    if(mode) {
        f.projection.collection_declarations=NULL;collection_projection_invalid(body,use,&f);f.projection.collection_declarations=&f.declarations;
        f.declarations.map_literal_count=0;collection_projection_invalid(body,use,&f);f.declarations.map_literal_count=1;
        f.declarations.functions=NULL;collection_projection_invalid(body,use,&f);f.declarations.functions=&f.identity;
        ++f.identity.module;collection_projection_invalid(body,use,&f);--f.identity.module;
        ++f.received.count;collection_projection_invalid(body,use,&f);--f.received.count;
        if(f.received.count){++f.received_origins[0].column;collection_projection_invalid(body,use,&f);--f.received_origins[0].column;}
        if(f.received.file.length){f.received_path[0]='z';collection_projection_invalid(body,use,&f);f.received_path[0]='x';}
    }
    Cancel cancellation={0,SIZE_MAX};XrCompileRequestOptions options={.cancel_poll=cancel,.cancel_context=&cancellation};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_body_project(body,use,&f.projection,&control,NULL)==XR_XIR_OK);
    size_t project_polls=cancellation.polls;CHECK(project_polls>0);xr_hir_body_free(body);
    size_t live=physical_live,bytes=physical_bytes;
    for(size_t point=0;point<allocations;++point) {
        collection_projection_fixture(&f,mode,&compile);body=collection_projection_body(&f,resources);
        use=(XrHirBodyUse){body,777};error=(XrHirCollectionError){.kind=XR_HIR_COLLECTION_NONVALUE};
        physical_fail=physical_attempts+point;
        CHECK(xr_hir_body_collection_construct(body,&f.input,NULL,&use,&error)==XR_XIR_OUT_OF_MEMORY);
        physical_fail=SIZE_MAX;CHECK(use.owner==body && use.index==777 && error.kind==XR_HIR_COLLECTION_NONVALUE && !xr_hir_body_use_count(body));
        xr_hir_body_free(body);CHECK(physical_live==live && physical_bytes==bytes);
    }
    collection_projection_fixture(&f,mode,&compile);body=collection_projection_body(&f,resources);
    cancellation=(Cancel){0,SIZE_MAX};control=(XrCompileRequestControl){0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_body_collection_construct(body,&f.input,&control,&use,&error)==XR_XIR_OK);
    size_t construct_polls=cancellation.polls;xr_hir_body_free(body);CHECK(construct_polls>0);
    for(size_t point=1;point<=construct_polls;++point) {
        collection_projection_fixture(&f,mode,&compile);body=collection_projection_body(&f,resources);
        cancellation=(Cancel){0,point};control=(XrCompileRequestControl){0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        use=(XrHirBodyUse){body,777};error=(XrHirCollectionError){.kind=XR_HIR_COLLECTION_NONVALUE};
        CHECK(xr_hir_body_collection_construct(body,&f.input,&control,&use,&error)==XR_XIR_CANCELLED);
        CHECK(use.owner==body && use.index==777 && error.kind==XR_HIR_COLLECTION_NONVALUE && !xr_hir_body_use_count(body));
        xr_hir_body_free(body);CHECK(physical_live==live && physical_bytes==bytes);
    }
    for(size_t point=1;point<=project_polls;++point) {
        collection_projection_fixture(&f,mode,&compile);body=collection_projection_body(&f,resources);
        CHECK(xr_hir_body_collection_construct(body,&f.input,NULL,&use,&error)==XR_XIR_OK);
        CHECK(xr_hir_body_consume(body,use,&action)==XR_XIR_OK && xr_hir_body_emit(body,use,(XrXirType)256,f.input.count)==XR_XIR_OK);
        cancellation=(Cancel){0,point};control=(XrCompileRequestControl){0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_hir_body_project(body,use,&f.projection,&control,NULL)==XR_XIR_CANCELLED);
        CHECK(xr_hir_body_project(body,use,&f.projection,NULL,NULL)==XR_XIR_OK);
        xr_hir_body_free(body);CHECK(physical_live==live && physical_bytes==bytes);
    }
    xr_compile_resources_release(resources);CHECK(!physical_live && !physical_bytes);
}
static void collection_projection_receivers(void) {
    for(unsigned mode=0;mode<4;++mode)collection_projection_owner(mode);
}
