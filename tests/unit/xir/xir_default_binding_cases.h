/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_default_binding_cases.h - Independent default authority and derivation probes
 *
 * KEY CONCEPT:
 *   Hand-built declarations distinguish purpose authority from equal signatures.
 */
#ifndef XIR_DEFAULT_BINDING_CASES_H
#define XIR_DEFAULT_BINDING_CASES_H
#include "xir_construction_fixture.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_declarations.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_value.h"
static void default_binding_case(unsigned mode,bool generic) {
 XrXirInstruction ret={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
 XrXirInstruction entry[]={
  {XR_XIR_CALL_DEFAULT,XR_XIR_I64,{0},{2,0},0,{0}},
  {XR_XIR_CALL,XR_XIR_I64,{0,1},{0},2,{0}},
  {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
 XrXirInstruction helper[]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
  {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}},{0}};
 XrXirBlock one={0,1,0,0},three={0,3,0,0},two={0,2,0,0};
 XrXirType i64=XR_XIR_I64;uint32_t operands[]={0};
 XrXirFunction functions[]={
  {"init",4,NULL,0,XR_XIR_UNIT,&one,1,&ret,1,NULL,0},
  {"entry",5,NULL,0,XR_XIR_I64,&three,1,entry,3,operands,1},
  {"owner",5,&i64,1,XR_XIR_I64,&one,1,&ret,1,NULL,0},
  {"helper",6,NULL,0,XR_XIR_I64,&two,1,helper,2,NULL,0}};
 XrXirSourceModule source={"root",4,NULL,0,0};
 XrXirFunctionIdentity ids[4]={{0},{0},{0},{0}};
 XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=ids,.entry_function=1};
 XrXirDefaultBinding bindings[2]={{XR_XIR_DEFAULT_PARAMETER,2,0,3},{XR_XIR_DEFAULT_PARAMETER,2,0,3}};
 XrXirDefaultTable table={bindings,1};
 XrXirType args[]={XR_XIR_I64,XR_XIR_STRING,XR_XIR_I64,XR_XIR_STRING};
 XrXirConstraint fc[2]={{0},{0}},hc[2]={{0},{0}};
 XrXirGeneric generics[4]={{0},{0},{0},{0}};
 if(generic){generics[1].arguments=args;generics[1].argument_count=4;
  generics[2].parameter_count=generics[3].parameter_count=2;generics[2].constraints=fc;generics[3].constraints=hc;
  entry[0].type_arguments[1]=2;entry[1].type_arguments[0]=2;entry[1].type_arguments[1]=2;}
 XrXirTypeNode callable={.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
 XrXirTypes types={&callable,1,NULL,NULL};
 XrXirModule module={XR_XIR_BUILT,functions,4,&declarations,generic?generics:NULL,NULL,NULL,XR_XIR_PROGRAM,&table};
 XrXirStatus expected=XR_XIR_BAD_STRUCTURE;
 switch(mode){
 case 0:expected=XR_XIR_OK;break;
 case 1:ids[3].exported=1;break;
 case 2:ids[3].cleanup_owner=3;break;
 case 3:ids[3].promises=XR_XIR_FUNCTION_NO_SUSPEND;break;
 case 4:bindings[0].function=0;break;
 case 5:bindings[0].function=1;break;
 case 6:bindings[0].owner_kind=1;break;
 case 7:bindings[0].ordinal=1;break;
 case 8:table.count=2;break;
 case 9:entry[0].op=XR_XIR_CALL;entry[0].immediate=3;entry[0].targets[0]=0;break;
 case 10:entry[0].targets[0]=3;break;
 case 11:entry[0].args[0]=1;break;
 case 12:entry[0].immediate=3;break;
 case 13:entry[0].type=XR_XIR_BOOL;expected=XR_XIR_BAD_TYPE;break;
 case 14:table.count=0;break;
 case 15:bindings[0].owner=3;break;
 case 16:hc[0].markers=XR_XIR_CONSTRAINT_SENDABLE;expected=XR_XIR_BAD_TYPE;break;
 case 17:generics[3].parameter_count=1;expected=XR_XIR_BAD_TYPE;break;
 case 18:functions[3].result=XR_XIR_BOOL;expected=XR_XIR_BAD_TYPE;break;
 case 22:module.types=&types;entry[0].op=XR_XIR_FUNCTION_REF;entry[0].type=(XrXirType)256;
  entry[0].immediate=3;entry[0].targets[0]=0;break;
 case 23:entry[0].op=XR_XIR_CLEANUP_REGISTER;entry[0].type=XR_XIR_UNIT;
  entry[0].immediate=3;entry[0].targets[0]=0;break;
 case 19:case 20:case 21:
  helper[0]=(XrXirInstruction){XR_XIR_SUSPEND,XR_XIR_UNIT,{0},{0},0,{0}};
  helper[1]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}};
  helper[2]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}};
  two.count=3;functions[3].instruction_count=3;expected=XR_XIR_OK;
  if(mode==20) ids[2].promises=XR_XIR_FUNCTION_NO_SUSPEND;
  if(mode==21){ids[1].promises=XR_XIR_FUNCTION_NO_SUSPEND;expected=XR_XIR_BAD_TYPE;}break;
 }
 if(mode==0){
  XrXirCompileContext b=consumer_context_ephemeral((XrCompileResourceLimits){67108864,8388608,128000000});
  XrCompileResourceStats initial=consumer_context_stats(&b);
  CHECK(xr_xir_compile_defaults_verify(&b,&module)==XR_XIR_OK);
  XrCompileResourceStats required=consumer_context_stats(&b);
  CHECK(required.live_bytes==initial.live_bytes && required.work>initial.work);
  consumer_context_ephemeral_free(&b,initial);
  for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){
   XrCompileResourceLimits limits={67108864,8388608,128000000};
   if(!axis)limits.allocated_bytes=required.allocated_bytes-minus;
   if(axis==1)limits.live_bytes=required.peak_bytes-minus;
   if(axis==2)limits.work=required.work-minus;
   XrXirCompileContext probe={0};probe.limits=xr_xir_compile_default_limits();
   XrCompileResourceStatus created=xr_compile_resources_new(&limits,&probe.resources);
   if(created==XR_COMPILE_RESOURCE_BUDGET){CHECK(minus);continue;}
   CHECK(created==XR_COMPILE_RESOURCE_OK);
   XrCompileResourceStats before=consumer_context_stats(&probe);
   CHECK(xr_xir_compile_defaults_verify(&probe,&module)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
   consumer_context_ephemeral_free(&probe,before);
  }
  if(generic){b=consumer_context_limits((XrCompileResourceLimits){67108864,initial.live_bytes,128000000});CHECK(xr_xir_compile_defaults_verify(&b,&module)==XR_XIR_BUDGET);}
  const XrXirDefaultBinding *found=(const XrXirDefaultBinding *)(uintptr_t)1;
  b=consumer_context_default();CHECK(xr_xir_compile_default_lookup(&b,&module,2,1,&found)==XR_XIR_OK && !found);
  b=consumer_context_limits((XrCompileResourceLimits){67108864,8388608,1});found=(const XrXirDefaultBinding *)(uintptr_t)1;
  CHECK(xr_xir_compile_default_lookup(&b,&module,2,0,&found)==XR_XIR_BUDGET && found==(const XrXirDefaultBinding *)(uintptr_t)1);
 }

 XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
 XrXirStatus status=xir_fixture_check(suite_context, &module, &checked, &diagnostic);
 if(status!=expected) fprintf(stderr,"mode%u generic%u status%u expected%u f%u i%u\n",mode,generic,status,expected,diagnostic.function,diagnostic.instruction);
 CHECK(status==expected);CHECK((checked!=NULL)==(expected==XR_XIR_OK));
 if(!checked)return;
 if(mode>=19){XrXirEffects *effects=NULL;CHECK(xr_xir_compile_effects_analyze(checked, &effects)==XR_XIR_OK);
 CHECK(xr_xir_effects_function(effects,1)->suspend==XR_XIR_EFFECT_MAY);
 const XrXirEffectWitness *w=xr_xir_effects_suspend_witness(effects,1);
 CHECK(w && w->callee==3 && w->instruction==0);xr_xir_compile_effects_free(effects);}

 XrXirArtifact *special=NULL,*again=NULL,*lowered=NULL,*read=NULL;
 CHECK(xr_xir_compile_specialize(checked, &special, &diagnostic)==XR_XIR_OK);
 CHECK(!special->module.defaults && special->module.provenance && special->module.provenance->source->module.defaults);
 CHECK(xr_xir_compile_specialize(special, &again, NULL)==XR_XIR_OK);
 CHECK(again->module.provenance && !again->module.provenance->source->module.provenance);
 XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
 CHECK(xr_xir_compile_lower(checked, &target, &lowered, NULL)==XR_XIR_BAD_STAGE && !lowered);
 CHECK(xr_xir_compile_lower(special, &target, &lowered, NULL)==XR_XIR_OK);
 XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(special, &packet, NULL)==XR_XIR_OK);
 CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &read, NULL)==XR_XIR_OK);
 xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(read);read=NULL;
 XrXirInstruction *mut=(XrXirInstruction *)special->module.functions[1].instructions;
 int64_t saved=mut[0].immediate;mut[0].immediate=saved==2?3:2;
 CHECK(xr_xir_compile_artifact_verify(special, NULL)!=XR_XIR_OK);mut[0].immediate=saved;
 CHECK(xr_xir_compile_artifact_verify(special, NULL)==XR_XIR_OK);
 if(generic){XrXirOrigin *origin=&special->module.provenance->origins[saved];
  XrXirType *tuple=(XrXirType *)origin->arguments;XrXirType old=tuple[0];tuple[0]=XR_XIR_STRING;
  CHECK(xr_xir_compile_artifact_verify(special, NULL)!=XR_XIR_OK);tuple[0]=old;
  CHECK(xr_xir_compile_artifact_verify(special, NULL)==XR_XIR_OK);}
 XrXirInstruction previous=mut[0];mut[0]=mut[1];mut[1]=previous;
 CHECK(xr_xir_compile_artifact_verify(special, NULL)!=XR_XIR_OK);mut[1]=mut[0];mut[0]=previous;
 CHECK(xr_xir_compile_artifact_verify(special, NULL)==XR_XIR_OK);
 xr_xir_compile_artifact_free(lowered);lowered=NULL;xr_xir_compile_artifact_free(again);again=NULL;xr_xir_compile_artifact_free(special);special=NULL;xr_xir_compile_artifact_free(checked);checked=NULL;
}
static void default_binding_cases(void) {
    for(unsigned i=0;i<24;++i) {
        if(i!=16 && i!=17) default_binding_case(i,false);
        default_binding_case(i,true);
    }
}
#endif // XIR_DEFAULT_BINDING_CASES_H
