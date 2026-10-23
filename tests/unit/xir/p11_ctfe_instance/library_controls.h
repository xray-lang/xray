/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * library_controls.h - Opaque literal type names grant no field authority
 */
#include "xir/xxir_source_generation.h"
#include "xir/xxir_source_query_internal.h"
#include "frontend/parser/xparse.h"
#include "frontend/parser/xast.h"
#include "../xir_slot_previous36_96_golden.h"
#include "../xir_effect_input_fixture.h"
static void library_slot_names(const XrXirConstruction *owner,const char *const *names,uint32_t count) {
    const XrXirSlotFacts *facts=xr_xir_compile_construction_slots(owner);
    CHECK(facts && facts->count==count && (!!facts->rows==!!count));
    for (uint32_t s=0;s<count;++s) {
        CHECK(facts->rows[s].slot==s && facts->rows[s].exported==0);
        CHECK(facts->rows[s].name_length==strlen(names[s]) &&
            !memcmp(facts->rows[s].name,names[s],facts->rows[s].name_length));
    }
}
static void library_slot_wire_controls(const XrXirCompileContext *context,XrXirCheckedPacket *packet,
    const XrXirSlotFacts *facts) {
    uint8_t pattern[256]={0};size_t length=4,offsets[5]={0};
    CHECK(facts && facts->count==4);evaluation_u32(pattern,facts->count);
    for (uint32_t s=0;s<facts->count;++s) {
        const XrXirSlotFact *row=&facts->rows[s];CHECK(length+12+row->name_length<=sizeof(pattern));
        if (!s) offsets[1]=length;
        evaluation_u32(pattern+length,row->slot);evaluation_u32(pattern+length+4,row->name_length);length+=8;
        if (!s) offsets[2]=length;
        memcpy(pattern+length,row->name,row->name_length);length+=row->name_length;
        if (!s) offsets[3]=length;
        if (s==1) offsets[4]=length;
        evaluation_u32(pattern+length,row->exported);length+=4;
    }
    size_t at=64;while (at+length<=packet->length && memcmp(packet->bytes+at,pattern,length)) ++at;
    CHECK(at+length<=packet->length);
    const uint8_t invalid[]={0,1,0xff,2,1};
    for (unsigned kind=0;kind<5;++kind) {
        size_t position=at+offsets[kind];uint8_t saved=packet->bytes[position];
        packet->bytes[position]=invalid[kind];evaluation_digest(packet);XrXirArtifact *out=NULL;
        CHECK(xr_xir_compile_checked_read(context,packet->bytes,packet->length,&out,NULL)==XR_XIR_BAD_STRUCTURE && !out);
        packet->bytes[position]=saved;evaluation_digest(packet);
    }
    XrXirArtifact *retry=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet->bytes,packet->length,&retry,NULL)==XR_XIR_OK && retry);
    xr_xir_compile_artifact_free(retry);
}
static void library_slot_raw_controls(const XrXirCompileContext *context,const XrXirArtifact *source) {
    const XrXirConstruction *original=source->construction;
    const XrXirSlotFacts *facts=xr_xir_compile_construction_slots(original);
    XrXirConstructionSpec spec={original->rows,original->count,original->materializations,
        original->materialization_count,&original->comptime};
    CHECK(facts && facts->count==4);XrXirConstruction *output=NULL;
    CHECK(xr_xir_compile_construction_new_v4(context,source->module.types,&spec,NULL,&output)==XR_XIR_BAD_STRUCTURE && !output);
    XrXirConstruction *occupied=(XrXirConstruction *)(uintptr_t)1;
    CHECK(xr_xir_compile_construction_new_v4(NULL,(const XrXirTypes *)(uintptr_t)1,
        (const XrXirConstructionSpec *)(uintptr_t)1,(const XrXirSlotFacts *)(uintptr_t)1,&occupied)==XR_XIR_BAD_STRUCTURE);
    CHECK(occupied==(XrXirConstruction *)(uintptr_t)1);
    for (unsigned kind=0;kind<8;++kind) {
        XrXirSlotFact rows[4];memcpy(rows,facts->rows,sizeof(rows));
        XrXirSlotFacts input={rows,4};const char embedded[]={'a',0,'b'};
        if (kind==0) rows[0].slot=1;
        if (kind==1) rows[1].slot=0;
        if (kind==2) rows[0].name=NULL;
        if (kind==3) rows[0].name_length=0;
        if (kind==4) { rows[0].name=embedded;rows[0].name_length=3; }
        if (kind==5) { rows[0].name="\xff";rows[0].name_length=1; }
        if (kind==6) rows[0].exported=2;
        if (kind==7) input.rows=NULL;
        output=NULL;
        CHECK(xr_xir_compile_construction_new_v4(context,source->module.types,&spec,&input,&output)==XR_XIR_BAD_STRUCTURE && !output);
    }
    for (unsigned kind=0;kind<3;++kind) {
        XrXirSlotFact rows[4];memcpy(rows,facts->rows,sizeof(rows));
        XrXirSlotFacts input={rows,kind==0?3u:kind==1?0u:4u};
        if (!input.count) input.rows=NULL;
        if (kind==2) rows[1].exported=1;
        output=NULL;CHECK(xr_xir_compile_construction_new_v4(context,source->module.types,&spec,&input,&output)==XR_XIR_OK);
        XrXirArtifact *received=NULL;
        CHECK(xr_xir_compile_recheck_v2(context,&source->module,output,&received,NULL)==XR_XIR_BAD_STRUCTURE && !received);
        received=(XrXirArtifact *)(uintptr_t)1;
        CHECK(xr_xir_compile_recheck_v2(context,&source->module,output,&received,NULL)==XR_XIR_BAD_STRUCTURE &&
            received==(XrXirArtifact *)(uintptr_t)1);
        xr_xir_compile_construction_free(output);
    }
    const size_t failures[]={0,2};
    for (unsigned point=0;point<2;++point) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=failures[point];
        source_fixture_compile_injected=false;output=NULL;
        CHECK(xr_xir_compile_construction_new_v4(context,source->module.types,&spec,facts,&output)==XR_XIR_OUT_OF_MEMORY &&
            !output && source_fixture_compile_injected);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(xr_xir_compile_construction_new_v4(context,source->module.types,&spec,facts,&output)==XR_XIR_OK);
        xr_xir_compile_construction_free(output);
    }
    XrXirSlotFact *actual=(XrXirSlotFact *)original->slots.rows;
    actual[0].exported=1;
    CHECK(xr_xir_compile_artifact_verify(source,NULL)==XR_XIR_BAD_STRUCTURE);
    actual[0].exported=0;CHECK(xr_xir_compile_artifact_verify(source,NULL)==XR_XIR_OK);
    XrXirArtifact *previous=NULL;
    CHECK(xr_xir_compile_checked_read(context,xir_slot_previous36_96,sizeof(xir_slot_previous36_96),&previous,NULL)==XR_XIR_BAD_STRUCTURE && !previous);
}
static void library_slot_program(XrXirArtifact *checked,const char *const *names,uint32_t count) {
    XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);library_slot_names(closed->construction,names,count);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);library_slot_names(lowered->construction,names,count);
    uint32_t entry=lowered->module.declarations->entry_function,run=UINT32_MAX;
    for (uint32_t f=0;f<lowered->module.function_count;++f) {
        const XrXirFunction *body=&lowered->module.functions[f];
        if (body->name_length==3 && !memcmp(body->name,"run",3)) { CHECK(run==UINT32_MAX);run=f; }
    }
    CHECK(run!=UINT32_MAX);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    for (unsigned repeat=0;repeat<2;++repeat) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        for (unsigned phase=0;phase<2;++phase) {
            CHECK(xr_xir_instance_start(instance,phase?run:entry,NULL,0)==XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
            XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
            CHECK(value.type==XR_XIR_I64 && value.payload==(phase?42u:0u));xr_xir_value_drop(&value);
        }
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
}
static void library_slot_fact_controls(void) {
    XrXirCompileContext producer,receiver;context_new(&producer);context_new(&receiver);
    const char *names[]={"base","step","left","right"};
    const char text[]="const base:i64=17;var step:i64=25;const (left,right)=(19,23);"
        "export fn run()->i64{return base+step+left+right-42}";
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=program_source(&producer,text,&source,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"slot facts Source %u: %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    CHECK(source.checked->module.declarations->slot_count==4);
    library_slot_names(source.checked->construction,names,4);library_slot_raw_controls(&receiver,source.checked);
    XrXirEffects *effects=NULL;
    const XirFixtureEffectInput effect_input={&source.checked->module,source.checked->construction};
    CHECK(xir_fixture_effects_infer(&receiver,effect_input,&effects)==XR_XIR_OK && effects);
    XrXirSlotFact rows[4];char bytes[4][8];
    const XrXirSlotFacts *original=xr_xir_compile_construction_slots(source.checked->construction);
    for (uint32_t s=0;s<4;++s) {
        rows[s]=original->rows[s];CHECK(rows[s].name_length<=sizeof(bytes[s]));
        memcpy(bytes[s],rows[s].name,rows[s].name_length);rows[s].name=bytes[s];
    }
    XrXirSlotFacts input={rows,4};XrXirConstructionSpec spec={0};XrXirConstruction *owned=NULL;
    CHECK(xr_xir_compile_construction_new_v4(&receiver,source.checked->module.types,&spec,&input,&owned)==XR_XIR_OK);
    memset(rows,0xa5,sizeof(rows));memset(bytes,0xa5,sizeof(bytes));library_slot_names(owned,names,4);
    XrXirArtifact *received=NULL;
    CHECK(xr_xir_compile_recheck_v2(&receiver,&source.checked->module,owned,&received,NULL)==XR_XIR_OK);
    XrXirSourceSnapshot *snapshot=NULL;
    CHECK(xr_xir_compile_source_snapshot_copy_v2(&receiver,xr_xir_compile_source_snapshot_view(source.snapshot),owned,&snapshot)==XR_XIR_OK);
    xr_xir_compile_construction_free(owned);xr_xir_compile_source_result_free(&source);
    xr_compile_resources_release(producer.resources);
    /* This compares the existing owned Module representation; the current
     * inference entry does not consume the borrowed Construction. */
    CHECK(xir_effects_source_body_match(&receiver,&received->module,effects)==XR_XIR_OK);
    xr_xir_compile_effects_free(effects);
    library_slot_names(xr_xir_compile_source_snapshot_construction(snapshot),names,4);
    library_slot_names(received->construction,names,4);CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0},again={0};CHECK(xr_xir_compile_checked_write(received,&packet,NULL)==XR_XIR_OK);
    library_slot_wire_controls(&receiver,&packet,xr_xir_compile_construction_slots(received->construction));
    xr_xir_compile_artifact_free(received);received=NULL;
    CHECK(xr_xir_compile_checked_read(&receiver,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(received,&again,NULL)==XR_XIR_OK && packet.length==again.length &&
        !memcmp(packet.bytes,again.bytes,packet.length));
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_checked_packet_free(&again);
    CHECK(!received->construction->count && !received->construction->materialization_count);
    library_slot_program(received,names,4);xr_xir_compile_source_snapshot_free(snapshot);
    const XrXirSlotFacts zero={NULL,0};owned=NULL;
    CHECK(!xr_xir_compile_construction_slots(NULL));
    CHECK(xr_xir_compile_construction_new_v4(&receiver,NULL,&spec,&zero,&owned)==XR_XIR_OK);
    library_slot_names(owned,NULL,0);xr_xir_compile_construction_free(owned);
    xr_compile_resources_release(receiver.resources);
    fprintf(stderr,"Complete slot facts: actual mutable/const/destructure, two receiving ledgers, poisoned input, snapshot/wire/projection/VM42 and raw rejects PASS\n");
}
static void library_slot_catalog_controls(void) {
    XrXirCompileContext context;context_new(&context);
    XrXirSourceResult source={0},consumer={0};
    const char library[]="const base:i64=17;export fn answer()->i64{return base+25}";
    CHECK(library_comptime_named_source(&context,"slot-owner.xr",library,true,NULL,&source)==XR_XIR_OK);
    const char *names[]={"base"};library_slot_names(source.checked->construction,names,1);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,MATERIALIZATION_ROOT};
    XrXirLibraryModuleInput binding={authority,"slot-owner.xr"};
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(input.packet,input.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(&context,&input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);memset(&input,0,sizeof(input));
    const char use[]="import {answer as renamed} from \"./slot-owner\";export fn run()->i64{return renamed()}";
    CHECK(library_comptime_named_source(&context,"slot-use.xr",use,false,catalog,&consumer)==XR_XIR_OK);
    library_slot_names(consumer.checked->construction,names,1);
    XrXirSourceResult rejected={0};
    const char private_use[]="import {base} from \"./slot-owner\";export fn run()->i64{return base}";
    CHECK(library_comptime_named_source(&context,"slot-private.xr",private_use,false,catalog,&rejected)!=XR_XIR_OK && !rejected.checked);
    xr_xir_compile_source_result_free(&rejected);xr_xir_compile_library_catalog_free(catalog);
    XrXirArtifact *owned=consumer.checked;consumer.checked=NULL;xr_xir_compile_source_result_free(&consumer);
    library_slot_program(owned,names,1);
    XrXirSourceDiagnostic diagnostic={0};
    CHECK(program_source(&context,"export const value:i64=42",&rejected,&diagnostic)==XR_XIR_BAD_STRUCTURE && !rejected.checked);
    CHECK(strstr(diagnostic.message,"exported state is not admitted"));xr_xir_compile_source_result_free(&rejected);
    xr_compile_resources_release(context.resources);
    fprintf(stderr,"Slot facts: actual Catalog private storage, selected function alias, retired packet/catalog, VM42 and private/export-state refusals PASS\n");
}
static void library_slot_source_import_controls(void) {
    XrXirCompileContext context;context_new(&context);char root[1024],peer[1024];
    CHECK(snprintf(root,sizeof(root),"%s/slot-source-root.xr",MATERIALIZATION_ROOT)>0);
    CHECK(snprintf(peer,sizeof(peer),"%s/slot-source-peer.xr",MATERIALIZATION_ROOT)>0);
    char root_text[]="import \"./slot-source-peer\" as peer;fn main()->i64{return 7};export fn run()->i64{return peer.value()}";
    char peer_text[]="fn start()->i64{return 40};const state=start();export fn value()->i64{return state+2}";
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,MATERIALIZATION_ROOT};
    XrModuleOverlayInput overlays[]={{authority,"slot-source-root.xr",root,root_text,sizeof(root_text)-1},
        {authority,"slot-source-peer.xr",peer,peer_text,sizeof(peer_text)-1}};
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrXirSourceRequest request={session,root,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_overlay(&request,overlays,2,&source,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"Source import slot status%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.checked->module.declarations->module_count>=2);
    const char *const names[]={"state"};library_slot_names(source.checked->construction,names,1);
    XrXirArtifact *owned=source.checked;source.checked=NULL;xr_xir_compile_source_result_free(&source);
    xr_compile_session_free(session);memset(root_text,0xa5,sizeof(root_text));memset(peer_text,0xa5,sizeof(peer_text));
    memset(overlays,0xa5,sizeof(overlays));library_slot_program(owned,names,1);xr_compile_resources_release(context.resources);
    fprintf(stderr,"Two real Source namespaces preserve imported private state after producer retirement; module initializers and run42 PASS\n");
}
typedef struct LibrarySlotAst {
    XrCompilerSession *session;
    XrXirSourceAst input;
    unsigned references;
} LibrarySlotAst;
static XrXirStatus library_slot_ast_retain(void *opaque,const XrXirSourceAst *input) {
    LibrarySlotAst *owner=opaque;
    if (!owner || !owner->session || !owner->references || !input || input->program!=owner->input.program)
        return XR_XIR_BAD_STRUCTURE;
    if (owner->references==UINT_MAX) return XR_XIR_BUDGET;
    ++owner->references;return XR_XIR_OK;
}
static void library_slot_ast_release(void *opaque) {
    LibrarySlotAst *owner=opaque;CHECK(owner && owner->references);
    if (!--owner->references) {
        xr_program_destroy(owner->input.program);owner->input.program=NULL;
        xr_compile_session_free(owner->session);owner->session=NULL;
    }
}
static uint64_t library_slot_repl_prepare(XrXirSourceGenerations *generations,
    XrXirCompileContext *context,LibrarySlotAst *owner,const char *text) {
    CHECK(!owner->session && !owner->references);
    CHECK(xr_compile_session_new(context->resources,&owner->session)==XR_COMPILER_SESSION_OK);
    owner->references=1;owner->input.source=(XrXirSourceText){NULL,text,strlen(text)};
    CHECK(xr_compile_parse_with_source(owner->session,text,NULL,&owner->input.program)==XR_PARSE_OK);
    XrXirSourceAstOwner syntax={owner->input,context->resources,owner,library_slot_ast_retain,library_slot_ast_release};
    XrXirSourceReplInput input={.ast=&syntax};uint64_t serial=0;XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_generation_prepare_repl_ast_bindings(generations,&input,&serial,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"slot history %u: %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK && serial);library_slot_ast_release(owner);return serial;
}
static void library_slot_history_controls(void) {
    XrXirCompileContext context,receiver;context_new(&context);context_new(&receiver);
    XrXirSourceGenerationConfig config={.context=context,.namespace_id="complete-slot-history"};
    XrXirSourceGenerations *owner=NULL;CHECK(xr_xir_compile_source_generations_new(&config,&owner)==XR_XIR_OK);
    LibrarySlotAst first={0},second={0};
    uint64_t one=library_slot_repl_prepare(owner,&context,&first,
        "const base:i64=17;export fn plus(value:i64)->i64{return value+base};plus(25)");
    XrXirSourceGenerationView birth={0},next={0};
    CHECK(xr_xir_compile_source_generation_view(owner,one,&birth)==XR_XIR_OK);
    const XrXirArtifact *original=birth.result->checked;
    const XrXirSlotFacts *facts=xr_xir_compile_construction_slots(original->construction);
    bool function=false,result=false,base=false;
    char result_name[64],function_prefix[64];
    CHECK(snprintf(result_name,sizeof(result_name),"$result%llu",(unsigned long long)one)>0);
    CHECK(snprintf(function_prefix,sizeof(function_prefix),"$function%llu_",(unsigned long long)one)>0);
    for (uint32_t s=0;s<facts->count;++s) {
        const XrXirSlotFact *row=&facts->rows[s];CHECK(row->slot==s && !row->exported);
        base|=row->name_length==4 && !memcmp(row->name,"base",4);
        function|=row->name_length>strlen(function_prefix) && !memcmp(row->name,function_prefix,strlen(function_prefix));
        result|=row->name_length==strlen(result_name) && !memcmp(row->name,result_name,row->name_length);
    }
    CHECK(base && function && result);
    /* This publication checks compiler metadata receiving only. It does not
     * claim Program binding, Instance publication or historical storage reuse. */
    CHECK(xr_xir_compile_source_generation_publish(owner,one)==XR_XIR_OK);
    uint64_t two=library_slot_repl_prepare(owner,&context,&second,"const next:i64=base+25;plus(next)");
    CHECK(xr_xir_compile_source_generation_view(owner,two,&next)==XR_XIR_OK && next.parent==one);
    const XrXirArtifact *received=next.result->checked;
    const XrXirSlotFacts *incoming=xr_xir_compile_construction_slots(received->construction);
    for (uint32_t s=0;s<facts->count;++s) {
        const XrXirSlotFact *row=&facts->rows[s];unsigned matches=0;
        const XrXirSlot *slot=&original->module.declarations->slots[s];
        const XrXirSourceModule *module=&original->module.declarations->modules[slot->module];
        for (uint32_t t=0;t<incoming->count;++t) {
            const XrXirSlotFact *candidate=&incoming->rows[t];
            const XrXirSlot *target=&received->module.declarations->slots[t];
            const XrXirSourceModule *target_module=&received->module.declarations->modules[target->module];
            if (row->name_length!=candidate->name_length || memcmp(row->name,candidate->name,row->name_length) ||
                module->name_length!=target_module->name_length || memcmp(module->name,target_module->name,module->name_length)) continue;
            CHECK(candidate->slot==t && candidate->exported==row->exported && target->mutable==slot->mutable);++matches;
        }
        CHECK(matches==1);
    }
    XrXirSourceSnapshot *snapshot=NULL;
    CHECK(xr_xir_compile_source_snapshot_copy_v2(&receiver,xr_xir_compile_source_snapshot_view(next.result->snapshot),
        xr_xir_compile_source_snapshot_construction(next.result->snapshot),&snapshot)==XR_XIR_OK);
    uint32_t count=incoming->count;xr_xir_compile_source_generations_free(owner);
    CHECK(!first.session && !second.session);
    const XrXirSlotFacts *retained=xr_xir_compile_construction_slots(xr_xir_compile_source_snapshot_construction(snapshot));
    CHECK(retained && retained->count==count);
    for (uint32_t s=0;s<count;++s) CHECK(retained->rows[s].slot==s && retained->rows[s].name_length && !retained->rows[s].exported);
    xr_xir_compile_source_snapshot_free(snapshot);xr_compile_resources_release(receiver.resources);xr_compile_resources_release(context.resources);
    fprintf(stderr,"Complete slot metadata: actual hidden function/result births and historical module relocation survive parser/generation retirement; runtime receiving NOT_TESTED\n");
}
static void library_slot_forward_controls(void) {
    const char *texts[]={
        "const seed:i64=17;fn make<T>()->i64{return comptime {const unused:Array<T>=[];len(unused)+42}};export fn run()->i64{return seed+make<i8>()-17}",
        "const seed:i64=41;export fn run()->i64{const xs:Array<i64>=[1];const part:Slice<i64>=xs[0:1];return seed+len(part)}"};
    const char *names[]={"seed"};
    for (unsigned kind=0;kind<2;++kind) {
        XrXirCompileContext context;context_new(&context);XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=program_source(&context,texts[kind],&source,&diagnostic);
        if (kind) {
            /* This real view input is still unadmitted. Preserve its existing
             * body rejection, rather than failing earlier for absent facts. */
            CHECK(status==XR_XIR_BAD_TYPE && !source.checked && !source.snapshot);
            CHECK(strstr(diagnostic.message,"constructed XIR failed checking at function 2 block 0 instruction 1"));
            xr_xir_compile_source_result_free(&source);xr_compile_resources_release(context.resources);continue;
        }
        if (status!=XR_XIR_OK) fprintf(stderr,"slot forwarding %u status%u: %s\n",kind,status,diagnostic.message);
        CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
        library_slot_names(source.checked->construction,names,1);
        CHECK(source.checked->construction->comptime.site_count && source.checked->construction->comptime.binding_count);
        XrXirArtifact *owned=source.checked;source.checked=NULL;xr_xir_compile_source_result_free(&source);
        library_slot_program(owned,names,1);xr_compile_resources_release(context.resources);
    }
    fprintf(stderr,"Slotful Source literal/comptime seal preserves complete facts through specialization/lower and VM42; existing view body rejection retained, view execution NOT_TESTED\n");
}
static void library_comptime_controls(void) {
    library_slot_fact_controls();
    library_slot_catalog_controls();
    library_slot_source_import_controls();
    library_slot_forward_controls();
    XrXirCompileContext context;context_new(&context);XrXirArtifact *source=NULL,*received=NULL;
    CHECK(library_comptime_operation(&context,"fresh",&source)==XR_XIR_OK);
    uint32_t provider=source->construction->comptime.bindings[0].provider;bool literal=false;
    CHECK(xir_literal_type_names(&context,&source->module,source->construction,provider,&literal)==XR_XIR_OK && literal);
    XrXirComptimeTable unbound=source->construction->comptime;
    CHECK(unbound.binding_count==2);unbound.bindings+=1;--unbound.binding_count;
    XrXirConstructionSpec spec={source->construction->rows,source->construction->count,source->construction->materializations,
        source->construction->materialization_count,&unbound};XrXirConstruction *removed=NULL;
    CHECK(xr_xir_compile_construction_new_v4(&context,source->module.types,&spec,xr_xir_compile_construction_slots(source->construction),&removed)==XR_XIR_OK);
    literal=true;CHECK(xir_literal_type_names(&context,&source->module,removed,provider,&literal)==XR_XIR_OK && !literal);
    CHECK(xr_xir_compile_recheck_v2(&context,&source->module,removed,&received,NULL)==XR_XIR_BAD_TYPE && !received);
    xr_xir_compile_construction_free(removed);
    XrXirStatus status=XR_XIR_OK;
    XrXirFunction *functions=xir_compile_copy(&context,source->module.functions,(size_t)source->module.function_count*sizeof(*functions),&status);
    XrXirFunctionIdentity *identities=xir_compile_copy(&context,source->module.declarations->functions,
        (size_t)source->module.function_count*sizeof(*identities),&status);CHECK(functions && identities && status==XR_XIR_OK);
    XrXirDeclarations declarations=*source->module.declarations;declarations.functions=identities;
    XrXirModule current=source->module;current.functions=functions;current.declarations=&declarations;
    identities[provider].exported=1;literal=true;
    CHECK(xir_literal_type_names(&context,&current,source->construction,provider,&literal)==XR_XIR_OK && !literal);
    CHECK(xr_xir_compile_recheck_v2(&context,&current,source->construction,&received,NULL)!=XR_XIR_OK && !received);
    identities[provider].exported=0;
    XrXirType nominal=XR_XIR_UNIT,array=source->module.functions[provider].result;
    for (uint32_t t=0;t<source->module.types->count;++t) {
        const XrXirTypeNode *node=&source->module.types->nodes[t];
        if (node->kind==XR_XIR_TYPE_NOMINAL) nominal=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
    }
    CHECK(nominal!=XR_XIR_UNIT && xr_xir_array_element(source->module.types,array)==nominal);
    XrXirInstruction ops[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},7,{0}},
        {XR_XIR_STRUCT_NEW,nominal,{0,1},{0},0,{0}},
        {XR_XIR_ARRAY_NEW,array,{1,1},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}}};
    uint32_t operands[]={0,1};XrXirBlock block={0,4,0,0};
    functions[provider].instructions=ops;functions[provider].instruction_count=4;
    functions[provider].blocks=&block;functions[provider].block_count=1;
    functions[provider].operands=operands;functions[provider].operand_count=2;literal=false;
    CHECK(xir_literal_type_names(&context,&current,source->construction,provider,&literal)==XR_XIR_OK && literal);
    CHECK(xr_xir_compile_recheck_v2(&context,&current,source->construction,&received,NULL)==XR_XIR_BAD_TYPE && !received);
    ops[1].op=XR_XIR_STRUCT_GET;literal=true;
    CHECK(xir_literal_type_names(&context,&current,source->construction,provider,&literal)==XR_XIR_OK && !literal);
    xr_compile_resources_free(identities);xr_compile_resources_free(functions);
    CHECK(xr_xir_compile_artifact_verify(source,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(source);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Library opaque names: private data recipe only, exported/field-read refuse, literal construction still needs actual owner, physical0\n");
}

static void library_comptime_site_controls(void) {
    library_slot_history_controls();
    XrXirCompileContext context;context_new(&context);XrXirArtifact *source=NULL,*checked=NULL;
    CHECK(pending_source(&context,"array",2,&source)==XR_XIR_OK);
    XirComptimePending *pending=NULL;CHECK(xr_xir_compile_comptime_pending(source,&pending,NULL)==XR_XIR_OK && pending);
    CHECK(xr_xir_compile_comptime_pending_specialize(pending,&checked,NULL)==XR_XIR_OK);
    const XrXirProvenance *proof=checked->module.provenance;const XirEvaluationRoot *root=&proof->evaluation;
    XrXirComptimeSite site={proof->origins[root->caller].function,root->instruction,proof->origins[root->callee].function};
    XirCtfeInstanceBatchRequest evaluation=instance_batch_request(checked,&root->callee,1);
    XirCtfeInstanceBatch *batch=NULL;CHECK(xr_xir_compile_ctfe_instance_batch(&evaluation,&batch)==XR_XIR_CTFE_OK);
    XirLiteralRequest request={&context,batch,1};XirLiteralDefinitions output={0};XrCompileResourceStats before=stats(&context);
    CHECK(xr_xir_compile_instance_literals_at(&request,NULL,&output,NULL)==XR_XIR_BAD_STRUCTURE);
    XrXirComptimeSite wrong=site;++wrong.instruction;
    CHECK(xr_xir_compile_instance_literals_at(&request,&wrong,&output,NULL)==XR_XIR_BAD_STRUCTURE);
    wrong=site;wrong.helper=wrong.caller;
    CHECK(xr_xir_compile_instance_literals_at(&request,&wrong,&output,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!output.definition && !output.bindings && !output.count && stats(&context).live_bytes==before.live_bytes);
    CHECK(xr_xir_compile_instance_literals_at(&request,&site,&output,NULL)==XR_XIR_OK);
    const XrXirComptimeTable *table=&output.definition->construction->comptime;
    CHECK(table->site_count==1 && table->binding_count==1 && table->bindings[0].provider==output.bindings[0].provider);
    CHECK(xr_xir_compile_artifact_verify(output.definition,NULL)==XR_XIR_OK);
    xr_xir_compile_instance_literals_free(&output);xr_xir_compile_ctfe_instance_batch_free(batch);
    xr_xir_compile_artifact_free(checked);xr_xir_compile_comptime_pending_free(pending);xr_xir_compile_artifact_free(source);
    checked=instance_source(&context,"array");uint32_t function=instance_named(checked,"array",0);
    evaluation=instance_batch_request(checked,&function,1);batch=NULL;
    CHECK(xr_xir_compile_ctfe_instance_batch(&evaluation,&batch)==XR_XIR_CTFE_OK);request.instances=batch;
    before=stats(&context);CHECK(xr_xir_compile_instance_literals_at(&request,&site,&output,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!output.definition && !output.bindings && !output.count && stats(&context).live_bytes==before.live_bytes);
    xr_xir_compile_ctfe_instance_batch_free(batch);xr_xir_compile_artifact_free(checked);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Library site receiving: genuine exact pending root, no missing/wrong site or ordinary INSTANCE authority, atomic prefixes physical0\n");
}
