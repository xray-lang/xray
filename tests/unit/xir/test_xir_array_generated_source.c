/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_generated_source.c - Source-backed Array execution and ownership qualification
 *
 * KEY CONCEPT:
 *   Source owns the finite Checked and Lowered graph used by both execution backends.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while(0)
#include "xir_array_program_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir_array_generated_cases.h"
static ArrayProgramEntries array_entries(const XrXirModule *module) {
    CHECK(module && module->declarations);ArrayProgramEntries result;
    for(unsigned n=0;n<ARRAY_ENTRY_COUNT;++n) {
        result.entries[n]=UINT32_MAX;size_t length=strlen(array_entry_names[n]);
        for(uint32_t f=0;f<module->function_count;++f) {
            const XrXirFunction *fn=module->functions+f;
            if(fn->name_length!=length || memcmp(fn->name,array_entry_names[n],length))continue;
            if(module->declarations->functions[f].module!=module->declarations->root_module)continue;
            CHECK(result.entries[n]==UINT32_MAX && module->declarations->functions[f].exported);
            CHECK(fn->parameter_count==(n==ARRAY_DEFAULT || n==ARRAY_APPEND?1u:0u));
            if(n==ARRAY_DEFAULT || n==ARRAY_APPEND)CHECK(fn->parameters[0]==XR_XIR_I64);
            result.entries[n]=f;
        }
        CHECK(result.entries[n]!=UINT32_MAX);
    }
    return result;
}
static XrXirStatus array_build(ArrayCompileOwner *owner,XrXirSourceProduct **product) {
    XrCompilerSession *session=NULL;
    XrCompilerSessionStatus ss=xr_compile_session_new(owner->context.resources,&session);
    if(ss!=XR_COMPILER_SESSION_OK)return ss==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_ARRAY_FIXTURES};
    XrXirSourceProductRequest request={{session,XR_ARRAY_FIXTURES "/root.xr",&authority,&owner->context,
        XR_ARRAY_STDLIB,NULL,XR_XIR_PROGRAM,NULL},{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_product_build(&request,product,&diagnostic);
    if(status!=XR_XIR_OK && !array_program_compile_injected)fprintf(stderr,"Array source status=%u stage=%u line=%d %s\n",status,diagnostic.stage,diagnostic.source.line,diagnostic.source.message);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);xr_compile_session_free(session);return status;
}
static ArrayProgramEntries array_product_entries(ArrayCompileOwner *owner,XrXirSourceProduct *product) {
    XrXirSourceProductPacketView packet={0};
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_checked_read(&owner->context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    ArrayProgramEntries result=array_entries(xr_xir_compile_artifact_module(checked));xr_xir_compile_artifact_free(checked);return result;
}
static void array_write(ArrayCompileOwner *owner,XrXirSourceProduct *product,const char *path) {
    ArrayProgramEntries entries=array_product_entries(owner,product);XrXirCSource output={0};
    CHECK(xr_xir_compile_source_product_emit(product,"array_generated",UINT64_C(1)*1024*1024,&output)==XR_XIR_OK);
    FILE *file=fopen(path,"wb");CHECK(file && fwrite(output.text,1,output.length,file)==output.length);
    CHECK(fputs("\nconst uint32_t array_generated_selection[11]={",file)>=0);
    for(unsigned i=0;i<ARRAY_ENTRY_COUNT;++i)CHECK(fprintf(file,"%s%uu",i?",":"",entries.entries[i])>0);
    CHECK(fputs("};\n",file)>=0 && !fclose(file));xr_xir_compile_c_source_free(&output);
}
typedef struct ArrayAttack { unsigned entry;XrXirOp op;unsigned kind;XrXirStatus expected;const char *reason; } ArrayAttack;
static uint32_t array_find_op(const XrXirFunction *fn,XrXirOp op) {
    for(uint32_t i=0;i<fn->instruction_count;++i)if(fn->instructions[i].op==op)return i;
    fprintf(stderr,"missing op=%u fn=%.*s\n",op,(int)fn->name_length,fn->name);
    CHECK(false);return UINT32_MAX;
}
static void array_lowered_attack(ArrayAttack attack) {
    ArrayCompileOwner owner;CHECK(array_compile_owner_new(array_compile_caps(),&owner)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceProduct *product=NULL;CHECK(array_build(&owner,&product)==XR_XIR_OK);
    XrXirSourceProductPacketView packet={0};CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    XrXirArtifact *checked=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    ArrayProgramEntries entries=array_entries(xr_xir_compile_artifact_module(checked));
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(checked,&target,&lowered,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t entry=entries.entries[attack.entry];XrXirFunction *fn=(XrXirFunction *)(uintptr_t)(module->functions+entry);
    uint32_t index=array_find_op(fn,attack.op);XrXirInstruction *op=(XrXirInstruction *)(uintptr_t)(fn->instructions+index);
    XrXirInstruction saved=*op;XrXirType saved_parameter=fn->parameter_count?fn->parameters[0]:XR_XIR_UNIT;
    if(attack.kind==1)op->type=XR_XIR_I64;
    else if(attack.kind==2)op->type=XR_XIR_UNIT;
    else if(attack.kind==3)op->immediate=1;
    else if(attack.kind==4)op->args[0]=UINT32_MAX;
    else if(attack.kind==5) {
        XrXirType cell=xr_xir_operand_type(fn,op->args[0]);uint32_t future=UINT32_MAX;
        for(uint32_t i=index+1;i<fn->instruction_count;++i)
            if(fn->instructions[i].op==XR_XIR_CELL_NEW && fn->instructions[i].type==cell) {future=i;break;}
        CHECK(future!=UINT32_MAX);op->args[0]=fn->parameter_count+future;
    }
    else if(attack.kind==6)op->args[0]=0;
    else if(attack.kind==7)op->args[1]=fn->parameter_count+array_find_op(fn,XR_XIR_ARRAY_NEW);
    else if(attack.kind==8)((XrXirType *)(uintptr_t)fn->parameters)[0]=XR_XIR_STRING;
    else if(attack.kind==9)op->type=module->functions[entries.entries[ARRAY_INTEGERS]].instructions[
        array_find_op(module->functions+entries.entries[ARRAY_INTEGERS],XR_XIR_ARRAY_NEW)].type;
    else if(attack.kind==10)op->args[0]=fn->operand_count+1;
    else if(attack.kind==11)op->type_arguments[0]=1;
    else if(attack.kind==12)op->targets[0]=1;
    else if(attack.kind==14)op->args[1]=65537;
    else if(attack.kind==13)((XrXirType *)(uintptr_t)fn->parameters)[0]=XR_XIR_BOOL;
    else CHECK(false);
    XrXirDiagnostic diagnostic={0};XrXirStatus status=xr_xir_compile_verify(&owner.context,module,&diagnostic);
    if(status!=attack.expected)fprintf(stderr,"Array attack %s actual=%u expected=%u fn=%u block=%u instruction=%u\n",attack.reason,status,attack.expected,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==attack.expected && diagnostic.status==status);
    *op=saved;if(fn->parameter_count)((XrXirType *)(uintptr_t)fn->parameters)[0]=saved_parameter;
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(checked);xr_xir_compile_source_product_free(product);
    CHECK(!runtime_live && !runtime_bytes);array_compile_owner_free(&owner);
}
static void array_source_reject(const char *file,const char *message) {
    ArrayCompileOwner owner;CHECK(array_compile_owner_new(array_compile_caps(),&owner)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    char path[1024];int length=snprintf(path,sizeof(path),"%s/%s",XR_ARRAY_FIXTURES,file);
    CHECK(length>0 && (size_t)length<sizeof(path));
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_ARRAY_FIXTURES};
    XrXirSourceProductRequest request={{session,path,&authority,&owner.context,XR_ARRAY_STDLIB,NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    if(status!=XR_XIR_BAD_TYPE)fprintf(stderr,"Source Array reject %s status=%u message=%s\n",file,status,diagnostic.source.message);
    CHECK(status==XR_XIR_BAD_TYPE && !product && diagnostic.stage==XR_XIR_SOURCE_PRODUCT_CHECK && diagnostic.source.line==1);
    if(message)CHECK(strstr(diagnostic.source.message,message));
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);xr_compile_session_free(session);
    CHECK(!runtime_live && !runtime_bytes);array_compile_owner_free(&owner);
}
static void array_lowered_admission(void) {
    const ArrayAttack attacks[]={
        {ARRAY_STRINGS,XR_XIR_ARRAY_NEW,9,XR_XIR_BAD_TYPE,"Array element type"},
        {ARRAY_STRINGS,XR_XIR_ARRAY_NEW,2,XR_XIR_BAD_TYPE,"owned constructor result"},
        {ARRAY_DEFAULT,XR_XIR_ARRAY_LEN,6,XR_XIR_BAD_TYPE,"length scalar receiver"},
        {ARRAY_DEFAULT,XR_XIR_ARRAY_LEN,4,XR_XIR_BAD_TYPE,"length missing value"},
        {ARRAY_STRINGS,XR_XIR_CELL_READ,5,XR_XIR_BAD_DOMINANCE,"copy before definition"},
        {ARRAY_STRINGS,XR_XIR_ARRAY_NEW,3,XR_XIR_BAD_STRUCTURE,"constructor immediate"},
        {ARRAY_STRINGS,XR_XIR_ARRAY_NEW,10,XR_XIR_BAD_STRUCTURE,"constructor operand span"},
        {ARRAY_STRINGS,XR_XIR_ARRAY_NEW,14,XR_XIR_BAD_STRUCTURE,"constructor operand count above structural limit"},
        {ARRAY_ALIAS_CHANGED,XR_XIR_CELL_READ,1,XR_XIR_BAD_TYPE,"retained owner type"},
        {ARRAY_ALIAS_CHANGED,XR_XIR_CELL_READ,3,XR_XIR_BAD_STRUCTURE,"retained owner immediate"},
        {ARRAY_ALIAS_CHANGED,XR_XIR_CELL_READ,4,XR_XIR_BAD_TYPE,"retained owner missing"},
        {ARRAY_ALIAS_CHANGED,XR_XIR_CELL_READ,5,XR_XIR_BAD_DOMINANCE,"retained owner forward use"},
        {ARRAY_APPEND,XR_XIR_ARRAY_PUSH,8,XR_XIR_BAD_TYPE,"append parameter type"},
        {ARRAY_APPEND,XR_XIR_ARRAY_PUSH,6,XR_XIR_BAD_TYPE,"append root place"},
        {ARRAY_APPEND,XR_XIR_ARRAY_PUSH,1,XR_XIR_BAD_TYPE,"append unit result"},
        {ARRAY_APPEND,XR_XIR_ARRAY_PUSH,7,XR_XIR_BAD_TYPE,"append element type"},
        {ARRAY_APPEND,XR_XIR_ARRAY_PUSH,12,XR_XIR_BAD_STRUCTURE,"append extraneous edge"},
        {ARRAY_DEFAULT,XR_XIR_ARRAY_REPEAT,13,XR_XIR_BAD_TYPE,"default count bool"},
        {ARRAY_DEFAULT,XR_XIR_ARRAY_REPEAT,11,XR_XIR_BAD_STRUCTURE,"default spurious type argument"},
        {ARRAY_DEFAULT,XR_XIR_ARRAY_REPEAT,2,XR_XIR_BAD_TYPE,"default owner result"}};
    for(size_t i=0;i<sizeof(attacks)/sizeof(*attacks);++i)array_lowered_attack(attacks[i]);
    array_source_reject("reject-default-string.xr","no admitted default initializer");
    array_source_reject("reject-default-bool.xr",NULL);
    array_source_reject("reject-read-push.xr","mutable");
    array_source_reject("reject-move-copyable.xr","parameter contract");
    puts("Array 20 Source-derived Lowered graph refusals; restored artifact reverified after each attack");
}
static XrXirStatus array_compiler_probe(XrCompileResourceLimits caps,XrCompileResourceStats *stats) {
    ArrayCompileOwner owner;XrCompileResourceStatus opened=array_compile_owner_new(caps,&owner);
    if(opened!=XR_COMPILE_RESOURCE_OK) {
        array_compile_owner_free(&owner);
        return opened==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
    }
    XrXirSourceProduct *product=NULL;XrXirProgram *program=NULL;XrXirCSource output={0};
    XrXirStatus status=array_build(&owner,&product);
    if(status!=XR_XIR_OK)CHECK(!product);
    if(status==XR_XIR_OK) {
        status=xr_xir_compile_source_product_emit(product,"array_generated",UINT64_C(1)*1024*1024,&output);
        if(status!=XR_XIR_OK)CHECK(!output.text && !output.length);
    }
    if(status==XR_XIR_OK) {
        status=xr_xir_compile_source_product_vm_take(product,&program);
        if(status!=XR_XIR_OK)CHECK(!program);
    }
    xr_xir_compile_c_source_free(&output);
    xr_xir_compile_source_product_free(product);xr_xir_compile_program_drop(program);
    if(stats)*stats=array_compile_stats(&owner);
    CHECK(!runtime_live && !runtime_bytes);array_compile_owner_free(&owner);return status;
}
static void array_compiler_faults(void) {
    array_program_compile_attempts=0;XrCompileResourceStats stats={0};
    CHECK(array_compiler_probe(array_compile_caps(),&stats)==XR_XIR_OK);
    size_t sites=array_program_compile_attempts;CHECK(sites && sites<20000);
    printf("Array complete compiler baseline sites=%zu allocated=%llu peak=%llu work=%llu\n",sites,
        (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
    for(size_t ordinal=0;ordinal<sites;++ordinal) {
        array_program_compile_attempts=0;array_program_compile_fail_at=ordinal;array_program_compile_injected=false;
        XrXirStatus status=array_compiler_probe(array_compile_caps(),NULL);
        array_program_compile_fail_at=SIZE_MAX;
        if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"Array compiler ordinal=%zu status=%u injected=%u\n",ordinal,status,array_program_compile_injected);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && array_program_compile_injected && array_program_compile_attempts>ordinal);
        CHECK(!array_program_compile_live && !array_program_compile_bytes && !runtime_live && !runtime_bytes);
    }
    for(unsigned axis=0;axis<3;++axis) {
        XrCompileResourceLimits exact=array_compile_caps();
        if(!axis)exact.allocated_bytes=stats.allocated_bytes;
        else if(axis==1)exact.live_bytes=stats.peak_bytes;else exact.work=stats.work;
        CHECK(array_compiler_probe(exact,NULL)==XR_XIR_OK);
        if(!axis)--exact.allocated_bytes;else if(axis==1)--exact.live_bytes;else --exact.work;
        CHECK(array_compiler_probe(exact,NULL)==XR_XIR_BUDGET);
    }
    printf("Array compiler all %zu actual OOM ordinals and 3 exact/minus1 axes; physical=0/0\n",sites);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);
    if(argc==2 && !strcmp(argv[1],"--admission")) {array_lowered_admission();return 0;}
    if(argc==2 && !strcmp(argv[1],"--compiler")) {array_compiler_faults();return 0;}
    ArrayCompileOwner owner;
    CHECK(array_compile_owner_new(array_compile_caps(),&owner)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceProduct *product=NULL;CHECK(array_build(&owner,&product)==XR_XIR_OK && product);
    CHECK(xr_xir_compile_source_product_context(product)->resources==owner.context.resources);
    if(argc==2)array_write(&owner,product,argv[1]);
    else {
        ArrayProgramEntries entries=array_product_entries(&owner,product);
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK && program);
        xr_xir_compile_source_product_free(product);product=NULL;
        CHECK(xr_xir_compile_program_retain(program));
        for(unsigned mode=0;mode<15;++mode) {
            if(mode)CHECK(xr_xir_compile_program_retain(program));
            const int expected[]={0,2,0,2,1,0,0,0,0,0,0,0,0,0,0};
            CHECK(array_case(program,entries,mode)==expected[mode]);
        }
        xr_xir_compile_program_drop(program);
    }
    if(product)xr_xir_compile_source_product_free(product);
    XrCompileResourceStats stats=array_compile_stats(&owner);
    printf("Array Source finite graph: allocated=%llu peak=%llu work=%llu attempts=%zu compiler/runtime physical0\n",
        (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work,array_program_compile_attempts);
    CHECK(!runtime_live && !runtime_bytes);array_compile_owner_free(&owner);return 0;
}
