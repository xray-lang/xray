/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_dependency_execution.c - Dependency-ready source execution qualification
 *
 * KEY CONCEPT:
 *   Frozen Checked bytes feed independent source-free runtime expectations.
 */
#include "xir/xxir_program_internal.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_dependency_execution.h"
XR_DATA const XrXirProgramSpec dependency_ready_program;
XR_DATA const uint32_t dependency_ready_functions[3];
typedef struct DependencyCode { XrXirCallEntry *entries; XrXirVmBinding *bindings; } DependencyCode;
static unsigned dependency_code_releases;
static void dependency_code_release(void *pointer) {
 DependencyCode *code=pointer;
 xr_compile_resources_free(code->entries);xr_compile_resources_free(code->bindings);
 xr_compile_resources_free(code);++dependency_code_releases;
}
int main(void){
 const XrXirCompileContext compile=*effects_source_owner(UINT64_C(32)*1024*1024,UINT64_C(64000000));
 XrXirProgramSpec spec=dependency_ready_program;XrXirArtifact *checked=NULL,*lowered=NULL;
 XrXirCallEntry *entries=NULL;XrXirVmBinding *bindings=NULL;
 ReadyEntries selected={dependency_ready_functions[0],dependency_ready_functions[1],dependency_ready_functions[2]};
 if(XR_DEPENDENCY_MIXED){CHECK(xr_xir_compile_checked_read(&compile,spec.packet.bytes,spec.packet.length,&checked,NULL)==XR_XIR_OK);
 CHECK(xr_xir_compile_lower(checked,&spec.target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
 CHECK(xr_compile_resources_calloc(compile.resources,spec.entry_count,sizeof(*entries),(void **)&entries)==XR_COMPILE_RESOURCE_OK);
 CHECK(xr_compile_resources_calloc(compile.resources,spec.entry_count,sizeof(*bindings),(void **)&bindings)==XR_COMPILE_RESOURCE_OK);
 unsigned native=0,vm=0;const XrXirModule *module=xr_xir_compile_artifact_module(lowered);ReadyEntries actual=ready_select(module);
 CHECK(actual.answer==selected.answer&&actual.retained==selected.retained&&actual.values==selected.values);
 for(uint32_t i=0;i<spec.entry_count;++i){CHECK(xr_xir_compile_vm_bind(lowered,i,&bindings[i],&entries[i])==XR_XIR_OK);
 bool choose=i%2!=0;if(i==selected.answer||i==selected.values)choose=true;if(i==selected.retained)choose=false;
 if(choose){entries[i]=spec.entries[i];++native;}else{CHECK(entries[i].resume!=spec.entries[i].resume);++vm;}}
 CHECK(native&&vm);CHECK(entries[selected.answer].resume==spec.entries[selected.answer].resume);
 CHECK(entries[selected.retained].resume!=spec.entries[selected.retained].resume);CHECK(entries[selected.values].resume==spec.entries[selected.values].resume);
 spec.entries=entries;spec.types=module->types;spec.declarations=module->declarations;spec.packet=xir_program_packet_borrow(lowered);
 }
 XrXirProgram *program=NULL;
 if(lowered){
  DependencyCode *code=NULL;
  CHECK(xr_compile_resources_calloc(compile.resources,1,sizeof(*code),(void **)&code)==XR_COMPILE_RESOURCE_OK);
  code->entries=entries;code->bindings=bindings;
  spec.code=(XrXirCodeLease){code,dependency_code_release};
  XrXirArtifact *pending=lowered;XrXirProgramInput *input=NULL;
  XrXirStatus received=xir_program_input_take(&compile,&pending,&input);
  if(received==XR_XIR_OK){spec.packet=(XrXirProgramPacket){0};received=xir_program_receive(&compile,&spec,&input,NULL,&program);}
  if(received==XR_XIR_OK){lowered=NULL;entries=NULL;bindings=NULL;}
  else if(input){input->artifact=NULL;xir_program_input_free(input);}
  CHECK(received==XR_XIR_OK && !input && !lowered);
 }else CHECK(xr_xir_compile_program_seal(&compile,&spec,&program)==XR_XIR_OK);
 XrXirValue held[2][2]={{{0}}};ready_pair(program,selected,held);ready_runtime_faults(program,selected);
 CHECK(!dependency_code_releases);
 xr_xir_compile_program_drop(program);
 CHECK(dependency_code_releases==(XR_DEPENDENCY_MIXED?1u:0u));
 xr_xir_compile_artifact_free(lowered);xr_compile_resources_free(entries);xr_compile_resources_free(bindings);
 ready_retained(held);CHECK(!runtime_live&&!runtime_bytes);effects_source_owners_free();
 puts(XR_DEPENDENCY_MIXED?"mixed readiness41 physical release PASS":"native readiness41 physical release PASS");return 0;
}
