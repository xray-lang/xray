/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_array_core_cases.h - Class identity metadata and instruction admission
 *
 * KEY CONCEPT:
 *   Canonical class body fields remain distinct from copied identity handles.
 */
#ifndef XIR_CLASS_ARRAY_CORE_CASES_H
#define XIR_CLASS_ARRAY_CORE_CASES_H
#include "xir/xxir.h"
#include "xir/xxir_types.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CLASS_CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#define CLASS_OP(o,t,a,b,k) (XrXirInstruction){o,t,{a,b},{0,0},k,{0}}
static void class_array_core_cases(void){
 XrXirNominalField fields[]={{{"value",5},(XrXirType)256,XR_XIR_FIELD_PRIVATE|XR_XIR_FIELD_MUTABLE},{{"label",5},(XrXirType)257,0}};
 XrXirNominalDeclaration decl={.module={"root",4},.name={"Counter",7},.exported=1,.fields=fields,.field_count=2,.kind=XR_XIR_NOMINAL_CLASS,.flags=XR_XIR_NOMINAL_FINAL};
 XrXirNominalTable nt={&decl,1,NULL};XrXirType ft[]={(XrXirType)256,(XrXirType)257};
 XrXirTypeNode nodes[]={
 {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
 {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_STRING},
 {.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,ft,2}}};
 XrXirTypes types={nodes,3,&nt,NULL};XrXirType ct=(XrXirType)258;
 XrXirInstruction init[]={CLASS_OP(XR_XIR_RETURN,XR_XIR_UNIT,0,0,0)};
 XrXirInstruction entry[]={CLASS_OP(XR_XIR_CONST_INT,XR_XIR_I64,0,0,41),CLASS_OP(XR_XIR_RETURN,XR_XIR_UNIT,0,0,0)};XrXirBlock entryblock={0,2,0,0};
 XrXirInstruction ctor[]={CLASS_OP(XR_XIR_CLASS_NEW,ct,0,2,0),CLASS_OP(XR_XIR_RETURN,XR_XIR_UNIT,2,0,0)};
 XrXirInstruction add[]={CLASS_OP(XR_XIR_CLASS_GET,(XrXirType)256,0,0,0),CLASS_OP(XR_XIR_CLASS_SET,XR_XIR_UNIT,0,1,0),CLASS_OP(XR_XIR_RETURN,XR_XIR_UNIT,2,0,0)};
 XrXirType cp[]={(XrXirType)256,(XrXirType)257},ap[]={ct,(XrXirType)256};uint32_t operands[]={0,1};
 XrXirBlock blocks[]={{0,1,0,0},{0,2,0,0},{0,3,0,0}};
 XrXirFunction functions[]={ {"init",4,NULL,0,XR_XIR_UNIT,&blocks[0],1,init,1,NULL,0}, {"new",3,cp,2,ct,&blocks[1],1,ctor,2,operands,2}, {"replace",7,ap,2,(XrXirType)256,&blocks[2],1,add,3,NULL,0}, {"entry",5,NULL,0,XR_XIR_I64,&entryblock,1,entry,2,NULL,0}};
 XrXirFunctionIdentity ids[]={{0},{.nominal_owner=1,.method_kind=XR_XIR_CONSTRUCTOR},{.nominal_owner=1,.method_kind=XR_XIR_READ_METHOD},{0}};
 XrXirLiteral text={"counter",7};XrXirSourceModule sm={"root",4,NULL,0,0};XrXirDeclarations ds={.modules=&sm,.module_count=1,.functions=ids,.entry_function=3,.literals=&text,.literal_count=1};
 XrXirModule m={XR_XIR_BUILT,functions,4,&ds,NULL,&types,NULL, XR_XIR_PROGRAM, NULL};XrXirDiagnostic d;XrXirStatus s=xr_xir_verify(&m,NULL,&d);
 if(s!=XR_XIR_OK)fprintf(stderr,"class array core status %u fn %u block %u instruction %u reason %u\n",s,d.function,d.block,d.instruction,d.reason);
 CLASS_CHECK(s==XR_XIR_OK);
 XrXirArtifact *checked=NULL,*read=NULL,*special=NULL,*lowered=NULL;CLASS_CHECK(xr_xir_check(&m,NULL,&checked,&d)==XR_XIR_OK);
 XrXirCheckedPacket packet={0};CLASS_CHECK(xr_xir_checked_write(checked,NULL,&packet,&d)==XR_XIR_OK);CLASS_CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&read,&d)==XR_XIR_OK);
 CLASS_CHECK(xr_xir_artifact_module(read)->types->nominals->declarations[0].flags==XR_XIR_NOMINAL_FINAL);
 CLASS_CHECK(xr_xir_specialize(read,NULL,&special,&d)==XR_XIR_OK);XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
 CLASS_CHECK(xr_xir_lower(special,&target,NULL,&lowered,&d)==XR_XIR_OK);CLASS_CHECK(xr_xir_artifact_module(lowered)->types->nominals->identities[0].flags==XR_XIR_NOMINAL_FINAL);
 uint32_t offsets[2]={0};XrXirStorageLayout layouts[3]={{0},{0},{.field_offsets=offsets,.field_count=2}};XrXirBudget budget=xr_xir_default_budget();
 CLASS_CHECK(xr_xir_storage_layouts(xr_xir_artifact_module(lowered)->types,&target,&budget,layouts,3)==XR_XIR_OK);
 CLASS_CHECK(layouts[2].value.size==8 && layouts[2].body.size==16 && offsets[0]==0 && offsets[1]==8);
 decl.flags=0;CLASS_CHECK(xr_xir_verify(&m,NULL,&d)==XR_XIR_BAD_STRUCTURE);decl.flags=XR_XIR_NOMINAL_FINAL;
 fields[0].flags=XR_XIR_FIELD_PRIVATE;CLASS_CHECK(xr_xir_verify(&m,NULL,&d)==XR_XIR_BAD_TYPE);fields[0].flags|=XR_XIR_FIELD_MUTABLE;
 ids[2].nominal_owner=0;ids[2].method_kind=XR_XIR_NON_MEMBER;CLASS_CHECK(xr_xir_verify(&m,NULL,&d)==XR_XIR_BAD_TYPE);ids[2].nominal_owner=1;ids[2].method_kind=XR_XIR_READ_METHOD;
 add[0].immediate=2;CLASS_CHECK(xr_xir_verify(&m,NULL,&d)==XR_XIR_BAD_STRUCTURE);add[0].immediate=0;
 ctor[0].op=XR_XIR_STRUCT_NEW;CLASS_CHECK(xr_xir_verify(&m,NULL,&d)==XR_XIR_BAD_TYPE);ctor[0].op=XR_XIR_CLASS_NEW;
 fields[0].type=XR_XIR_UNIT;CLASS_CHECK(xr_xir_verify(&m,NULL,&d)==XR_XIR_BAD_TYPE);fields[0].type=(XrXirType)256;
 nodes[0].element=XR_XIR_BOOL;CLASS_CHECK(xr_xir_verify(&m,NULL,&d)==XR_XIR_BAD_TYPE);nodes[0].element=XR_XIR_I64;
 ctor[0].args[1]=1;CLASS_CHECK(xr_xir_verify(&m,NULL,&d)==XR_XIR_BAD_STRUCTURE);ctor[0].args[1]=2;
 XrXirNominalIdentity *identity=(XrXirNominalIdentity *)xr_xir_artifact_module(lowered)->types->nominals->identities;
 identity[0].flags=0;CLASS_CHECK(xr_xir_verify(xr_xir_artifact_module(lowered),NULL,&d)==XR_XIR_BAD_STRUCTURE);identity[0].flags=XR_XIR_NOMINAL_FINAL;
 XrXirBudget denied=xr_xir_default_budget();denied.scratch_bytes=0;XrXirArtifact *rejected=NULL;
 CLASS_CHECK(xr_xir_check(&m,&denied,&rejected,&d)==XR_XIR_BUDGET && rejected==NULL);
 xr_xir_artifact_free(lowered);xr_xir_artifact_free(special);xr_xir_artifact_free(read);xr_xir_checked_packet_free(&packet);xr_xir_artifact_free(checked);
}

#undef CLASS_CHECK
#undef CLASS_OP
#endif // XIR_CLASS_ARRAY_CORE_CASES_H
