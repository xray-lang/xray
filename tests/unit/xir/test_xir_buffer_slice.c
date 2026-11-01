/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_buffer_slice.c - Live byte storage with independently retired views
 *
 * KEY CONCEPT:
 *   A scope owns readonly access, while the payload owner can invalidate every
 *   alias without waiting for physical view or origin metadata retirement.
 */
#include "xir/xxir_buffer_storage.h"
#include "xir/xxir_pointer.h"
#include "xir/xxir_view_value_internal.h"
#include "xir/xxir_view_adoption_internal.h"
#include "xir/xxir_from_utf8_internal.h"
#include "xir/xxir_array.h"
#include "xir/xxir_class.h"
#include "xir/xxir_nominal.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"buffer slice %d: %s\n",__LINE__,#c);exit(1);}} while (0)
typedef struct Binding {int64_t value;XrXirCellAuthority authority;} Binding;
typedef struct Scope {Binding bindings[4];uint32_t count;bool wrong,writable;} Scope;
#ifdef XR_BUFFER_SLICE_FAULT
XR_FUNC void buffer_slice_fail(size_t attempt);
XR_FUNC void buffer_slice_fail_end(void);
XR_FUNC size_t buffer_slice_physical(void);
#endif
static void bind_view(Scope *scope,const XrXirValue *view,XrXirCellAuthority authority) {
    CHECK(scope->count<4);scope->bindings[scope->count++]=(Binding){view->payload,authority};
}
static XrXirValueStatus authorize(void *context,const XrXirValue *view,bool writable,
    XrXirValueAdmission *admission,XrXirCellAuthority *authority) {
    Scope *scope=context;(void)admission;
    if (writable && !scope->writable) return XR_XIR_VALUE_BAD_ARGUMENT;
    for (uint32_t i=0;i<scope->count;++i) if (scope->bindings[i].value==view->payload) {
        *authority=scope->bindings[i].authority;if (scope->wrong) ++authority->epoch;
        return XR_XIR_VALUE_OK;
    }
    return XR_XIR_VALUE_BAD_ARGUMENT;
}
int main(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats compile_frame={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&compile_frame)==XR_COMPILE_RESOURCE_OK);
    const XrXirType field_type=XR_XIR_BUFFER_STORAGE;
    const XrXirNominalFieldIdentity field={.name={"_storage",8},.flags=XR_XIR_FIELD_PRIVATE};
    const XrXirNominalIdentity nominal={.module={"mem",3},
        .name={"Buffer",6},.exported=1,.fields=&field,.field_count=1,
        .kind=XR_XIR_NOMINAL_CLASS,.flags=XR_XIR_NOMINAL_FINAL};
    const XrXirNominalTable nominals={NULL,1,&nominal};
    const XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_U8},
        {.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_U8},
        {.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_U8},
        {.kind=XR_XIR_TYPE_MUT_PTR,.element=XR_XIR_U8},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)259},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,&field_type,1}},
        {.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)263}};
    const XrXirTypes types={nodes,9,&nominals,NULL};XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(&context,&types,&arena)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL,*foreign=NULL;
    CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1048576,&foreign)==XR_XIR_VALUE_OK);
    const XrXirDomainStats frame=xr_xir_domain_stats(domain);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
    Scope scope={0};XrXirViewAccess access={&admission,authorize,&scope};
    XrXirCellAuthority authority={domain,&scope,1},child_authority={domain,&scope,2};
    XrXirValue storage={0},pointer={0},view={0},child={0},value={0},copy={0},text={0};
    XrXirViewOriginPin pin={0},child_pin={0};XrXirFaultDetail fault={0};int64_t length=-1;
#ifdef XR_BUFFER_SLICE_FAULT
    CHECK(xr_xir_buffer_storage_new(domain,0,0,true,&storage)==XR_XIR_VALUE_OK);
    const uint64_t live_frame=xr_xir_domain_stats(domain).live_bytes;
    const size_t physical_frame=buffer_slice_physical();
    for (size_t attempt=0;attempt<2;++attempt) {
        buffer_slice_fail(attempt);
        CHECK(xr_xir_buffer_storage_view(&storage,(XrXirType)256,&authority,&admission,&view)==XR_XIR_VALUE_OOM);
        buffer_slice_fail_end();
        CHECK(!view.type && !view.payload && xr_xir_value_valid(&storage));
        CHECK(xr_xir_domain_stats(domain).live_bytes==live_frame && buffer_slice_physical()==physical_frame);
    }
    CHECK(xr_xir_buffer_storage_view(&storage,(XrXirType)256,&authority,&admission,&view)==XR_XIR_VALUE_OK);
    bind_view(&scope,&view,authority);
    CHECK(xr_xir_view_len(&view,&access,&length)==XR_XIR_VALUE_OK && !length);
    xr_xir_value_drop(&view);xr_xir_value_drop(&storage);scope.count=0;
#endif
    CHECK(xr_xir_buffer_storage_new(domain,3,0,true,&storage)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_buffer_storage_pointer(&storage,arena,(XrXirType)260,&pointer)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_pointer_store(&pointer,0,'a')==XR_XIR_VALUE_OK);
    CHECK(xr_xir_pointer_store(&pointer,1,'b')==XR_XIR_VALUE_OK);
    CHECK(xr_xir_pointer_store(&pointer,2,'c')==XR_XIR_VALUE_OK);
    CHECK(xr_xir_buffer_storage_view(&storage,(XrXirType)257,&authority,&admission,&view)==XR_XIR_VALUE_BAD_ARGUMENT && !view.type);
    CHECK(xr_xir_buffer_storage_view(&storage,(XrXirType)258,&authority,&admission,&view)==XR_XIR_VALUE_BAD_ARGUMENT && !view.type);
    XrXirValueAdmission other=admission;other.domain=foreign;
    CHECK(xr_xir_buffer_storage_view(&storage,(XrXirType)256,&authority,&other,&view)==XR_XIR_VALUE_BAD_ARGUMENT && !view.type);
    uint64_t work=admission.work;admission.work=0;
    CHECK(xr_xir_buffer_storage_view(&storage,(XrXirType)256,&authority,&admission,&view)==XR_XIR_VALUE_LIMIT && !view.type);
    admission.work=work;
    CHECK(xr_xir_buffer_storage_view(&storage,(XrXirType)256,&authority,&admission,&view)==XR_XIR_VALUE_OK);
    xr_xir_view_scope_pin_created(&view,&authority,&pin);bind_view(&scope,&view,authority);
    XrXirViewOriginInfo info={0};
    CHECK(xr_xir_view_origin_info(&pin,&admission,&info)==XR_XIR_VALUE_OK &&
        info.logical_active && info.root && info.root->payload==storage.payload);
    CHECK(xr_xir_view_len(&view,&access,&length)==XR_XIR_VALUE_OK && length==3);
    CHECK(xr_xir_pointer_store(&pointer,1,'z')==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_view_get(&view,1,&access,&value,&fault)==XR_XIR_VALUE_OK && value.type==XR_XIR_U8 && value.payload=='b');
    xr_xir_value_drop(&value);
    scope.wrong=true;
    CHECK(xr_xir_view_get(&view,0,&access,&value,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && !value.type);
    scope.wrong=false;value=(XrXirValue){XR_XIR_U8,0,1};
    CHECK(xr_xir_view_set(&view,0,&value,&access,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);xr_xir_value_drop(&value);
    CHECK(xr_xir_view_get(&view,3,&access,&value,&fault)==XR_XIR_VALUE_BOUNDS && fault.index==3 && fault.length==3);
    CHECK(xr_xir_view_copy((XrXirType)259,&view,&access,&copy)==XR_XIR_VALUE_OK);
    CHECK(xir_from_utf8(&view,&access,&text)==XR_XIR_VALUE_OK);
    const char *bytes=NULL;size_t count=0;
    CHECK(xr_xir_string_view(&text,&bytes,&count) && count==3 && bytes[1]=='b');xr_xir_value_drop(&text);
    CHECK(xr_xir_pointer_store(&pointer,0,255)==XR_XIR_VALUE_BAD_ARGUMENT);
    XrXirViewStorageSpec spec={child_authority,1,2,XR_XIR_TYPE_SLICE};
    CHECK(xr_xir_view_reborrow((XrXirType)256,&view,&spec,&access,&child)==XR_XIR_VALUE_OK);
    xr_xir_view_scope_pin_created(&child,&child_authority,&child_pin);bind_view(&scope,&child,child_authority);
    CHECK(xr_xir_view_get(&child,0,&access,&value,&fault)==XR_XIR_VALUE_OK && value.payload=='b');xr_xir_value_drop(&value);
    XrXirCellAuthority adopted={domain,&scope,3};
    xr_xir_view_scope_adopt(&child_pin,&adopted);scope.bindings[1].authority=adopted;
    CHECK(xr_xir_view_get(&child,1,&access,&value,&fault)==XR_XIR_VALUE_OK && value.payload=='c');xr_xir_value_drop(&value);
    bool resized=false;
    CHECK(xr_xir_buffer_storage_resize(&storage,1024,&resized)==XR_XIR_VALUE_BAD_ARGUMENT && !resized);
    XrXirViewOriginPin observer={0};
    CHECK(xr_xir_view_origin_pin(&view,&access,&observer)==XR_XIR_VALUE_OK);
    xr_xir_view_scope_close(&pin);
    CHECK(xr_xir_view_get(&child,1,&access,&value,&fault)==XR_XIR_VALUE_OK && value.payload=='c');
    xr_xir_value_drop(&value);xr_xir_view_scope_close(&child_pin);
    CHECK(xr_xir_pointer_store(&pointer,0,255)==XR_XIR_VALUE_OK);
    XrXirValue invalid={0};
    CHECK(xr_xir_buffer_storage_view(&storage,(XrXirType)256,&authority,&admission,&invalid)==XR_XIR_VALUE_OK);
    bind_view(&scope,&invalid,authority);
    CHECK(xir_from_utf8(&invalid,&access,&text)==XR_XIR_VALUE_BAD_UTF8 && !text.type);
    xr_xir_value_drop(&invalid);
    CHECK(xr_xir_pointer_store(&pointer,0,'a')==XR_XIR_VALUE_OK);
    CHECK(xr_xir_buffer_storage_resize(&storage,1024,&resized)==XR_XIR_VALUE_OK && resized);
    CHECK(xr_xir_value_valid(&view) && xr_xir_value_valid(&child));
    CHECK(!xr_xir_view_scope_active(&pin) && !xr_xir_view_scope_active(&child_pin));
    CHECK(xr_xir_view_origin_info(&observer,&admission,&info)==XR_XIR_VALUE_OK && !info.logical_active && !info.root);
    CHECK(xr_xir_view_len(&view,&access,&length)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_view_get(&child,0,&access,&value,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && !value.type);
    CHECK(xr_xir_array_get(&copy,1,&admission,&value,&fault)==XR_XIR_VALUE_OK && value.payload=='b');xr_xir_value_drop(&value);
    xr_xir_view_scope_close(&child_pin);xr_xir_view_scope_close(&pin);
    xr_xir_view_origin_unpin(&observer);
    xr_xir_value_drop(&child);xr_xir_value_drop(&view);xr_xir_value_drop(&pointer);xr_xir_value_drop(&copy);
    CHECK(xr_xir_buffer_storage_view(&storage,(XrXirType)256,&authority,&admission,&view)==XR_XIR_VALUE_OK);
    scope.count=0;bind_view(&scope,&view,authority);xr_xir_view_scope_pin_created(&view,&authority,&pin);
    const XrXirDomainStats before_drop=xr_xir_domain_stats(domain);
    xr_xir_value_drop(&storage);
    CHECK(xr_xir_domain_stats(domain).live_bytes+1024<=before_drop.live_bytes);
    CHECK(xr_xir_value_valid(&view) && !xr_xir_view_scope_active(&pin));
    CHECK(xr_xir_view_origin_info(&pin,&admission,&info)==XR_XIR_VALUE_OK && !info.logical_active && !info.root);
    CHECK(xr_xir_view_get(&view,0,&access,&value,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && !value.type);
    xr_xir_view_scope_close(&pin);xr_xir_value_drop(&view);
    /* A real exclusive Buffer range writes its original payload. The scope
     * ends before physical pins and escaped tombstone handles are dropped. */
    CHECK(xr_xir_buffer_storage_new(domain,4,0,true,&storage)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_buffer_storage_pointer(&storage,arena,(XrXirType)260,&pointer)==XR_XIR_VALUE_OK);
    XrXirValue buffer={0},owner={0},owner_read={0};XrXirCellLoan native_loan={0};
    XrXirCellAuthority native_authority={domain,&native_loan,10};
    CHECK(xr_xir_class_new((XrXirType)263,&storage,1,&admission,&buffer)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)264,&buffer,&admission,&owner)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&buffer);
    CHECK(xr_xir_cell_loan_prepare(&owner,NULL,&native_loan)==XR_XIR_VALUE_OK);
    xr_xir_cell_loan_commit(&owner,&native_loan,&native_authority);
    spec=(XrXirViewStorageSpec){authority,0,4,XR_XIR_TYPE_MUT_SLICE};
    CHECK(xr_xir_buffer_storage_mutable_view(&pointer,&storage,(XrXirType)257,&spec,&admission,&view)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_buffer_view_bind_root(&view,&owner,&native_authority,&admission)==XR_XIR_VALUE_OK);
    scope.count=0;scope.writable=true;bind_view(&scope,&view,authority);
    xr_xir_view_scope_pin_created(&view,&authority,&pin);
    CHECK(xr_xir_view_ref_return_prepare(&owner,&native_loan,&native_authority,&admission)==XR_XIR_VALUE_OK);
    xr_xir_view_ref_return_commit(&native_loan);
    CHECK(xr_xir_cell_read(&owner,&owner_read)==XR_XIR_VALUE_BAD_ARGUMENT && !owner_read.type);
    value=(XrXirValue){XR_XIR_U8,0,7};
    CHECK(xr_xir_view_set(&view,0,&value,&access,&fault)==XR_XIR_VALUE_OK);xr_xir_value_drop(&value);
    uint8_t byte=0;
    CHECK(xr_xir_pointer_load(&pointer,0,&byte)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_pointer_store(&pointer,0,1)==XR_XIR_VALUE_BAD_ARGUMENT);
    resized=false;CHECK(xr_xir_buffer_storage_resize(&storage,8,&resized)==XR_XIR_VALUE_BAD_ARGUMENT && !resized);
    CHECK(xr_xir_buffer_storage_view(&storage,(XrXirType)256,&child_authority,&admission,&child)==XR_XIR_VALUE_BAD_ARGUMENT);
    spec=(XrXirViewStorageSpec){child_authority,1,2,XR_XIR_TYPE_MUT_SLICE};
    CHECK(xr_xir_view_reborrow((XrXirType)257,&view,&spec,&access,&child)==XR_XIR_VALUE_OK);
    bind_view(&scope,&child,child_authority);xr_xir_view_scope_pin_created(&child,&child_authority,&child_pin);
    CHECK(xr_xir_view_get(&view,0,&access,&value,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    value=(XrXirValue){XR_XIR_U8,0,9};
    CHECK(xr_xir_view_set(&child,0,&value,&access,&fault)==XR_XIR_VALUE_OK);xr_xir_value_drop(&value);
    xr_xir_view_scope_close(&child_pin);
    CHECK(xr_xir_view_len(&view,&access,&length)==XR_XIR_VALUE_OK && length==4);
    xr_xir_view_scope_close(&pin);
    CHECK(xr_xir_cell_read(&owner,&owner_read)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&owner_read);
    CHECK(xr_xir_pointer_load(&pointer,0,&byte)==XR_XIR_VALUE_OK && byte==7);
    CHECK(xr_xir_pointer_load(&pointer,1,&byte)==XR_XIR_VALUE_OK && byte==9);
    CHECK(xr_xir_view_get(&view,0,&access,&value,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_buffer_storage_resize(&storage,8,&resized)==XR_XIR_VALUE_OK && resized);
    xr_xir_value_drop(&child);xr_xir_value_drop(&view);xr_xir_value_drop(&pointer);xr_xir_value_drop(&storage);
    xr_xir_value_drop(&owner);
    scope.count=0;scope.writable=false;
    /* The original Cell-backed paths retain their ordinary root loans. */
    CHECK(xr_xir_string_new(domain,"xy",2,&text)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)261,&text,&admission,&storage)==XR_XIR_VALUE_OK);
    spec=(XrXirViewStorageSpec){authority,0,2,XR_XIR_TYPE_SLICE};
    CHECK(xr_xir_view_new((XrXirType)256,&storage,NULL,&spec,&admission,&view)==XR_XIR_VALUE_OK);
    scope.count=0;bind_view(&scope,&view,authority);
    CHECK(xr_xir_view_get(&view,1,&access,&value,&fault)==XR_XIR_VALUE_OK && value.payload=='y');
    xr_xir_value_drop(&value);xr_xir_value_drop(&view);xr_xir_value_drop(&storage);xr_xir_value_drop(&text);
    const XrXirValue items[]={{XR_XIR_U8,0,21},{XR_XIR_U8,0,42}};
    CHECK(xr_xir_array_new((XrXirType)259,items,2,&admission,&copy)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)262,&copy,&admission,&storage)==XR_XIR_VALUE_OK);
    spec.kind=XR_XIR_TYPE_MUT_SLICE;
    CHECK(xr_xir_view_new((XrXirType)257,&storage,NULL,&spec,&admission,&view)==XR_XIR_VALUE_OK);
    scope.count=0;bind_view(&scope,&view,authority);
    CHECK(xr_xir_view_get(&view,1,&access,&value,&fault)==XR_XIR_VALUE_OK && value.payload==42);
    xr_xir_value_drop(&value);xr_xir_value_drop(&view);xr_xir_value_drop(&storage);xr_xir_value_drop(&copy);
    CHECK(xr_xir_domain_stats(domain).live_bytes==frame.live_bytes);
    xr_xir_domain_drop(foreign);xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==compile_frame.held_bytes);
    xr_compile_resources_release(context.resources);puts("buffer slice PASS");return 0;
}
