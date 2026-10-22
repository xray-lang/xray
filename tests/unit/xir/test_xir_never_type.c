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
    old[8]=(uint8_t)(XR_XIR_CHECKED_SCHEMA-1);
    CHECK(xr_xir_compile_checked_read(work,old,packet.length,&received,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!received);
    memcpy(old,packet.bytes,packet.length);
    old[12]=(uint8_t)(XR_XIR_CHECKED_CONTRACT-1);
    CHECK(xr_xir_compile_checked_read(work,old,packet.length,&received,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!received);
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

static void check_source_bounds(const XrXirArtifact *artifact,bool has_closure) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    CHECK(module->defaults && module->defaults->count==1);
    uint32_t helper=module->defaults->records[0].function;
    CHECK(helper<module->function_count);
    CHECK(module->functions[helper].error_type==XR_XIR_ERROR);
    bool named=false,closure=false;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        CHECK(function->error_type!=XR_XIR_UNIT);
        if (function->name_length==11 && !memcmp(function->name,"withDefault",11)) {
            CHECK(function->error_type==XR_XIR_NEVER);
            named=true;
        }
        if (function->name_length>=8 && !memcmp(function->name,"$closure",8)) {
            CHECK(function->error_type==XR_XIR_ERROR);
            closure=true;
        }
    }
    CHECK(named);
    CHECK(closure==has_closure);
    for (uint32_t m=0;m<module->declarations->module_count;++m) {
        uint32_t initializer=module->declarations->modules[m].initializer;
        CHECK(module->functions[initializer].error_type==XR_XIR_ERROR);
    }
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
    CHECK(xr_compile_session_new(work->resources,&session)==XR_COMPILER_SESSION_OK);
    XrXirSourceRequest request={session,library,&authority,work,XR_SOURCE_STDLIB,NULL,XR_XIR_LIBRARY,NULL};
    const char text[]="export fn withDefault(x:i64=41)->i64{return x+1}\n";
    XrXirSourceText input={"lib.xr",text,sizeof(text)-1};
    XrXirSourceResult source={0};
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&source,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"source E producer %u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK);
    check_source_bounds(source.checked,false);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    xr_compile_session_free(session);
    session=NULL;
    XrXirLibraryModuleInput binding={authority,"lib.xr"};
    XrXirLibraryInput library_input={packet.bytes,packet.length,{0},&binding,1};
    xr_sha256(packet.bytes,packet.length,library_input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(work,&library_input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);
    xr_xir_compile_checked_packet_free(&packet);

    CHECK(xr_compile_session_new(work->resources,&session)==XR_COMPILER_SESSION_OK);
    request.session=session;request.entry_path=root;request.linkage_kind=XR_XIR_PROGRAM;request.libraries=catalog;
    const char consumer[]="import {withDefault} from \"./lib\"\n"
        "export fn answer()->i64{return withDefault()}\n"
        "export fn viaClosure()->i64{var f=fn()->i64{return 9};return f()}\n";
    input=(XrXirSourceText){"root.xr",consumer,sizeof(consumer)-1};
    status=xr_xir_compile_source_check_text(&request,&input,&source,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"source E receiver %u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK);
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

int main(void) {
    xr_test_suppress_dialogs();
    XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrXirCompileContext work={0};
    CHECK(xr_compile_resources_new(&limits,&work.resources)==XR_COMPILE_RESOURCE_OK);
    work.limits=xr_xir_compile_default_limits();
    NeverFixture *producer=xr_malloc(sizeof(*producer));
    CHECK(producer);
    fixture_init(producer);
    CHECK(xr_xir_compile_type_descriptors_verify(&work,&producer->types)==XR_XIR_OK);
    check_error_expression(&work,&producer->types);
    check_no_value(&work,&producer->types);
    XrXirTypes *owned=NULL;
    CHECK(xr_xir_compile_types_clone(&work,&producer->types,&owned)==XR_XIR_OK);
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
    check_source_transport(&work);
    xr_compile_resources_release(work.resources);
    puts("Error declarations, substitution, owned packets and Source library transport passed");
    return 0;
}
