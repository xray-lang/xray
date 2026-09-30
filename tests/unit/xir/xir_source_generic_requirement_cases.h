/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_generic_requirement_cases.h - Original generic method witnesses
 *
 * KEY CONCEPT:
 *   Parent and method parameters retain their declaration identity through owned packets.
 */
#ifndef XIR_SOURCE_GENERIC_REQUIREMENT_CASES_H
#define XIR_SOURCE_GENERIC_REQUIREMENT_CASES_H
static void source_generic_requirement_run(XrXirSourceRequest *request, const char *source) {
    write_source(request->entry_path,source);
    XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_source_check(request,&result,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"generic requirement: %u %d:%d %s\n",status,
        diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
    const XrXirSourceView *view=xr_xir_source_snapshot_view(result.snapshot);
    const XrXirSourceDeclaration *owner=declaration(view,"Mapper",0);
    if (owner) {
    const XrXirSourceDeclaration *method=declaration(view,"map",owner->id);
    CHECK(method && method->generic_parent==owner->id && method->generic_parent_count==1 &&
        method->generic_parameter_count==2 && method->parameter_count==2);
    CHECK(method->type.generic_owner==method->id && method->parameters[0].generic_owner==method->id &&
        method->parameters[1].generic_owner==method->id);
    CHECK(method->type.type==(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1));
    }
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_checked_write(result.checked,NULL,&packet,NULL)==XR_XIR_OK);
    xr_xir_artifact_free(result.checked); result.checked=NULL;
    write_source(request->entry_path,"const replaced=0\n");
    if (owner) CHECK(declaration(view,"map",owner->id)->generic_constraints[1].markers==XR_XIR_CONSTRAINT_SENDABLE);
    xr_xir_source_result_free(&result);
    XrXirArtifact *checked=NULL,*specialized=NULL,*lowered=NULL;
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&checked,NULL)==XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_specialize(checked,NULL,&specialized,NULL)==XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(specialized,&target,NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_artifact_free(specialized);
    const XrXirModule *module=xr_xir_artifact_module(lowered);
    uint32_t entries[2]={UINT32_MAX,UINT32_MAX};
    const char *names[]={"genericMethodNumber","genericMethodText"};
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        for (uint32_t e=0;e<2;++e)
            if (function->name_length==strlen(names[e]) && !memcmp(function->name,names[e],strlen(names[e]))) entries[e]=f;
        for (uint32_t i=0;i<function->instruction_count;++i) CHECK(function->instructions[i].op!=XR_XIR_CALL_REQUIREMENT);
    }
    CHECK(entries[0]!=UINT32_MAX && entries[1]!=UINT32_MAX);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
    XrXirValue retained[2]={{0},{0}};
    for (uint32_t run=0;run<2;++run) {
        XrXirInstanceConfig config=xr_xir_instance_defaults(); XrXirInstance *instance=NULL;
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        for (uint32_t e=0;e<2;++e) {
            CHECK(xr_xir_instance_start(instance,entries[e],NULL,0)==XR_XIR_CALL_READY);
            XrXirCallResult outcome=xr_xir_instance_poll(instance).outcome;
            CHECK(outcome.status==XR_XIR_CALL_RETURNED);
            XrXirValue value={0};
            CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
            if (!e) { CHECK(value.type==XR_XIR_I64 && value.payload==41); xr_xir_value_drop(&value); }
            else retained[run]=value;
        }
        CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_program_drop(program);
    for (uint32_t run=0;run<2;++run) {
        const char *bytes=NULL; size_t length=0;
        CHECK(retained[run].type==XR_XIR_STRING && xr_xir_string_view(&retained[run],&bytes,&length));
        CHECK(length==6 && !memcmp(bytes,"mapped",6));
        xr_xir_value_drop(&retained[run]);
    }
}
static void source_generic_requirement_positive(XrXirSourceRequest *request) {
    source_generic_requirement_run(request,
        "interface Mapper<A> { map<U:Sendable>(seed:A,value:U)->U }\n"
        "struct Adapter<X,Y> implements Mapper<X> { seed:X; label:Y; "
        "map<V:Sendable>(seed:X,value:V)->V{return value} }\n"
        "fn apply<A,T:Mapper<A>,U:Sendable>(mapper:T,seed:A,value:U)->U{return mapper.map<U>(seed,value)}\n"
        "export fn genericMethodNumber()->i64{return apply<i64,Adapter<i64,string>,i64>(Adapter<i64,string>{seed:0,label:\"a\"},0,41)}\n"
        "export fn genericMethodText()->string{return apply<i64,Adapter<i64,string>,string>(Adapter<i64,string>{seed:0,label:\"b\"},0,\"map\"+\"ped\")}\n");
    source_generic_requirement_run(request,
        "interface Evidence<A> {}\n"
        "interface Strong<A> extends Evidence<A> {}\n"
        "interface Left<P> { map<U:Strong<P>>(value:U)->U }\n"
        "interface Right<Q> { map<V:Strong<Q> & Evidence<Q>>(value:V)->V }\n"
        "interface Both<R> extends Left<R>,Right<R> {}\n"
        "struct Token implements Strong<i64> { value:i64 }\n"
        "struct Adapter<X,Y> implements Both<X> { map<W:Strong<X>>(value:W)->W{return value} }\n"
        "fn apply<P,T:Both<P>,U:Strong<P>>(mapper:T,value:U)->U{return mapper.map<U>(value)}\n"
        "export fn genericMethodNumber()->i64{return apply<i64,Adapter<i64,string>,Token>(Adapter<i64,string>{},Token{value:41}).value}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n");
    source_generic_requirement_run(request,
        "interface Evidence<A> {}\n"
        "interface Forward { map<U:Evidence<V>,V>(value:U,tag:V)->U }\n"
        "struct Token implements Evidence<string> { value:i64 }\n"
        "struct Adapter<X,Y> implements Forward { map<C:Evidence<D>,D>(value:C,tag:D)->C{return value} }\n"
        "fn apply<T:Forward,A:Evidence<B>,B>(mapper:T,value:A,tag:B)->A{return mapper.map<A,B>(value,tag)}\n"
        "export fn genericMethodNumber()->i64{return apply<Adapter<i64,string>,Token,string>(Adapter<i64,string>{},Token{value:41},\"tag\").value}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n");
    source_generic_requirement_run(request,
        "interface Evidence<A> { evidence()->A }\n"
        "interface Nested<A> { map<U:Evidence<Array<A>>>(value:U)->U }\n"
        "struct Token implements Evidence<Array<i64>> { value:i64; evidence()->Array<i64>{return [this.value]} }\n"
        "struct Adapter<X,Y> implements Nested<Y> { map<V:Evidence<Array<Y>>>(value:V)->V{return value} }\n"
        "fn apply<Z,T:Nested<Z>,U:Evidence<Array<Z>>>(mapper:T,value:U)->U{return mapper.map<U>(value)}\n"
        "export fn genericMethodNumber()->i64{return apply<i64,Adapter<string,i64>,Token>(Adapter<string,i64>{},Token{value:41}).value}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n");
}
static void source_generic_requirement_rejections(XrXirSourceRequest *request) {
    const char *rejected[] = {
        /* Unused bodies still check their own parameter bounds. */
        ("interface I { map<U:Sendable>(value:U)->U }\n"
         "fn unused<T:I,U>(receiver:T,value:U)->U{return receiver.map<U>(value)}\n"),
        /* An implementation cannot use its own stronger bound to authorize itself. */
        ("interface I { map<U>(value:U)->U }\n"
         "struct S implements I { map<V:Sendable>(value:V)->V{return value} }\n"),
        /* Same flat index across unrelated owners is not the same type. */
        ("interface I<A> { map<U>(seed:A,value:U)->U }\n"
         "struct S<X,Y> implements I<X> { map<V>(seed:V,value:X)->X{return value} }\n"),
        ("interface I<A> { map<A>(value:A)->A }\n"),
        ("interface I { map<U,U>(value:U)->U }\n"),
        ("interface I { map<U>(value:U)->U }\n"
         "struct S implements I { map(value:i64)->i64{return value} }\n"),
        ("interface I { map<U>(value:U)->U }\n"
         "fn unused<T:I>(receiver:T)->i64{return receiver.map(41)}\n"),
        ("interface I { map<U>(value:U)->U }\n"
         "fn unused<T:I>(receiver:T)->i64{return receiver.map<i64,string>(41)}\n"),
        /* No body-only nominal premise can repair explicit implementation legality. */
        ("interface I { map<U>(value:U)->U }\n"
         "struct S<X> implements I { map<V>(value:V)->V where X:Sendable{return value} }\n"),
        /* Own arity is part of inherited same-name contracts, even when unused. */
        ("interface A { map<U>(value:U)->U }\n"
         "interface B { map<U,V>(value:U)->U }\n"
         "interface C extends A,B {}\n"),
        /* Own conditions are part of inherited compatibility, not caller hints. */
        ("interface A { map<U:Sendable>(value:U)->U }\n"
         "interface B { map<V>(value:V)->V }\n"
         "interface C extends A,B {}\n")
    };
    const char *reasons[] = {
        "method type argument does not prove", "implementation witness definition obligations failed",
        "implementation witness definition obligations failed", "duplicates or shadows", "failed to parse module",
        "matching own parameters", "exact explicit type arguments", "exact explicit type arguments",
        "implementation witness definition obligations failed", "source declaration type structure is invalid",
        "source declaration type structure is invalid"
    };
    const XrXirStatus statuses[] = {
        XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE,
        XR_XIR_BAD_STRUCTURE, XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE,
        XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE
    };
    _Static_assert(sizeof(statuses)/sizeof(*statuses)==sizeof(rejected)/sizeof(*rejected),
        "Every rejection requires an independent status expectation");
    _Static_assert(sizeof(reasons)/sizeof(*reasons)==sizeof(rejected)/sizeof(*rejected),
        "Every rejection requires an independent diagnostic expectation");
    for (uint32_t i=0;i<sizeof(rejected)/sizeof(*rejected);++i) {
        write_source(request->entry_path,rejected[i]);
        XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_source_check(request,&result,&diagnostic);
        if (status!=statuses[i] || !strstr(diagnostic.message,reasons[i]))
            fprintf(stderr,"generic requirement rejection %u: %u %s\n",i,status,diagnostic.message);
        CHECK(status==statuses[i] && !result.checked && strstr(diagnostic.message,reasons[i]));
        if (result.snapshot) CHECK(!xr_xir_source_snapshot_view(result.snapshot)->complete);
        xr_xir_source_result_free(&result);
    }
}
#endif // XIR_SOURCE_GENERIC_REQUIREMENT_CASES_H
