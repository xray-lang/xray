/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native.c - Generated C and both mixed rest call directions
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "base/xmalloc.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_emit_c.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}} while(0)
#include "../xir_source_fixture_owner.h"
#include "execution.h"
extern const XrXirProgramSpec rest_declarations_program;
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    FILE *file=fopen(REST_PACKET,"rb");CHECK(file);
    CHECK(!fseek(file,0,SEEK_END));long length=ftell(file);CHECK(length>0 && length<1048576);
    rewind(file);char *bytes=xr_malloc((size_t)length);CHECK(bytes);
    CHECK(fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,bytes,(size_t)length,&checked,NULL)==XR_XIR_OK);
    memset(bytes,0,(size_t)length);xr_free(bytes);
    rest_program(&owner.context,&rest_declarations_program);
    rest_vm(&owner.context,checked,NULL);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(closed,&rest_declarations_program.target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(checked);
    uint32_t count=rest_declarations_program.entry_count;
    CHECK(count && count<=32);
    for (uint32_t direction=0;direction<2;++direction) {
        XrXirCallEntry entries[32];XrXirVmBinding bindings[32];uint32_t vm_count=0;
        memcpy(entries,rest_declarations_program.entries,(size_t)count*sizeof(*entries));
        for (uint32_t f=0;f<count;++f) {
            bool root=rest_declarations_program.declarations->functions[f].module==
                rest_declarations_program.declarations->root_module;
            if (root==(direction==0)) {
                CHECK(xr_xir_compile_vm_bind(lowered,f,&bindings[f],&entries[f])==XR_XIR_OK);++vm_count;
            }
        }
        CHECK(vm_count && vm_count<count);
        XrXirProgramSpec mixed=rest_declarations_program;mixed.entries=entries;
        rest_program(&owner.context,&mixed);
    }
    xr_xir_compile_artifact_free(lowered);source_fixture_owner_free(&owner);
    puts("Owned rest declarations native/VM/both mixed call directions PASS");return 0;
}
