/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xir/xxir_source.h"
#include "xir/xxir_source_query_internal.h"
#include "xir/xxir_source_calls.h"
#include "xir/xxir_construction.h"
#include "toolchain/xcompiler_session.h"
#include "shared/xr_core_intrinsic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_sdk_resource_test.h"

static XrXirSourceSnapshot *core_producer(const XrXirCompileContext *context,const char *program) {
    size_t length=strlen(program);char *text=xr_malloc(length+1);CHECK(text);memcpy(text,program,length+1);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"core-call-observer",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceText input={NULL,text,length};XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Core producer: %u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
    XrXirSourceSnapshot *snapshot=result.snapshot;result.snapshot=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);memset(text,'?',length);xr_free(text);
    return snapshot;
}
static uint32_t core_equal_target(const XrXirSourceSnapshot *snapshot) {
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(snapshot);
    const XrXirSourceCalls *calls=xr_xir_compile_source_snapshot_calls(snapshot);CHECK(view&&calls);
    uint32_t found=0;
    for(uint32_t i=0;i<calls->count;++i) {
        const XrXirSourceCall *call=&calls->items[i];
        if(call->reference==UINT32_MAX)continue;
        CHECK(call->reference<view->reference_count);uint32_t target=view->references[call->reference].target;
        CHECK(target&&target<=view->declaration_count);
        const XrXirSourceDeclaration *d=&view->declarations[target-1];
        if(d->native_identity!=XR_CORE_BUILTIN_ASSERT_EQUAL||d->kind!=XR_XIR_SOURCE_INTRINSIC)continue;
        CHECK(call->signature.known&&call->signature.parameter_count==3&&call->signature.result.known);
        CHECK(call->signature.result.type==XR_XIR_UNIT&&!call->signature.parameter_flags&&!call->signature.owner_type.known);
        CHECK(call->signature.parameters[0].known&&call->signature.parameters[0].type==XR_XIR_I64);
        CHECK(call->signature.parameters[1].known&&call->signature.parameters[1].type==XR_XIR_I64);
        CHECK(call->signature.parameters[2].known&&call->signature.parameters[2].type==XR_XIR_STRING);
        CHECK(!found);found=target;
    }
    CHECK(found);return found;
}
static XrXirSourceSnapshot *core_receiver(const XrXirCompileContext *context,const XrXirSourceSnapshot *producer,unsigned mode) {
    const XrXirSourceView *original=xr_xir_compile_source_snapshot_view(producer);XrXirSourceView view=*original;
    XrXirSourceDeclaration *declarations=xr_malloc(view.declaration_count*sizeof(*declarations));CHECK(declarations);
    memcpy(declarations,view.declarations,view.declaration_count*sizeof(*declarations));view.declarations=declarations;
    XrXirSourceQueryModule *modules=xr_malloc(view.module_count*sizeof(*modules));CHECK(modules);
    memcpy(modules,view.modules,view.module_count*sizeof(*modules));view.modules=modules;
    uint32_t target=core_equal_target(producer);XrXirSourceDeclaration *callee=&declarations[target-1];
    XrXirSourceQueryModule *module=&modules[callee->range.module];
    switch(mode) {
    case 1:callee->native_identity=XR_CORE_BUILTIN_ASSERT_THROWS;break;
    case 2:callee->name="assert";break;
    case 3:module->identity="memory-module-v1:id=4:fake";break;
    case 4:module->path="fake.xr";break;
    case 5:module->fingerprint.bytes[0]^=1;break;
    case 6:callee->kind=XR_XIR_SOURCE_FUNCTION;break;
    case 7:callee->native_identity=0;break;
    case 8:callee->parameter_count=2;break;
    }
    XrXirSourceSnapshot *snapshot=NULL;
    CHECK(xr_xir_compile_source_snapshot_copy_v2(context,&view,xr_xir_compile_source_snapshot_construction(producer),&snapshot)==XR_XIR_OK);
    CHECK(xr_xir_compile_source_snapshot_syntax_copy(snapshot,xr_xir_compile_source_snapshot_syntax(producer))==XR_XIR_OK);
    memset(declarations,0xa5,view.declaration_count*sizeof(*declarations));memset(modules,0xa5,view.module_count*sizeof(*modules));
    xr_free(declarations);xr_free(modules);return snapshot;
}
static void core_invalid(const XrXirCompileContext *context,const XrXirSourceSnapshot *producer) {
    for(unsigned mode=1;mode<=8;++mode) {
        XrXirSourceSnapshot *snapshot=core_receiver(context,producer,mode);
        size_t blocks=runtime_live,bytes=runtime_bytes;uint64_t live=sdk_stats(context->resources).live_bytes;
        CHECK(xr_xir_compile_source_snapshot_calls_copy(snapshot,xr_xir_compile_source_snapshot_calls(producer))==XR_XIR_BAD_STRUCTURE);
        CHECK(!xr_xir_compile_source_snapshot_calls(snapshot));
        CHECK(runtime_live==blocks&&runtime_bytes==bytes&&sdk_stats(context->resources).live_bytes==live);
        xr_xir_compile_source_snapshot_free(snapshot);
    }
}
static size_t core_attachment(const XrXirCompileContext *context,const XrXirSourceSnapshot *producer,bool failures) {
    size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        if(!failures&&probe)break;
        XrXirSourceSnapshot *snapshot=core_receiver(context,producer,0);
        size_t blocks=runtime_live,bytes=runtime_bytes,start=runtime_attempts;uint64_t live=sdk_stats(context->resources).live_bytes;
        runtime_fail_at=probe?start+probe-1:SIZE_MAX;
        XrXirStatus status=xr_xir_compile_source_snapshot_calls_copy(snapshot,xr_xir_compile_source_snapshot_calls(producer));
        runtime_fail_at=SIZE_MAX;
        if(!probe){CHECK(status==XR_XIR_OK);sites=runtime_attempts-start;CHECK(sites);}
        else {
            CHECK(status==XR_XIR_OUT_OF_MEMORY&&!xr_xir_compile_source_snapshot_calls(snapshot));
            CHECK(runtime_live==blocks&&runtime_bytes==bytes&&sdk_stats(context->resources).live_bytes==live);
            CHECK(xr_xir_compile_source_snapshot_calls_copy(snapshot,xr_xir_compile_source_snapshot_calls(producer))==XR_XIR_OK);
        }
        CHECK(core_equal_target(snapshot));xr_xir_compile_source_snapshot_free(snapshot);
    }
    return sites;
}
static void core_programs(const XrXirCompileContext *context) {
    const char *programs[]={
        "fn main(){assert(true)}\n",
        "fn main(){assertEqual(1,1,\"ok\")}\n",
        "fn compare<Item:Equal>(value:Item){assertEqual(value,value)}\nfn main(){compare(1)}\n",
        "fn main(){assertPanics(fn(){assert(false)})}\n"
    };
    for(unsigned p=0;p<4;++p) {
        XrXirSourceSnapshot *snapshot=core_producer(context,programs[p]);
        const XrXirSourceCalls *calls=xr_xir_compile_source_snapshot_calls(snapshot);
        const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(snapshot);CHECK(calls&&view);
        unsigned observed=0;
        for(uint32_t i=0;i<calls->count;++i) {
            const XrXirSourceCall *call=&calls->items[i];if(call->reference==UINT32_MAX)continue;
            uint32_t target=view->references[call->reference].target;CHECK(target&&target<=view->declaration_count);
            if(view->declarations[target-1].kind!=XR_XIR_SOURCE_INTRINSIC)continue;
            CHECK(call->signature.known&&call->signature.result.known&&call->signature.result.type==XR_XIR_UNIT);
            CHECK(!call->signature.owner_type.known&&!call->signature.parameter_flags);++observed;
            if(p==2) {
                CHECK(call->signature.type_parameter_count==1&&call->signature.type_parameter_owner==call->caller);
                CHECK(!strcmp(call->signature.type_parameters[0],"Item"));
                CHECK(call->signature.parameters[0].generic_owner==call->caller&&call->signature.parameters[1].generic_owner==call->caller);
            }
        }
        CHECK(observed==(p==3?2u:1u));xr_xir_compile_source_snapshot_free(snapshot);
    }
}
int main(int argc,char **argv) {
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--resources")));bool failures=argc==2;
    XrCompileResources *resources=sdk_ledger(&sdk_unlimited);XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    core_programs(&context);
    XrXirSourceSnapshot *producer=core_producer(&context,"@test\nfn test_add(){assertEqual(1+1,2)}\n");
    core_invalid(&context,producer);size_t sites=core_attachment(&context,producer,failures);
    XrCompileResources *receiving=sdk_ledger(&sdk_unlimited);
    XrXirCompileContext receiving_context={receiving,xr_xir_compile_default_limits()};
    XrXirSourceSnapshot *owned=core_receiver(&receiving_context,producer,0);
    CHECK(xr_xir_compile_source_snapshot_calls_copy(owned,xr_xir_compile_source_snapshot_calls(producer))==XR_XIR_OK);
    xr_xir_compile_source_snapshot_free(producer);xr_compile_resources_release(resources);
    xr_compile_resources_release(receiving);CHECK(core_equal_target(owned));
    xr_xir_compile_source_snapshot_free(owned);CHECK(!runtime_live&&!runtime_bytes);
    printf("Core call observations invalid=8 sites=%zu full_FI=%u producer_dead=1 physical0/0\n",sites,failures?1u:0u);return 0;
}
