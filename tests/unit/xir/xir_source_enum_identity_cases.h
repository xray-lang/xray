/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_enum_identity_cases.h - Owned enum identity strings
 *
 * KEY CONCEPT:
 *   Identity strings retain bytes without reading payloads or extending program authority.
 */
#ifndef XIR_SOURCE_ENUM_IDENTITY_CASES_H
#define XIR_SOURCE_ENUM_IDENTITY_CASES_H
#include "xir/xxir_effects.h"
static void source_enum_identity_run(XrXirSourceRequest *request, const char *source, const char *expected) {
    write_source(request->entry_path,source);
    XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_source_check(request,&result,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"generic requirement: %u %d:%d %s\n",status,
        diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
    const XrXirSourceView *view=xr_xir_source_snapshot_view(result.snapshot);
    const XrXirSourceDeclaration *owner=declaration(view,"Box",0);
    if(owner){
        CHECK(declaration(view,"name",owner->id)->kind==XR_XIR_SOURCE_INTRINSIC);
        CHECK(declaration(view,"toString",owner->id)->type.type==XR_XIR_STRING);
    }
    if(owner){
        /* Private payload syntax is not admitted by source. Exercise its real
         * XIR authority separately on a local producer view, never mutate the
         * owned Checked artifact or claim this view has Checked provenance. */
        XrXirModule private_view=*xr_xir_artifact_module(result.checked);
        XrXirTypes private_types=*private_view.types;
        CHECK(private_types.nominals->count==1);
        XrXirNominalTable table=*private_types.nominals;
        XrXirNominalDeclaration record=table.declarations[0];
        CHECK(record.field_count==1);
        XrXirNominalField field=record.fields[0];field.flags|=XR_XIR_FIELD_PRIVATE;
        record.fields=&field;table.declarations=&record;private_types.nominals=&table;private_view.types=&private_types;
        XrXirBudget budget=xr_xir_default_budget();
        CHECK(xr_xir_nominal_structure_verify(&table,&private_types,&budget)==XR_XIR_BAD_STRUCTURE);
        CHECK(xr_xir_verify(&private_view,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    }
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_checked_write(result.checked,NULL,&packet,NULL)==XR_XIR_OK);
    xr_xir_artifact_free(result.checked); result.checked=NULL;
    write_source(request->entry_path,"const replaced=0\n");
    if(owner) CHECK(declaration(view,"name",owner->id)->type.type==XR_XIR_STRING);
    xr_xir_source_result_free(&result);
    XrXirArtifact *checked=NULL,*specialized=NULL,*lowered=NULL;
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&checked,NULL)==XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    XrXirEffects *effects=NULL;
    CHECK(xr_xir_effects_analyze(checked,NULL,&effects)==XR_XIR_OK);
    const XrXirModule *checked_module=xr_xir_artifact_module(checked);
    for(uint32_t f=0;f<checked_module->function_count;++f) {
        const XrXirFunction *function=&checked_module->functions[f];
        if(function->name_length==4 && !memcmp(function->name,"text",4)) {
            const XrXirFunctionEffects *fact=xr_xir_effects_function(effects,f);
            CHECK(fact && fact->suspend==XR_XIR_EFFECT_NONE && fact->throws==XR_XIR_EFFECT_NONE);
        }
    }
    xr_xir_effects_free(effects);
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
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); XrXirInstance *instance=NULL;
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        for (uint32_t e=0;e<2;++e) {
            CHECK(xr_xir_instance_start(instance,entries[e],NULL,0)==XR_XIR_CALL_READY);
            XrXirCallResult outcome=xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome;
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
        CHECK(length==strlen(expected) && !memcmp(bytes,expected,length));
        xr_xir_value_drop(&retained[run]);
    }
}
static void source_enum_identity_cases(XrXirSourceRequest *request) {
    source_enum_identity_run(request,
        "enum Box<T>{Empty,Full{value:T},Last}\n"
        "const seen=Atomic(0)\n"
        "fn touch()->Box<string>{seen.fetchAdd(1);return Box<string>.Full{value:\"payload\"}}\n"
        "fn text<T>(value:Box<T>)->string{return value.toString()}\n"
        "export fn genericMethodNumber()->i64{const name=touch().name;const text=touch().toString();if(name==\"Full\" && text==\"Box.Full\"){return 39+seen.load()}else{return 0}}\n"
        "export fn genericMethodText()->string{return text<string>(Box<string>.Full{value:\"ignored\"})}\n"
        ,"Box.Full");
    source_enum_identity_run(request,
        "enum Container<T>{Full{value:T}}\n"
        "const called=Atomic(0)\n"
        "fn callback()->string{called.fetchAdd(1);Coro.yield();return \"should not run\"}\n"
        "fn text<T>(value:Container<T>)->string{return value.toString()}\n"
        "export fn genericMethodNumber()->i64{const label=text<fn()->string>(Container<fn()->string>.Full{value:callback});if(label==\"Container.Full\"){return 41+called.load()}else{return 0}}\n"
        "export fn genericMethodText()->string{return text<fn()->string>(Container<fn()->string>.Full{value:callback})}\n"
        ,"Container.Full");
    const struct {const char *source,*reason;} rejected[]={
        {"enum E{A}; fn unused()->string{return E.A.toString(1)}", "enum toString accepts no value or type arguments"},
        {"enum E{A}; fn unused()->string{return E.A.toString<i64>()}", "enum toString accepts no value or type arguments"},
        {"enum E{A}; fn unused()->string{return E.A.name<i64>}", "enum name is not generic"},
        {"fn unused<T:Error>(value:T)->string{return value.toString()}", "receiver has no declared interface requirements"},
        {"fn unused<T>(value:T)->string{return value.toString()}", "receiver has no declared interface requirements"}
    };
    for(uint32_t i=0;i<sizeof(rejected)/sizeof(*rejected);++i){
        write_source(request->entry_path,rejected[i].source);
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_source_check(request,&result,&diagnostic);
        if(status!=XR_XIR_BAD_TYPE || !strstr(diagnostic.message,rejected[i].reason))
            fprintf(stderr,"enum identity rejection %u: %u %s\n",i,status,diagnostic.message);
        CHECK(status==XR_XIR_BAD_TYPE && !result.checked && strstr(diagnostic.message,rejected[i].reason));
        if(result.snapshot)CHECK(!xr_xir_source_snapshot_view(result.snapshot)->complete);
        xr_xir_source_result_free(&result);
    }
    char enum_name[302],variant_name[402],expected[705],source[4096];
    memset(enum_name,'E',sizeof(enum_name)-1);enum_name[sizeof(enum_name)-1]=0;
    memset(variant_name,'V',sizeof(variant_name)-1);variant_name[sizeof(variant_name)-1]=0;
    int size=snprintf(expected,sizeof(expected),"%s.%s",enum_name,variant_name);
    CHECK(size==703);
    size=snprintf(source,sizeof(source),"enum %s{%s}; export fn genericMethodNumber()->i64{return 41}; export fn genericMethodText()->string{return %s.%s.toString()}",enum_name,variant_name,enum_name,variant_name);
    CHECK(size>0 && (size_t)size<sizeof(source));source_enum_identity_run(request,source,expected);

    char library[XR_TEST_PATH_MAX];
    CHECK(snprintf(library,sizeof(library),"%s/enum-identity-owner.xr",request->authority->physical_root)>0);
    write_source(library,"export enum Original<T>{Empty,Full{value:T}}\n");
    source_enum_identity_run(request,
        "import {Original as Alias} from \"./enum-identity-owner\"\n"
        "fn text<T>(value:Alias<T>)->string{return value.toString()}\n"
        "export fn genericMethodNumber()->i64{const name=Alias<i64>.Full{value:41}.name;if(name==\"Full\"){return 41}else{return 0}}\n"
        "export fn genericMethodText()->string{return text<string>(Alias<string>.Full{value:\"payload\"})}\n",
        "Original.Full");

    write_source(library,
        "export struct Secret{private n:i64;static make()->Secret{return Secret{n:41}}}\n"
        "export enum Public<T>{Full{value:T}}\n"
        "export fn make()->Public<Secret>{return Public<Secret>.Full{value:Secret.make()}}\n");
    source_enum_identity_run(request,
        "import {make} from \"./enum-identity-owner\"\n"
        "export fn genericMethodNumber()->i64{const text=make().toString();if(text==\"Public.Full\"){return 41}else{return 0}}\n"
        "export fn genericMethodText()->string{return make().toString()}\n", "Public.Full");

    write_source(request->entry_path,
        "import {make,Secret} from \"./enum-identity-owner\"\n"
        "fn unused()->Secret{const identity=make().toString();return Secret{n:41}}\n");
    XrXirSourceResult denied={0};XrXirSourceDiagnostic denied_diagnostic={0};
    XrXirStatus denied_status=xr_xir_source_check(request,&denied,&denied_diagnostic);
    if(denied_status!=XR_XIR_BAD_TYPE || !strstr(denied_diagnostic.message,"field access is not permitted"))
        fprintf(stderr,"enum identity does not grant construction: %u %s\n",denied_status,denied_diagnostic.message);
    CHECK(denied_status==XR_XIR_BAD_TYPE && !denied.checked && strstr(denied_diagnostic.message,"field access is not permitted"));
    xr_xir_source_result_free(&denied);
    write_source(library,"enum Original<T>{Empty,Full{value:T}}\n");
    write_source(request->entry_path,"import {Original as Alias} from \"./enum-identity-owner\"\nfn unused(value:Alias<i64>)->string{return value.toString()}\n");
    XrXirSourceResult hidden={0};XrXirSourceDiagnostic hidden_diagnostic={0};
    CHECK(xr_xir_source_check(request,&hidden,&hidden_diagnostic)==XR_XIR_BAD_STRUCTURE);
    CHECK(!hidden.checked && strstr(hidden_diagnostic.message,"import requires an exported declaration"));
    xr_xir_source_result_free(&hidden);CHECK(xr_test_unlink(library)==0);

}
#endif
