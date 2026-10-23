/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xir/xxir_source_query_internal.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_types.h"
#include "shared/xnative_declaration.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
typedef struct NativeFixture {
    XrXirSourceQueryModule modules[2];
    XrXirSourceDeclaration declarations[4];
    XrXirSourceDeclarationSyntax names[4];
    XrXirSourceReference reference;
    XrXirSourceRange selection;
    XrXirSourceFunctionRange functions[4];
    XrXirSourceCall call;
    XrXirSourceCalls calls;
    XrXirSourceView view;
    XrXirSourceSyntaxView syntax;
    XrXirTypes types;
    XrXirTypeNode nodes[2];
    XrXirConstraint constraint;
    XrXirSourceType schema;
    XrXirSourceType *parameters;
    uint32_t *flags;
} NativeFixture;
static void native_fixture(NativeFixture *f) {
    memset(f,0,sizeof(*f));
    const XrNativeTypeDeclaration *owner=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ARRAY);
    const XrNativeMemberDeclaration *member=xr_native_declaration_member(owner,"push");CHECK(member);
    f->modules[0]=(XrXirSourceQueryModule){owner->identity,owner->source_path,owner->source_fingerprint};
    f->modules[1]=(XrXirSourceQueryModule){.identity="user",.path="user.xr"};
    for(uint32_t i=0;i<4;++i)f->declarations[i].id=i+1;
    f->declarations[0]=(XrXirSourceDeclaration){.id=1,.kind=XR_XIR_SOURCE_TYPE,.name=owner->name,
        .range={0,(int)owner->line,(int)owner->column,(int)owner->line,(int)(owner->column+strlen(owner->name))},
        .native_identity=owner->id,.generic_parameter_count=1,.generic_constraints=&f->constraint};
    f->schema=(XrXirSourceType){(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,1,true};
    f->declarations[1]=(XrXirSourceDeclaration){.id=2,.parent=1,.kind=XR_XIR_SOURCE_MEMBER,.name=member->name,
        .range={0,(int)member->line,(int)member->column,(int)member->line,(int)(member->column+strlen(member->name))},
        .type={XR_XIR_UNIT,0,true},.parameters=&f->schema,.parameter_count=1,.mutable=true,
        .native_identity=member->id,.signature=member->signature,.generic_parent=1,.generic_parent_count=1};
    f->declarations[2]=(XrXirSourceDeclaration){.id=3,.kind=XR_XIR_SOURCE_FUNCTION,.name="main",.range={1,1,4,1,8},
        .type={XR_XIR_UNIT,0,true}};
    f->declarations[3]=(XrXirSourceDeclaration){.id=4,.parent=2,.kind=XR_XIR_SOURCE_PARAMETER,.name="value",.range={0},.type=f->schema};
    for(uint32_t i=0;i<3;++i)f->names[i]=(XrXirSourceDeclarationSyntax){.name=f->declarations[i].range,.role=XR_XIR_SOURCE_SYNTAX_NAME};
    f->selection=(XrXirSourceRange){1,4,3,4,7};f->reference=(XrXirSourceReference){f->selection,2,2,XR_XIR_SOURCE_CALL};
    f->functions[2]=(XrXirSourceFunctionRange){{1,1,1,8,2},true};
    f->parameters=xr_malloc(sizeof(*f->parameters));f->flags=xr_malloc(sizeof(*f->flags));CHECK(f->parameters&&f->flags);
    *f->parameters=(XrXirSourceType){XR_XIR_I64,0,true};*f->flags=0;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,.parameter_span=1};
    f->types.nodes=f->nodes;f->types.count=2;
    f->call=(XrXirSourceCall){.reference=0,.caller=3,.body=3,.range=f->selection,.selection_known=true,
        .signature={.parameters=f->parameters,.parameter_count=1,.result={XR_XIR_UNIT,0,true},.known=true,
            .owner_type={(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,0,true},.parameter_flags=f->flags}};
    f->calls=(XrXirSourceCalls){&f->call,1,f->functions,4};
    f->view=(XrXirSourceView){.complete=true,.modules=f->modules,.module_count=2,.declarations=f->declarations,.declaration_count=4,
        .references=&f->reference,.reference_count=1,.types=&f->types};
    f->syntax=(XrXirSourceSyntaxView){f->names,4,&f->selection,1,NULL,0};
}
static void native_dispose(NativeFixture *f) {
    memset(f->parameters,0xa5,sizeof(*f->parameters));memset(f->flags,0xa5,sizeof(*f->flags));
    xr_free(f->parameters);xr_free(f->flags);memset(f,0xa5,sizeof(*f));
}
static XrXirSourceSnapshot *native_snapshot(const XrXirCompileContext *context,NativeFixture *f) {
    const XrXirConstructionSpec input={0};
    XrXirConstruction *construction=NULL;CHECK(xr_xir_compile_construction_new_v4(context,NULL,&input,&(XrXirSlotFacts){NULL,0},&construction)==XR_XIR_OK);
    XrXirSourceSnapshot *s=NULL;CHECK(xr_xir_compile_source_snapshot_copy_v2(context,&f->view,construction,&s)==XR_XIR_OK);
    xr_xir_compile_construction_free(construction);CHECK(xr_xir_compile_source_snapshot_syntax_copy(s,&f->syntax)==XR_XIR_OK);return s;
}
static void native_owned(const XrXirSourceSnapshot *s) {
    const XrXirSourceCalls *calls=xr_xir_compile_source_snapshot_calls(s);CHECK(calls&&calls->count==1);
    const XrXirSourceCallSignature *v=&calls->items[0].signature;
    CHECK(v->known&&v->parameter_count==1&&v->parameters[0].known&&v->parameters[0].type==XR_XIR_I64);
    CHECK(v->owner_type.known&&v->owner_type.type==(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE&&v->parameter_flags&&!v->parameter_flags[0]);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(s);
    CHECK(view&&view->declarations[1].native_identity==3&&!strcmp(view->declarations[3].name,"value"));
}
static void native_invalid(void) {
    for(unsigned mode=0;mode<16;++mode) {
        NativeFixture f;native_fixture(&f);XrXirStatus expected=XR_XIR_BAD_STRUCTURE;
        switch(mode) {
        case 0:f.declarations[1].native_identity=91;break;
        case 1:f.declarations[0].native_identity=XR_NATIVE_DECLARATION_STRING;break;
        case 2:f.modules[0].fingerprint.bytes[0]^=1;break;
        case 3:f.modules[0].path="other.xr";break;
        case 4:f.declarations[1].signature="(value: bool)";break;
        case 5:f.declarations[1].name="get";break;
        case 6:f.declarations[1].range.line++;f.declarations[1].range.end_line++;f.names[1].name=f.declarations[1].range;break;
        case 7:f.call.signature.owner_type.type=XR_XIR_I64;expected=XR_XIR_BAD_TYPE;break;
        case 8:*f.flags=4;break;
        case 9:*f.flags=XR_XIR_SOURCE_CALL_OPTIONAL;break;
        case 10:f.call.signature.parameter_flags=NULL;expected=XR_XIR_BAD_TYPE;break;
        case 11:f.call.signature.owner_type=(XrXirSourceType){0};expected=XR_XIR_BAD_TYPE;break;
        case 12:f.call.signature.owner_type.generic_owner=3;expected=XR_XIR_BAD_TYPE;break;
        case 13:f.declarations[0].name="Wrong";break;
        case 14:f.declarations[1].range.end_column++;f.names[1].name=f.declarations[1].range;break;
        case 15:f.declarations[0].range.line++;f.declarations[0].range.end_line++;f.names[0].name=f.declarations[0].range;break;
        }
        XrCompileResources *r=sdk_ledger(&sdk_unlimited);XrXirCompileContext context={r,xr_xir_compile_default_limits()};
        XrXirSourceSnapshot *s=native_snapshot(&context,&f);size_t blocks=runtime_live,bytes=runtime_bytes;
        uint64_t live=sdk_stats(r).live_bytes;
        CHECK(xr_xir_compile_source_snapshot_calls_copy(s,&f.calls)==expected&&!xr_xir_compile_source_snapshot_calls(s));
        CHECK(runtime_live==blocks&&runtime_bytes==bytes&&sdk_stats(r).live_bytes==live);
        native_dispose(&f);xr_xir_compile_source_snapshot_free(s);xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);
    }
}
static size_t native_attachment(bool failures) {
    size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        if(!failures&&probe)break;
        NativeFixture f;native_fixture(&f);XrCompileResources *r=sdk_ledger(&sdk_unlimited);
        XrXirCompileContext context={r,xr_xir_compile_default_limits()};XrXirSourceSnapshot *s=native_snapshot(&context,&f);
        size_t blocks=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;uint64_t live=sdk_stats(r).live_bytes;
        runtime_fail_at=probe?begin+probe-1:SIZE_MAX;XrXirStatus status=xr_xir_compile_source_snapshot_calls_copy(s,&f.calls);
        if(!probe){CHECK(status==XR_XIR_OK);sites=runtime_attempts-begin;CHECK(sites);}
        else {
            CHECK(status==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>runtime_fail_at&&!xr_xir_compile_source_snapshot_calls(s));
            CHECK(runtime_live==blocks&&runtime_bytes==bytes&&sdk_stats(r).live_bytes==live);
            runtime_fail_at=SIZE_MAX;CHECK(xr_xir_compile_source_snapshot_calls_copy(s,&f.calls)==XR_XIR_OK);
        }
        const XrXirSourceCallSignature *v=&xr_xir_compile_source_snapshot_calls(s)->items[0].signature;
        CHECK(v->parameters!=f.parameters&&v->parameter_flags!=f.flags);native_owned(s);
        native_dispose(&f);xr_compile_resources_release(r);native_owned(s);xr_xir_compile_source_snapshot_free(s);
        CHECK(!runtime_live&&!runtime_bytes);runtime_fail_at=SIZE_MAX;
    }
    return sites;
}
int main(int argc,char **argv) {
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--resources")));native_invalid();size_t sites=native_attachment(argc==2);
    printf("Native call owner invalid=16 sites=%zu full_FI=%u producer_dead=1 physical0/0\n",sites,argc==2?1u:0u);return 0;
}
