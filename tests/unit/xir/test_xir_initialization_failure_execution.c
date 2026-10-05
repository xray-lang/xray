/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_initialization_failure_execution.c - Sticky initialization failure execution qualification
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
#include "xir/xxir_generic.h"
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_initialization_failure_execution.h"
extern const XrXirProgramSpec initialization_failure_program;

int main(void){const XrXirCompileContext compile=*effects_source_owner(UINT64_C(32)*1024*1024,UINT64_C(64000000));
XrXirProgramSpec spec=initialization_failure_program;
init_module_roles(spec.declarations);
XrXirArtifact *checked=NULL,*lowered=NULL;
XrXirCallEntry *entries=NULL;
XrXirVmBinding *bindings=NULL;

 if(XR_INITIALIZATION_MIXED){CHECK(xr_xir_compile_checked_read(&compile,spec.proof.bytes,spec.proof.length,&checked,NULL)==XR_XIR_OK);
CHECK(xr_xir_compile_lower(checked,&spec.target,&lowered,NULL)==XR_XIR_OK);
xr_xir_compile_artifact_free(checked);
CHECK(xr_compile_resources_calloc(compile.resources,spec.entry_count,sizeof(*entries),(void **)&entries)==XR_COMPILE_RESOURCE_OK);
CHECK(xr_compile_resources_calloc(compile.resources,spec.entry_count,sizeof(*bindings),(void **)&bindings)==XR_COMPILE_RESOURCE_OK);
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
CHECK(entries[m->declarations->modules[init_modules.failing].initializer].resume==spec.entries[m->declarations->modules[init_modules.failing].initializer].resume);
CHECK(entries[m->declarations->modules[init_modules.base].initializer].resume!=spec.entries[m->declarations->modules[init_modules.base].initializer].resume);
CHECK(entries[m->declarations->modules[init_modules.root].initializer].resume!=spec.entries[m->declarations->modules[init_modules.root].initializer].resume);
spec.entries=entries;
spec.types=m->types;
spec.declarations=m->declarations;
spec.proof=xr_xir_compile_program_proof(lowered);
}
 XrXirProgram *program=NULL;
CHECK(xr_xir_compile_program_seal(&compile,&spec,&program)==XR_XIR_OK);
XrXirValue held[3]={{0}};
init_pair(program,spec.declarations->entry_function,held);
init_protocol_failures(program,spec.declarations->entry_function);
init_runtime_faults(program,spec.declarations->entry_function);
xr_xir_compile_program_drop(program);
xr_xir_compile_artifact_free(lowered);
xr_compile_resources_free(entries);
xr_compile_resources_free(bindings);
init_retained(held);
CHECK(!runtime_live&&!runtime_bytes);effects_source_owners_free();
puts(XR_INITIALIZATION_MIXED?"mixed initialization failure41 physical release PASS":"native initialization failure41 physical release PASS");
return 0;
}
