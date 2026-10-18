/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xir/xxir_source.h"
#include "xir/xxir_source_query_internal.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_sdk_resource_test.h"

static XrXirSourceSnapshot *display_producer(const XrXirCompileContext *context,const char *program) {
    size_t length=strlen(program);char *text=xr_malloc(length+1);CHECK(text);memcpy(text,program,length+1);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"source-display-observer",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceText input={NULL,text,length};XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Display producer: %u %s\n%s",status,diagnostic.message,program);
    CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
    XrXirSourceSnapshot *snapshot=result.snapshot;result.snapshot=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);memset(text,'?',length);xr_free(text);
    return snapshot;
}
static uint32_t display_find(const XrXirSourceView *view,const char *name) {
    uint32_t found=0;
    for(uint32_t d=0;d<view->declaration_count;++d)if(!strcmp(view->declarations[d].name,name)) {
        CHECK(!found);found=d+1;
    }
    CHECK(found);return found;
}
static void display_names(const XrXirSourceSnapshot *snapshot,const char *name,const char *first,const char *second) {
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(snapshot);
    const XrXirSourceSyntaxView *syntax=xr_xir_compile_source_snapshot_syntax(snapshot);CHECK(view&&syntax);
    const XrXirSourceDeclarationSyntax *fact=&syntax->declarations[display_find(view,name)-1];
    CHECK(fact->type_parameter_count==(second?2u:1u)&&fact->type_parameters);
    CHECK(!strcmp(fact->type_parameters[0],first));if(second)CHECK(!strcmp(fact->type_parameters[1],second));
}
static void display_bindings(const XrXirSourceSnapshot *snapshot) {
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(snapshot);
    const XrXirSourceSyntaxView *syntax=xr_xir_compile_source_snapshot_syntax(snapshot);CHECK(view&&syntax);
    const char *inferred[]={"inferred","mutableInferred","globalInferred"};
    const char *explicit_names[]={"explicit","mutableExplicit","globalExplicit","uninitialized"};
    for(unsigned i=0;i<3;++i) {
        uint32_t id=display_find(view,inferred[i]);
        CHECK(view->declarations[id-1].kind==XR_XIR_SOURCE_BINDING&&view->declarations[id-1].type.known);
        CHECK(syntax->declarations[id-1].flags&XR_XIR_SOURCE_SYNTAX_INFERRED_BINDING);
    }
    for(unsigned i=0;i<4;++i) {
        uint32_t id=display_find(view,explicit_names[i]);
        CHECK(!(syntax->declarations[id-1].flags&XR_XIR_SOURCE_SYNTAX_INFERRED_BINDING));
    }
    uint32_t wrapper=display_find(view,"wrapper"),inferred_id=display_find(view,"inferred");
    CHECK(view->declarations[inferred_id-1].type.generic_owner==wrapper);
    display_names(snapshot,"wrapper","ActualElement",NULL);
}
static XrXirSourceSnapshot *display_receiver(const XrXirCompileContext *context,const XrXirSourceSnapshot *producer) {
    XrXirSourceSnapshot *snapshot=NULL;
    CHECK(xr_xir_compile_source_snapshot_copy_v2(context,xr_xir_compile_source_snapshot_view(producer),
        xr_xir_compile_source_snapshot_construction(producer),&snapshot)==XR_XIR_OK);
    return snapshot;
}
static void display_invalid(const XrXirCompileContext *context,const XrXirSourceSnapshot *producer) {
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(producer);
    XrXirSourceSyntaxView syntax=*xr_xir_compile_source_snapshot_syntax(producer);
    size_t bytes=syntax.declaration_count*sizeof(*syntax.declarations);
    XrXirSourceDeclarationSyntax *facts=xr_malloc(bytes);CHECK(facts);syntax.declarations=facts;
    uint32_t wrapper=display_find(view,"wrapper"),binding=display_find(view,"inferred");
    const char *empty[]={""},*missing[]={NULL},*extra[]={"ActualElement","Extra"};
    for(unsigned mode=1;mode<=10;++mode) {
        memcpy(facts,xr_xir_compile_source_snapshot_syntax(producer)->declarations,bytes);
        XrXirSourceDeclarationSyntax *fact=&facts[wrapper-1];
        switch(mode) {
        case 1:fact->type_parameters=NULL;break;
        case 2:fact->type_parameter_count=0;break;
        case 3:fact->type_parameter_count=65537;break;
        case 4:fact->type_parameters=empty;break;
        case 5:fact->type_parameters=missing;break;
        case 6:fact->type_parameters=extra;fact->type_parameter_count=2;break;
        case 7:facts[binding-1].type_parameters=fact->type_parameters;facts[binding-1].type_parameter_count=1;break;
        case 8:fact->flags|=XR_XIR_SOURCE_SYNTAX_INFERRED_BINDING;break;
        case 9:facts[binding-1].flags|=XR_XIR_SOURCE_SYNTAX_STATIC;break;
        case 10:facts[binding-1].flags|=4;break;
        }
        XrXirSourceSnapshot *snapshot=display_receiver(context,producer);
        size_t blocks=runtime_live,live_bytes=runtime_bytes;uint64_t live=sdk_stats(context->resources).live_bytes;
        CHECK(xr_xir_compile_source_snapshot_syntax_copy(snapshot,&syntax)==XR_XIR_BAD_STRUCTURE);
        CHECK(!xr_xir_compile_source_snapshot_syntax(snapshot));
        CHECK(runtime_live==blocks&&runtime_bytes==live_bytes&&sdk_stats(context->resources).live_bytes==live);
        CHECK(xr_xir_compile_source_snapshot_syntax_copy(snapshot,xr_xir_compile_source_snapshot_syntax(producer))==XR_XIR_OK);
        display_bindings(snapshot);xr_xir_compile_source_snapshot_free(snapshot);
    }
    memset(facts,0xa5,bytes);xr_free(facts);
}
static size_t display_attachment(const XrXirCompileContext *context,const XrXirSourceSnapshot *producer,bool failures) {
    size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        if(!failures&&probe)break;
        XrXirSourceSnapshot *snapshot=display_receiver(context,producer);
        size_t blocks=runtime_live,bytes=runtime_bytes,start=runtime_attempts;uint64_t live=sdk_stats(context->resources).live_bytes;
        runtime_fail_at=probe?start+probe-1:SIZE_MAX;
        XrXirStatus status=xr_xir_compile_source_snapshot_syntax_copy(snapshot,xr_xir_compile_source_snapshot_syntax(producer));
        runtime_fail_at=SIZE_MAX;
        if(!probe){CHECK(status==XR_XIR_OK);sites=runtime_attempts-start;CHECK(sites);}
        else {
            CHECK(status==XR_XIR_OUT_OF_MEMORY&&!xr_xir_compile_source_snapshot_syntax(snapshot));
            CHECK(runtime_live==blocks&&runtime_bytes==bytes&&sdk_stats(context->resources).live_bytes==live);
            CHECK(xr_xir_compile_source_snapshot_syntax_copy(snapshot,xr_xir_compile_source_snapshot_syntax(producer))==XR_XIR_OK);
        }
        display_bindings(snapshot);xr_xir_compile_source_snapshot_free(snapshot);
    }
    return sites;
}
static void display_nominals(const XrXirCompileContext *context) {
    const char *programs[]={
        "struct Box<Enclosing> {const value:Enclosing pick<Own>(other:Own)->Own{return other}}\n",
        "interface Reader<Enclosing> {read<Own>(value:Own)->Own}\n",
        "enum Choice<Payload> {Empty,Some{value:Payload}}\n",
        "final class ClassBox<Content> {const value:Content}\n",
        "fn compare<RealItem:Equal>(value:RealItem){assertEqual(value,value)}\n",
        "fn outer<ClosureItem>(value:ClosureItem)->ClosureItem{\n const nested=fn(item:ClosureItem)->ClosureItem{return item}\n return nested(value)\n}\n"
    };
    const char *owners[]={"Box","Reader","Choice","ClassBox","compare","outer"};
    const char *names[]={"Enclosing","Enclosing","Payload","Content","RealItem","ClosureItem"};
    for(unsigned p=0;p<6;++p) {
        XrXirSourceSnapshot *snapshot=display_producer(context,programs[p]);display_names(snapshot,owners[p],names[p],NULL);
        if(p==0)display_names(snapshot,"pick","Enclosing","Own");
        if(p==1)display_names(snapshot,"read","Enclosing","Own");
        if(p==5) {
            const XrXirSourceSyntaxView *syntax=xr_xir_compile_source_snapshot_syntax(snapshot);unsigned closures=0;
            for(uint32_t d=0;d<syntax->declaration_count;++d)if(syntax->declarations[d].role==XR_XIR_SOURCE_SYNTAX_CLOSURE) {
                CHECK(syntax->declarations[d].type_parameter_count==1);
                CHECK(!strcmp(syntax->declarations[d].type_parameters[0],"ClosureItem"));++closures;
            }
            CHECK(closures==1);
        }
        xr_xir_compile_source_snapshot_free(snapshot);
    }
}
static void display_field_defaults(const XrXirCompileContext *context) {
    XrXirSourceSnapshot *snapshot=display_producer(context,
        "struct FieldBox<Element> {const value:Element const structLabel:string=\"field\"}\n"
        "final class FieldPair<Left,Right> {const first:Left const second:Right const classLabel:i64=7}\n");
    display_names(snapshot,"FieldBox","Element",NULL);display_names(snapshot,"FieldPair","Left","Right");
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(snapshot);
    const XrXirSourceSyntaxView *syntax=xr_xir_compile_source_snapshot_syntax(snapshot);
    const char *fields[]={"structLabel","classLabel"};
    const char *owners[]={"FieldBox","FieldPair"};
    for(unsigned f=0;f<2;++f) {
        uint32_t id=display_find(view,fields[f]),owner=display_find(view,owners[f]);
        const XrXirSourceDeclaration *field=&view->declarations[id-1];
        CHECK(field->kind==XR_XIR_SOURCE_MEMBER&&field->parent==owner&&!field->generic_parameter_count);
        CHECK(!syntax->declarations[id-1].type_parameters&&!syntax->declarations[id-1].type_parameter_count);
    }
    xr_xir_compile_source_snapshot_free(snapshot);
}
int main(int argc,char **argv) {
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--resources")));bool failures=argc==2;
    XrCompileResources *resources=sdk_ledger(&sdk_unlimited);XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    display_nominals(&context);display_field_defaults(&context);
    XrXirSourceSnapshot *producer=display_producer(&context,
        "const globalInferred=1\nconst globalExplicit:i64=2\n"
        "fn wrapper<ActualElement>(value:ActualElement)->ActualElement{\n"
        " const inferred=value\n const explicit:ActualElement=value\n var mutableInferred=1\n"
        " var mutableExplicit:i64=2\n var uninitialized:i64\n return inferred\n}\n");
    display_bindings(producer);display_invalid(&context,producer);size_t sites=display_attachment(&context,producer,failures);
    XrCompileResources *receiving=sdk_ledger(&sdk_unlimited);
    XrXirCompileContext receiving_context={receiving,xr_xir_compile_default_limits()};
    XrXirSourceSnapshot *owned=display_receiver(&receiving_context,producer);
    CHECK(xr_xir_compile_source_snapshot_syntax_copy(owned,xr_xir_compile_source_snapshot_syntax(producer))==XR_XIR_OK);
    xr_xir_compile_source_snapshot_free(producer);xr_compile_resources_release(resources);
    xr_compile_resources_release(receiving);display_bindings(owned);xr_xir_compile_source_snapshot_free(owned);
    CHECK(!runtime_live&&!runtime_bytes);
    printf("Display facts invalid=10 sites=%zu full_FI=%u producer_dead=1 physical0/0\n",sites,failures?1u:0u);return 0;
}
