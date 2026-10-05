/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_tuple_source_owner.c - Real requirement reification and substitution
 */
#include "xir/xxir.h"
#include "tuple_owner_observer.h"
#include "xir/xxir_source.c"
static XrXirType constructed(uint32_t n) {return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+n);}
static XrXirType parameter(uint32_t n) {return (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+n);}
static XrXirStatus reification(const XrXirCompileContext *c) {
    XrXirCallableParameter fields[3]={{constructed(0),0},{XR_XIR_UNIT,0},{parameter(1),0}};
    XrXirTypeNode nodes[2]={{.kind=XR_XIR_TYPE_ARRAY,.element=parameter(0),.parameter_span=1},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=3,.parameter_span=2}};
    XrXirTypes foreign={nodes,2,NULL,NULL};XrXirType actuals[2]={XR_XIR_STRING,XR_XIR_I64};
    SourceContext ctx={0};ctx.compile=*c;ctx.linkage_kind=XR_XIR_LIBRARY;
    XrXirSourceSnapshot *snapshot=NULL;
    XrXirType reified=XR_XIR_UNIT,result=XR_XIR_UNIT;SourceSubstitution sub={actuals,2};
    XrXirStatus status=XR_XIR_OK;
    if (!source_reify_type(&ctx,NULL,&foreign,constructed(1),&reified))status=ctx.diagnostic.status;
    if (status==XR_XIR_OK && !source_substitute(&ctx,&sub,reified,0,&result))status=ctx.diagnostic.status;
    if (status==XR_XIR_OK) {
        const XrXirTypeNode *tuple=xr_xir_tuple_signature(&ctx.types,result);
        CHECK(tuple && tuple->parameter_count==3 && !tuple->parameter_span && tuple->parameters[1].type==XR_XIR_UNIT &&
            tuple->parameters[2].type==XR_XIR_I64 && xr_xir_array_element(&ctx.types,tuple->parameters[0].type)==XR_XIR_STRING);
        CHECK(tuple->parameters!=fields);
        XrXirSourceView view={.complete=true,.types=&ctx.types};
        status=xr_xir_compile_source_snapshot_copy(c,&view,&snapshot);
    }
    memset(fields,0xcc,sizeof(fields));
    while(ctx.memory){SourceMemory *next=ctx.memory->next;xr_compile_resources_free(ctx.memory);ctx.memory=next;}
    if(status==XR_XIR_OK) {
        const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(snapshot);
        CHECK(view->types->nodes!=ctx.types.nodes);
        status=xr_xir_compile_types_structure_verify(c,view->types);
        if(status==XR_XIR_OK)status=xr_xir_compile_type_expression_shape(c,view->types,result,0);
    }
    xr_xir_compile_source_snapshot_free(snapshot);
    return status;
}
static XrCompileResourceStats measured(XrCompileResourceLimits limits,XrXirStatus expected) {
    XrXirCompileContext c=owner_new(limits);uint64_t baseline=stats(&c).live_bytes;
    XrXirStatus status=reification(&c);if(status!=expected)fprintf(stderr,"status=%u expected=%u\n",status,expected);
    CHECK(status==expected);XrCompileResourceStats result=stats(&c);owner_free(&c,baseline);return result;
}
static void syntax_owned(void) {
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    SourceContext ctx={0};ctx.compile=c;
    XrTypeRef unit={.kind=XR_TREF_UNIT},string={.kind=XR_TREF_STRING};
    XrTypeRef *children[2]={&unit,&string};XrTypeRef ref={.kind=XR_TREF_TUPLE,.nchildren=2,.children=children};
    XrXirType type=XR_XIR_UNIT;
    CHECK(source_type_ref(&ctx,&ref,&type) && type==constructed(0));
    CHECK(ctx.types.nodes[0].parameter_count==2 && ctx.types.nodes[0].parameters[0].type==XR_XIR_UNIT &&
        ctx.types.nodes[0].parameters[1].type==XR_XIR_STRING);
    XrXirSourceView view={.complete=true,.types=&ctx.types};XrXirSourceSnapshot *snapshot=NULL;
    CHECK(xr_xir_compile_source_snapshot_copy(&c,&view,&snapshot)==XR_XIR_OK);
    const XrXirSourceView *owned=xr_xir_compile_source_snapshot_view(snapshot);
    CHECK(owned->types->nodes!=ctx.types.nodes && owned->types->nodes[0].parameters!=ctx.types.nodes[0].parameters);
    XrTypeRef empty={.kind=XR_TREF_TUPLE};XrXirType ignored=XR_XIR_UNIT;
    CHECK(!source_type_ref(&ctx,&empty,&ignored) && ctx.diagnostic.status==XR_XIR_BAD_TYPE);
    memset(children,0xcc,sizeof(children));
    while(ctx.memory){SourceMemory *next=ctx.memory->next;xr_compile_resources_free(ctx.memory);ctx.memory=next;}
    CHECK(xr_xir_compile_types_structure_verify(&c,owned->types)==XR_XIR_OK);
    CHECK(owned->types->nodes[0].parameters[0].type==XR_XIR_UNIT && owned->types->nodes[0].parameters[1].type==XR_XIR_STRING);
    xr_xir_compile_source_snapshot_free(snapshot);
    owner_free(&c,baseline);puts("Source concrete Unit/String Tuple annotation owned after producer release; forged empty Tuple rejected PASS");
}
int main(void) {
    syntax_owned();
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass) {
        XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
        attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;XrXirStatus status=reification(&c);size_t actual=attempts;fail_at=SIZE_MAX;
        if(!pass){CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites&&sites<4096);}
        else CHECK(injected&&status==XR_XIR_OUT_OF_MEMORY);
        owner_free(&c,baseline);
    }
    XrCompileResourceStats s=measured(caps(),XR_XIR_OK);
    XrCompileResourceLimits exact={s.allocated_bytes,s.peak_bytes,s.work};
    XrCompileResourceStats actual=measured(exact,XR_XIR_OK);
    CHECK(actual.allocated_bytes==s.allocated_bytes&&actual.peak_bytes==s.peak_bytes&&actual.work==s.work);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)measured(less,XR_XIR_BUDGET);
    less=exact;--less.live_bytes;(void)measured(less,XR_XIR_BUDGET);
    less=exact;--less.work;(void)measured(less,XR_XIR_BUDGET);
    printf("real Tuple source requirement reify/substitute OOM sites=%zu finite exact and 3 axes -1 physical=0/0 PASS\n",sites);
    return 0;
}
