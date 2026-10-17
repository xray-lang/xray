/* Actual internal formal roles and complete external raw admission. */
#ifndef XIR_ORDINARY_INITIAL_ROLE_CASES_H
#define XIR_ORDINARY_INITIAL_ROLE_CASES_H
static bool conditional_initial_role_refuse(const void *owner,uint64_t work) {
    (void)owner;(void)work;return false;
}
static void conditional_initial_role_literals(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    XrXirTypeNode callable={.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
        .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    XrXirTypes types={&callable,1,NULL,NULL};XrXirType parameters[2]={(XrXirType)256,XR_XIR_I64};
    XrXirInstruction ops[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42},
        {.op=XR_XIR_RETURN,.args={2,0}}},done={.op=XR_XIR_RETURN};
    XrXirFunction functions[2]={{.parameters=parameters,.parameter_count=2,.result=XR_XIR_I64,
        .instructions=ops,.instruction_count=2},{.result=XR_XIR_UNIT,.instructions=&done,.instruction_count=1}};
    XrXirFunctionIdentity identities[2]={0};XrXirSourceModule module_name={"initial",7,NULL,0,1};
    XrXirDeclarations declarations={.modules=&module_name,.module_count=1,.functions=identities};
    XrXirModule source={.stage=XR_XIR_BUILT,.types=&types,.functions=functions,.function_count=2,
        .declarations=&declarations,.linkage_kind=XR_XIR_PROGRAM};
    EffectOrdinaryContexts *contexts=NULL;EffectBodyMaterial *material=NULL;
    CHECK(effect_ordinary_build(&context,&source,NULL,&contexts)==XR_XIR_OK);
    contexts->terms.remaining=&context;EffectOrdinaryView view={0};
    CHECK(effect_ordinary_view(contexts,&source,&view)==XR_XIR_OK && !view.module.provenance);
    CHECK(effect_body_material_generation_input(&context,&view.module,NULL,&view.input,NULL,&material)==XR_XIR_OK);
    CHECK(material->ordinary_source_count==2 && material->ordinary_identities && !material->evidence_kind &&
        material->ordinary_identities[0].declaration==0 && material->ordinary_identities[0].owner==UINT32_MAX &&
        material->functions[0].parameters[0]==(XrXirType)256 &&
        material->functions[0].initial_roles[0]==XR_XIR_EFFECT_PARAMETER_VARIABLE &&
        material->functions[0].initial_roles[1]==XR_XIR_EFFECT_PARAMETER_FIXED);
    CHECK(effect_body_initial_roles_match(&context,material,0)==XR_XIR_OK);
    uint8_t *roles=(uint8_t *)material->functions[0].initial_roles;
    roles[0]=XR_XIR_EFFECT_PARAMETER_FIXED;
    CHECK(effect_body_initial_roles_match(&context,material,0)==XR_XIR_BAD_STRUCTURE);
    roles[0]=XR_XIR_EFFECT_PARAMETER_VARIABLE;roles[1]=XR_XIR_EFFECT_PARAMETER_VARIABLE;
    CHECK(effect_body_initial_roles_match(&context,material,0)==XR_XIR_BAD_STRUCTURE);
    roles[1]=XR_XIR_EFFECT_PARAMETER_FIXED;
    CHECK(effect_body_source_match(&context,material,&view.module)==XR_XIR_BAD_STRUCTURE);
    EffectBodyMaterial *empty=NULL,*occupied=material;size_t attempts=rp_attempts;
    CHECK(effect_body_material_generation_input(&context,&view.module,NULL,&view.input,NULL,&occupied)==XR_XIR_BAD_STRUCTURE &&
        occupied==material && rp_attempts==attempts);
    XrXirModule wrong=view.module;XrXirTypes foreign=*wrong.types;wrong.types=&foreign;
    CHECK(effect_body_material_generation_input(&context,&wrong,NULL,&view.input,NULL,&empty)==XR_XIR_BAD_STRUCTURE && !empty);
    wrong=view.module;--wrong.function_count;
    CHECK(effect_body_material_generation_input(&context,&wrong,NULL,&view.input,NULL,&empty)==XR_XIR_BAD_STRUCTURE && !empty);
    uint32_t count=contexts->count;contexts->count=contexts->capacity+1;
    CHECK(effect_body_material_generation_input(&context,&view.module,NULL,&view.input,NULL,&empty)==XR_XIR_BAD_STRUCTURE && !empty);
    contexts->count=count;
    XrXirType *current_parameters=(XrXirType *)contexts->functions[0].parameters;
    XrXirType saved_type=current_parameters[1];current_parameters[1]=(XrXirType)UINT32_MAX;
    CHECK(effect_body_material_generation_input(&context,&view.module,NULL,&view.input,NULL,&empty)==XR_XIR_BAD_STRUCTURE && !empty);
    current_parameters[1]=saved_type;
    uint32_t declaration=contexts->nodes[0].declaration;contexts->nodes[0].declaration=2;
    CHECK(effect_body_material_generation_input(&context,&view.module,NULL,&view.input,NULL,&empty)==XR_XIR_BAD_STRUCTURE && !empty);
    contexts->nodes[0].declaration=declaration;
    XrXirCompileContext foreign_owner=rp_owner(rp_caps());
    uint64_t foreign_baseline=rp_stats(&foreign_owner).live_bytes;
    XrCompileResources *ledger=contexts->uses.resources;contexts->uses.resources=foreign_owner.resources;
    CHECK(effect_body_material_generation_input(&context,&view.module,NULL,&view.input,NULL,&empty)==XR_XIR_BAD_STRUCTURE && !empty);
    contexts->uses.resources=ledger;rp_owner_free(&foreign_owner,foreign_baseline);
    XrXirProvenance missing={.kind=XR_XIR_EVIDENCE_TEMPLATE};source.provenance=&missing;
    CHECK(effect_body_material_create(&context,&source,&empty)==XR_XIR_BAD_STRUCTURE && !empty);
    source.provenance=NULL;
    effect_ordinary_free(contexts);contexts=NULL;
    memset(functions,0xCE,sizeof(functions));memset(&callable,0xCE,sizeof(callable));
    CHECK(effect_body_initial_roles_match(&context,material,0)==XR_XIR_OK &&
        material->functions[0].parameters[0]==(XrXirType)256 &&
        material->functions[0].initial_roles[0]==XR_XIR_EFFECT_PARAMETER_VARIABLE);
    XrXirInstruction literal_ops[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42},
        {.op=XR_XIR_RETURN,.args={2,0}}};
    XrXirType literal_parameters[2]={(XrXirType)256,XR_XIR_I64};
    XrXirFunction literal={.parameters=literal_parameters,.parameter_count=2,.result=XR_XIR_I64,
        .instructions=literal_ops,.instruction_count=2};
    CHECK(effect_body_function_match(&context,&material->functions[0],&literal)==XR_XIR_OK);
    literal_ops[0].immediate=43;
    CHECK(effect_body_function_match(&context,&material->functions[0],&literal)==XR_XIR_BAD_STRUCTURE);
    literal_ops[0].immediate=42;
    EffectBodyRequest read=effect_body_owned_request(&context,material);
    EffectBodyInstructionShape shape={XR_XIR_INVALID,XR_XIR_UNIT},sentinel=shape;
    read.read.consume=conditional_initial_role_refuse;
    CHECK(effect_body_request_shape(&read,0,0,&shape)==XR_XIR_BUDGET && !memcmp(&shape,&sentinel,sizeof(shape)));
    read=effect_body_owned_request(&context,material);
    CHECK(effect_body_request_shape(&read,0,0,&shape)==XR_XIR_OK && shape.op==XR_XIR_CONST_INT && shape.type==XR_XIR_I64);
    effect_body_material_free(material);rp_owner_free(&context,baseline);rp_balanced(physical);
}
#endif
