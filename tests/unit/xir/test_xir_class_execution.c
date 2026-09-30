/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_class_execution.c - Real class identity execution and lifetime gates
 *
 * KEY CONCEPT:
 *   One Checked source graph feeds independent VM and native expected results.
 */
#include "xir/xxir_class.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define C(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#define CHECK(x) C(x)
#include "xir_runtime_allocations.h"
extern const XrXirProgramSpec source_class_program;
static void class_runtime_faults(XrXirProgram *program) {
 size_t base=runtime_live,bytes=runtime_bytes,sites=0;
 for(size_t pass=0;pass<=sites;++pass){runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
 XrXirInstance *instance=NULL;XrXirInstanceConfig config=xr_xir_instance_defaults();XrXirValue value={0};
 XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
 if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,3,NULL,0);
 if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll(instance).outcome.status;
 if(status==XR_XIR_CALL_RETURNED){C(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);C(value.type==XR_XIR_I64&&value.payload==41);xr_xir_value_drop(&value);
 status=xr_xir_instance_start(instance,4,NULL,0);if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll(instance).outcome.status;
 if(status==XR_XIR_CALL_RETURNED)C(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);}
 if(!pass){C(status==XR_XIR_CALL_RETURNED);sites=runtime_attempts;C(sites>0);}else C(status==XR_XIR_CALL_OOM);
 if(instance)C(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_value_drop(&value);
 C(runtime_live==base && runtime_bytes==bytes);
 }
 runtime_fail_at=SIZE_MAX;printf("class runtime OOM sites %zu physical baseline restored\n",sites);
}
int main(void){
 const int argc=XR_CLASS_MIXED?2:1;XrXirProgramSpec spec=source_class_program;XrXirProgram *program=NULL;
 XrXirProgramSpec old=spec;old.abi_version=23;old.declarations=(const XrXirDeclarations *)(uintptr_t)1;
 C(xr_xir_program_seal(&old,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_BAD_LAYOUT && !program);
 old=spec;old.target.abi_version=13;old.declarations=(const XrXirDeclarations *)(uintptr_t)1;
 C(xr_xir_program_seal(&old,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_BAD_LAYOUT && !program);
 uint8_t *packet=malloc(spec.proof.length);C(packet);memcpy(packet,spec.proof.bytes,spec.proof.length);
 for(unsigned mode=0;mode<2;++mode){memcpy(packet,spec.proof.bytes,spec.proof.length);packet[mode?12:8]=mode?46:18;
 XrSHA256Context sha;xr_sha256_init(&sha);xr_sha256_update(&sha,packet,32);xr_sha256_update(&sha,packet+64,spec.proof.length-64);xr_sha256_final(&sha,packet+32);
 XrXirArtifact *rejected=NULL;C(xr_xir_checked_read(packet,spec.proof.length,NULL,&rejected,NULL)==XR_XIR_BAD_STRUCTURE && !rejected);}free(packet);
 XrXirArtifact *checked=NULL,*lowered=NULL;XrXirCallEntry entries[32];XrXirVmBinding bindings[32];
 if(argc>1){C(spec.entry_count<=32);C(xr_xir_checked_read(spec.proof.bytes,spec.proof.length,NULL,&checked,NULL)==XR_XIR_OK);
 C(xr_xir_lower(checked,&spec.target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_artifact_free(checked);
 const XrXirModule *m=xr_xir_artifact_module(lowered);unsigned native=0,vm=0;
 for(uint32_t f=0;f<spec.entry_count;++f){C(xr_xir_vm_bind(lowered,f,&bindings[f],&entries[f])==XR_XIR_OK);
 if((f%2)==1){entries[f]=spec.entries[f];++native;}else{C(entries[f].resume!=spec.entries[f].resume);++vm;}}
 C(native && vm);spec.entries=entries;spec.declarations=m->declarations;spec.types=m->types;spec.proof=xr_xir_program_proof(lowered);
 }
 C(xr_xir_program_seal(&spec,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
 class_runtime_faults(program);
 XrXirValue saved[2]={{0},{0}};
 for(unsigned i=0;i<2;++i){XrXirInstance *instance=NULL;XrXirInstanceConfig config=xr_xir_instance_defaults();
 C(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
 C(xr_xir_instance_start(instance,3,NULL,0)==XR_XIR_CALL_READY);C(xr_xir_instance_poll(instance).outcome.status==XR_XIR_CALL_RETURNED);
 XrXirValue number={0};C(xr_xir_instance_take_result(instance,&number)==XR_XIR_CALL_RETURNED);C(number.type==XR_XIR_I64 && number.payload==41);xr_xir_value_drop(&number);
 C(xr_xir_instance_start(instance,4,NULL,0)==XR_XIR_CALL_READY);C(xr_xir_instance_poll(instance).outcome.status==XR_XIR_CALL_RETURNED);
 C(xr_xir_instance_take_result(instance,&saved[i])==XR_XIR_CALL_RETURNED);C(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);}
 xr_xir_program_drop(program);xr_xir_artifact_free(lowered);
 for(unsigned i=0;i<2;++i){XrXirValue value={0};C(xr_xir_class_get(&saved[i],0,&value)==XR_XIR_VALUE_OK && value.type==XR_XIR_I64 && value.payload==40);xr_xir_value_drop(&value);
 C(xr_xir_class_get(&saved[i],1,&value)==XR_XIR_VALUE_OK);const char *text=NULL;size_t n=0;C(xr_xir_string_view(&value,&text,&n)&&n==7&&!memcmp(text,"counter",7));xr_xir_value_drop(&saved[i]);C(xr_xir_string_view(&value,&text,&n)&&n==7);xr_xir_value_drop(&value);}
 C(!runtime_live && !runtime_bytes);
 puts(argc>1?"class source mixed41 retained40/string oldABI PASS":"class source native41 retained40/string oldABI PASS");return 0;
}
