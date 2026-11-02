/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_class_root_fields_controls.h - Real root constructor frontiers
 *
 * KEY CONCEPT:
 *   A compiler-owned callback tests coordinates without granting an equation.
 *   Every negative keeps the actual ProgramInput and original VM body intact.
 */
#ifndef TEST_XIR_CLASS_ROOT_FIELDS_CONTROLS_H
#define TEST_XIR_CLASS_ROOT_FIELDS_CONTROLS_H
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_source.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_call_internal.h"

static const char class_root_fields_source[]=
    "class Item{value:i64\ninit(value:i64){this.value=value}\n}\n"
    "class Box{item:Item\ninit(item:Item){this.item=item}\n}\n"
    "export fn rootFieldsProbe()->i64{var item=Item(42);var box=Box(item);return box.item.value}\n";
typedef struct ClassRootFieldsControl {
    XrXirProgram *program;
    XrXirResumeEntry resume;
    uint32_t constructor,helper,other,entry,mode,calls,staged;
} ClassRootFieldsControl;
static ClassRootFieldsControl class_root_fields_control;

static XrXirAction class_root_fields_resume(XrXirCallView *view) {
    ClassRootFieldsControl *test=&class_root_fields_control;
    XrXirCallFrontier frontier={0};XrXirValue unit={0};
    CHECK(view->phase==XR_XIR_CALL_NORMAL && view->argument_count==1);
    CHECK(!xr_xir_call_invocation_pending(view,&frontier));++test->calls;
    if(test->calls==1) {
        XirEffectInvocationSelection unowned={.caller=test->constructor,
            .instruction=0,.target=test->helper,.producer=UINT32_MAX};
        CHECK(xr_xir_call_invocation_stage(view,&unowned)==XR_XIR_CALL_BAD_STATE);
        if(test->mode==1) {
            XrXirProgramInput *input=test->program->permissions->input;
            CHECK(input);test->program->permissions->input=NULL;
            CHECK(xr_xir_instance_call_at(view,0,test->helper,view->arguments,1)==XR_XIR_CALL_BAD_STATE);
            test->program->permissions->input=input;
        } else if(test->mode==2) {
            CHECK(xr_xir_instance_call_at(view,0,test->other,view->arguments,1)==XR_XIR_CALL_BAD_ARGUMENT);
            CHECK(xr_xir_instance_call_at(view,1,test->helper,view->arguments,1)==XR_XIR_CALL_BAD_ARGUMENT);
        } else if(test->mode==3) {
            XirEffectInvocationSelection forged={.owner=test->program->permissions->effects,
                .caller=test->constructor,.instruction=0,.target=test->helper,.producer=0};
            CHECK(xr_xir_call_invocation_stage(view,&forged)==XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_call_entry_admit(view,test->helper,&unit,view->arguments,1)==XR_XIR_CALL_BAD_STATE);
            ++test->staged;
            return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=test->helper,
                .arguments=view->arguments,.argument_count=1};
        } else if(test->mode==4) {
            CHECK(xr_xir_instance_call_entry_admit(view,test->helper,&unit,view->arguments,1)==XR_XIR_CALL_BAD_STATE);
            return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=test->helper,
                .arguments=view->arguments,.argument_count=1};
        }
    }
    XrXirAction action=test->resume(view);
    if(action.kind==XR_XIR_ACTION_CALL && action.callee==test->helper) {
        CHECK(xr_xir_call_invocation_pending(view,&frontier));
        CHECK(frontier.instruction==0 && frontier.target==test->helper && frontier.producer==UINT32_MAX);
        CHECK(xr_xir_instance_call_at(view,0,test->helper,action.arguments,action.argument_count)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_call_entry_admit(view,test->other,&unit,action.arguments,action.argument_count)==XR_XIR_CALL_BAD_STATE);
        XrXirCallFrontier after={0};CHECK(xr_xir_call_invocation_pending(view,&after));
        CHECK(after.instruction==frontier.instruction && after.target==frontier.target && after.producer==frontier.producer);
        ++test->staged;
    }
    return action;
}
static uint32_t class_root_fields_owner(const XrXirNominalTable *table,const char *name) {
    uint32_t found=UINT32_MAX;size_t length=strlen(name);
    for(uint32_t n=0;n<table->count;++n) {
        XrXirLiteral actual=table->declarations?table->declarations[n].name:table->identities[n].name;
        if(actual.length!=length || memcmp(actual.bytes,name,length))continue;
        CHECK(found==UINT32_MAX);found=n;
    }
    CHECK(found!=UINT32_MAX);return found+1;
}
static XrXirProgram *class_root_fields_program(XrXirCompileContext *context) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"class-root-fields",NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=context,.linkage_kind=XR_XIR_PROGRAM};
    XrXirSourceText text={.text=class_root_fields_source,.length=sizeof(class_root_fields_source)-1};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&text,&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"root Fields Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    XrXirArtifact *checked=source.checked;source.checked=NULL;
    xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
    XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    XrXirTarget target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    CHECK(program->permissions && program->permissions->input && program->permissions->input->artifact);
    const XrXirModule *module=&program->permissions->input->artifact->module;
    uint32_t box=class_root_fields_owner(module->types->nominals,"Box");
    uint32_t item=class_root_fields_owner(module->types->nominals,"Item");
    class_root_fields_control=(ClassRootFieldsControl){.program=program,.constructor=UINT32_MAX,.helper=UINT32_MAX,.other=UINT32_MAX,.entry=UINT32_MAX};
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunctionIdentity *identity=module->declarations->functions+f;
        const XrXirFunction *body=module->functions+f;
        if(body->name_length==15 && !memcmp(body->name,"rootFieldsProbe",15)) {
            CHECK(identity->exported && !identity->nominal_owner && class_root_fields_control.entry==UINT32_MAX);
            class_root_fields_control.entry=f;
        }
        if(identity->nominal_owner==box && identity->method_kind==XR_XIR_CONSTRUCTOR)class_root_fields_control.constructor=f;
        if(identity->nominal_owner==box && identity->method_kind==XR_XIR_CONSTRUCTOR_FIELDS)class_root_fields_control.helper=f;
        if(identity->nominal_owner==item && identity->method_kind==XR_XIR_CONSTRUCTOR_FIELDS)class_root_fields_control.other=f;
    }
    ClassRootFieldsControl *test=&class_root_fields_control;
    CHECK(test->constructor!=UINT32_MAX && test->helper!=UINT32_MAX && test->other!=UINT32_MAX && test->entry!=UINT32_MAX);
    CHECK(module->functions[test->constructor].instructions[0].op==XR_XIR_CALL);
    CHECK(module->functions[test->constructor].instructions[0].immediate==test->helper);
    test->resume=program->entries[test->constructor].resume;
    program->entries[test->constructor].resume=class_root_fields_resume;
    return program;
}
static void class_test_root_fields(XrXirCompileContext *context) {
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t original_live=runtime_live;
    XrXirProgram *program=class_root_fields_program(context);ClassRootFieldsControl *test=&class_root_fields_control;
    size_t program_live=runtime_live;
    for(uint32_t mode=0;mode<5;++mode) {
        test->mode=mode;test->calls=0;test->staged=0;
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        XrXirValue host_argument={XR_XIR_I64,0,42};
        CHECK(program->entries[test->other].parameter_count==1 && program->entries[test->other].parameters[0]==XR_XIR_I64);
        CHECK(xr_xir_instance_start(instance,test->other,&host_argument,1)==XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_instance_start(instance,test->entry,NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult result={0};uint32_t polls=0;
        do {CHECK(++polls<1024);result=xr_xir_instance_poll_bounded(instance,64);}while(result.outcome.status==XR_XIR_CALL_READY);
        CHECK(result.outcome.status==(mode<3?XR_XIR_CALL_RETURNED:XR_XIR_CALL_BAD_STATE));
        if(!test->calls || test->staged!=(mode==4?0u:1u))fprintf(stderr,"root Fields mode%u status%u calls%u staged%u\n",mode,result.outcome.status,test->calls,test->staged);
        CHECK(test->calls && test->staged==(mode==4?0u:1u));
        if(mode<3) {
            XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
            CHECK(value.type==XR_XIR_I64 && value.payload==42 && test->calls>=2);xr_xir_value_drop(&value);
        } else CHECK(result.outcome.value.type==XR_XIR_UNIT && !result.outcome.value.payload && !result.outcome.panic.detail.code);
        XrXirCallStatus released=xr_xir_instance_free(instance);
        CHECK(released==(mode<3?XR_XIR_CALL_READY:XR_XIR_CALL_BAD_STATE));
        if(runtime_live!=program_live)fprintf(stderr,"root Fields mode%u live%zu program%zu\n",mode,runtime_live,program_live);
        CHECK(runtime_live==program_live);
    }
    program->entries[test->constructor].resume=test->resume;xr_xir_compile_program_drop(program);
    CHECK(runtime_live==original_live);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
    memset(test,0,sizeof(*test));
    puts("Root Fields: real class parameter, noInput/host/target/producer/frontier refusals, one call and retired pending PASS");
}
#endif // TEST_XIR_CLASS_ROOT_FIELDS_CONTROLS_H
