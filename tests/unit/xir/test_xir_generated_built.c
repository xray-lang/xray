/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generated_built.c - Generated ownership and complete receiving
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while(0)
#include "xir_source_compile_owner.h"
#include "xir/xxir_generated.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_implementation.h"
static const XrCompileResourceLimits generated_caps={67108864,8388608,128000000};
typedef struct GeneratedFixture {
    char names[3][5],module_name[8],file[24],field[16];
    XrXirInstruction init,ops[3],entry[2];
    XrXirBlock blocks[3];
    XrXirFunction functions[3];
    XrXirFunctionIdentity identities[3];
    XrXirSourceModule source_module;
    XrXirDeclarations declarations;
    XrXirTypeNode cell;
    XrXirTypes types;
    XrXirNominalField member;
    XrXirNominalDeclaration nominal;
    XrXirNominalTable nominals;
    XrXirType argument,member_type;
    XrXirModule built;
    XrXirGeneratedOrigin sites[3][3];
    XrXirGeneratedFunctionOrigins origins[3];
} GeneratedFixture;
static XrCompileResourceStats generated_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static XrXirCompileContext generated_context(const XrCompileResourceLimits *caps) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();return context;
}
static void generated_fixture(GeneratedFixture *f,unsigned kind) {
    memset(f,0,sizeof(*f));memcpy(f->names[0],"init",5);memcpy(f->names[1],"run",4);memcpy(f->names[2],"main",5);
    memcpy(f->module_name,"user",5);memcpy(f->file,"user/derive.xr",15);memcpy(f->field,"Record.value",13);
    f->init=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->ops[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    f->ops[1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->entry[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=1};
    f->entry[1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    unsigned count=2;
    if(kind==2 || kind==3) {
        f->ops[2]=f->ops[1];count=3;
        f->ops[1]=(XrXirInstruction){.op=kind==2?XR_XIR_SUSPEND:XR_XIR_CELL_NEW,
            .type=kind==2?XR_XIR_UNIT:(XrXirType)256};
        if(kind==3)f->ops[2].args[0]=1;
    }
    f->blocks[0]=(XrXirBlock){0,1,0,0};f->blocks[1]=(XrXirBlock){0,count,0,0};f->blocks[2]=(XrXirBlock){0,2,0,0};
    f->functions[0]=(XrXirFunction){f->names[0],4,NULL,0,XR_XIR_UNIT,&f->blocks[0],1,&f->init,1,NULL,0};
    f->functions[1]=(XrXirFunction){f->names[1],3,NULL,0,kind==1?XR_XIR_BOOL:kind==3?(XrXirType)256:XR_XIR_I64,
        &f->blocks[1],1,f->ops,count,NULL,0};
    f->functions[2]=(XrXirFunction){f->names[2],4,NULL,0,XR_XIR_I64,&f->blocks[2],1,f->entry,2,NULL,0};
    f->identities[2].exported=1;
    if(kind==2)f->identities[1].promises=XR_XIR_FUNCTION_NO_SUSPEND;
    f->source_module=(XrXirSourceModule){f->module_name,4,NULL,0,0};
    f->declarations=(XrXirDeclarations){&f->source_module,1,f->identities,NULL,0,NULL,0,0,2,NULL};
    f->built=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=3,
        .declarations=&f->declarations,.linkage_kind=XR_XIR_PROGRAM};
    if(kind==3) {
        f->cell=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
        f->types=(XrXirTypes){&f->cell,1,NULL,NULL};f->built.types=&f->types;
    }
    if(kind==4) {
        f->argument=(XrXirType)256;f->member_type=XR_XIR_I64;
        f->member=(XrXirNominalField){{f->field+7,5},XR_XIR_I64,XR_XIR_FIELD_PRIVATE};
        f->nominal=(XrXirNominalDeclaration){.module={f->module_name,4},.name={f->field,6},
            .exported=1,.fields=&f->member,.field_count=1,.kind=XR_XIR_NOMINAL_STRUCT};
        f->nominals=(XrXirNominalTable){&f->nominal,1,NULL};
        f->cell=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={.fields=&f->member_type,.field_count=1}};
        f->types=(XrXirTypes){&f->cell,1,&f->nominals,NULL};f->built.types=&f->types;
        f->functions[1].parameters=&f->argument;f->functions[1].parameter_count=1;
        f->ops[0]=(XrXirInstruction){.op=XR_XIR_STRUCT_GET,.type=XR_XIR_I64};f->ops[1].args[0]=1;
        f->entry[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    }
    for(unsigned fn=0;fn<3;++fn) {
        for(unsigned i=0;i<f->functions[fn].instruction_count;++i)
            f->sites[fn][i]=(XrXirGeneratedOrigin){{f->module_name,4},{f->file,14},{f->field,12},23+i,7+fn};
        f->origins[fn]=(XrXirGeneratedFunctionOrigins){f->sites[fn][0],f->sites[fn],f->functions[fn].instruction_count};
    }
}
static XrXirStatus generated_receive(const XrXirCompileContext *context,GeneratedFixture *f,XrXirGeneratedBuilt **owner) {
    XrXirConstruction *construction=NULL;
    uint32_t field=0;XrXirConstructionRow row={0,&field,1};
    bool nominal=f->built.types && f->built.types->nominals;
    const XrXirConstructionSpec input={.rows=nominal?&row:NULL,.count=nominal?1u:0u};
    XrXirStatus status=xr_xir_compile_construction_new_v3(context,f->built.types,&input,&construction);
    if(status==XR_XIR_OK) {
        XrXirGeneratedReceive request={&f->built,construction,f->origins,3};
        status=xr_xir_compile_generated_receive(context,&request,owner);
    }
    xr_xir_compile_construction_free(construction);return status;
}
static void generated_run(const XrXirCompileContext *context,XrXirArtifact **checked) {
    XrXirCheckedPacket packet={0};XrXirArtifact *received=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(*checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(*checked);*checked=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(received,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,2,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,16).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    CHECK(value.type==XR_XIR_I64 && value.payload==42 && !value.reserved);xr_xir_value_drop(&value);
}
static void generated_normal(unsigned kind) {
    XrXirCompileContext context=generated_context(&generated_caps);uint64_t live=generated_stats(&context).live_bytes;
    GeneratedFixture fixture;generated_fixture(&fixture,kind);XrXirGeneratedBuilt *owner=NULL;
    XrXirStatus status=generated_receive(&context,&fixture,&owner);
    if(status!=XR_XIR_OK)fprintf(stderr,"receive kind=%u status=%u\n",kind,(unsigned)status);
    CHECK(status==XR_XIR_OK && owner);memset(&fixture,0xa5,sizeof(fixture));
    for(unsigned retry=0;retry<2;++retry) {
        XrCompileResourceStats before=generated_stats(&context);XrXirArtifact *checked=NULL;
        XrXirGeneratedDiagnostic diagnostic={0};status=xr_xir_compile_generated_check(owner,&checked,&diagnostic);
        if(kind) {
            CHECK(status==XR_XIR_BAD_TYPE && !checked && diagnostic.cause.status==status);
            CHECK(diagnostic.cause.function==1 && diagnostic.origin);
            CHECK(diagnostic.origin->module.length==4 && !memcmp(diagnostic.origin->module.bytes,"user",4));
            CHECK(diagnostic.origin->file.length==14 && !memcmp(diagnostic.origin->file.bytes,"user/derive.xr",14));
            CHECK(diagnostic.origin->field_path.length==12 && !memcmp(diagnostic.origin->field_path.bytes,"Record.value",12));
            CHECK(diagnostic.origin->line>=23 && diagnostic.origin->column==8);
            if(kind==2)CHECK(diagnostic.cause.reason==XR_XIR_DIAGNOSTIC_NO_SUSPEND);
            if(kind==4)CHECK(diagnostic.cause.instruction==0);
        }else {
            CHECK(status==XR_XIR_OK && checked && !diagnostic.origin);
            XrCompileResourceStats occupied=generated_stats(&context);XrXirArtifact *saved=checked;
            CHECK(xr_xir_compile_generated_check(owner,&checked,NULL)==XR_XIR_BAD_STRUCTURE && checked==saved);
            CHECK(generated_stats(&context).work==occupied.work && generated_stats(&context).allocated_bytes==occupied.allocated_bytes);
            if(retry) {
                xr_xir_compile_generated_free(owner);owner=NULL;
                generated_run(&context,&checked);
            }else xr_xir_compile_artifact_free(checked);
        }
        CHECK(generated_stats(&context).work>before.work);
    }
    xr_xir_compile_generated_free(owner);CHECK(generated_stats(&context).live_bytes==live);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void generated_invalid(void) {
    XrXirCompileContext context=generated_context(&generated_caps);uint64_t live=generated_stats(&context).live_bytes;
    for(unsigned kind=0;kind<6;++kind) {
        GeneratedFixture f;generated_fixture(&f,0);
        XrXirImplementationTable malformed={NULL,1};
        if(kind==0)f.built.stage=XR_XIR_CHECKED;
        if(kind==1)f.origins[1].instruction_count=1;
        if(kind==2)f.sites[1][1].line=0;
        if(kind==3)f.sites[1][1].field_path=(XrXirGeneratedText){"\xff",1};
        if(kind==4)f.functions[1].instructions=NULL;
        if(kind==5)f.declarations.implementations=&malformed;
        XrXirGeneratedBuilt *owner=NULL;XrXirStatus status=generated_receive(&context,&f,&owner);
        CHECK(status==(kind==0?XR_XIR_BAD_STAGE:XR_XIR_BAD_STRUCTURE) && !owner);
        CHECK(generated_stats(&context).live_bytes==live);
    }
    GeneratedFixture f;generated_fixture(&f,0);XrXirGeneratedBuilt *owner=NULL;CHECK(generated_receive(&context,&f,&owner)==XR_XIR_OK);
    XrCompileResourceStats before=generated_stats(&context);XrXirGeneratedBuilt *saved=owner;
    XrXirGeneratedReceive occupied={0};CHECK(xr_xir_compile_generated_receive(&context,&occupied,&owner)==XR_XIR_BAD_STRUCTURE && owner==saved);
    CHECK(generated_stats(&context).work==before.work && generated_stats(&context).allocated_bytes==before.allocated_bytes);
    xr_xir_compile_generated_free(owner);CHECK(generated_stats(&context).live_bytes==live);
    generated_fixture(&f,0);f.functions[2].result=XR_XIR_BOOL;owner=NULL;
    CHECK(generated_receive(&context,&f,&owner)==XR_XIR_OK);
    memset(&f,0xa5,sizeof(f));XrXirArtifact *checked=NULL;XrXirGeneratedDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_generated_check(owner,&checked,&diagnostic)==XR_XIR_BAD_TYPE && !checked);
    CHECK(diagnostic.cause.function==UINT32_MAX && diagnostic.origin && diagnostic.origin->column==9);
    CHECK(diagnostic.origin->field_path.length==12 && !memcmp(diagnostic.origin->field_path.bytes,"Record.value",12));
    xr_xir_compile_generated_free(owner);CHECK(generated_stats(&context).live_bytes==live);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static XrXirStatus generated_attempt(const XrXirCompileContext *context,unsigned kind) {
    GeneratedFixture f;generated_fixture(&f,kind);XrXirGeneratedBuilt *owner=NULL;XrXirArtifact *checked=NULL;
    XrXirStatus status=generated_receive(context,&f,&owner);memset(&f,0xa5,sizeof(f));
    if(status==XR_XIR_OK)status=xr_xir_compile_generated_check(owner,&checked,NULL);
    xr_xir_compile_generated_free(owner);xr_xir_compile_artifact_free(checked);return status;
}
static void generated_resources(bool faults) {
    for(unsigned kind=0;kind<5;++kind) {
        XrXirStatus expected=kind?XR_XIR_BAD_TYPE:XR_XIR_OK;
        XrXirCompileContext context=generated_context(&generated_caps);uint64_t live=generated_stats(&context).live_bytes;
        source_fixture_compile_attempts=0;CHECK(generated_attempt(&context,kind)==expected);
        size_t count=source_fixture_compile_attempts;XrCompileResourceStats cost=generated_stats(&context);
        CHECK(count && cost.live_bytes==live);xr_compile_resources_release(context.resources);
        for(size_t point=0;faults && point<count;++point) {
            context=generated_context(&generated_caps);XrCompileResources *same=context.resources;live=generated_stats(&context).live_bytes;
            source_fixture_compile_attempts=0;source_fixture_compile_fail_at=point;source_fixture_compile_injected=false;
            XrXirStatus status=generated_attempt(&context,kind);source_fixture_compile_fail_at=SIZE_MAX;
            CHECK(source_fixture_compile_injected && status==XR_XIR_OUT_OF_MEMORY && generated_stats(&context).live_bytes==live);
            XrCompileResourceStats failed=generated_stats(&context);source_fixture_compile_attempts=0;
            CHECK(generated_attempt(&context,kind)==expected && source_fixture_compile_attempts==count && context.resources==same);
            CHECK(generated_stats(&context).live_bytes==live && generated_stats(&context).work>=failed.work &&
                generated_stats(&context).allocated_bytes>=failed.allocated_bytes);
            xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
        }
        for(unsigned axis=0;axis<3;++axis)for(unsigned exact=0;exact<2;++exact) {
            XrCompileResourceLimits caps=generated_caps;uint64_t value=axis==0?cost.allocated_bytes:axis==1?cost.peak_bytes:cost.work;
            CHECK(value>0);value-=!exact;
            if(axis==0)caps.allocated_bytes=value;else if(axis==1)caps.live_bytes=value;else caps.work=value;
            context=generated_context(&caps);live=generated_stats(&context).live_bytes;
            CHECK(generated_attempt(&context,kind)==(exact?expected:XR_XIR_BUDGET));CHECK(generated_stats(&context).live_bytes==live);
            xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
        }
        printf("generated kind=%u sites=%zu full-FI=%s allocated=%llu peak=%llu work=%llu exact-minus-one=PASS compiler-physical=0/0\n",
            kind,count,faults?"PASS":"NOT_RUN",(unsigned long long)cost.allocated_bytes,
            (unsigned long long)cost.peak_bytes,(unsigned long long)cost.work);
    }
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    if(!strcmp(argv[1],"--resources"))generated_resources(true);
    else if(!strcmp(argv[1],"--budgets"))generated_resources(false);
    else if(!strcmp(argv[1],"invalid"))generated_invalid();
    else {
        unsigned kind=!strcmp(argv[1],"ownership")?0:!strcmp(argv[1],"type")?1:!strcmp(argv[1],"effect")?2:
            !strcmp(argv[1],"escape")?3:4;
        CHECK(kind!=4 || !strcmp(argv[1],"private"));generated_normal(kind);
    }
    return 0;
}
