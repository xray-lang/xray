/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generic_fields_execution.c - Generic class field execution qualification
 *
 * KEY CONCEPT:
 *   Frozen Checked bytes feed independent source-free runtime expectations.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_generic_fields_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_generic_fields_execution.h"
extern const XrXirProgramSpec generic_fields_program;

int main(void){SourceFixtureOwner compiler={0}; source_fixture_owner_new(&compiler);
XrXirProgramSpec spec=generic_fields_program;
XrXirArtifact *checked=NULL,*lowered=NULL;
XrXirCallEntry *entries=NULL;
XrXirVmBinding *bindings=NULL;

 CHECK(xr_xir_compile_checked_read(&compiler.context,spec.proof.bytes,spec.proof.length,&checked,NULL)==XR_XIR_OK);
GenericFieldEntries selected=generic_fields_entries(xr_xir_compile_artifact_module(checked));
 if(XR_GENERIC_FIELDS_MIXED){
CHECK(xr_xir_compile_lower(checked,&spec.target,&lowered,NULL)==XR_XIR_OK);
xr_xir_compile_artifact_free(checked);checked=NULL;
CHECK(xr_compile_resources_calloc(compiler.context.resources,spec.entry_count,sizeof(*entries),(void **)&entries)==XR_COMPILE_RESOURCE_OK);
CHECK(xr_compile_resources_calloc(compiler.context.resources,spec.entry_count,sizeof(*bindings),(void **)&bindings)==XR_COMPILE_RESOURCE_OK);
CHECK(entries&&bindings);
unsigned native=0,vm=0;
for(uint32_t i=0;
i<spec.entry_count;
++i){CHECK(xr_xir_compile_vm_bind(lowered,i,&bindings[i],&entries[i])==XR_XIR_OK);
if(i%2){entries[i]=spec.entries[i];
++native;
}else{CHECK(entries[i].resume!=spec.entries[i].resume);
++vm;
}}CHECK(native&&vm);
const XrXirModule *m=xr_xir_compile_artifact_module(lowered);
spec.entries=entries;
spec.types=m->types;
spec.declarations=m->declarations;
XrXirProgramProof proof=xr_xir_compile_program_proof(lowered);
CHECK(proof.identity&&spec.proof.identity&&proof.length==spec.proof.length&&
    !memcmp(proof.identity,spec.proof.identity,32)&&!memcmp(proof.bytes,spec.proof.bytes,proof.length));
spec.proof=proof;
}
 xr_xir_compile_artifact_free(checked);
 XrXirProgram *program=NULL;
uint32_t current_abi=spec.abi_version;spec.abi_version=24;
CHECK(xr_xir_compile_program_seal(&compiler.context,&spec,&program)==XR_XIR_BAD_LAYOUT&&!program);
spec.abi_version=current_abi;
CHECK(xr_xir_compile_program_seal(&compiler.context,&spec,&program)==XR_XIR_OK);
XrXirValue held[2]={{0}};
generic_fields_pair(program,selected,held);

generic_fields_faults(program,selected);
xr_xir_compile_program_drop(program);
xr_xir_compile_artifact_free(lowered);
xr_compile_resources_free(entries);
xr_compile_resources_free(bindings);
generic_fields_retained(held);
CHECK(!runtime_live&&!runtime_bytes);
generic_fields_compile_owner_free(&compiler);
puts(XR_GENERIC_FIELDS_MIXED?"mixed generic fields result41 physical release PASS":"native generic fields result41 physical release PASS");
return 0;
}
