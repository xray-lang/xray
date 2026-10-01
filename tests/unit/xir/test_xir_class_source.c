/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_class_source.c - Real class identity execution and lifetime gates
 *
 * KEY CONCEPT:
 *   One Checked source graph feeds independent VM and native expected results.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_class.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define C(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
int main(int argc,char **argv){C(argc==1 || argc==2);XrCompilerSession *session=xr_compiler_session_new(NULL);C(session);
 XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,NULL,NULL,NULL, XR_XIR_PROGRAM, NULL};XrXirSourceResult result={0};XrXirSourceDiagnostic d={0};XrXirStatus status=xr_xir_source_check(&request,&result,&d);fprintf(stderr,"source=%u %d:%d %s\n",status,d.line,d.column,d.message);C(status==XR_XIR_OK);
 XrXirCheckedPacket packet={0};C(xr_xir_checked_write(result.checked,NULL,&packet,NULL)==XR_XIR_OK);C(packet.length<=262144);
 if(argc==2){FILE *packet_file=fopen(argv[1],"wb");C(packet_file);C(fwrite(packet.bytes,1,packet.length,packet_file)==packet.length);C(!fclose(packet_file));}
 XrXirArtifact *decoded=NULL;C(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL)==XR_XIR_OK);xr_xir_checked_packet_free(&packet);xr_xir_source_result_free(&result);result.checked=decoded;
 XrXirArtifact *special=NULL,*lowered=NULL;XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};C(xr_xir_specialize(result.checked,NULL,&special,NULL)==XR_XIR_OK);xr_xir_source_result_free(&result);xr_compiler_session_delete(session);C(xr_xir_lower(special,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_artifact_free(special);
 const XrXirModule *module=xr_xir_artifact_module(lowered);uint32_t answer=UINT32_MAX,retained=UINT32_MAX;for(uint32_t f=0;f<module->function_count;++f){if(module->functions[f].name_length==6&&!memcmp(module->functions[f].name,"answer",6))answer=f;if(module->functions[f].name_length==8&&!memcmp(module->functions[f].name,"retained",8))retained=f;}C(answer==3&&retained==4);
 XrXirProgram *program=NULL;C(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);XrXirValue saved[2]={{0},{0}};
 for(unsigned i=0;i<2;++i){XrXirInstance *instance=NULL;XrXirInstanceConfig config=xr_xir_instance_defaults();C(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);C(xr_xir_instance_start(instance,answer,NULL,0)==XR_XIR_CALL_READY);C(xr_xir_instance_poll(instance).outcome.status==XR_XIR_CALL_RETURNED);XrXirValue number={0};C(xr_xir_instance_take_result(instance,&number)==XR_XIR_CALL_RETURNED);C(number.type==XR_XIR_I64&&number.payload==41);xr_xir_value_drop(&number);C(xr_xir_instance_start(instance,retained,NULL,0)==XR_XIR_CALL_READY);C(xr_xir_instance_poll(instance).outcome.status==XR_XIR_CALL_RETURNED);C(xr_xir_instance_take_result(instance,&saved[i])==XR_XIR_CALL_RETURNED);C(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);}
 xr_xir_program_drop(program);for(unsigned i=0;i<2;++i){XrXirValue value={0};C(xr_xir_class_get(&saved[i],0,&(XrXirValueAdmission){.arena=xr_xir_value_arena(&saved[i]),.work=10000},&value)==XR_XIR_VALUE_OK);C(value.type==XR_XIR_I64&&value.payload==40);xr_xir_value_drop(&value);C(xr_xir_class_get(&saved[i],1,&(XrXirValueAdmission){.arena=xr_xir_value_arena(&saved[i]),.work=10000},&value)==XR_XIR_VALUE_OK);const char *bytes=NULL;size_t length=0;C(xr_xir_string_view(&value,&bytes,&length)&&length==7&&!memcmp(bytes,"counter",7));xr_xir_value_drop(&saved[i]);C(xr_xir_string_view(&value,&bytes,&length)&&length==7&&!memcmp(bytes,"counter",7));xr_xir_value_drop(&value);}puts("source class VM41 retained PASS");return 0;}
