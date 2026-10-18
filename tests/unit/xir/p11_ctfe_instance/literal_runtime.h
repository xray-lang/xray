/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * literal_runtime.h - Ordinary exported callers execute actual literal providers
 */
static XrXirArtifact *literal_runtime_callers(XrXirCompileContext *context,const XirLiteralDefinitions *result) {
    const XrXirModule *module=&result->definition->module;uint32_t count=module->function_count;XrXirStatus status=XR_XIR_OK;
    XrXirFunction *functions=xir_compile_calloc(context,count+2,sizeof(*functions),&status);
    XrXirFunctionIdentity *identities=xir_compile_calloc(context,count+2,sizeof(*identities),&status);
    XrXirGeneric *generics=xir_compile_calloc(context,count+2,sizeof(*generics),&status);
    XrXirFunctionEffectContract *contracts=xir_compile_calloc(context,count+2,sizeof(*contracts),&status);
    CHECK(status==XR_XIR_OK && functions && identities && generics && contracts);
    CHECK(xir_compile_work(context,(uint64_t)count*(sizeof(*functions)+sizeof(*identities)+sizeof(*generics)+sizeof(*contracts))));
    memcpy(functions,module->functions,(size_t)count*sizeof(*functions));
    memcpy(identities,module->declarations->functions,(size_t)count*sizeof(*identities));
    memcpy(generics,module->generics,(size_t)count*sizeof(*generics));
    memcpy(contracts,module->provenance->contracts,(size_t)count*sizeof(*contracts));
    XrXirInstruction ops[2][2];XrXirBlock blocks[2]={{.first=0,.count=2},{.first=0,.count=2}};
    for (unsigned i=0;i<2;++i) {
        uint32_t provider=result->bindings[i].provider;CHECK(module->functions[provider].result==XR_XIR_I64);
        ops[i][0]=(XrXirInstruction){XR_XIR_CALL,XR_XIR_I64,{0},{0},provider,{0}};
        ops[i][1]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
        functions[count+i]=(XrXirFunction){.name=i?"literal_two":"literal_one",.name_length=11,.result=XR_XIR_I64,
            .blocks=&blocks[i],.block_count=1,.instructions=ops[i],.instruction_count=2};
        identities[count+i]=(XrXirFunctionIdentity){.module=module->declarations->functions[provider].module,.exported=1};
    }
    XrXirProvenance proof=*module->provenance;proof.contract_count=count+2;proof.contracts=contracts;
    XrXirDeclarations declarations=*module->declarations;declarations.functions=identities;
    XrXirModule built=*module;built.stage=XR_XIR_BUILT;built.function_count=count+2;built.functions=functions;
    built.generics=generics;built.declarations=&declarations;built.provenance=&proof;XrXirArtifact *output=NULL;
    CHECK(xr_xir_compile_check_v2(context,&built,result->definition->construction,&output,NULL)==XR_XIR_OK);
    xr_compile_resources_free(functions);xr_compile_resources_free(identities);xr_compile_resources_free(generics);xr_compile_resources_free(contracts);
    return output;
}
static void literal_runtime_instances(XrXirArtifact **lowered) {
    uint32_t entries[2]={named(&(*lowered)->module,"literal_one"),named(&(*lowered)->module,"literal_two")};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(lowered,&program)==XR_XIR_OK && !*lowered);
    for (unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entries[i],NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64 && value.payload==42);xr_xir_value_drop(&value);
        CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY && xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);fprintf(stderr,"Typed literal actual providers through ordinary exported callers two real VM Instances returned42\n");
}
