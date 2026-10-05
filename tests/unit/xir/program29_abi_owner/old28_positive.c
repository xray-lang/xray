#include "execution/xr_xir_host_execution.h"
#include "xir/xxir_nominal.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
extern const XrXirProgramSpec old28_program;
int main(void) {
    CHECK(old28_program.abi_version==28 && old28_program.types->nominals->count==2);
    const XrCompileResourceLimits limits={16777216,8388608,67108864};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&old28_program,&program)==XR_XIR_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirHostExecutionRequest request={program,&config,1,NULL,0};XrXirCallResult result={0};
    CHECK(xr_xir_host_execute(&request,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.value.type==XR_XIR_I64 && result.value.payload==42);
    xr_xir_call_result_drop(&result);xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    puts("Authentic old Program28/Value20/Call25 generated C with two nominal identities executes independent42 PASS");return 0;
}
