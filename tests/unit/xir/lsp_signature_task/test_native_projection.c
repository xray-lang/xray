/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "app/lsp/xlsp_source_workspace.h"
#include "app/lsp/xlsp_source_signature.h"
#include "xir/xxir_source_calls.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_sdk_resource_test.h"
#include "native_cases.h"
#include "app/lsp/xlsp_source_workspace.c"

static void same_node(const XrXirTypeNode *a,const XrXirTypeNode *b) {
    CHECK(a->kind==b->kind&&a->element==b->element&&a->result==b->result);
    CHECK(a->flags==b->flags&&a->parameter_span==b->parameter_span);
    CHECK(a->parameter_count==b->parameter_count);
    for(uint32_t p=0;p<a->parameter_count;++p)
        CHECK(a->parameters[p].type==b->parameters[p].type&&a->parameters[p].mode==b->parameters[p].mode);
    CHECK(a->nominal.declaration==b->nominal.declaration);
    CHECK(a->nominal.argument_count==b->nominal.argument_count&&a->nominal.field_count==b->nominal.field_count);
    for(uint32_t p=0;p<a->nominal.argument_count;++p)CHECK(a->nominal.arguments[p]==b->nominal.arguments[p]);
    for(uint32_t p=0;p<a->nominal.field_count;++p)CHECK(a->nominal.fields[p]==b->nominal.fields[p]);
}
static bool open_array(const XrXirTypeNode *node) {
    return node->kind==XR_XIR_TYPE_ARRAY&&node->element==(XrXirType)XR_XIR_TYPE_PARAMETER_BASE&&node->parameter_span==1;
}
static void projection(XlspSourceWorkspace *w,const XlspSourceDocument *documents,bool generic) {
    XrModuleOverlayInput inputs[2]={0};char *paths[2]={0},*identities[2]={0},*logical[2]={0};
    for(size_t i=0;i<2;++i) {
        CHECK(xlsp_source_uri_path(w->context.resources,documents[i].uri,documents[i].uri_length,&paths[i])==XR_XIR_OK);
        inputs[i].authority=*xr_source_authority_entry(w->authorities[i]);
        CHECK(xr_compile_module_identity_from_source(w->context.resources,&inputs[i].authority,paths[i],
            &identities[i],&logical[i])==XR_MODULE_OK);
        inputs[i].logical_path=logical[i];inputs[i].source_path=paths[i];
        inputs[i].text=documents[i].text;inputs[i].length=documents[i].length;
    }
    XrXirSourceRequest request={w->session,paths[0],&inputs[0].authority,&w->context,w->stdlib.path,
        xr_source_authority_lockfile(w->authorities[0]),XR_XIR_PROGRAM,w->catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};XrXirSourceFailure failure_path={0};
    XrXirStatus status=xr_xir_compile_source_check_overlay(&request,inputs,2,&result,&diagnostic,&failure_path);
    if(status!=XR_XIR_OK)fprintf(stderr,"projection: %u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot&&!failure_path.path);
    const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);
    CHECK(module&&module->types&&view&&view->complete&&view->types);
    CHECK(view->types->count==module->types->count+(generic?0u:1u));
    uint32_t open=0;
    for(uint32_t i=0;i<module->types->count;++i) {
        same_node(&module->types->nodes[i],&view->types->nodes[i]);
        if(open_array(&module->types->nodes[i]))++open;
        if(!generic)CHECK(!module->types->nodes[i].parameter_span);
    }
    CHECK(open==(generic?1u:0u));
    if(!generic)CHECK(open_array(&view->types->nodes[module->types->count]));
    uint32_t schemas=0;
    for(uint32_t i=0;i<view->declaration_count;++i) {
        const XrXirSourceDeclaration *d=&view->declarations[i];
        if(d->kind!=XR_XIR_SOURCE_MEMBER||!d->native_identity)continue;
        if(d->type.known&&d->type.generic_owner) {
            const XrXirTypeNode *node=xr_xir_type_node(view->types,d->type.type);
            if(node&&open_array(node))++schemas;
        }
        for(uint32_t p=0;p<d->parameter_count;++p)if(d->parameters[p].known&&d->parameters[p].generic_owner) {
            const XrXirTypeNode *node=xr_xir_type_node(view->types,d->parameters[p].type);
            if(node&&open_array(node))++schemas;
        }
    }
    CHECK(schemas);
    xr_xir_compile_artifact_free(result.checked);result.checked=NULL;
    CHECK(xr_xir_compile_source_snapshot_view(result.snapshot)->types->count==view->types->count);
    xr_xir_compile_source_result_free(&result);
    for(size_t i=0;i<2;++i) {
        xr_compile_resources_free(paths[i]);xr_compile_resources_free(identities[i]);xr_compile_resources_free(logical[i]);
    }
}
static void target_identity(XlspSourceWorkspace *w,const XlspSourceDocument *documents,unsigned scenario,
    unsigned row,unsigned column) {
    XlspSyntaxSnapshot *syntax=NULL;
    CHECK(xlsp_source_syntax_build(w->session,&documents[0],&syntax)==XR_PARSE_OK);
    XlspSourceSignatureSelection selection={.resources=w->context.resources,.uri=documents[0].uri,.uri_length=documents[0].uri_length};
    CHECK(xlsp_source_syntax_call(syntax,(XrLspPosition){row,column},&selection.call)==XR_XIR_OK);
    XlspSourceSignature signatures[2]={0};
    for(size_t i=0;i<2;++i) {
        bool contains=false;
        CHECK(xlsp_source_snapshot_contains(w->snapshots[i],documents[0].uri,documents[0].uri_length,&contains)==XR_XIR_OK&&contains);
        CHECK(xlsp_source_signature(w->snapshots[i],&selection,&signatures[i])==XR_XIR_OK);
        CHECK(signatures[i].target_native_identity==(scenario==59?4u:0u));
        CHECK(signatures[i].target_identity.length&&signatures[i].target_path.length);
        CHECK(!strcmp(signatures[i].label.text,"fn Array<i64>.get(index: i64) -> i64"));
        XlspSourceLocation location={0};
        XrXirStatus located=xlsp_source_definition(w->snapshots[i],documents[0].uri,documents[0].uri_length,
            selection.call.callee.start,&location);
        if(scenario==60)CHECK(located==XR_XIR_OK&&location.version==1&&!strcmp(location.uri,documents[0].uri));
        else CHECK(located==XR_XIR_UNRESOLVED);
    }
    CHECK(!strcmp(signatures[0].target_identity.text,signatures[1].target_identity.text));
    CHECK(!strcmp(signatures[0].target_path.text,signatures[1].target_path.text));
    CHECK(!memcmp(&signatures[0].target_fingerprint,&signatures[1].target_fingerprint,sizeof(XrFingerprint)));
    CHECK(!memcmp(&signatures[0].target_range,&signatures[1].target_range,sizeof(XrXirSourceRange)));
    for(size_t i=0;i<2;++i)xlsp_source_signature_free(&signatures[i]);
    xlsp_source_syntax_free(syntax);
}
static void run(const char *uri,const char *library,unsigned scenario) {
    const char *program=NULL;unsigned row=0,column=0;
    bool generic=scenario==52;
    CHECK(signature_native_fixture(scenario,&program,&row,&column));
    if(generic)program="fn wrapper<Item>(value:Item)->Item {\n var values=[value]\n const reserved=values.reserve(2)\n return values.get(0)\n}\nfn main()->i64{return wrapper(1)}\n";
    const char *dependency=scenario>=59?"import { compute } from \"./root\"\nexport fn mix()->i64{return compute()}\n":
        "export fn mix()->i64{return 1}\n";
    size_t length=strlen(program);char *text=xr_malloc(length+1);CHECK(text);memcpy(text,program,length+1);
    XlspSourceDocument documents[]={{uri,text,strlen(uri),length,1},{library,dependency,strlen(library),strlen(dependency),7}};
    XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    CHECK(limits.work<sdk_unlimited.work);XrCompileResources *r=sdk_ledger(&limits);uint64_t baseline=sdk_stats(r).live_bytes;
    XrXirCompileContext context={r,xr_xir_compile_default_limits()};XlspSourceWorkspace *w=NULL;
    unsigned stage=0;XlspSourceWorkspaceFailure failure={0};
    XrXirStatus status=xlsp_source_workspace_build(&context,documents,2,&w,&stage,&failure);
    if(status!=XR_XIR_OK)fprintf(stderr,"workspace scenario=%u stage=%u status=%u %s\n",scenario,stage,status,failure.diagnostic.message);
    CHECK(status==XR_XIR_OK);
    if(scenario>=59)target_identity(w,documents,scenario,row,column);
    else projection(w,documents,generic);
    memset(text,'?',length);xr_free(text);
    xlsp_source_workspace_free(w);xlsp_source_workspace_failure_free(&failure);CHECK(sdk_stats(r).live_bytes==baseline);
    xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);
}
int main(int argc,char **argv) {
    CHECK(argc==3);
    const unsigned scenarios[]={27,28,31,33,37,38,43,52,59,60};
    for(size_t i=0;i<sizeof(scenarios)/sizeof(scenarios[0]);++i)run(argv[1],argv[2],scenarios[i]);
    puts("native projection monomorphic=7 reused_generic=1 cross_root=2 producer_dead=1 physical0/0");return 0;
}
