/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_enum_packet.c - Dedicated enum identity source and ownership execution
 *
 * KEY CONCEPT:
 *   The same owned Checked program supplies independent VM and native expectations.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
#include "xir_runtime_allocations.h"
#include "xir_source_enum_identity_execution.h"
#include "xir_source_enum_identity_selection.h"
static XrXirArtifact *enum_lower(XrXirArtifact *checked) {
    XrXirArtifact *specialized=NULL,*lowered=NULL;
    CHECK(xr_xir_specialize(checked,NULL,&specialized,NULL)==XR_XIR_OK);xr_xir_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(specialized,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_artifact_free(specialized);return lowered;
}
int main(int argc,char **argv){
    CHECK(argc>=1 && argc<=3);FILE *file=fopen(argc==3?argv[2]:XR_CHECKED_FIXTURE,"rb");CHECK(file);
    CHECK(!fseek(file,0,SEEK_END));long size=ftell(file);CHECK(size>0 && size<262144);CHECK(!fseek(file,0,SEEK_SET));
    uint8_t *bytes=xr_malloc((size_t)size);CHECK(bytes);CHECK(fread(bytes,1,(size_t)size,file)==(size_t)size && !fclose(file));
    XrXirArtifact *checked=NULL;CHECK(xr_xir_checked_read(bytes,(size_t)size,NULL,&checked,NULL)==XR_XIR_OK);
    memset(bytes,0xcc,(size_t)size);xr_free(bytes);XrXirArtifact *lowered=enum_lower(checked);
    const XrXirModule *module=xr_xir_artifact_module(lowered);SourceEnumIdentityEntries entries=source_enum_identity_select(module);
    XrXirCSource emitted={0};CHECK(xr_xir_emit_c(lowered,"fixture_enum",8388608,&emitted)==XR_XIR_OK);
    printf("Enum generated C: %zu bytes / %u functions\n",emitted.length,module->function_count);
    if(argc>=2){file=fopen(argv[1],"wb");CHECK(file);CHECK(fwrite(emitted.text,1,emitted.length,file)==emitted.length);
        CHECK(fprintf(file,"\nconst uint32_t fixture_enum_values[4]={%uu,%uu,%uu,%uu};\nconst uint32_t fixture_enum_count=%uu;\n",entries.values[0],entries.values[1],entries.values[2],entries.values[3],entries.count)>0);CHECK(!fclose(file));}
    xr_xir_c_source_free(&emitted);XrXirProgram *program=NULL;
    CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
    XrXirValue retained[2][4]={{{0}}};
    source_enum_identity_pair(program,entries,retained);
    source_enum_identity_runtime_failures(program,entries);
    xr_xir_program_drop(program);
    source_enum_identity_retained_drop(retained);
    CHECK(!runtime_live && !runtime_bytes);
    puts("enum packet VM: independent 41, Box.Full, 703/401-byte strings");return 0;
}
