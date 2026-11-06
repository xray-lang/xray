/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_nested_nullable_execution.c - Actual native and mixed complete Programs
 *
 * KEY CONCEPT:
 *   Every selected entry shares the sealed Program's exact proof and declarations.
 */
#include "xir/xxir_program_packet_internal.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_nested_nullable_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_nested_nullable_fixture.h"
#include "xir_nested_nullable_cases.h"
extern const XrXirProgramSpec nested_nullable_program;
static void nested_native_program(bool direction) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirProgramSpec spec=nested_nullable_program;XrXirArtifact *checked=NULL,*lowered=NULL;
    XrXirCallEntry *entries=NULL;XrXirVmBinding *bindings=NULL;
    CHECK(nested_built.function_count==8);
    CHECK(spec.entry_count==8 && spec.declarations && spec.types && spec.packet.bytes && spec.packet.identity);
    CHECK(xr_xir_compile_checked_read(&owner.context,spec.packet.bytes,spec.packet.length,&checked,NULL)==XR_XIR_OK);
    if(XR_NESTED_NULLABLE_MIXED){
        CHECK(xr_xir_compile_lower(checked,&spec.target, NULL,&lowered,NULL)==XR_XIR_OK);
        CHECK(xr_compile_resources_calloc(owner.context.resources,8,sizeof(*entries),(void**)&entries)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_resources_calloc(owner.context.resources,8,sizeof(*bindings),(void**)&bindings)==XR_COMPILE_RESOURCE_OK);
        unsigned vm=0,native=0;
        for(uint32_t f=0;f<8;++f){
            CHECK(xr_xir_compile_vm_bind(lowered,f,&bindings[f],&entries[f])==XR_XIR_OK);
            /* Root calls Some(None), then one unwrap; swap both transitions. */
            bool select_vm=(f==1 || f==5)?!direction:direction;
            if(!select_vm){entries[f]=spec.entries[f];++native;}
            else{CHECK(entries[f].resume!=spec.entries[f].resume);++vm;}
        }
        CHECK(vm && native);
        const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
        spec.entries=entries;spec.types=module->types;spec.declarations=module->declarations;
        XrXirProgramPacket proof=xir_program_packet_borrow(lowered);
        CHECK(proof.identity && proof.length==spec.packet.length &&
            !memcmp(proof.identity,spec.packet.identity,32) && !memcmp(proof.bytes,spec.packet.bytes,proof.length));
        spec.packet=proof;
    }
    xr_xir_compile_artifact_free(checked);
    XrXirProgram *program=NULL;XrXirProgramSpec malformed=spec;
    XrXirTypeNode nodes[2]={nested_nodes[0],nested_nodes[1]};nodes[0].element=XR_XIR_BOOL;
    XrXirTypes wrong_types={nodes,2,NULL,NULL};malformed=spec;malformed.types=&wrong_types;
    CHECK(xr_xir_compile_program_seal(&owner.context,&malformed,&program)!=XR_XIR_OK && !program);
    CHECK(xr_xir_compile_program_seal(&owner.context,&spec,&program)==XR_XIR_OK);
    XrXirValue held[6]={{0}};nested_pair(program,held);
    nested_runtime_faults(program);nested_cancel_prefixes(program);
    xr_xir_compile_program_drop(program);xr_xir_compile_artifact_free(lowered);
    xr_compile_resources_free(bindings);xr_compile_resources_free(entries);
    nested_retained(held);CHECK(!runtime_live && !runtime_bytes);nested_nullable_compile_owner_free(&owner);
}
int main(void){
    nested_native_program(false);if(XR_NESTED_NULLABLE_MIXED)nested_native_program(true);
    puts(XR_NESTED_NULLABLE_MIXED?"nested Nullable two mixed directions three-state physical PASS":
        "nested Nullable actual native three-state physical PASS");return 0;
}
