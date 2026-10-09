/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_atomic_admission_owner.c - Closed definition proofs before execution
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_types.h"
#include "xir/xxir_declarations.h"
#include "xir/xxir_type_scratch_internal.h"
#include "toolchain/xcompiler_session.h"
#include "shared/xnative_declaration.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);} } while(0)
#include "xir_library_compile_owner.h"
#include "xir_checked_scalar60_golden.h"
#include "xir_atomic_nominal_cases.h"
#include "xir/xxir_operand_roles.h"
#include "xir_atomic_instruction_cases.h"
#include "xir_atomic_instruction_reject_cases.h"
static char root_path[XR_TEST_PATH_MAX],directory_path[XR_TEST_PATH_MAX];
static void write_source(const char *source) {
    FILE *f=fopen(root_path,"wb");CHECK(f);size_t n=strlen(source);
    CHECK(fwrite(source,1,n,f)==n && fclose(f)==0);
}
static XrXirStatus source_pipeline(const XrXirCompileContext *context,void *unused) {
    (void)unused;XrCompilerSession *session=NULL;XrCompilerSessionStatus session_status=xr_compile_session_new(context->resources,&session);
    if(session_status!=XR_COMPILER_SESSION_OK)return session_status==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,directory_path};
    XrXirSourceRequest request={session,root_path,&authority,context,XR_ATOMIC_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirArtifact *read=NULL,*specialized=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&source,&diagnostic,NULL);
    if(status==XR_XIR_OK){
        CHECK(source.checked && source.snapshot);
        const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(source.snapshot);CHECK(view);
        bool governed=false;
        for(uint32_t i=0;i<view->declaration_count;++i){
            const XrXirSourceDeclaration *d=&view->declarations[i];
            if(d->native_identity==XR_NATIVE_DECLARATION_ATOMIC){
                CHECK(!strcmp(d->name,"Atomic") && d->generic_parameter_count==1 && d->generic_constraints);
                CHECK(d->generic_constraints[0].markers==XR_XIR_CONSTRAINT_ATOMIC_VALUE);governed=true;
            }
        }
        for(uint32_t i=0;i<view->declaration_count;++i){
            const XrXirSourceDeclaration *d=&view->declarations[i];
            if(d->native_identity==XR_NATIVE_DECLARATION_ORDERING){
                CHECK(!strcmp(d->name,"Ordering") && !d->generic_parameter_count && d->exported);governed=true;
            }
        }
        CHECK(governed);
        status=xr_xir_compile_checked_write(source.checked,&packet,NULL);
        if(status!=XR_XIR_OK)CHECK(!packet.bytes && !packet.length);
    }else CHECK(!source.checked && !source.snapshot);
    xr_compile_session_free(session);session=NULL;
    if(status==XR_XIR_OK){status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);if(status!=XR_XIR_OK)CHECK(!read);}
    if(status==XR_XIR_OK){status=xr_xir_compile_specialize(read,&specialized,NULL);if(status!=XR_XIR_OK)CHECK(!specialized);}
    if(status==XR_XIR_OK){status=xr_xir_compile_lower(specialized,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL);if(status!=XR_XIR_OK)CHECK(!lowered);}
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(lowered,NULL);
    if(status!=XR_XIR_OK && source_program_compile_fail_at==SIZE_MAX)
        fprintf(stderr,"source pipeline status=%u diagnostic=%s\n",status,diagnostic.message);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(specialized);
    xr_xir_compile_artifact_free(read);xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&source);
    return status;
}
static void marker_and_shape_cases(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrXirTypeNode node={.kind=XR_XIR_TYPE_ATOMIC,.element=XR_XIR_I64};
    XrXirTypes types={.nodes=&node,.count=1};
    XrXirInstruction ret={.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={0,0}};
    XrXirBlock block={.first=0,.count=1};XrXirType parameter=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirFunction function={.name="keep",.name_length=4,.parameters=&parameter,.parameter_count=1,
        .result=parameter,.blocks=&block,.block_count=1,.instructions=&ret,.instruction_count=1};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=&function,.function_count=1,.types=&types};
    XrXirProofContext context={&module,{XR_XIR_CONTEXT_FUNCTION,0,0}};
    const XrXirType leaves[]={XR_XIR_I64,XR_XIR_F64,XR_XIR_BOOL};
    for(unsigned i=0;i<3;++i){node.element=leaves[i];CHECK(xr_xir_compile_types_structure_verify(&owner.context,&types)==XR_XIR_OK);
        CHECK(xr_xir_compile_type_markers_prove(&owner.context,&context,parameter,XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_OK);
        CHECK(xr_xir_compile_type_markers_prove(&owner.context,&context,parameter,XR_XIR_CONSTRAINT_EQUAL)==XR_XIR_BAD_TYPE);
    }
    const XrXirType invalid[]={XR_XIR_UNIT,XR_XIR_STRING,XR_XIR_I8,XR_XIR_U64,XR_XIR_F32,(XrXirType)4,XR_XIR_ERROR};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i){node.element=invalid[i];CHECK(xr_xir_compile_types_structure_verify(&owner.context,&types)==XR_XIR_BAD_TYPE);}
    node.element=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;node.parameter_span=1;
    XrXirConstraint fact={0};XrXirGeneric generic={.constraints=&fact,.parameter_count=1};module.generics=&generic;
    for(unsigned marker=0;marker<3;++marker){
        fact.markers=marker==0?XR_XIR_CONSTRAINT_ATOMIC_VALUE:marker==1?XR_XIR_CONSTRAINT_ATOMIC_NUMBER:XR_XIR_CONSTRAINT_ATOMIC_BOOLEAN;
        CHECK(xr_xir_compile_type_use_verify(&owner.context,&context,parameter)==XR_XIR_OK);
        CHECK(xr_xir_compile_type_markers_prove(&owner.context,&context,node.element,XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_OK);
        CHECK(xr_xir_compile_type_markers_prove(&owner.context,&context,node.element,XR_XIR_CONSTRAINT_EQUAL)==XR_XIR_BAD_TYPE);
    }
    fact.markers=XR_XIR_CONSTRAINT_ATOMIC_VALUE;
    XrXirArtifact *checked=NULL;
    CHECK(xir_fixture_check(&owner.context, &module, &checked, NULL)==XR_XIR_OK && checked);
    XrXirArtifact *rejected=NULL;
    fact.markers=0;
    CHECK(xir_fixture_check(&owner.context, &module, &rejected, NULL)==XR_XIR_BAD_TYPE && !rejected);
    xr_xir_compile_artifact_free(checked);
    fact.markers=XR_XIR_CONSTRAINT_SENDABLE;CHECK(xr_xir_compile_type_use_verify(&owner.context,&context,parameter)==XR_XIR_BAD_TYPE);
    fact.markers=0;CHECK(xr_xir_compile_type_use_verify(&owner.context,&context,parameter)==XR_XIR_BAD_TYPE);
    for(unsigned i=0;i<3;++i){fact.markers=XR_XIR_CONSTRAINT_ATOMIC_VALUE;CHECK(xr_xir_compile_type_markers_prove(&owner.context,&context,leaves[i],XR_XIR_CONSTRAINT_ATOMIC_VALUE)==XR_XIR_OK);}
    fact.markers=XR_XIR_CONSTRAINT_ATOMIC_VALUE;
    CHECK(xr_xir_compile_type_markers_prove(&owner.context,&context,node.element,XR_XIR_CONSTRAINT_ATOMIC_NUMBER)==XR_XIR_BAD_TYPE);
    fact.markers=XR_XIR_CONSTRAINT_ATOMIC_NUMBER;
    CHECK(xr_xir_compile_type_markers_prove(&owner.context,&context,node.element,XR_XIR_CONSTRAINT_ATOMIC_BOOLEAN)==XR_XIR_BAD_TYPE);
    library_compile_owner_drop(&owner);
}
static void source_cases(void) {
    const char *positive[]={
        "fn hold<T: AtomicValue>(x: Atomic<T>) -> Atomic<T> { return x }\nconst f=hold<i64>\nfn main() -> i64 { return 7 }\n",
        "fn hold<T: AtomicNumber>(x: Atomic<T>) -> Atomic<T> { return x }\nconst f=hold<f64>\nfn main() -> i64 { return 7 }\n",
        "fn hold<T: AtomicBoolean>(x: Atomic<T>) -> Atomic<T> { return x }\nconst f=hold<bool>\nfn main() -> i64 { return 7 }\n"};
    for(unsigned i=0;i<3;++i){write_source(positive[i]);library_compile_operation_cases(i==0?"Atomic<i64>":i==1?"Atomic<f64>":"Atomic<bool>",source_pipeline,NULL);}
    const char *negative[]={
        "fn unused<T>(x: Atomic<T>) {}\n",
        "fn unused<T: Sendable>(x: Atomic<T>) {}\n",
        "fn unused(x: Atomic<string>) {}\n",
        "fn unused(x: Atomic<i32>) {}\n",
        "fn unused(x: Atomic<f32>) {}\n",
        "struct S {}\nfn unused(x: Atomic<S>) {}\n",
        "fn unused(x: Atomic<i64?>) {}\n",
        "interface AtomicValue {}\nfn unused<T: AtomicValue>(x: Atomic<T>) {}\n",
        };
    const char *current_admitted[]={"const x=Atomic(1)\n","fn unused(x: Ordering) {}\n"};
    for(unsigned i=0;i<2;++i){write_source(current_admitted[i]);library_compile_operation_cases(i==0?"original Atomic(1) now admitted":"original Ordering parameter now admitted",source_pipeline,NULL);}
    for(unsigned i=0;i<sizeof(negative)/sizeof(*negative);++i){
        write_source(negative[i]);LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,directory_path};
        XrXirSourceRequest request={session,root_path,&authority,&owner.context,XR_ATOMIC_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check(&request,&output,&diagnostic,NULL);
        if(status!=XR_XIR_BAD_TYPE)fprintf(stderr,"negative%u status%u %s\n",i,status,diagnostic.message);
        CHECK(status==XR_XIR_BAD_TYPE && !output.checked && !output.snapshot);
        xr_compile_session_free(session);library_compile_owner_drop(&owner);
    }
}
static void pool_and_access_cases(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrXirTypeNode nodes[3]={{.kind=XR_XIR_TYPE_ATOMIC,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_ATOMIC,.element=XR_XIR_F64},{.kind=XR_XIR_TYPE_ATOMIC,.element=XR_XIR_BOOL}};
    XrXirTypes types={.nodes=nodes,.count=3};
    CHECK(xr_xir_compile_types_structure_verify(&owner.context,&types)==XR_XIR_OK);
    /* An unused suffix must not evade the full pool verifier. */
    nodes[2].element=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2);
    CHECK(xr_xir_compile_types_structure_verify(&owner.context,&types)==XR_XIR_BAD_TYPE);
    nodes[2].element=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    CHECK(xr_xir_compile_types_structure_verify(&owner.context,&types)==XR_XIR_BAD_TYPE);
    nodes[2].element=XR_XIR_BOOL;nodes[2].result=XR_XIR_I64;
    CHECK(xr_xir_compile_types_structure_verify(&owner.context,&types)==XR_XIR_BAD_STRUCTURE);
    nodes[2].result=XR_XIR_UNIT;
    CHECK(xr_xir_compile_types_structure_verify(&owner.context,&types)==XR_XIR_OK);
    /* Access is independently rechecked even for a hostile Atomic<Hidden>
     * expression which the closed eligibility predicate will also reject. */
    XrXirNominalDeclaration hidden={.module={"home",4},.name={"Hidden",6},.kind=XR_XIR_NOMINAL_STRUCT};
    XrXirNominalTable nominals={.declarations=&hidden,.count=1};
    nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL};
    nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ATOMIC,.element=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE};
    types.count=2;types.nominals=&nominals;
    XrXirSourceModule modules[2]={{"home",4,NULL,0,0},{"away",4,NULL,0,1}};
    XrXirFunctionIdentity identities[2]={{.module=0},{.module=1}};
    XrXirDeclarations declarations={.modules=modules,.module_count=2,.functions=identities};
    XrXirFunction functions[2]={{0},{0}};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=2,.types=&types,.declarations=&declarations};
    XirTypeScratch scratch={owner.context.resources,NULL,0};
    XrXirType atomic=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
    CHECK(xr_xir_compile_type_access_scratch(&owner.context,&module,0,atomic,&scratch)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_access_scratch(&owner.context,&module,1,atomic,&scratch)==XR_XIR_BAD_TYPE);
    hidden.exported=1;
    CHECK(xr_xir_compile_type_access_scratch(&owner.context,&module,1,atomic,&scratch)==XR_XIR_BAD_TYPE);
    uint32_t imported=0;modules[1].dependencies=&imported;modules[1].dependency_count=1;
    CHECK(xr_xir_compile_type_access_scratch(&owner.context,&module,1,atomic,&scratch)==XR_XIR_OK);
    hidden.exported=0;
    CHECK(xr_xir_compile_type_access_scratch(&owner.context,&module,1,atomic,&scratch)==XR_XIR_BAD_TYPE);
    xir_type_scratch_free(&scratch);library_compile_owner_drop(&owner);
}
static void shadow_cases(void) {
    const char *sources[]={"class Atomic<T> {}\nfn unused(x:Atomic<i64>) {}\n",
        "fn unused(x:Atomic<i64>) {}\nstruct Atomic<T> {}\n"};
    for(unsigned i=0;i<2;++i){
        write_source(sources[i]);LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,directory_path};
        XrXirSourceRequest request={session,root_path,&authority,&owner.context,XR_ATOMIC_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
        if(status!=XR_XIR_OK)fprintf(stderr,"shadow%u status%u %s\n",i,status,diagnostic.message);
        CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
        const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);CHECK(view && view->types);
        bool found=false;
        for(uint32_t d=0;d<view->declaration_count;++d){
            const XrXirSourceDeclaration *decl=&view->declarations[d];
            CHECK(decl->native_identity!=XR_NATIVE_DECLARATION_ATOMIC);
            if(decl->kind==XR_XIR_SOURCE_FUNCTION && !strcmp(decl->name,"unused")){
                CHECK(decl->parameter_count==1 && decl->parameters && decl->parameters[0].known);
                const XrXirTypeNode *node=xr_xir_type_node(view->types,decl->parameters[0].type);
                CHECK(node && node->kind==XR_XIR_TYPE_NOMINAL && node->nominal.argument_count==1 && node->nominal.arguments[0]==XR_XIR_I64);found=true;
            }
        }
        CHECK(found);xr_compile_session_free(session);xr_xir_compile_source_result_free(&result);library_compile_owner_drop(&owner);
    }
}
static void authority_and_history(void) {
    const XrNativeTypeDeclaration *atomic=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ATOMIC);
    const XrNativeTypeDeclaration *ordering=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ORDERING);
    CHECK(atomic && ordering && atomic->kind==XR_NATIVE_DECLARATION_IDENTITY && atomic->member_count==10 && ordering->kind==XR_NATIVE_DECLARATION_VALUE && ordering->line && ordering->member_count==5);
    CHECK(xr_native_declaration_validate(atomic) && xr_native_declaration_validate(ordering));
    for(unsigned i=0;i<5;++i)CHECK(ordering->members[i].line && ordering->members[i].column && ordering->members[i].is_static && !ordering->members[i].lowered);
    const uint32_t expected_arity[]={1,2,2,2,2,2,2,3,1,0};
    for(unsigned i=0;i<10;++i){CHECK(atomic->members[i].parameter_count==expected_arity[i]);
        if(expected_arity[i])CHECK(atomic->members[i].parameters[expected_arity[i]-1].optional);
        const XrNativeOperation operations[]={XR_NATIVE_OPERATION_ATOMIC_LOAD,XR_NATIVE_OPERATION_ATOMIC_STORE,
            XR_NATIVE_OPERATION_ATOMIC_ADD,XR_NATIVE_OPERATION_ATOMIC_SUB,XR_NATIVE_OPERATION_ATOMIC_FETCH_ADD,
            XR_NATIVE_OPERATION_ATOMIC_FETCH_SUB,XR_NATIVE_OPERATION_ATOMIC_SWAP,XR_NATIVE_OPERATION_ATOMIC_COMPARE_EXCHANGE,
            XR_NATIVE_OPERATION_ATOMIC_TOGGLE,XR_NATIVE_OPERATION_ATOMIC_TO_STRING};
        CHECK(atomic->members[i].operation==operations[i] && atomic->members[i].lowered);
    }
    XrNativeTypeDeclaration fake=*ordering;fake.id=XR_NATIVE_DECLARATION_ATOMIC;CHECK(!xr_native_declaration_validate(&fake));
    fake=*ordering;fake.source_fingerprint.bytes[0]^=1;CHECK(!xr_native_declaration_validate(&fake));
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrXirArtifact *output=NULL;CHECK(xr_xir_compile_checked_read(&owner.context,checked_scalar60_golden,sizeof(checked_scalar60_golden),&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
    library_compile_owner_drop(&owner);
}
int main(void) {
    char relative[XR_TEST_PATH_MAX]="atomic-a1-XXXXXX";CHECK(xr_test_mkdtemp(relative));
    CHECK(xr_test_realpath_buf(relative,directory_path,sizeof(directory_path)));
    CHECK(snprintf(root_path,sizeof(root_path),"%s/main.xr",directory_path)>0);
    XrCompileResources *none=NULL;
    source_program_compile_attempts=0;source_program_compile_fail_at=0;source_program_compile_injected=false;
    CHECK(xr_compile_resources_new(&library_compile_limits,&none)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !none);
    CHECK(source_program_compile_injected && !source_program_compile_live && !source_program_compile_bytes);
    source_program_compile_fail_at=SIZE_MAX;
    marker_and_shape_cases();pool_and_access_cases();authority_and_history();shadow_cases();source_cases();atomic_nominal_cases();atomic_instruction_cases();
    CHECK(remove(root_path)==0);CHECK(xr_test_rmdir(directory_path)==0);
    library_compile_observer_free();fprintf(stderr,"Atomic compiler admission owner PASS; complete runtime/native family OPEN\n");return 0;
}
