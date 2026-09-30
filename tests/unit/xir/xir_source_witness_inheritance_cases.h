/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_witness_inheritance_cases.h - Source inheritance through owned packets
 *
 * KEY CONCEPT:
 *   Original application obligations survive source destruction and select the
 *   same checked implementation independently of inheritance traversal order.
 */
#ifndef XIR_SOURCE_WITNESS_INHERITANCE_CASES_H
#define XIR_SOURCE_WITNESS_INHERITANCE_CASES_H
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_implementation.h"

typedef struct SourceWitnessInheritanceCase {
    const char *name, *source;
    uint32_t records, bindings;
    bool multiple_applications;
} SourceWitnessInheritanceCase;

static const SourceWitnessInheritanceCase source_witness_inheritance_programs[] = {
    {"multilevel argument permutation",
        "interface Base<A,B> { measure(first:A,second:B)->i64 }\n"
        "interface Middle<X,Y> extends Base<Y,X> {}\n"
        "interface Leaf<P,Q> extends Middle<Q,P> {}\n"
        "struct Meter implements Leaf<i64,string> { measure(first:i64,second:string)->i64{return first} }\n"
        "fn read<T:Leaf<A,B>,A,B>(value:T,first:A,second:B)->i64{return value.measure(first,second)}\n"
        "export fn measured()->i64{return read<Meter,i64,string>(Meter{},41,\"proof\")}\n",
        1,1,false},
    {"diamond left first",
        "interface Base<A> { measure(value:A)->i64 }\n"
        "interface Left<T> extends Base<T> {}\ninterface Right<U> extends Base<U> {}\n"
        "interface Diamond<V> extends Left<V>,Right<V> {}\n"
        "struct Meter implements Diamond<i64> { measure(value:i64)->i64{return value} }\n"
        "fn read<T:Diamond<i64>>(value:T)->i64{return value.measure(41)}\n"
        "export fn measured()->i64{return read<Meter>(Meter{})}\n",1,1,false},
    {"diamond right first",
        "interface Base<A> { measure(value:A)->i64 }\n"
        "interface Left<T> extends Base<T> {}\ninterface Right<U> extends Base<U> {}\n"
        "interface Diamond<V> extends Right<V>,Left<V> {}\n"
        "struct Meter implements Diamond<i64> { measure(value:i64)->i64{return value} }\n"
        "fn read<T:Diamond<i64>>(value:T)->i64{return value.measure(41)}\n"
        "export fn measured()->i64{return read<Meter>(Meter{})}\n",1,1,false},
    {"different origins left first",
        "interface Left { measure(first:i64)->i64 }\ninterface Right { measure(second:i64)->i64 }\n"
        "interface Both extends Left,Right {}\n"
        "struct Meter implements Both { measure(actual:i64)->i64{return actual} }\n"
        "fn read<T:Both>(value:T)->i64{return value.measure(41)}\n"
        "export fn measured()->i64{return read<Meter>(Meter{})}\n",1,2,false},
    {"different origins right first",
        "interface Left { measure(first:i64)->i64 }\ninterface Right { measure(second:i64)->i64 }\n"
        "interface Both extends Right,Left {}\n"
        "struct Meter implements Both { measure(actual:i64)->i64{return actual} }\n"
        "fn read<T:Both>(value:T)->i64{return value.measure(41)}\n"
        "export fn measured()->i64{return read<Meter>(Meter{})}\n",1,2,false},
    {"same origin different applications",
        "interface Measure<T> { measure()->i64 }\n"
        "struct Meter implements Measure<i64>,Measure<string> { measure()->i64{return 41} }\n"
        "fn read<T:Measure<i64>&Measure<string>>(value:T)->i64{return value.measure()}\n"
        "export fn measured()->i64{return read<Meter>(Meter{})}\n",2,2,true},
    {"same origin reversed applications",
        "interface Measure<T> { measure()->i64 }\n"
        "struct Meter implements Measure<string>,Measure<i64> { measure()->i64{return 41} }\n"
        "fn read<T:Measure<string>&Measure<i64>>(value:T)->i64{return value.measure()}\n"
        "export fn measured()->i64{return read<Meter>(Meter{})}\n",2,2,true}
};

static void source_witness_inheritance_obligations(const XrXirModule *module,
    const SourceWitnessInheritanceCase *test) {
    CHECK(module && module->declarations && module->declarations->implementations);
    const XrXirImplementationTable *table = module->declarations->implementations;
    CHECK(table->count == test->records);
    uint32_t bindings = 0, function = UINT32_MAX;
    for (uint32_t r = 0; r < table->count; ++r) {
        const XrXirImplementation *record = &table->records[r];
        bindings += record->binding_count;
        for (uint32_t b = 0; b < record->binding_count; ++b) {
            const XrXirImplementationBinding *binding = &record->bindings[b];
            if (function == UINT32_MAX) function = binding->function;
            CHECK(binding->function == function);
            CHECK(binding->requirement.declaration < module->types->interfaces->count);
            const XrXirInterfaceDeclaration *origin =
                &module->types->interfaces->declarations[binding->requirement.declaration];
            CHECK(binding->member < origin->method_count);
            CHECK(origin->methods[binding->member].name.length == 7 &&
                !memcmp(origin->methods[binding->member].name.bytes,"measure",7));
        }
    }
    CHECK(bindings == test->bindings);
    if (test->records == 1 && test->bindings == 2) {
        const XrXirImplementationBinding *a = &table->records[0].bindings[0];
        const XrXirImplementationBinding *b = &table->records[0].bindings[1];
        CHECK(a->requirement.declaration != b->requirement.declaration);
    }
    if (test->multiple_applications) {
        const XrXirImplementationBinding *a = &table->records[0].bindings[0];
        const XrXirImplementationBinding *b = &table->records[1].bindings[0];
        CHECK(a->requirement.declaration == b->requirement.declaration && a->member == b->member);
        CHECK(a->requirement.argument_count == 1 && b->requirement.argument_count == 1);
        XrXirType x = a->requirement.arguments[0], y = b->requirement.arguments[0];
        CHECK((x == XR_XIR_I64 && y == XR_XIR_STRING) || (x == XR_XIR_STRING && y == XR_XIR_I64));
    }
}

static XrXirArtifact *source_witness_inheritance_packet(XrXirSourceRequest *request,
    const SourceWitnessInheritanceCase *test) {
    write_source(request->entry_path,test->source);
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrXirSourceRequest local = *request; local.session = session;
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_source_check(&local,&result,&diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr,"inheritance %s: %u %d:%d %s\n",test->name,status,
        diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    xr_compiler_session_delete(session);
    write_source(request->entry_path,"const overwritten=0\n");
    source_witness_inheritance_obligations(xr_xir_artifact_module(result.checked),test);
    CHECK(xr_xir_artifact_verify(result.checked,NULL,NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(result.checked,NULL,&packet,NULL) == XR_XIR_OK);
    xr_xir_source_result_free(&result);
    XrXirArtifact *copy = NULL;
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&copy,NULL) == XR_XIR_OK && copy);
    xr_xir_checked_packet_free(&packet);
    source_witness_inheritance_obligations(xr_xir_artifact_module(copy),test);
    return copy;
}

static void source_witness_inheritance_execute(XrXirArtifact *checked) {
    XrXirArtifact *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_specialize(checked,NULL,&closed,NULL) == XR_XIR_OK && closed);
    xr_xir_artifact_free(checked);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed,&target,NULL,&lowered,NULL) == XR_XIR_OK && lowered);
    xr_xir_artifact_free(closed);
    const XrXirModule *module = xr_xir_artifact_module(lowered);
    uint32_t measured = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (function->name_length == 8 && !memcmp(function->name,"measured",8)) measured = f;
        for (uint32_t i = 0; i < function->instruction_count; ++i)
            CHECK(function->instructions[i].op != XR_XIR_CALL_REQUIREMENT);
    }
    CHECK(measured != UINT32_MAX);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){33554432,64000000},&program) == XR_XIR_OK && !lowered);
    for (uint32_t run = 0; run < 2; ++run) {
        XrXirInstanceConfig config = xr_xir_instance_defaults(); XrXirInstance *instance = NULL;
        CHECK(xr_xir_instance_new(program,&config,&instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,measured,NULL,0) == XR_XIR_CALL_READY);
        XrXirCallResult outcome = xr_xir_instance_poll(instance).outcome;
        CHECK(outcome.status == XR_XIR_CALL_RETURNED && outcome.value.payload == 41);
        CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    }
    xr_xir_program_drop(program);
}

static void source_witness_inheritance_rejections(XrXirSourceRequest *request) {
    const char *sources[] = {
        "interface Base<A,B> { measure(first:A,second:B)->i64 }\n"
            "interface Middle<X,Y> extends Base<Y,X> {}\ninterface Leaf<P,Q> extends Middle<Q,P> {}\n"
            "struct Meter implements Leaf<i64,string> { measure(first:string,second:i64)->i64{return second} }\n",
        "interface Base<A> { measure(value:A)->i64 }\n"
            "interface Left extends Base<i64> {}\ninterface Right extends Base<string> {}\n"
            "interface Unused extends Left,Right {}\n",
        "interface Left { measure(value:i64)->i64 }\ninterface Right { measure(value:i64)->bool }\n"
            "interface Unused extends Left,Right {}\n",
        "interface Left { measure(value:i64)->i64 }\ninterface Right { measure(value:i64)->bool }\n"
            "interface Unused extends Right,Left {}\n",
        "interface Left { measure(callback:fn()->i64)->i64 }\n"
            "interface Right { measure(callback:fn()->bool)->i64 }\ninterface Unused extends Left,Right {}\n",
        "interface Measure<T> { measure(value:T)->i64 }\n"
            "struct Meter implements Measure<i64>,Measure<string> { measure(value:i64)->i64{return value} }\n",
        "interface Measure<T> { measure(value:T)->i64 }\n"
            "fn unused<T:Measure<i64>&Measure<string>>(value:T)->i64{return 41}\n",
        "interface Base<T> { measure(value:T)->i64 }\ninterface Child<T> extends Base<T> {}\n"
            "fn unused<T:Child<i64>>(value:T)->i64{return value.measure(\"wrong\")}\n"
    };
    for (uint32_t i = 0; i < sizeof(sources)/sizeof(*sources); ++i) {
        write_source(request->entry_path,sources[i]);
        XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
        XrXirSourceRequest local = *request; local.session = session;
        XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
        XrXirStatus status = xr_xir_source_check(&local,&result,&diagnostic);
        if (status != XR_XIR_BAD_TYPE) fprintf(stderr,"inheritance negative %u: %u %s\n",i,status,diagnostic.message);
        CHECK(status == XR_XIR_BAD_TYPE && !result.checked);
        xr_xir_source_result_free(&result); xr_compiler_session_delete(session);
    }
}

static void source_witness_inheritance_cases(XrXirSourceRequest *request) {
    for (uint32_t i = 0; i < sizeof(source_witness_inheritance_programs)/sizeof(*source_witness_inheritance_programs); ++i)
        source_witness_inheritance_execute(source_witness_inheritance_packet(request,&source_witness_inheritance_programs[i]));
    source_witness_inheritance_rejections(request);
}
#endif // XIR_SOURCE_WITNESS_INHERITANCE_CASES_H
