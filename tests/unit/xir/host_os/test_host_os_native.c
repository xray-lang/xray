/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_host_os_native.c - Identical generated entries with real host effects
 */
#include "host_os_runtime.h"
#include "host_os_original_suite_runtime.h"
#include "host_os_path_cases.h"
#include "host_os_cpu_cases.h"
#include "host_os_system_cases.h"
#include "host_os_stat_cases.h"
#include "host_os_io_bool_cases.h"
#include "host_os_io_links_cases.h"
#include "host_os_io_temp_cases.h"
#include "host_os_io_directory_cases.h"
#include "host_os_signal_frontier_cases.h"
#include "host_os_thread_token_cases.h"
#include "host_os_file_kernel_cases.h"
#include "host_os_io_file_real_cases.h"
#include "base/xstdio_binary.h"
extern const XrXirProgramSpec host_count_program,host_pin_program,host_path_program,host_system_program,host_stat_program,host_io_bool_program,host_signal_frontier_program,host_io_links_program,host_io_temp_program,host_thread_tokens_program,host_io_directory_program;
extern const uint32_t host_count_ids[2],host_pin_ids[2],host_path_ids[5],host_system_ids[9],host_stat_ids[4],host_io_bool_ids[7],host_signal_frontier_ids[5],host_io_links_ids[6],host_io_temp_ids[4],host_thread_tokens_ids[HOST_TOKEN_COUNT],host_io_directory_ids[6];
static XrXirProgram *host_replica(const XrXirCompileContext *context,const XrXirProgram *source){
 (void)source;XrXirProgram *out=NULL;CHECK(xr_xir_compile_program_seal(context,&host_path_program,&out)==XR_XIR_OK);return out;
}
#if defined(XR_OS_WINDOWS)
static XrXirProgram *host_directory_replica(const XrXirCompileContext *context,const XrXirProgram *source){
 (void)source;XrXirProgram *out=NULL;CHECK(xr_xir_compile_program_seal(context,&host_io_directory_program,&out)==XR_XIR_OK);return out;
}
#endif
extern const XrXirProgramSpec host_io_file_real_program;
extern const uint32_t host_io_file_real_ids[HOST_REAL_COUNT];
int main(int argc,char **argv){
#if defined(XR_OS_WINDOWS)
 CHECK(xr_stdio_binary_init());
#endif
 int original=host_original_suite_mode(argc,argv);
 if(original!=-1)return original<0?2:host_original_native((unsigned)original,argv[3]);
 host_file_stdio_child=argc==2&&!strcmp(argv[1],"--file-stdio-child");CHECK(argc==1||host_file_stdio_child);
 if(host_file_stdio_child)CHECK(fgetc(stdin)==0x0d);else{host_cpu_concurrency();host_file_kernel_execute();}XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
 XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
 XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
 const XrXirProgramSpec *specs[]={&host_count_program,&host_pin_program,&host_path_program,&host_system_program,&host_stat_program,&host_io_bool_program,&host_signal_frontier_program,&host_io_links_program,&host_io_temp_program,&host_thread_tokens_program,&host_io_directory_program};const uint32_t *ids[]={host_count_ids,host_pin_ids,host_path_ids,host_system_ids,host_stat_ids,host_io_bool_ids,host_signal_frontier_ids,host_io_links_ids,host_io_temp_ids,host_thread_tokens_ids,host_io_directory_ids};
 if(!host_file_stdio_child)for(unsigned kind=0;kind<11;++kind){
  XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,specs[kind],&program)==XR_XIR_OK);
  if(kind==10){host_io_directory_execute(&context,program,ids[kind]);xr_xir_compile_program_drop(program);}else if(kind==9){host_thread_tokens_execute(program,ids[kind]);xr_xir_compile_program_drop(program);}else if(kind==8){host_io_temp_execute(program,ids[kind]);xr_xir_compile_program_drop(program);}else if(kind==7){host_io_links_execute(program,ids[kind]);xr_xir_compile_program_drop(program);}else if(kind==6){host_signal_frontier_execute(program,ids[kind]);xr_xir_compile_program_drop(program);}else if(kind==5){host_io_bool_execute(program,ids[kind]);xr_xir_compile_program_drop(program);}else if(kind==4){host_stat_execute(program,ids[kind]);xr_xir_compile_program_drop(program);}else if(kind==3){host_system_execute(program,ids[kind]);xr_xir_compile_program_drop(program);}else if(kind==2){host_path_execute(program,ids[kind]);host_path_receiving(&context,&program,ids[kind]);CHECK(!program);}
  else{host_cpu_execute(program,ids[kind],kind==1);xr_xir_compile_program_drop(program);}
 }
 XrXirProgram *file_program=NULL;CHECK(xr_xir_compile_program_seal(&context,&host_io_file_real_program,&file_program)==XR_XIR_OK);
 if(host_file_stdio_child)host_file_stdio_execute(file_program,host_io_file_real_ids);else host_file_real_execute(file_program,host_io_file_real_ids);xr_xir_compile_program_drop(file_program);
 CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&before.held_bytes==after.held_bytes&&!runtime_live&&!runtime_bytes);
 xr_compile_resources_release(context.resources);if(!host_file_stdio_child){host_file_stdio_parent();CHECK(xr_os_io_thread_token_shutdown()==XR_OS_IO_OK);puts("42");}return 0;
}
