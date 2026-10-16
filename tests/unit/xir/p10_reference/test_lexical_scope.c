/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lexical_scope.c - Explicit lexical scopes preserve private birth modules
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../xir_source_fixture_owner.h"

static void slot_scope_checks(SourceFixtureOwner *owner) {
    XrXirInstruction initialize[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41},
        {.op=XR_XIR_SLOT_INIT,.args={0},.immediate=0},
        {.op=XR_XIR_RETURN}};
    XrXirInstruction ret={.op=XR_XIR_RETURN};
    XrXirInstruction load[]={
        {.op=XR_XIR_SLOT_LOAD,.type=XR_XIR_I64,.immediate=0},
        {.op=XR_XIR_RETURN,.args={0}}};
    XrXirBlock three={0,3,0,0},one={0,1,0,0},two={0,2,0,0};
    XrXirFunction functions[]={
        {"$init",5,NULL,0,XR_XIR_UNIT,&three,1,initialize,3,NULL,0},
        {"$init",5,NULL,0,XR_XIR_UNIT,&one,1,&ret,1,NULL,0},
        {"answer",6,NULL,0,XR_XIR_I64,&two,1,load,2,NULL,0}};
    uint32_t dependency=0;
    XrXirSourceModule modules[]={
        {"birth",5,NULL,0,0,0},{"current",7,&dependency,1,1,1}};
    XrXirFunctionIdentity identities[3]={{0}};
    identities[1].module=identities[2].module=1;identities[2].exported=1;
    XrXirSlot slot={.module=0,.type=XR_XIR_I64,.mutable=0,.exported=0,.name=0};
    XrXirDeclarations declarations={modules,2,identities,&slot,1,NULL,0,1,2,NULL};
    XrXirModule built={XR_XIR_BUILT,functions,3,&declarations,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirConstruction *construction=NULL;XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_construction_new(&owner->context,NULL,NULL,0,&construction)==XR_XIR_OK);
    CHECK(xr_xir_compile_check_v2(&owner->context,&built,construction,&checked,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    modules[1].lexical_scope=0;
    CHECK(xr_xir_compile_check_v2(&owner->context,&built,construction,&checked,NULL)==XR_XIR_BAD_STRUCTURE && !checked);
    modules[1].lexical_scope=1;modules[1].dependency_count=0;
    CHECK(xr_xir_compile_check_v2(&owner->context,&built,construction,&checked,NULL)==XR_XIR_BAD_STRUCTURE && !checked);
    modules[1].dependency_count=1;
    load[0]=(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.immediate=0};
    CHECK(xr_xir_compile_check_v2(&owner->context,&built,construction,&checked,NULL)==XR_XIR_BAD_STRUCTURE && !checked);
    xr_xir_compile_construction_free(construction);
}
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    slot_scope_checks(&owner);
    XrXirInstruction ret={.op=XR_XIR_RETURN};
    XrXirInstruction old[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41},
        {.op=XR_XIR_RETURN,.args={0}}};
    XrXirInstruction call[]={
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=2},
        {.op=XR_XIR_RETURN,.args={0}}};
    XrXirBlock one={0,1,0,0},two={0,2,0,0};
    XrXirFunction functions[]={
        {"$init",5,NULL,0,XR_XIR_UNIT,&one,1,&ret,1,NULL,0},
        {"$init",5,NULL,0,XR_XIR_UNIT,&one,1,&ret,1,NULL,0},
        {"old",3,NULL,0,XR_XIR_I64,&two,1,old,2,NULL,0},
        {"answer",6,NULL,0,XR_XIR_I64,&two,1,call,2,NULL,0}};
    uint32_t dependency=0;
    XrXirSourceModule modules[]={
        {"birth",5,NULL,0,0,0},{"current",7,&dependency,1,1,1}};
    XrXirFunctionIdentity identities[4]={{0}};
    identities[1].module=identities[3].module=1;identities[3].exported=1;
    XrXirDeclarations declarations={modules,2,identities,NULL,0,NULL,0,1,3,NULL};
    XrXirModule built={XR_XIR_BUILT,functions,4,&declarations,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirConstruction *construction=NULL;
    CHECK(xr_xir_compile_construction_new(&owner.context,NULL,NULL,0,&construction)==XR_XIR_OK);
    XrXirArtifact *checked=NULL,*decoded=NULL;
    CHECK(xr_xir_compile_check_v2(&owner.context,&built,construction,&checked,NULL)==XR_XIR_OK);
    CHECK(identities[2].module==0 && !identities[2].exported && xr_xir_module_same_scope(&declarations,0,1));
    modules[1].lexical_scope=0;
    CHECK(xr_xir_compile_check_v2(&owner.context,&built,construction,&decoded,NULL)==XR_XIR_BAD_STRUCTURE && !decoded);
    modules[1].lexical_scope=1;modules[1].dependency_count=0;
    CHECK(xr_xir_compile_check_v2(&owner.context,&built,construction,&decoded,NULL)==XR_XIR_BAD_STRUCTURE && !decoded);
    modules[1].dependency_count=1;
    for (uint32_t scope=2;scope<=3;++scope) {
        modules[1].lexical_scope=scope;
        CHECK(!xr_xir_module_same_scope(&declarations,0,1));
        CHECK(xr_xir_compile_check_v2(&owner.context,&built,construction,&decoded,NULL)==XR_XIR_BAD_STRUCTURE && !decoded);
    }
    modules[1].lexical_scope=1;modules[0].lexical_scope=2;
    CHECK(xr_xir_compile_check_v2(&owner.context,&built,construction,&decoded,NULL)==XR_XIR_BAD_STRUCTURE && !decoded);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    const XrXirDeclarations *received=xr_xir_compile_artifact_module(decoded)->declarations;
    CHECK(received->modules[1].lexical_scope==1 && received->modules[0].lexical_scope==0 &&
        received->functions[2].module==0 && !received->functions[2].exported);
    xr_xir_compile_artifact_free(decoded);xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(checked);xr_xir_compile_construction_free(construction);
    source_fixture_owner_free(&owner);
    puts("Explicit scope, private birth identity, dependency, malformed scope and owned packet checks passed");
    return 0;
}
