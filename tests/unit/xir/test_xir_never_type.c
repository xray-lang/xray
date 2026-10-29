/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_never_type.c - Empty error expressions across compiler ownership
 *
 * KEY CONCEPT:
 *   Error constraints admit Never without inventing a value or storage layout.
 */
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_types.h"
#include "base/xcompile_resources.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xir_construction_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
#include "toolchain/xcompiler_session.h"
#include "base/xsha256.h"
#include "../test_win_compat.h"

#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr,"FAIL %d: %s\n",__LINE__,#expression); exit(1); \
} } while (0)

typedef struct NeverFixture {
    XrXirConstraint constraint;
    XrXirNominalVariant variant;
    XrXirNominalDeclaration declaration;
    XrXirNominalTable table;
    XrXirType argument;
    XrXirTypeNode node;
    XrXirTypes types;
} NeverFixture;

static void fixture_init(NeverFixture *f) {
    *f=(NeverFixture){0};
    f->constraint.markers=XR_XIR_CONSTRAINT_ERROR;
    f->variant=(XrXirNominalVariant){{"Empty",5},0,0};
    f->declaration=(XrXirNominalDeclaration){.module={"test",4},.name={"ErrorTag",8},
        .constraints=&f->constraint,.parameter_count=1,.kind=XR_XIR_NOMINAL_ENUM,
        .variants=&f->variant,.variant_count=1};
    f->table=(XrXirNominalTable){&f->declaration,1,NULL};
    f->argument=XR_XIR_NEVER;
    f->node=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
        .nominal={.arguments=&f->argument,.argument_count=1}};
    f->types=(XrXirTypes){&f->node,1,&f->table,NULL};
}

static void check_error_expression(const XrXirCompileContext *work,const XrXirTypes *types) {
    XrXirModule module={.types=types};
    XrXirProofContext closed={&module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    CHECK(xr_xir_compile_type_expression_shape(work,types,XR_XIR_NEVER,0)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_markers_prove(work,&closed,XR_XIR_NEVER,XR_XIR_CONSTRAINT_ERROR)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_markers_prove(work,&closed,XR_XIR_I64,XR_XIR_CONSTRAINT_ERROR)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_markers_prove(work,&closed,XR_XIR_UNIT,XR_XIR_CONSTRAINT_ERROR)==XR_XIR_BAD_TYPE);
    XrXirTypeDescription description={0};
    CHECK(xr_xir_compile_type_description(work,types,XR_XIR_NEVER,&description)==XR_XIR_OK);
    CHECK(description.type==XR_XIR_NEVER && !description.kind && !description.node);

    XrXirType argument=XR_XIR_NEVER;
    XrXirConstraintUse use={&module,{XR_XIR_CONTEXT_NOMINAL,0,0},0,&argument,1};
    CHECK(xr_xir_compile_constraints_prove(work,&closed,&use)==XR_XIR_OK);
    XrXirProofContext formal={&module,{XR_XIR_CONTEXT_NOMINAL,0,0}};
    XrXirType parameter=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    CHECK(xr_xir_compile_type_markers_prove(work,&formal,parameter,XR_XIR_CONSTRAINT_ERROR)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_substitution_matches(work,types,&argument,1,parameter,XR_XIR_NEVER)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_substitution_matches(work,types,&argument,1,parameter,XR_XIR_UNIT)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_substitution_matches(work,types,&argument,1,XR_XIR_NEVER,XR_XIR_ERROR)==XR_XIR_BAD_TYPE);
    argument=XR_XIR_I64;
    CHECK(xr_xir_compile_constraints_prove(work,&closed,&use)==XR_XIR_BAD_TYPE);
}

static void check_no_value(const XrXirCompileContext *work,const XrXirTypes *types) {
    XrXirModule module={.types=types};
    XrXirProofContext closed={&module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    CHECK(xr_xir_compile_type_storage_prove(work,&closed,XR_XIR_NEVER)==XR_XIR_BAD_TYPE);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    for (uint32_t kind=XR_XIR_LAYOUT_STORAGE;kind<=XR_XIR_LAYOUT_FRAME;++kind) {
        XrXirLayout layout={123,8};
        CHECK(xr_xir_compile_layout(work,types,XR_XIR_NEVER,&target,(XrXirLayoutContext)kind,&layout)==XR_XIR_BAD_LAYOUT);
        CHECK(layout.size==123 && layout.alignment==8);
    }
    XrXirValue value={XR_XIR_NEVER,0,0};
    CHECK(!xr_xir_value_valid(&value));
    XrXirTypeNode array={.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_NEVER};
    XrXirTypes invalid={&array,1,NULL,NULL};
    CHECK(xr_xir_compile_type_descriptors_verify(work,&invalid)==XR_XIR_BAD_TYPE);
}

static void check_declaration_packet(const XrXirCompileContext *work,XrXirType error_type) {
    NeverFixture *producer=xr_malloc(sizeof(*producer));
    CHECK(producer);
    fixture_init(producer);
    const XrXirBlock block={0,1,0,0};
    const XrXirInstruction op={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirFunction function={.name="declared",.name_length=8,.result=XR_XIR_UNIT,
        .blocks=&block,.block_count=1,.instructions=&op,.instruction_count=1};
    const XrXirSourceModule source={.name="test",.name_length=4};
    const XrXirFunctionIdentity identity={0};
    const XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=&identity,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=&function,.function_count=1,
        .declarations=&declarations,
        .types=(uint32_t)error_type>=XR_XIR_CONSTRUCTED_TYPE_BASE ? &producer->types : NULL,
        .linkage_kind=XR_XIR_LIBRARY};
    CHECK(xr_xir_compile_declaration_constraints_verify(work,&module)==XR_XIR_BAD_TYPE);
    function.error_type=XR_XIR_I64;
    CHECK(xr_xir_compile_declaration_constraints_verify(work,&module)==XR_XIR_BAD_TYPE);
    XrXirArtifact *invalid=NULL;
    CHECK(xir_fixture_check(work,&module,&invalid,NULL)==XR_XIR_BAD_TYPE && !invalid);
    function.error_type=error_type;
    CHECK(xr_xir_compile_declaration_constraints_verify(work,&module)==XR_XIR_OK);
    XrXirArtifact *checked=NULL;
    XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(work,&module,&checked,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"declared E%u status%u f%u b%u i%u reason%u\n",
        (unsigned)error_type,(unsigned)status,diagnostic.function,diagnostic.block,diagnostic.instruction,(unsigned)diagnostic.reason);
    CHECK(status==XR_XIR_OK);
    xr_free(producer);
    function.error_type=XR_XIR_UNIT;
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_module(checked)->functions[0].error_type==error_type);

    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    XrXirArtifact *received=NULL;
    uint8_t *old=xr_malloc(packet.length);
    CHECK(old);
    memcpy(old,packet.bytes,packet.length);
    for (uint32_t i=0;i<4;++i) old[8+i]=(uint8_t)((XR_XIR_CHECKED_SCHEMA-1)>>(8*i));
    CHECK(xr_xir_compile_checked_read(work,old,packet.length,&received,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!received);
    memcpy(old,packet.bytes,packet.length);
    for (uint32_t i=0;i<4;++i) old[12+i]=(uint8_t)((XR_XIR_CHECKED_CONTRACT-1)>>(8*i));
    CHECK(xr_xir_compile_checked_read(work,old,packet.length,&received,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!received);
    size_t error_at=64+12+4+function.name_length+4+4;
    CHECK(error_at+4<=packet.length);
    const XrXirType invalid_errors[]={XR_XIR_UNIT,XR_XIR_I64};
    for (uint32_t e=0;e<2;++e) {
        memcpy(old,packet.bytes,packet.length);
        for (uint32_t i=0;i<4;++i) old[error_at+i]=(uint8_t)((uint32_t)invalid_errors[e]>>(8*i));
        XrSHA256Context digest;
        xr_sha256_init(&digest);
        xr_sha256_update(&digest,old,32);
        xr_sha256_update(&digest,old+64,packet.length-64);
        xr_sha256_final(&digest,old+32);
        CHECK(xr_xir_compile_checked_read(work,old,packet.length,&received,NULL)==XR_XIR_BAD_TYPE);
        CHECK(!received);
    }
    xr_free(old);
    CHECK(xr_xir_compile_checked_read(work,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_module(received)->functions[0].error_type==error_type);
    xr_xir_compile_artifact_free(received);
}

static void check_generic_error_argument(const XrXirCompileContext *work) {
    const XrXirBlock blocks[2]={{0,2,0,0},{0,1,0,0}};
    const XrXirInstruction caller[]={
        {XR_XIR_CALL,XR_XIR_UNIT,{0},{0},1,{0,1}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirInstruction body={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirFunction functions[]={
        {.name="entry",.name_length=5,.result=XR_XIR_UNIT,.blocks=&blocks[0],.block_count=1,
         .instructions=caller,.instruction_count=2,.error_type=XR_XIR_NEVER},
        {.name="generic",.name_length=7,.result=XR_XIR_UNIT,.blocks=&blocks[1],.block_count=1,
         .instructions=&body,.instruction_count=1,.error_type=XR_XIR_NEVER}};
    XrXirConstraint bound={.markers=XR_XIR_CONSTRAINT_ERROR};
    const XrXirType argument=XR_XIR_NEVER;
    const XrXirGeneric generics[]={
        {.arguments=&argument,.argument_count=1},
        {.constraints=&bound,.parameter_count=1}};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=2,
        .generics=generics,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL,*closed=NULL;
    bound.markers=0;
    CHECK(xir_fixture_check(work,&module,&checked,NULL)==XR_XIR_BAD_TYPE && !checked);
    bound.markers=XR_XIR_CONSTRAINT_ERROR;
    CHECK(xir_fixture_check(work,&module,&checked,NULL)==XR_XIR_OK);
    XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_specialize(checked,&closed,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"specialize E status%u f%u b%u i%u reason%u\n",
        (unsigned)status,diagnostic.function,diagnostic.block,diagnostic.instruction,(unsigned)diagnostic.reason);
    CHECK(status==XR_XIR_OK);
    const XrXirModule *result=xr_xir_compile_artifact_module(closed);
    bool found=false;
    for (uint32_t f=0;f<result->function_count;++f) {
        uint32_t original=UINT32_MAX;
        CHECK(xr_xir_compile_specialization_origin(closed,f,&original)==XR_XIR_OK);
        if (original==1) {
            found=true;
        }
    }
    CHECK(found);
    CHECK(xr_xir_compile_artifact_verify(closed,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    xr_xir_compile_artifact_free(checked);
}

static void check_generic_declaration(const XrXirCompileContext *work) {
    const XrXirBlock blocks[2]={{0,2,0,0},{0,1,0,0}};
    const XrXirInstruction caller[]={
        {XR_XIR_CALL,XR_XIR_UNIT,{0},{0},1,{0,1}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirInstruction body={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirFunction functions[]={
        {.name="entry",.name_length=5,.result=XR_XIR_UNIT,.blocks=&blocks[0],.block_count=1,
         .instructions=caller,.instruction_count=2,.error_type=XR_XIR_NEVER},
        {.name="generic",.name_length=7,.result=XR_XIR_UNIT,.blocks=&blocks[1],.block_count=1,
         .instructions=&body,.instruction_count=1,.error_type=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE}};
    XrXirConstraint bound={.markers=XR_XIR_CONSTRAINT_ERROR};
    const XrXirType argument=XR_XIR_NEVER;
    const XrXirGeneric generics[]={
        {.arguments=&argument,.argument_count=1},
        {.constraints=&bound,.parameter_count=1}};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=2,
        .generics=generics,.linkage_kind=XR_XIR_PROGRAM};
    CHECK(xr_xir_compile_declaration_constraints_verify(work,&module)==XR_XIR_OK);
    bound.markers=0;
    CHECK(xr_xir_compile_declaration_constraints_verify(work,&module)==XR_XIR_BAD_TYPE);
    bound.markers=XR_XIR_CONSTRAINT_ERROR;
    XrXirArtifact *checked=NULL,*closed=NULL;
    CHECK(xir_fixture_check(work,&module,&checked,NULL)==XR_XIR_OK);
    XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_specialize(checked,&closed,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"specialize E status%u f%u b%u i%u reason%u\n",
        (unsigned)status,diagnostic.function,diagnostic.block,diagnostic.instruction,(unsigned)diagnostic.reason);
    CHECK(status==XR_XIR_OK);
    const XrXirModule *result=xr_xir_compile_artifact_module(closed);
    bool found=false;
    for (uint32_t f=0;f<result->function_count;++f) {
        uint32_t original=UINT32_MAX;
        CHECK(xr_xir_compile_specialization_origin(closed,f,&original)==XR_XIR_OK);
        if (original==1) {
            found=true;
            CHECK(result->functions[f].error_type==XR_XIR_NEVER);
        }
    }
    CHECK(found);
    CHECK(xr_xir_compile_artifact_verify(closed,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    xr_xir_compile_artifact_free(checked);
}

static void check_lowered_error_remap(const XrXirCompileContext *work) {
    NeverFixture fixture;
    fixture_init(&fixture);
    XrXirType parameter=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirTypeNode nodes[2]={fixture.node,fixture.node};
    nodes[0].parameter_span=1;
    nodes[0].nominal.arguments=&parameter;
    fixture.types.nodes=nodes;fixture.types.count=2;
    const XrXirBlock blocks[3]={{0,1,0,0},{0,3,0,0},{0,1,0,0}};
    const XrXirInstruction caller[]={
        {XR_XIR_CALL,XR_XIR_UNIT,{0},{0},2,{0,1}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    const XrXirInstruction body={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirFunction functions[]={
        {.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=&blocks[0],.block_count=1,
         .instructions=&body,.instruction_count=1,.error_type=XR_XIR_ERROR},
        {.name="entry",.name_length=5,.result=XR_XIR_I64,.blocks=&blocks[1],.block_count=1,
         .instructions=caller,.instruction_count=3,.error_type=XR_XIR_NEVER},
        {.name="generic",.name_length=7,.result=XR_XIR_UNIT,.blocks=&blocks[2],.block_count=1,
         .instructions=&body,.instruction_count=1,.error_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1)}};
    XrXirConstraint bound={.markers=XR_XIR_CONSTRAINT_ERROR};
    const XrXirGeneric generics[]={
        {0},
        {.arguments=&fixture.argument,.argument_count=1},
        {.constraints=&bound,.parameter_count=1}};
    const XrXirSourceModule source={.name="test",.name_length=4};
    const XrXirFunctionIdentity identities[3]={{0},{0},{0}};
    const XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .root_module=0,.entry_function=1};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=3,
        .types=&fixture.types,.generics=generics,.declarations=&declarations,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    functions[1].error_type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    CHECK(xir_fixture_check(work,&module,&checked,NULL)==XR_XIR_BAD_TYPE && !checked);
    functions[1].error_type=XR_XIR_NEVER;
    XrXirDiagnostic rejected={0};
    CHECK(xir_fixture_check(work,&module,&checked,&rejected)==XR_XIR_BAD_TYPE && !checked);
    CHECK(rejected.function==1 && rejected.block==0 && rejected.instruction==0);
    /* ErrorTag<Never> still has an Empty variant and remains the declared E. */
    functions[1].error_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
    CHECK(xir_fixture_check(work,&module,&checked,NULL)==XR_XIR_OK);
    memset(nodes,0xa5,sizeof(nodes));
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *before=xr_xir_compile_artifact_module(closed);
    bool before_remap=false;
    for (uint32_t f=0;f<before->function_count;++f)
        before_remap|=before->functions[f].error_type==(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
    CHECK(before_remap && before->types->nodes[0].parameter_span==1);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *after=xr_xir_compile_artifact_module(lowered);
    bool remapped=false;
    for (uint32_t f=0;f<after->function_count;++f)
        if (after->functions[f].error_type!=XR_XIR_NEVER && after->functions[f].error_type!=XR_XIR_ERROR) {
            CHECK(after->functions[f].error_type==(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE);
            CHECK(xr_xir_type_is_enum(after->types,after->functions[f].error_type));
            remapped=true;
        }
    CHECK(remapped);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered);
}

static uint32_t source_named_function(const XrXirModule *module,const char *name) {
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==strlen(name) &&
            !memcmp(module->functions[f].name,name,strlen(name))) return f;
    CHECK(false);
    return 0;
}
static uint32_t source_closure_function(const XrXirModule *module,const char *owner) {
    const XrXirFunction *function=&module->functions[source_named_function(module,owner)];
    uint32_t target=UINT32_MAX,raw=source_named_function(module,"raw");
    for (uint32_t i=0;i<function->instruction_count;++i) {
        const XrXirInstruction *op=&function->instructions[i];
        if (op->op!=XR_XIR_FUNCTION_REF) continue;
        CHECK(op->immediate>=0 && (uint64_t)op->immediate<module->function_count);
        if ((uint32_t)op->immediate==raw) continue;
        CHECK(target==UINT32_MAX);
        target=(uint32_t)op->immediate;
    }
    CHECK(target<module->function_count);
    return target;
}

/* Finished owned instructions provide an independent receiving-side view of
 * the callee value. Argument-group offsets do not identify that value. */
static void check_source_indirect_bounds(const XrXirModule *module,const XrXirFunction *function,
    uint32_t expected_errors,uint32_t expected_origins) {
    uint32_t errors=0,origins=0;
    for (uint32_t i=0;i<function->instruction_count;++i) {
        const XrXirInstruction *op=&function->instructions[i];
        CHECK(op->op!=XR_XIR_CELL_NEW);
        if (op->op!=XR_XIR_CALL_INDIRECT) continue;
        CHECK(op->immediate>=0 && (uint64_t)op->immediate<=UINT32_MAX);
        uint32_t value=(uint32_t)op->immediate;
        XrXirType type;
        if (value<function->parameter_count) {
            type=function->parameters[value];origins|=1u;
        } else {
            uint32_t producer=value-function->parameter_count;
            CHECK(producer<i);type=function->instructions[producer].type;origins|=2u;
        }
        const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,type);
        CHECK(signature && signature->result==op->type);
        CHECK(signature->element==XR_XIR_NEVER || signature->element==XR_XIR_ERROR);
        errors|=signature->element==XR_XIR_NEVER?1u:2u;
        if (signature->element==XR_XIR_ERROR) CHECK(value!=op->args[0]);
    }
    CHECK(errors==expected_errors && origins==expected_origins);
    CHECK(function->error_type==(expected_errors==1u?XR_XIR_NEVER:XR_XIR_ERROR));
}

static char *source_transport_text(size_t *length) {
    const char prefix[]="export struct Box{value:i64=41\nenabled:bool=true\nlabel:string=\"owned\"}\n"
        "export fn raw()->i64{return 41}\nfn defaultValue()->i64{return 41}\n"
        "export fn withDefault(x:i64=41)->i64{return x+1}\n"
        "export fn withCall(x:i64=defaultValue())->i64{return x}\n"
        "export fn withLiterals(flag:bool=true,text:string=\"owned\",value:i64=1+2)->i64{return value}\n"
        "export fn withUnknown(f:fn()->i64=fn()->i64{return 7})->fn()->i64{return f}\n"
        "var seed:i64=41\nfn identity<T>(value:T)->T{return value}\n"
        "export fn makeGeneric()->fn()->i64{return fn()->i64{return identity<i64>(41)}}\n"
        "export fn makeCell(base:i64)->fn()->i64{var captured=base;return fn()->i64{return captured}}\n"
        "export fn makeSlot()->fn()->i64{return fn()->i64{return seed}}\n"
        "export fn makeText()->fn()->string{return fn()->string{return \"owned\"}}\n"
        "export fn makeIndirect(f:fn()->i64)->fn()->i64{return fn()->i64{return f()}}\n"
        "export fn makeStoredIndirect(f:fn()->i64)->fn()->i64{var target=f;"
        "return fn()->i64{return target()}}\n"
        "export fn filterEqual()->i64{var data=[1,2,3,4];"
        "return len(data.filter(fn(x:i64)->bool{return x==2}))}\n"
        "export fn filterRemainder()->i64{var data=[1,2,3,4];"
        "return len(data.filter(fn(x:i64)->bool{return x%2==0}))}\n"
        "export fn filterCaptured()->i64{var pivot:i64=2;var data=[1,2,3,4];"
        "return len(data.filter(fn(x:i64)->bool{return x==pivot}))}\n"
        "export fn filterLess()->i64{var data=[1,2,3,4];"
        "return len(data.filter(fn(x:i64)->bool{return x<3}))}\n"
        "export fn filterLessEqual()->i64{var data=[1,2,3,4];"
        "return len(data.filter(fn(x:i64)->bool{return x<=3}))}\n"
        "export fn filterGreater()->i64{var data=[1,2,3,4];"
        "return len(data.filter(fn(x:i64)->bool{return x>3}))}\n"
        "export fn filterGreaterEqual()->i64{var data=[1,2,3,4];"
        "return len(data.filter(fn(x:i64)->bool{return x>=3}))}\n"
        "export fn filterCondition()->i64{var data=[1,2,3,4];"
        "return len(data.filter(fn(x:i64)->bool{if(x<3){return true}else{return false}}))}\n"
        "enum PredicateFailure{Bad}\n"
        "export fn throwingCondition() throws(PredicateFailure)->bool{var f=fn(x:i64)->bool{"
        "if(x<3){throw PredicateFailure.Bad};return false};return try f(4)}\n"
        "export fn equalFloatControl()->bool{var f=fn(x:f64)->bool{return x==2.0};return f(2.0)}\n"
        "export fn notEqualControl()->bool{var f=fn(x:i64)->bool{return x!=2};return f(2)}\n"
        "export fn makeCounter(base:i64)->fn()->i64{var count=base;"
        "return fn()->i64{count=count+1;return count}}\n"
        "export fn makeComposed(f:fn(i64)->i64,g:fn(i64)->i64)->fn(i64)->i64{"
        "return fn(x:i64)->i64{return f(g(x))}}\n"
        "export fn writeBoolControl()->bool{var flag=false;"
        "var f=fn()->bool{flag=true;return flag};return f()}\n";
    /* Compile the actual captured-body declaration instead of a second fixture
     * spelling that could diverge from the standard library. */
    FILE *file=fopen(XR_SOURCE_STDLIB "/_probe/_probe.xr","rb");
    CHECK(file && !fseek(file,0,SEEK_END));
    long size=ftell(file);CHECK(size>0 && !fseek(file,0,SEEK_SET));
    char *bytes=xr_malloc((size_t)size+1);CHECK(bytes);
    CHECK(fread(bytes,1,(size_t)size,file)==(size_t)size && !fclose(file));bytes[size]=0;
    char *first=strstr(bytes,"export fn probeMakeAdder(");CHECK(first);
    char *last=strchr(first,'{');CHECK(last);
    uint32_t depth=1;
    for (++last;depth && *last;++last) {
        if (*last=='{') ++depth;
        else if (*last=='}') --depth;
    }
    CHECK(!depth);
    size_t declaration=(size_t)(last-first),prefix_length=sizeof(prefix)-1;
    CHECK(declaration<SIZE_MAX-prefix_length);
    *length=prefix_length+declaration;
    char *text=xr_malloc(*length+1);CHECK(text);
    memcpy(text,prefix,prefix_length);memcpy(text+prefix_length,first,declaration);text[*length]=0;
    memset(bytes,0xa5,(size_t)size);xr_free(bytes);
    return text;
}

static void check_source_bounds(const XrXirArtifact *artifact,bool consumer) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    CHECK(module->defaults);
    uint32_t literal=source_named_function(module,"withDefault"),call=source_named_function(module,"withCall");
    bool default_literal=false,default_call=false,default_unknown=false,constructor=false;
    uint32_t literals=0,fields=0;
    for (uint32_t d=0;d<module->defaults->count;++d) {
        const XrXirDefaultBinding *binding=&module->defaults->records[d];
        CHECK(binding->function<module->function_count);
        if (binding->owner==literal) {
            CHECK(module->functions[binding->function].error_type==XR_XIR_NEVER);
            default_literal=true;
        }
        if (binding->owner==call) {
            CHECK(module->functions[binding->function].error_type==XR_XIR_NEVER);
            default_call=true;
        }
        if (binding->owner==source_named_function(module,"withLiterals")) {
            CHECK(binding->ordinal<3 && module->functions[binding->function].error_type==XR_XIR_NEVER);
            XrXirType expected[]={XR_XIR_BOOL,XR_XIR_STRING,XR_XIR_I64};
            CHECK(module->functions[binding->function].result==expected[binding->ordinal]);
            literals|=1u<<binding->ordinal;
        }
        if (binding->owner==source_named_function(module,"withUnknown")) {
            CHECK(module->functions[binding->function].error_type==XR_XIR_NEVER);
            default_unknown=true;
        }
    }
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        CHECK(function->error_type!=XR_XIR_UNIT);
        if (function->name_length==14 && !memcmp(function->name,"$field_default",14)) {
            CHECK(module->declarations->functions[f].nominal_owner);
            CHECK(function->error_type==XR_XIR_NEVER);
            fields|=function->result==XR_XIR_I64?1u:function->result==XR_XIR_BOOL?2u:
                function->result==XR_XIR_STRING?4u:0u;
        }
        if (function->name_length==8 && !memcmp(function->name,"$default",8)) {
            CHECK(function->error_type==XR_XIR_NEVER);
            constructor=true;
        }
    }
    CHECK(default_literal && default_call && default_unknown && constructor && literals==7u && fields==7u);
    CHECK(module->functions[literal].error_type==XR_XIR_NEVER);
    CHECK(module->functions[call].error_type==XR_XIR_NEVER);
    CHECK(module->functions[source_named_function(module,"raw")].error_type==
        (consumer ? XR_XIR_ERROR : XR_XIR_NEVER));
    const char *empty[]={"probeMakeAdder","makeGeneric","makeCell","makeSlot","makeText",
        "filterEqual","filterRemainder","filterCaptured","makeCounter",
        "filterLess","filterLessEqual","filterGreater","filterGreaterEqual","filterCondition"};
    for (size_t i=0;i<sizeof(empty)/sizeof(empty[0]);++i) {
        const XrXirFunction *closure=&module->functions[source_closure_function(module,empty[i])];
        if (closure->error_type!=XR_XIR_NEVER)
            fprintf(stderr,"closure %s E=%u expected Never\n",empty[i],(unsigned)closure->error_type);
        CHECK(closure->error_type==XR_XIR_NEVER);
    }
    check_source_indirect_bounds(module,&module->functions[source_closure_function(module,"makeIndirect")],1u,1u);
    check_source_indirect_bounds(module,&module->functions[source_closure_function(module,"makeStoredIndirect")],1u,2u);
    CHECK(xr_xir_type_is_enum(module->types,module->functions[source_closure_function(module,"throwingCondition")].error_type));
    CHECK(module->functions[source_closure_function(module,"equalFloatControl")].error_type==XR_XIR_NEVER);
    CHECK(module->functions[source_closure_function(module,"notEqualControl")].error_type==XR_XIR_NEVER);
    CHECK(module->functions[source_closure_function(module,"writeBoolControl")].error_type==XR_XIR_NEVER);
    const XrXirFunction *composed=&module->functions[source_closure_function(module,"makeComposed")];
    CHECK(composed->error_type==XR_XIR_NEVER && composed->parameter_count==3);
    check_source_indirect_bounds(module,composed,1u,1u);
    for (uint32_t p=0;p<2;++p) {
        const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,composed->parameters[p]);
        CHECK(signature && signature->element==XR_XIR_NEVER);
    }
    CHECK(composed->parameters[2]==XR_XIR_I64);
    if (consumer) {
        CHECK(module->functions[source_closure_function(module,"viaClosure")].error_type==XR_XIR_NEVER);
        CHECK(module->functions[source_closure_function(module,"viaWide")].error_type==XR_XIR_ERROR);
        check_source_indirect_bounds(module,&module->functions[source_closure_function(module,"viaWideIndirect")],3u,3u);
    }
    for (uint32_t m=0;m<module->declarations->module_count;++m) {
        uint32_t initializer=module->declarations->modules[m].initializer;
        CHECK(module->functions[initializer].error_type==XR_XIR_ERROR);
    }
}

/* An external producer may declare a wider Error even for a literal body.
 * A receiving Source must preserve that declaration instead of shrinking it. */
static void source_packet_widen_raw(XrXirCheckedPacket *packet) {
    uint8_t pattern[19]={3,0,0,0,'r','a','w'};
    for (unsigned i=0;i<4;++i) {
        pattern[11+i]=(uint8_t)((uint32_t)XR_XIR_I64>>(8*i));
        pattern[15+i]=(uint8_t)((uint32_t)XR_XIR_NEVER>>(8*i));
    }
    size_t found=0,offset=0;
    for (size_t at=64;at+sizeof(pattern)<=packet->length;++at)
        if (!memcmp(packet->bytes+at,pattern,sizeof(pattern))) {++found;offset=at+15;}
    CHECK(found==1);
    for (unsigned i=0;i<4;++i) packet->bytes[offset+i]=(uint8_t)((uint32_t)XR_XIR_ERROR>>(8*i));
    XrSHA256Context digest;xr_sha256_init(&digest);
    xr_sha256_update(&digest,packet->bytes,32);
    xr_sha256_update(&digest,packet->bytes+64,packet->length-64);
    xr_sha256_final(&digest,packet->bytes+32);
}

static void check_imported_default_bound(const XrXirArtifact *artifact,bool wide) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    CHECK(module && module->defaults);
    uint32_t owner=source_named_function(module,"fromDefault"),callee=source_named_function(module,"raw");
    CHECK(module->functions[callee].error_type==(wide?XR_XIR_ERROR:XR_XIR_NEVER));
    bool found=false;
    for (uint32_t d=0;d<module->defaults->count;++d) {
        const XrXirDefaultBinding *binding=&module->defaults->records[d];
        if (binding->owner!=owner) continue;
        CHECK(binding->owner_kind==XR_XIR_DEFAULT_PARAMETER && !binding->ordinal);
        CHECK(binding->function<module->function_count);
        const XrXirFunction *helper=&module->functions[binding->function];
        CHECK(helper->result==XR_XIR_I64 && helper->error_type==(wide?XR_XIR_ERROR:XR_XIR_NEVER));
        for (uint32_t i=0;i<helper->instruction_count;++i)
            if (helper->instructions[i].op==XR_XIR_CALL) {
                CHECK(helper->instructions[i].immediate==callee);
                found=true;
            }
    }
    CHECK(found && module->functions[owner].error_type==XR_XIR_NEVER);
}

static void check_imported_default(const XrXirCompileContext *work,bool wide) {
    char directory[XR_TEST_PATH_MAX]="xir-default-declaration-XXXXXX";
    CHECK(xr_test_mkdtemp(directory));
    char absolute[XR_TEST_PATH_MAX],library[XR_TEST_PATH_MAX],root[XR_TEST_PATH_MAX];
    CHECK(xr_test_realpath_buf(directory,absolute,sizeof(absolute)));
    CHECK(snprintf(library,sizeof(library),"%s/lib.xr",absolute)>0);
    CHECK(snprintf(root,sizeof(root),"%s/root.xr",absolute)>0);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,absolute};
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(work->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceRequest request={session,library,&authority,work,XR_SOURCE_STDLIB,NULL,XR_XIR_LIBRARY,NULL};
    char producer[]="export fn raw()->i64{return 7}\n";
    XrXirSourceText input={"lib.xr",producer,sizeof(producer)-1};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_source_check_text(&request,&input,&source,&diagnostic,NULL)==XR_XIR_OK);
    const XrXirDefaultTable *defaults=xr_xir_compile_artifact_module(source.checked)->defaults;
    CHECK(!defaults || !defaults->count);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    xr_compile_session_free(session);session=NULL;
    memset(producer,0xa5,sizeof(producer));
    /* This library has no dependent helper: a wider declaration over its pure
     * body remains truthful before the receiving Source constructs a default. */
    if (wide) source_packet_widen_raw(&packet);
    XrXirLibraryModuleInput binding={authority,"lib.xr"};
    XrXirLibraryInput library_input={packet.bytes,packet.length,{0},&binding,1};
    xr_sha256(packet.bytes,packet.length,library_input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(work,&library_input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_compile_session_new(work->resources,&session)==XR_COMPILE_RESOURCE_OK);
    request.session=session;request.entry_path=root;request.linkage_kind=XR_XIR_PROGRAM;request.libraries=catalog;
    char consumer[]="import {raw} from \"./lib\"\n"
        "export fn fromDefault(x:i64=try raw())->i64{return x}\ntry fromDefault()\n";
    input=(XrXirSourceText){"root.xr",consumer,sizeof(consumer)-1};
    CHECK(xr_xir_compile_source_check_text(&request,&input,&source,&diagnostic,NULL)==XR_XIR_OK);
    memset(consumer,0xa5,sizeof(consumer));
    xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    check_imported_default_bound(source.checked,wide);
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    XrXirArtifact *received=NULL;
    CHECK(xr_xir_compile_checked_read(work,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    check_imported_default_bound(received,wide);
    CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(received);
    CHECK(xr_test_rmdir(directory)==0);
}

static void check_source_transport(const XrXirCompileContext *work) {
    char directory[XR_TEST_PATH_MAX]="xir-error-transport-XXXXXX";
    CHECK(xr_test_mkdtemp(directory));
    char absolute[XR_TEST_PATH_MAX],library[XR_TEST_PATH_MAX],root[XR_TEST_PATH_MAX];
    CHECK(xr_test_realpath_buf(directory,absolute,sizeof(absolute)));
    CHECK(snprintf(library,sizeof(library),"%s/lib.xr",absolute)>0);
    CHECK(snprintf(root,sizeof(root),"%s/root.xr",absolute)>0);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,absolute};
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(work->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceRequest request={session,library,&authority,work,XR_SOURCE_STDLIB,NULL,XR_XIR_LIBRARY,NULL};
    size_t text_length=0;char *text=source_transport_text(&text_length);
    XrXirSourceText input={"lib.xr",text,text_length};
    XrXirSourceResult source={0};
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&source,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"source E producer %u at %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK);
    memset(text,0xa5,text_length);xr_free(text);
    check_source_bounds(source.checked,false);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    xr_compile_session_free(session);
    session=NULL;
    source_packet_widen_raw(&packet);
    XrXirLibraryModuleInput binding={authority,"lib.xr"};
    XrXirLibraryInput library_input={packet.bytes,packet.length,{0},&binding,1};
    xr_sha256(packet.bytes,packet.length,library_input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(work,&library_input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);
    xr_xir_compile_checked_packet_free(&packet);

    CHECK(xr_compile_session_new(work->resources,&session)==XR_COMPILE_RESOURCE_OK);
    request.session=session;request.entry_path=root;request.linkage_kind=XR_XIR_PROGRAM;request.libraries=catalog;
    char consumer[]="import {withDefault,withCall,Box,raw,probeMakeAdder} from \"./lib\"\n"
        "export fn answer()->i64{return withDefault()+withCall()+Box().value}\n"
        "export fn viaClosure()->i64{var f=fn()->i64{return 9};return f()}\n"
        "export fn viaWide() throws(Error)->i64{var f=fn()->i64{return try raw()};return try f()}\n"
        "export fn captured()->i64{var f=probeMakeAdder(40);return f(2)}\n"
        "export fn viaWideIndirect(f:fn()->i64) throws(Error)->i64{var target=raw;"
        "var local=fn()->i64{f();return try target()};return try local()}\n";
    input=(XrXirSourceText){"root.xr",consumer,sizeof(consumer)-1};
    status=xr_xir_compile_source_check_text(&request,&input,&source,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"source E receiver %u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK);
    memset(consumer,0xa5,sizeof(consumer));
    xr_compile_session_free(session);
    xr_xir_compile_library_catalog_free(catalog);
    check_source_bounds(source.checked,true);
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    XrXirArtifact *received=NULL;
    CHECK(xr_xir_compile_checked_read(work,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    check_source_bounds(received,true);
    CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(received);
    CHECK(xr_test_rmdir(directory)==0);
}

#include "xir_callable_error_cases.h"

static void check_catch_binding_names(const XrXirCompileContext *work) {
    char directory[XR_TEST_PATH_MAX]="xir-catch-binding-XXXXXX";
    CHECK(xr_test_mkdtemp(directory));
    char absolute[XR_TEST_PATH_MAX],root[XR_TEST_PATH_MAX];
    CHECK(xr_test_realpath_buf(directory,absolute,sizeof(absolute)));
    CHECK(snprintf(root,sizeof(root),"%s/root.xr",absolute)>0);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,absolute};
    const struct { const char *clause; bool valid; } cases[]={
        {"catch (Fault)",false},
        {"catch (Typo)",false},
        {"catch (\xCE\x94)",false},
        {"catch (\xC4\x82)",false},
        {"catch (\xD0\x81)",false},
        {"catch (\xF0\x90\x90\x80)",false},
        {"catch (error)",true},
        {"catch (Fault: Fault)",true},
        {"catch (Fault.Bad)",true},
        {"catch panic (P)",true},
        {"catch (\xCE\xB4)",true},
        {"catch (\xCE\xA2)",true},
        {"catch (_Fault)",true},
    };
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        char text[256];
        int length=snprintf(text,sizeof(text),
            "enum Fault { Bad }\nfn caller() {\n    try {} %s {}\n}\n",cases[i].clause);
        CHECK(length>0 && (size_t)length<sizeof(text));
        XrCompilerSession *session=NULL;
        CHECK(xr_compile_session_new(work->resources,&session)==XR_COMPILE_RESOURCE_OK);
        XrXirSourceRequest request={session,root,&authority,work,XR_SOURCE_STDLIB,NULL,XR_XIR_LIBRARY,NULL};
        XrXirSourceText input={"root.xr",text,(size_t)length};
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);
        xr_compile_session_free(session);
        if ((status==XR_XIR_OK)!=cases[i].valid)
            fprintf(stderr,"catch binding %s: %u at %d:%d: %s\n",cases[i].clause,
                status,diagnostic.line,diagnostic.column,diagnostic.message);
        if (cases[i].valid) {
            CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
            CHECK(xr_xir_compile_artifact_verify(result.checked,NULL)==XR_XIR_OK);
        } else {
            CHECK(status==XR_XIR_BAD_TYPE && diagnostic.status==status);
            CHECK(!result.checked && !result.snapshot);
            CHECK(diagnostic.line==3 && diagnostic.column==19);
            CHECK(strncmp(diagnostic.message,"E0334:",6)==0);
            CHECK(strstr(diagnostic.message,"catch (e: TypeName)"));
        }
        xr_xir_compile_source_result_free(&result);
    }
    CHECK(xr_test_rmdir(directory)==0);
}

int main(void) {
    xr_test_suppress_dialogs();
    XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_ORDINARY};
    XrXirCompileContext work={0};
    CHECK(xr_compile_resources_new(&limits,&work.resources)==XR_COMPILE_RESOURCE_OK);
    work.limits=xr_xir_compile_default_limits();
    check_catch_binding_names(&work);
    check_callable_errors(&work);
    NeverFixture *producer=xr_malloc(sizeof(*producer));
    CHECK(producer);
    fixture_init(producer);
    CHECK(xr_xir_compile_type_descriptors_verify(&work,&producer->types)==XR_XIR_OK);
    check_error_expression(&work,&producer->types);
    check_no_value(&work,&producer->types);
    XrXirTypes *owned=NULL;
    CHECK(xr_xir_compile_types_clone(&work,&producer->types, NULL,&owned)==XR_XIR_OK);
    xr_free(producer);
    CHECK(owned && owned->nodes[0].nominal.arguments[0]==XR_XIR_NEVER);
    CHECK(xr_xir_compile_type_descriptors_verify(&work,owned)==XR_XIR_OK);
    check_error_expression(&work,owned);
    check_no_value(&work,owned);
    xr_xir_compile_types_free(owned);
    check_declaration_packet(&work,XR_XIR_NEVER);
    check_declaration_packet(&work,XR_XIR_ERROR);
    check_declaration_packet(&work,(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE);
    check_generic_declaration(&work);
    check_generic_error_argument(&work);
    check_lowered_error_remap(&work);
    check_source_transport(&work);
    check_imported_default(&work,false);
    check_imported_default(&work,true);
    xr_compile_resources_release(work.resources);
    puts("Error declarations, substitution, owned packets and Source library transport passed");
    return 0;
}
