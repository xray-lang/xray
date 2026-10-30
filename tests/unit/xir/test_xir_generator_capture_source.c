/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_capture_source.c - Complete captured producer environments cross real Source
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
#include "xir/xxir_effects.c"
#include "xir_final_origin_receiving_cases.h"
typedef struct CaptureSourceCase {const char *name,*body;bool cold,unknown;} CaptureSourceCase;
static const CaptureSourceCase capture_source_cases[]={
    {"physical_walk","fn keep(it:Iterator<i64>)->i64{return 17}\nconst original=make()\nconst first=fn()->i64{return keep(original)}\nconst second=fn()->i64{return first()}\nconst third=fn()->i64{return second()}\nreturn third()",true,false},
    {"physical_unknown_bound","fn keep(it:Iterator<i64>)->i64{return 17}\nconst original=make()\nconst first=fn()->i64{return keep(original)}\nconst second=fn()->i64{return first()}\nconst third=fn()->i64{return second()}\nfn invoke(f:fn()->i64)->i64{return f()}\nreturn invoke(third)",true,true},
    {"iterator_argument","const original=values()\nfn nested(it:Iterator<i64>)->Iterator<i64>{try{yield it.next()} catch(error){yield -2}}\nconst it=nested(original)\nreturn it.next()",false,false},
    {"iterator_capture","const original=values()\nfn nested()->Iterator<i64>{try{yield original.next()} catch(error){yield -2}}\nconst it=nested()\nreturn it.next()",false,false},
    {"callable_capture","const factory=fn()->Iterator<i64>{return values()}\nfn nested()->Iterator<i64>{const it=factory()\ntry{yield it.next()} catch(error){yield -2}}\nconst it=nested()\nreturn it.next()",false,false}
};
static uint32_t capture_source_function(const XrXirModule *module,const char *name) {
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(name) &&
            !memcmp(module->functions[f].name,name,strlen(name))) return f;
    return UINT32_MAX;
}
static void capture_source_facts(XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);XrXirEffects *effects=NULL;
    CHECK(xr_xir_compile_effects_analyze(artifact,&effects)==XR_XIR_OK);
    uint32_t worker=capture_source_function(module,"worker");CHECK(worker!=UINT32_MAX);
    XrXirEffectFacts facts=xr_xir_effects_read(effects,worker);
    CHECK(facts.status==XR_XIR_OK && !facts.root.requires_root && !facts.root.unresolved);
    xr_xir_compile_effects_free(effects);
}
/* A distinct callback retains complete independent SSA replay over the same
 * real immutable fields. It supplies neither expected rows nor permissions. */
static XrXirStatus capture_source_instruction(const EffectBodyRequest *request,uint32_t f,uint32_t i,
    XrXirInstruction *storage,const XrXirInstruction **output,EffectBodyInstructionFields *fields) {
    return effect_body_owned_instruction(request,f,i,storage,output,fields);
}
static XrXirStatus capture_source_replay(const XrXirCompileContext *context,XrXirEffects *effects,bool owned) {
    EffectBodyRequest body=effect_body_owned_request(context,effects->invocations->body);
    if(!owned)body.instruction=capture_source_instruction;
    return final_origin_receive(context,effects,&body);
}

/* Iterator discovery remains independent of declaration-based Error checking.
 * Both actual body readers preserve pending and discovered call inputs. */
static void capture_source_pending_calls(const XrXirCompileContext *context,XrXirEffects *effects) {
    uint32_t pending_total=0,known_total=0;
    for(uint32_t route=0;route<2;++route) {
        const EffectInvocationCertificate *certificate=effects->invocations;
        EffectBodyRequest body=effect_body_owned_request(context,certificate->body);
        if(route)body.instruction=capture_source_instruction;
        CHECK(local_errors_request(context,&body,NULL)==XR_XIR_OK);
        CHECK(final_origin_request_rows(&body)==XR_XIR_OK);
        EffectInvocationRead read={0};CHECK(effect_invocation_read_open(context,certificate,certificate->final,&read)==XR_XIR_OK);
        for(uint32_t n=0;n<read.final->count;++n) {
            EffectInvocationFlow replay={.read=&read,.node=n,.instruction=UINT32_MAX};
            CHECK(effect_invocation_origins_read(&replay,&read.body)==XR_XIR_OK);
            for(uint32_t i=0;i<replay.function->instruction_count;++i) {
                XrXirInstruction storage;const XrXirInstruction *op=NULL;
                CHECK(effect_invocation_flow_instruction(&replay,i,&storage,&op)==XR_XIR_OK);
                if(op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT)continue;
                bool pending=false;CHECK(effect_invocation_iterator_pending(&replay,op,&pending)==XR_XIR_OK);
                if(pending)++pending_total;else ++known_total;
            }
            xr_compile_resources_free(replay.memory);
        }
        effect_invocation_read_close(&read);
    }
    CHECK(pending_total && known_total);
    printf("CAPTURE_PENDING %u pending, %u discovered calls; current call inputs owned/delegated PASS\n",pending_total,known_total);
}

/* Mutate the published finite capture relation, not retired discovery rows.
 * Both body adapters exhaust their actual inputs before the authentic reader. */
static void capture_source_atoms(XrXirArtifact *artifact,bool pending) {
    const XrXirCompileContext *context=xr_xir_compile_artifact_context(artifact);XrXirEffects *effects=NULL;
    CHECK(xr_xir_compile_effects_analyze(artifact,&effects)==XR_XIR_OK && effects);
    EffectInvocationFinal *final=effects->invocations->final;
    uint32_t ordinal=UINT32_MAX;
    for(uint32_t p=0;p<final->capture.count;++p)
        if(final->capture.facts[p].word!=EFFECT_CAPTURE_FLAGS) {ordinal=p;break;}
    CHECK(ordinal!=UINT32_MAX);
    EffectCaptureFact saved=final->capture.facts[ordinal];
    EffectInvocationFinal dimensions=*final;dimensions.binding_count=UINT32_MAX;dimensions.site_count=UINT32_MAX;
    EffectInvocationRead invalid={.work=context,.final=&dimensions};EffectInvocationBasis basis={0};
    uint32_t atoms=73;XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(effect_invocation_producer_atoms(&invalid,&basis,&atoms)==XR_XIR_BUDGET && atoms==73);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes &&
        after.held_bytes==before.held_bytes);
    for(uint32_t route=0;route<2;++route) {
        CHECK(capture_source_replay(context,effects,route!=0)==XR_XIR_OK);
        for(uint32_t attack=0;attack<8;++attack) {
            EffectCaptureFact *fact=&final->capture.facts[ordinal];*fact=saved;
            uint32_t count=final->capture.count,capacity=final->capture.capacity;
            if(attack==0)fact->node=final->count;
            if(attack==1)fact->value=UINT32_MAX;
            if(attack==2)fact->word=EFFECT_CAPTURE_FLAGS;
            if(attack==3)fact->bit=64;
            if(attack==4)fact->rank=UINT64_MAX;
            if(attack==5) {
                memmove(fact,fact+1,(size_t)(count-ordinal-1)*sizeof(*fact));--final->capture.count;
            }
            if(attack==6)final->capture.count=capacity+1;
            if(attack==7) {final->capture.count=UINT32_MAX;final->capture.capacity=UINT32_MAX;}
            XrXirStatus status=capture_source_replay(context,effects,route!=0);
            if(status!=XR_XIR_BAD_STRUCTURE)fprintf(stderr,"Capture relation route%u attack%u status%u\n",route,attack,status);
            CHECK(status==XR_XIR_BAD_STRUCTURE);
            if(attack==5)memmove(fact+1,fact,(size_t)(count-ordinal-1)*sizeof(*fact));
            final->capture.count=count;final->capture.capacity=capacity;*fact=saved;
        }
        CHECK(capture_source_replay(context,effects,route!=0)==XR_XIR_OK);
    }
    if(pending)capture_source_pending_calls(context,effects);
    xr_xir_compile_effects_free(effects);
    puts("Captured Fn/Iterator Final relation: 8 attacks on owned and delegated receiving paths PASS");
}

static void capture_source_execute(XrXirArtifact **lowered) {
    const XrXirModule *module=xr_xir_compile_artifact_module(*lowered);
    uint32_t entry=capture_source_function(module,"run");CHECK(entry!=UINT32_MAX);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(lowered,&program)==XR_XIR_OK && !*lowered);
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=xr_xir_instance_poll_bounded(instance,1000000).outcome.status;
    if(status!=XR_XIR_CALL_RETURNED) fprintf(stderr,"Iterator consumption execution status%u\n",status);
    CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && value.payload==17);xr_xir_value_drop(&value);
    xr_xir_compile_program_drop(program);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
static void capture_source_check(const CaptureSourceCase *fixture,bool root,const char *c_path) {
    char text[4096];int length=snprintf(text,sizeof(text),
        "var state:i64=0\nfn values()->Iterator<i64>{yield %s}\n"
        "fn make()->Iterator<i64>{return values()}\n"
        "fn consume(it:Iterator<i64>)->i64{return it.next()}\n"
        "fn worker()->i64{try{%s} catch(error){return -99}}\nexport fn run() throws(Error)->i64{return try await go worker()}\nfn main(){}\n",
        root?"state":"17",fixture->body);
    CHECK(length>0 && (size_t)length<sizeof(text));
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generator-capture",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&owner.context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,text,(size_t)length},&source,&diagnostic,NULL);
    xr_compile_session_free(session);bool reject=(root && !fixture->cold) || fixture->unknown;
    XrXirStatus expected=reject?XR_XIR_BAD_TYPE:XR_XIR_OK;
    if(status!=expected) fprintf(stderr,"%s root%u expected%u status%u line%d: %s\n",
        fixture->name,root,expected,status,diagnostic.line,diagnostic.message);
    CHECK(status==expected);
    if(reject) {
        CHECK(!source.checked && !source.snapshot);
        CHECK(strstr(diagnostic.message,fixture->unknown?"GO target lacks proof for worker execution":
            "GO target requires the current instance root execution"));
    } else {
        CHECK(source.checked && source.snapshot);capture_source_facts(source.checked);capture_source_atoms(source.checked,fixture->cold);
        XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
        CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
        xr_xir_compile_source_result_free(&source);
        CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
        memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);capture_source_facts(decoded);
        CHECK(xr_xir_compile_specialize(decoded,&closed,NULL, NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
        CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}, NULL,&lowered,NULL)==XR_XIR_OK);
        xr_xir_compile_artifact_free(closed);CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
        XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"generator_capture",1048576,&c)==XR_XIR_OK);
        CHECK(c.text && c.length && !strstr(c.text,"({"));
        if(c_path) {
            FILE *file=fopen(c_path,"wb");CHECK(file);CHECK(fwrite(c.text,1,c.length,file)==c.length);
            CHECK(fprintf(file,"\nconst uint32_t generator_capture_source_entry=%u;\n",
                capture_source_function(xr_xir_compile_artifact_module(lowered),"run"))>0);
            const XrXirModule *module=xr_xir_compile_artifact_module(lowered);CHECK(module->function_count<=64);
            uint8_t targets[64]={0};
            for(uint32_t f=0;f<module->function_count;++f)for(uint32_t i=0;i<module->functions[f].instruction_count;++i) {
                const XrXirInstruction *op=&module->functions[f].instructions[i];
                if(op->op==XR_XIR_FUNCTION_REF) {CHECK(op->immediate>=0 && (uint64_t)op->immediate<module->function_count);targets[op->immediate]=1;}
            }
            CHECK(fprintf(file,"const uint32_t generator_capture_function_count=%u;\nconst uint8_t generator_capture_function_targets[]={",module->function_count)>0);
            for(uint32_t f=0;f<module->function_count;++f)CHECK(fprintf(file,"%s%u",f?",":"",targets[f])>0);
            CHECK(fputs("};\n",file)>=0 && !fclose(file));
        }
        xr_xir_compile_c_source_free(&c);
        fprintf(stderr,"Capture %s root%u: Check, complete packet, specialize, lower and C11 prefix PASS\n",fixture->name,root);
        capture_source_execute(&lowered);
    }
    xr_xir_compile_source_result_free(&source);source_fixture_owner_free(&owner);
    fprintf(stderr,"Consumption %s root%u %s PASS\n",fixture->name,root,reject?"rejected":"executed");
}
int main(int argc,char **argv) {
    CHECK(argc==2 || argc==3);const CaptureSourceCase *fixture=NULL;
    for(size_t i=0;i<sizeof(capture_source_cases)/sizeof(*capture_source_cases);++i)
        if(!strcmp(argv[1],capture_source_cases[i].name))fixture=&capture_source_cases[i];
    CHECK(fixture);
    capture_source_check(fixture,true,NULL);
    capture_source_check(fixture,false,argc==3?argv[2]:NULL);
    if(fixture->unknown)printf("Complete captured producer %s: original unresolved bound rejected in both Source controls PASS\n",fixture->name);
    else printf("Complete captured producer %s: ROOT %s, pure Source/C11/VM PASS\n",fixture->name,fixture->cold?"cold accepted":"rejected");
    return 0;
}
