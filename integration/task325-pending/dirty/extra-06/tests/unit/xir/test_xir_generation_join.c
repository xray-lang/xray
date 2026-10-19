/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generation_join.c - Actual generation receivers and live VM ancestors
 */
#include "xir/xxir_source_generation.h"
#include "xir/xxir_instance_generation.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_instance_debug_internal.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_source_fixture_owner.h"

static XrXirProgram *join_program(const XrXirArtifact *checked,const char *name,uint32_t *entry) {
    XrXirArtifact *closed=NULL,*lowered=NULL;XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);bool found=false;
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->declarations->functions[f].module==module->declarations->root_module &&
           module->functions[f].name_length==strlen(name) && !memcmp(module->functions[f].name,name,strlen(name))) {
            CHECK(!found);*entry=f;found=true;
        }
    XrXirStatus status=xr_xir_compile_vm_program_take(&lowered,&program);
    if(status!=XR_XIR_OK) fprintf(stderr,"generation join Program take=%u found=%u\n",status,found?1u:0u);
    CHECK(found && status==XR_XIR_OK && !lowered);
    return program;
}
static XrXirValue join_value(XrXirInstance *instance,uint32_t entry,const XrXirValue *arguments,uint32_t count) {
    CHECK(xr_xir_instance_start(instance,entry,arguments,count)==XR_XIR_CALL_READY);
    XrXirInstanceResult result={0};
    for(unsigned i=0;i<1024;++i) {
        result=xr_xir_instance_poll_bounded(instance,256);
        if(result.outcome.status!=XR_XIR_CALL_READY) break;
    }
    if(result.outcome.status!=XR_XIR_CALL_RETURNED)
        fprintf(stderr,"generation join outcome=%u\n",result.outcome.status);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    return value;
}
static void join_return(XrXirInstance *instance,uint32_t entry,int64_t expected) {
    XrXirValue value=join_value(instance,entry,NULL,0);
    CHECK(value.type==XR_XIR_I64 && value.payload==expected);xr_xir_value_drop(&value);
}
#include "xir_generation_join_fixture.h"
static void join_iterator(void) {
    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    uint32_t entry=0;XrXirProgram *program=join_iterator_program(&compiler.context,&entry);
    XrXirInstanceConfig runtime;CHECK(xr_xir_instance_config_init(&runtime,sizeof(runtime))==XR_XIR_CALL_READY);
    XrXirInstance *ordinary=NULL,*root=NULL;
    CHECK(xr_xir_instance_new(program,&runtime,&ordinary)==XR_XIR_CALL_READY);
    join_return(ordinary,entry,41);CHECK(xr_xir_instance_free(ordinary)==XR_XIR_CALL_READY);
    puts("ordinary worker Iterator returned 41");fflush(stdout);
    CHECK(xr_xir_instance_generation_new(&runtime,&root)==XR_XIR_CALL_READY);
    uint64_t candidate=0;CHECK(xr_xir_instance_generation_prepare(root,program,&candidate)==XR_XIR_CALL_READY);
    join_return(root,entry,41);
    CHECK(xr_xir_instance_generation_publish(root,candidate)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(root)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);source_fixture_owner_free(&compiler);
    puts("generation worker Iterator returned 41");
}

enum {JOIN_ENTRIES=64,JOIN_FRAMES=64};
typedef struct JoinCode {XrXirProgram *program;XrXirResumeEntry resume[JOIN_ENTRIES];} JoinCode;
typedef struct JoinFrame {uint64_t id,parent;uint32_t entry;const XrXirTypeArena *arena;XrXirCallCodeKind code;bool left;} JoinFrame;
static JoinCode join_code[2];
static JoinFrame join_frames[JOIN_FRAMES];
static unsigned join_frame_count,join_same,join_foreign,join_bad_event;
static void join_event(void *owner,const XrXirCallEvent *event) {
    CHECK(owner==join_frames && event && event->frame);
    JoinFrame *frame=NULL;
    for(unsigned i=0;i<join_frame_count;++i) if(join_frames[i].id==event->frame) frame=&join_frames[i];
    if(event->kind==XR_XIR_CALL_FRAME_ENTER || event->kind==XR_XIR_CALL_FRAME_PRESENT) {
        CHECK(!frame && join_frame_count<JOIN_FRAMES);
        join_frames[join_frame_count++]=(JoinFrame){.id=event->frame,.parent=event->parent,.entry=event->entry,.code=event->code};return;
    }
    CHECK(frame && !frame->left);
    if(event->kind==XR_XIR_CALL_FRAME_CODE) frame->code=event->code;
    if(event->kind==XR_XIR_CALL_FRAME_LEAVE) frame->left=true;
}
static const XrXirCallObserver join_observer={join_frames,join_event};
static XrXirAction join_resume(XrXirCallView *view) {
    CHECK(xr_xir_call_observer_bind(view,&join_observer)==XR_XIR_CALL_READY);
    uint32_t entry=xr_xir_call_current_entry(view->activation);JoinCode *code=NULL;
    for(unsigned i=0;i<2;++i) if(join_code[i].program && join_code[i].program->arena==view->arena) code=&join_code[i];
    CHECK(code && entry<JOIN_ENTRIES && code->resume[entry]);
    /* This trampoline invokes the unchanged admitted VM body. It never edits
     * a frame, view, metadata table, invocation proof or executor binding. */
    XrXirAction action=code->resume[entry](view);
    if(action.kind==XR_XIR_ACTION_FAULT && view->phase==XR_XIR_CALL_NORMAL)
        fprintf(stderr,"VM generation join fault entry=%u arena=%u status=%lld\n",entry,code==join_code?0u:1u,(long long)action.value.payload);
    if(view->phase!=XR_XIR_CALL_NORMAL || !xir_call_vm_view_current(view)) return action;
    JoinFrame *top=NULL;
    for(unsigned i=0;i<join_frame_count;++i) if(!join_frames[i].left) top=&join_frames[i];
    CHECK(top);top->arena=view->arena;
    if(top->entry!=entry) ++join_bad_event;
    top->entry=entry;
    for(JoinFrame *frame=top;frame;) {
        if(frame->code==XR_XIR_CALL_CODE_VM && frame->arena) {
            uint32_t actual=UINT32_MAX-1;XrXirCallCodeKind kind=XR_XIR_CALL_CODE_UNKNOWN;
            XrXirCallStatus vm=xir_call_vm_frame_entry(view,frame->id,&actual);
            if(frame->arena!=view->arena) {
                if(vm!=XR_XIR_CALL_BAD_STATE) fprintf(stderr,"foreign ancestor lookup=%u entry=%u\n",vm,actual);
                CHECK(vm==XR_XIR_CALL_BAD_STATE && actual==UINT32_MAX-1);
                CHECK(xir_call_debug_frame_entry(view,frame->id,&actual,&kind)==XR_XIR_CALL_BAD_STATE);
                CHECK(actual==UINT32_MAX-1 && kind==XR_XIR_CALL_CODE_UNKNOWN);
                XrXirValue value={0};CHECK(xir_instance_observe_slot(view,frame->id,0,&value)==XR_XIR_CALL_BAD_STATE);
                CHECK(!value.type && !value.reserved && !value.payload);++join_foreign;
            } else {
                CHECK(vm==XR_XIR_CALL_READY && actual==frame->entry);
                CHECK(xir_call_debug_frame_entry(view,frame->id,&actual,&kind)==XR_XIR_CALL_READY);
                CHECK(actual==frame->entry && kind==XR_XIR_CALL_CODE_VM);
                if(frame!=top) ++join_same;
            }
        }
        uint64_t parent=frame->parent;frame=NULL;
        for(unsigned i=0;i<join_frame_count;++i) if(join_frames[i].id==parent) frame=&join_frames[i];
    }
    return action;
}
static void join_observation(void) {
    const char *texts[]={
        "const value=41\nfn leaf()->i64{return value}\n"
        "fn same()->fn()->i64{return leaf}\nexport fn seed()->fn()->i64{return same()}",
        "struct Padding{flag:bool}\nconst value=\"shadow\"\nexport fn answer(callback:fn()->i64)->i64{return callback()}"};
    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    XrXirSourceGenerationConfig config={compiler.context,"generation-observation",NULL,NULL,NULL};
    XrXirSourceGenerations *source=NULL;CHECK(xr_xir_compile_source_generations_new(&config,&source)==XR_XIR_OK);
    XrXirInstanceConfig runtime;CHECK(xr_xir_instance_config_init(&runtime,sizeof(runtime))==XR_XIR_CALL_READY);
    XrXirInstance *root=NULL;CHECK(xr_xir_instance_generation_new(&runtime,&root)==XR_XIR_CALL_READY);
    XrXirValue saved={0};
    for(unsigned i=0;i<2;++i) {
        uint64_t serial=0;XrXirSourceDiagnostic diagnostic={0};
        CHECK(xr_xir_compile_source_generation_prepare(source,&(XrXirSourceText){NULL,texts[i],strlen(texts[i])},&serial,&diagnostic)==XR_XIR_OK);
        XrXirSourceGenerationView view={0};CHECK(xr_xir_compile_source_generation_view(source,serial,&view)==XR_XIR_OK);
        uint32_t entry=0;XrXirProgram *program=join_program(view.result->checked,i?"answer":"seed",&entry);
        join_code[i].program=program;CHECK(program->entry_count<JOIN_ENTRIES);
        for(uint32_t f=0;f<program->entry_count;++f) {join_code[i].resume[f]=program->entries[f].resume;program->entries[f].resume=join_resume;}
        CHECK(xr_xir_compile_source_generation_bind_program(source,serial,program)==XR_XIR_OK);
        uint64_t candidate=0;CHECK(xr_xir_source_generation_prepare_instance(source,serial,root,&candidate)==XR_XIR_CALL_READY);
        join_frame_count=0;memset(join_frames,0,sizeof(join_frames));
        XrXirValue value=join_value(root,entry,i?&saved:NULL,i?1:0);
        if(i) {CHECK(value.type==XR_XIR_I64 && value.payload==41);xr_xir_value_drop(&value);}
        else saved=value;
        CHECK(xr_xir_instance_generation_publish(root,candidate)==XR_XIR_CALL_READY);
        CHECK(xr_xir_compile_source_generation_publish(source,serial)==XR_XIR_OK);
    }
    CHECK(join_same && join_foreign && !join_bad_event);xr_xir_value_drop(&saved);
    CHECK(xr_xir_instance_free(root)==XR_XIR_CALL_READY);xr_xir_compile_source_generations_free(source);
    for(unsigned i=0;i<2;++i) xr_xir_compile_program_drop(join_code[i].program);
    source_fixture_owner_free(&compiler);printf("same-metadata ancestors=%u foreign-metadata refusals=%u\n",join_same,join_foreign);
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    if(!strcmp(argv[1],"iterator")) join_iterator();
    else {CHECK(!strcmp(argv[1],"observation"));join_observation();}
    return 0;
}
