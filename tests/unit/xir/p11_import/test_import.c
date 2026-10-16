/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_import.c - Real foreign data receiving through the compiler entry
 */
#define main materialization_original_main
#include "../p11_materialization/test_materialization.c"
#undef main
#include "xir/xxir_array.h"
#include "xir/xxir_compile_domain.h"
#include "xir/xxir_type_arena.h"

static XrXirArtifact *import_lower(XrXirCompileContext *context,const char *text) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"foreign-data",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,strlen(text)};XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&source,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"Foreign Source=%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked);xr_compile_session_free(session);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    return lowered;
}
static XrXirCtfeCallRequest import_request(XrXirArtifact *artifact,const char *name,const XrXirValue *argument) {
    XrXirCtfeCallRequest request={0};request.evaluation.artifact=artifact;
    request.evaluation.function=named(xr_xir_compile_artifact_module(artifact),name);
    request.evaluation.target=(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    memcpy(request.evaluation.checked_identity,artifact->checked_identity,32);
    request.evaluation.limits=(XrXirCtfeLimits){65536,65536,8388608};
    request.evaluation.arguments=argument;request.evaluation.argument_count=argument?1:0;request.call_depth=128;return request;
}
static const char array_text[]=
    "fn produce()->Array<string>{return [\"ab\",\"cd\"]}\n"
    "export fn pass(value:Array<string>)->Array<string>{return value}\n"
    "export fn measure(value:Array<string>)->i64{return len(value[0])+len(value[1])+38}\n";
static const char tuple_text[]=
    "fn produce()->(string,Array<i64>){return (\"ab\",[20,22])}\n"
    "export fn pass(value:(string,Array<i64>))->(string,Array<i64>){return value}\n"
    "export fn measure(value:(string,Array<i64>))->i64{const(text,values)=value;return len(text)+values[0]+values[1]-2}\n";
static const char nominal_text[]=
    "struct Node{private value:i64;private children:Array<Node>;static make()->Node{return Node{value:40,children:[Node{value:2,children:[]}]}};sum()->i64{return this.value+this.children[0].value}}\n"
    "fn produce()->Node{return Node.make()}\nexport fn pass(value:Node)->Node{return value}\n"
    "export fn measure(value:Node)->i64{return value.sum()}\n";
static const char nullable_text[]=
    "fn produce()->Array<i64?>{return [null,42]}\n"
    "export fn pass(value:Array<i64?>)->Array<i64?>{return value}\n"
    "export fn measure(value:Array<i64?>)->i64{return (value[0]??1)+(value[1]??0)-1}\n";
static const char enum_text[]=
    "enum Item{Value{text:string},Empty}\nfn produce()->Item{return Item.Value{text:\"ab\"}}\n"
    "export fn pass(value:Item)->Item{return value}\n"
    "export fn measure(value:Item)->i64{return match(value){Item.Value{text}->len(text)+40,Item.Empty->42}}\n";
static const char empty_text[]=
    "enum Item{Value{text:string},Empty}\nfn produce()->Item{return Item.Empty}\n"
    "export fn pass(value:Item)->Item{return value}\n"
    "export fn measure(value:Item)->i64{return match(value){Item.Value{text}->len(text)+40,Item.Empty->42}}\n";
static const char *import_text(const char *kind) {
    return !strcmp(kind,"array")?array_text:!strcmp(kind,"tuple")?tuple_text:
        !strcmp(kind,"nullable")?nullable_text:!strcmp(kind,"enum")?enum_text:!strcmp(kind,"empty")?empty_text:nominal_text;
}
static void import_normal(const char *kind,bool runtime,bool exhausted) {
    const char *text=import_text(kind);
    XrXirCompileContext producer,receiver;context_new(&producer);context_new(&receiver);
    char producer_text[2048];
    CHECK(snprintf(producer_text,sizeof(producer_text),"%s%s",!strcmp(kind,"nominal")?
        "struct Padding{unused:i8}\n":strcmp(kind,"enum") && strcmp(kind,"empty")?
        "fn padding()->(bool,bool,bool){return (true,false,true)}\n":"",text)>0);
    XrXirArtifact *origin=import_lower(&producer,producer_text),*target=import_lower(&receiver,text);
    XrXirCtfeCallRequest make=import_request(origin,"produce",NULL);XrXirValue input={0};
    XrXirCtfeStatus status=xr_xir_compile_ctfe_calls(&make,&input);
    if (status!=XR_XIR_CTFE_OK) fprintf(stderr,"Produce %s status=%u\n",kind,status);
    CHECK(status==XR_XIR_CTFE_OK);
    XrXirDomain *foreign=NULL;
    if (runtime) {
        CHECK(!strcmp(kind,"array"));CHECK(xr_xir_domain_new(1048576,&foreign)==XR_XIR_VALUE_OK);
        XrXirValueAdmission admission={.arena=xr_xir_value_arena(&input),.domain=foreign,.work=1000000,.scratch_bytes=65536};
        XrXirValue values[2]={{0}},array={0};XrXirFaultDetail fault={0};
        for (unsigned i=0;i<2;++i) CHECK(xr_xir_array_get(&input,i,&admission,&values[i],&fault)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_array_with_capacity((XrXirType)input.type,7,&admission,&array)==XR_XIR_VALUE_OK);
        XrXirValuePlace place={(XrXirType)array.type,&array.payload};
        for (unsigned i=0;i<2;++i) { CHECK(xr_xir_array_push(&place,&values[i],&admission)==XR_XIR_VALUE_OK);xr_xir_value_drop(&values[i]); }
        xr_xir_value_drop(&input);input=array;
    }
    for (uint64_t prefix=0;prefix<=64;++prefix) {
        XrCompileResources *query=NULL;CHECK(xr_compile_resources_new(&caps,&query)==XR_COMPILE_RESOURCE_OK);
        XrCompileResourceStats initial={0};CHECK(xr_compile_resources_stats(query,&initial)==XR_COMPILE_RESOURCE_OK);
        CHECK(initial.work<=caps.work-prefix && xr_compile_resources_work(query,caps.work-prefix-initial.work)==XR_COMPILE_RESOURCE_OK);
        const XrXirTypeArena *arena=NULL;
        XrXirValueStatus observed=xr_xir_compile_value_arena_borrow(query,&input,&arena);
        CHECK(observed==(prefix==64?XR_XIR_VALUE_OK:XR_XIR_VALUE_LIMIT));
        CHECK((arena!=NULL)==(prefix==64));
        if (prefix<64) CHECK(xr_xir_compile_value_arena_borrow(query,&input,&arena)==XR_XIR_VALUE_LIMIT && !arena);
        else { XrCompileResourceStats paid={0};CHECK(xr_compile_resources_stats(query,&paid)==XR_COMPILE_RESOURCE_OK && paid.work==caps.work && paid.allocation_count==initial.allocation_count); }
        xr_compile_resources_release(query);
    }
    XrCompileResourceStats before=stats(&producer);
    if (exhausted) { CHECK(xr_compile_resources_work(producer.resources,caps.work-before.work)==XR_COMPILE_RESOURCE_OK);before=stats(&producer); }
    XrXirCtfeCallRequest pass=import_request(target,"pass",&input);
    XrXirValue result={0};
    for (unsigned leaf=0;leaf<2;++leaf) {
        status=leaf?xr_xir_compile_ctfe_leaf(&pass.evaluation,&result):xr_xir_compile_ctfe_calls(&pass,&result);
        fprintf(stderr,"Foreign %s runtime=%u leaf=%u sourceType=%u targetType=%u status=%u\n",kind,runtime,leaf,input.type,
            xr_xir_compile_artifact_module(target)->functions[pass.evaluation.function].parameters[0],status);
        CHECK(status==XR_XIR_CTFE_OK && result.payload!=input.payload && xr_xir_value_arena(&result)!=xr_xir_value_arena(&input));
        XrXirCtfeCallRequest measure=import_request(target,"measure",&result);XrXirValue answer={0};
        CHECK(xr_xir_compile_ctfe_calls(&measure,&answer)==XR_XIR_CTFE_OK && answer.type==XR_XIR_I64 && answer.payload==42);
        xr_xir_value_drop(&answer);
        if (!strcmp(kind,"array")) {
            const XrXirTypeArena *arena=xr_xir_value_arena(&result);XrXirDomain *domain=NULL;
            CHECK(xr_compile_resources_work(receiver.resources,4096)==XR_COMPILE_RESOURCE_OK);
            CHECK(xr_xir_compile_value_domain_borrow(receiver.resources,&result,arena,&domain)==XR_XIR_VALUE_OK);
            XrXirValueAdmission admission={.arena=arena,.domain=domain,.work=1000000,.scratch_bytes=65536};int64_t capacity=0;
            CHECK(xr_xir_array_capacity(&result,&admission,&capacity)==XR_XIR_VALUE_OK && (!runtime || capacity==7));
        }
        CHECK(stats(&producer).work==before.work && stats(&producer).allocated_bytes==before.allocated_bytes);
        if (!leaf) xr_xir_value_drop(&result);
    }
    xr_xir_value_drop(&input);xr_xir_domain_drop(foreign);xr_xir_compile_artifact_free(origin);xr_compile_resources_release(producer.resources);
    XrXirCtfeCallRequest measure=import_request(target,"measure",&result);XrXirValue answer={0};
    CHECK(xr_xir_compile_ctfe_calls(&measure,&answer)==XR_XIR_CTFE_OK && answer.payload==42);
    xr_xir_value_drop(&answer);xr_xir_compile_artifact_free(target);xr_compile_resources_release(receiver.resources);
    CHECK(xr_xir_value_valid(&result));xr_xir_value_drop(&result);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void import_nominal_mismatch(void) {
    char other[sizeof(nominal_text)+64];size_t offset=0;
    for (const char *cursor=nominal_text;*cursor;) {
        if (!strncmp(cursor,"Node",4)) { memcpy(other+offset,"Other",5);offset+=5;cursor+=4; }
        else other[offset++]=*cursor++;
        CHECK(offset<sizeof(other));
    }
    other[offset]=0;XrXirCompileContext producer,receiver;context_new(&producer);context_new(&receiver);
    XrXirArtifact *origin=import_lower(&producer,nominal_text),*target=import_lower(&receiver,other);
    XrXirCtfeCallRequest make=import_request(origin,"produce",NULL);XrXirValue input={0};
    CHECK(xr_xir_compile_ctfe_calls(&make,&input)==XR_XIR_CTFE_OK);
    XrXirCtfeCallRequest request=import_request(target,"pass",&input);
    for (unsigned leaf=0;leaf<2;++leaf) {
        XrXirValue output={0};XrCompileResourceStats before=stats(&receiver);
        XrXirCtfeStatus status=leaf?xr_xir_compile_ctfe_leaf(&request.evaluation,&output):xr_xir_compile_ctfe_calls(&request,&output);
        fprintf(stderr,"Different nominal leaf=%u status=%u\n",leaf,status);
        CHECK(status==XR_XIR_CTFE_BAD_ARGUMENT && !output.type && !output.reserved && !output.payload);
        CHECK(stats(&receiver).live_bytes==before.live_bytes);
    }
    xr_xir_value_drop(&input);xr_xir_compile_artifact_free(origin);xr_xir_compile_artifact_free(target);
    xr_compile_resources_release(producer.resources);xr_compile_resources_release(receiver.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static XrXirCtfeStatus import_evaluate(const XrXirCtfeCallRequest *request,bool leaf,XrXirValue *output) {
    return leaf?xr_xir_compile_ctfe_leaf(&request->evaluation,output):xr_xir_compile_ctfe_calls(request,output);
}
static void import_faults(const char *kind,bool prefix) {
    const char *text=import_text(kind);XrXirCompileContext producer;context_new(&producer);
    XrXirArtifact *origin=import_lower(&producer,text);XrXirCtfeCallRequest make=import_request(origin,"produce",NULL);
    XrXirValue input={0};CHECK(xr_xir_compile_ctfe_calls(&make,&input)==XR_XIR_CTFE_OK);
    XrXirValue unchanged=input;XrCompileResourceStats producer_stats=stats(&producer);
    for (unsigned mode=0;mode<2;++mode) {
        size_t sites=0;
        {
            XrXirCompileContext receiver;context_new(&receiver);XrXirArtifact *target=import_lower(&receiver,text);
            XrXirCtfeCallRequest request=import_request(target,"pass",&input);XrXirValue output={0};
            size_t first=source_fixture_compile_attempts;XrCompileResourceStats baseline=stats(&receiver);
            CHECK(import_evaluate(&request,mode!=0,&output)==XR_XIR_CTFE_OK);
            sites=source_fixture_compile_attempts-first;CHECK(sites && xr_xir_value_valid(&output));xr_xir_value_drop(&output);
            CHECK(stats(&receiver).live_bytes==baseline.live_bytes);
            xr_xir_compile_artifact_free(target);xr_compile_resources_release(receiver.resources);
        }
        size_t trials=prefix?(sites==1?1:2):sites;
        for (size_t trial=0;trial<trials;++trial) {
            size_t fault=prefix?(trial?sites-1:0):trial;
            size_t blocks=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
            XrXirCompileContext receiver;context_new(&receiver);XrXirArtifact *target=import_lower(&receiver,text);
            XrXirCtfeCallRequest request=import_request(target,"pass",&input);XrXirValue output={0};
            XrCompileResourceStats baseline=stats(&receiver);size_t first=source_fixture_compile_attempts;
            source_fixture_compile_injected=false;source_fixture_compile_fail_at=first+fault;
            XrXirCtfeStatus status=import_evaluate(&request,mode!=0,&output);
            CHECK(status==XR_XIR_CTFE_OUT_OF_MEMORY && source_fixture_compile_injected);
            CHECK(source_fixture_compile_attempts==first+fault+1 && !output.type && !output.reserved && !output.payload);
            source_fixture_compile_fail_at=SIZE_MAX;CHECK(stats(&receiver).live_bytes==baseline.live_bytes);
            CHECK(import_evaluate(&request,mode!=0,&output)==XR_XIR_CTFE_OK);
            XrXirCtfeCallRequest measure=import_request(target,"measure",&output);XrXirValue answer={0};
            CHECK(xr_xir_compile_ctfe_calls(&measure,&answer)==XR_XIR_CTFE_OK && answer.payload==42);
            xr_xir_value_drop(&answer);xr_xir_value_drop(&output);CHECK(stats(&receiver).live_bytes==baseline.live_bytes);
            xr_xir_compile_artifact_free(target);xr_compile_resources_release(receiver.resources);
            CHECK(source_fixture_compile_live==blocks && source_fixture_compile_bytes==bytes);
            CHECK(!memcmp(&input,&unchanged,sizeof(input)) && stats(&producer).work==producer_stats.work);
        }
        fprintf(stderr,"Foreign %s leaf=%u actualFI=%zu selected=%zu %s same-owner retry42/live/source-owner unchanged\n",
            kind,mode,sites,trials,prefix?"FIRST_LAST_ONLY":"COMPLETE_ALL_ORDINALS");
    }
    xr_xir_value_drop(&input);xr_xir_compile_artifact_free(origin);xr_compile_resources_release(producer.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
#ifndef XR_IMPORT_MAIN
#define XR_IMPORT_MAIN main
#endif
int XR_IMPORT_MAIN(int argc,char **argv) {
    if (argc==3 && (!strcmp(argv[2],"--prefix") || !strcmp(argv[2],"--faults"))) {
        import_faults(argv[1],!strcmp(argv[2],"--prefix"));return 0;
    }
    CHECK(argc==2 || argc==3);bool runtime=argc==3 && !strcmp(argv[2],"--runtime");
    bool exhausted=argc==3 && !strcmp(argv[2],"--exhausted");CHECK(argc==2 || runtime || exhausted);
    if (!strcmp(argv[1],"mismatch")) { import_nominal_mismatch();return 0; }
    CHECK(!strcmp(argv[1],"array") || !strcmp(argv[1],"tuple") || !strcmp(argv[1],"nominal") ||
        !strcmp(argv[1],"nullable") || !strcmp(argv[1],"enum") || !strcmp(argv[1],"empty"));
    import_normal(argv[1],runtime,exhausted);return 0;
}
