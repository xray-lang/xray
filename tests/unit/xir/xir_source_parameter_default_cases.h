/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_parameter_default_cases.h - Declaration-owned generic default purposes
 *
 * KEY CONCEPT:
 *   Original binders survive source destruction and one checked specialization.
 */
#ifndef XIR_SOURCE_PARAMETER_DEFAULT_CASES_H
#define XIR_SOURCE_PARAMETER_DEFAULT_CASES_H
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
static XrXirArtifact *source_parameter_defaults_check(XrXirSourceRequest *request, const char *text) {
    write_source(request->entry_path,text);
    XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_source_check(request,&result,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"parameter defaults %u %d:%d %s\n",status,
        diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
    XrXirArtifact *owned=result.checked;result.checked=NULL;
    const XrXirModule *module=xr_xir_artifact_module(owned);
    CHECK(module->defaults && module->defaults->count);
    for(uint32_t i=0;i<module->defaults->count;++i){
        const XrXirDefaultBinding *binding=&module->defaults->records[i];
        const XrXirFunctionIdentity *identity=&module->declarations->functions[binding->function];
        CHECK(!identity->exported && !identity->promises && !identity->cleanup_owner);
        CHECK(!module->functions[binding->function].parameter_count);
        CHECK(module->functions[binding->function].result==module->functions[binding->owner].parameters[binding->ordinal]);
        uint32_t owner=module->generics?module->generics[binding->owner].parameter_count:0;
        uint32_t helper=module->generics?module->generics[binding->function].parameter_count:0;
        CHECK(owner==helper);
    }
    xr_xir_source_result_free(&result);
    write_source(request->entry_path,"const poisoned=0\n");
    return owned;
}
static void source_parameter_defaults_execute(XrXirArtifact *owned) {
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_checked_write(owned,NULL,&packet,NULL)==XR_XIR_OK);
    xr_xir_artifact_free(owned);owned=NULL;
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&owned,NULL)==XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    XrXirArtifact *specialized=NULL,*lowered=NULL;
    CHECK(xr_xir_specialize(owned,NULL,&specialized,NULL)==XR_XIR_OK);
    xr_xir_artifact_free(owned);
    CHECK(!xr_xir_artifact_module(specialized)->defaults);
    CHECK(xr_xir_artifact_verify(specialized,NULL,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(specialized,&target,NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_artifact_free(specialized);
    const XrXirModule *module=xr_xir_artifact_module(lowered);uint32_t entry=UINT32_MAX;
    CHECK(!module->defaults);
    for(uint32_t f=0;f<module->function_count;++f){
        const XrXirFunction *function=&module->functions[f];
        if(function->name_length==6 && !memcmp(function->name,"answer",6))entry=f;
        for(uint32_t i=0;i<function->instruction_count;++i)CHECK(function->instructions[i].op!=XR_XIR_CALL_DEFAULT);
    }
    CHECK(entry!=UINT32_MAX);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
    for(uint32_t i=0;i<2;++i){
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);XrXirInstance *instance=NULL;
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll(instance).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64 && value.payload==41);xr_xir_value_drop(&value);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_program_drop(program);
}
static void source_parameter_default_cases(XrXirSourceRequest *request) {
    XrXirArtifact *owned=source_parameter_defaults_check(request,
        "fn free<T>(v:T,cb:fn(T)->T=fn(x:T)->T{return x})->T{return cb(v)}\n"
        "struct Box<T>{value:T;constructor(value,extra:i64=0){this.value=value};"
        "map<U>(v:U,cb:fn(T,U)->U=fn(t:T,u:U)->U{return u})->U{return cb(this.value,v)};"
        "static pass<U>(v:U,cb:fn(U)->U=fn(x:U)->U{return x})->U where T:Sendable,U:Sendable{return cb(v)}}\n"
        "export fn answer()->i64{const box=Box<string>(\"parent\");return Box<string>.pass<i64>(box.map<i64>(free<i64>(41)))}\n");
    source_parameter_defaults_execute(owned);
    owned=source_parameter_defaults_check(request,
        "interface Measure{measure(extra:i64)->i64}\n"
        "struct Meter implements Measure{value:i64;measure(extra:i64=0)->i64{return this.value+extra}}\n"
        "fn read<T:Measure>(value:T)->i64{return value.measure(0)}\n"
        "export fn answer()->i64{const m=Meter{value:41};return read<Meter>(m)+m.measure()-41}\n");
    source_parameter_defaults_execute(owned);
    owned=source_parameter_defaults_check(request,
        "fn value()->i64{Coro.yield();return 41}\n"
        "final class Box{value:i64;constructor(value:i64=value()){this.value=value}}\n"
        "export fn answer()->i64{return Box().value}\n");
    xr_xir_artifact_free(owned);
    write_source(request->entry_path,
        "fn phantom<T>(value:i64=41)->i64{return value}\nexport fn answer()->i64{return phantom()}\n");
    XrXirSourceResult rejected={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(xr_xir_source_check(request,&rejected,&diagnostic)==XR_XIR_BAD_TYPE);
    CHECK(!rejected.checked && strstr(diagnostic.message,"cannot infer all declaration type arguments"));
    xr_xir_source_result_free(&rejected);
    const char *denied[]={
        "interface Measure{measure(extra:i64)->i64}\nstruct Meter implements Measure{value:i64;measure(extra:i64=0)->i64{return this.value+extra}}\nfn unused<T:Measure>(value:T)->i64{return value.measure()}\n",
        "fn value(extra:i64=41)->i64{return extra}\nfn unused()->i64{const f=value;return f()}\n"
    };
    const char *reasons[]={"interface method argument arity mismatch","indirect call requires its declared signature"};
    for(uint32_t i=0;i<2;++i){
        write_source(request->entry_path,denied[i]);rejected=(XrXirSourceResult){0};diagnostic=(XrXirSourceDiagnostic){0};
        CHECK(xr_xir_source_check(request,&rejected,&diagnostic)==XR_XIR_BAD_TYPE);
        CHECK(!rejected.checked && strstr(diagnostic.message,reasons[i]));
        xr_xir_source_result_free(&rejected);
    }
}
#endif // XIR_SOURCE_PARAMETER_DEFAULT_CASES_H
