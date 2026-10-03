/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_witness_promises.h - Source witness contracts across effect boundaries
 *
 * KEY CONCEPT:
 *   Abstract calls use declared interface promises at definition checking time.
 *   Packet ownership and both execution backends preserve that same contract.
 */
#ifndef XIR_SOURCE_WITNESS_PROMISES_H
#define XIR_SOURCE_WITNESS_PROMISES_H

static void witness_promise_file(const XrXirSourceRequest *request,
    const char *name, const char *text) {
    char path[8192]; source_fixture_path(request,name,path);
    FILE *file = fopen(path,"wb"); CHECK(file);
    CHECK(fputs(text,file) >= 0 && fclose(file) == 0);
}

static XrXirArtifact *witness_promise_check(const XrXirSourceRequest *request,
    bool accepted, const char *label, const char *reason) {
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrXirSourceRequest local = *request; local.session = session;
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_source_check(&local,&result,&diagnostic);
    if (status != (accepted ? XR_XIR_OK : XR_XIR_BAD_TYPE))
        fprintf(stderr,"witness promise %s: %u %d:%d %s\n",label,status,
            diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status == (accepted ? XR_XIR_OK : XR_XIR_BAD_TYPE));
    CHECK((result.checked != NULL) == accepted);
    if (!accepted) {
        if (!strstr(diagnostic.message,reason))
            fprintf(stderr,"witness rejection %s: expected %s, got %s\n",label,reason,diagnostic.message);
        CHECK(strstr(diagnostic.message,reason));
        xr_xir_source_result_free(&result); xr_compiler_session_delete(session);
        return NULL;
    }
    CHECK(result.snapshot);
    xr_compiler_session_delete(session);
    CHECK(xr_xir_artifact_verify(result.checked,NULL,NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0}; XrXirArtifact *copy = NULL;
    CHECK(xr_xir_checked_write(result.checked,NULL,&packet,NULL) == XR_XIR_OK);
    xr_xir_source_result_free(&result);
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&copy,NULL) == XR_XIR_OK);
    memset(packet.bytes,0xCC,packet.length); xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_artifact_verify(copy,NULL,NULL) == XR_XIR_OK);
    return copy;
}

static void witness_promise_execute(XrXirArtifact *checked,
    const char *prefix, const char *output) {
    XrXirArtifact *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_specialize(checked,NULL,&closed,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed,&target,NULL,&lowered,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    const XrXirModule *module = xr_xir_artifact_module(lowered);
    uint32_t entry = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (function->name_length == 6 && !memcmp(function->name,"answer",6)) entry = f;
        for (uint32_t i = 0; i < function->instruction_count; ++i)
            CHECK(function->instructions[i].op != XR_XIR_CALL_REQUIREMENT);
    }
    CHECK(entry != UINT32_MAX);
    if (output) {
        XrXirCSource source = {0};
        CHECK(xr_xir_emit_c(lowered,prefix,4194304,&source) == XR_XIR_OK);
        CHECK(!strstr(source.text,"({") && !strstr(source.text,"xr_xir_vm"));
        FILE *file = fopen(output,"ab"); CHECK(file);
        CHECK(fwrite(source.text,1,source.length,file) == source.length);
        CHECK(fprintf(file,"\nconst uint32_t %s_selected_entry = %uu;\n",prefix,entry) > 0);
        CHECK(fclose(file) == 0); xr_xir_c_source_free(&source);
    }
    XrXirProgram *program = NULL;
    CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){33554432,64000000},
        &program) == XR_XIR_OK && !lowered);
    for (uint32_t run = 0; run < 2; ++run) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); XrXirInstance *instance = NULL;
        CHECK(xr_xir_instance_new(program,&config,&instance) == XR_XIR_CALL_READY);
        XrXirCallStatus started = xr_xir_instance_start(instance,entry,NULL,0);
        if (started != XR_XIR_CALL_READY)
            fprintf(stderr,"witness execute %s run=%u entry=%u start=%u\n",prefix,run,entry,started);
        CHECK(started == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        XrXirValue result = {0};
        CHECK(xr_xir_instance_take_result(instance,&result) == XR_XIR_CALL_RETURNED);
        CHECK(result.type == XR_XIR_I64 && result.payload == 41); xr_xir_value_drop(&result);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    }
    xr_xir_program_drop(program);
}

static void witness_promise_direct(const XrXirSourceRequest *request, const char *output) {
    witness_promise_file(request,"witness_promises.xr",
        "interface Measure { get()->i64 }\n"
        "struct Meter implements Measure { value:i64; get()->i64{return this.value} }\n"
        "struct Box<T:Measure> implements Measure { value:T; get()->i64{return this.value.get()} }\n"
        "fn read<T:Measure>(value:T)->i64{return value.get()}\n"
        "export fn answer()->i64{return read<Box<Meter>>(Box<Meter>{value:Meter{value:41}})}\n");
    SourceTestDeclaration records[] = {{"get","Measure",NULL,true},
        {"read",NULL,NULL,true},{"get","Meter",NULL,true},{"get","Box",NULL,true}};
    source_manifest_write(request,"witness_promises.xr",records,2);
    witness_promise_execute(witness_promise_check(request,true,"generic implementation",NULL),
        "witness_direct",output);
    source_manifest_write(request,"witness_promises.xr",records + 1,3);
    CHECK(!witness_promise_check(request,false,"unknown abstract callee","declared no_suspend"));
    source_manifest_write(request,"witness_promises.xr",records,4);
    witness_promise_file(request,"witness_promises.xr",
        "interface Measure { get()->i64 }\n"
        "struct Meter implements Measure { value:i64; get()->i64{return this.value} }\n"
        "struct Box<T> implements Measure { value:T; get()->i64 where T:Measure{return this.value.get()} }\n"
        "fn read<T:Measure>(value:T)->i64{return value.get()}\n");
    CHECK(!witness_promise_check(request,false,"implementation strengthens obligations",
        "implementation witness definition obligations failed"));
}

static void witness_promise_callback(const XrXirSourceRequest *request, const char *output) {
    const char *program =
        "interface Apply { apply(f:fn()->i64)->i64 }\n"
        "struct Runner implements Apply { apply(f:fn()->i64)->i64{return f()} }\n"
        "fn forward<T:Apply>(value:T,f:fn()->i64)->i64{return value.apply(f)}\n"
        "fn pure()->i64{return 41}\n"
        "export fn answer()->i64{return forward<Runner>(Runner{},pure)}\n";
    witness_promise_file(request,"witness_promises.xr",program);
    SourceTestDeclaration records[] = {{"apply","Apply","f",true},
        {"apply","Runner","f",true},{"forward",NULL,"f",true},{"pure",NULL,NULL,true}};
    SourceTestDeclaration derived[] = {records[0],records[2],records[3]};
    source_manifest_write(request,"witness_promises.xr",derived,3);
    witness_promise_execute(witness_promise_check(request,true,"qualified callback",NULL),
        "witness_callback",output);
    records[2].parameter = NULL;
    source_manifest_write(request,"witness_promises.xr",records,4);
    CHECK(!witness_promise_check(request,false,"unqualified forwarding parameter",
        "callable conversion may only discard its top-level promise"));
    records[2].parameter = "f"; records[0].parameter = NULL;
    source_manifest_write(request,"witness_promises.xr",records,4);
    CHECK(!witness_promise_check(request,false,"implementation strengthens callback contract",
        "implementation witness definition obligations failed"));
    records[0].parameter = "f";
    source_manifest_write(request,"witness_promises.xr",records,3);
    witness_promise_file(request,"witness_promises.xr",
        "interface Apply { apply(f:fn()->i64)->i64 }\n"
        "struct Runner implements Apply { apply(f:fn()->i64)->i64{return f()} }\n"
        "fn forward<T:Apply>(value:T,f:fn()->i64)->i64{return value.apply(f)}\n"
        "fn pure()->i64{Coro.yield();return 41}\n"
        "export fn answer()->i64{return forward<Runner>(Runner{},pure)}\n");
    CHECK(!witness_promise_check(request,false,"suspending callback","explicit target promise"));
    source_manifest_write(request,"witness_promises.xr",derived,3);
    witness_promise_file(request,"witness_promises.xr",
        "interface Apply { apply(f:fn()->i64)->i64 }\n"
        "struct Runner implements Apply { apply(f:fn()->i64)->i64{Coro.yield();return f()} }\n"
        "fn forward<T:Apply>(value:T,f:fn()->i64)->i64{return value.apply(f)}\n"
        "fn pure()->i64{return 41}\n"
        "export fn answer()->i64{return forward<Runner>(Runner{},pure)}\n");
    CHECK(!witness_promise_check(request,false,"inherited implementation effect","declared no_suspend"));
}

static void witness_promise_inherited(const XrXirSourceRequest *request, const char *output) {
    witness_promise_file(request,"witness_promises.xr",
        "interface Left { apply(f:fn()->i64)->i64 }\n"
        "interface Right { apply(f:fn()->i64)->i64 }\n"
        "interface Both extends Left,Right {}\n"
        "struct Runner implements Both { apply(f:fn()->i64)->i64{return f()} }\n"
        "fn forward<T:Both>(value:T,f:fn()->i64)->i64{return value.apply(f)}\n"
        "fn pure()->i64{return 41}\n"
        "export fn answer()->i64{return forward<Runner>(Runner{},pure)}\n");
    SourceTestDeclaration records[] = {{"apply","Left","f",true},
        {"apply","Right","f",true},{"apply","Runner","f",true},
        {"forward",NULL,"f",true},{"pure",NULL,NULL,true}};
    SourceTestDeclaration derived[] = {records[0],records[1],records[3],records[4]};
    source_manifest_write(request,"witness_promises.xr",derived,4);
    witness_promise_execute(witness_promise_check(request,true,"identical inherited contracts",NULL),
        "witness_inherited",output);
    derived[1].no_suspend = false;
    source_manifest_write(request,"witness_promises.xr",derived,4);
    CHECK(!witness_promise_check(request,false,"inherited suspension conflict",
        "source declaration type structure is invalid"));
    derived[1].no_suspend = true; derived[1].parameter = NULL;
    source_manifest_write(request,"witness_promises.xr",derived,4);
    CHECK(!witness_promise_check(request,false,"inherited callback conflict",
        "source declaration type structure is invalid"));
}

static void witness_promise_cross_manifest(const XrXirSourceRequest *request, bool correct) {
    char text[2048];
    int count = snprintf(text,sizeof(text),
        "[declarations]\nversion=1\n"
        "[[declarations.function]]\nmodule=\"%s\"\nowner=\"Measure\"\nname=\"get\"\nno_suspend=true\n"
        "[[declarations.function]]\nmodule=\"witness_promises.xr\"\nowner=\"Meter\"\nname=\"get\"\nno_suspend=true\n"
        "[[declarations.function]]\nmodule=\"witness_promises.xr\"\nname=\"read\"\nno_suspend=true\n",
        correct ? "witness_contract.xr" : "witness_decoy.xr");
    CHECK(count > 0 && count < (int)sizeof(text)); source_manifest_raw(request,text);
}

static void witness_promise_cross(const XrXirSourceRequest *request, const char *output) {
    witness_promise_file(request,"witness_contract.xr","export interface Measure { get()->i64 }\n");
    witness_promise_file(request,"witness_decoy.xr",
        "export interface Measure { get()->i64 }\nexport fn token()->i64{return 0}\n");
    witness_promise_file(request,"witness_promises.xr",
        "import { Measure } from \"./witness_contract\"\n"
        "import { token } from \"./witness_decoy\"\n"
        "struct Meter implements Measure { get()->i64{return 41} }\n"
        "fn read<T:Measure>(value:T)->i64{return value.get()}\n"
        "export fn answer()->i64{return read<Meter>(Meter{})+token()}\n");
    witness_promise_cross_manifest(request,true);
    witness_promise_execute(witness_promise_check(request,true,"canonical module promise",NULL),
        "witness_cross",output);
    witness_promise_cross_manifest(request,false);
    CHECK(!witness_promise_check(request,false,"same name in another module","declared no_suspend"));
}

static void source_witness_promise_cases(const XrXirSourceRequest *request, const char *output) {
    char path[8192]; source_fixture_path(request,"witness_promises.xr",path);
    XrXirSourceRequest probe = *request; probe.entry_path = path;
    witness_promise_direct(&probe,output);
    witness_promise_callback(&probe,output);
    witness_promise_inherited(&probe,output);
    witness_promise_cross(&probe,output);
    source_manifest_raw(request,"");
    const char *names[] = {"witness_promises.xr","witness_contract.xr","witness_decoy.xr"};
    for (uint32_t i = 0; i < 3; ++i) {
        source_fixture_path(request,names[i],path); CHECK(xr_test_unlink(path) == 0);
    }
}
#endif // XIR_SOURCE_WITNESS_PROMISES_H
