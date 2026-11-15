/* Real Hashable declaration owners survive producer retirement and cold reads.
 * This gate grants static qualification, never a runtime guest-key carrier. */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_hashable_internal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_library_catalog.h"
#include "toolchain/xcompiler_session.h"
#include "base/xsha256.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "../xir_source_compile_owner.h"
static const char producer[]=
    "export struct Token<T:Hashable> implements Hashable {\n"
    "value:T\nequals(other:Token<T>)->bool{return true}\nhash()->i64{return 0}\n}\n"
    "export fn keyed<K:Hashable>(value:K)->K{return value}\n";
static XrXirStatus source(const XrXirCompileContext *compile,const char *logical,
    const char *text,XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog,
    XrCompileRequestControl *control,XrXirSourceResult *result) {
    XrCompilerSession *session=NULL;
    XrCompileResourceStatus made=xr_compile_session_new(compile->resources,&session);
    if(made==XR_COMPILE_RESOURCE_OK && control)made=xr_compile_session_bind_request(session,control);
    if(made!=XR_COMPILE_RESOURCE_OK){xr_compile_session_free(session);return xir_compile_resource_status(made);}
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,HASHABLE_LIBRARY_ROOT};
    char path[1024];int n=snprintf(path,sizeof(path),"%s/%s",HASHABLE_LIBRARY_ROOT,logical);
    CHECK(n>0 && (size_t)n<sizeof(path));
    XrXirSourceRequest request={session,path,&authority,compile,NULL,NULL,kind,catalog};
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){logical,text,strlen(text)},result,&diagnostic,NULL);
    if(status!=XR_XIR_OK && !control && source_fixture_compile_fail_at==SIZE_MAX)
        fprintf(stderr,"Hashable source %s status=%u %s\n",logical,(unsigned)status,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static void facts(const XrXirCompileContext *compile,const XrXirArtifact *artifact,bool closed) {
    const XrXirModule *m=xr_xir_compile_artifact_module(artifact);
    CHECK(m && m->declarations && m->declarations->hashable_witness_count);
    XrXirDiagnostic diagnostic={0};XrXirStatus verified=xr_xir_compile_artifact_verify(artifact,&diagnostic);
    if(verified!=XR_XIR_OK)fprintf(stderr,"Hashable artifact verify closed=%u status=%u f=%u block=%u instruction=%u\n",
        closed?1u:0u,(unsigned)verified,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(verified==XR_XIR_OK);
    CHECK(xir_hashable_witnesses_verify(compile,m,NULL)==XR_XIR_OK);
    for(uint32_t i=0;i<m->declarations->hashable_witness_count;++i) {
        XrXirHashableWitness row=m->declarations->hashable_witnesses[i];
        const XrXirTypeNode *node=xr_xir_type_node(m->types,row.nominal_type);
        CHECK(node && node->kind==XR_XIR_TYPE_NOMINAL);
        if(closed)CHECK(!node->parameter_span && !m->generics);
        XirMetadataDomain domain=xir_metadata_domain_from_module(m);
        XirMetadataProofContext proof={&domain,{XR_XIR_CONTEXT_NOMINAL,node->nominal.declaration,0}};
        XirHashableTargets targets={0};
        CHECK(xir_hashable_witness_targets(compile,&proof,row.nominal_type,NULL,NULL,&targets)==XR_XIR_OK);
        CHECK(targets.functions[0]==row.equals_function && targets.functions[1]==row.hash_function);
        CHECK(targets.argument_count==(node->parameter_span?node->nominal.argument_count:0));
        domain.signature=NULL;
        CHECK(xir_hashable_witness_read(compile,&proof,row.nominal_type,NULL,NULL)==XR_XIR_BAD_TYPE);
    }
}
static void forged(const XrXirCompileContext *compile,const XrXirArtifact *artifact) {
    const XrXirModule *m=xr_xir_compile_artifact_module(artifact);
    CHECK(m->declarations->hashable_witness_count==1);
    XrXirHashableWitness row=m->declarations->hashable_witnesses[0];
    XrXirFunction *functions=malloc((size_t)m->function_count*sizeof(*functions));
    XrXirFunctionIdentity *identities=malloc((size_t)m->function_count*sizeof(*identities));
    CHECK(functions && identities);
    const XrXirTypeNode *node=xr_xir_type_node(m->types,row.nominal_type);
    for(uint32_t bad=0;bad<14;++bad) {
        memcpy(functions,m->functions,(size_t)m->function_count*sizeof(*functions));
        memcpy(identities,m->declarations->functions,(size_t)m->function_count*sizeof(*identities));
        XrXirDeclarations d=*m->declarations;d.functions=identities;d.hashable_witnesses=&row;
        XrXirModule altered=*m;altered.functions=functions;altered.declarations=&d;
        XrXirHashableWitness saved=row;
        XrXirFunction *equals=&functions[row.equals_function],*hash=&functions[row.hash_function];
        XrXirFunctionIdentity *id=&identities[row.equals_function];
        XrXirType parameters[2]={row.nominal_type,XR_XIR_I64};
        if(bad==0)row.equals_function=m->function_count;
        else if(bad==1)id->nominal_owner=0;
        else if(bad==2)id->method_kind=XR_XIR_STATIC_METHOD;
        else if(bad==3)id->member_access=XR_XIR_MEMBER_PRIVATE;
        else if(bad==4){equals->name="equalz";equals->name_length=6;}
        else if(bad==5)equals->parameters=parameters;
        else if(bad==6)equals->result=XR_XIR_I64;
        else if(bad==7)hash->result=XR_XIR_STRING;
        else if(bad==8)equals->error_type=XR_XIR_ERROR;
        else if(bad==9)equals->consume_parameter_count=1;
        else if(bad==10)equals->escaping_parameter_count=1;
        else if(bad==11)id->promises|=XR_XIR_FUNCTION_REST;
        else if(bad==12)id->capture_count=1;
        else {hash->name="hashCode";hash->name_length=8;}
        XirMetadataDomain domain=xir_metadata_domain_from_module(&altered);
        XirMetadataProofContext proof={&domain,{XR_XIR_CONTEXT_NOMINAL,node->nominal.declaration,0}};
        XirHashableTargets untouched={{UINT32_MAX,UINT32_MAX},NULL,999},output=untouched;
        XrXirStatus status=xir_hashable_witness_targets(compile,&proof,saved.nominal_type,NULL,NULL,&output);
        CHECK(status==XR_XIR_BAD_TYPE || status==XR_XIR_BAD_STRUCTURE);
        CHECK(!memcmp(&output,&untouched,sizeof(output)));
        CHECK(xir_hashable_witnesses_verify(compile,&altered,NULL)!=XR_XIR_OK);
        row=saved;
    }
    free(identities);free(functions);
}
static void conditional_refusal(const XrXirCompileContext *compile,const XrXirArtifact *artifact) {
    const XrXirModule *m=xr_xir_compile_artifact_module(artifact);
    XrXirHashableWitness row=m->declarations->hashable_witnesses[0];
    const XrXirTypeNode *node=xr_xir_type_node(m->types,row.nominal_type);
    uint32_t owner=node->nominal.declaration;
    XrXirNominalDeclaration *declarations=malloc((size_t)m->types->nominals->count*sizeof(*declarations));
    CHECK(declarations);memcpy(declarations,m->types->nominals->declarations,
        (size_t)m->types->nominals->count*sizeof(*declarations));
    CHECK(declarations[owner].parameter_count==1);
    XrXirConstraint weaker={0};declarations[owner].constraints=&weaker;
    XrXirNominalTable table=*m->types->nominals;table.declarations=declarations;
    XrXirTypes types=*m->types;types.nominals=&table;
    XrXirModule altered=*m;altered.types=&types;
    XirMetadataDomain domain=xir_metadata_domain_from_module(&altered);
    XirMetadataProofContext proof={&domain,{XR_XIR_CONTEXT_NOMINAL,owner,0}};
    /* All IDs and method signatures remain real. Their stronger Hashable<T>
     * requirements cannot become facts after the nominal bound is removed. */
    CHECK(xir_hashable_witness_read(compile,&proof,row.nominal_type,NULL,NULL)==XR_XIR_OK);
    CHECK(xir_hashable_witnesses_verify(compile,&altered,NULL)==XR_XIR_BAD_TYPE);
    free(declarations);
}
typedef struct Cancel {size_t polls,after;} Cancel;
static bool cancelled(const void *owner) {
    Cancel *cancel=(Cancel *)owner;return ++cancel->polls>=cancel->after;
}
static void cold_failures(const XrXirCompileContext *compile,const XrXirCheckedPacket *packet) {
    size_t live=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    XrXirArtifact *decoded=NULL;
    source_fixture_compile_attempts=0;
    CHECK(xr_xir_compile_checked_read(compile,packet->bytes,packet->length,&decoded,NULL)==XR_XIR_OK);
    size_t sites=source_fixture_compile_attempts;CHECK(sites);facts(compile,decoded,false);
    xr_xir_compile_artifact_free(decoded);decoded=NULL;
    CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    for(size_t p=0;p<sites;++p) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=p;source_fixture_compile_injected=false;
        CHECK(xr_xir_compile_checked_read(compile,packet->bytes,packet->length,&decoded,NULL)==XR_XIR_OUT_OF_MEMORY);
        CHECK(source_fixture_compile_injected && !decoded);
        CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    }
    source_fixture_compile_fail_at=SIZE_MAX;
    Cancel cancellation={0,SIZE_MAX};XrCompileRequestControl control={0};
    XrCompileRequestOptions options={.cancel_poll=cancelled,.cancel_context=&cancellation};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xir_checked_read(compile,packet->bytes,packet->length,&decoded,NULL,&control)==XR_XIR_OK);
    size_t polls=cancellation.polls;xr_xir_compile_artifact_free(decoded);decoded=NULL;CHECK(polls);
    for(size_t p=1;p<=polls;++p) {
        cancellation=(Cancel){0,p};control=(XrCompileRequestControl){0};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        CHECK(xir_checked_read(compile,packet->bytes,packet->length,&decoded,NULL,&control)==XR_XIR_CANCELLED);
        CHECK(!decoded && source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    }
    printf("Hashable cold owner FI=%zu cancel-prefixes=%zu\n",sites,polls);
}
static void clone_failures(const XrXirCompileContext *compile,const XrXirArtifact *artifact) {
    const XrXirModule *m=xr_xir_compile_artifact_module(artifact);
    size_t live=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    XrXirDeclarations *copy=NULL;source_fixture_compile_attempts=0;
    CHECK(xr_xir_compile_declarations_clone(compile,m->declarations,m->function_count,NULL,&copy)==XR_XIR_OK);
    size_t sites=source_fixture_compile_attempts;CHECK(sites);
    CHECK(copy->hashable_witnesses!=m->declarations->hashable_witnesses &&
        copy->hashable_witness_count==m->declarations->hashable_witness_count);
    xr_xir_compile_declarations_free(copy);copy=NULL;
    for(size_t p=0;p<sites;++p) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=p;source_fixture_compile_injected=false;
        CHECK(xr_xir_compile_declarations_clone(compile,m->declarations,m->function_count,NULL,&copy)==XR_XIR_OUT_OF_MEMORY);
        CHECK(source_fixture_compile_injected && !copy && source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    }
    source_fixture_compile_fail_at=SIZE_MAX;
}
static void no_nominal_specialization(const XrXirCompileContext *compile) {
    const char text[]="fn identity(value:Array<i64>)->Array<i64>{return value}\nconst data=identity([1,2])\n";
    XrXirSourceResult result={0};
    CHECK(source(compile,"array-only.xr",text,XR_XIR_PROGRAM,NULL,NULL,&result)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);
    CHECK(module->types && module->types->count && !module->types->nominals);
    XrXirArtifact *closed=NULL;
    CHECK(xr_xir_compile_specialize(result.checked,&closed,NULL,NULL)==XR_XIR_OK && closed);
    CHECK(xr_xir_compile_artifact_verify(closed,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_source_result_free(&result);
}
static void closed_origin_refusal(const XrXirCompileContext *compile,const XrXirArtifact *artifact) {
    const XrXirModule *m=xr_xir_compile_artifact_module(artifact);
    CHECK(m->provenance && m->provenance->origins && m->declarations->hashable_witness_count==2);
    XrXirHashableWitness row=m->declarations->hashable_witnesses[0];
    XrXirOrigin *origins=malloc((size_t)m->function_count*sizeof(*origins));CHECK(origins);
    const XrXirModule *source_module=xr_xir_compile_artifact_module(m->provenance->source);
    CHECK(source_module);
    for(uint32_t bad=0;bad<3;++bad) {
        memcpy(origins,m->provenance->origins,(size_t)m->function_count*sizeof(*origins));
        XrXirProvenance provenance=*m->provenance;provenance.origins=origins;
        XrXirModule altered=*m;altered.provenance=&provenance;
        XrXirType incorrect=XR_XIR_BOOL;
        if(bad==0)origins[row.equals_function].function=source_module->function_count;
        else if(bad==1)origins[row.equals_function].function=origins[row.hash_function].function;
        else {CHECK(origins[row.equals_function].argument_count==1);origins[row.equals_function].arguments=&incorrect;}
        XirMetadataFunctionSignature result={0};
        XrXirStatus status=xir_metadata_module_signature(&altered,compile,row.equals_function,NULL,NULL,&result);
        CHECK(status==XR_XIR_BAD_STRUCTURE || status==XR_XIR_BAD_TYPE);
        CHECK(!result.name && !result.parameters);
        CHECK(xir_hashable_witnesses_verify(compile,&altered,NULL)!=XR_XIR_OK);
    }
    free(origins);
}
#include "hashable_source_failure_cases.inc.c"
#include "hashable_closed_reader_cases.inc.c"
#include "hashable_native_owner_cases.inc.c"
int main(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};XrXirCompileContext compile={0};
    CHECK(xr_compile_resources_new(&limits,&compile.resources)==XR_COMPILE_RESOURCE_OK);
    compile.limits=xr_xir_compile_default_limits();
    native_file_stat_discovery(&compile);
    no_nominal_specialization(&compile);
    source_failures(&compile);fprintf(stderr,"Hashable phase unused origin\n");fflush(stderr);
    unused_origin_argument_refusal(&compile);
    char *text=xr_malloc(sizeof(producer));CHECK(text);memcpy(text,producer,sizeof(producer));
    XrXirSourceResult result={0};
    CHECK(source(&compile,"library.xr",text,XR_XIR_LIBRARY,NULL,NULL,&result)==XR_XIR_OK);
    memset(text,0xa5,sizeof(producer));xr_free(text);
    facts(&compile,result.checked,false);forged(&compile,result.checked);
    conditional_refusal(&compile,result.checked);clone_failures(&compile,result.checked);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);
    cold_failures(&compile,&packet);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,HASHABLE_LIBRARY_ROOT};
    XrXirLibraryModuleInput binding={authority,"library.xr"};
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=&binding,.module_count=1};
    xr_sha256(packet.bytes,packet.length,input.sha256);XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(&compile,&input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    memset(&binding,0xa5,sizeof(binding));memset(&input,0xa5,sizeof(input));
    const char consumer[]="import {Token,keyed} from \"./library\"\n"
        "const first=keyed(Token<i64>{value:7})\nconst second=keyed(Token<string>{value:\"x\"})\n";
    CHECK(source(&compile,"consumer.xr",consumer,XR_XIR_PROGRAM,catalog,NULL,&result)==XR_XIR_OK);
    xr_xir_compile_library_catalog_free(catalog);facts(&compile,result.checked,false);
    XrXirArtifact *closed=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_specialize(result.checked,&closed,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Hashable specialize status=%u f=%u\n",(unsigned)status,diagnostic.function);
    CHECK(status==XR_XIR_OK && closed);xr_xir_compile_source_result_free(&result);
    facts(&compile,closed,true);
    closed_origin_refusal(&compile,closed);
    fprintf(stderr,"Hashable phase closed reader\n");fflush(stderr);closed_reader_failures(&compile,closed);
    closed_module_refusal(&compile,closed);closed_nominal_refusal(&compile,closed);
    CHECK(xr_xir_compile_artifact_module(closed)->declarations->hashable_witness_count==2);
    CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    fprintf(stderr,"Hashable phase closed cold packet\n");fflush(stderr);
    cold_failures(&compile,&packet);xr_xir_compile_checked_packet_free(&packet);
    xr_compile_resources_release(compile.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Hashable actual owners, cold receiving, forgery refusal and closed method identities PASS");return 0;
}
