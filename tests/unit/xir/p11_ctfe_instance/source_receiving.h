/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_receiving.h - Actual generic results enter a fresh ordinary Source owner
 */
static void instance_source_receiving(const char *kind,unsigned ordinal) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *original=instance_source(&context,kind);
    uint32_t selected=instance_named(original,kind,ordinal);
    const XrXirOrigin *origin=&original->module.provenance->origins[selected];
    CHECK(origin->argument_count==1);XrXirType arguments[2]={origin->arguments[0],
        original->module.provenance->origins[instance_named(original,kind,1-ordinal)].arguments[0]};
    CHECK(arguments[0]!=arguments[1]);
    if (!strcmp(kind,"nested")) CHECK(instance_argument_absent(original,arguments[0]) && instance_argument_absent(original,arguments[1]));
    XirSourceInstanceSelection selections[2]={{origin->function,selected},
        {origin->function,instance_named(original,kind,1-ordinal)}};
    xr_xir_compile_artifact_free(original);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"instance-constant",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText text={NULL,instance_source_text(),strlen(instance_source_text())};
    XirSourceInstanceRequest receive={.source=&request,.text=&text,.selections=selections,.count=2,
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    XirSourceInstanceConstants constants={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=!strcmp(kind,"foreign")?instance_foreign_source(&request,NULL,&diagnostic,&constants,selections,2):
        xr_xir_compile_source_instance_constants(&receive,&constants,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"Source instance %s[%u] status=%u %d:%d %s\n",kind,ordinal,status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && constants.source.checked && constants.source.snapshot && constants.count==2);
    uint32_t provider=constants.functions[0],other_provider=constants.functions[1];
    CHECK(provider!=other_provider);
    CHECK(provider<constants.source.checked->module.function_count);
    CHECK(constants.source.checked->module.functions[provider].parameter_count==0 &&
        constants.source.checked->module.generics[provider].parameter_count==0);
    if (!strcmp(kind,"boxed")) {
        CHECK(!constants.source.checked->module.declarations->functions[provider].nominal_owner);
        CHECK(xr_xir_compile_construction_materialization_count(constants.source.checked->construction)==2);
        bool same=false;
        const XrXirConstruction *snapshot=xr_xir_compile_source_snapshot_construction(constants.source.snapshot);
        CHECK(xir_construction_compare(&context,constants.source.checked->module.types,
            constants.source.checked->construction,snapshot,&same)==XR_XIR_OK && same);
        XrXirMaterializationBinding *binding=&constants.source.checked->construction->materializations[0];
        uint32_t function=binding->function,replacement=0;
        while (replacement<constants.source.checked->module.function_count &&
            (replacement==function || replacement==binding->caller)) ++replacement;
        CHECK(replacement<constants.source.checked->module.function_count);binding->function=replacement;
        CHECK(xir_construction_shape(&context,constants.source.checked->module.types,
            constants.source.checked->construction)==XR_XIR_OK);
        same=true;
        CHECK(xir_construction_compare(&context,constants.source.checked->module.types,
            constants.source.checked->construction,snapshot,&same)==XR_XIR_OK && !same);
        CHECK(xr_xir_compile_artifact_verify(constants.source.checked,NULL)==XR_XIR_BAD_STRUCTURE);
        binding->function=function;CHECK(xr_xir_compile_artifact_verify(constants.source.checked,NULL)==XR_XIR_OK);
    }
    if (!strcmp(kind,"foreign")) {
        XrXirArtifact *actual=NULL;CHECK(xr_xir_compile_specialize(constants.source.checked,&actual,NULL)==XR_XIR_OK);
        instance_foreign_edges(actual);xr_xir_compile_artifact_free(actual);
    }
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(constants.source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_instance_constants_free(&constants);xr_compile_session_free(session);
    XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    uint32_t function=UINT32_MAX,other_function=UINT32_MAX;
    for (uint32_t f=0;f<closed->module.function_count;++f)
        if (closed->module.provenance->origins[f].function==provider) { CHECK(function==UINT32_MAX);function=f; }
        else if (closed->module.provenance->origins[f].function==other_provider) { CHECK(other_function==UINT32_MAX);other_function=f; }
    CHECK(function!=UINT32_MAX && other_function!=UINT32_MAX);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    uint32_t entry=named(&lowered->module,"run");
    XrXirCtfeCallRequest evaluation=import_request(lowered,"run",NULL);evaluation.evaluation.function=function;
    XrXirValue constant={0};CHECK(xr_xir_compile_ctfe_calls(&evaluation,&constant)==XR_XIR_CTFE_OK);
    evaluation.evaluation.function=other_function;XrXirValue other={0};
    CHECK(xr_xir_compile_ctfe_calls(&evaluation,&other)==XR_XIR_CTFE_OK);
    if (strcmp(kind,"scalar") && strcmp(kind,"foreign") && strcmp(kind,"unit") && strcmp(kind,"text")) CHECK(constant.type!=other.type);
    if (!strcmp(kind,"text")) {
        const char *bytes=NULL;size_t length=0;
        CHECK(xr_xir_string_view(&constant,&bytes,&length) && length==3 && !memcmp(bytes,"a\0b",3));
        CHECK(xr_xir_string_view(&other,&bytes,&length) && length==3 && !memcmp(bytes,"a\0b",3));
    }
    if (!strcmp(kind,"scalar") || !strcmp(kind,"foreign")) CHECK(constant.type==XR_XIR_I64 && constant.payload==42);
    if (!strcmp(kind,"array")) {
        XrXirValueAdmission admission={.arena=xr_xir_value_arena(&constant),.work=65536,.scratch_bytes=65536};
        int64_t length=-1;CHECK(xr_xir_array_len(&constant,&admission,&length)==XR_XIR_VALUE_OK && !length);
    }
    if (!strcmp(kind,"optional")) { bool some=true;const XrXirValue *payload=NULL;CHECK(xr_xir_nullable_view(&constant,&some,&payload) && !some); }
    if (!strcmp(kind,"unit")) CHECK(!constant.type && !constant.reserved && !constant.payload);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    xr_compile_resources_release(context.resources);
    for (unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED && xr_xir_value_valid(&value));
        CHECK(value.type==XR_XIR_I64 && value.payload==42);
        xr_xir_value_drop(&value);CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);CHECK(xr_xir_value_valid(&constant) && xr_xir_value_valid(&other));
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;
    xr_xir_value_drop(&constant);xr_xir_value_drop(&other);
    CHECK(source_fixture_compile_attempts==attempts);source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Source instance %s[%u] arguments=%u/%u actual fresh providers=%u/%u private entry denied two real Instances final physical0\n",
        kind,ordinal,arguments[0],arguments[1],provider,other_provider);
}
