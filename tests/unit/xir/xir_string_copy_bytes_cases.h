/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_string_copy_bytes_cases.h - Atomic value failures and genuine Source bounds
 */
#ifndef XIR_STRING_COPY_BYTES_CASES_H
#define XIR_STRING_COPY_BYTES_CASES_H
static void byte_direct_cases(void) {
 XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
 CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
 XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_U8},{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64}};
 XrXirTypes types={nodes,2,NULL,NULL};XrXirTypeArena *arena=NULL;CHECK(xr_xir_compile_type_arena_new(&context,&types,&arena)==XR_XIR_VALUE_OK);memset(nodes,0xa5,sizeof(nodes));
 XrXirDomain *origin=NULL,*receiver=NULL;CHECK(xr_xir_domain_new(65536,&origin)==XR_XIR_VALUE_OK && xr_xir_domain_new(65536,&receiver)==XR_XIR_VALUE_OK);
 const unsigned char expected[]={65,195,169,0,240,159,152,128,66};XrXirValue source={0},out={0},saved={0};
 CHECK(xr_xir_string_new(origin,(const char *)expected,sizeof(expected),&source)==XR_XIR_VALUE_OK);saved=source;
 XrXirValueAdmission admission={arena,receiver,NULL,NULL,UINT64_MAX,65536};
 XrXirValue occupied={XR_XIR_I64,0,17},wrong={XR_XIR_I64,0,9};uint64_t work=admission.work;
 CHECK(xr_xir_string_copy_bytes(&source,(XrXirType)256,&admission,&occupied)==XR_XIR_VALUE_BAD_ARGUMENT && occupied.payload==17 && admission.work==work);
 CHECK(xr_xir_string_copy_bytes(&source,(XrXirType)256,&admission,&source)==XR_XIR_VALUE_BAD_ARGUMENT && !memcmp(&source,&saved,sizeof(source)));
 CHECK(xr_xir_string_copy_bytes(&source,(XrXirType)257,&admission,&out)==XR_XIR_VALUE_BAD_ARGUMENT && !out.type);
 CHECK(xr_xir_string_copy_bytes(&wrong,(XrXirType)256,&admission,&out)==XR_XIR_VALUE_BAD_ARGUMENT && !out.type);
 CHECK(xr_xir_string_copy_bytes(&source,(XrXirType)256,NULL,&out)==XR_XIR_VALUE_BAD_ARGUMENT && !out.type);
 for(uint64_t limit=0;limit<10;++limit){admission.work=limit;CHECK(xr_xir_string_copy_bytes(&source,(XrXirType)256,&admission,&out)==XR_XIR_VALUE_LIMIT && !out.type && admission.work==limit);}
 admission.work=10;runtime_attempts=0;CHECK(xr_xir_string_copy_bytes(&source,(XrXirType)256,&admission,&out)==XR_XIR_VALUE_OK && !admission.work);size_t sites=runtime_attempts;CHECK(sites);byte_value(&out,false);xr_xir_value_drop(&out);
 size_t held=runtime_live,bytes=runtime_bytes;
 for(size_t site=0;site<sites;++site){
  runtime_attempts=0;runtime_fail_at=site;admission.work=UINT64_MAX;
  CHECK(xr_xir_string_copy_bytes(&source,(XrXirType)256,&admission,&out)==XR_XIR_VALUE_OOM && !out.type && !out.reserved && !out.payload && runtime_attempts>site);
  CHECK(runtime_live==held && runtime_bytes==bytes && !memcmp(&source,&saved,sizeof(source)));
  runtime_fail_at=SIZE_MAX;admission.work=UINT64_MAX;CHECK(xr_xir_string_copy_bytes(&source,(XrXirType)256,&admission,&out)==XR_XIR_VALUE_OK);byte_value(&out,false);xr_xir_value_drop(&out);CHECK(runtime_live==held && runtime_bytes==bytes);
 }
 XrXirDomain *small=NULL;CHECK(xr_xir_domain_new(sizeof(XrXirDomain),&small)==XR_XIR_VALUE_OK);admission.domain=small;admission.work=UINT64_MAX;
 CHECK(xr_xir_string_copy_bytes(&source,(XrXirType)256,&admission,&out)==XR_XIR_VALUE_LIMIT && !out.type && xr_xir_domain_stats(small).live_bytes==sizeof(XrXirDomain));xr_xir_domain_drop(small);
 small=NULL;CHECK(xr_xir_domain_new(65536,&small)==XR_XIR_VALUE_OK);const XrXirDomainBudgetControls controls={65536,65536,9,65536,65536};
 CHECK(xr_xir_domain_budget_bind(small,&controls)==XR_XIR_VALUE_OK);admission.domain=small;admission.work=UINT64_MAX;
 CHECK(xr_xir_string_copy_bytes(&source,(XrXirType)256,&admission,&out)==XR_XIR_VALUE_LIMIT && !out.type && !xr_xir_domain_budget_stats(small).work && admission.work==UINT64_MAX);xr_xir_domain_drop(small);
 admission.domain=receiver;admission.work=UINT64_MAX;CHECK(xr_xir_string_copy_bytes(&source,(XrXirType)256,&admission,&out)==XR_XIR_VALUE_OK);
 xr_xir_value_drop(&source);xr_xir_domain_drop(origin);xr_xir_domain_drop(receiver);xr_xir_compile_type_arena_drop(arena);xr_compile_resources_release(context.resources);
 byte_value(&out,false);xr_xir_value_drop(&out);CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
 printf("UTF-8 copy direct helper: %zu actual allocation faults, same-owner retries, exact10/minus-one work, invalid/occupied outputs and independent Domain/type lifetime\n",sites);
}
static void byte_source_refusals(void) {
 XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
 CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();XrCompileResources *same=context.resources;
 const char *invalid[]={"export fn run()->Array<u8>{return \"a\".copyBytes(1)}", "export fn run()->Array<i64>{return \"a\".copyBytes()}", "export fn run()->Array<u8>{return \"a\".bytes()}"};
 for(unsigned i=0;i<3;++i){XrXirSourceResult out={0};XrXirSourceDiagnostic diagnostic={0};CHECK(byte_source(&context,invalid[i],NULL,&out,&diagnostic)==XR_XIR_BAD_TYPE && !out.checked && !out.snapshot && diagnostic.line==1 && diagnostic.column>0);}
 const uint64_t limits[]={0,8,9};XrXirSourceResult out={0};
 for(unsigned i=0;i<3;++i){XrXirSourceComptimeLimits limit={limits[i]};XrXirSourceDiagnostic diagnostic={0};
  CHECK(byte_source(&context,"export fn run()->Array<u8>{return comptime \"Aé\\0😀B\".copyBytes()}",&limit,&out,&diagnostic)==(i==2?XR_XIR_OK:XR_XIR_BUDGET));
  CHECK(limit.output_bytes==limits[i] && context.resources==same && (i==2?(out.checked && out.snapshot):(!out.checked && !out.snapshot && diagnostic.line==1 && diagnostic.column>0)));xr_xir_compile_source_result_free(&out);
 }
 XrXirSourceComptimeLimits empty={0};CHECK(byte_source(&context,"export fn run()->Array<u8>{return comptime \"\".copyBytes()}",&empty,&out,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&out);
 xr_compile_resources_release(context.resources);CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
 puts("Authentic string declaration rejects wrong arity/type and borrowed bytes; same finite Source owner preserves request0/8/9 and empty0 limits");
}
static void byte_opcode_refusals(XrXirArtifact *artifact) {
 const XrXirModule *module=xr_xir_compile_artifact_module(artifact);const XrXirFunction *fn=&module->functions[byte_function(module,"copy")];XrXirInstruction *op=NULL;
 for(uint32_t i=0;i<fn->instruction_count;++i)if(fn->instructions[i].op==XR_XIR_STRING_COPY_BYTES){CHECK(!op);op=(XrXirInstruction *)&fn->instructions[i];}CHECK(op);
 XrXirInstruction saved=*op;
 for(unsigned attack=0;attack<5;++attack){
  if(attack==0)op->type=XR_XIR_STRING;
  else if(attack==1)op->args[0]=UINT32_MAX;
  else if(attack==2)op->args[1]=1;
  else if(attack==3)op->immediate=1;
  else op->targets[0]=1;
  CHECK(xr_xir_compile_artifact_verify(artifact,NULL)!=XR_XIR_OK);*op=saved;CHECK(xr_xir_compile_artifact_verify(artifact,NULL)==XR_XIR_OK);
 }
}
#endif // XIR_STRING_COPY_BYTES_CASES_H
