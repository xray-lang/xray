/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XIR_SOURCE_ENUM_WITNESS_CASES_H
#define XIR_SOURCE_ENUM_WITNESS_CASES_H
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"

static void source_enum_witness_run(XrXirSourceRequest *request, const char *source) {
    write_source(request->entry_path,source);
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(request, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr,"enum witness: %u %d:%d %s\n",status,
        diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    XrXirCheckedPacket packet = {0};
    const XrXirCompileContext packet_context = *xr_xir_compile_artifact_context(result.checked);
    CHECK(xr_xir_compile_checked_write(result.checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);
    write_source(request->entry_path,"const replaced=0\n");
    XrXirArtifact *checked = NULL, *specialized = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&packet_context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked, &specialized, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(specialized, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(specialized);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    uint32_t entry = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (function->name_length == 11 && !memcmp(function->name,"enumWitness",11)) entry = f;
        for (uint32_t i = 0; i < function->instruction_count; ++i)
            CHECK(function->instructions[i].op != XR_XIR_CALL_REQUIREMENT);
    }
    CHECK(entry != UINT32_MAX);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK);
    for (uint32_t run = 0; run < 2; ++run) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); XrXirInstance *instance = NULL;
        CHECK(xr_xir_instance_new(program,&config,&instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entry,NULL,0) == XR_XIR_CALL_READY);
        XrXirCallResult outcome = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome;
        CHECK(outcome.status == XR_XIR_CALL_RETURNED && outcome.value.payload == 41);
        CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
}
static void source_enum_ref_declaration_facts(const XrXirSourceResult *result) {
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result->snapshot);
    CHECK(view && view->complete && view->diagnostic.status == XR_XIR_OK);
    const XrXirSourceDeclaration *owner = declaration(view, "E", 0);
    CHECK(owner && owner->type.known && !owner->native_identity);
    const XrXirTypeNode *type = xr_xir_type_node(view->types, owner->type.type);
    CHECK(type && type->kind == XR_XIR_TYPE_NOMINAL && view->types->nominals &&
        type->nominal.declaration < view->types->nominals->count);
    CHECK(view->types->nominals->declarations[type->nominal.declaration].kind == XR_XIR_NOMINAL_ENUM);
    const XrXirSourceDeclaration *method = declaration(view, "f", owner->id);
    CHECK(method && method->kind == XR_XIR_SOURCE_FUNCTION && !method->native_identity);
    const XrXirSourceDeclaration *receiver = declaration(view, "this", method->id);
    CHECK(receiver && receiver->kind == XR_XIR_SOURCE_PARAMETER && receiver->mutable &&
        receiver->type.known && receiver->type.type == owner->type.type);
    const XrXirModule *module = xr_xir_compile_artifact_module(result->checked);
    CHECK(module && module->declarations && module->declarations->functions);
    uint32_t matches = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        const XrXirFunctionIdentity *identity = &module->declarations->functions[f];
        if (function->name_length != 1 || function->name[0] != 'f' ||
            identity->nominal_owner != type->nominal.declaration + 1) continue;
        ++matches;
        CHECK(identity->method_kind == XR_XIR_READ_METHOD && identity->member_access == XR_XIR_MEMBER_PUBLIC);
        CHECK(function->parameter_count == 1 && function->parameters && function->result == XR_XIR_I64);
        CHECK(xr_xir_type_is_cell(module->types, function->parameters[0]) &&
            xr_xir_cell_element(module->types, function->parameters[0]) == owner->type.type);
        unsigned constants = 0, returns = 0;
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *instruction = &function->instructions[i];
            if (instruction->op == XR_XIR_CONST_INT) {
                ++constants; CHECK(instruction->type == XR_XIR_I64 && instruction->immediate == 41);
            }
            returns += instruction->op == XR_XIR_RETURN;
        }
        CHECK(constants == 1 && returns == 1);
    }
    CHECK(matches == 1);
}
static void source_enum_ref_permission_rejections(XrXirSourceRequest *request) {
    const char *sources[] = {
        "enum E { V ref f()->i64{return 41} }\nfn bad()->i64{const e=E.V;return e.f()}\n",
        "enum E { V ref f()->i64{return 41} }\nfn bad(e:E)->i64{return e.f()}\n",
        "enum E { V ref f()->i64{return 41} }\nfn bad()->i64{return E.V.f()}\n",
        "enum E { V ref f()->i64{return 41} }\nvar e=E.V\nfn bad()->i64{return e.f()}\n",
        "enum E { V ref f()->i64{return 41} }\nfn bad()->i64{var es=[E.V];return es[0].f()}\n"
    };
    const char *reasons[] = {
        "ref receiver requires a mutable local binding",
        "ref receiver requires a mutable local binding",
        "expression cannot satisfy its declared type",
        "ref receiver of module state is not implemented in XIR",
        "ref receiver place is not implemented in XIR"
    };
    unsigned mismatches = 0;
    for (uint32_t i = 0; i < sizeof(sources)/sizeof(*sources); ++i) {
        write_source(request->entry_path, sources[i]);
        XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
        XrXirStatus status = xr_xir_compile_source_check(request, &result, &diagnostic, NULL);
        if (status != XR_XIR_BAD_TYPE || strcmp(diagnostic.message, reasons[i]))
            fprintf(stderr, "enum ref permission %u: %u %s\n", i, status, diagnostic.message);
        if (status != XR_XIR_BAD_TYPE || result.checked || result.snapshot ||
            strcmp(diagnostic.message, reasons[i])) ++mismatches;
        xr_xir_compile_source_result_free(&result);
    }
    CHECK(!mismatches);
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
    const XrXirStatus statuses[] = {
        XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE,
        XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE, XR_XIR_BAD_STRUCTURE,
        XR_XIR_BAD_STRUCTURE, XR_XIR_OK, XR_XIR_BAD_TYPE, XR_XIR_BAD_VALUE,
        XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE,
        XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE
    };
    const char *reasons[] = {
        "method conflicts with enum builtin member", "method conflicts with enum builtin member",
        "method conflicts with enum builtin member", "variant conflicts with enum type metadata member",
        "method conflicts with enum builtin member", "enum method conflicts with variant",
        "enum method conflicts with variant", "duplicate or empty declaration name",
        "module graph build failed", "", "method declaration contract is not admitted",
        "name is not an initialized value", "member receiver is not a nominal value",
        "explicit implementation is missing a method",
        "interface implementation requires a read instance method with matching own parameters",
        "implementation witness definition obligations failed", "explicit implementation is missing a method",
        "type-qualified access requires a static method", "static method requires type-qualified access"
    };
    _Static_assert(sizeof(statuses)/sizeof(*statuses) == sizeof(rejected)/sizeof(*rejected),
        "Every original enum declaration requires an independent status expectation");
    _Static_assert(sizeof(reasons)/sizeof(*reasons) == sizeof(rejected)/sizeof(*rejected),
        "Every original enum declaration requires an independent diagnostic expectation");
    uint32_t unexpected = 0;
    for (uint32_t i = 0; i < sizeof(rejected)/sizeof(*rejected); ++i) {
        write_source(request->entry_path,rejected[i]); XrXirSourceResult result = {0};
        XrXirSourceDiagnostic diagnostic = {0};
        XrXirStatus status = xr_xir_compile_source_check(request, &result, &diagnostic, NULL);
        fprintf(stderr, "enum rejection %u: %u %d:%d %s\n", i, status,
            diagnostic.line, diagnostic.column, diagnostic.message);
        bool matched = status == statuses[i] && !strcmp(diagnostic.message, reasons[i]);
        if (status == XR_XIR_OK) {
            matched = matched && result.checked && result.snapshot;
            if (matched) source_enum_ref_declaration_facts(&result);
        } else matched = matched && !result.checked && !result.snapshot;
        if (!matched) ++unexpected;
        xr_xir_compile_source_result_free(&result);
    }
    CHECK(unexpected == 0);
    source_enum_witness_run(request,
        "enum E { V ref f()->i64{return 41} }\n"
        "export fn enumWitness()->i64{var e=E.V;return e.f()}\n");
    source_enum_ref_permission_rejections(request);
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
