/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_unit_slots_execution.c - Zero-size local execution qualification
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
#include "xir_unit_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_unit_slots_execution.h"
extern const XrXirProgramSpec unit_slots_program;

int main(void){UnitCompileOwner owner;unit_compile_owner_new(&owner);const XrXirCompileContext *context=&owner.context;XrXirProgramSpec spec=unit_slots_program;
XrXirArtifact *checked=NULL,*lowered=NULL;
XrXirCallEntry *entries=NULL;
XrXirVmBinding *bindings=NULL;

 CHECK(xr_xir_compile_checked_read(context,spec.proof.bytes,spec.proof.length,&checked,NULL)==XR_XIR_OK);
UnitEntries selected=unit_entries(xr_xir_compile_artifact_module(checked));
 if(XR_UNIT_SLOTS_MIXED){
CHECK(xr_xir_compile_lower(checked,&spec.target,&lowered,NULL)==XR_XIR_OK);
xr_xir_compile_artifact_free(checked);checked=NULL;
CHECK(xr_compile_resources_calloc(context->resources,spec.entry_count,sizeof(*entries),(void **)&entries)==XR_COMPILE_RESOURCE_OK);
CHECK(xr_compile_resources_calloc(context->resources,spec.entry_count,sizeof(*bindings),(void **)&bindings)==XR_COMPILE_RESOURCE_OK);
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
spec.proof=xr_xir_compile_program_proof(lowered);
}
 xr_xir_compile_artifact_free(checked);
 XrXirProgram *program=NULL;
uint32_t current_abi=spec.abi_version;spec.abi_version=24;
CHECK(xr_xir_compile_program_seal(context,&spec,&program)==XR_XIR_BAD_LAYOUT&&!program);
spec.abi_version=current_abi;
CHECK(xr_xir_compile_program_seal(context,&spec,&program)==XR_XIR_OK);
XrXirValue held[2]={{0}};
unit_pair(program,selected,held);
unit_cancel(program,selected);
unit_faults(program,selected);
xr_xir_compile_program_drop(program);
xr_xir_compile_artifact_free(lowered);
xr_compile_resources_free(entries);
xr_compile_resources_free(bindings);
unit_retained(held);
CHECK(!runtime_live&&!runtime_bytes);
puts(XR_UNIT_SLOTS_MIXED?"mixed Unit trace1234 result41 physical release PASS":"native Unit trace1234 result41 physical release PASS");
unit_compile_owner_report(&owner,XR_UNIT_SLOTS_MIXED?"unit_slots mixed":"unit_slots native");unit_compile_owner_free(&owner);return 0;
}
