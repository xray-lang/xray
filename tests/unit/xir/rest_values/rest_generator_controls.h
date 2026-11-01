/* Open generator sites retain the same independent structural obligations. */
#ifndef XR_REST_GENERATOR_CONTROLS_H
#define XR_REST_GENERATOR_CONTROLS_H
static void rest_generator_controls(XrXirArtifact *artifact) {
    const XrXirModule *module=&artifact->module;
    uint32_t bodies[2]={UINT32_MAX,UINT32_MAX},sites[2]={0,0};
    for(uint32_t f=0;f<module->function_count;++f)for(uint32_t i=0;i<module->functions[f].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[f].instructions[i];
        if(op->op!=XR_XIR_GENERATOR_NEW || !op->args[1])continue;
        unsigned open=xr_xir_type_span(module->types,op->type)!=0;
        if(bodies[open]==UINT32_MAX){bodies[open]=f;sites[open]=i;}
    }
    CHECK(bodies[0]!=UINT32_MAX&&bodies[1]!=UINT32_MAX);
    CHECK(module->function_count<128);uint64_t before=held(artifact->context.resources);
    for(unsigned open=0;open<2;++open)for(unsigned attack=0;attack<7;++attack) {
        XrXirFunction functions[128];XrXirFunctionIdentity ids[128];
        memcpy(functions,module->functions,module->function_count*sizeof(*functions));
        memcpy(ids,module->declarations->functions,module->function_count*sizeof(*ids));
        uint32_t body=bodies[open],count=functions[body].instruction_count;
        XrXirInstruction *instructions=xr_malloc((size_t)count*sizeof(*instructions));CHECK(instructions);
        memcpy(instructions,functions[body].instructions,(size_t)count*sizeof(*instructions));
        functions[body].instructions=instructions;XrXirInstruction *op=&instructions[sites[open]];
        uint32_t target=(uint32_t)op->immediate;
        if(attack==0)op->immediate=-1;
        if(attack==1)op->immediate=module->function_count;
        if(attack==2)op->immediate=rest_find(module,"ordinary");
        if(attack==3)op->type=XR_XIR_I64;
        if(attack==4)--op->args[1];
        if(attack==5)ids[target].promises&=~XR_XIR_FUNCTION_GENERATOR;
        if(attack==6)ids[target].cleanup_owner=body+1;
        XrXirDeclarations declarations=*module->declarations;declarations.functions=ids;
        XrXirArtifact invalid=*artifact;invalid.module.functions=functions;invalid.module.declarations=&declarations;
        CHECK(xr_xir_compile_artifact_verify(&invalid,NULL)!=XR_XIR_OK);
        xr_free(instructions);CHECK(held(artifact->context.resources)==before);
    }
    CHECK(xr_xir_compile_artifact_verify(artifact,NULL)==XR_XIR_OK);
    puts("Rest open/ground generator target, result, capture and cleanup refusals 14 preserve owners");
}
#endif
