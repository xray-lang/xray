/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_library_string_execution.c - One runtime for four import consumers
 *
 * KEY CONCEPT:
 *   Source hands off its first owned Checked; other modes never link the frontend.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#if CONSUMER_KIND != 3
#include "base/xsha256.c"
#endif
#include "xir_library_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_specialize.c"
#include "xir/xxir_effects.c"
#include "xir/xxir_vm.c"
#if CONSUMER_KIND==0
#include "xir/xxir_emit_c.c"
#endif
#if CONSUMER_KIND==3
#include "xir/xxir_library_catalog.c"
#endif
#define XR_LIBRARY_STRING_RUNTIME_IMPLEMENTATION
#include "xir_library_string_runtime.h"
#if CONSUMER_KIND==1 || CONSUMER_KIND==2
extern const XrXirProgramSpec library_string_program;
extern const uint32_t library_string_export_indices[LIBRARY_STRING_EXPORTS];
#endif
#if CONSUMER_KIND==0 || CONSUMER_KIND==3
static XrXirArtifact *library_string_lower(XrXirArtifact *owned,uint32_t ids[LIBRARY_STRING_EXPORTS]) {
    CHECK(xr_xir_compile_artifact_verify(owned,NULL)==XR_XIR_OK);
    XrXirArtifact *special=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(owned,&special,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(owned);
    CHECK(xr_xir_compile_artifact_verify(special,NULL)==XR_XIR_OK);
    const XrXirModule *m=xr_xir_compile_artifact_module(special);CHECK(!m->defaults&&m->provenance&&m->provenance->source->module.defaults);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(special,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(special);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);library_string_ids(xr_xir_compile_artifact_module(lowered),ids);return lowered;
}
#endif
#if CONSUMER_KIND!=1
static XrXirProgramSpec library_string_vm_spec(XrXirArtifact *lowered,XrXirCallEntry *entries,XrXirVmBinding *bindings) {
    const XrXirModule *m=xr_xir_compile_artifact_module(lowered);CHECK(m->function_count<=128);
    XrXirProgramSpec spec={0};spec.abi_version=XR_XIR_PROGRAM_ABI_VERSION;
    spec.target=(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};spec.entry_count=m->function_count;
    spec.types=m->types;spec.declarations=m->declarations;spec.proof=xr_xir_compile_program_proof(lowered);
    for(uint32_t f=0;f<m->function_count;++f)CHECK(xr_xir_compile_vm_bind(lowered,f,&bindings[f],&entries[f])==XR_XIR_OK);
    spec.entries=entries;return spec;
}
#endif
#if CONSUMER_KIND==3
XR_FUNC size_t *xr_test_library_string_runtime_counter(unsigned index) {
    CHECK(index<4);size_t *values[]={&runtime_attempts,&runtime_fail_at,&runtime_live,&runtime_bytes};return values[index];
}
XR_FUNC void xr_test_library_string_run(const XrXirCompileContext *context,XrXirArtifact *owned) {
    uint32_t ids[LIBRARY_STRING_EXPORTS];XrXirArtifact *lowered=library_string_lower(owned,ids);
    XrXirCallEntry entries[128];XrXirVmBinding bindings[128];XrXirProgramSpec spec=library_string_vm_spec(lowered,entries,bindings);
    library_string_finish(context,&spec,lowered,ids);puts("Library strings direct first Source Checked after producer destruction PASS");
}
#else
int main(int argc,char **argv) {
    LibraryCompileOwner compiler={0};CHECK(library_compile_owner_new(&compiler,&library_compile_limits)==XR_XIR_OK);
    XrXirArtifact *lowered=NULL;
#if CONSUMER_KIND!=1
    XrXirArtifact *checked=NULL;
#endif
    uint32_t ids[LIBRARY_STRING_EXPORTS];XrXirProgramSpec spec={0};
#if CONSUMER_KIND==0
    CHECK(argc==3);FILE *f=fopen(argv[1],"rb");CHECK(f&&!fseek(f,0,SEEK_END));long n=ftell(f);
    CHECK(n>=64&&n<=262144&&!fseek(f,0,SEEK_SET));void *bytes=malloc((size_t)n);
    CHECK(bytes&&fread(bytes,1,(size_t)n,f)==(size_t)n&&!fclose(f));
    CHECK(xr_xir_compile_checked_read(&compiler.context,bytes,(size_t)n,&checked,NULL)==XR_XIR_OK);free(bytes);
    lowered=library_string_lower(checked,ids);checked=NULL;
    XrXirCSource output={0};CHECK(xr_xir_compile_emit_c(lowered,"library_string",1048576,&output)==XR_XIR_OK);
    f=fopen(argv[2],"wb");CHECK(f&&fwrite(output.text,1,output.length,f)==output.length);
    CHECK(fprintf(f,"\nconst uint32_t library_string_export_indices[10]={%u,%u,%u,%u,%u,%u,%u,%u,%u,%u};\n",ids[0],ids[1],ids[2],ids[3],ids[4],ids[5],ids[6],ids[7],ids[8],ids[9])>0&&!fclose(f));
    xr_xir_compile_c_source_free(&output);
#else
    CHECK(argc==1);(void)argv;spec=library_string_program;memcpy(ids,library_string_export_indices,sizeof(ids));
#if CONSUMER_KIND==2
    CHECK(xr_xir_compile_checked_read(&compiler.context,spec.proof.bytes,spec.proof.length,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked,&spec.target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);checked=NULL;
#endif
#endif
#if CONSUMER_KIND!=1
    XrXirCallEntry entries[128];XrXirVmBinding bindings[128];spec=library_string_vm_spec(lowered,entries,bindings);
#if CONSUMER_KIND==2
    CHECK(spec.entry_count==library_string_program.entry_count);
    for(uint32_t f=0;f<spec.entry_count;++f)if(f%2)entries[f]=library_string_program.entries[f];
#endif
#endif
    library_string_finish(&compiler.context,&spec,lowered,ids);library_compile_owner_drop(&compiler);library_compile_observer_free();printf("Library strings consumer=%u PASS\n",CONSUMER_KIND);return 0;
}
#endif
