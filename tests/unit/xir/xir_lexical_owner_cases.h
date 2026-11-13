/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static void lexical_directory_ownership(void) {
    EffectMark baseline=effect_mark();
    XrXirInstruction code[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},7,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirBlock block={.first=0,.count=2};
    XrXirFunction function={.instructions=code,.instruction_count=2,.blocks=&block,
        .block_count=1,.result=XR_XIR_I64};
    XrXirModule module={.functions=&function,.function_count=1,.stage=XR_XIR_CHECKED};
    XirLexicalScope scopes[]={{UINT32_MAX,10},{0,11}};
    XirLexicalBinding binding={.key={0,1,11,0,0,UINT32_MAX},.scope=1,.value=0,
        .logical=XR_XIR_I64,.physical=XR_XIR_I64,.mode=XR_PARAM_READ,.role=XIR_LEXICAL_LOCAL};
    XirLexicalEvent events[]={
        {.block=0,.instruction=0,.scope=0,.target=UINT32_MAX,.binding=UINT32_MAX,
            .value=UINT32_MAX,.position=XIR_LEXICAL_BLOCK_ENTRY,.kind=XIR_LEXICAL_ENTER},
        {.block=0,.instruction=0,.scope=1,.target=1,.binding=0,.value=0,
            .position=XIR_LEXICAL_AFTER,.kind=XIR_LEXICAL_BIND},
        {.block=0,.instruction=1,.scope=1,.target=UINT32_MAX,.binding=UINT32_MAX,
            .value=0,.position=XIR_LEXICAL_BEFORE,.kind=XIR_LEXICAL_RETURN}};
    XirLexicalFunction row={scopes,2,&binding,1,events,3};
    XirLexicalProgram input={&row,1},copy={0};
    CHECK(xir_lexical_verify(&effect_context,&module,&input)==XR_XIR_OK);
    attempts=0;injected=false;
    CHECK(xir_lexical_copy(&effect_context,&input,&copy,NULL)==XR_XIR_OK);
    size_t sites=attempts;
    CHECK(sites==4 && copy.functions!=input.functions && copy.functions[0].scopes!=scopes &&
        copy.functions[0].bindings!=&binding && copy.functions[0].events!=events);
    binding.key.node=99;scopes[1].node=99;events[2].value=UINT32_MAX;
    CHECK(copy.functions[0].bindings[0].key.node==11 && copy.functions[0].scopes[1].node==11 &&
        copy.functions[0].events[2].value==0);
    XrXirProvenance provenance={.lexical=copy};
    XrXirArtifact artifact={.module=module};
    artifact.module.provenance=&provenance;
    XirLexicalFunction borrowed={0};
    CHECK(xr_xir_compile_lexical_function(&artifact,0,&borrowed)==XR_XIR_OK &&
        borrowed.events==copy.functions[0].events);
    CHECK(xr_xir_compile_lexical_function(&artifact,1,&borrowed)==XR_XIR_BAD_STRUCTURE &&
        borrowed.events==copy.functions[0].events);
    artifact.module.provenance=NULL;
    CHECK(xr_xir_compile_lexical_function(&artifact,0,&borrowed)==XR_XIR_UNRESOLVED &&
        borrowed.events==copy.functions[0].events);
    xir_lexical_free(&copy);
    binding.key.node=11;scopes[1].node=11;events[2].value=0;
    effect_mark_check(baseline);
    XrXirType unit_parameter=XR_XIR_UNIT;
    XrXirFunction unit_function={.parameters=&unit_parameter,.parameter_count=1};
    XrXirModule unit_module={.functions=&unit_function,.function_count=1,.stage=XR_XIR_CHECKED};
    XirLexicalBinding unit_binding={.key={.function=0},.scope=0,.value=0,
        .logical=XR_XIR_UNIT,.physical=XR_XIR_UNIT,.mode=XR_PARAM_READ,.role=XIR_LEXICAL_PARAMETER_READ};
    XirLexicalFunction unit_row={.scopes=scopes,.scope_count=1,.bindings=&unit_binding,.binding_count=1};
    XirLexicalProgram unit_program={&unit_row,1};
    CHECK(xir_lexical_verify(&effect_context,&unit_module,&unit_program)==XR_XIR_OK);
    for (size_t point=0;point<sites;++point) {
        attempts=0;injected=false;fail_at=point;
        CHECK(xir_lexical_copy(&effect_context,&input,&copy,NULL)==XR_XIR_OUT_OF_MEMORY &&
            injected && !copy.functions && !copy.function_count);
        fail_at=SIZE_MAX;
        effect_mark_check(baseline);
    }
    for (unsigned variant=0;variant<10;++variant) {
        XirLexicalScope bad_scopes[]={scopes[0],scopes[1]};
        XirLexicalBinding bad_binding=binding;
        XirLexicalEvent bad_events[]={events[0],events[1],events[2]};
        XirLexicalFunction bad={bad_scopes,2,&bad_binding,1,bad_events,3};
        XirLexicalProgram bad_program={&bad,1};
        switch (variant) {
        case 0:bad_scopes[1].parent=1;break;
        case 1:bad_binding.value=2;break;
        case 2:bad_binding.physical=XR_XIR_U64;break;
        case 3:bad_binding.key.function=1;break;
        case 4:bad_binding.role=XIR_LEXICAL_PARAMETER_READ;break;
        case 5:bad_events[1].binding=1;break;
        case 6:bad_events[2].instruction=2;break;
        case 7:bad_events[0].ordinal=1;break;
        case 8:bad_events[0].position=XIR_LEXICAL_NORMAL_EDGE;
            bad_events[0].ordinal=UINT32_MAX;break;
        default:bad_events[0].position=XIR_LEXICAL_PANIC_EDGE;
            bad_events[0].kind=XIR_LEXICAL_PANIC;break;
        }
        CHECK(xir_lexical_verify(&effect_context,&module,&bad_program)==XR_XIR_BAD_STRUCTURE);
        effect_mark_check(baseline);
    }
    puts("Lexical directory owned copy, query, retirement, 4 FI and 10 malformed controls PASS");
}
