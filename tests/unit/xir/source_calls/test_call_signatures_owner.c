/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xir/xxir_types.h"
#define main source_calls_original_main
#include "test_calls_owner.c"
#undef main

typedef struct SignatureFixture {
    CallsFixture calls;
    XrXirTypes types;
    XrXirSourceType *parameters;
    XrXirConstraint constraint;
    const char *names[1];
    char *name;
} SignatureFixture;
static void signature_fixture(SignatureFixture *f) {
    memset(f,0,sizeof(*f));fixture(&f->calls);
    f->parameters=xr_malloc(2*sizeof(*f->parameters));CHECK(f->parameters);
    f->parameters[0]=(XrXirSourceType){XR_XIR_I64,0,true};
    f->parameters[1]=(XrXirSourceType){XR_XIR_BOOL,0,true};
    f->calls.view.types=&f->types;
    f->calls.declarations[0].type=(XrXirSourceType){XR_XIR_I64,0,true};
    f->calls.declarations[0].parameters=f->parameters;f->calls.declarations[0].parameter_count=2;
    f->calls.items[0].signature=(XrXirSourceCallSignature){.parameters=f->parameters,.parameter_count=2,
        .result={XR_XIR_I64,0,true},.known=true};
}
static void signature_generic(SignatureFixture *f) {
    f->calls.declarations[1].generic_parameter_count=1;f->calls.declarations[1].generic_constraints=&f->constraint;
    f->name=xr_malloc(5);CHECK(f->name);memcpy(f->name,"Item",5);f->names[0]=f->name;
    XrXirSourceCallSignature *signature=&f->calls.items[0].signature;
    signature->type_parameters=f->names;signature->type_parameter_count=1;signature->type_parameter_owner=2;
    f->parameters[0]=(XrXirSourceType){(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,2,true};signature->result=f->parameters[0];
}
static void signature_dispose(SignatureFixture *f) {
    if(f->name)memset(f->name,0xa5,5);xr_free(f->name);
    memset(f->parameters,0xa5,2*sizeof(*f->parameters));xr_free(f->parameters);memset(f,0xa5,sizeof(*f));
}
static void signature_owned(const XrXirSourceCalls *calls,bool generic) {
    CHECK(calls&&calls->count==3);const XrXirSourceCallSignature *s=&calls->items[0].signature;
    XrXirType expected=generic?(XrXirType)XR_XIR_TYPE_PARAMETER_BASE:XR_XIR_I64;
    CHECK(s->known&&s->parameter_count==2&&!s->receiver_count&&s->result.known&&s->result.type==expected);
    CHECK(s->parameters[0].known&&s->parameters[0].type==expected&&s->parameters[1].known&&s->parameters[1].type==XR_XIR_BOOL);
    CHECK(s->result.generic_owner==(generic?2u:0u)&&s->parameters[0].generic_owner==(generic?2u:0u));
    if(generic)CHECK(s->type_parameter_count==1&&s->type_parameter_owner==2&&!strcmp(s->type_parameters[0],"Item"));
    else CHECK(!s->type_parameter_count&&!s->type_parameter_owner&&!s->type_parameters);
}
static void signature_invalid(void) {
    for(unsigned mode=0;mode<21;++mode) {
        SignatureFixture f;signature_fixture(&f);if(mode>=14)signature_generic(&f);
        XrCompileResources *r=sdk_ledger(&sdk_unlimited);
        XrXirCompileContext context={r,xr_xir_compile_default_limits()};XrXirSourceSnapshot *s=snapshot(&context,&f.calls);
        XrXirSourceCallSignature *v=&f.calls.items[0].signature;XrXirStatus expected=XR_XIR_BAD_STRUCTURE;
        switch(mode) {
        case 0:v->known=false;break;
        case 1:v->parameters=NULL;break;
        case 2:v->receiver_count=3;break;
        case 3:v->parameter_count=1;break;
        case 4:v->result.known=false;expected=XR_XIR_BAD_TYPE;break;
        case 5:f.parameters[0].known=false;expected=XR_XIR_BAD_TYPE;break;
        case 6:f.parameters[0].type=(XrXirType)UINT32_MAX;expected=XR_XIR_BAD_TYPE;break;
        case 7:f.parameters[0].generic_owner=3;expected=XR_XIR_BAD_TYPE;break;
        case 8:f.parameters[0].generic_owner=1;expected=XR_XIR_BAD_TYPE;break;
        case 9:f.calls.items[1].signature=*v;break;
        case 10:v->parameter_count=65537;break;
        case 11:v->result.generic_owner=1;expected=XR_XIR_BAD_TYPE;break;
        case 12:v->receiver_count=1;break;
        case 13:v->receiver_count=2;break;
        case 14:v->type_parameters=NULL;break;
        case 15:v->type_parameter_count=0;break;
        case 16:v->type_parameter_owner=3;break;
        case 17:v->type_parameter_owner=1;break;
        case 18:f.name[0]=0;break;
        case 19:f.names[0]=NULL;break;
        case 20:f.parameters[0].generic_owner=1;expected=XR_XIR_BAD_TYPE;break;
        }
        size_t blocks=runtime_live,bytes=runtime_bytes;XrCompileResourceStats before=sdk_stats(r);
        CHECK(xr_xir_compile_source_snapshot_calls_copy(s,&f.calls.calls)==expected);
        CHECK(!xr_xir_compile_source_snapshot_calls(s)&&runtime_live==blocks&&runtime_bytes==bytes&&sdk_stats(r).live_bytes==before.live_bytes);
        signature_dispose(&f);xr_xir_compile_source_snapshot_free(s);xr_compile_resources_release(r);
        CHECK(!runtime_live&&!runtime_bytes);
    }
}
static size_t signature_attachment(bool failures,bool generic) {
    size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        if(!failures&&probe)break;
        SignatureFixture f;signature_fixture(&f);if(generic)signature_generic(&f);
        XrCompileResources *r=sdk_ledger(&sdk_unlimited);
        XrXirCompileContext context={r,xr_xir_compile_default_limits()};XrXirSourceSnapshot *s=snapshot(&context,&f.calls);
        size_t blocks=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;XrCompileResourceStats before=sdk_stats(r);
        runtime_fail_at=probe?begin+probe-1:SIZE_MAX;
        XrXirStatus status=xr_xir_compile_source_snapshot_calls_copy(s,&f.calls.calls);
        if(!probe){CHECK(status==XR_XIR_OK);sites=runtime_attempts-begin;CHECK(sites);}
        else {
            CHECK(status==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>runtime_fail_at&&!xr_xir_compile_source_snapshot_calls(s));
            CHECK(runtime_live==blocks&&runtime_bytes==bytes&&sdk_stats(r).live_bytes==before.live_bytes);
            runtime_fail_at=SIZE_MAX;CHECK(xr_xir_compile_source_snapshot_calls_copy(s,&f.calls.calls)==XR_XIR_OK);
        }
        const XrXirSourceCalls *owned=xr_xir_compile_source_snapshot_calls(s);signature_owned(owned,generic);
        CHECK(owned->items[0].signature.parameters!=f.parameters);
        if(generic)CHECK(owned->items[0].signature.type_parameters!=f.names&&owned->items[0].signature.type_parameters[0]!=f.name);
        signature_dispose(&f);
        xr_compile_resources_release(r);signature_owned(owned,generic);
        xr_xir_compile_source_snapshot_free(s);CHECK(!runtime_live&&!runtime_bytes);runtime_fail_at=SIZE_MAX;
    }
    return sites;
}
static void signature_checked_receiver(void) {
    for(unsigned mode=0;mode<7;++mode) {
        SignatureFixture f;signature_fixture(&f);
        XrXirSourceDeclaration declarations[3];memcpy(declarations,f.calls.declarations,sizeof(f.calls.declarations));
        declarations[0].parent=3;declarations[0].range=(XrXirSourceRange){0};
        declarations[0].checked_receiver_count=1;
        declarations[2]=(XrXirSourceDeclaration){.id=3,.kind=XR_XIR_SOURCE_TYPE,.name="Owner",.range={0},
            .type={XR_XIR_I64,0,true}};
        XrXirSourceDeclarationSyntax names[3];memcpy(names,f.calls.names,sizeof(f.calls.names));
        names[0]=(XrXirSourceDeclarationSyntax){0};names[2]=(XrXirSourceDeclarationSyntax){0};
        XrXirSourceFunctionRange functions[3];memcpy(functions,f.calls.functions,sizeof(f.calls.functions));
        functions[0]=(XrXirSourceFunctionRange){0};functions[2]=(XrXirSourceFunctionRange){0};
        f.calls.view.declarations=declarations;f.calls.view.declaration_count=3;
        f.calls.syntax.declarations=names;f.calls.syntax.declaration_count=3;
        f.calls.calls.functions=functions;f.calls.calls.declaration_count=3;
        f.calls.items[0].signature.receiver_count=1;
        f.calls.items[0].signature.owner_type=(XrXirSourceType){XR_XIR_I64,0,true};
        switch(mode) {
        case 1:declarations[0].checked_receiver_count=2;break;
        case 2:declarations[0].parent=0;break;
        case 3:declarations[2].kind=XR_XIR_SOURCE_BINDING;break;
        case 4:f.calls.items[0].signature.receiver_count=0;break;
        case 5:f.calls.items[0].signature.owner_type=(XrXirSourceType){0};break;
        case 6:declarations[0].checked_receiver_count=0;break;
        }
        XrCompileResources *r=sdk_ledger(&sdk_unlimited);
        XrXirCompileContext context={r,xr_xir_compile_default_limits()};
        XrXirSourceSnapshot *s=snapshot(&context,&f.calls);
        XrCompileResourceStats before=sdk_stats(r);size_t blocks=runtime_live,bytes=runtime_bytes;
        XrXirStatus status=xr_xir_compile_source_snapshot_calls_copy(s,&f.calls.calls);
        if(!mode) {
            CHECK(status==XR_XIR_OK);
            declarations[0].checked_receiver_count=0;memset(names,0xa5,sizeof(names));
            signature_dispose(&f);xr_compile_resources_release(r);
            const XrXirSourceView *owned=xr_xir_compile_source_snapshot_view(s);
            const XrXirSourceCalls *calls=xr_xir_compile_source_snapshot_calls(s);
            CHECK(owned&&owned->declarations[0].checked_receiver_count==1&&!owned->declarations[0].range.line);
            CHECK(calls&&calls->items[0].signature.receiver_count==1&&calls->items[0].signature.parameters[0].type==XR_XIR_I64);
            const XrXirSourceSyntaxView *syntax=xr_xir_compile_source_snapshot_syntax(s);
            CHECK(syntax&&syntax->declarations[0].role==XR_XIR_SOURCE_SYNTAX_NONE);
        } else {
            CHECK(status==(mode==5?XR_XIR_BAD_TYPE:XR_XIR_BAD_STRUCTURE));
            CHECK(!xr_xir_compile_source_snapshot_calls(s)&&runtime_live==blocks&&runtime_bytes==bytes&&sdk_stats(r).live_bytes==before.live_bytes);
            signature_dispose(&f);xr_compile_resources_release(r);
        }
        xr_xir_compile_source_snapshot_free(s);CHECK(!runtime_live&&!runtime_bytes);
    }
    puts("Checked receiver display owner positive=1 rejection=6 producer_dead=1 lexical_span_absent=1 physical0/0");
}
int main(int argc,char **argv) {
    if(argc==2&&!strcmp(argv[1],"--legacy"))return source_calls_original_main();
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--resources")));
    signature_checked_receiver();signature_invalid();size_t sites=signature_attachment(argc==2,false),generic=signature_attachment(argc==2,true);
    printf("Source call signature owner invalid=21 sites=%zu generic_sites=%zu full_FI=%u producer_dead=1 physical0/0\n",sites,generic,argc==2?1u:0u);
    return 0;
}
