/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_callable_error_cases.h - Required error identities across actual owners
 *
 * KEY CONCEPT:
 *   A callable retains its declaration's error expression through substitution,
 *   transport and execution; widening never manufactures a narrower proof.
 */
#ifndef XIR_CALLABLE_ERROR_CASES_H
#define XIR_CALLABLE_ERROR_CASES_H
#include "xir/xxir_type_inference.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_program.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_array.h"
#include "xir/xxir_program_packet_internal.h"
#include "xir/xxir_view_prepare_internal.h"
#include "xir/xxir_implementation.h"
#include "xir_effect_input_fixture.h"
#include "xir_callable_old35_golden.h"

static void callable_error_old_packet(const XrXirCompileContext *work,const uint8_t *bytes,size_t length,const uint8_t *expected) {
    uint8_t digest[32];xr_sha256(bytes,length,digest);CHECK(!memcmp(digest,expected,32));
    CHECK(bytes[8]==35 && bytes[12]==95);
    XrXirArtifact *output=NULL;
    CHECK(xr_xir_compile_checked_read(work,bytes,length,&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
}

static void callable_error_types(const XrXirCompileContext *work) {
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE},
        {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_ERROR,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE},
        {.kind=XR_XIR_TYPE_CALLABLE,.element=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,
         .result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE,.parameter_span=1},
        {.kind=XR_XIR_TYPE_CALLABLE,.element=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1),
         .result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE,.parameter_span=2}};
    XrXirTypes types={nodes,4,NULL,NULL};
    const XrXirType narrow=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,wide=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
    const XrXirType formal=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2),actual=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+3);
    CHECK(xr_xir_compile_type_descriptors_verify(work,&types)==XR_XIR_OK);
    CHECK(xr_xir_callable_error(&types,narrow)==XR_XIR_NEVER);
    CHECK(xr_xir_callable_error(&types,XR_XIR_I64)==XR_XIR_UNIT);
    CHECK(xr_xir_compile_callable_weakening(work,&types,narrow,wide)==XR_XIR_OK);
    CHECK(xr_xir_compile_callable_weakening(work,&types,wide,narrow)==XR_XIR_BAD_TYPE);
    XirEffectCallableBound bound={&types,&types,NULL,0,wide,narrow};
    CHECK(xir_effect_callable_bound_matches(work,&bound)==XR_XIR_OK);
    bound.declared=narrow;bound.actual=wide;
    CHECK(xir_effect_callable_bound_matches(work,&bound)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_substitution_matches(work,&types,NULL,0,narrow,wide)==XR_XIR_BAD_TYPE);
    XrXirType argument=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1);
    CHECK(xr_xir_compile_type_substitution_matches(work,&types,&argument,1,formal,actual)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_substitution_matches(work,&types,&argument,1,formal,narrow)==XR_XIR_BAD_TYPE);
    XrXirConstraint bounds[2]={{.markers=XR_XIR_CONSTRAINT_ERROR},{.markers=XR_XIR_CONSTRAINT_ERROR}};
    const XrXirGeneric generic={.constraints=bounds,.parameter_count=2};
    const XrXirFunction function={0};
    const XrXirModule module={.functions=&function,.function_count=1,.types=&types,.generics=&generic};
    const XrXirProofContext proof={&module,{XR_XIR_CONTEXT_FUNCTION,0,0}};
    CHECK(xr_xir_compile_type_use_verify(work,&proof,formal)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_use_verify(work,&proof,actual)==XR_XIR_OK);
    bound.declared=formal;bound.actual=narrow;
    CHECK(xir_effect_callable_bound_matches_definition(work,&module,0,&bound)==XR_XIR_OK);
    argument=XR_XIR_ERROR;
    bound.arguments=&argument;bound.argument_count=1;
    CHECK(xir_effect_callable_bound_matches(work,&bound)==XR_XIR_OK);
    bound.arguments=NULL;bound.argument_count=0;
    bounds[0].markers=0;
    CHECK(xr_xir_compile_type_use_verify(work,&proof,formal)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_use_verify(work,&proof,actual)==XR_XIR_OK);
    CHECK(xir_effect_callable_bound_matches_definition(work,&module,0,&bound)==XR_XIR_BAD_TYPE);
    XrXirInferenceState *inference=NULL;
    const XrXirInferenceRequest request={.types=&types,.own_count=1};
    CHECK(xr_xir_compile_inference_begin(work,&request,&inference)==XR_XIR_OK);
    XrXirInferenceKnown known={0};
    CHECK(xr_xir_compile_inference_expected_known(inference,&types,formal,&known)==XR_XIR_OK && !known.known);
    CHECK(xr_xir_compile_inference_observe(inference,&types,(XrXirInferencePair){formal,narrow})==XR_XIR_OK);
    CHECK(xr_xir_compile_inference_expected_known(inference,&types,formal,&known)==XR_XIR_OK && known.known);
    argument=XR_XIR_UNIT;
    CHECK(xr_xir_compile_inference_finalize(inference,&types,&argument,1)==XR_XIR_OK && argument==XR_XIR_NEVER);
    xr_xir_compile_inference_dispose(inference);
    nodes[0].element=XR_XIR_UNIT;
    CHECK(xr_xir_compile_type_descriptors_verify(work,&types)==XR_XIR_BAD_TYPE);
    nodes[0].element=XR_XIR_I64;
    CHECK(xr_xir_compile_type_descriptors_verify(work,&types)==XR_XIR_BAD_TYPE);
}

static void callable_error_interface(const XrXirCompileContext *work) {
    XrXirType receiver=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,argument=XR_XIR_ERROR;
    XrXirTypeNode nodes[2]={
        {.kind=XR_XIR_TYPE_NOMINAL},
        {.kind=XR_XIR_TYPE_CALLABLE,.element=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,
         .result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED,.parameter_span=1}};
    XrXirNominalDeclaration nominal={.module={"alpha",5},.name={"Meter",5},.exported=1,
        .kind=XR_XIR_NOMINAL_STRUCT};
    XrXirNominalTable nominals={&nominal,1,NULL};
    XrXirConstraint constraint={.markers=XR_XIR_CONSTRAINT_ERROR};
    XrXirInterfaceMethod method={{"measure",7},(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1),0,0,NULL};
    XrXirInterfaceDeclaration interface={.module={"alpha",5},.name={"Measure",7},.exported=1,
        .constraints=&constraint,.parameter_count=1,.methods=&method,.method_count=1};
    XrXirInterfaceTable interfaces={&interface,1};
    XrXirTypes types={nodes,2,&nominals,&interfaces};
    XrXirInstruction init={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirInstruction measure[]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    XrXirBlock one={0,1,0,0},two={0,2,0,0};
    XrXirFunction functions[]={
        {.name="init",.name_length=4,.blocks=&one,.block_count=1,.instructions=&init,
         .instruction_count=1,.error_type=XR_XIR_NEVER},
        {.name="measure",.name_length=7,.parameters=&receiver,.parameter_count=1,.result=XR_XIR_I64,
         .blocks=&two,.block_count=1,.instructions=measure,.instruction_count=2,.error_type=XR_XIR_NEVER}};
    XrXirSourceModule source={"alpha",5,NULL,0,0,0,XR_XIR_MODULE_FRESH_STORAGE};
    XrXirFunctionIdentity identities[]={{0},{.exported=1,.nominal_owner=1,
        .method_kind=XR_XIR_READ_METHOD,.member_access=XR_XIR_MEMBER_PUBLIC}};
    XrXirImplementationBinding binding={{0,&argument,1},0,1};
    XrXirImplementation implementation={0,{0,&argument,1},&binding,1};
    XrXirImplementationTable table={&implementation,1};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX,.implementations=&table};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=2,
        .declarations=&declarations,.types=&types,.linkage_kind=XR_XIR_LIBRARY};
    XrXirArtifact *owned=NULL;
    CHECK(xir_fixture_check(work,&module,&owned,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(owned,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(owned);owned=NULL;
    argument=XR_XIR_NEVER;functions[1].error_type=XR_XIR_ERROR;
    CHECK(xir_fixture_verify(work,&module,NULL)==XR_XIR_BAD_TYPE);
    argument=XR_XIR_ERROR;
    CHECK(xir_fixture_verify(work,&module,NULL)==XR_XIR_OK);
    constraint.markers=0;
    CHECK(xir_fixture_verify(work,&module,NULL)==XR_XIR_BAD_TYPE);
    memset(nodes,0xa5,sizeof(nodes));memset(functions,0xa5,sizeof(functions));
    memset(&interface,0xa5,sizeof(interface));memset(&implementation,0xa5,sizeof(implementation));
    CHECK(xr_xir_compile_checked_read(work,packet.bytes,packet.length,&owned,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_module(owned)->functions[1].error_type==XR_XIR_NEVER);
    xr_xir_compile_artifact_free(owned);
}

typedef struct CallableErrorFixture {
    XrXirTypeNode nodes[10];
    XrXirType map_arguments[2],operating_type,wide_type,narrow_type,array_type,capture_type;
    XrXirNominalVariant variant;
    XrXirConstraint sendable,error_bound,caller_bound;
    XrXirNominalDeclaration nominal;
    XrXirNominalTable nominals;
    XrXirTypes types;
    XrXirInstruction init,entry[12],callee[2],pass;
    XrXirBlock blocks[4];
    XrXirFunction functions[4];
    XrXirType arguments[2],pass_parameter;
    uint32_t operands[3];
    XrXirGeneric generics[4];
    XrXirSourceModule source;
    XrXirFunctionIdentity identities[4];
    XrXirDeclarations declarations;
    XrXirModule module;
} CallableErrorFixture;

static void callable_error_fixture(CallableErrorFixture *f,XrXirType error) {
    *f=(CallableErrorFixture){0};
    f->sendable.markers=XR_XIR_CONSTRAINT_SENDABLE;f->error_bound.markers=XR_XIR_CONSTRAINT_ERROR;
    f->caller_bound=f->error_bound;
    f->variant=(XrXirNominalVariant){{"Empty",5},0,0};
    f->nominal=(XrXirNominalDeclaration){.module={"test",4},.name={"NestedError",11},
        .constraints=&f->sendable,.parameter_count=1,.kind=XR_XIR_NOMINAL_ENUM,.variants=&f->variant,.variant_count=1};
    f->nominals=(XrXirNominalTable){&f->nominal,1,NULL};
    f->map_arguments[0]=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    f->map_arguments[1]=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=XR_XIR_I64};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=XR_XIR_STRING};
    for (uint32_t n=0;n<2;++n) f->nodes[n+2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
        .nominal={.arguments=&f->map_arguments[n],.argument_count=1}};
    f->nodes[4]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.element=error,.result=XR_XIR_I64,
        .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->nodes[5]=f->nodes[4];f->nodes[5].element=error==XR_XIR_ERROR?XR_XIR_NEVER:XR_XIR_ERROR;
    uint32_t type_count=6;
    if (error!=XR_XIR_NEVER && error!=XR_XIR_ERROR) {
        f->nodes[type_count]=f->nodes[4];f->nodes[type_count++].element=XR_XIR_NEVER;
    }
    f->operating_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+4);
    f->wide_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+(error==XR_XIR_ERROR?4:5));
    f->narrow_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+(error==XR_XIR_NEVER?4:error==XR_XIR_ERROR?5:6));
    f->array_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+type_count);
    f->nodes[type_count++]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=f->wide_type};
    f->capture_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+type_count);
    f->nodes[type_count++]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,
        .result=f->wide_type,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->types=(XrXirTypes){f->nodes,type_count,&f->nominals,NULL};
    f->init=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    f->entry[0]=(XrXirInstruction){XR_XIR_FUNCTION_REF,f->narrow_type,{0},{0},2,{0,1}};
    f->entry[1]=(XrXirInstruction){XR_XIR_FUNCTION_REF,f->operating_type,{0},{0},2,{1,1}};
    f->entry[2]=(XrXirInstruction){XR_XIR_COPY,f->operating_type,{1},{0},0,{0}};
    uint32_t count=3;
    if (error!=XR_XIR_ERROR) f->entry[count++]=(XrXirInstruction){XR_XIR_FUNCTION_WEAKEN,f->wide_type,{2},{0},0,{0}};
    f->operands[0]=count-1;
    f->entry[count]=(XrXirInstruction){XR_XIR_CALL,f->wide_type,{0,1},{0},3,{0}};++count;
    f->operands[1]=count-1;
    f->entry[count++]=(XrXirInstruction){XR_XIR_ARRAY_NEW,f->array_type,{1,1},{0},0,{0}};
    uint32_t array=count-1;
    f->operands[2]=f->operands[1];
    f->entry[count++]=(XrXirInstruction){XR_XIR_FUNCTION_REF,f->capture_type,{2,1},{0},3,{0}};
    f->entry[count++]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}};
    f->entry[count]=(XrXirInstruction){XR_XIR_ARRAY_GET,f->wide_type,{array,count-1},{0},0,{0}};++count;
    f->entry[count]=(XrXirInstruction){XR_XIR_CALL_INDIRECT,XR_XIR_I64,{0},{0},count-1,{0}};++count;
    f->entry[count]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{count-1},{0},0,{0}};++count;
    f->callee[0]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},42,{0}};
    f->callee[1]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    f->pass=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    f->blocks[0]=(XrXirBlock){0,1,0,0};f->blocks[1]=(XrXirBlock){0,count,0,0};
    f->blocks[2]=(XrXirBlock){0,2,0,0};f->blocks[3]=(XrXirBlock){0,1,0,0};
    f->pass_parameter=f->wide_type;
    f->functions[0]=(XrXirFunction){.name="init",.name_length=4,.blocks=&f->blocks[0],.block_count=1,
        .instructions=&f->init,.instruction_count=1,.error_type=XR_XIR_ERROR};
    f->functions[1]=(XrXirFunction){.name="entry",.name_length=5,.result=XR_XIR_I64,.blocks=&f->blocks[1],.block_count=1,
        .instructions=f->entry,.instruction_count=count,.operands=f->operands,.operand_count=3,.error_type=XR_XIR_NEVER};
    f->functions[2]=(XrXirFunction){.name="callee",.name_length=6,.result=XR_XIR_I64,.blocks=&f->blocks[2],.block_count=1,
        .instructions=f->callee,.instruction_count=2,.error_type=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE};
    f->functions[3]=(XrXirFunction){.name="pass",.name_length=4,.parameters=&f->pass_parameter,.parameter_count=1,
        .result=f->wide_type,.blocks=&f->blocks[3],.block_count=1,.instructions=&f->pass,.instruction_count=1,.error_type=XR_XIR_NEVER};
    f->arguments[0]=XR_XIR_NEVER;f->arguments[1]=error;
    f->generics[1]=(XrXirGeneric){.arguments=f->arguments,.argument_count=2};
    f->generics[2]=(XrXirGeneric){.constraints=&f->error_bound,.parameter_count=1};
    f->source=(XrXirSourceModule){.name="test",.name_length=4};
    f->identities[2].exported=1;f->identities[3].exported=1;
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,.root_module=0,.entry_function=1};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=4,.types=&f->types,
        .generics=f->generics,.declarations=&f->declarations,.linkage_kind=XR_XIR_PROGRAM};
}

static void callable_error_resign(XrXirCheckedPacket *packet) {
    XrSHA256Context digest;xr_sha256_init(&digest);
    xr_sha256_update(&digest,packet->bytes,32);
    xr_sha256_update(&digest,packet->bytes+64,packet->length-64);
    xr_sha256_final(&digest,packet->bytes+32);
}
static void callable_error_bad_packet(const XrXirCompileContext *work,const XrXirCheckedPacket *packet,XrXirType error) {
    const uint32_t words[]={XR_XIR_TYPE_CALLABLE,0,0,XR_XIR_I64,(uint32_t)error,XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    uint8_t pattern[sizeof(words)];
    for (size_t w=0;w<sizeof(words)/sizeof(words[0]);++w)
        for (uint32_t b=0;b<4;++b) pattern[w*4+b]=(uint8_t)(words[w]>>(b*8));
    uint8_t *bytes=xr_malloc(packet->length);CHECK(bytes);
    size_t found=0,offset=0;
    for (size_t p=64;p+sizeof(pattern)<=packet->length;++p)
        if (!memcmp(packet->bytes+p,pattern,sizeof(pattern))) {++found;offset=p+16;}
    CHECK(found==1);
    const XrXirType invalid[]={XR_XIR_UNIT,XR_XIR_I64};
    for (uint32_t i=0;i<2;++i) {
        memcpy(bytes,packet->bytes,packet->length);
        for (uint32_t b=0;b<4;++b) bytes[offset+b]=(uint8_t)((uint32_t)invalid[i]>>(b*8));
        XrXirCheckedPacket bad={bytes,packet->length};callable_error_resign(&bad);
        XrXirArtifact *output=NULL;
        CHECK(xr_xir_compile_checked_read(work,bad.bytes,bad.length,&output,NULL)==XR_XIR_BAD_TYPE && !output);
    }
    xr_free(bytes);
}

typedef struct CallableErrorHost {
    uint32_t entry,target,narrow_target,pass;
    XrXirType type,narrow,wide,array,capture;
    XrXirValue held,stored,captured;
} CallableErrorHost;
static XrXirAction callable_error_host_resume(XrXirCallView *view) {
    CallableErrorHost *host=(CallableErrorHost *)view->environment;
    uint32_t current=xr_xir_call_current_entry(view->activation);
    if (current==host->pass) return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value=view->arguments[0]};
    if (current!=host->entry) return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,
        .value=current==host->target || current==host->narrow_target ? (XrXirValue){XR_XIR_I64,0,42}:(XrXirValue){0}};
    XrXirValue actual={0},wide={0},narrow={0},bad={0};
    CHECK(xr_xir_instance_function(view,host->type,host->target,NULL,0,&actual)==XR_XIR_CALL_READY);
    if (host->type!=host->wide)
        CHECK(xr_xir_instance_weaken_function(view,host->wide,&actual,&wide)==XR_XIR_CALL_READY);
    else CHECK(xr_xir_value_copy(&actual,&wide)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_instance_function(view,host->narrow,host->narrow_target,NULL,0,&narrow)==XR_XIR_CALL_READY);
    if (xr_xir_callable_error(xr_xir_compile_type_arena_types(view->arena),host->type)!=XR_XIR_NEVER) {
        CHECK(xr_xir_instance_function(view,host->narrow,host->target,NULL,0,&bad)==XR_XIR_CALL_BAD_ARGUMENT && !bad.type);
        CHECK(xr_xir_instance_weaken_function(view,host->narrow,&wide,&bad)==XR_XIR_CALL_BAD_ARGUMENT && !bad.type);
    }
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    CHECK(xr_xir_array_new(host->array,&wide,1,admission,&host->stored)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_instance_function(view,host->capture,host->pass,&wide,1,&host->captured)==XR_XIR_CALL_READY);
    CHECK(xr_xir_value_copy(&wide,&host->held)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&actual);xr_xir_value_drop(&wide);xr_xir_value_drop(&narrow);
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,42}};
}
static void callable_error_host(const XrXirCompileContext *work,const XrXirArtifact *lowered) {
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    CallableErrorHost host={.entry=module->declarations->entry_function};
    const XrXirFunction *entry=&module->functions[host.entry];
    host.narrow_target=(uint32_t)entry->instructions[0].immediate;host.narrow=entry->instructions[0].type;
    host.target=(uint32_t)entry->instructions[1].immediate;host.type=entry->instructions[1].type;
    for (uint32_t i=2;i<entry->instruction_count;++i) {
        const XrXirInstruction *op=&entry->instructions[i];
        if (op->op==XR_XIR_CALL) {host.pass=(uint32_t)op->immediate;host.wide=op->type;}
        if (op->op==XR_XIR_ARRAY_NEW) host.array=op->type;
        if (op->op==XR_XIR_FUNCTION_REF) host.capture=op->type;
    }
    CHECK(host.wide && host.array && host.capture);
    XrXirCallEntry *entries=xr_calloc(module->function_count,sizeof(*entries));CHECK(entries);
    for (uint32_t i=0;i<module->function_count;++i) entries[i]=(XrXirCallEntry){
        .abi_version=XR_XIR_CALL_ABI_VERSION,.parameters=module->functions[i].parameters,
        .parameter_count=module->functions[i].parameter_count,.result=module->functions[i].result,
        .resume=callable_error_host_resume,.environment=&host};
    XrXirProgramSpec spec={.abi_version=XR_XIR_PROGRAM_ABI_VERSION,
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.entries=entries,.entry_count=module->function_count,
        .declarations=module->declarations,.types=module->types,.packet=xir_program_packet_borrow(lowered)};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(work,&spec,&program)==XR_XIR_OK);
    xr_free(entries);
    XrXirInstanceConfig config={0};CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL,*other=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&other)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,host.entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult polled={0};
    do { polled=xr_xir_instance_poll_bounded(instance,1024); } while (polled.outcome.status==XR_XIR_CALL_READY);
    CHECK(polled.outcome.status==XR_XIR_CALL_RETURNED && polled.outcome.value.payload==42);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_start_function(other,&host.held,NULL,0)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start_function(instance,&host.held,NULL,0)==XR_XIR_CALL_READY);
    do { polled=xr_xir_instance_poll_bounded(instance,1024); } while (polled.outcome.status==XR_XIR_CALL_READY);
    CHECK(polled.outcome.status==XR_XIR_CALL_RETURNED && polled.outcome.value.payload==42);
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_free(other)==XR_XIR_CALL_READY && xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_value_valid(&host.held) && xr_xir_value_valid(&host.captured) && xr_xir_value_valid(&host.stored));
    xr_xir_value_drop(&host.held);xr_xir_value_drop(&host.captured);xr_xir_value_drop(&host.stored);
}

static void callable_error_program(const XrXirCompileContext *work,XrXirType error) {
    CallableErrorFixture *f=xr_malloc(sizeof(*f));CHECK(f);callable_error_fixture(f,error);
    XrXirArtifact *checked=NULL,*received=NULL,*closed=NULL,*lowered=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(work,&f->module,&checked,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"Callable E%u status%u f%u b%u i%u reason%u\n",
        (unsigned)error,(unsigned)status,diagnostic.function,diagnostic.block,diagnostic.instruction,(unsigned)diagnostic.reason);
    CHECK(status==XR_XIR_OK);
    /* E belongs to the actual reference target, not its flags or result shape. */
    f->arguments[1]=error==XR_XIR_NEVER?XR_XIR_ERROR:XR_XIR_NEVER;
    CHECK(xir_fixture_verify(work,&f->module,NULL)==XR_XIR_BAD_TYPE);
    f->arguments[1]=error;
    f->error_bound.markers=0;
    CHECK(xir_fixture_verify(work,&f->module,NULL)==XR_XIR_BAD_TYPE);
    f->error_bound.markers=XR_XIR_CONSTRAINT_ERROR;
    if (error==(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2)) {
        f->nodes[4].element=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+3);
        CHECK(xir_fixture_verify(work,&f->module,NULL)==XR_XIR_BAD_TYPE);
    }
    memset(f,0xa5,sizeof(*f));xr_free(f);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    callable_error_bad_packet(work,&packet,error);
    CHECK(xr_xir_compile_checked_read(work,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(received);
    CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);
    uint8_t identity[32];xr_sha256(packet.bytes,packet.length,identity);
    xr_xir_compile_checked_packet_free(&packet);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    XrXirCtfeCallRequest request={.evaluation={.artifact=lowered,.function=module->declarations->entry_function,
        .target=target,.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    memcpy(request.evaluation.checked_identity,identity,32);
    XrXirValue output={0};
    CHECK(xr_xir_compile_ctfe_calls(&request,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I64 && output.payload==42);
    xr_xir_value_drop(&output);callable_error_host(work,lowered);xr_xir_compile_artifact_free(lowered);
}
static void callable_error_library(const XrXirCompileContext *work,bool nested) {
    char directory[XR_TEST_PATH_MAX]="xir-callable-errors-XXXXXX",absolute[XR_TEST_PATH_MAX];
    char library[XR_TEST_PATH_MAX],root[XR_TEST_PATH_MAX];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory,absolute,sizeof(absolute)));
    CHECK(snprintf(library,sizeof(library),"%s/lib.xr",absolute)>0 && snprintf(root,sizeof(root),"%s/root.xr",absolute)>0);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,absolute};
    char *canonical=NULL;
    CHECK(xr_compile_module_identity_from_logical(work->resources,&authority,"lib.xr",&canonical)==XR_MODULE_OK);
    CallableErrorFixture *f=xr_malloc(sizeof(*f));CHECK(f);
    callable_error_fixture(f,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2));
    f->source.name=canonical;f->source.name_length=(uint32_t)strlen(canonical);
    f->nominal.module=(XrXirLiteral){canonical,(uint32_t)strlen(canonical)};
    f->nominal.exported=1;f->identities[1].exported=1;
    XrXirType generic_error=nested?(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2):(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    if (nested) {
        f->nodes[0].result=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;f->nodes[0].parameter_span=1;
        f->nodes[2].parameter_span=1;
    }
    f->nodes[4].element=generic_error;f->nodes[4].parameter_span=1;f->arguments[1]=generic_error;
    f->generics[1].constraints=nested?&f->sendable:&f->caller_bound;f->generics[1].parameter_count=1;
    f->functions[1].result=f->operating_type;
    f->entry[2]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}};
    f->functions[1].instruction_count=3;f->blocks[1].count=3;
    f->functions[1].operands=NULL;f->functions[1].operand_count=0;
    f->module.linkage_kind=XR_XIR_LIBRARY;
    f->declarations.root_module=UINT32_MAX;f->declarations.entry_function=UINT32_MAX;
    /* The first real binder span uses an admitted nominal argument; it is
     * independent of the following declaration-local error parameter. */
    XrXirType first_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+f->types.count);
    f->nodes[f->types.count]=f->nodes[4];
    f->nodes[f->types.count].element=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+3);
    f->nodes[f->types.count++].parameter_span=0;
    f->entry[0].type=first_type;f->arguments[0]=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+3);
    XrXirArtifact *checked=NULL;XrXirDiagnostic initial={0};
    f->error_bound.markers=0;
    CHECK(xir_fixture_verify(work,&f->module,NULL)==XR_XIR_BAD_TYPE);
    f->error_bound.markers=XR_XIR_CONSTRAINT_ERROR;
    XrXirConstruction *facts=NULL;CHECK(xir_fixture_construction(work,&f->types,&facts)==XR_XIR_OK);
    XrXirStatus ordinary=xir_compile_view_prepare_ordinary(work,&f->module,facts,&initial);
    if (ordinary!=XR_XIR_OK) fprintf(stderr,"Callable library ordinary %u f%u b%u i%u\n",ordinary,initial.function,initial.block,initial.instruction);
    CHECK(ordinary==XR_XIR_OK);
    XrXirEffects *effects=NULL;
    XrXirStatus inferred=xir_fixture_effects_infer(work,
        (XirFixtureEffectInput){&f->module,facts},&effects);
    xr_xir_compile_construction_free(facts);
    if (inferred!=XR_XIR_OK) fprintf(stderr,"Callable library effects %u\n",inferred);
    CHECK(inferred==XR_XIR_OK);
    xr_xir_compile_effects_free(effects);
    XrXirStatus initial_status=xir_fixture_check(work,&f->module,&checked,&initial);
    if (initial_status!=XR_XIR_OK) fprintf(stderr,"Callable library raw %u f%u b%u i%u generic%u constraints%u declarations%u\n",
        initial_status,initial.function,initial.block,initial.instruction,
        xr_xir_compile_generics_structure_verify(work,&f->module),xr_xir_compile_declaration_constraints_verify(work,&f->module),
        xr_xir_compile_declarations_verify(work,&f->declarations,&f->types,4,XR_XIR_LIBRARY));
    CHECK(initial_status==XR_XIR_OK);
    memset(f,0xa5,sizeof(*f));xr_free(f);xr_compile_resources_free(canonical);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    checked=NULL;CHECK(xr_xir_compile_checked_read(work,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    XrXirLibraryModuleInput binding={authority,"lib.xr"};
    XrXirLibraryInput library_input={packet.bytes,packet.length,{0},&binding,1};
    xr_sha256(packet.bytes,packet.length,library_input.sha256);
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(work,&library_input,1,&catalog)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(work->resources,&session)==XR_COMPILER_SESSION_OK);
    XrXirSourceRequest request={session,root,&authority,work,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,catalog};
    char text[128];CHECK(snprintf(text,sizeof(text),"import {entry} from \"./lib\"\nvar f=entry<%s>()\ntry f()\n",nested?"i64":"Error")>0);
    XrXirSourceText input={"root.xr",text,strlen(text)};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&source,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"Callable E library %u at %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK);
    memset(text,0xa5,sizeof(text));xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);bool found=false;
    for (uint32_t n=0;n<module->function_count;++n) for (uint32_t i=0;i<module->functions[n].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[n].instructions[i];
        if (op->op!=XR_XIR_CALL || !xr_xir_type_is_callable(module->types,op->type)) continue;
        XrXirType error=xr_xir_callable_error(module->types,op->type);
        if (!nested) { CHECK(error==XR_XIR_ERROR);found=true;continue; }
        const XrXirTypeNode *nominal=xr_xir_type_node(module->types,error);
        CHECK(xr_xir_type_is_enum(module->types,error) && nominal && !nominal->parameter_span && nominal->nominal.argument_count==1);
        const XrXirTypeNode *map=xr_xir_type_node(module->types,nominal->nominal.arguments[0]);
        CHECK(map && map->kind==XR_XIR_TYPE_MAP && map->element==XR_XIR_I64 && map->result==XR_XIR_I64);found=true;
    }
    CHECK(found && xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);checked=NULL;
    CHECK(xr_xir_compile_checked_read(work,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);CHECK(xr_test_rmdir(directory)==0);
}
/* Real open value parameters, results and captures stay open even when the
 * same reference has a valid declaration-local generic error expression. */
static void callable_error_open_values(const XrXirCompileContext *work,uint32_t mode) {
    CallableErrorFixture *f=xr_malloc(sizeof(*f));CHECK(f);
    callable_error_fixture(f,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2));
    XrXirType variable=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1),caller_parameter=variable,target_parameter=variable;
    XrXirCallableParameter parameter={variable,XR_PARAM_READ};
    XrXirConstraint constraints[2]={{.markers=XR_XIR_CONSTRAINT_ERROR},{0}};
    XrXirType arguments[]={(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+3),variable,
        (XrXirType)XR_XIR_TYPE_PARAMETER_BASE,variable};
    f->nodes[4].element=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    f->nodes[4].parameter_span=mode<2?2u:1u;
    if (!mode) {f->nodes[4].parameters=&parameter;f->nodes[4].parameter_count=1;}
    if (mode==1) f->nodes[4].result=variable;
    XrXirType first=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+f->types.count);
    f->nodes[f->types.count]=f->nodes[4];f->nodes[f->types.count].element=arguments[0];
    f->nodes[f->types.count++].parameter_span=mode<2?2u:0u;
    f->entry[0]=(XrXirInstruction){XR_XIR_FUNCTION_REF,first,{0,mode==2?1u:0u},{0},2,{0,2}};
    f->entry[1]=(XrXirInstruction){XR_XIR_FUNCTION_REF,f->operating_type,{mode==2?1u:0u,mode==2?1u:0u},{0},2,{2,2}};
    f->entry[2]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{mode==2?2u:1u},{0},0,{0}};
    f->blocks[1].count=f->functions[1].instruction_count=3;f->functions[1].result=f->operating_type;
    f->functions[1].parameters=mode==2?&caller_parameter:NULL;f->functions[1].parameter_count=mode==2;
    f->operands[0]=f->operands[1]=0;
    f->functions[1].operands=mode==2?f->operands:NULL;f->functions[1].operand_count=mode==2?2u:0u;
    f->functions[2].parameters=mode==1?NULL:&target_parameter;f->functions[2].parameter_count=mode==1?0u:1u;
    if (mode==1) {
        f->functions[2].result=variable;f->callee[0]=(XrXirInstruction){XR_XIR_MATCH_FAIL,XR_XIR_UNIT,{0},{0},0,{0}};
        f->functions[2].instruction_count=f->blocks[2].count=1;
    } else f->callee[1].args[0]=1;
    f->generics[1]=(XrXirGeneric){.constraints=constraints,.parameter_count=2,.arguments=arguments,.argument_count=4};
    f->generics[2]=(XrXirGeneric){.constraints=constraints,.parameter_count=2};
    f->module.linkage_kind=XR_XIR_LIBRARY;f->declarations.root_module=f->declarations.entry_function=UINT32_MAX;
    XrXirArtifact *owned=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(work,&f->module,&owned,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"Callable open values %u status%u f%u i%u\n",mode,status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);
    if (mode<2) {
        f->nodes[4].parameter_span=1;
        CHECK(xr_xir_compile_type_descriptors_verify(work,&f->types)==XR_XIR_BAD_TYPE);
    } else {
        caller_parameter=XR_XIR_I64;
        CHECK(xir_fixture_verify(work,&f->module,NULL)==XR_XIR_BAD_TYPE);
    }
    memset(f,0xa5,sizeof(*f));xr_free(f);memset(arguments,0xa5,sizeof(arguments));memset(constraints,0xa5,sizeof(constraints));
    parameter.type=XR_XIR_UNIT;caller_parameter=target_parameter=XR_XIR_UNIT;
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(owned,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(owned);owned=NULL;
    CHECK(xr_xir_compile_checked_read(work,packet.bytes,packet.length,&owned,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    const XrXirModule *received=xr_xir_compile_artifact_module(owned);
    CHECK(received->types->nodes[4].parameter_span==(mode<2?2u:1u));
    if (!mode) CHECK(received->types->nodes[4].parameters[0].type==variable);
    if (mode==1) CHECK(received->types->nodes[4].result==variable);
    if (mode==2) CHECK(received->functions[1].parameters[0]==variable && received->functions[2].parameters[0]==variable);
    xr_xir_compile_artifact_free(owned);
}
static void check_callable_errors(const XrXirCompileContext *work) {
    callable_error_old_packet(work,callable_old35_library,sizeof(callable_old35_library),callable_old35_library_sha256);
    callable_error_old_packet(work,callable_old35_program,sizeof(callable_old35_program),callable_old35_program_sha256);
    callable_error_types(work);
    callable_error_interface(work);
    callable_error_program(work,XR_XIR_NEVER);
    callable_error_program(work,XR_XIR_ERROR);
    callable_error_program(work,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2));
    callable_error_library(work,false);
    callable_error_library(work,true);
    for (uint32_t mode=0;mode<3;++mode) callable_error_open_values(work,mode);
}
#endif // XIR_CALLABLE_ERROR_CASES_H
