#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xir/xxir_vm.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"rest native %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "rest_runtime.h"
XR_DATA const XrXirProgramSpec rest_values_program;
extern const uint32_t rest_ids[5],rest_plain_type;
int main(void) {
    XrXirCompileContext c={.limits=xr_xir_compile_default_limits()};const XrCompileResourceLimits limits={0};
    CHECK(xr_compile_resources_new(&limits,&c.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(c.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirProgramSpec spec=rest_values_program;CHECK(spec.entry_count<=256);
    XrXirCallEntry entries[256];
    for(uint32_t f=0;f<spec.entry_count;++f){rest_entries[f]=spec.entries[f];entries[f]=spec.entries[f];entries[f].resume=rest_delegate;}
    spec.entries=entries;answer_entry=rest_ids[1];rest_entry=rest_ids[2];plain_entry=rest_ids[3];rest_type=(XrXirType)rest_ids[4];plain_type=(XrXirType)rest_plain_type;
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&c,&spec,&program)==XR_XIR_OK);
    rest_execute(program,rest_ids[0],answer_entry);CHECK(rest_runtime_refusals==8);
    CHECK(xr_compile_resources_stats(c.resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    xr_compile_resources_release(c.resources);puts("Rest values generated C11 native and authentic carrier refusals PASS");return 0;
}
