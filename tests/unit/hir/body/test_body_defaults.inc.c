/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Default projections preserve producers, conversions and owned instantiations. */
typedef struct DefaultCancel {XrCompileResources *resources;size_t held;} DefaultCancel;
static bool default_cancel_staged(const void *opaque) {
    const DefaultCancel *state=opaque;XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(state->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats.held_bytes>state->held;
}
static void default_receivers(void) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirTypes types={0};XrXirType formals[]={XR_XIR_I64,XR_XIR_I64};
    XrXirFunction functions[]={{.result=XR_XIR_I64},{.parameters=formals,.parameter_count=2,.result=XR_XIR_I64}},*view=functions;
    XrXirFunctionIdentity identities[2]={{0},{0}},*identity_view=identities;
    uint32_t function_count=2,module_count=1,slot_count=0;XrXirSlot *slots=NULL;
    XrXirDefaultBinding records[]={{.owner_kind=XR_XIR_DEFAULT_PARAMETER,.owner=1,.ordinal=0,.function=0},
        {.owner_kind=XR_XIR_DEFAULT_PARAMETER,.owner=1,.ordinal=1,.function=0}};
    XrXirDefaultTable defaults={.records=records,.count=2};XrXirInterfaceTable interfaces={0};
    XrHirBodyDeclarations declarations={&view,&identity_view,&function_count,&module_count,&defaults,&interfaces,&slots,&slot_count};
    XrHirBody *body=NULL;CHECK(xr_hir_body_new(resources,&types,&body)==XR_XIR_OK);
    CHECK(xr_hir_body_declarations(body,&declarations)==XR_XIR_OK);
    XrHirBodyScope scope={0};CHECK(xr_hir_body_scope(body,scope,(XrHirBodyOrigin){0},NULL,&scope)==XR_XIR_OK);
    XrHirSignatureInput declaration={.types=&types,.scope=scope,.family=XR_HIR_CALL_FUNCTION,.function=1,.count=2};
    XrHirBodySignature signature={0};CHECK(xr_hir_body_signature(body,&declaration,NULL,&signature,NULL)==XR_XIR_OK);
    XrXirType arguments[]={XR_XIR_I64,XR_XIR_STRING},expected[]={XR_XIR_I64,XR_XIR_STRING};
    XrHirCallUseInput input={.types=&types,.scope=scope,.kind=XR_HIR_CALL_FINAL,.type=XR_XIR_I64,
        .value=42,.default_value=41,.default_call=40,.default_conversion=XR_XIR_COPY,
        .substitution=arguments,.substitution_count=2,.defaulted=true,.default_function=0};
    XrHirBodyUse sentinel={body,123},use=sentinel;XrHirBodyAction action;
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t live=physical_live,bytes=physical_bytes;
    input.kind=XR_HIR_CALL_PLAN;
    CHECK(xr_hir_body_call_argument(body,signature,&input,NULL,&use,&action)==XR_XIR_BAD_STRUCTURE&&use.index==123);
    input.kind=XR_HIR_CALL_FINAL;input.default_function=1;
    CHECK(xr_hir_body_call_argument(body,signature,&input,NULL,&use,&action)==XR_XIR_BAD_STRUCTURE&&use.index==123);
    input.default_function=0;
    CHECK(!xr_hir_body_use_count(body)&&xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK&&
        after.held_bytes==before.held_bytes&&physical_live==live&&physical_bytes==bytes);
    /* The vector allocation and the following use-table growth are distinct
     * rollback boundaries, each with no published use or retained staging. */
    physical_fail=physical_attempts;
    CHECK(xr_hir_body_call_argument(body,signature,&input,NULL,&use,&action)==XR_XIR_OUT_OF_MEMORY);physical_fail=SIZE_MAX;
    CHECK(use.owner==sentinel.owner&&use.index==sentinel.index&&!xr_hir_body_use_count(body));
    physical_fail=physical_attempts+1;
    CHECK(xr_hir_body_call_argument(body,signature,&input,NULL,&use,&action)==XR_XIR_OUT_OF_MEMORY);physical_fail=SIZE_MAX;
    CHECK(use.index==sentinel.index&&!xr_hir_body_use_count(body));
    DefaultCancel cancellation={resources,before.held_bytes};XrCompileRequestOptions options={.cancel_poll=default_cancel_staged,.cancel_context=&cancellation};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_body_call_argument(body,signature,&input,&control,&use,&action)==XR_XIR_CANCELLED);
    CHECK(use.index==sentinel.index&&!xr_hir_body_use_count(body)&&physical_live==live&&physical_bytes==bytes);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    input.default_conversion=XR_XIR_INVALID;
    CHECK(xr_hir_body_call_argument(body,signature,&input,NULL,&use,&action)==XR_XIR_BAD_STRUCTURE);
    input.default_conversion=XR_XIR_COPY;
    CHECK(xr_hir_body_call_argument(body,signature,&input,NULL,&use,&action)==XR_XIR_OK);
    CHECK(xr_hir_body_consume(body,use,&action)==XR_XIR_OK);
    memset(arguments,0xa5,sizeof(arguments));
    XrHirBodyProjection projection={.type=XR_XIR_I64,.source_value=42,.has_value=true,.value=42,
        .has_default=true,.default_owner=1,.default_value=41,.default_call=40,.default_conversion=XR_XIR_COPY,
        .default_type_arguments=expected,.default_type_argument_count=2};
    projection.default_value=43;CHECK(xr_hir_body_project(body,use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);projection.default_value=41;
    projection.default_call=39;CHECK(xr_hir_body_project(body,use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);projection.default_call=40;
    projection.default_conversion=XR_XIR_FUNCTION_WEAKEN;CHECK(xr_hir_body_project(body,use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);projection.default_conversion=XR_XIR_COPY;
    projection.default_owner=0;CHECK(xr_hir_body_project(body,use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);projection.default_owner=1;
    projection.default_parameter=1;CHECK(xr_hir_body_project(body,use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);projection.default_parameter=0;
    projection.default_type_argument_count=1;CHECK(xr_hir_body_project(body,use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);projection.default_type_argument_count=2;
    projection.default_type_arguments=NULL;CHECK(xr_hir_body_project(body,use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);projection.default_type_arguments=expected;
    expected[1]=XR_XIR_BOOL;CHECK(xr_hir_body_project(body,use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);expected[1]=XR_XIR_STRING;
    CHECK(xr_hir_body_project(body,use,&projection,NULL,NULL)==XR_XIR_OK);
    input.formal=input.argument=1;input.value=43;input.default_value=input.default_call=43;input.default_conversion=XR_XIR_INVALID;
    use=sentinel;CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_body_call_argument(body,signature,&input,NULL,&use,&action)==XR_XIR_BAD_STRUCTURE&&use.index==123);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    memcpy(arguments,expected,sizeof(arguments));
    CHECK(xr_hir_body_call_argument(body,signature,&input,NULL,&use,&action)==XR_XIR_OK);
    projection=(XrHirBodyProjection){.formal=1,.argument=1,.type=XR_XIR_I64,.source_value=43,.has_value=true,.value=43,
        .has_default=true,.default_owner=1,.default_parameter=1,.default_value=43,.default_call=43,
        .default_type_arguments=expected,.default_type_argument_count=2};
    owned_projection(body,use,projection);
    XrXirInstruction call={.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=1};
    CHECK(xr_hir_body_call_finish(body,signature,&call,NULL,&use,&action)==XR_XIR_OK);
    uint32_t actuals[]={42,43};
    owned_projection(body,use,(XrHirBodyProjection){.type=XR_XIR_I64,.operation=XR_XIR_CALL,.call=call,
        .has_call=true,.has_instruction=true,.source_arguments=actuals,.source_argument_count=2});
    CHECK(xr_hir_body_finish(body,NULL)==XR_XIR_OK);
    xr_hir_body_end(body);xr_hir_body_free(body);xr_compile_resources_release(resources);
    CHECK(!physical_live&&!physical_bytes);
}
