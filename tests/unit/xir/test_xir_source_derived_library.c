/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_derived_library.c - Cold receiving of real derived libraries
 *
 * KEY CONCEPT:
 *   A source-free generator precedes the exported future type. Its derived
 *   methods then cross another complete Checked library before downstream use.
 */
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_derivation_internal.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_generated_schema.h"
#include "xir_source_constructor_names_source.h"
static const XrCompileResourceLimits library_caps={67108864,8388608,128000000};
static const char read_generator_source[]=
    "export fn readGenerate(info:TypeInfo,context:Context)->Generated {\n"
    " const node=info.1[info.0]\n"
    " if(node.7!=1 || len(context.0)!=1 || context.0[0].4!=0 || context.0[0].3!=0){return unsupported()}\n"
    " const field=info.2[node.6]\n"
    " if(field.0!=\"sum\" || field.3!=0){return unsupported()}\n"
    " const method=context.0[0]\n"
    " const functions:Array<FunctionRow>=[(method.0,0,0,method.1,0,1,0,1,0,2,0,0,0,0)]\n"
    " const parameters:Array<i64>=[info.0];const blocks:Array<BlockRow>=[(0,2,0,0)]\n"
    " const code:Array<InstructionRow>=[(%u,field.1,0,0,0,0,field.3,0,0,field.3),(%u,-1,1,0,0,0,0,0,0,-1)]\n"
    " const operands:Array<i64>=[];const literals:Array<string>=[];const arguments:Array<i64>=[]\n"
    " return (1,functions,parameters,blocks,code,operands,literals,arguments)\n}\n";
static const char derived_library_source[]=
    "import {generate,readGenerate} from \"./constructor_names\"\n"
    "export interface CtorNames {parameterNames()->string}\n"
    "export interface Measure {measure()->i64}\n"
    "@derive(CtorNames,generate,construct:true)\n"
    "@derive(Measure,readGenerate,describe:[\"sum\"],read:[\"sum\"])\n"
    "export struct Future {private const sum:i64;private const reserved:i64\n"
    " private init(left:i64,right:i64){this.sum=left+right;this.reserved=99}\n"
    " static seed()->Future{return Future(17,25)} }\n"
    "export fn observed()->string{return Future.seed().parameterNames()}\n";
static const char downstream_source[]=
    "import {Future,observed} from \"./derived_library\"\n"
    "export fn names()->string{return Future.seed().parameterNames()}\n"
    "export fn main()->i64{\n"
    " const folded=comptime Future.seed().parameterNames()\n"
    " const measured=comptime Future.seed().measure()\n"
    " return (folded==\"left:right\" && measured==42 && observed()==folded && names()==folded)?Future.seed().measure():0\n}\n";
static const char downstream_local_source[]=
    "import {Future,observed,CtorNames} from \"./derived_library\"\n"
    "import {generate} from \"./constructor_names\"\n"
    "@derive(CtorNames,generate,construct:true)\n"
    "struct Later {private const total:i64\n"
    " private init(right:i64,left:i64){this.total=right+left}\n"
    " static seed()->Later{return Later(9,7)} }\n"
    "export fn names()->string{return Future.seed().parameterNames()}\n"
    "export fn main()->i64{\n"
    " const folded=comptime Later.seed().parameterNames()\n"
    " const measured=comptime Future.seed().measure()\n"
    " return (folded==\"right:left\" && measured==42 && observed()==\"left:right\" && names()==\"left:right\")?Future.seed().measure():0\n}\n";
static XrXirCompileContext library_context(void) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&library_caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();return context;
}
static void library_absent(const char *logical) {
    char path[2048];int n=snprintf(path,sizeof(path),"%s/%s",XR_DERIVED_LIBRARY_WORK,logical);CHECK(n>0 && n<(int)sizeof(path));
    errno=0;FILE *file=fopen(path,"rb");
    if(file){CHECK(!fclose(file));CHECK(false);}
    CHECK(errno==ENOENT);
}
static XrXirStatus library_source(const XrXirCompileContext *context,const XrXirLibraryCatalog *catalog,
    const char *logical,const char *text,XrXirLinkageKind kind,XrXirSourceResult *output) {
    library_absent(logical);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    char path[2048];int n=snprintf(path,sizeof(path),"%s/%s",XR_DERIVED_LIBRARY_WORK,logical);CHECK(n>0 && n<(int)sizeof(path));
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_DERIVED_LIBRARY_WORK};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog,NULL};
    XrXirSourceText source={logical,text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&source,output,&diagnostic,NULL);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"Derived library %s Source%u %d:%d %s spent=%llu/%llu/%llu\n",logical,(unsigned)status,
        diagnostic.line,diagnostic.column,diagnostic.message,(unsigned long long)stats.allocated_bytes,
        (unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);fflush(stderr);
    if(status==XR_XIR_OK)CHECK(output->checked && output->snapshot);else CHECK(!output->checked && !output->snapshot);
    xr_compile_session_free(session);return status;
}
static XrXirCheckedPacket library_transport(const XrXirCompileContext *transport,XrXirSourceResult *source) {
    XrXirCheckedPacket original={0},copy={0};CHECK(xr_xir_compile_checked_write(source->checked,&original,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(source);void *bytes=NULL;
    CHECK(xr_compile_resources_alloc(transport->resources,original.length,&bytes)==XR_COMPILE_RESOURCE_OK);
    copy.bytes=bytes;copy.length=original.length;memcpy(copy.bytes,original.bytes,copy.length);
    memset(original.bytes,0xa5,original.length);xr_xir_compile_checked_packet_free(&original);return copy;
}
static XrXirLibraryCatalog *library_catalog(const XrXirCompileContext *context,const XrXirCheckedPacket *packet,
    XrXirLibraryModuleInput *bindings,size_t count) {
    XrXirLibraryInput input={packet->bytes,packet->length,{0},bindings,count};xr_sha256(packet->bytes,packet->length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;XrXirStatus status=xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog);
    fprintf(stderr,"Derived library catalog%u modules%zu\n",(unsigned)status,count);CHECK(status==XR_XIR_OK && catalog);return catalog;
}
static uint32_t library_function(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(name) &&
        !memcmp(module->functions[f].name,name,strlen(name))){CHECK(found==UINT32_MAX);found=f;}
    CHECK(found!=UINT32_MAX);return found;
}
static XrXirValue library_execute(const XrXirCompileContext *context,XrXirSourceResult *source) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source->checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(source);XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t entries[]={library_function(module,"names"),library_function(module,"main")};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);XrXirValue retained={0};
    for(unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entries[i],NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,4096).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        if(!i)retained=result;else{CHECK(result.type==XR_XIR_I64 && !result.reserved && result.payload==42);xr_xir_value_drop(&result);}
    }
    xr_xir_compile_program_drop(program);return retained;
}
int main(void) {
    XrXirCompileContext transport=library_context(),producer=library_context();char generator[12000];
    int n=snprintf(generator,sizeof(generator),names_generator,generated_schema_source,XR_XIR_CONST_STRING,XR_XIR_RETURN);
    CHECK(n>0 && n<(int)sizeof(generator));
    int extra=snprintf(generator+n,sizeof(generator)-(size_t)n,read_generator_source,XR_XIR_STRUCT_GET,XR_XIR_RETURN);
    CHECK(extra>0 && (size_t)extra<sizeof(generator)-(size_t)n);XrXirSourceResult source={0};
    CHECK(library_source(&producer,NULL,"constructor_names.xr",generator,XR_XIR_LIBRARY,&source)==XR_XIR_OK);
    memset(generator,0xa5,sizeof(generator));XrXirCheckedPacket g=library_transport(&transport,&source);xr_compile_resources_release(producer.resources);
    XrXirCompileContext middle=library_context();XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_DERIVED_LIBRARY_WORK};
    XrXirLibraryModuleInput generator_binding={authority,"constructor_names.xr"};
    XrXirLibraryCatalog *catalog=library_catalog(&middle,&g,&generator_binding,1);
    char future[sizeof(derived_library_source)];memcpy(future,derived_library_source,sizeof(future));
    CHECK(library_source(&middle,catalog,"derived_library.xr",future,XR_XIR_LIBRARY,&source)==XR_XIR_OK);memset(future,0xa5,sizeof(future));
    xr_xir_compile_library_catalog_free(catalog);const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);
    const XrXirDerivationTable *grants=module->declarations->derivations;
    CHECK(grants && grants->policy_count==2 && grants->count==2 && !grants->policies[0].member_count && grants->policies[0].constructor &&
        grants->policies[1].member_count==1 && !grants->policies[1].constructor && grants->policies[1].members[0].field==0 &&
        grants->policies[1].members[0].operations==(XR_XIR_DERIVE_DESCRIBE|XR_XIR_DERIVE_READ));
    CHECK(grants->records[1].policy==1 && grants->records[1].root==1);
    const XrXirFunction *read=&module->functions[grants->records[1].function];
    CHECK(read->instruction_count==2 && read->instructions[0].op==XR_XIR_STRUCT_GET && read->instructions[0].immediate==0);
    CHECK(module->declarations);fprintf(stderr,"Derived library actual modules%u root%u\n",module->declarations->module_count,module->declarations->root_module);
    for(uint32_t i=0;i<module->declarations->module_count;++i)fprintf(stderr,"Module%u %.*s\n",i,
        (int)module->declarations->modules[i].name_length,module->declarations->modules[i].name);
    CHECK(module->declarations->module_count==2 && module->declarations->root_module==UINT32_MAX);
    XrXirLibraryModuleInput bindings[2]={{0}};const char *paths[]={"derived_library.xr","constructor_names.xr"};
    for(unsigned p=0;p<2;++p) {
        char *canonical=NULL;CHECK(xr_compile_module_identity_from_logical(middle.resources,&authority,paths[p],&canonical)==XR_MODULE_OK);
        unsigned matched=0;
        for(unsigned i=0;i<2;++i)if(module->declarations->modules[i].name_length==strlen(canonical) &&
            !memcmp(module->declarations->modules[i].name,canonical,strlen(canonical))) {
            CHECK(!bindings[i].logical_path);bindings[i]=(XrXirLibraryModuleInput){authority,paths[p]};++matched;
        }
        CHECK(matched==1);xr_compile_resources_free(canonical);
    }
    XrXirCheckedPacket derived=library_transport(&transport,&source);xr_compile_resources_release(middle.resources);
    memset(g.bytes,0xa5,g.length);xr_xir_compile_checked_packet_free(&g);
    XrXirCompileContext receivers[4];XrXirLibraryCatalog *catalogs[4];
    for(unsigned i=0;i<4;++i){receivers[i]=library_context();catalogs[i]=library_catalog(&receivers[i],&derived,bindings,2);}
    memset(bindings,0xa5,sizeof(bindings));
    memset(derived.bytes,0xa5,derived.length);xr_xir_compile_checked_packet_free(&derived);xr_compile_resources_release(transport.resources);
    const char *consumers[]={downstream_source,downstream_local_source,
        "import {Future} from \"./derived_library\"\nexport fn main()->i64{const v=Future(1,2);return 0}\n",
        "import {Future} from \"./derived_library\"\nexport fn main()->i64{return Future.seed().sum}\n"};
    for(unsigned i=0;i<4;++i) {
        char text[8192],logical[64];n=snprintf(text,sizeof(text),"%s",consumers[i]);CHECK(n>0 && n<(int)sizeof(text));
        n=snprintf(logical,sizeof(logical),"downstream-%u.xr",i);CHECK(n>0 && n<(int)sizeof(logical));
        XrXirStatus status=library_source(&receivers[i],catalogs[i],logical,text,XR_XIR_PROGRAM,&source);memset(text,0xa5,sizeof(text));
        CHECK(status==(i<2?XR_XIR_OK:XR_XIR_BAD_TYPE));xr_xir_compile_library_catalog_free(catalogs[i]);
        if(i<2) {
            const XrXirDerivationTable *copied=xr_xir_compile_artifact_module(source.checked)->declarations->derivations;
            CHECK(copied && copied->policy_count==(i?3u:2u) && copied->count==(i?3u:2u) &&
                !copied->policies[0].member_count && copied->policies[1].member_count==1 &&
                copied->policies[1].members[0].operations==(XR_XIR_DERIVE_DESCRIBE|XR_XIR_DERIVE_READ));
            XrXirGeneratedText field=copied->records[1].origins.instructions[0].field_path;
            CHECK(field.length==10 && !memcmp(field.bytes,"Future.sum",10));
        }
        XrXirValue result={0};if(i<2)result=library_execute(&receivers[i],&source);else xr_xir_compile_source_result_free(&source);
        XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(receivers[i].resources,&spent)==XR_COMPILE_RESOURCE_OK);
        fprintf(stderr,"Derived library receiver%u complete allocated=%llu peak=%llu work=%llu\n",i,
            (unsigned long long)spent.allocated_bytes,(unsigned long long)spent.peak_bytes,(unsigned long long)spent.work);
        xr_compile_resources_release(receivers[i].resources);
        if(i<2) {
            const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&result,&bytes,&length) && length==10 && !memcmp(bytes,"left:right",10));
            xr_xir_value_drop(&result);
        }
    }
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Cold derived library: dead producers and packets, imported plus local actual derivation/comptime42, private init/field refusal, last String owner and physical0");return 0;
}
