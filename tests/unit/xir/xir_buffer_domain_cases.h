/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_buffer_domain_cases.h - Container and leaf admission share one Domain
 *
 * KEY CONCEPT:
 *   Unique storage with spare capacity must detach for a new Domain, and an
 *   existing foreign child must be rejected before any root mutation.
 */
#ifndef XIR_BUFFER_DOMAIN_CASES_H
#define XIR_BUFFER_DOMAIN_CASES_H
#include "xir/xxir_array.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_value_internal.h"
static const XirObject *buffer_domain_object(const XrXirValue *value) {
    const XirObject *object=NULL;CHECK(xr_xir_value_valid(value));
    memcpy(&object,&value->payload,sizeof(object));return object;
}
static void buffer_domain_cases(XrXirCompileContext *compile) {
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(compile->resources,&before)==XR_COMPILE_RESOURCE_OK);
    const XrXirTypeNode node={.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_BUFFER_STORAGE};
    const XrXirTypes types={&node,1,NULL,NULL};XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_compile_types_structure_verify(compile,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_arena_new(compile,&types,&arena)==XR_XIR_VALUE_OK);
    for(unsigned path=0;path<2;++path)for(unsigned cross=0;cross<2;++cross)for(unsigned populated=0;populated<2;++populated) {
        XrXirDomain *home=NULL,*other=NULL;
        CHECK(xr_xir_domain_new(1048576,&home)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_domain_new(1048576,&other)==XR_XIR_VALUE_OK);
        uint64_t home_frame=xr_xir_domain_stats(home).live_bytes,other_frame=xr_xir_domain_stats(other).live_bytes;
        XrXirValueAdmission owner={.arena=arena,.domain=home,.work=UINT64_MAX,.scratch_bytes=1048576};
        XrXirValueAdmission caller={.arena=arena,.domain=cross?other:home,.work=UINT64_MAX,.scratch_bytes=1048576};
        XrXirValue array={0},old={0},incoming={0},read={0};XrXirFaultDetail fault={0};
        CHECK(xr_xir_array_with_capacity((XrXirType)256,4,&owner,&array)==XR_XIR_VALUE_OK);
        XrXirValuePlace root={(XrXirType)256,&array.payload};
        if(populated) {
            CHECK(xr_xir_buffer_storage_new(home,21,0,true,&old)==XR_XIR_VALUE_OK);
            CHECK(xr_xir_array_push(&root,&old,&owner)==XR_XIR_VALUE_OK);
        }
        CHECK(xr_xir_buffer_storage_new(caller.domain,13,0,true,&incoming)==XR_XIR_VALUE_OK);
        int64_t old_root=array.payload;const XirObject *original=buffer_domain_object(&array);
        CHECK(original->domain==home && atomic_load(&original->references)==1);
        uint64_t held_home=xr_xir_domain_stats(home).live_bytes,held_other=xr_xir_domain_stats(other).live_bytes;
        XrXirValuePath no_steps={0};
        XrXirValueStatus status=path ? xr_xir_value_path_push(&root,&no_steps,&incoming,&caller,&fault) :
            xr_xir_array_push(&root,&incoming,&caller);
        if(cross && populated) {
            CHECK(status==XR_XIR_VALUE_BAD_ARGUMENT && array.payload==old_root);
            CHECK(xr_xir_domain_stats(home).live_bytes==held_home && xr_xir_domain_stats(other).live_bytes==held_other);
            CHECK(xr_xir_value_admit(&array,(XrXirType)256,&caller)==XR_XIR_VALUE_BAD_ARGUMENT);
            CHECK(xr_xir_value_admit(&array,(XrXirType)256,&owner)==XR_XIR_VALUE_OK);
            int64_t length=0;CHECK(xr_xir_array_len(&array,&owner,&length)==XR_XIR_VALUE_OK && length==1);
            CHECK(xr_xir_array_get(&array,0,&owner,&read,&fault)==XR_XIR_VALUE_OK && read.payload==old.payload);
            CHECK(xr_xir_value_admit(&read,XR_XIR_BUFFER_STORAGE,&owner)==XR_XIR_VALUE_OK);
        } else {
            CHECK(status==XR_XIR_VALUE_OK && (cross ? array.payload!=old_root : array.payload==old_root));
            CHECK(buffer_domain_object(&array)->domain==caller.domain);
            CHECK(xr_xir_value_admit(&array,(XrXirType)256,&caller)==XR_XIR_VALUE_OK);
            int64_t length=0;CHECK(xr_xir_array_len(&array,&caller,&length)==XR_XIR_VALUE_OK && length==1+(int64_t)populated);
            CHECK(xr_xir_array_get(&array,populated,&caller,&read,&fault)==XR_XIR_VALUE_OK);
            CHECK(read.payload==incoming.payload && buffer_domain_object(&read)->domain==caller.domain);
            CHECK(xr_xir_value_admit(&read,XR_XIR_BUFFER_STORAGE,&caller)==XR_XIR_VALUE_OK);
            if(cross)CHECK(xr_xir_value_admit(&array,(XrXirType)256,&owner)==XR_XIR_VALUE_BAD_ARGUMENT);
        }
        xr_xir_value_drop(&read);xr_xir_value_drop(&array);xr_xir_value_drop(&old);xr_xir_value_drop(&incoming);
        CHECK(xr_xir_domain_stats(home).live_bytes==home_frame && xr_xir_domain_stats(other).live_bytes==other_frame);
        xr_xir_domain_drop(home);xr_xir_domain_drop(other);
    }
    xr_xir_compile_type_arena_drop(arena);
    CHECK(xr_compile_resources_stats(compile->resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    puts("Array/path Buffer Domain consistency: same/cross, empty/nonempty, unique capacity4 PASS");
}
#endif
