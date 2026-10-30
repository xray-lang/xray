/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_body.c - Real type owners, controlled growth and distinct typed uses
 */
#include "hir/xhir_body.h"
#include "hir/xhir_tuple.h"
#include "hir/xhir_nominal_source.h"
#include "xir/xxir_types.h"
#include "xir/xxir_interface.h"
#include "xir/xxir_nominal.h"
#include "shared/xnative_declaration.h"
#include "base/xmalloc.h"
#include "body_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
static size_t physical_live,physical_bytes,physical_attempts,physical_fail=SIZE_MAX;
typedef union BodyAllocation {struct {size_t bytes;uintptr_t active;} data;long double alignment;void *pointer;} BodyAllocation;
static void *body_allocate(size_t bytes) {
    if(physical_attempts++==physical_fail||bytes>SIZE_MAX-sizeof(BodyAllocation))return NULL;
    BodyAllocation *p=xr_malloc(sizeof(*p)+bytes);if(!p)return NULL;
    p->data.bytes=bytes;++physical_live;physical_bytes+=bytes;return p+1;
}
static void body_release(void *pointer) {
    if(!pointer)return;BodyAllocation *p=(BodyAllocation *)pointer-1;
    CHECK(physical_live&&physical_bytes>=p->data.bytes);--physical_live;physical_bytes-=p->data.bytes;xr_free(p);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc body_allocate
#define xr_free body_release
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free
typedef struct Cancel {size_t polls,after;} Cancel;
static bool cancel(const void *opaque){Cancel *c=(Cancel *)opaque;return ++c->polls>=c->after;}
static void write_bytes(const char *path,const char *data,size_t bytes) {
    FILE *file=fopen(path,"wb");CHECK(file&&fwrite(data,1,bytes,file)==bytes&&!fclose(file));
}
static XrHirBodyUse checked(XrHirBody *body,const XrXirTypes *types,XrHirBodyInput input) {
    XrHirBodyUse use={0};CHECK(xr_hir_body_check(body,types,&input,NULL,&use,NULL)==XR_XIR_OK);
    XrHirBodyAction action={0};CHECK(xr_hir_body_consume(body,use,&action)==XR_XIR_OK);return use;
}
static void typed_owners(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats initial={0},before={0},after={0};CHECK(xr_compile_resources_stats(resources,&initial)==XR_COMPILE_RESOURCE_OK);
    XrXirTypes types={0},foreign={0};XrHirBody *body=NULL,*other=NULL;
    CHECK(xr_hir_body_new(resources,&types,&body)==XR_XIR_OK&&xr_hir_body_new(resources,&types,&other)==XR_XIR_OK);
    XrHirBodyInput input={.rule=XR_HIR_BODY_EQUAL,.source=XR_XIR_I64,.target=XR_XIR_I64};
    XrHirBodyUse first=checked(body,&types,input),second=checked(body,&types,input),sentinel={other,123},out=sentinel;
    CHECK(first.index!=second.index&&xr_hir_body_use_count(body)==2);
    CHECK(xr_hir_body_check(body,&foreign,&input,NULL,&out,NULL)==XR_XIR_BAD_STRUCTURE&&out.owner==sentinel.owner&&out.index==sentinel.index);
    XrHirBodyAction action={0};CHECK(xr_hir_body_consume(other,first,&action)==XR_XIR_BAD_STRUCTURE);
    input.target=XR_XIR_STRING;CHECK(xr_hir_body_check(body,&types,&input,NULL,&out,NULL)==XR_XIR_BAD_TYPE);
    input.target=XR_XIR_I64;
    while(xr_hir_body_use_count(body)<16)(void)checked(body,&types,input);
    CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);size_t live=physical_live,bytes=physical_bytes;
    physical_fail=physical_attempts;CHECK(xr_hir_body_check(body,&types,&input,NULL,&out,NULL)==XR_XIR_OUT_OF_MEMORY);physical_fail=SIZE_MAX;
    CHECK(xr_hir_body_use_count(body)==16&&physical_live==live&&physical_bytes==bytes);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    Cancel cancellation={0,4};XrCompileRequestOptions options={.cancel_poll=cancel,.cancel_context=&cancellation};XrCompileRequestControl control={0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_body_check(body,&types,&input,&control,&out,NULL)==XR_XIR_CANCELLED);
    CHECK(xr_hir_body_use_count(body)==16&&physical_live==live&&physical_bytes==bytes);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    (void)checked(body,&types,input);CHECK(xr_hir_body_use_count(body)==17);
    XrHirBodyProjection edge={.function=0,.block=2,.value=17,.has_value=true,.type=XR_XIR_I64};
    CHECK(xr_hir_body_project(body,first,&edge,NULL,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_project(body,second,&edge,NULL,NULL)==XR_XIR_OK&&xr_hir_body_project(body,first,&edge,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    xr_hir_body_end(body);memset(&types,0xa5,sizeof(types));
    CHECK(xr_hir_body_check(body,&types,&input,NULL,&out,NULL)==XR_XIR_BAD_STRUCTURE);
    xr_hir_body_free(body);xr_hir_body_free(other);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==initial.held_bytes);
    xr_compile_resources_release(resources);CHECK(!physical_live&&!physical_bytes);
}
#include "test_body_owned.inc.c"
#include "test_body_receivers.inc.c"
#include "test_body_defaults.inc.c"
#include "test_body_tuples.inc.c"
#include "test_body_nominals.inc.c"
int main(void) {
    typed_owners();declaration_owners();place_receivers();contextual_call_receivers();default_receivers();tuple_receivers();nominal_receivers();write_bytes("control.xr",body_control,sizeof(body_control)-1);
    write_bytes("control.out",body_expected,sizeof(body_expected)-1);
    write_bytes("original.out",body_original_expected,sizeof(body_original_expected)-1);
    write_bytes("uses.xr",body_uses,sizeof(body_uses)-1);write_bytes("uses.out",body_uses_expected,sizeof(body_uses_expected)-1);
    write_bytes("places.xr",body_places,sizeof(body_places)-1);write_bytes("places.out",body_places_expected,sizeof(body_places_expected)-1);
    write_bytes("callback.xr",body_callback,sizeof(body_callback)-1);write_bytes("callback.out",body_callback_expected,sizeof(body_callback_expected)-1);
    write_bytes("callback-family.xr",body_callback_family,sizeof(body_callback_family)-1);write_bytes("callback-family.out",body_callback_family_expected,sizeof(body_callback_family_expected)-1);
    write_bytes("handlers.xr",body_handlers,sizeof(body_handlers)-1);write_bytes("handlers.out",body_handlers_expected,sizeof(body_handlers_expected)-1);
    write_bytes("handler-bad-type.xr",body_handler_bad_type,sizeof(body_handler_bad_type)-1);
    write_bytes("handler-bad-name.xr",body_handler_bad_name,sizeof(body_handler_bad_name)-1);
    write_bytes("handler-bad-type.out",body_handler_bad_type_expected,sizeof(body_handler_bad_type_expected)-1);
    write_bytes("handler-bad-name.out",body_handler_bad_name_expected,sizeof(body_handler_bad_name_expected)-1);
    write_bytes("generic-comptime.xr",body_generic_comptime,sizeof(body_generic_comptime)-1);
    write_bytes("generic-comptime.out",body_generic_comptime_expected,sizeof(body_generic_comptime_expected)-1);
    write_bytes("core-calls.xr",body_core_calls,sizeof(body_core_calls)-1);
    write_bytes("core-calls.out",body_core_calls_expected,sizeof(body_core_calls_expected)-1);
    write_bytes("inferred-callables.xr",body_inferred_callables,sizeof(body_inferred_callables)-1);
    write_bytes("inferred-callables.out",body_inferred_callables_expected,sizeof(body_inferred_callables_expected)-1);
    for(size_t i=0;i<sizeof(body_refusals)/sizeof(body_refusals[0]);++i) {
        char path[128];CHECK(snprintf(path,sizeof(path),"%s.xr",body_refusals[i].name)>0);
        write_bytes(path,body_refusals[i].source,strlen(body_refusals[i].source));
        CHECK(snprintf(path,sizeof(path),"%s.expected",body_refusals[i].name)>0);
        write_bytes(path,body_refusals[i].output,strlen(body_refusals[i].output));
    }
    puts("Typed body owner, distinct uses, two uses of one value and controlled growth PASS");return 0;
}
