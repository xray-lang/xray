/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_records.c - Exact record schemas, independent cold imports and owner retirement
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "toolchain/xcompiler_session.h"
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"record %d: %s\n",__LINE__,#c);exit(1);}}while(0)

static XrXirCompileContext context;
static void baseline(uint64_t held) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
    CHECK(stats.held_bytes==held);
}
static void raw_case(unsigned invalid) {
    XrXirType arguments[]={XR_XIR_I64,XR_XIR_I64};XrXirConstraint constraints[2]={{0},{0}};
    XrXirNominalField fields[]={{{"x",1},(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,XR_XIR_FIELD_MUTABLE},
        {{"x",1},(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1),XR_XIR_FIELD_MUTABLE}};
    XrXirNominalDeclaration declaration={.module={"records",7},.name={"shape",5},.exported=1,
        .constraints=constraints,.parameter_count=1,.fields=fields,.field_count=1,.kind=XR_XIR_NOMINAL_STRUCT,.flags=XR_XIR_NOMINAL_RECORD};
    XrXirNominalTable table={.declarations=&declaration,.count=1};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,arguments,1,NULL,0}};
    XrXirTypes types={.nodes=&node,.count=1,.nominals=&table};
    XrXirType record=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirInstruction init={.op=XR_XIR_RETURN};
    XrXirInstruction code[]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42},
        {.op=XR_XIR_STRUCT_NEW,.type=record,.args={0,1}},
        {.op=XR_XIR_STRUCT_GET,.type=XR_XIR_I64,.args={1,0}},
        {.op=XR_XIR_RETURN,.args={2,0}}};
    uint32_t operand=0;XrXirBlock blocks[]={{.first=0,.count=1},{.first=0,.count=4}};
    XrXirFunction functions[]={{.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=blocks,.block_count=1,.instructions=&init,.instruction_count=1,.error_type=XR_XIR_NEVER},
        {.name="entry",.name_length=5,.result=XR_XIR_I64,.blocks=blocks+1,.block_count=1,.instructions=code,.instruction_count=4,.operands=&operand,.operand_count=1,.error_type=XR_XIR_NEVER}};
    XrXirSourceModule module={.name="records",.name_length=7};
    XrXirFunctionIdentity ids[2]={{0},{.exported=1}};
    XrXirDeclarations declarations={.modules=&module,.module_count=1,.functions=ids,.entry_function=1};
    XrXirModule built={.stage=XR_XIR_BUILT,.functions=functions,.function_count=2,.declarations=&declarations,.types=&types,.linkage_kind=XR_XIR_PROGRAM};
    XrXirLiteral literal={"wrong",5};
    XrXirStatus expected=XR_XIR_BAD_TYPE;
    switch(invalid) {
    case 0: expected=XR_XIR_OK;break;
    case 1: declaration.flags|=XR_XIR_NOMINAL_MODULE_LITERAL;expected=XR_XIR_BAD_STRUCTURE;break;
    case 2: fields[0].type=XR_XIR_I64;break;
    case 3: fields[0].flags=XR_XIR_FIELD_PRIVATE;break;
    case 4: declaration.kind=XR_XIR_NOMINAL_CLASS;expected=XR_XIR_BAD_STRUCTURE;break;
    case 5: declaration.native.native_id=1;expected=XR_XIR_BAD_STRUCTURE;break;
    case 6: constraints[0].markers=XR_XIR_CONSTRAINT_SENDABLE;break;
    case 7: declaration.parameter_count=0;declaration.constraints=NULL;expected=XR_XIR_BAD_STRUCTURE;break;
    case 8: ids[1].nominal_owner=1;ids[1].method_kind=XR_XIR_MEMBER_HELPER;
        CHECK(xr_xir_compile_method_signature_verify(&context,&built,1)==XR_XIR_BAD_TYPE);
        declaration.flags=0;CHECK(xr_xir_compile_method_signature_verify(&context,&built,1)==XR_XIR_OK);
        declaration.flags=XR_XIR_NOMINAL_RECORD;expected=XR_XIR_BAD_STRUCTURE;break;
    case 9: code[0].op=XR_XIR_CONST_STRING;code[0].type=XR_XIR_STRING;code[0].immediate=0;
        declarations.literals=&literal;declarations.literal_count=1;break;
    case 10: code[2].type=XR_XIR_BOOL;break;
    case 11: declaration.field_count=declaration.parameter_count=2;node.nominal.argument_count=2;expected=XR_XIR_BAD_STRUCTURE;break;
    default: CHECK(false);
    }
    XrXirArtifact *owned=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&context,&built,&owned,&diagnostic);
    if(status!=expected)fprintf(stderr,"raw %u got %u expected %u\n",invalid,status,expected);
    CHECK(status==expected && (owned!=NULL)==(invalid==0));
    if(owned) {
        XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(owned,&packet,NULL)==XR_XIR_OK);
        xr_xir_compile_artifact_free(owned);owned=NULL;
        CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&owned,NULL)==XR_XIR_OK);
        xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(owned);
    }
}
static void lowered_identity_case(bool record,bool wrong) {
    XrXirNominalFieldIdentity field={.name={"x",1},.flags=XR_XIR_FIELD_MUTABLE};
    XrXirNominalIdentity identity={.module={"records",7},.name={"shape",5},.exported=1,.arity=1,
        .fields=&field,.field_count=1,.kind=XR_XIR_NOMINAL_STRUCT,.flags=record?XR_XIR_NOMINAL_RECORD:0};
    XrXirNominalTable table={.count=1,.identities=&identity};
    XrXirType argument=XR_XIR_I64,physical=wrong?XR_XIR_STRING:XR_XIR_I64;
    XrXirTypeNode node={.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,&argument,1,&physical,1}};
    XrXirTypes types={.nodes=&node,.count=1,.nominals=&table};
    XrXirStatus status=xr_xir_compile_types_structure_verify(&context,&types);
    XrXirStatus expected=wrong&&record?XR_XIR_BAD_TYPE:XR_XIR_OK;
    if(status!=expected)fprintf(stderr,"Lowered record physical field %s status%u expected%u\n",wrong?"mismatch":"exact",status,expected);
    CHECK(status==expected);
}
static void write_text(const char *root,const char *name,const char *text,char *path,size_t capacity) {
    int n=snprintf(path,capacity,"%s/%s",root,name);CHECK(n>0&&(size_t)n<capacity);
    FILE *file=fopen(path,"wb");CHECK(file);size_t length=strlen(text);CHECK(fwrite(text,1,length,file)==length&&!fclose(file));
}
static XrXirArtifact *source(const char *path,const XrModuleIdentityAuthority *authority,XrXirLinkageKind linkage,const XrXirLibraryCatalog *catalog) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceRequest request={.session=session,.entry_path=path,.authority=authority,.context=&context,.linkage_kind=linkage,.libraries=catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};XrXirSourceFailure failure={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,&failure);
    if(status!=XR_XIR_OK)fprintf(stderr,"source %s status%u %d:%d %s\n",path,status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
    XrXirArtifact *owned=result.checked;result.checked=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);return owned;
}
static void run_result(XrXirArtifact *checked) {
    XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t entry=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)if(module->declarations->functions[f].module==module->declarations->root_module&&
        module->functions[f].name_length==6&&!memcmp(module->functions[f].name,"result",6)){CHECK(entry==UINT32_MAX);entry=f;}
    CHECK(entry!=UINT32_MAX);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64&&value.payload==42);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
}
static void previous_contract_packets(void) {
    const char *names[]={"exported-i64-const-source.chk","exported-i64-const-catalog.chk",
        "exported-f64-const-source.chk","exported-f64-const-catalog.chk"};
    for(unsigned i=0;i<4;++i) {
        char path[2048];int n=snprintf(path,sizeof(path),"%s/%s",XR_RECORD_PREVIOUS_PACKETS,names[i]);CHECK(n>0&&(size_t)n<sizeof(path));
        FILE *file=fopen(path,"rb");CHECK(file&&!fseek(file,0,SEEK_END));long size=ftell(file);
        CHECK(size>=64&&size<=16777216&&!fseek(file,0,SEEK_SET));uint8_t *bytes=xr_malloc((size_t)size);CHECK(bytes);
        CHECK(fread(bytes,1,(size_t)size,file)==(size_t)size&&!ferror(file)&&!fclose(file));
        CHECK(bytes[8]==39&&!bytes[9]&&!bytes[10]&&!bytes[11]&&bytes[12]==107&&!bytes[13]&&!bytes[14]&&!bytes[15]);
        XrSHA256Context sha;uint8_t digest[32];xr_sha256_init(&sha);xr_sha256_update(&sha,bytes,32);
        xr_sha256_update(&sha,bytes+64,(size_t)size-64);xr_sha256_final(&sha,digest);CHECK(!memcmp(digest,bytes+32,32));
        XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
        XrXirArtifact *output=NULL;XrXirDiagnostic diagnostic={0};
        CHECK(xr_xir_compile_checked_read(&context,bytes,(size_t)size,&output,&diagnostic)==XR_XIR_BAD_STRUCTURE&&!output);
        CHECK(diagnostic.status==XR_XIR_BAD_STRUCTURE&&xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(before.held_bytes==after.held_bytes&&before.allocation_count==after.allocation_count);xr_free(bytes);
    }
}
static void cold_libraries(const char *root) {
    const char *texts[]={"export fn left()->{x:i64,y:string} {return {y:\"alice\",x:40}}\n",
        "export fn right()->{y:string,x:i64} {return {x:41,y:\"bob\"}}\n"};
    const char *names[]={"left.xr","right.xr"};
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirCheckedPacket packets[2]={{0},{0}};XrXirLibraryInput inputs[2]={{0},{0}};
    XrXirLibraryModuleInput bindings[2]={ {authority,names[0]}, {authority,names[1]} };
    for(unsigned i=0;i<2;++i) {
        char path[2048];write_text(root,names[i],texts[i],path,sizeof(path));
        XrXirArtifact *checked=source(path,&authority,XR_XIR_LIBRARY,NULL);
        CHECK(xr_xir_compile_checked_write(checked,&packets[i],NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
        inputs[i]=(XrXirLibraryInput){.packet=packets[i].bytes,.length=packets[i].length,.modules=bindings+i,.module_count=1};
        xr_sha256(packets[i].bytes,packets[i].length,inputs[i].sha256);CHECK(!remove(path));
    }
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(&context,inputs,2,&catalog)==XR_XIR_OK);
    for(unsigned i=0;i<2;++i)xr_xir_compile_checked_packet_free(packets+i);
    char path[2048];write_text(root,"root.xr","import {left} from \"./left\"\nimport {right} from \"./right\"\n"
        "fn get(v:{x:i64,y:string})->i64 {return v.x}\nexport fn result()->i64 {return get(left())+get(right())-39}\n",path,sizeof(path));
    XrXirArtifact *checked=source(path,&authority,XR_XIR_PROGRAM,catalog);
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);uint32_t records=0;
    for(uint32_t n=0;n<module->types->nominals->count;++n)if(module->types->nominals->declarations[n].flags&XR_XIR_NOMINAL_RECORD)++records;
    CHECK(records==1);xr_xir_compile_library_catalog_free(catalog);CHECK(!remove(path));run_result(checked);
}
int main(int argc,char **argv) {
    CHECK(argc==2);const XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_FINITE,.memory_limit=67108864,.profile_allocations=true};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStats start={0};CHECK(xr_compile_resources_stats(context.resources,&start)==XR_COMPILE_RESOURCE_OK);
    for(unsigned invalid=0;invalid<12;++invalid){raw_case(invalid);baseline(start.held_bytes);}
    lowered_identity_case(true,false);baseline(start.held_bytes);lowered_identity_case(true,true);baseline(start.held_bytes);
    lowered_identity_case(false,true);baseline(start.held_bytes);
    previous_contract_packets();baseline(start.held_bytes);
    cold_libraries(argv[1]);baseline(start.held_bytes);xr_compile_resources_release(context.resources);
    puts("record exact schema/typed operand/nominal authority 12 cases/3 Lowered descriptor cases/4 real previous107 packets; two independent cold libraries/one shape/actual callee42/retirement PASS");return 0;
}
