/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generated_data.c - Ordinary generator output through full checking
 *
 * KEY CONCEPT:
 *   A source-free library chooses the actual future private field from facts.
 *   Its returned body is data until the receiving definition and checker bind it.
 */
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "xir/xxir_source.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_compile_memory.h"
#include "xir/xxir_types.h"
#include "xir/xxir_generated_data.h"
#include "xir/xxir_derivation_internal.h"
#include "xir/xxir_implementation.h"
#include "xir/xxir_implementation_verify.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while(0)
#include "xir_source_compile_owner.h"
#include "xir_generated_data_source.h"
typedef struct GeneratedFixture {
    XrXirCompileContext context;
    XrXirArtifact *source,*lowered;
    uint8_t identity[32];
    XrXirGeneratedOrigin entry,field;
    char module[2048],path[2048];
    XrXirType references[3];
    uint32_t nominal,protocol,grantor,generator;
    uint64_t baseline;
} GeneratedFixture;
static uint32_t generated_function(const XrXirModule *module,const char *name) {
    size_t length=strlen(name);uint32_t found=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==length && !memcmp(module->functions[f].name,name,length)) {
            CHECK(found==UINT32_MAX);found=f;
        }
    CHECK(found!=UINT32_MAX);return found;
}
static void generated_write(const char *path,const char *text) {
    FILE *file=fopen(path,"wb");size_t size=strlen(text);
    CHECK(file && fwrite(text,1,size,file)==size && !fclose(file));
}
static void generated_absent(const char *path) {
    FILE *file=fopen(path,"rb");CHECK(!file);
}
static void generated_source(GeneratedFixture *f,const char *path,XrXirLinkageKind kind,
    const XrXirLibraryCatalog *catalog,XrXirSourceResult *output) {
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(f->context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_GENERATED_DATA_WORK};
    XrXirSourceRequest request={session,path,&authority,&f->context,NULL,NULL,kind,catalog,NULL};
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,output,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"generated source=%u %d:%d %s\n",(unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && output->checked && output->snapshot);xr_compile_session_free(session);
}
static void generated_bind_definition(GeneratedFixture *f,const XrXirSourceResult *user) {
    const XrXirModule *m=xr_xir_compile_artifact_module(user->checked);
    f->generator=generated_function(m,"generate");
    CHECK(m->declarations->functions[f->generator].exported);
    uint32_t helper=generated_function(m,"privateBody");CHECK(!m->declarations->functions[helper].exported);
    f->nominal=UINT32_MAX;f->protocol=UINT32_MAX;
    for (uint32_t t=0;t<m->types->count;++t) {
        XrXirType type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);XrXirTypeDescription d={0};
        CHECK(xr_xir_compile_type_description(&f->context,m->types,type,&d)==XR_XIR_OK);
        if (d.name.length==6 && !memcmp(d.name.bytes,"Future",6)) {
            CHECK(f->nominal==UINT32_MAX && d.instance && d.field_count==2);
            f->nominal=d.instance->declaration;f->references[0]=type;
            XrXirFieldDescription hidden={0},secret={0};
            CHECK(xr_xir_compile_description_field(&f->context,&d,0,&hidden)==XR_XIR_OK);
            CHECK(xr_xir_compile_description_field(&f->context,&d,1,&secret)==XR_XIR_OK);
            CHECK(hidden.type==XR_XIR_STRING && secret.type==XR_XIR_I64 &&
                hidden.flags==XR_XIR_FIELD_PRIVATE && secret.flags==XR_XIR_FIELD_PRIVATE);
            f->references[1]=hidden.type;f->references[2]=secret.type;
        }
    }
    for (uint32_t p=0;p<m->types->interfaces->count;++p) {
        const XrXirInterfaceDeclaration *d=&m->types->interfaces->declarations[p];
        if (d->name.length==7 && !memcmp(d->name.bytes,"Measure",7)) {CHECK(f->protocol==UINT32_MAX);f->protocol=p;}
    }
    CHECK(f->nominal!=UINT32_MAX && f->protocol!=UINT32_MAX);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(user->snapshot);
    bool found=false;
    for (uint32_t d=0;d<view->declaration_count;++d) {
        const XrXirSourceDeclaration *row=&view->declarations[d];
        if (row->kind!=XR_XIR_SOURCE_MEMBER || strcmp(row->name,"secret")) continue;
        CHECK(!found);found=true;f->grantor=row->range.module;
        const XrXirSourceModule *module=&m->declarations->modules[f->grantor];
        CHECK(module->name_length<sizeof(f->module));memcpy(f->module,module->name,module->name_length);
        f->module[module->name_length]=0;
        f->entry=(XrXirGeneratedOrigin){{f->module,module->name_length},{f->path,(uint32_t)strlen(f->path)},
            {"Future",6},(uint32_t)row->range.line,(uint32_t)row->range.column};
        f->field=f->entry;f->field.field_path=(XrXirGeneratedText){"Future.secret",13};
    }
    CHECK(found && !m->declarations->derivations && !m->declarations->implementations);
}
static void generated_prepare_definition(GeneratedFixture *f) {
    const XrXirModule *source=xr_xir_compile_artifact_module(f->source);
    XrXirDerivationMember members[]={{0,XR_XIR_DERIVE_DESCRIBE},{1,XR_XIR_DERIVE_DESCRIBE|XR_XIR_DERIVE_READ}};
    XrXirDerivationPolicy policy={f->grantor,f->references[0],{f->protocol,NULL,0},f->generator,0,members,2,f->entry};
    XrXirDerivationTable table={&policy,1,NULL,0};XrXirDeclarations declarations=*source->declarations;
    declarations.derivations=&table;XrXirModule built=*source;built.stage=XR_XIR_BUILT;built.provenance=NULL;
    built.declarations=&declarations;XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_check_v2(&f->context,&built,
        xr_xir_compile_artifact_construction(f->source),&checked,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"definition before generator=%u f=%u i=%u\n",(unsigned)status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && checked);xr_xir_compile_artifact_free(f->source);f->source=checked;
    memset(members,0xa5,sizeof(members));memset(&policy,0xa5,sizeof(policy));memset(&table,0xa5,sizeof(table));
}
static void generated_fixture_new(GeneratedFixture *f) {
    memset(f,0,sizeof(*f));XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&f->context.resources)==XR_COMPILE_RESOURCE_OK);
    f->context.limits=xr_xir_compile_default_limits();XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(f->context.resources,&stats)==XR_COMPILE_RESOURCE_OK);f->baseline=stats.live_bytes;
    char library_path[2048];int n=snprintf(library_path,sizeof(library_path),"%s/generator.xr",XR_GENERATED_DATA_WORK);
    CHECK(n>0 && n<(int)sizeof(library_path));
    n=snprintf(f->path,sizeof(f->path),"%s/future.xr",XR_GENERATED_DATA_WORK);CHECK(n>0 && n<(int)sizeof(f->path));
    FILE *previous=fopen(f->path,"rb");if(previous){CHECK(!fclose(previous) && !remove(f->path));}generated_absent(f->path);
    char text[8192];n=snprintf(text,sizeof(text),generated_library_format,generated_schema_source,
        (unsigned)XR_XIR_STRUCT_GET,-(int)XR_XIR_STRING-1,(unsigned)XR_XIR_RETURN,
        (unsigned)XR_XIR_STRUCT_GET,(unsigned)XR_XIR_STRUCT_SET,(unsigned)XR_XIR_RETURN);
    CHECK(n>0 && n<(int)sizeof(text));generated_write(library_path,text);memset(text,0xa5,sizeof(text));
    XrXirSourceResult library={0};generated_source(f,library_path,XR_XIR_LIBRARY,NULL,&library);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(library.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&library);CHECK(!remove(library_path));generated_absent(library_path);
    XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_GENERATED_DATA_WORK},"generator.xr"};
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(&f->context,&input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);memset(&input,0xa5,sizeof(input));
    n=snprintf(text,sizeof(text),generated_user_format,generated_schema_source);CHECK(n>0 && n<(int)sizeof(text));
    generated_write(f->path,text);memset(text,0xa5,sizeof(text));XrXirSourceResult user={0};
    generated_source(f,f->path,XR_XIR_PROGRAM,catalog,&user);xr_xir_compile_library_catalog_free(catalog);
    generated_bind_definition(f,&user);CHECK(!remove(f->path));generated_absent(f->path);generated_absent(library_path);
    CHECK(xr_xir_compile_checked_write(user.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&user);
    CHECK(xr_xir_compile_checked_read(&f->context,packet.bytes,packet.length,&f->source,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    generated_prepare_definition(f);
    XrXirArtifact *closed=NULL;CHECK(xr_xir_compile_specialize(f->source,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);xr_sha256(packet.bytes,packet.length,f->identity);
    xr_xir_compile_checked_packet_free(&packet);const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&f->lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
}
static XrXirValue generated_evaluate(GeneratedFixture *f,int64_t mode) {
    const XrXirModule *m=xr_xir_compile_artifact_module(f->lowered);XrXirValue argument={.type=XR_XIR_I64,.payload=mode};
    XrXirCtfeCallRequest request={.evaluation={.artifact=f->lowered,.function=generated_function(m,"produce"),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.arguments=&argument,.argument_count=1,
        .limits={65536,65536,8388608}},.call_depth=128};
    memcpy(request.evaluation.checked_identity,f->identity,32);XrXirValue value={0};
    XrXirCtfeStatus status=xr_xir_compile_ctfe_calls(&request,&value);
    if(status!=XR_XIR_CTFE_OK)fprintf(stderr,"ordinary generator CTFE mode=%lld status=%u\n",(long long)mode,(unsigned)status);
    CHECK(status==XR_XIR_CTFE_OK);return value;
}
static void *generated_alloc(GeneratedFixture *f,uint32_t count,size_t size) {
    XrXirStatus status=XR_XIR_OK;void *output=xir_compile_calloc(&f->context,count,size,&status);
    CHECK(output && status==XR_XIR_OK);return output;
}
static void generated_metadata_probe(GeneratedFixture *f,const XrXirModule *built) {
    fprintf(stderr,"metadata types=%u declarations=%u generics=%u grants-shape=%u grants=%u implementations=%u defaults=%u\n",
        (unsigned)xr_xir_compile_types_structure_verify(&f->context,built->types),
        (unsigned)xr_xir_compile_declarations_verify(&f->context,built->declarations,built->types,built->function_count,built->linkage_kind),
        (unsigned)xr_xir_compile_generics_structure_verify(&f->context,built),
        (unsigned)xir_derivations_structure_verify(&f->context,built->declarations->derivations),
        (unsigned)xir_derivations_verify(&f->context,built),
        (unsigned)xr_xir_compile_implementations_verify(&f->context,built),
        (unsigned)xr_xir_compile_defaults_verify(&f->context,built));
    for(uint32_t n=0;n<built->function_count;++n){
        const XrXirFunction *fn=&built->functions[n];
        fprintf(stderr,"fn%u %.*s p=%u b=%u i=%u o=%u firstblock=%u/%u\n",n,(int)fn->name_length,fn->name,
            fn->parameter_count,fn->block_count,fn->instruction_count,fn->operand_count,fn->blocks[0].first,fn->blocks[0].count);
    }
}
static XrXirGeneratedBuilt *generated_receive(GeneratedFixture *f,const XrXirGeneratedDataView *data) {
    const XrXirModule *source=xr_xir_compile_artifact_module(f->source);uint32_t count=source->function_count;
    CHECK(data->function_count==1 && data->members[0]==0 && data->roots[0]==0 && !data->literal_count);
    XrXirFunction *functions=generated_alloc(f,count+1,sizeof(*functions));memcpy(functions,source->functions,count*sizeof(*functions));
    XrXirFunctionIdentity *identities=generated_alloc(f,count+1,sizeof(*identities));
    memcpy(identities,source->declarations->functions,count*sizeof(*identities));
    XrXirGeneric *generics=source->generics?generated_alloc(f,count+1,sizeof(*generics)):NULL;
    if(generics){memcpy(generics,source->generics,count*sizeof(*generics));generics[count]=data->generics[0];}
    else CHECK(!data->generics[0].argument_count);
    functions[count]=data->functions[0];
    identities[count]=(XrXirFunctionIdentity){.module=f->grantor,.exported=source->types->nominals->declarations[f->nominal].exported,.nominal_owner=f->nominal+1,
        .method_kind=XR_XIR_READ_METHOD,.derivation=1};
    uint32_t operands[]={0};XrXirBlock block={0,3,0,0};
    XrXirInstruction body[]={
        {.op=XR_XIR_CALL,.type=f->references[0],.immediate=generated_function(source,"make")},
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},.immediate=count},
        {.op=XR_XIR_RETURN,.args={1,0}}};
    uint32_t main=generated_function(source,"main");functions[main].instructions=body;functions[main].instruction_count=3;
    functions[main].blocks=&block;functions[main].block_count=1;functions[main].operands=operands;functions[main].operand_count=1;
    CHECK(source->declarations->derivations && source->declarations->derivations->policy_count==1 &&
        !source->declarations->derivations->count);
    XrXirGeneratedFunctionOrigins *origins=generated_alloc(f,count+1,sizeof(*origins));
    for(uint32_t n=0;n<=count;++n){
        XrXirGeneratedOrigin *rows=generated_alloc(f,functions[n].instruction_count,sizeof(*rows));
        for(uint32_t i=0;i<functions[n].instruction_count;++i)rows[i]=
            n==count && data->instruction_fields[i]>=0?f->field:f->entry;
        origins[n]=(XrXirGeneratedFunctionOrigins){f->entry,rows,functions[n].instruction_count};
    }
    XrXirDerivationGrant binding={count,0,0,0,f->references[0],origins[count]};
    XrXirDerivationTable table={source->declarations->derivations->policies,1,&binding,1};
    XrXirImplementationBinding witness={{f->protocol,NULL,0},0,count};
    XrXirImplementation implementation={f->nominal,{f->protocol,NULL,0},&witness,1,1};
    XrXirImplementationTable implementations={&implementation,1};XrXirDeclarations declarations=*source->declarations;
    declarations.functions=identities;declarations.derivations=&table;declarations.implementations=&implementations;
    XrXirModule built=*source;built.stage=XR_XIR_BUILT;built.provenance=NULL;
    built.functions=functions;built.function_count=count+1;
    built.declarations=&declarations;built.generics=generics;
    const XrXirConstruction *construction=xr_xir_compile_artifact_construction(f->source);
    XrXirGeneratedReceive request={&built,construction,origins,count+1};XrXirGeneratedBuilt *output=NULL;
    XrXirStatus status=xr_xir_compile_generated_receive(&f->context,&request,&output);
    if(status!=XR_XIR_OK){
        generated_metadata_probe(f,&built);
        XrXirArtifact *probe=NULL;XrXirDiagnostic diagnostic={0};
        XrXirStatus raw=xr_xir_compile_check_v2(&f->context,&built,construction,&probe,&diagnostic);
        fprintf(stderr,"ordinary generated receiving status=%u raw=%u f=%u b=%u i=%u origin=%u:%u module=%u functions=%u\n",
            (unsigned)status,(unsigned)raw,diagnostic.function,diagnostic.block,diagnostic.instruction,
            f->entry.line,f->entry.column,f->grantor,count+1);xr_xir_compile_artifact_free(probe);
    }
    CHECK(status==XR_XIR_OK && output);
    for(uint32_t n=0;n<=count;++n)xr_compile_resources_free((void *)origins[n].instructions);
    memset(functions,0xa5,(count+1)*sizeof(*functions));memset(identities,0xa5,(count+1)*sizeof(*identities));
    xr_compile_resources_free(origins);xr_compile_resources_free(functions);xr_compile_resources_free(identities);xr_compile_resources_free(generics);
    return output;
}
static void generated_run(GeneratedFixture *f,XrXirArtifact *checked) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&f->context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    uint32_t main=generated_function(xr_xir_compile_artifact_module(lowered),"main");
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,main,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult outcome=xr_xir_instance_poll_bounded(instance,1024);
    if(outcome.outcome.status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"generated body VM=%u\n",(unsigned)outcome.outcome.status);
    CHECK(outcome.outcome.status==XR_XIR_CALL_RETURNED);XrXirValue result={0};
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && !result.reserved && result.payload==42);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
}
int main(void) {
    GeneratedFixture f;generated_fixture_new(&f);XrXirArtifact *positive=NULL;
    for(int64_t mode=0;mode<5;++mode){
        XrXirValue value=generated_evaluate(&f,mode);XrXirGeneratedData *decoded=NULL;
        const XrXirModule *source=xr_xir_compile_artifact_module(f.source);
        XrXirGeneratedDataRequest request={.value=&value,.types=source->types,.references=f.references,.reference_count=3,
            .first_function=source->function_count,.first_literal=source->declarations->literal_count,.field_count=2,.method_count=1};
        XrXirStatus status=xr_xir_compile_generated_data_decode(&f.context,&request,&decoded);xr_xir_value_drop(&value);
        if(mode>=3){CHECK(status==XR_XIR_BAD_STRUCTURE && !decoded);continue;}
        CHECK(status==XR_XIR_OK && decoded);const XrXirGeneratedDataView *data=xr_xir_compile_generated_data_view(decoded);
        CHECK(data->functions[0].instructions[0].op==XR_XIR_STRUCT_GET && data->functions[0].instructions[0].immediate==1 &&
            data->instruction_fields[0]==1 && data->functions[0].parameters[0]==f.references[0]);
        XrXirGeneratedBuilt *built=generated_receive(&f,data);xr_xir_compile_generated_data_free(decoded);
        XrXirArtifact *checked=NULL;XrXirGeneratedDiagnostic diagnostic={0};status=xr_xir_compile_generated_check(built,&checked,&diagnostic);
        if(!mode){if(status!=XR_XIR_OK)fprintf(stderr,"generated check=%u f=%u i=%u\n",(unsigned)status,diagnostic.cause.function,diagnostic.cause.instruction);CHECK(status==XR_XIR_OK && checked);positive=checked;}
        else{CHECK(status==XR_XIR_BAD_TYPE && !checked && diagnostic.origin && diagnostic.origin->field_path.length==13 &&
            !memcmp(diagnostic.origin->field_path.bytes,"Future.secret",13));}
        xr_xir_compile_generated_free(built);
    }
    xr_xir_compile_artifact_free(f.source);xr_xir_compile_artifact_free(f.lowered);f.source=NULL;f.lowered=NULL;
    generated_run(&f,positive);XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(f.context.resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.live_bytes==f.baseline);
    fprintf(stderr,"ordinary source-free generator/private helper/future private field/Built recheck/packet death/VM=42; compiler physical0/0\n");
    xr_compile_resources_release(f.context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);return 0;
}
