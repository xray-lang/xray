/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_panics_execution.inc.c - Independent typed outcomes and effect boundaries
 */
static XrXirArtifact *panics_lower(XrXirSourceResult *source) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_checked_write(source->checked,NULL,&packet,NULL)==XR_XIR_OK);
    xr_xir_source_result_free(source);
    XrXirArtifact *read=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&read,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length);xr_xir_checked_packet_free(&packet);
    XrXirDiagnostic diagnostic={0};XrXirStatus status=xr_xir_specialize(read,NULL,&closed,&diagnostic);
    if (status!=XR_XIR_OK) {
        const XrXirModule *module=xr_xir_artifact_module(read);
        fprintf(stderr,"panics specialize=%u f=%u b=%u i=%u source-name=%.*s\n",status,
            diagnostic.function,diagnostic.block,diagnostic.instruction,
            diagnostic.function<module->function_count ? (int)module->functions[diagnostic.function].name_length : 0,
            diagnostic.function<module->function_count ? module->functions[diagnostic.function].name : "");
    }
    CHECK(status==XR_XIR_OK);xr_xir_artifact_free(read);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_artifact_free(closed);return lowered;
}
static void panics_execution(const char *directory,const char *path,const char *generated_path) {
    const char *source=
        "struct S{value:string}\n"
        "enum E{Bad{message:string}}\n"
        "final class C{value:string constructor(value:string){this.value=value}}\n"
        "export fn arrayNormal()->i64{try{assertPanics(fn()->Array<string>{return [\"owned\",\"array\"]})}catch panic(p){return p.code};return -1}\n"
        "export fn structNormal()->i64{try{assertPanics(fn()->S{return S{value:\"owned\"}})}catch panic(p){return p.code};return -1}\n"
        "export fn enumNormal()->i64{try{assertPanics(fn()->E{return E.Bad{message:\"owned\"}})}catch panic(p){return p.code};return -1}\n"
        "export fn classNormal()->i64{try{assertPanics(fn()->C{return C(\"owned\")})}catch panic(p){return p.code};return -1}\n"
        "export fn callableNormal()->i64{const owned=\"captured\";try{assertPanics(fn()->fn()->string{return fn()->string{return owned}})}catch panic(p){return p.code};return -1}\n"
        "export fn defaultEmpty()->i64{try{assertPanics(fn(){})}catch panic(p){return len(p.message)};return -1}\n"
        "export fn innerAssertion()->i64{assertPanics(fn(){assert(false,\"inner\")});return 11}\n"
        "export fn typedError()->i64{try{assertPanics(fn(){throw E.Bad{message:\"typed error\"}})}catch(E.Bad{message}){return len(message)}catch panic{return -1};return -2}\n"
        "export fn actionExpression()->i64{const factory=fn()->fn(){var zero=0;var ignored=1/zero;return fn(){}};try{assertPanics(factory())}catch panic(p){return p.code};return -1}\n"
        "export fn messageExpression()->i64{const message=fn()->string{var zero=0;var ignored=1/zero;return \"bad\"};try{assertPanics(fn(){},message())}catch panic(p){return p.code};return -1}\n"
        "export fn orderOnce()->i64{var state=0;const factory=fn()->fn(){state=state*10+1;return fn(){state=state*10+3}};const message=fn()->string{state=state*10+2;return \"order\"};try{assertPanics(factory(),message())}catch panic{return state};return -1}\n"
        "export fn cleanupOnce()->i64{var state=0;try{assertPanics(fn(){defer{state=state+1};state=state+2})}catch panic{return state};return -1}\n"
        "export fn suspendPanic()->i64{assertPanics(fn(){Coro.yield();var zero=0;var ignored=1/zero});return 19}\n"
        "export fn suspendNormal()->i64{try{assertPanics(fn()->string{Coro.yield();return \"owned\"})}catch panic(p){return p.code};return -1}\n";
    const char *names[]={"arrayNormal","structNormal","enumNormal","classNormal","callableNormal","defaultEmpty",
        "innerAssertion","typedError","actionExpression","messageExpression","orderOnce","cleanupOnce","suspendPanic","suspendNormal"};
    const int64_t expected[]={445,445,445,445,445,0,11,11,420,420,123,3,19,445};
    XrXirSourceResult result=panics_source(directory,path,source,true);XrXirArtifact *lowered=panics_lower(&result);
    uint32_t entries[14];for (uint32_t i=0;i<14;++i) entries[i]=panics_find(xr_xir_artifact_module(lowered),names[i]);
    XrXirCSource generated={0};CHECK(xr_xir_emit_c(lowered,"panics_matrix",16777216,&generated)==XR_XIR_OK);
    CHECK(!strstr(generated.text,"({"));
    char output_path[2048];CHECK(snprintf(output_path,sizeof(output_path),"%s.matrix.c",generated_path)>0);
    FILE *output=fopen(output_path,"wb");CHECK(output);
    CHECK(fwrite(generated.text,1,generated.length,output)==generated.length);
    CHECK(fputs("\nconst uint32_t panics_matrix_functions[14]={",output)>=0);
    for (uint32_t i=0;i<14;++i) CHECK(fprintf(output,"%s%uu",i ? "," : "",entries[i])>0);
    CHECK(fputs("};\n",output)>=0 && fclose(output)==0);xr_xir_c_source_free(&generated);
    XrXirProgram *program=NULL;CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){16777216,64000000},&program)==XR_XIR_OK);
    XrXirInstanceConfig config=xr_xir_instance_defaults();XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);xr_xir_program_drop(program);
    for (uint32_t i=0;i<14;++i) {
        CHECK(xr_xir_instance_start(instance,entries[i],NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult polled=xr_xir_instance_poll(instance);uint32_t suspended=0;
        while (polled.outcome.status==XR_XIR_CALL_SUSPENDED) {
            CHECK(++suspended==1 && i>=12);
            CHECK(xr_xir_instance_resume(instance,polled.epoch,polled.outcome.wake)==XR_XIR_CALL_READY);
            polled=xr_xir_instance_poll(instance);
        }
        CHECK(suspended==(uint32_t)(i>=12));
        if (polled.outcome.status!=XR_XIR_CALL_RETURNED) fprintf(stderr,"panics execution %s status=%u\n",names[i],polled.outcome.status);
        CHECK(polled.outcome.status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        if (value.type!=XR_XIR_I64 || (int64_t)value.payload!=expected[i]) fprintf(stderr,"panics execution %s value=%llu expected=%lld\n",
            names[i],(unsigned long long)value.payload,(long long)expected[i]);
        CHECK(value.type==XR_XIR_I64 && (int64_t)value.payload==expected[i]);xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);CHECK(!source_live && !source_bytes && !runtime_live && !runtime_bytes);
    puts("Array/struct/enum/class/callable result drops; typed Error; outer expressions; once/order/cleanup; suspend outcomes PASS");
}
