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
#include "xir_runtime_allocations.h"
#include "xir_generic_fields_execution.h"
extern const XrXirProgramSpec generic_fields_program;

int main(void){XrXirProgramSpec spec=generic_fields_program;
XrXirArtifact *checked=NULL,*lowered=NULL;
XrXirCallEntry *entries=NULL;
XrXirVmBinding *bindings=NULL;

 CHECK(xr_xir_checked_read(spec.proof.bytes,spec.proof.length,NULL,&checked,NULL)==XR_XIR_OK);
GenericFieldEntries selected=generic_fields_entries(xr_xir_artifact_module(checked));
 if(XR_GENERIC_FIELDS_MIXED){
CHECK(xr_xir_lower(checked,&spec.target,NULL,&lowered,NULL)==XR_XIR_OK);
xr_xir_artifact_free(checked);checked=NULL;
entries=calloc(spec.entry_count,sizeof(*entries));
bindings=calloc(spec.entry_count,sizeof(*bindings));
CHECK(entries&&bindings);
unsigned native=0,vm=0;
for(uint32_t i=0;
i<spec.entry_count;
++i){CHECK(xr_xir_vm_bind(lowered,i,&bindings[i],&entries[i])==XR_XIR_OK);
if(i%2){entries[i]=spec.entries[i];
++native;
}else{CHECK(entries[i].resume!=spec.entries[i].resume);
++vm;
}}CHECK(native&&vm);
const XrXirModule *m=xr_xir_artifact_module(lowered);
spec.entries=entries;
spec.types=m->types;
spec.declarations=m->declarations;
spec.proof=xr_xir_program_proof(lowered);
}
 xr_xir_artifact_free(checked);
 XrXirProgram *program=NULL;
uint32_t current_abi=spec.abi_version;spec.abi_version=24;
CHECK(xr_xir_program_seal(&spec,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_BAD_LAYOUT&&!program);
spec.abi_version=current_abi;
CHECK(xr_xir_program_seal(&spec,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
XrXirValue held[2]={{0}};
generic_fields_pair(program,selected,held);

generic_fields_faults(program,selected);
xr_xir_program_drop(program);
xr_xir_artifact_free(lowered);
free(entries);
free(bindings);
generic_fields_retained(held);
CHECK(!runtime_live&&!runtime_bytes);
puts(XR_GENERIC_FIELDS_MIXED?"mixed generic fields result41 physical release PASS":"native generic fields result41 physical release PASS");
return 0;
}
