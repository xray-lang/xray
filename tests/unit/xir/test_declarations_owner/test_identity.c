/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_identity.c - Roles are verified semantics and proof-bound identity
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
static const char *packet_directory;
static XrXirArtifact *make(const XrXirCompileContext *context,uint32_t role,uint32_t timeout) {
    const XrXirInstruction unit={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    const XrXirInstruction code[]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},42,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirBlock blocks[]={{0,1,0,0},{0,2,0,0}};
    const XrXirFunction functions[]={{"init",4,NULL,0,XR_XIR_UNIT,blocks,1,&unit,1,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,blocks+1,1,code,2,NULL,0},
        {"test",4,NULL,0,XR_XIR_UNIT,blocks,1,&unit,1,NULL,0},
        {"unused_generic",14,NULL,0,XR_XIR_UNIT,blocks,1,&unit,1,NULL,0}};
    XrXirFunctionIdentity identities[4]={{0},{0},{.test_role=role,.test_timeout_seconds=timeout},{0}};
    const XrXirConstraint unconstrained={0};
    const XrXirGeneric generics[4]={{0},{0},{0},{&unconstrained,1,NULL,0,NULL}};
    const XrXirSourceModule source={"root",4,NULL,0,0};
    const XrXirDeclarations declarations={&source,1,identities,NULL,0,NULL,0,0,1,NULL};
    XrXirModule module={XR_XIR_BUILT,functions,4,&declarations,generics,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_check(context,&module,&checked,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    if(packet_directory) {
        char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%u-%u.chk",packet_directory,role,timeout)>0);
        FILE *file=fopen(path,"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));
    }
    XrXirArtifact *roundtrip=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&roundtrip,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_module(roundtrip)->declarations->functions[2].test_role==role);
    CHECK(xr_xir_compile_artifact_module(roundtrip)->declarations->functions[2].test_timeout_seconds==timeout);
    for(uint32_t field=0;field<2;++field) {
        size_t offset=field?12:8;uint8_t saved=packet.bytes[offset];packet.bytes[offset]=(uint8_t)(saved-1);
        XrXirArtifact *sentinel=(XrXirArtifact *)(uintptr_t)1;
        CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&sentinel,NULL)==XR_XIR_BAD_STRUCTURE);
        CHECK(sentinel==(XrXirArtifact *)(uintptr_t)1);packet.bytes[offset]=saved;
    }
    xr_xir_compile_artifact_free(roundtrip);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_module(closed)->provenance &&
        xr_xir_compile_artifact_module(closed)->function_count==3);
    XrXirFunctionIdentity *owned=(XrXirFunctionIdentity *)xr_xir_compile_artifact_module(closed)->declarations->functions;
    XrXirFunctionIdentity original=owned[2];
    if(timeout) owned[2].test_timeout_seconds=timeout+1;
    else owned[2].test_role=role==XR_XIR_TEST_ROLE_TEST?XR_XIR_TEST_ROLE_NONE:XR_XIR_TEST_ROLE_TEST;
    CHECK(xr_xir_compile_artifact_verify(closed,NULL)==XR_XIR_BAD_STRUCTURE);owned[2]=original;
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);xr_xir_compile_artifact_free(closed);
    return lowered;
}
static void match(const XrXirCompileContext *context,uint32_t role_a,uint32_t timeout_a,uint32_t role_b,uint32_t timeout_b) {
    XrXirArtifact *a=make(context,role_a,timeout_a),*b=make(context,role_b,timeout_b);
    XrXirVmBinding bindings[3];XrXirCallEntry entries[3];
    for(uint32_t f=0;f<3;++f)CHECK(xr_xir_compile_vm_bind(a,f,&bindings[f],&entries[f])==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(a);
    XrXirFunctionIdentity identities[3];memcpy(identities,module->declarations->functions,sizeof(identities));
    identities[2].test_role=role_b;identities[2].test_timeout_seconds=timeout_b;
    XrXirDeclarations declarations=*module->declarations;declarations.functions=identities;
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,*xr_xir_compile_artifact_target(a),entries,3,
        &declarations,{0},module->types,xr_xir_compile_program_proof(b)};
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(context,&spec,&program)==XR_XIR_OK);
    xr_xir_compile_program_drop(program);program=NULL;
    spec.proof=xr_xir_compile_program_proof(a);
    CHECK(xr_xir_compile_program_match(context,&spec,spec.proof.layouts,a)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_xir_compile_program_proof_verify(context,&spec,&spec.proof)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_xir_compile_program_seal(context,&spec,&program)==XR_XIR_BAD_STRUCTURE && !program);
    xr_xir_compile_artifact_free(a);xr_xir_compile_artifact_free(b);
}
static void invalid(const XrXirCompileContext *context) {
    const XrXirInstruction unit={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    const XrXirInstruction code[]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirBlock blocks[]={{0,1,0,0},{0,2,0,0}};
    XrXirFunction functions[]={{"init",4,NULL,0,XR_XIR_UNIT,blocks,1,&unit,1,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,blocks+1,1,code,2,NULL,0},
        {"test",4,NULL,0,XR_XIR_UNIT,blocks,1,&unit,1,NULL,0}};
    XrXirFunctionIdentity identities[3]={{0},{0},{0}};
    const XrXirSourceModule source={"root",4,NULL,0,0};
    const XrXirDeclarations declarations={&source,1,identities,NULL,0,NULL,0,0,1,NULL};
    XrXirModule module={XR_XIR_BUILT,functions,3,&declarations,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    const XrXirFunctionIdentity malformed[]={
        {.test_role=7},{.test_timeout_seconds=1},
        {.test_role=XR_XIR_TEST_ROLE_SKIP,.test_timeout_seconds=1},
        {.test_role=XR_XIR_TEST_ROLE_BEFORE_ALL,.test_timeout_seconds=1},
        {.test_role=XR_XIR_TEST_ROLE_TEST,.test_timeout_seconds=UINT32_MAX},
        {.test_role=XR_XIR_TEST_ROLE_TEST,.nominal_owner=1,.method_kind=XR_XIR_STATIC_METHOD},
        {.test_role=XR_XIR_TEST_ROLE_TEST,.cleanup_owner=1}};
    for(size_t i=0;i<sizeof(malformed)/sizeof(*malformed);++i) {
        identities[2]=malformed[i];XrXirArtifact *out=(XrXirArtifact *)(uintptr_t)1;
        CHECK(xr_xir_compile_check(context,&module,&out,NULL)==XR_XIR_BAD_STRUCTURE);
        CHECK(out==(XrXirArtifact *)(uintptr_t)1);
    }
    identities[2]=(XrXirFunctionIdentity){.test_role=XR_XIR_TEST_ROLE_TEST};
    const XrXirType parameter=XR_XIR_I64;
    const XrXirConstraint constraint={0};
    const XrXirGeneric generics[3]={{0},{0},{&constraint,1,NULL,0,NULL}};
    for(unsigned i=0;i<3;++i) {
        XrXirFunction saved=functions[2];
        if(i==0) {functions[2].parameters=&parameter;functions[2].parameter_count=1;}
        if(i==1) functions[2].result=XR_XIR_I64;
        if(i==2) module.generics=generics;
        XrXirArtifact *out=(XrXirArtifact *)(uintptr_t)1;
        CHECK(xr_xir_compile_check(context,&module,&out,NULL)==XR_XIR_BAD_TYPE);
        CHECK(out==(XrXirArtifact *)(uintptr_t)1);functions[2]=saved;module.generics=NULL;
    }
    identities[2]=(XrXirFunctionIdentity){0};
    for(uint32_t f=0;f<2;++f) {
        identities[f].test_role=XR_XIR_TEST_ROLE_TEST;XrXirArtifact *out=NULL;
        CHECK(xr_xir_compile_check(context,&module,&out,NULL)==XR_XIR_BAD_STRUCTURE && !out);
        identities[f].test_role=XR_XIR_TEST_ROLE_NONE;
    }
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);if(argc==2)packet_directory=argv[1];
    XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    invalid(&context);
    match(&context,XR_XIR_TEST_ROLE_TEST,0,XR_XIR_TEST_ROLE_BEFORE_ALL,0);
    match(&context,XR_XIR_TEST_ROLE_BEFORE_ALL,0,XR_XIR_TEST_ROLE_TEST,0);
    match(&context,XR_XIR_TEST_ROLE_TEST,7,XR_XIR_TEST_ROLE_TEST,8);
    match(&context,XR_XIR_TEST_ROLE_TEST,8,XR_XIR_TEST_ROLE_TEST,7);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==before.live_bytes);
    xr_compile_resources_release(context.resources);
    puts("role/timeout: valid alternate proofs, Checked/provenance/native mismatch rejection PASS");return 0;
}
