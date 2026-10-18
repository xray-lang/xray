/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_regex_unicode_mixed.c - Real ordinary Unicode calls across both engines
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_regex_unicode_runtime.h"
#include "xir/xxir_vm.h"
XR_DATA const XrXirProgramSpec fixture_unicode_program;
XR_DATA const uint32_t fixture_unicode_selection[5];
typedef struct UnicodeMixedOwner {XrXirArtifact *artifact;XrXirCallEntry *entries;XrXirVmBinding *bindings;} UnicodeMixedOwner;
static unsigned unicode_released;
static void unicode_mixed_release(void *pointer) {
 UnicodeMixedOwner *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);xr_compile_resources_free(owner->entries);xr_compile_resources_free(owner->bindings);xr_compile_resources_free(owner);++unicode_released;
}
static XrXirArtifact *unicode_mixed_lower(const XrXirCompileContext *context) {
 FILE *file=fopen(XR_UNICODE_PACKET,"rb");CHECK(file && !fseek(file,0,SEEK_END));long size=ftell(file);CHECK(size>0 && !fseek(file,0,SEEK_SET));void *bytes=NULL;CHECK(xr_compile_resources_alloc(context->resources,(size_t)size,&bytes)==XR_COMPILE_RESOURCE_OK);CHECK(fread(bytes,1,(size_t)size,file)==(size_t)size && !fclose(file));
 XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_checked_read(context,bytes,(size_t)size,&checked,NULL)==XR_XIR_OK);memset(bytes,0xa5,(size_t)size);xr_compile_resources_free(bytes);CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);return lowered;
}
static void unicode_mixed(unsigned direction) {
 XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();unsigned released=unicode_released;
 XrXirArtifact *artifact=unicode_mixed_lower(&context);const XrXirModule *module=xr_xir_compile_artifact_module(artifact);CHECK(module->function_count==fixture_unicode_program.entry_count);const char *names[]={"property","has","folded","rxUnicodePropertyId","rxUnicodeHasProperty"};
 for(unsigned i=0;i<5;++i){uint32_t entry=fixture_unicode_selection[i];CHECK(entry<module->function_count && module->functions[entry].name_length==strlen(names[i]) && !memcmp(module->functions[entry].name,names[i],strlen(names[i])));}
 for(unsigned i=0;i<2;++i){const XrXirFunction *caller=&module->functions[fixture_unicode_selection[i]];bool calls=false;for(uint32_t j=0;j<caller->instruction_count;++j)calls|=caller->instructions[j].op==XR_XIR_CALL && caller->instructions[j].immediate==fixture_unicode_selection[i+3];CHECK(calls);}
 UnicodeMixedOwner *owner=NULL;CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);owner->artifact=artifact;CHECK(xr_xir_compile_vm_bind_table(artifact,&owner->bindings,&owner->entries)==XR_XIR_OK);
 for(unsigned i=0;i<2;++i){uint32_t selected=fixture_unicode_selection[direction?i+3:i],other=fixture_unicode_selection[direction?i:i+3];owner->entries[selected]=fixture_unicode_program.entries[selected];CHECK(owner->entries[other].resume!=fixture_unicode_program.entries[other].resume);}
 XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},owner->entries,module->function_count,module->declarations,{owner,unicode_mixed_release},module->types,xr_xir_compile_program_proof(artifact)};XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK);
 unicode_range_oracles(program,fixture_unicode_selection[1]);unicode_name_oracles(program,fixture_unicode_selection[0]);XrXirInstance *instance=unicode_instance(program);XrXirValue value=unicode_call(instance,fixture_unicode_selection[2],NULL,0);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
 xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);CHECK(unicode_released==released+1);unicode_folded_value(&value);xr_xir_value_drop(&value);CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);printf("Ordinary Unicode actual mixed direction%u, complete independent query oracles, one lease, folded8 after Program/ledger callers die, physical0\n",direction);
}
int main(void) {unicode_mixed(0);unicode_mixed(1);return 0;}
