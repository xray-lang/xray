/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_deep_cases.h - Nested value storage preserves shared class identity
 *
 * KEY CONCEPT:
 *   Array COW separates array owners while class leaves retain one mutable identity.
 */
#ifndef XIR_CLASS_DEEP_CASES_H
#define XIR_CLASS_DEEP_CASES_H
static XrXirTypeArena *deep_arena(XrXirDomain *domain,bool writable) {
 XrXirNominalFieldIdentity cf[]={{{"count",5},writable?XR_XIR_FIELD_MUTABLE:0},{{"text",4},XR_XIR_FIELD_MUTABLE}};
 XrXirNominalFieldIdentity sf[]={{{"object",6},0},{{"label",5},0}};
 XrXirNominalIdentity ids[]={{{"deep",4},{"Counter",7},1,0,cf,2,XR_XIR_NOMINAL_CLASS,NULL,0,XR_XIR_NOMINAL_FINAL},
 {{"deep",4},{"Holder",6},1,0,sf,2,XR_XIR_NOMINAL_STRUCT,NULL,0,0}};
 XrXirNominalTable table={NULL,2,ids};XrXirType cfields[]={XR_XIR_I64,XR_XIR_STRING},sfields[]={(XrXirType)256,XR_XIR_STRING};
 XrXirTypeNode nodes[]={
 {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{0,NULL,0,cfields,2}},
 {XR_XIR_TYPE_ARRAY,(XrXirType)256,NULL,0,XR_XIR_UNIT,0,0,{0}},
 {XR_XIR_TYPE_ARRAY,(XrXirType)257,NULL,0,XR_XIR_UNIT,0,0,{0}},
 {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{1,NULL,0,sfields,2}},
 {XR_XIR_TYPE_ARRAY,(XrXirType)259,NULL,0,XR_XIR_UNIT,0,0,{0}}};
 XrXirTypes types={nodes,5,&table,NULL};XrXirBudget budget=xr_xir_default_budget();XrXirTypeArena *arena=NULL;
 CHECK(xr_xir_type_arena_new(domain,&types,&budget,&arena)==XR_XIR_VALUE_OK);
 memset(ids,0xcc,sizeof(ids));memset(nodes,0xcc,sizeof(nodes));return arena;
}
static XrXirValueStatus deep_sequence(XrXirTypeArena *arena,XrXirDomain *domain,XrXirValue text) {
 XrXirValue v[11]={{0}};XrXirValueStatus status=XR_XIR_VALUE_OK;XrXirFaultDetail fault={0};
 XrXirValueAdmission a={arena,domain,NULL,NULL,1000000,65536};XrXirValue fields[]={{XR_XIR_I64,0,40},text};
#define STEP(expr) do{status=(expr);if(status!=XR_XIR_VALUE_OK)goto done;}while(0)
 STEP(xr_xir_class_new((XrXirType)256,fields,2,&a,&v[0]));
 STEP(xr_xir_array_new((XrXirType)257,&v[0],1,&a,&v[1]));
 STEP(xr_xir_array_new((XrXirType)258,&v[1],1,&a,&v[2]));
 STEP(xr_xir_value_copy(&v[2],&v[3]));
 XrXirValuePlace outer={(XrXirType)258,&v[2].payload};
 STEP(xr_xir_array_push(&outer,&v[1],&a));CHECK(v[2].payload!=v[3].payload);
 int64_t n=0;STEP(xr_xir_array_len(&v[3],&a,&n));CHECK(n==1);
 STEP(xr_xir_array_get(&v[3],0,&a,&v[4],&fault));CHECK(v[4].payload==v[1].payload);
 XrXirValuePlace inner={(XrXirType)257,&v[4].payload};
 STEP(xr_xir_array_push(&inner,&v[0],&a));CHECK(v[4].payload!=v[1].payload);
 STEP(xr_xir_array_len(&v[1],&a,&n));CHECK(n==1);
 STEP(xr_xir_array_get(&v[4],1,&a,&v[5],&fault));CHECK(v[5].payload==v[0].payload);
 XrXirValue forty_one={XR_XIR_I64,0,41};size_t setter_calls=runtime_attempts;
 STEP(xr_xir_class_set(&v[5],0,&forty_one,&a));CHECK(runtime_attempts==setter_calls);
 XrXirValue result={0};STEP(xr_xir_class_get(&v[0],0,&result));CHECK(result.type==XR_XIR_I64&&result.payload==41);xr_xir_value_drop(&result);
 XrXirValue holder_fields[]={v[0],text};STEP(xr_xir_struct_new((XrXirType)259,holder_fields,2,&a,&v[6]));
 STEP(xr_xir_array_new((XrXirType)260,&v[6],1,&a,&v[7]));STEP(xr_xir_value_copy(&v[7],&v[8]));
 XrXirValuePlace holders={(XrXirType)260,&v[7].payload};STEP(xr_xir_array_push(&holders,&v[6],&a));CHECK(v[7].payload!=v[8].payload);
 STEP(xr_xir_array_get(&v[8],0,&a,&v[9],&fault));STEP(xr_xir_struct_get(&v[9],0,&a,&v[10]));CHECK(v[10].payload==v[0].payload);
 STEP(xr_xir_class_get(&v[10],0,&result));CHECK(result.type==XR_XIR_I64&&result.payload==41);xr_xir_value_drop(&result);
 done:;
 size_t before=runtime_attempts;for(uint32_t i=11;i;--i)xr_xir_value_drop(&v[i-1]);CHECK(runtime_attempts==before);
#undef STEP
 return status;
}
static void class_deep_cases(void) {
 CHECK(!runtime_live && !runtime_bytes);XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
 XrXirTypeArena *arena=deep_arena(domain,true);XrXirValue text={0};CHECK(xr_xir_string_new(domain,"deep",4,&text)==XR_XIR_VALUE_OK);
 const XrXirStorageLayout *c=xr_xir_type_arena_storage(arena,(XrXirType)256),*s=xr_xir_type_arena_storage(arena,(XrXirType)259);
 CHECK(c->value.size==8 && c->body.size==16 && s->value.size==16 && s->field_offsets[0]==0 && s->field_offsets[1]==8);
 printf("storage classheader=%llu structheader=%llu frame=%zu\n", (unsigned long long)array_header_bytes(arena,(XrXirType)256), (unsigned long long)array_header_bytes(arena,(XrXirType)259),sizeof(StorageFrame));
 size_t baseline=runtime_live,bytes=runtime_bytes,sites=0;
 for(size_t pass=0;pass<=sites;++pass){runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
 XrXirValueStatus status=deep_sequence(arena,domain,text);if(!pass){CHECK(status==XR_XIR_VALUE_OK);sites=runtime_attempts;CHECK(sites>0);}else CHECK(status==XR_XIR_VALUE_OOM);
 CHECK(runtime_live==baseline && runtime_bytes==bytes);}
 runtime_fail_at=SIZE_MAX;printf("nested class deep OOM %zu sites; classdepth=%u owned=%u structdepth=%u owned=%u\n",sites,c->depth,c->owned_depth,s->depth,s->owned_depth);
 xr_xir_value_drop(&text);xr_xir_type_arena_drop(arena);xr_xir_domain_drop(domain);CHECK(!runtime_live&&!runtime_bytes);
}
static void class_cross_arena_cases(void) {
 XrXirDomain *domain=NULL,*other=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
 CHECK(xr_xir_domain_new(1048576,&other)==XR_XIR_VALUE_OK);
 XrXirTypeArena *arena=deep_arena(domain,true),*same_name=deep_arena(domain,false);
 XrXirValue text={0},object={0},out={0};CHECK(xr_xir_string_new(domain,"old",3,&text)==XR_XIR_VALUE_OK);
 XrXirValue fields[]={{XR_XIR_I64,0,40},text};XrXirValueAdmission a={arena,domain,NULL,NULL,100000,65536};
 CHECK(xr_xir_class_new((XrXirType)256,fields,2,&a,&object)==XR_XIR_VALUE_OK);
 XrXirValueAdmission bad=a;bad.arena=same_name;
 CHECK(xr_xir_class_set(&object,0,&fields[0],&bad)==XR_XIR_VALUE_BAD_ARGUMENT);
 CHECK(xr_xir_value_admit(&object,(XrXirType)256,&bad)==XR_XIR_VALUE_BAD_ARGUMENT);
 bad=a;bad.domain=other;CHECK(xr_xir_class_set(&object,0,&fields[0],&bad)==XR_XIR_VALUE_BAD_ARGUMENT);
 bad=a;bad.domain=NULL;CHECK(xr_xir_class_set(&object,0,&fields[0],&bad)==XR_XIR_VALUE_BAD_ARGUMENT);
 CHECK(xr_xir_class_get(&object,2,&out)==XR_XIR_VALUE_BAD_ARGUMENT && !out.type);
 size_t before=runtime_attempts;CHECK(xr_xir_class_set(&object,1,&text,&a)==XR_XIR_VALUE_OK);CHECK(runtime_attempts==before);
 xr_xir_value_drop(&text);xr_xir_type_arena_drop(same_name);xr_xir_type_arena_drop(arena);xr_xir_domain_drop(domain);xr_xir_domain_drop(other);
 CHECK(xr_xir_class_get(&object,1,&out)==XR_XIR_VALUE_OK);xr_xir_value_drop(&object);
 const char *bytes=NULL;size_t size=0;CHECK(xr_xir_string_view(&out,&bytes,&size)&&size==3&&!memcmp(bytes,"old",3));
 xr_xir_value_drop(&out);CHECK(!runtime_live&&!runtime_bytes);
}
#include "xir_class_owned_fixture.h"
static XrXirProgram *class_exit_program(unsigned mode,uint32_t *entry) {
 ClassOwnedFixture f;class_owned_fixture(&f);XrXirType type=(XrXirType)256;
 XrXirInstruction root_ops[]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},{XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
 XrXirBlock root_block={0,2,0,0};f.functions[4]=(XrXirFunction){"root",4,NULL,0,XR_XIR_I64,&root_block,1,root_ops,2,NULL,0};
 f.generics[4]=(XrXirGeneric){0};f.module.generics=NULL;f.declarations.entry_function=4;f.identities[3].exported=1;
 f.generics[3]=(XrXirGeneric){0};f.functions[3].result=type;f.functions[3].operand_count=2;
 if(mode==1){f.entry[3]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}};
 f.entry[4]=(XrXirInstruction){XR_XIR_DIV_INT,XR_XIR_I64,{0,3},{0},0,{0}};}
 else {f.entry[3]=(XrXirInstruction){XR_XIR_SUSPEND,XR_XIR_UNIT,{0},{0},0,{0}};
 f.entry[4]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},1,{0}};}
 f.entry[5]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}};
 f.functions[3].instruction_count=6;f.blocks[3].count=6;
 XrXirArtifact *checked=NULL,*special=NULL,*lowered=NULL;
 XrXirDiagnostic diagnostic={0};XrXirStatus verify=xr_xir_check(&f.module,NULL,&checked,&diagnostic);
 if(verify!=XR_XIR_OK)fprintf(stderr,"mode%u check %u function%u instruction%u reason%u\n",mode,verify,diagnostic.function,diagnostic.instruction,diagnostic.reason);CHECK(verify==XR_XIR_OK);
 CHECK(xr_xir_specialize(checked,NULL,&special,NULL)==XR_XIR_OK);xr_xir_artifact_free(checked);
 XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_lower(special,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_artifact_free(special);
 const XrXirModule *closed=xr_xir_artifact_module(lowered);*entry=UINT32_MAX;
 for(uint32_t index=0;index<closed->function_count;++index)if(closed->functions[index].name_length==5&&!memcmp(closed->functions[index].name,"entry",5))*entry=index;CHECK(*entry!=UINT32_MAX);
 XrXirProgram *program=NULL;CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);return program;
}
static void class_exit_cases(void) {
 for(unsigned mode=0;mode<3;++mode){uint32_t entry=UINT32_MAX;XrXirProgram *program=class_exit_program(mode,&entry);size_t base=runtime_live,bytes=runtime_bytes,sites=0;
 for(size_t pass=0;pass<=sites;++pass){runtime_fail_at=pass?pass-1:SIZE_MAX;runtime_attempts=0;XrXirInstance *instance=NULL;XrXirValue value={0};
 XrXirInstanceConfig config=xr_xir_instance_defaults();XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
 if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,entry,NULL,0);
 if(status==XR_XIR_CALL_READY){XrXirInstanceResult result=xr_xir_instance_poll(instance);status=result.outcome.status;
 if(status==XR_XIR_CALL_SUSPENDED){if(mode==0)status=xr_xir_instance_stop(instance);
 else {status=xr_xir_instance_resume(instance,result.epoch,result.outcome.wake);if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll(instance).outcome.status;}}}
 if(!pass){CHECK(status==(mode==0?XR_XIR_CALL_READY:mode==1?XR_XIR_CALL_DIVIDE_BY_ZERO:XR_XIR_CALL_RETURNED));sites=runtime_attempts;CHECK(sites>0);}
 else CHECK(status==XR_XIR_CALL_OOM);
 if(status==XR_XIR_CALL_RETURNED){CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);XrXirValue count={0};CHECK(xr_xir_class_get(&value,0,&count)==XR_XIR_VALUE_OK&&count.payload==40);xr_xir_value_drop(&count);}
 if(instance)CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_value_drop(&value);CHECK(runtime_live==base&&runtime_bytes==bytes);
 }
 runtime_fail_at=SIZE_MAX;printf("class exit mode%u OOM %zu sites\n",mode,sites);xr_xir_program_drop(program);CHECK(!runtime_live&&!runtime_bytes);}
}
static void class_program_retained_case(void) {
 uint32_t entry=UINT32_MAX;XrXirProgram *program=class_exit_program(2,&entry);
 XrXirInstance *instance=NULL;XrXirInstanceConfig config=xr_xir_instance_defaults();
 CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
 CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
 XrXirInstanceResult result=xr_xir_instance_poll(instance);CHECK(result.outcome.status==XR_XIR_CALL_SUSPENDED);
 CHECK(xr_xir_instance_resume(instance,result.epoch,result.outcome.wake)==XR_XIR_CALL_READY);
 CHECK(xr_xir_instance_poll(instance).outcome.status==XR_XIR_CALL_RETURNED);
 XrXirValue object={0},count={0},text={0};CHECK(xr_xir_instance_take_result(instance,&object)==XR_XIR_CALL_RETURNED);
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_program_drop(program);
 CHECK(xr_xir_class_get(&object,0,&count)==XR_XIR_VALUE_OK&&count.type==XR_XIR_I64&&count.payload==40);
 CHECK(xr_xir_class_get(&object,1,&text)==XR_XIR_VALUE_OK);xr_xir_value_drop(&object);xr_xir_value_drop(&count);
 const char *bytes=NULL;size_t size=0;CHECK(xr_xir_string_view(&text,&bytes,&size)&&size==7&&!memcmp(bytes,"counter",7));
 size_t before=runtime_attempts;xr_xir_value_drop(&text);CHECK(runtime_attempts==before);CHECK(!runtime_live&&!runtime_bytes);
}
#endif
