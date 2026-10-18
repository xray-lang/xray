/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_history_function_value.h - Escaped birth closures through actual Source
 */
#ifndef SOURCE_HISTORY_FUNCTION_VALUE_H
#define SOURCE_HISTORY_FUNCTION_VALUE_H
static void history_record_coordinates(XrXirSourceGenerations *owner,
    const XrXirSourceGenerationView *current) {
    XrXirSourceGenerationView birth={0};
    CHECK(xr_xir_compile_source_generation_view(owner,1,&birth)==XR_XIR_OK);
    const XrXirModule *modules[]={xr_xir_compile_artifact_module(birth.result->checked),
        xr_xir_compile_artifact_module(current->result->checked)};
    XrXirType records[]={XR_XIR_UNIT,XR_XIR_UNIT};
    for (unsigned m=0;m<2;++m) {
        for (uint32_t f=0;f<modules[m]->function_count;++f) {
            const XrXirFunction *fn=&modules[m]->functions[f];
            if (fn->name_length!=5 || memcmp(fn->name,"fetch",5)) continue;
            CHECK(records[m]==XR_XIR_UNIT);records[m]=fn->result;
        }
        const XrXirTypeNode *shape=xr_xir_type_node(modules[m]->types,records[m]);
        CHECK(shape && shape->kind==XR_XIR_TYPE_RECORD && shape->parameter_count==2 &&
            shape->record.names[0].length==5 && !memcmp(shape->record.names[0].bytes,"apply",5) &&
            xr_xir_callable_signature(modules[m]->types,shape->parameters[0].type) &&
            shape->record.names[1].length==5 && !memcmp(shape->record.names[1].bytes,"items",5) &&
            xr_xir_array_element(modules[m]->types,shape->parameters[1].type)==XR_XIR_I64);
    }
    printf("RECORD_PROGRAM_COORDINATES birth=%u current=%u\n",(unsigned)records[0],(unsigned)records[1]);
    CHECK(records[0]!=records[1]);
}
static int history_function_value(bool record) {

    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    XrXirSourceGenerationConfig config={compiler.context,record?"birth-record-value":"birth-function-value",NULL,NULL,NULL};
    XrXirSourceGenerations *owner=NULL;
    CHECK(xr_xir_compile_source_generations_new(&config,&owner)==XR_XIR_OK);
    XrXirInstanceConfig runtime;CHECK(xr_xir_instance_config_init(&runtime,sizeof(runtime))==XR_XIR_CALL_READY);
    unsigned outputs=0;runtime.output=(XrXirOutputProvider){.abi_version=XR_XIR_CALL_ABI_VERSION,
        .write=generation_output,.context=&outputs};
    XrXirInstance *instance=NULL,*foreign=NULL;
    CHECK(xr_xir_instance_generation_new(&runtime,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_generation_new(&runtime,&foreign)==XR_XIR_CALL_READY);
    XrXirProgram *programs[4]={0};XrXirValue escaped[2]={{0}};
    for (unsigned i=0;i<4;++i) {
        uint64_t source=prepare(owner,record?source_history_record_input(i+1,true):source_history_function_input(i+1,true));XrXirSourceGenerationView view={0};uint32_t entry=0;
        CHECK(xr_xir_compile_source_generation_view(owner,source,&view)==XR_XIR_OK);
        if (record && i==1) history_record_coordinates(owner,&view);
        programs[i]=program(&view,i==0?"seed":i==3?"fail":"answer",&entry);
        bind_program(&compiler,owner,source,programs[i]);uint64_t candidate=0;
        XrXirCallStatus prepared=xr_xir_source_generation_prepare_instance(owner,source,instance,&candidate);
        if (prepared!=XR_XIR_CALL_READY) fprintf(stderr,"birth function value prepare generation=%u status=%u\n",i+1,prepared);
        CHECK(prepared==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
        XrXirCallStatus status=generation_poll(instance);
        if (status!=(i==3?XR_XIR_CALL_ASSERTION:XR_XIR_CALL_RETURNED))
            fprintf(stderr,"birth function value generation=%u status=%u\n",i+1,status);
        CHECK(status==(i==3?XR_XIR_CALL_ASSERTION:XR_XIR_CALL_RETURNED));
        if (i==3) {
            CHECK(xr_xir_instance_generation_publish(instance,candidate)==XR_XIR_CALL_BAD_STATE);
            CHECK(xr_xir_instance_generation_abort(instance,candidate)==XR_XIR_CALL_READY);
            CHECK(xr_xir_compile_source_generation_abort(owner,source)==XR_XIR_OK);
            continue;
        }
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        if (!i) {CHECK(value.type==XR_XIR_I64 && !value.payload);xr_xir_value_drop(&value);}
        else {
            const XrXirFunctionBinding *binding=xr_xir_function_binding(&value);
            CHECK(binding && binding->capture_count==1 && xr_xir_value_arena(&value)==
                xr_xir_value_arena(&binding->captures[0]));
            CHECK(xr_xir_instance_start_function(instance,&value,NULL,0)==XR_XIR_CALL_BAD_ARGUMENT);
            CHECK(xr_xir_instance_start_function(foreign,&value,NULL,0)==XR_XIR_CALL_BAD_ARGUMENT);
            escaped[i-1]=value;
        }
        CHECK(xr_xir_instance_generation_publish(instance,candidate)==XR_XIR_CALL_READY);
        source_publish(&compiler,owner,source);
        if (i) {
            CHECK(xr_xir_instance_start_function(instance,&escaped[i-1],NULL,0)==XR_XIR_CALL_READY &&
                generation_poll(instance)==XR_XIR_CALL_RETURNED);
            XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
            CHECK(result.type==XR_XIR_I64 && result.payload==41+i);xr_xir_value_drop(&result);
        }
    }
    if (record) {
        xr_xir_compile_source_generations_free(owner);owner=NULL;
        for (unsigned i=0;i<4;++i) {xr_xir_compile_program_drop(programs[i]);programs[i]=NULL;}
    }
    CHECK(xr_xir_instance_start_function(instance,&escaped[1],NULL,0)==XR_XIR_CALL_READY &&
        generation_poll(instance)==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && result.payload==44);xr_xir_value_drop(&result);
    CHECK(outputs==1 && xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start_function(instance,&escaped[1],NULL,0)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && xr_xir_instance_free(foreign)==XR_XIR_CALL_READY);
    xr_xir_compile_source_generations_free(owner);
    for (unsigned i=0;i<4;++i) xr_xir_compile_program_drop(programs[i]);
    const XrXirFunctionBinding *binding=xr_xir_function_binding(&escaped[1]);
    CHECK(binding && binding->capture_count==1);
    CHECK(xr_xir_cell_read(&binding->captures[0],&result)==XR_XIR_VALUE_OK && result.payload==44);
    xr_xir_value_drop(&result);xr_xir_value_drop(&escaped[0]);xr_xir_value_drop(&escaped[1]);
    source_fixture_owner_free(&compiler);
    puts("Received birth functions preserved exact Program/captures,host42/43/abort44,pending and foreign refusal,dead-producer capture44 and compiler baseline");
    return 0;
}
static int history_function_call(bool record) {

    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    XrXirSourceGenerationConfig config={compiler.context,record?"birth-record-call":"birth-closure-probe",NULL,NULL,NULL};
    XrXirSourceGenerations *owner=NULL;CHECK(xr_xir_compile_source_generations_new(&config,&owner)==XR_XIR_OK);
    XrXirInstanceConfig runtime;CHECK(xr_xir_instance_config_init(&runtime,sizeof(runtime))==XR_XIR_CALL_READY);
    unsigned outputs=0;runtime.output=(XrXirOutputProvider){.abi_version=XR_XIR_CALL_ABI_VERSION,.write=generation_output,.context=&outputs};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_generation_new(&runtime,&instance)==XR_XIR_CALL_READY);
    XrXirProgram *programs[4]={0};uint32_t entries[4]={0};
    for(unsigned i=0;i<4;++i) {
        fprintf(stderr,"BIRTH_CLOSURE prepare generation=%u\n",i+1);
        uint64_t source=prepare(owner,record?source_history_record_input(i+1,false):source_history_function_input(i+1,false));
        XrXirSourceGenerationView view={0};CHECK(xr_xir_compile_source_generation_view(owner,source,&view)==XR_XIR_OK);
        if (record && i==1) history_record_coordinates(owner,&view);
        programs[i]=program(&view,i==0?"seed":i==3?"fail":"answer",&entries[i]);
        bind_program(&compiler,owner,source,programs[i]);
        uint64_t candidate=0;CHECK(xr_xir_source_generation_prepare_instance(owner,source,instance,&candidate)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entries[i],NULL,0)==XR_XIR_CALL_READY);
        XrXirCallStatus status=generation_poll(instance);
        fprintf(stderr,"BIRTH_CLOSURE generation=%u status=%u outputs=%u\n",i+1,status,outputs);
        CHECK(status==(i==3?XR_XIR_CALL_ASSERTION:XR_XIR_CALL_RETURNED));
        if(i==3) {
            CHECK(xr_xir_instance_generation_publish(instance,candidate)==XR_XIR_CALL_BAD_STATE);
            CHECK(xr_xir_instance_generation_abort(instance,candidate)==XR_XIR_CALL_READY);
            CHECK(xr_xir_compile_source_generation_abort(owner,source)==XR_XIR_OK);
        } else {
            XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
            CHECK(value.type==XR_XIR_I64 && value.payload==(i==0?0:i==1?42:43));xr_xir_value_drop(&value);
            CHECK(xr_xir_instance_generation_publish(instance,candidate)==XR_XIR_CALL_READY);
            CHECK(xr_xir_compile_source_generation_publish(owner,source)==XR_XIR_OK);
        }
    }
    if (record) {
        xr_xir_compile_source_generations_free(owner);owner=NULL;
        for (unsigned i=0;i<4;++i) {xr_xir_compile_program_drop(programs[i]);programs[i]=NULL;}
    }
    CHECK(xr_xir_instance_start(instance,entries[2],NULL,0)==XR_XIR_CALL_READY && generation_poll(instance)==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && result.payload==45);xr_xir_value_drop(&result);
    CHECK(outputs==1 && xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_compile_source_generations_free(owner);for(unsigned i=0;i<4;++i)xr_xir_compile_program_drop(programs[i]);
    source_fixture_owner_free(&compiler);return 0;
}

#endif // SOURCE_HISTORY_FUNCTION_VALUE_H
