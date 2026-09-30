/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_unit_locals_execution.c - Zero-size local execution qualification
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
#include "xir_unit_locals_execution.h"
extern const XrXirProgramSpec unit_locals_program;

int main(void){XrXirProgramSpec spec=unit_locals_program;
XrXirArtifact *checked=NULL,*lowered=NULL;
XrXirCallEntry *entries=NULL;
XrXirVmBinding *bindings=NULL;

 CHECK(xr_xir_checked_read(spec.proof.bytes,spec.proof.length,NULL,&checked,NULL)==XR_XIR_OK);
UnitEntries selected=unit_entries(xr_xir_artifact_module(checked));
 if(XR_UNIT_LOCALS_MIXED){
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
CHECK(xr_xir_program_seal(&spec,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
XrXirValue held[2]={{0}};
unit_pair(program,selected,held);
unit_cancel(program,selected);
unit_faults(program,selected);
xr_xir_program_drop(program);
xr_xir_artifact_free(lowered);
free(entries);
free(bindings);
unit_retained(held);
CHECK(!runtime_live&&!runtime_bytes);
puts(XR_UNIT_LOCALS_MIXED?"mixed Unit trace1234 result41 physical release PASS":"native Unit trace1234 result41 physical release PASS");
return 0;
}
