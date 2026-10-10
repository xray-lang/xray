/* Real Checked factory followed by an owned pre-bottom refinement.
 * The current nested physical result and original outer advertisement are
 * independent. Producer death cannot change either receiving fact. */
static XrXirStatus conditional_invocation_reference_result(const XrXirCompileContext *context,bool oracle) {
    XrXirTypeNode nodes[4]={
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=(XrXirType)256,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=(XrXirType)258,.flags=XR_XIR_CALLABLE_ROOT_NONE}};
    XrXirTypes types={nodes,4,NULL,NULL};XrXirType capture=XR_XIR_I64;uint32_t operand=0;
    XrXirInstruction entry[5]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=40},
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)257,.immediate=1,.args={0,1}},
        {.op=XR_XIR_CALL_INDIRECT,.type=(XrXirType)256,.immediate=1},
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=2},
        {.op=XR_XIR_RETURN,.args={3,0}}};
    XrXirInstruction factory[2]={{.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=2,.args={0,1}},
        {.op=XR_XIR_RETURN,.args={1,0}}};
    XrXirInstruction callback[3]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2},
        {.op=XR_XIR_ADD_INT,.type=XR_XIR_I64,.args={0,1}},
        {.op=XR_XIR_RETURN,.args={2,0}}};
    XrXirInstruction init={.op=XR_XIR_RETURN};
    XrXirBlock blocks[4]={{.first=0,.count=5},{.first=0,.count=2},{.first=0,.count=3},{.first=0,.count=1}};
    XrXirFunction functions[4]={
        {.name="entry",.name_length=5,.result=XR_XIR_I64,.instructions=entry,.instruction_count=5,
         .operands=&operand,.operand_count=1,.blocks=&blocks[0],.block_count=1},
        {.name="factory",.name_length=7,.parameters=&capture,.parameter_count=1,
         .result=(XrXirType)256,.instructions=factory,.instruction_count=2,
         .operands=&operand,.operand_count=1,.blocks=&blocks[1],.block_count=1},
        {.name="callback",.name_length=8,.parameters=&capture,.parameter_count=1,.result=XR_XIR_I64,
         .instructions=callback,.instruction_count=3,.blocks=&blocks[2],.block_count=1},
        {.name="init",.name_length=4,.instructions=&init,.instruction_count=1,.blocks=&blocks[3],.block_count=1}};
    XrXirFunctionIdentity identities[4]={0};
    XrXirSourceModule source={.name="reference_result",.name_length=16,.initializer=3};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,.entry_function=0};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=4,.types=&types,
        .declarations=&declarations,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&module,&checked,&diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,&diagnostic);
    XrXirEffects effects={.resources=context->resources,.count=4};
    if (status==XR_XIR_OK) status=xir_effects_refinement_capture(context,&module,&effects);
    if (status==XR_XIR_OK) {
        /* This mirrors inferred Source changes after its immutable prefix was captured. */
        factory[0].type=(XrXirType)258;functions[1].result=(XrXirType)258;
        entry[1].type=(XrXirType)259;entry[2].type=(XrXirType)258;
    }
    EffectContextOwner *execution=NULL;
    if (status==XR_XIR_OK) status=effect_context_owner_build(context,&module,effects.refinement,&execution);
    xr_xir_compile_artifact_free(checked);effect_refinement_free(effects.refinement);effects.refinement=NULL;
    if (status==XR_XIR_OK && oracle) {
        CHECK(execution && !execution->refinement && execution->forest && execution->forest->invocations &&
            !execution->dense->uses.invocations);
        EffectOrdinaryContexts *dense=execution->dense;
        const EffectInvocationCertificate *certificate=execution->forest->invocations;
        uint32_t mask=UINT32_MAX;
        CHECK(effect_invocation_bounds_mask(context,execution->declared,0,1,&mask)==XR_XIR_OK &&
            mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
        CHECK(effect_invocation_bounds_mask(context,certificate->declared,0,2,&mask)==XR_XIR_OK &&
            mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
        const XrXirTypeNode *reference=xr_xir_callable_signature(&dense->terms.types,dense->functions[0].instructions[1].type);
        CHECK(reference && reference->result==dense->functions[1].result && reference->parameter_count==0);
        const XrXirTypeNode *returned=xr_xir_callable_signature(&dense->terms.types,reference->result);
        CHECK(returned && returned->result==XR_XIR_I64 && returned->parameter_count==0 &&
            returned->flags==XR_XIR_CALLABLE_ROOT_NONE);
        CHECK(certificate->bodies.functions!=dense->functions && certificate->bodies.function_count==dense->count &&
            certificate->bodies.functions[1].result==dense->functions[1].result &&
            certificate->terms.types.nodes!=dense->terms.types.nodes);
        CHECK(effect_lowered_snapshot_match(context,&certificate->terms.types,&dense->terms.types)==XR_XIR_OK);
        /* Public nested/UNKNOWN rules remain exact. */
        CHECK(xr_xir_compile_callable_weakening(context,&dense->terms.types,(XrXirType)259,(XrXirType)257)==XR_XIR_BAD_TYPE);
        CHECK(xr_xir_compile_callable_weakening(context,&dense->terms.types,(XrXirType)256,(XrXirType)258)==XR_XIR_BAD_TYPE);
        /* Mutation controls recheck the same real copied site, never publish an
         * equation or grant a binding. All out-types stay masked on failure. */
        dense->terms.remaining=context;
        XrXirType bound=(XrXirType)257;
        CHECK(effect_invocation_context_reference_result(dense,&module,0,1,&bound)==XR_XIR_OK);
        const XrXirTypeNode *original=xr_xir_callable_signature(&dense->terms.types,bound);
        CHECK(original && original->result==(XrXirType)258 && original->flags==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
        XrXirInstruction *working=(XrXirInstruction *)dense->functions[0].instructions;
        working[0].type=XR_XIR_STRING;bound=(XrXirType)257;
        CHECK(effect_invocation_context_reference_result(dense,&module,0,1,&bound)==XR_XIR_BAD_TYPE && bound==(XrXirType)257);
        working[0].type=XR_XIR_I64;
        functions[1].parameters=NULL;bound=(XrXirType)257;
        CHECK(effect_invocation_context_reference_result(dense,&module,0,1,&bound)==XR_XIR_BAD_TYPE && bound==(XrXirType)257);
        functions[1].parameters=&capture;
        XrXirGeneric generic[4]={0};generic[0].parameter_count=1;module.generics=generic;
        CHECK(effect_invocation_context_reference_result(dense,&module,0,1,&bound)==XR_XIR_BAD_TYPE && bound==(XrXirType)257);
        module.generics=NULL;
        XrXirOp saved_op=entry[1].op;entry[1].op=XR_XIR_COPY;
        CHECK(effect_invocation_context_reference_result(dense,&module,0,1,&bound)==XR_XIR_BAD_TYPE && bound==(XrXirType)257);
        entry[1].op=saved_op;
        XrXirTypeNode deeper=*xr_xir_callable_signature(&dense->terms.types,(XrXirType)257);
        deeper.result=(XrXirType)257;bound=effect_term_intern(&dense->terms,deeper);
        CHECK(dense->terms.status==XR_XIR_OK);XrXirType unchanged=bound;
        CHECK(effect_invocation_context_reference_result(dense,&module,0,1,&bound)==XR_XIR_BAD_TYPE && bound==unchanged);
        XrXirType cell=effect_term_intern(&dense->terms,(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64});
        XrXirCallableParameter ref={cell,XR_PARAM_REF};
        XrXirTypeNode ref_shape={.kind=XR_XIR_TYPE_CALLABLE,.result=(XrXirType)258,
            .parameters=&ref,.parameter_count=1,.flags=XR_XIR_CALLABLE_ROOT_NONE};
        XrXirType actual_ref=effect_term_intern(&dense->terms,ref_shape);
        ref_shape.result=(XrXirType)256;ref_shape.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
        bound=effect_term_intern(&dense->terms,ref_shape);unchanged=bound;
        CHECK(dense->terms.status==XR_XIR_OK);
        XrXirType prefix[2]={XR_XIR_I64,cell};functions[1].parameters=prefix;functions[1].parameter_count=2;
        entry[1].type=actual_ref;working[1].type=actual_ref;
        CHECK(effect_invocation_context_reference_result(dense,&module,0,1,&bound)==XR_XIR_BAD_TYPE && bound==unchanged);
        functions[1].parameters=&capture;functions[1].parameter_count=1;
        entry[1].type=(XrXirType)259;working[1].type=(XrXirType)259;
        dense->terms.remaining=NULL;
    }
    /* The complete certificate moved to forest exactly once and owns its
     * proof independently from every producer record or mutation above. */
    memset(nodes,0,sizeof(nodes));memset(functions,0,sizeof(functions));memset(entry,0,sizeof(entry));
    memset(factory,0,sizeof(factory));memset(callback,0,sizeof(callback));
    if (status==XR_XIR_OK && oracle) {
        const EffectInvocationCertificate *certificate=execution->forest->invocations;
        uint32_t mask=UINT32_MAX;
        CHECK(!execution->dense->uses.invocations && !certificate->terms.remaining && certificate->equations &&
            !certificate->equations->work && !certificate->equations->module && !certificate->equations->graph);
        CHECK(effect_invocation_bounds_mask(context,certificate->declared,0,1,&mask)==XR_XIR_OK &&
            mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
        CHECK(effect_invocation_certificate_node_mask(context,certificate,certificate->equations->roots[1],&mask)==XR_XIR_OK && !mask);
    }
    effect_context_owner_free(execution);return status;
}
