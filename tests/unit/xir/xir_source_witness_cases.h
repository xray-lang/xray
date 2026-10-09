/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_witness_cases.h - Real source witnesses through sealed execution
 */
#ifndef XIR_SOURCE_WITNESS_CASES_H
#define XIR_SOURCE_WITNESS_CASES_H
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_implementation.h"

static const char source_witness_program[] =
    "interface Measure { measure()->i64 }\n"
    "struct Meter implements Measure { value:i64; measure()->i64{return this.value} }\n"
    "fn read<T:Measure>(value:T)->i64{return value.measure()}\n"
    "export fn measured()->i64{return read<Meter>(Meter{value:41})}\n"
    "const initialized=measured()\n"
    "export fn initializedValue()->i64{return initialized}\n";

static XrXirArtifact *source_witness_checked(XrXirSourceRequest *request, const char *packet_path) {
    write_source(request->entry_path,source_witness_program);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(request->context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrXirSourceRequest local = *request; local.session = session;
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&local, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr,"source witness: %u %d:%d %s\n",status,
        diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    const XrXirModule *module = xr_xir_compile_artifact_module(result.checked);
    CHECK(module->declarations->implementations && module->declarations->implementations->count == 1);
    CHECK(module->declarations->implementations->records[0].binding_count == 1);
    uint32_t calls = 0;
    for (uint32_t f = 0; f < module->function_count; ++f)
        for (uint32_t i = 0; i < module->functions[f].instruction_count; ++i)
            if (module->functions[f].instructions[i].op == XR_XIR_CALL_REQUIREMENT) ++calls;
    CHECK(calls == 1);
    xr_compile_session_free(session);
    write_source(request->entry_path,"const overwritten=0\n");
    CHECK(xr_xir_compile_artifact_verify(result.checked, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    const XrXirCompileContext packet_context = *xr_xir_compile_artifact_context(result.checked);
    CHECK(xr_xir_compile_checked_write(result.checked, &packet, NULL) == XR_XIR_OK);
    if (packet_path) {
        FILE *file = fopen(packet_path,"wb"); CHECK(file);
        CHECK(fwrite(packet.bytes,1,packet.length,file) == packet.length && fclose(file) == 0);
    }
    xr_xir_compile_source_result_free(&result);
    XrXirArtifact *copy = NULL;
    CHECK(xr_xir_compile_checked_read(&packet_context, packet.bytes, packet.length, &copy, NULL) == XR_XIR_OK && copy);
    xr_xir_compile_checked_packet_free(&packet); return copy;
}
static void source_witness_forward_array(XrXirSourceRequest *request) {
    const char *program =
        "interface Measure { measure()->i64 }\n"
        "fn unused(values:Array<Box<Meter>>)->Array<Box<Meter>> { return values }\n"
        "struct Meter implements Measure { value:i64; measure()->i64{return this.value} }\n"
        "struct Box<T:Measure> implements Measure { value:T; measure()->i64{return this.value.measure()} }\n";
    write_source(request->entry_path,program);
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(request, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr,"forward Array witness: %u %s\n",status,diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked);
    xr_xir_compile_source_result_free(&result);
    write_source(request->entry_path,
        "interface Measure { measure()->i64 }\n"
        "fn unused(values:Array<Box<i64>>)->Array<Box<i64>> { return values }\n"
        "struct Box<T:Measure> implements Measure { value:T; measure()->i64{return this.value.measure()} }\n");
    CHECK(xr_xir_compile_source_check(request, &result, &diagnostic, NULL) == XR_XIR_BAD_TYPE && !result.checked && !result.snapshot);
    xr_xir_compile_source_result_free(&result);
}
static void source_witness_empty_helper_signatures(XrXirSourceRequest *request) {
    write_source(request->entry_path,
        "struct Secret { private n:i64=7; callback:fn()->i64=fn()->i64{return Secret{n:11,callback:fn()->i64{return 0}}.n} }\n"
        "fn noCapture()->i64 { defer { const ignored=1; }; const call=fn()->i64{return 41}; return call() }\n"
        "const instance=Secret()\n");
    XrXirSourceResult result = {0};
    XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(request, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK)
        fprintf(stderr, "empty helper source: %u at %d:%d %s\n",
            status, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked);
    const XrXirModule *module = xr_xir_compile_artifact_module(result.checked);
    uint32_t helpers = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        CHECK(!!function->parameters == !!function->parameter_count);
        if (module->declarations->functions[f].method_kind == XR_XIR_MEMBER_HELPER) ++helpers;
    }
    CHECK(helpers >= 3);
    xr_xir_compile_source_result_free(&result);
}
static void source_witness_rejections(XrXirSourceRequest *request) {
    const char *sources[] = {
        "interface I { get()->i64 }\nstruct S implements I { value:i64 }\n",
        "interface I { get()->i64 }\nstruct S implements I { get()->bool{return true} }\n",
        ("interface I { get()->i64 }\nstruct S { get()->i64{return 41} }\n"
            "fn use<T:I>(x:T)->i64{return x.get()}\nconst answer=use<S>(S{})\n"),
        "interface I { get()->i64 }\nstruct S implements I,I { get()->i64{return 41} }\n",
        "interface I { get()->i64 }\nstruct Box<T> implements I { value:T; get()->i64 where T:I{return this.value.get()} }\n",
        "interface I { get()->i64 }\nstruct Box<T:I> implements I { value:T; get()->i64{return this.value.get()} }\nconst bad=Box<i64>{value:41}\n",
        "interface I { get()->i64 }\nfn unused<T>(x:T)->i64{return x.get()}\n",
        "interface I { get(x:i64)->i64 }\nfn unused<T:I>(x:T)->i64{return x.get(true)}\n",
        "interface I { get()->i64 }\nstruct S implements I { static get()->i64{return 41} }\n"
    };
    for (uint32_t i = 0; i < sizeof(sources) / sizeof(*sources); ++i) {
        write_source(request->entry_path,sources[i]);
        XrXirSourceResult result = {0};
        CHECK(xr_xir_compile_source_check(request, &result, NULL, NULL) != XR_XIR_OK && !result.checked && !result.snapshot);
        xr_xir_compile_source_result_free(&result);
    }
}
static void source_witness_cases(XrXirSourceRequest *request) {
    XrXirArtifact *checked = source_witness_checked(request,NULL), *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK && closed);
    xr_xir_compile_artifact_free(checked);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK && lowered);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    uint32_t measured = UINT32_MAX, initialized = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (function->name_length == 8 && !memcmp(function->name,"measured",8)) measured = f;
        if (function->name_length == 16 && !memcmp(function->name,"initializedValue",16)) initialized = f;
        for (uint32_t i = 0; i < function->instruction_count; ++i)
            CHECK(function->instructions[i].op != XR_XIR_CALL_REQUIREMENT);
    }
    CHECK(measured != UINT32_MAX && initialized != UINT32_MAX);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    for (uint32_t run = 0; run < 2; ++run) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        XrXirInstance *instance = NULL;
        CHECK(xr_xir_instance_new(program,&config,&instance) == XR_XIR_CALL_READY);
        uint32_t entries[] = {initialized,measured};
        for (uint32_t call = 0; call < 2; ++call) {
            CHECK(xr_xir_instance_start(instance,entries[call],NULL,0) == XR_XIR_CALL_READY);
            XrXirCallResult outcome = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome;
            CHECK(outcome.status == XR_XIR_CALL_RETURNED && outcome.value.payload == 41);
        }
        CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    source_witness_rejections(request);
    source_witness_forward_array(request);
    source_witness_empty_helper_signatures(request);
}
#endif // XIR_SOURCE_WITNESS_CASES_H
