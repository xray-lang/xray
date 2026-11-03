/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_host_os_native.c - Identical generated entries with real host effects
 */
#include "host_os_runtime.h"
#include "host_os_path_cases.h"
#include "host_os_cpu_cases.h"
#include "host_os_system_cases.h"
#include "host_os_stat_cases.h"
#include "host_os_io_bool_cases.h"
extern const XrXirProgramSpec host_count_program,host_pin_program,host_path_program,host_system_program,host_stat_program,host_io_bool_program;
extern const uint32_t host_count_ids[2],host_pin_ids[2],host_path_ids[5],host_system_ids[9],host_stat_ids[4],host_io_bool_ids[7];
static XrXirProgram *host_replica(const XrXirCompileContext *context,const XrXirProgram *source){
 (void)source;XrXirProgram *out=NULL;CHECK(xr_xir_compile_program_seal(context,&host_path_program,&out)==XR_XIR_OK);return out;
}
int main(int argc,char **argv){
#if defined(XR_OS_WINDOWS)
 CHECK(_setmode(_fileno(stdout),_O_BINARY)!=-1);
#endif
 CHECK(argc==1);(void)argv;host_cpu_concurrency();XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
 XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
 XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
 const XrXirProgramSpec *specs[]={&host_count_program,&host_pin_program,&host_path_program,&host_system_program,&host_stat_program,&host_io_bool_program};const uint32_t *ids[]={host_count_ids,host_pin_ids,host_path_ids,host_system_ids,host_stat_ids,host_io_bool_ids};
 for(unsigned kind=0;kind<6;++kind){
  XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,specs[kind],&program)==XR_XIR_OK);
  if(kind==5){host_io_bool_execute(program,ids[kind]);xr_xir_compile_program_drop(program);}else if(kind==4){host_stat_execute(program,ids[kind]);xr_xir_compile_program_drop(program);}else if(kind==3){host_system_execute(program,ids[kind]);xr_xir_compile_program_drop(program);}else if(kind==2){host_path_execute(program,ids[kind]);host_path_receiving(&context,&program,ids[kind]);CHECK(!program);}
  else{host_cpu_execute(program,ids[kind],kind==1);xr_xir_compile_program_drop(program);}
 }
 CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&before.held_bytes==after.held_bytes&&!runtime_live&&!runtime_bytes);
 xr_compile_resources_release(context.resources);puts("42");return 0;
}
