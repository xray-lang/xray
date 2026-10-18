/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_factory_native.c - Native factories share sealed runtime ownership
 *
 * KEY CONCEPT:
 *   Only the owning executor holds code authority for a live producer.
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_generator_factory_cases.h"
XR_DATA const XrXirProgramSpec generator_factory_program;
static XrXirAction (*factory_original_resume)(XrXirCallView *);
static bool factory_probe_done;
static XrXirAction factory_probe(XrXirCallView *view) {
    if(!factory_probe_done && view->phase==XR_XIR_CALL_NORMAL) {
        factory_probe_done=true;XrXirValue output={0};
        CHECK(xr_xir_instance_generator_at(view,UINT32_MAX,NULL,0,&output)==XR_XIR_CALL_BAD_ARGUMENT && !output.type);
        CHECK(xr_xir_instance_generator_at(view,0,NULL,0,&output)==XR_XIR_CALL_BAD_ARGUMENT && !output.type);
        XrXirValue wrong={XR_XIR_BOOL,0,1};
        CHECK(xr_xir_instance_generator_at(view,0,&wrong,1,&output)==XR_XIR_CALL_BAD_ARGUMENT && !output.type);
    }
    return factory_original_resume(view);
}

int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&generator_factory_program,&program)==XR_XIR_OK && program);
    generator_factory_cases(program);
    XrXirCallEntry entries[16];CHECK(generator_factory_program.entry_count<=16);
    memcpy(entries,generator_factory_program.entries,generator_factory_program.entry_count*sizeof(*entries));
    factory_original_resume=entries[2].resume;entries[2].resume=factory_probe;
    XrXirProgramSpec probe=generator_factory_program;probe.entries=entries;program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&probe,&program)==XR_XIR_OK && program);
    generator_factory_cases(program);CHECK(factory_probe_done);source_fixture_owner_free(&owner);
    CHECK(!runtime_live && !runtime_bytes);puts("Real generated-C factory and closed Iterator after final Program/Instance; physical0 PASS");return 0;
}
