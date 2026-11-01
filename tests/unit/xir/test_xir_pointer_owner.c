/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_pointer_owner.c - Byte pointer permissions and independent retirement
 *
 * KEY CONCEPT:
 *   The actual payload owner invalidates every surviving pointer on resize or
 *   retirement. Pointer metadata neither retains nor resurrects that payload.
 */
#include "xir/xxir_pointer.h"
#include "xir/xxir_buffer_storage.h"
#include "xir/xxir_value_internal.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"pointer %d: %s\n",__LINE__,#c);exit(1);}} while (0)
#ifdef XR_POINTER_FAULT_CASE
XR_FUNC void pointer_fail_next_unmap(void);
#endif
int main(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats compile_frame={0};
    CHECK(xr_compile_resources_stats(context.resources,&compile_frame)==XR_COMPILE_RESOURCE_OK);
    const XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_PTR,.element=XR_XIR_U8},
        {.kind=XR_XIR_TYPE_MUT_PTR,.element=XR_XIR_U8}};
    XrXirTypes types={nodes,2,NULL,NULL};XrXirTypeArena *arena=NULL;
    CHECK(!xr_xir_type_is_channel(&types,XR_XIR_I64) && xr_xir_channel_element(&types,XR_XIR_I64)==XR_XIR_UNIT);
    for(uint32_t i=0;i<2;++i) {
        XrXirType type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i);
        CHECK(!xr_xir_type_is_channel(&types,type) && xr_xir_channel_element(&types,type)==XR_XIR_UNIT);
    }
    CHECK(xr_xir_compile_type_arena_new(&context,&types,&arena)==XR_XIR_VALUE_OK);
    XrXirType readonly=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirType writable=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirDomainStats frame=xr_xir_domain_stats(domain);
    XrXirValue storage={0},pointer={0},read={0},offset={0};
    CHECK(xr_xir_buffer_storage_new(domain,8,0,true,&storage)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_buffer_storage_pointer(&storage,arena,writable,&pointer)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_pointer_store(&pointer,3,42)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_pointer_readonly(&pointer,arena,readonly,&read)==XR_XIR_VALUE_OK);
    uint8_t byte=0;CHECK(xr_xir_pointer_load(&read,3,&byte)==XR_XIR_VALUE_OK && byte==42);
    CHECK(xr_xir_pointer_store(&read,3,1)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_pointer_offset(&pointer,3,&offset)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_pointer_load(&offset,0,&byte)==XR_XIR_VALUE_OK && byte==42);
    const int64_t widths[]={1,2,4,8};
    for (unsigned i=0;i<4;++i) {
        int64_t stored=widths[i]==8?INT64_MIN+42:INT64_C(0x10203040),loaded=73;
        uint64_t bits=(uint64_t)stored;
        if (widths[i]<8) bits&=(UINT64_C(1)<<(widths[i]*8))-1;
        int64_t expected=0;memcpy(&expected,&bits,sizeof(bits));
        CHECK(xr_xir_pointer_volatile_store(&pointer,stored,widths[i],i!=0)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_pointer_volatile_load(&read,widths[i],&loaded)==XR_XIR_VALUE_OK && loaded==expected);
        CHECK(xr_xir_pointer_volatile_store(&read,0,widths[i],false)==XR_XIR_VALUE_BAD_ARGUMENT);
    }
    int64_t sentinel=73;
    CHECK(xr_xir_pointer_volatile_load(&pointer,3,&sentinel)==XR_XIR_VALUE_BAD_ARGUMENT && sentinel==73);
    CHECK(xr_xir_pointer_volatile_store(&pointer,0,3,false)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_pointer_prefetch(&read,0)==XR_XIR_VALUE_OK && xr_xir_pointer_prefetch(&pointer,1)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_pointer_cache_flush(&read,8)==XR_XIR_VALUE_OK);
    bool resized=false;CHECK(xr_xir_buffer_storage_resize(&storage,16,&resized)==XR_XIR_VALUE_OK && resized);
    byte=77;CHECK(xr_xir_pointer_load(&pointer,0,&byte)==XR_XIR_VALUE_BAD_ARGUMENT && byte==77);
    CHECK(xr_xir_pointer_load(&read,0,&byte)==XR_XIR_VALUE_BAD_ARGUMENT && byte==77);
    xr_xir_value_drop(&pointer);xr_xir_value_drop(&read);xr_xir_value_drop(&offset);
    CHECK(xr_xir_buffer_storage_pointer(&storage,arena,writable,&pointer)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_buffer_storage_resize(&storage,16,&resized)==XR_XIR_VALUE_OK && resized);
    CHECK(xr_xir_pointer_load(&pointer,0,&byte)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_value_drop(&pointer);
    CHECK(xr_xir_buffer_storage_pointer(&storage,arena,writable,&pointer)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&storage);
    CHECK(xr_xir_pointer_load(&pointer,0,&byte)==XR_XIR_VALUE_BAD_ARGUMENT && byte==77);
    xr_xir_value_drop(&pointer);
    CHECK(xr_xir_domain_stats(domain).live_bytes==frame.live_bytes);
    XrXirValue page={0},alias={0},interior={0};bool freed=false;
    CHECK(xr_xir_page_allocate(domain,arena,writable,64,3,&page)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&page,&alias)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_pointer_store(&page,0,91)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_pointer_offset(&page,1,&interior)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_page_free(&interior,64,&freed)==XR_XIR_VALUE_OK && !freed);
    CHECK(xr_xir_page_free(&page,63,&freed)==XR_XIR_VALUE_OK && !freed);
    CHECK(xr_xir_pointer_load(&alias,0,&byte)==XR_XIR_VALUE_OK && byte==91);
    CHECK(xr_xir_page_free(&page,64,&freed)==XR_XIR_VALUE_OK && freed);
    CHECK(xr_xir_page_free(&alias,64,&freed)==XR_XIR_VALUE_OK && !freed);
    CHECK(xr_xir_pointer_load(&alias,0,&byte)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_value_drop(&page);xr_xir_value_drop(&alias);xr_xir_value_drop(&interior);
    CHECK(xr_xir_domain_stats(domain).live_bytes==frame.live_bytes);
    CHECK(xr_xir_page_allocate(domain,arena,writable,1,3,&page)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_page_allocate(domain,arena,writable,17,3,&alias)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&page);
    CHECK(xr_xir_domain_stats(domain).live_bytes>frame.live_bytes);
#ifdef XR_POINTER_FAULT_CASE
    XrXirDomainStats before_close=xr_xir_domain_stats(domain);
    pointer_fail_next_unmap();
    CHECK(xr_xir_domain_close(domain)==XR_XIR_VALUE_IO);
    CHECK(xr_xir_domain_stats(domain).live_bytes==before_close.live_bytes);
    CHECK(xr_xir_pointer_store(&alias,0,13)==XR_XIR_VALUE_OK);
#endif
    CHECK(xr_xir_domain_close(domain)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_pointer_load(&alias,0,&byte)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_value_drop(&alias);
    CHECK(xr_xir_domain_stats(domain).live_bytes==frame.live_bytes);
    xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK &&
        stats.held_bytes==compile_frame.held_bytes);
    xr_compile_resources_release(context.resources);puts("pointer owner PASS");return 0;
}
