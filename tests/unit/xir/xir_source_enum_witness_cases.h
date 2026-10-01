/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XIR_SOURCE_ENUM_WITNESS_CASES_H
#define XIR_SOURCE_ENUM_WITNESS_CASES_H
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"

static void source_enum_witness_run(XrXirSourceRequest *request, const char *source) {
    write_source(request->entry_path,source);
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_source_check(request,&result,&diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr,"enum witness: %u %d:%d %s\n",status,
        diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(result.checked,NULL,&packet,NULL) == XR_XIR_OK);
    xr_xir_source_result_free(&result);
    write_source(request->entry_path,"const replaced=0\n");
    XrXirArtifact *checked = NULL, *specialized = NULL, *lowered = NULL;
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&checked,NULL) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_specialize(checked,NULL,&specialized,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(specialized,&target,NULL,&lowered,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(specialized);
    const XrXirModule *module = xr_xir_artifact_module(lowered);
    uint32_t entry = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (function->name_length == 11 && !memcmp(function->name,"enumWitness",11)) entry = f;
        for (uint32_t i = 0; i < function->instruction_count; ++i)
            CHECK(function->instructions[i].op != XR_XIR_CALL_REQUIREMENT);
    }
    CHECK(entry != UINT32_MAX);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){33554432,64000000},&program) == XR_XIR_OK);
    for (uint32_t run = 0; run < 2; ++run) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); XrXirInstance *instance = NULL;
        CHECK(xr_xir_instance_new(program,&config,&instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entry,NULL,0) == XR_XIR_CALL_READY);
        XrXirCallResult outcome = xr_xir_instance_poll(instance).outcome;
        CHECK(outcome.status == XR_XIR_CALL_RETURNED && outcome.value.payload == 41);
        CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    }
    xr_xir_program_drop(program);
}
static void source_enum_witness_rejections(XrXirSourceRequest *request) {
    const char *rejected[] = {
        "enum E { V name()->i64{return 41} }\n",
        "enum E { V ordinal()->i64{return 41} }\n",
        "enum E { V toString()->string{return \"custom\"} }\n",
        "enum E { variants }\n",
        "enum E { V static variants()->i64{return 41} }\n",
        "enum E { V V()->i64{return 41} }\n",
        "enum E { V static V()->i64{return 41} }\n",
        "enum E { V f()->i64{return 41} static f()->i64{return 41} }\n",
        "enum E { V constructor(){} }\n",
        "enum E { V ref f()->i64{return 41} }\n",
        "enum E { V move f()->i64{return 41} }\n",
        "enum E { V static f()->E{return this} }\n",
        "enum E { V { value:i64 } f()->i64{return this.value} }\n",
        "interface Measure{measure()->i64}\nenum E implements Measure { V }\n",
        "interface Measure{measure()->i64}\nenum E implements Measure { V static measure()->i64{return 41} }\n",
        "interface Measure{measure()->i64}\nenum E<T> implements Measure { V { value:T } measure()->i64 where T:Measure{return 41} }\n",
        "interface Text{toString()->string}\nenum E implements Text { V }\n",
        "enum E { V f()->i64{return 41} }\nconst bad=E.f()\n",
        "enum E { V static f()->i64{return 41} }\nconst bad=E.V.f()\n"
    };
    for (uint32_t i = 0; i < sizeof(rejected)/sizeof(*rejected); ++i) {
        write_source(request->entry_path,rejected[i]); XrXirSourceResult result = {0};
        CHECK(xr_xir_source_check(request,&result,NULL) != XR_XIR_OK && !result.checked);
        xr_xir_source_result_free(&result);
    }
}
static void source_enum_witness_cases(XrXirSourceRequest *request) {
    source_enum_witness_run(request,
        "interface Measure { measure()->i64 }\n"
        "enum Reading implements Measure { Value { value:i64 }, Empty "
            "measure()->i64{return match(this){Reading.Value {value}->value,Reading.Empty->0}} "
            "static fromValue(value:i64)->Reading{return Reading.Value {value:value}} }\n"
        "fn read<T:Measure>(value:T)->i64{return value.measure()}\n"
        "export fn enumWitness()->i64{return read<Reading>(Reading.fromValue(41))}\n");
    source_enum_witness_run(request,
        "interface Measure { measure()->i64 }\n"
        "struct Meter implements Measure { value:i64; measure()->i64{return this.value} }\n"
        "enum Reading<T:Measure> implements Measure { Value { value:T }, Empty "
            "measure()->i64{return match(this){Reading.Value {value}->value.measure(),Reading.Empty->0}} "
            "static fromValue(value:T)->Reading<T>{return Reading<T>.Value {value:value}} }\n"
        "fn read<T:Measure>(value:T)->i64{return value.measure()}\n"
        "export fn enumWitness()->i64{return read<Reading<Meter>>(Reading<Meter>.fromValue(Meter{value:41}))}\n");
    source_enum_witness_run(request,
        "enum Names { name, ordinal, toString }\n"
        "export fn enumWitness()->i64{return Names.name.ordinal+Names.ordinal.ordinal+Names.toString.ordinal+38}\n");
    source_enum_witness_run(request,
        "enum Names { Value "
            "static name()->i64{return 41} static ordinal()->i64{return 41} "
            "static toString()->i64{return 41} variants()->i64{return 41} }\n"
        "export fn enumWitness()->i64{if(Names.name()!=41 || Names.ordinal()!=41 || Names.toString()!=41){return 0};return Names.Value.variants()}\n");
    source_enum_witness_run(request,
        "interface Measure { measure()->i64 }\n"
        "struct Meter implements Measure { value:i64; measure()->i64{return this.value} }\n"
        "enum Holder<T> { Value { value:T } "
            "get<U>(value:U)->U where T:Measure{return value} "
            "static make(value:T)->Holder<T>{return Holder<T>.Value {value:value}} }\n"
        "export fn enumWitness()->i64{const h=Holder<Meter>.make(Meter{value:41});const bound=h.get<i64>;return bound(41)}\n");
    source_enum_witness_run(request,
        "interface Measure { measure()->i64 }\ninterface Derived extends Measure {}\n"
        "enum Reading implements Derived { Value { value:i64 } "
            "measure()->i64{return match(this){Reading.Value {value}->value}} }\n"
        "fn read<T:Measure>(value:T)->i64{return value.measure()}\n"
        "export fn enumWitness()->i64{return read<Reading>(Reading.Value {value:41})}\n");
    source_enum_witness_rejections(request);
}
#endif // XIR_SOURCE_ENUM_WITNESS_CASES_H
