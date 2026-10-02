/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_compile_old_data.c - Reject independently compiled old producer data
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
extern const XrXirProgramSpec *old_program_data(void);
extern const XrXirCallConfig *old_call_config_data(void);
extern const XrXirCallEntry *old_call_entry_data(void);
extern unsigned old_callback_count(void);
extern unsigned old_release_count(void);
int main(void) {
    const XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrXirCompileContext context={0}; context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;
    CHECK(old_program_data()->abi_version==26 && old_program_data()->target.abi_version==17);
    CHECK(old_call_config_data()->abi_version==21 && old_call_entry_data()->abi_version==21);
    CHECK(xr_xir_compile_program_seal(&context,old_program_data(),&program)==XR_XIR_BAD_LAYOUT && !program);
    XrXirCall *call=NULL;
    CHECK(xr_xir_call_new(old_call_config_data(),0,NULL,0,&call)==XR_XIR_CALL_BAD_ABI && !call);
    XrXirCallConfig config;
    CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirCallAccounting accounting={0}; config.accounting=&accounting;
    config.entries=old_call_entry_data(); config.entry_count=1;
    CHECK(xr_xir_call_new(&config,0,NULL,0,&call)==XR_XIR_CALL_BAD_ABI && !call);
    CHECK(!old_callback_count() && !old_release_count());
    xr_compile_resources_release(context.resources);
    puts("Old Program 26 / Call 21 / Value 17 producer data rejected before poison pointers and callbacks");
    return 0;
}
