/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native.c - Generated C and both mixed complete math call directions
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
extern const XrXirProgramSpec math_library_program;
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    FILE *file=fopen(MATH_PACKET,"rb");CHECK(file);
    CHECK(!fseek(file,0,SEEK_END));long length=ftell(file);CHECK(length>0 && length<1048576);
    rewind(file);char *bytes=xr_malloc((size_t)length);CHECK(bytes);
    CHECK(fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,bytes,(size_t)length,&checked,NULL)==XR_XIR_OK);
    memset(bytes,0,(size_t)length);xr_free(bytes);
    math_program(&owner.context,&math_library_program);
    math_vm(&owner.context,checked,NULL);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(closed,&math_library_program.target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(checked);
    uint32_t count=math_library_program.entry_count;
    CHECK(count && count<=96);
    XrXirVmBinding *vm_bindings=NULL;XrXirCallEntry *vm_entries=NULL;
    CHECK(xr_xir_compile_vm_bind_table(lowered,&vm_bindings,&vm_entries)==XR_XIR_OK);
    for (uint32_t direction=0;direction<2;++direction) {
        XrXirCallEntry entries[96];uint32_t vm_count=0;
        memcpy(entries,math_library_program.entries,(size_t)count*sizeof(*entries));
        for (uint32_t f=0;f<count;++f) {
            bool root=math_library_program.declarations->functions[f].module==
                math_library_program.declarations->root_module;
            if (root==(direction==0)) {
                entries[f]=vm_entries[f];++vm_count;
            }
        }
        CHECK(vm_count && vm_count<count);
        XrXirProgramSpec mixed=math_library_program;mixed.entries=entries;
        math_program(&owner.context,&mixed);
    }
    xr_compile_resources_free(vm_entries);xr_compile_resources_free(vm_bindings);
    xr_xir_compile_artifact_free(lowered);source_fixture_owner_free(&owner);
    puts("Complete owned math family native/VM/both mixed call directions PASS");return 0;
}
