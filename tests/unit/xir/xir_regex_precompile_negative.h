/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_regex_precompile_negative.h - Exact memory authority and regex refusals
 */
#ifndef XIR_REGEX_PRECOMPILE_NEGATIVE_H
#define XIR_REGEX_PRECOMPILE_NEGATIVE_H
static XrXirLibraryInput regex_input(const XrXirCheckedPacket *packet,XrXirLibraryModuleInput binding[2]) {
 binding[0]=(XrXirLibraryModuleInput){{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_REGEX_PRECOMPILE_WORK},"regex.xr"};
 binding[1]=(XrXirLibraryModuleInput){{XR_MODULE_IDENTITY_MEMORY,"xray-core-assertions-v1",NULL},NULL};
 XrXirLibraryInput input={packet->bytes,packet->length,{0},binding,2};xr_sha256(packet->bytes,packet->length,input.sha256);return input;
}
static void regex_binding_refusals(const XrXirCheckedPacket *packet) {
 size_t blocks=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
 for(unsigned which=0;which<3;++which){
  const XrXirCompileContext *context=source_fixture_source_owner(67108864,128000000);
  XrXirLibraryModuleInput binding[2];XrXirLibraryInput input=regex_input(packet,binding);
  if(!which)binding[1].authority.namespace_id="forged-core";
  else if(which==1)binding[1].logical_path="core.xr";
  else binding[1].authority.physical_root=XR_REGEX_PRECOMPILE_WORK;
  XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog)==XR_XIR_BAD_STRUCTURE && !catalog);
  source_fixture_source_owner_close(context);CHECK(source_fixture_compile_live==blocks && source_fixture_compile_bytes==bytes);
 }
}
static void regex_source_refusal(const XrXirCheckedPacket *packet,unsigned which) {
 const XrXirCompileContext *context=source_fixture_source_owner(67108864,128000000);XrCompileResources *same=context->resources;
 XrXirLibraryModuleInput binding[2];XrXirLibraryInput input=regex_input(packet,binding);XrXirLibraryCatalog *catalog=NULL;
 CHECK(xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog)==XR_XIR_OK);
 const char *invalid[]={"*a","a**","c"};char text[512];int n=snprintf(text,sizeof(text),
  "import {compileRegex} from \"./regex\"\nexport fn program()->Array<i64>{return comptime compileRegex(\"%s\")}\n",which<3?invalid[which]:"ab*");
 CHECK(n>0 && n<(int)sizeof(text));XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};XrXirSourceComptimeLimits limits={8};
 XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(context->resources,&baseline)==XR_COMPILE_RESOURCE_OK);
 if(which<3){
  CHECK(regex_source(context,text,catalog,NULL,&output,&diagnostic)==XR_XIR_BAD_TYPE && !output.checked && !output.snapshot);
  CHECK(diagnostic.line==2 && diagnostic.column>0 && strstr(diagnostic.message,"comptime assertion failed"));
 }else{
  const uint64_t caps[]={8,47,48};
  for(unsigned i=0;i<3;++i){limits.output_bytes=caps[i];const XrXirSourceComptimeLimits saved=limits;
   XrXirStatus status=regex_source(context,text,catalog,&limits,&output,&diagnostic);
   CHECK(status==(i==2?XR_XIR_OK:XR_XIR_BUDGET) && limits.output_bytes==saved.output_bytes);
   CHECK(i==2?(output.checked && output.snapshot):(!output.checked && !output.snapshot));
   xr_xir_compile_source_result_free(&output);
  }
 }
 XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
 CHECK(context->resources==same && after.live_bytes==baseline.live_bytes && after.work>=baseline.work && after.allocated_bytes>=baseline.allocated_bytes);
 xr_xir_compile_library_catalog_free(catalog);source_fixture_source_owner_close(context);
}
static void regex_refusals(const XrXirCheckedPacket *packet) {
 size_t blocks=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
 regex_binding_refusals(packet);for(unsigned which=0;which<4;++which)regex_source_refusal(packet,which);
 CHECK(source_fixture_compile_live==blocks && source_fixture_compile_bytes==bytes);
}
#endif // XIR_REGEX_PRECOMPILE_NEGATIVE_H
